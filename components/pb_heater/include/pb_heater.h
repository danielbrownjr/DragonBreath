// SPDX-License-Identifier: MIT
// pb_heater — chamber heater control with defense-in-depth over-temp safety.
//
// Hardware backstops that exist REGARDLESS of this firmware (see docs/SAFETY.md):
//   1. Bonded thermal cutoff in the PTC's mains lead (opens on element over-temp,
//      upstream of the SSR — covers a welded-shut SSR).
//   2. PTC ceramic self-limiting (Curie point) — cannot thermally run away.
// This component is the third (soft) layer: correct set-point control + an
// element/chamber over-temp cutoff + a comms-loss watchdog.
#pragma once

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// Machine-readable fault cause. Persisted across reboot as a u8 (see
// pb_heater_load_fault); a live free-form reason string (pb_heater_fault_reason)
// carries session detail that is NOT persisted. Append new causes at the END so
// the persisted numeric codes stay stable across firmware versions.
typedef enum {
    PB_FAULT_NONE = 0,
    PB_FAULT_PTC_OVERTEMP,        // element over-temp cutoff
    PB_FAULT_CHAMBER_OVERTEMP,    // chamber over-temp cutoff
    PB_FAULT_CHAMBER_SENSOR,      // chamber thermistor open/short/uninit while armed
    PB_FAULT_PTC_SENSOR,          // PTC thermistor open/short/uninit while armed
    PB_FAULT_LINK_LOST,           // comms-loss watchdog while heating
    PB_FAULT_PANIC_OFF,           // front-panel long-press panic-off
    PB_FAULT_INHIBITED,           // permanent inhibit (reboot-only)
    PB_FAULT_EMERGENCY,           // generic external emergency_off trip
    PB_FAULT_NVS_UNREADABLE,      // boot fail-safe: persisted fault state unreadable
    PB_FAULT__COUNT               // sentinel: any stored code >= this is treated as
                                  // corrupt and mapped to a generic latched fault
} pb_fault_reason_t;

// Persisted user-selectable chamber controller. Numeric values are stored in NVS;
// append only. PID=0 preserves the behavior of existing devices and empty flash.
typedef enum {
    PB_HEATER_CONTROL_PID = 0,
    PB_HEATER_CONTROL_BANG_BANG,
    PB_HEATER_CONTROL__COUNT,
} pb_heater_control_algorithm_t;

typedef enum {
    PB_HEATER_PROCESS_LOCAL_NTC = 0,
    PB_HEATER_PROCESS_BAMBU,
    PB_HEATER_PROCESS_UNAVAILABLE,
} pb_heater_process_source_t;

typedef struct {
    pb_heater_control_algorithm_t algorithm;
    pb_heater_process_source_t process_source;
    float process_variable_c;
    float pid_output_duty;       // backend output before approach limiting / SSR window
    float requested_duty;        // controller demand after approach limiting, before vetoes
    float approach_cap;
    bool approach_limited;
    bool bang_bang_demand;
    bool bambu_preference;
    bool bambu_effective;
    bool local_foldback_active;
    bool element_foldback_active;
    bool output_constrained;
} pb_heater_control_snapshot_t;

// Pure fail-safe decision for the boot-time fault restore (pb_heater_load_fault),
// inline so it can be host-tested without an NVS backend. Given the outcome of
// reading the persisted latch/code, decides whether to come up latched and with
// which code:
//   - namespace/key never written (fresh device)  -> NOT latched.
//   - a genuine read error (open or latch read)    -> FAIL SAFE: latch, NVS_UNREADABLE.
//   - a readable latch flag == 1                   -> latched with the stored code;
//       an out-of-range/NONE stored code is corrupt -> latch anyway (PB_FAULT_EMERGENCY).
// open_ok / latch_read_ok are false ONLY on a real error (the not-found cases are
// signalled separately via ns_not_found / latch_not_found).
static inline bool pb_heater_fault_decide(bool open_ok, bool ns_not_found,
                                          bool latch_read_ok, bool latch_not_found,
                                          uint8_t latch_val, uint8_t code_val,
                                          pb_fault_reason_t *out_code)
{
    pb_fault_reason_t code = PB_FAULT_NONE;
    bool latched = false;
    if (ns_not_found || latch_not_found) {
        latched = false;                                   // fresh device / never latched
    } else if (!open_ok || !latch_read_ok) {
        latched = true; code = PB_FAULT_NVS_UNREADABLE;    // can't read -> fail safe
    } else if (latch_val) {
        latched = true;
        code = (code_val < PB_FAULT__COUNT && code_val != PB_FAULT_NONE)
               ? (pb_fault_reason_t)code_val : PB_FAULT_EMERGENCY;   // corrupt code -> generic
    }
    if (out_code) *out_code = code;
    return latched;
}

