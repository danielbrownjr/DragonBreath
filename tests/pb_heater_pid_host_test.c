// Host regression coverage for DragonBreath's dc_pid adapter and product-owned
// chamber/SSR shaping. Heater safety itself remains in pb_heater.h.
#include "pb_heater.h"
#include "pb_heater_pid.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
#define CHECK_NEAR(a, b, eps) CHECK(fabsf((a) - (b)) <= (eps))

static void test_known_gains_and_heater_policy(void)
{
    pb_heater_pid_state_t state = {0};
    float duty = -1.0f;

    CHECK(pb_heater_pid_step(&state, 60.0f, 52.0f, true, &duty));
    CHECK(isfinite(duty));
    CHECK(duty > 0.19f && duty < 0.22f);
    CHECK(state.controller.initialized);

    for (int i = 0; i < 120; ++i)
        CHECK(pb_heater_pid_step(&state, 60.0f, 52.0f, true, &duty));
    CHECK(duty > 0.30f && duty < 0.40f);

    // DragonBreath is heater-only: exact/above target commands zero, while
    // dc_pid history remains coherent rather than being reset at every crossing.
    CHECK(pb_heater_pid_step(&state, 60.0f, 60.0f, true, &duty));
    CHECK(duty == 0.0f);
    CHECK(state.controller.initialized);
    CHECK(pb_heater_pid_step(&state, 60.0f, 61.0f, true, &duty));
    CHECK(duty == 0.0f);
    CHECK(state.controller.prev_measurement == 61.0f);
}

static void test_process_variable_selection(void)
{
    bool external = true;
    CHECK(pb_heater_pid_process_variable(53.0f, NAN, &external) == 53.0f);
    CHECK(!external);
    CHECK(pb_heater_pid_process_variable(53.0f, 31.0f, &external) == 31.0f);
    CHECK(external);
}

static void test_approach_and_ssr_window(void)
{
    CHECK(pb_heater_pid_approach_max_duty(-0.1f) == 0.0f);
    CHECK(pb_heater_pid_approach_max_duty(0.0f) == 0.0f);
    CHECK(pb_heater_pid_approach_max_duty(0.1f) == 0.40f);
    CHECK(pb_heater_pid_approach_max_duty(4.99f) == 0.40f);
    CHECK(pb_heater_pid_approach_max_duty(5.0f) == 0.70f);
    CHECK(pb_heater_pid_approach_max_duty(9.99f) == 0.70f);
    CHECK(pb_heater_pid_approach_max_duty(10.0f) == 1.0f);

    pb_heater_pid_state_t state = {0};
    const int64_t start = 1000000;
    CHECK(pb_heater_pid_window_on(&state, 0.30f, start));
    CHECK(pb_heater_pid_window_on(&state, 0.30f, start + 2999999));
    CHECK(!pb_heater_pid_window_on(&state, 0.30f, start + 3000000));
    CHECK(!pb_heater_pid_window_on(&state, 0.30f, start + 9999999));
    CHECK(pb_heater_pid_window_on(&state, 0.30f, start + 10000000));
    CHECK(!pb_heater_pid_window_on(&state, 0.0f, start + 10000001));
}

static void test_safety_inhibition_holds_integral(void)
{
    pb_heater_pid_state_t state = {0};
    float duty = 0.0f;
    for (int i = 0; i < 80; ++i)
        CHECK(pb_heater_pid_step(&state, 60.0f, 55.0f, true, &duty));

    float held_integral = state.controller.integral;
    CHECK(held_integral > 0.0f);

    // This is the path used while either local foldback governor forces the
    // heater off: I is pinned, but the process-variable history still advances.
    CHECK(pb_heater_pid_step(&state, 60.0f, 54.0f, false, &duty));
    CHECK_NEAR(state.controller.integral, held_integral, 0.000001f);
    CHECK(state.controller.prev_measurement == 54.0f);

    bool local_cut = pb_heater_local_foldback_cut(true, 72.0f, false);
    CHECK(local_cut);
    CHECK(duty > 0.0f); // controller demand can exist behind the governor
    bool drive = !local_cut && pb_heater_pid_window_on(&state, duty, 1000000);
    CHECK(!drive);      // local thermal authority dominates PID demand

    CHECK(pb_heater_pid_step(&state, 60.0f, 54.0f, true, &duty));
    CHECK(state.controller.integral > held_integral);
}

static void test_source_transition_reset(void)
{
    pb_heater_pid_state_t state = {0};
    float duty = 0.0f;
    CHECK(pb_heater_pid_set_source(&state, false));
    CHECK(state.source_known);
    CHECK(!state.using_external);
    CHECK(pb_heater_pid_step(&state, 55.0f, 50.0f, true, &duty));
    CHECK(state.controller.initialized);
    CHECK(pb_heater_pid_window_on(&state, duty, 1000000));

    CHECK(!pb_heater_pid_set_source(&state, false));
    CHECK(state.controller.initialized); // same source preserves history

    // Switching between the local NTC and Bambu telemetry discards history
    // associated with the physically different process variable.
    CHECK(pb_heater_pid_set_source(&state, true));
    CHECK(!state.controller.initialized);
    CHECK(state.controller.integral == 0.0f);
    CHECK(state.window_start_us == 0);
    CHECK(!state.window_initialized);
    CHECK(state.source_known);
    CHECK(state.using_external);
}

int main(void)
{
    test_known_gains_and_heater_policy();
    test_process_variable_selection();
    test_approach_and_ssr_window();
    test_safety_inhibition_holds_integral();
    test_source_transition_reset();
    puts("pb_heater dc_pid host checks: PASS");
    return 0;
}
