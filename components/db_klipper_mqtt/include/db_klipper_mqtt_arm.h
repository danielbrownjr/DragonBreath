#pragma once
// Pure, host-testable core of the MQTT-Klipper control source: split-status value
// extraction + the retained-aware arming state machine. No ESP deps, so it can be
// unit-tested on the host (mirrors pb_bambu_parse.h / pb_ntc / pb_heater).
//
// Safety model (see plans/mqtt-klipper-implementation-design.md §5): Moonraker
// publishes Klipper split-status RETAINED, so a reconnecting device is handed stale
// desired-state (armed=1/target/seq) and the last heartbeat value immediately. Heat
// must therefore NEVER engage from received values alone — it engages only on a NEW
// coherent `seq` (the macro writes `seq` LAST, so all fields are consistent when it
// changes) AND while liveness is fresh (the DB_LINK heartbeat has been observed to
// change since connect and did so within the timeout). A retained snapshot is the
// baseline `seq`, which is not a change, so it can never arm on its own.
//
// The state machine is deliberately quick to DISARM (armed->0, mode->off, or lost
// liveness disengage immediately, no seq needed) and careful to ARM (full seq
// coherence + liveness). Failing to engage is fail-safe; falsely engaging is not.
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---- split-status payload value extraction --------------------------------
// Moonraker split-status payload is JSON: {"eventtime":<ts>,"value":<v>} where <v>
// is a JSON literal (number, "string", or true/false). Copy the raw <v> token
// (quotes stripped for strings) into `out`. Returns false if no "value" key.
static inline bool db_km_value(const char *payload, char *out, size_t outsz)
{
    if (!payload || !out || outsz == 0) return false;
    const char *q = strstr(payload, "\"value\"");
    if (!q) return false;
    q = strchr(q, ':');
    if (!q) return false;
    q++;
    while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
    size_t i = 0;
    if (*q == '"') {                         // string value
        q++;
        while (*q && *q != '"' && i + 1 < outsz) out[i++] = *q++;
    } else {                                 // number / bool / literal
        while (*q && *q != ',' && *q != '}' && *q != ' ' &&
               *q != '\r' && *q != '\n' && *q != '\t' && i + 1 < outsz)
            out[i++] = *q++;
    }
    out[i] = '\0';
    return true;
}

// ---- topic → field classification -----------------------------------------
// The desired-state lives on gcode_macro DRAGONBREATH; the heartbeat on
// gcode_macro DB_LINK. Split-status topic is:
//   <INST>/klipper/state/gcode_macro DRAGONBREATH/<statename>
// (the macro object name keeps its literal space). Classify by the trailing
// <statename> plus which macro object it belongs to.
typedef enum {
    DB_KM_F_NONE = 0,
    DB_KM_F_SEQ,
    DB_KM_F_TARGET,
    DB_KM_F_MODE,
    DB_KM_F_FAN,
    DB_KM_F_ARMED,
    DB_KM_F_PURGE,
    DB_KM_F_HEARTBEAT,
} db_km_field_t;

static inline db_km_field_t db_km_field_of(const char *topic)
{
    if (!topic) return DB_KM_F_NONE;
    bool is_db   = strstr(topic, "gcode_macro DRAGONBREATH/") != NULL;
    bool is_link = strstr(topic, "gcode_macro DB_LINK/") != NULL;
    if (!is_db && !is_link) return DB_KM_F_NONE;
    const char *slash = strrchr(topic, '/');
    const char *name = slash ? slash + 1 : topic;
    if (is_link) return (strcmp(name, "heartbeat") == 0) ? DB_KM_F_HEARTBEAT : DB_KM_F_NONE;
    if (strcmp(name, "seq")         == 0) return DB_KM_F_SEQ;
    if (strcmp(name, "target")      == 0) return DB_KM_F_TARGET;
    if (strcmp(name, "mode")        == 0) return DB_KM_F_MODE;
    if (strcmp(name, "fan")         == 0) return DB_KM_F_FAN;
    if (strcmp(name, "armed")       == 0) return DB_KM_F_ARMED;
    if (strcmp(name, "purge_nonce") == 0) return DB_KM_F_PURGE;
    return DB_KM_F_NONE;
}