// --- Safety limits ----------------------------------------------------------
// Fixed hardware-safety cutoffs — NEVER user-configurable.
#define PB_HEATER_PTC_CUTOFF_C   105.0f   // element over-temp -> force off (stock parity)
#define PB_HEATER_CHAMBER_MAX_C  85.0f    // chamber over-temp -> force off
#define PB_HEATER_HYSTERESIS_C     1.0f    // legacy bang-bang band below target

// When regulation uses a remote/printer chamber sensor, the local chamber NTC can
// be much hotter because it sits in DragonBreath's outlet/near-heater airflow. Keep
// that local region below a non-latching SOFT ceiling instead of relying on the 85 C
// emergency trip as normal control. This limiter applies only while an external
// control temperature is active; local-sensor regulation is unchanged.
#define PB_HEATER_LOCAL_FOLDBACK_CUT_C     72.0f
#define PB_HEATER_LOCAL_FOLDBACK_RESUME_C  67.0f

// Pure hysteresis helper for the local thermal limiter. A bad local read holds the
// previous state; the fail-closed sensor-fault path in pb_heater_eval_trip() remains
// authoritative while armed.
static inline bool pb_heater_local_foldback_cut(bool chamber_ok, float chamber_c,
                                                 bool prev_cut)
{
    if (!chamber_ok) return prev_cut;
    if (chamber_c >= PB_HEATER_LOCAL_FOLDBACK_CUT_C) return true;
    if (chamber_c < PB_HEATER_LOCAL_FOLDBACK_RESUME_C) return false;
    return prev_cut;
}
// Soft element-temperature FOLDBACK, a layer strictly BELOW the hard cutoff: it holds
// the PTC element just under the 105 C trip so a marginal/hot install can reach set-point
// instead of tripping and needing a manual clear. It never adds heat and never delays the
// hard cutoff — if the element still reaches PB_HEATER_PTC_CUTOFF_C (welded SSR, runaway,
// sensor issue) the latching trip fires exactly as before.
//
// Implemented as a HYSTERESIS hard-cut rather than a proportional duty ramp: a linear
// ramp (full power at 100 C -> 0 only at 105 C) proved too gentle on a hot-running board
// — ~50% duty at 102-103 C kept feeding the element so it ratcheted up to ~104.8 C (just
// shy of the 105 C watchdog) with no real cool-down between the fast on/off ticks. The
// hysteresis instead forces the SSR FULLY OFF at the cut point and keeps it off until the
// element cools below the resume point, giving a genuine cool-down cycle and capping the
// peak with margin. The thresholds are PER BOARD VARIANT (selected by the Rref strap, see
// pb_ntc_rref_kohm()): the 33 kOhm (V1.0) board runs the element hotter / ratchets, so it
// gets a lower, more conservative cut; the 82 kOhm (V1.0.1) board tops out cooler and can
// hold a higher cut without approaching 105. Both stay below the 105 C hard cutoff.
#define PB_HEATER_PTC_FOLDBACK_CUT_82K_C     102.0f   // 82k board: element >= this -> cut
#define PB_HEATER_PTC_FOLDBACK_RESUME_82K_C   99.0f   //           hold off until below this
#define PB_HEATER_PTC_FOLDBACK_CUT_33K_C      99.0f   // 33k board: element >= this -> cut
#define PB_HEATER_PTC_FOLDBACK_RESUME_33K_C   96.0f   //           hold off until below this

