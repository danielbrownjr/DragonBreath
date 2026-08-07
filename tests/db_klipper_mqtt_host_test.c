// Host unit test for the MQTT-Klipper arming core (db_klipper_mqtt_arm.h): the
// retained-aware arm/heartbeat state machine + split-status value extraction +
// topic field classification. Pure logic, no ESP deps.
//
// The arming tests encode the safety contract from
// plans/mqtt-klipper-implementation-design.md §5.
#include "db_klipper_mqtt_arm.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg) do { \
    int ok_ = (cond); if (!ok_) fails++; \
    printf("[%s] %s\n", ok_ ? "PASS" : "FAIL", msg); } while (0)

// 15 s liveness window (3 x 5 s heartbeat), in microseconds.
#define TIMEOUT_US ((int64_t)15 * 1000 * 1000)
#define SEC_US(s)  ((int64_t)(s) * 1000 * 1000)

int main(void)
{
    // ---------- split-status value extraction ----------
    char v[32];
    CHECK(db_km_value("{\"eventtime\":123.4,\"value\":55.0}", v, sizeof v) && strcmp(v, "55.0") == 0,
          "value: number");
    CHECK(db_km_value("{\"value\":\"heat\",\"eventtime\":1}", v, sizeof v) && strcmp(v, "heat") == 0,
          "value: string (quotes stripped)");
    CHECK(db_km_value("{\"eventtime\":1,\"value\":true}", v, sizeof v) && strcmp(v, "true") == 0,
          "value: bool");
    CHECK(db_km_value("{\"eventtime\": 9, \"value\" : 7 }", v, sizeof v) && strcmp(v, "7") == 0,
          "value: whitespace tolerant");
    CHECK(!db_km_value("{\"eventtime\":1}", v, sizeof v), "value: absent -> false");

    // ---------- topic field classification (literal space in object name) ----------
    CHECK(db_km_field_of("p/klipper/state/gcode_macro DRAGONBREATH/seq")    == DB_KM_F_SEQ,    "field: seq");
    CHECK(db_km_field_of("p/klipper/state/gcode_macro DRAGONBREATH/target") == DB_KM_F_TARGET, "field: target");
    CHECK(db_km_field_of("p/klipper/state/gcode_macro DRAGONBREATH/armed")  == DB_KM_F_ARMED,  "field: armed");
    CHECK(db_km_field_of("p/klipper/state/gcode_macro DB_LINK/heartbeat")   == DB_KM_F_HEARTBEAT, "field: heartbeat");
    CHECK(db_km_field_of("p/klipper/state/gcode_macro OTHER/seq")           == DB_KM_F_NONE,   "field: other macro -> none");
    CHECK(db_km_field_of("p/moonraker/status")                             == DB_KM_F_NONE,   "field: non-state -> none");

    float tgt;
    db_km_action_t a;

    // ---------- retained snapshot must NOT arm ----------
    // On connect, retained desired-state (armed=1, mode heat, seq=7, target=55) and a
    // retained heartbeat=100 all arrive as the baseline snapshot. No live heartbeat
    // change yet -> must stay disarmed.
    {
        db_km_arm_t st; db_km_arm_reset(&st);
        db_km_arm_seq(&st, 7);
        db_km_arm_target(&st, 55.0f);
        db_km_arm_mode(&st, "heat");
        db_km_arm_armed(&st, 1);
        db_km_arm_hb(&st, 100, SEC_US(0));         // snapshot heartbeat (not live)
        a = db_km_arm_eval(&st, SEC_US(1), TIMEOUT_US, &tgt);
        CHECK(a == DB_KM_HOLD && !st.engaged, "retained snapshot does not arm");
    }

    // ---------- arm only after a fresh seq + live heartbeat ----------
    {
        db_km_arm_t st; db_km_arm_reset(&st);
        db_km_arm_seq(&st, 7);                     // baseline
        db_km_arm_target(&st, 55.0f);
        db_km_arm_mode(&st, "heat");
        db_km_arm_armed(&st, 1);
        db_km_arm_hb(&st, 100, SEC_US(0));         // baseline hb
        // heartbeat increments (liveness) but seq unchanged -> still no engage
        db_km_arm_hb(&st, 101, SEC_US(5));
        a = db_km_arm_eval(&st, SEC_US(5), TIMEOUT_US, &tgt);
        CHECK(a == DB_KM_HOLD && !st.engaged, "live heartbeat alone (no new seq) does not arm");
        // now a fresh coherent desired-state (macro re-arm bumps seq)
        db_km_arm_seq(&st, 8);
        a = db_km_arm_eval(&st, SEC_US(6), TIMEOUT_US, &tgt);
        CHECK(a == DB_KM_ENGAGE && st.engaged && tgt == 55.0f, "new seq + armed + live -> ENGAGE");
    }

    // ---------- heartbeat timeout while engaged -> comms lost ----------
    {
        db_km_arm_t st; db_km_arm_reset(&st);
        db_km_arm_seq(&st, 1); db_km_arm_target(&st, 50.0f);
        db_km_arm_mode(&st, "heat"); db_km_arm_armed(&st, 1);
        db_km_arm_hb(&st, 0, SEC_US(0));
        db_km_arm_hb(&st, 1, SEC_US(5));           // live at t=5
        db_km_arm_seq(&st, 2);
        a = db_km_arm_eval(&st, SEC_US(6), TIMEOUT_US, &tgt);
        CHECK(a == DB_KM_ENGAGE && st.engaged, "engaged for timeout test");
        // no more heartbeats; at t=5+16s (>15s since last change) liveness is gone
        a = db_km_arm_eval(&st, SEC_US(21), TIMEOUT_US, &tgt);
        CHECK(a == DB_KM_COMMS_LOST && !st.engaged, "heartbeat timeout -> COMMS_LOST + off");
    }

    // ---------- re-arm after recovery requires a NEW seq ----------
    {
        db_km_arm_t st; db_km_arm_reset(&st);
        db_km_arm_seq(&st, 1); db_km_arm_target(&st, 50.0f);
        db_km_arm_mode(&st, "heat"); db_km_arm_armed(&st, 1);
        db_km_arm_hb(&st, 0, SEC_US(0)); db_km_arm_hb(&st, 1, SEC_US(5));
        db_km_arm_seq(&st, 2);
        db_km_arm_eval(&st, SEC_US(6), TIMEOUT_US, &tgt);                 // engaged
        db_km_arm_eval(&st, SEC_US(30), TIMEOUT_US, &tgt);               // comms lost -> off
        // heartbeat resumes, armed still 1, but seq unchanged -> must NOT re-engage
        db_km_arm_hb(&st, 2, SEC_US(31));
        a = db_km_arm_eval(&st, SEC_US(32), TIMEOUT_US, &tgt);
        CHECK(a == DB_KM_HOLD && !st.engaged, "recovery without new seq stays off");
        // explicit re-arm (macro bumps seq) -> engages
        db_km_arm_seq(&st, 3);
        a = db_km_arm_eval(&st, SEC_US(33), TIMEOUT_US, &tgt);
        CHECK(a == DB_KM_ENGAGE && st.engaged, "explicit re-arm after recovery -> ENGAGE");
    }

    // ---------- armed=0 disengages promptly (no seq needed) ----------
    {
        db_km_arm_t st; db_km_arm_reset(&st);
        db_km_arm_seq(&st, 1); db_km_arm_target(&st, 50.0f);
        db_km_arm_mode(&st, "heat"); db_km_arm_armed(&st, 1);
        db_km_arm_hb(&st, 0, SEC_US(0)); db_km_arm_hb(&st, 1, SEC_US(5));
        db_km_arm_seq(&st, 2);
        db_km_arm_eval(&st, SEC_US(6), TIMEOUT_US, &tgt);                 // engaged
        db_km_arm_armed(&st, 0);                                          // disarm field
        a = db_km_arm_eval(&st, SEC_US(7), TIMEOUT_US, &tgt);
        CHECK(a == DB_KM_DISENGAGE && !st.engaged, "armed=0 disengages promptly");
    }

    // ---------- mode=off disengages promptly ----------
    {
        db_km_arm_t st; db_km_arm_reset(&st);
        db_km_arm_seq(&st, 1); db_km_arm_target(&st, 50.0f);
        db_km_arm_mode(&st, "heat"); db_km_arm_armed(&st, 1);
        db_km_arm_hb(&st, 0, SEC_US(0)); db_km_arm_hb(&st, 1, SEC_US(5));
        db_km_arm_seq(&st, 2);
        db_km_arm_eval(&st, SEC_US(6), TIMEOUT_US, &tgt);
        db_km_arm_mode(&st, "off");
        a = db_km_arm_eval(&st, SEC_US(7), TIMEOUT_US, &tgt);
        CHECK(a == DB_KM_DISENGAGE && !st.engaged, "mode=off disengages promptly");
    }

    // ---------- target update takes effect only on a new seq ----------
    {
        db_km_arm_t st; db_km_arm_reset(&st);
        db_km_arm_seq(&st, 1); db_km_arm_target(&st, 50.0f);
        db_km_arm_mode(&st, "heat"); db_km_arm_armed(&st, 1);
        db_km_arm_hb(&st, 0, SEC_US(0)); db_km_arm_hb(&st, 1, SEC_US(5));
        db_km_arm_seq(&st, 2);
        db_km_arm_eval(&st, SEC_US(6), TIMEOUT_US, &tgt);
        CHECK(tgt == 50.0f, "engaged at 50");
        db_km_arm_target(&st, 60.0f);                                     // new target field, no seq bump
        a = db_km_arm_eval(&st, SEC_US(7), TIMEOUT_US, &tgt);
        CHECK(a == DB_KM_HOLD, "target change without seq bump does not re-apply");
        db_km_arm_seq(&st, 3);                                            // coherent update
        a = db_km_arm_eval(&st, SEC_US(8), TIMEOUT_US, &tgt);
        CHECK(a == DB_KM_ENGAGE && tgt == 60.0f, "target applied on new seq");
    }

    // ---------- purge nonce: baseline does not fire, advance fires ----------
    {
        db_km_arm_t st; db_km_arm_reset(&st);
        CHECK(db_km_arm_purge(&st, 5) == false, "retained purge nonce (baseline) does not fire");
        CHECK(db_km_arm_purge(&st, 5) == false, "same nonce does not fire");
        CHECK(db_km_arm_purge(&st, 6) == true,  "advanced nonce fires");
    }

    printf(fails ? "\n%d FAILED\n" : "\nALL PASS\n", fails);
    return fails ? 1 : 0;
}