// ---- arming state machine --------------------------------------------------
typedef struct {
    // latest desired-state fields
    long   seq;          bool seq_known;
    long   acted_seq;    // last seq we engaged/disengaged on (baseline on connect)
    float  target;
    bool   mode_heat;    // mode == "heat"
    int    armed;        // 0 | 1
    uint8_t fan;         // 0..100
    long   purge_nonce;  bool purge_known;
    // heartbeat liveness
    long    hb;          bool hb_known;
    int64_t hb_change_us;
    bool    hb_live;     // observed >=1 heartbeat change since connect
    // engagement
    bool   engaged;
} db_km_arm_t;

// Result of an eval step — the transition the caller must apply.
typedef enum {
    DB_KM_HOLD = 0,   // no change
    DB_KM_ENGAGE,     // (re)apply heat at out_target — take/refresh the lease
    DB_KM_DISENGAGE,  // normal off (armed->0, mode->off, or a coherent off update)
    DB_KM_COMMS_LOST, // liveness lost while engaged — force off + latch comms_lost
} db_km_action_t;

// Call on every (re)connect: clears all liveness/arming state to DISARMED so that
// retained desired-state cannot arm without a fresh, live re-arm.
static inline void db_km_arm_reset(db_km_arm_t *st)
{
    memset(st, 0, sizeof(*st));
}

static inline void db_km_arm_seq(db_km_arm_t *st, long v)
{
    if (!st->seq_known) { st->seq_known = true; st->acted_seq = v; }  // baseline
    st->seq = v;
}
static inline void db_km_arm_target(db_km_arm_t *st, float v) { st->target = v; }
static inline void db_km_arm_mode(db_km_arm_t *st, const char *m)
{
    st->mode_heat = (m && strcmp(m, "heat") == 0);
}
static inline void db_km_arm_armed(db_km_arm_t *st, int v) { st->armed = (v != 0); }
static inline void db_km_arm_fan(db_km_arm_t *st, int v)
{
    st->fan = (uint8_t)(v < 0 ? 0 : (v > 100 ? 100 : v));
}
// Returns true if the purge nonce advanced (a fresh one-shot request), false on the
// connect-baseline value (a retained nonce must not re-fire).
static inline bool db_km_arm_purge(db_km_arm_t *st, long v)
{
    if (!st->purge_known) { st->purge_known = true; st->purge_nonce = v; return false; }
    if (v != st->purge_nonce) { st->purge_nonce = v; return true; }
    return false;
}
static inline void db_km_arm_hb(db_km_arm_t *st, long v, int64_t now_us)
{
    if (!st->hb_known) { st->hb_known = true; st->hb = v; st->hb_change_us = now_us; return; }
    if (v != st->hb) { st->hb = v; st->hb_change_us = now_us; st->hb_live = true; }
}

// True when the heartbeat has been observed to change since connect AND the last
// change was within the timeout window.
static inline bool db_km_arm_live(const db_km_arm_t *st, int64_t now_us, int64_t timeout_us)
{
    return st->hb_live && (now_us - st->hb_change_us) < timeout_us;
}

// Evaluate the current state. Quick to disarm, careful to arm (see file header).
// On DB_KM_ENGAGE, *out_target holds the target to drive.
static inline db_km_action_t db_km_arm_eval(db_km_arm_t *st, int64_t now_us,
                                            int64_t timeout_us, float *out_target)
{
    bool live = db_km_arm_live(st, now_us, timeout_us);
    if (out_target) *out_target = st->target;

    // 1) Safety: drop engagement immediately on lost liveness / disarm / mode-off —
    //    no seq change required.
    if (st->engaged) {
        if (!live)                          { st->engaged = false; return DB_KM_COMMS_LOST; }
        if (st->armed == 0 || !st->mode_heat) { st->engaged = false; return DB_KM_DISENGAGE; }
    }

    // 2) Act only on a NEW coherent seq (the macro writes seq LAST).
    if (st->seq_known && st->seq != st->acted_seq) {
        st->acted_seq = st->seq;
        if (st->armed == 1 && st->mode_heat && live) {
            st->engaged = true;
            if (out_target) *out_target = st->target;
            return DB_KM_ENGAGE;            // engage or refresh target
        }
        if (st->engaged) { st->engaged = false; return DB_KM_DISENGAGE; }
    }
    return DB_KM_HOLD;
}