// Resolve the foldback cut/resume thresholds for a board's Rref (kOhm). The 33 kOhm
// variant (incl. a floating strap defaulted to 33k) gets the conservative pair; anything
// else uses the 82 kOhm pair (the higher, more permissive default).
static inline void pb_heater_foldback_thresholds(int rref_kohm, float *cut_c, float *resume_c)
{
    if (rref_kohm == 33) { *cut_c = PB_HEATER_PTC_FOLDBACK_CUT_33K_C; *resume_c = PB_HEATER_PTC_FOLDBACK_RESUME_33K_C; }
    else                 { *cut_c = PB_HEATER_PTC_FOLDBACK_CUT_82K_C; *resume_c = PB_HEATER_PTC_FOLDBACK_RESUME_82K_C; }
}

// Optional user override of the foldback CUT (advanced setting; see the settable API
// below). The resume point always trails the cut by one hysteresis band. The override
// is bounded well under the fixed 105 C hard cutoff, so it can never defeat over-temp
// protection — it only shifts where the SOFT foldback engages. 0 = "auto" (use the
// board's per-Rref default from pb_heater_foldback_thresholds).
#define PB_HEATER_PTC_FOLDBACK_BAND_C   3.0f    // resume = cut - band
#define PB_HEATER_FB_CUT_MIN_C          90.0f
#define PB_HEATER_FB_CUT_MAX_C         104.0f   // < 105 hard cutoff, always

// Resolve the EFFECTIVE cut/resume: a positive user override wins (resume = cut - band),
// otherwise fall back to the board's per-Rref default. Pure/inline for host testing.
static inline void pb_heater_effective_foldback(float user_cut_c, int rref_kohm,
                                                 float *cut_c, float *resume_c)
{
    if (user_cut_c > 0.0f) { *cut_c = user_cut_c; *resume_c = user_cut_c - PB_HEATER_PTC_FOLDBACK_BAND_C; }
    else                     pb_heater_foldback_thresholds(rref_kohm, cut_c, resume_c);
}

// Pure element-foldback hysteresis, inline so it is host-testable without hardware.
// Returns whether to FORCE the SSR off this tick given the element temperature, whether
// its reading is valid, the previous cut-latch state, and the board's cut/resume points:
//   ptc >= cut_c               -> cut  (true)
//   ptc <  resume_c            -> allow (false)
//   resume_c <= ptc < cut_c    -> hold the previous state (the hysteresis band)
//   reading not OK             -> hold the previous state (a bad read is owned by the
//                                 sensor-fault trip / hard cutoff, not by the foldback)
static inline bool pb_heater_foldback_cut(bool ptc_ok, float ptc_c, bool prev_cut,
                                          float cut_c, float resume_c)
{
    if (!ptc_ok)            return prev_cut;
    if (ptc_c >= cut_c)     return true;
    if (ptc_c <  resume_c)  return false;
    return prev_cut;
}
// Settable set-point ceiling: default + absolute cap + floor. Raising the
// production cap above 70 C is a separate hw-validation item (80 C leaves only
// 5 C below the fixed 85 C chamber trip — not enough margin without measured
// worst-case lag/overshoot), so ABS_MAX stays at 70.
#define PB_HEATER_MAX_TARGET_C_DEFAULT  70.0f
#define PB_HEATER_ABS_MAX_TARGET_C      70.0f
#define PB_HEATER_MIN_TARGET_C          30.0f
// Comms deadman: no controller for this long while heating -> latch off. Runtime-
// configurable within [MIN, MAX]; never disabled or extended beyond MAX (5 min).
#define PB_HEATER_COMMS_TIMEOUT_MS_DEFAULT  (5 * 60 * 1000)
#define PB_HEATER_COMMS_TIMEOUT_MS_MIN      (10 * 1000)
#define PB_HEATER_COMMS_TIMEOUT_MS_MAX      (5 * 60 * 1000)
// Residual-heat cooldown-purge "cool down to" temperature: after heat ran, the fan
// keeps running until the chamber + PTC drop below this, then stops. Raise it for a
// hot room where the sensors can't reach the default (else the fan runs forever).
// This is a comfort/wear setting, NOT a safety cutoff — fault airflow and the fixed
// over-temp trips are independent.
#define PB_HEATER_COOL_RELEASE_C_DEFAULT    40.0f
#define PB_HEATER_COOL_RELEASE_MIN_C        30.0f
#define PB_HEATER_COOL_RELEASE_MAX_C        65.0f

// Pure safety-trip decision for pb_heater_tick(), inline so the fail-closed
// priority ordering can be host-tested without the ADC/SSR/RTOS backend. Given the
// freshest per-channel sensor reads (status-OK flags + instantaneous °C), whether a
// target is armed, and whether the comms deadman has expired, returns the fault the
// tick must latch — or PB_FAULT_NONE if it is safe to run the selected controller. The
// order is load-bearing and MUST stay identical to pb_heater_tick():
//   1. PTC over-temp     — only trusted when the PTC sensor reads valid
//   2. chamber over-temp — only trusted when the chamber sensor reads valid
//   3. armed + chamber sensor NOT ok -> fail-closed (blind heater)
//   4. armed + PTC sensor NOT ok     -> fail-closed (unmonitored element)
//   5. armed + comms deadman expired -> link-lost watchdog
// The over-temp cutoffs fire regardless of `armed`; the sensor-fault and comms
// trips are gated on `armed` (an idle heater with a disconnected sensor is not a
// hazard). A non-OK sensor also means its temperature is NAN — the `*_ok` gate on
// each over-temp check keeps a NAN comparison from ever masking a real trip.
static inline pb_fault_reason_t pb_heater_eval_trip(
    bool ptc_ok, float ptc_c, bool chamber_ok, float chamber_c,
    bool armed, bool link_expired)
{
    if (ptc_ok && ptc_c >= PB_HEATER_PTC_CUTOFF_C)          return PB_FAULT_PTC_OVERTEMP;
    if (chamber_ok && chamber_c >= PB_HEATER_CHAMBER_MAX_C) return PB_FAULT_CHAMBER_OVERTEMP;
    if (armed && !chamber_ok)                               return PB_FAULT_CHAMBER_SENSOR;
    if (armed && !ptc_ok)                                   return PB_FAULT_PTC_SENSOR;
    if (armed && link_expired)                              return PB_FAULT_LINK_LOST;
    return PB_FAULT_NONE;
}

// Select the process variable for the one chamber PID path. A usable external
// sample wins only after the local chamber sensor is known-good; otherwise the
// local NTC is used. Safety evaluation remains separate and always consumes the
// local NTC/PTC readings directly.
static inline float pb_heater_select_process_variable(
    bool local_chamber_ok, float local_chamber_c, float external_chamber_c)
{
    if (!local_chamber_ok || !isfinite(local_chamber_c)) return NAN;
    return isfinite(external_chamber_c) ? external_chamber_c : local_chamber_c;
}

// Bang-bang is intentionally local-only. PID may consume the optional external
// Bambu sample; a missing/invalid sample falls back to the valid local NTC.
static inline float pb_heater_controller_process_variable(
    pb_heater_control_algorithm_t algorithm,
    bool local_chamber_ok, float local_chamber_c, float external_chamber_c)
{
    if (algorithm == PB_HEATER_CONTROL_BANG_BANG)
        return local_chamber_ok && isfinite(local_chamber_c) ? local_chamber_c : NAN;
    return pb_heater_select_process_variable(
        local_chamber_ok, local_chamber_c, external_chamber_c);
}

static inline bool pb_heater_bang_bang_step(bool previous_demand,
                                            float target_c, float local_chamber_c)
{
    if (!isfinite(target_c) || !isfinite(local_chamber_c) || target_c <= 0.0f)
        return false;
    if (local_chamber_c < target_c - PB_HEATER_HYSTERESIS_C) return true;
    if (local_chamber_c >= target_c) return false;
    return previous_demand;
}

// A no-op selection is always allowed. A real controller transition requires both
// an unarmed target and a physically de-energized SSR.
static inline bool pb_heater_algorithm_change_allowed(
    pb_heater_control_algorithm_t current,
    pb_heater_control_algorithm_t requested,
    float target_c, bool ssr_on)
{
    return current == requested || (target_c <= 0.0f && !ssr_on);
}

static inline bool pb_heater_algorithm_change_requires_reset(
    pb_heater_control_algorithm_t current,
    pb_heater_control_algorithm_t requested)
{
    return current != requested;
}

// The chamber has a large amount of stored heat and keeps climbing after the SSR
// backs off. Taper maximum heater power as the selected process variable approaches
// target so PID cannot charge the thermal mass at full power right up to setpoint.
// Hardware data showed that the former 20% tier inside 2 C was below the roughly
// 33% holding duty at 53 C, creating a limit cycle at that tier boundary instead
// of allowing the integral term to reach a 55 C target. Keep 40% available through
// the final approach; the heater still commands zero at/above target in the shared
// backend adapter. This is control shaping only; all safety cutoffs remain separate.
static inline float pb_heater_pid_approach_max_duty(float error_c)
{
    if (error_c <= 0.0f) return 0.0f;
    if (error_c < 5.0f)  return 0.40f;
    if (error_c < 10.0f) return 0.70f;
    return 1.0f;
}

// Bring up the SSR GPIO in a guaranteed-OFF state. Call before anything can
// request heat. Idempotent.
esp_err_t pb_heater_init(void);

// Set the desired chamber temperature (clamped to the runtime ceiling returned
// by pb_heater_get_max_target_c(), never above PB_HEATER_ABS_MAX_TARGET_C).
// 0 (or below) disables heating. Returns:
//   ESP_OK             - accepted
//   ESP_ERR_INVALID_ARG   - non-finite (NaN/Inf) target, rejected
//   ESP_ERR_INVALID_STATE - a positive target while a safety fault is latched
//                           (clear it with pb_heater_clear_fault first); maps to
//                           HTTP 409. Heat is never queued behind a fault latch.
esp_err_t pb_heater_set_target_c(float target_c);
float pb_heater_get_target_c(void);

// Optional external chamber measurement used ONLY as the process variable for
// PID control. Pass NAN to select the local chamber NTC. Bang-bang control always
// uses the local NTC regardless of this value. The local
// chamber NTC and PTC remain authoritative for over-temperature trips,
// sensor-fault detection, and all other safety decisions regardless.
void pb_heater_set_control_chamber_c(float temp_c);

// --- Runtime-configurable, persisted settings (pb_heater is the sole owner) ---
// Load persisted settings from NVS (namespace app_nvs). MUST be called AFTER
// nvs_init() — pb_heater_init() only sets conservative defaults (nvs isn't up
// yet). Values are clamped on read.
void  pb_heater_load_config(void);
// Algorithm changes are persisted and rejected while a positive target or SSR
// output is active. A successful change resets PID and bang-bang runtime state on
// the control task before the next demand calculation.
esp_err_t pb_heater_set_control_algorithm(pb_heater_control_algorithm_t algorithm);
pb_heater_control_algorithm_t pb_heater_get_control_algorithm(void);
const char *pb_heater_control_algorithm_str(pb_heater_control_algorithm_t algorithm);

// Mirror the separately persisted Bambu preference into read-only controller
// diagnostics. This does not change eligibility; PID + a finite external sample
// is still required before Bambu becomes the effective process variable.
void pb_heater_set_bambu_chamber_preference(bool enabled);
void pb_heater_get_control_snapshot(pb_heater_control_snapshot_t *snapshot);
// Restore a persisted fault latch from NVS. MUST be called AFTER nvs_init() and
// BEFORE the control task / HTTP API are exposed, so a device that latched off
// before a power cycle comes back up heater-OFF and commands-inhibited rather than
// silently ready to heat. Fail-safe: if the persisted state cannot be read
// reliably it latches a generic fault (PB_FAULT_NVS_UNREADABLE). A namespace that
// was never written (fresh device) is NOT a fault.
void  pb_heater_load_fault(void);
// Settable set-point ceiling. Setter clamps to [MIN_TARGET_C, ABS_MAX_TARGET_C],
// persists it, and pulls a live target down if it now exceeds the cap.
esp_err_t pb_heater_set_max_target_c(float max_c);
float     pb_heater_get_max_target_c(void);
// Comms deadman timeout. Setter clamps to [MS_MIN, MS_MAX] (never disabled or
// extended past 5 min) and persists it.
esp_err_t pb_heater_set_comms_timeout_ms(uint32_t ms);
uint32_t  pb_heater_get_comms_timeout_ms(void);
// Cooldown-purge "cool down to" temperature. Setter clamps to
// [COOL_RELEASE_MIN_C, COOL_RELEASE_MAX_C] and persists it. Read by pb_policy's
// residual-heat purge (pb_purge_decide).
esp_err_t pb_heater_set_cool_release_c(float c);
float     pb_heater_get_cool_release_c(void);
// Advanced: user override of the element-foldback CUT temperature. Setter clamps a
// positive value to [FB_CUT_MIN_C, FB_CUT_MAX_C] and persists it; pass <= 0 to clear
// the override and return to the board's per-Rref default. Getter returns the raw
// stored value (0 = auto). The resume point always trails by one hysteresis band.
esp_err_t pb_heater_set_fb_cut_c(float c);
float     pb_heater_get_fb_cut_c(void);

// Feed the comms watchdog: call whenever a live controller link is confirmed
// (Moonraker connected, or a fresh command). If not called within
// pb_heater_get_comms_timeout_ms() while heating, the heater latches off.
void pb_heater_notify_link_alive(void);

// Periodic control tick (call at ~1-2 Hz). Reads the local chamber + PTC temps,
// enforces all safety cutoffs from those local sensors, and drives the SSR with
// the selected PID or legacy local-NTC bang-bang controller.
void pb_heater_tick(void);

// Immediate, latching shutoff. Clears the target and latches off. Heat stays off
// until pb_heater_clear_fault() is called AND the condition has cleared.
// CONTROL-TASK ONLY: it writes the SSR GPIO directly, so calling it from another
// task would break the single-SSR-writer invariant. Off-task callers use
// pb_heater_request_panic_off() instead.
void pb_heater_emergency_off(const char *reason);

// Latch the heater off from ANY task without touching the SSR GPIO. Sets the
// same latch/target/reason as emergency_off but leaves the physical drop to the
// next pb_heater_tick() (control task), so the single-writer invariant holds.
// Used by the front-panel long-press ("panic-off"). To minimize the drop
// latency, wake the control task immediately after calling this.
void pb_heater_request_panic_off(const char *reason);

// Explicitly clear a latched safety fault (over-temp / sensor fault / comms loss).
// Setting a new target does NOT auto-clear it — this is a deliberate reset. The
// next tick re-evaluates safety and re-latches if the fault still holds. Also
// clears the persisted latch in NVS. Returns:
//   ESP_OK                - cleared (and the NVS clear committed)
//   ESP_ERR_INVALID_STATE - a permanent inhibit is latched (not clearable)
//   other                 - the RAM latch was cleared but persisting the clear
//                           FAILED (so it would return on reboot); callers should
//                           surface this (HTTP 500) rather than report success.
esp_err_t pb_heater_clear_fault(void);

// PERMANENT inhibit: force the heater off and refuse all heat until reboot. Unlike
// a latched fault this is NOT clearable by pb_heater_clear_fault()/API clear_fault —
// only a power cycle lifts it. Use for conditions under which the heater must
// never run again this boot (e.g. the control-loop watchdog could not be armed,
// so a hung loop would go undetected). Reports as a fault in API v2 state.
void pb_heater_inhibit(const char *reason);

// True once pb_heater_inhibit() has been called (reboot-only).
bool pb_heater_is_inhibited(void);

// True if a safety trip has latched the heater off (needs an explicit clear).
bool pb_heater_is_faulted(void);

// The reason string from the most recent trip (NULL if none / after clear). This
// is live session detail and is NOT persisted; after a reboot-restored latch it
// reflects the canonical string for the persisted code.
const char *pb_heater_fault_reason(void);

// The machine-readable cause of the current latch (PB_FAULT_NONE if not faulted).
// Stable across firmware versions and survives a reboot, unlike the reason string.
pb_fault_reason_t pb_heater_fault_code(void);

// Canonical, stable string for a fault code (never NULL; out-of-range -> generic).
const char *pb_heater_fault_str(pb_fault_reason_t code);

// True if the SSR is currently commanded on.
bool pb_heater_is_on(void);

// True in "heat mode": a target is armed and no fault is latched. Steady across
// controller's output cycling — use this (not pb_heater_is_on) for fan/LED.
bool pb_heater_heat_mode(void);
