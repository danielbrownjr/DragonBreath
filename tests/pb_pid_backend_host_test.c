// SPDX-License-Identifier: MIT
#include "pb_pid_backend.h"
#include "db_bambu_chamber_control.h"
#include "pb_heater.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

int main(void)
{
    pb_pid_backend_state_t state = {0};
    float output = pb_pid_backend_step(&state, 60.0f, 52.0f, 0.5f,
                                       0.04f, 0.0008f, 0.02f, 0.20f);
    assert(isfinite(output));
    assert(output > 0.0f && output <= 1.0f);

    for (int i = 0; i < 1000; ++i)
        output = pb_pid_backend_step(&state, 70.0f, 0.0f, 0.5f,
                                     0.04f, 0.0008f, 0.02f, 0.20f);
    assert(output == 1.0f);

    // Heater policy lives in the adapter, independent of the math backend.
    output = pb_pid_backend_step(&state, 60.0f, 60.0f, 0.5f,
                                 0.04f, 0.0008f, 0.02f, 0.20f);
    assert(output == 0.0f);
#if defined(CONFIG_PB_HEATER_PID_BACKEND_DC_PID)
    // Crossing setpoint must force heater demand to zero without erasing the
    // controller's derivative/integral history. A full reset here made the slow
    // chamber rebuild holding duty after every crossing and produced a large
    // repeatable limit cycle on hardware.
    assert(state.initialized);
#endif

    output = pb_pid_backend_step(&state, 60.0f, 61.0f, 0.5f,
                                 0.04f, 0.0008f, 0.02f, 0.20f);
    assert(output == 0.0f);
#if defined(CONFIG_PB_HEATER_PID_BACKEND_DC_PID)
    assert(state.initialized);
#endif

    output = pb_pid_backend_step(&state, 60.0f, NAN, 0.5f,
                                 0.04f, 0.0008f, 0.02f, 0.20f);
    assert(output == 0.0f);

    // Regression for the hardware-observed 55 C / local-NTC limit cycle. The
    // former 20% tier began at 2 C error and trapped the chamber around 53 C.
    // Approach authority must now remain 40% on both sides of that old boundary,
    // while zero/negative error still authoritatively requests no heat.
    assert(pb_heater_pid_approach_max_duty(2.1f) == 0.40f);
    assert(pb_heater_pid_approach_max_duty(2.0f) == 0.40f);
    assert(pb_heater_pid_approach_max_duty(1.9f) == 0.40f);
    assert(pb_heater_pid_approach_max_duty(0.1f) == 0.40f);
    assert(pb_heater_pid_approach_max_duty(0.0f) == 0.0f);
    assert(pb_heater_pid_approach_max_duty(-0.1f) == 0.0f);

    // The selector changes only the process variable; OFF, fresh ON, and stale
    // ON all execute this same selected backend (dc_pid by default, pb_pid in the
    // retained fallback build). A hot Bambu reading suppresses demand only when
    // enabled and fresh; otherwise the cooler local NTC produces positive demand.
    const float local_c = 55.0f;
    const float bambu_c = 61.0f;
    float external_c = db_bambu_chamber_control_source(false, true, bambu_c);
    float process_c = pb_heater_select_process_variable(true, local_c, external_c);
    pb_pid_backend_reset(&state);
    output = pb_pid_backend_step(&state, 60.0f, process_c, 0.5f,
                                 0.04f, 0.0008f, 0.02f, 0.20f);
    assert(process_c == local_c && output > 0.0f);

    external_c = db_bambu_chamber_control_source(true, true, bambu_c);
    process_c = pb_heater_select_process_variable(true, local_c, external_c);
    pb_pid_backend_reset(&state);
    output = pb_pid_backend_step(&state, 60.0f, process_c, 0.5f,
                                 0.04f, 0.0008f, 0.02f, 0.20f);
    assert(process_c == bambu_c && output == 0.0f);

    external_c = db_bambu_chamber_control_source(true, true, NAN);
    process_c = pb_heater_select_process_variable(true, local_c, external_c);
    pb_pid_backend_reset(&state);
    output = pb_pid_backend_step(&state, 60.0f, process_c, 0.5f,
                                 0.04f, 0.0008f, 0.02f, 0.20f);
    assert(process_c == local_c && output > 0.0f);

    // The restored legacy bang-bang controller turns on below target-1 C, holds
    // its previous state throughout the band, and turns off at/above target.
    bool bang = pb_heater_bang_bang_step(false, 55.0f, 53.9f);
    assert(bang);
    assert(pb_heater_bang_bang_step(bang, 55.0f, 54.0f));
    assert(pb_heater_bang_bang_step(bang, 55.0f, 54.9f));
    bang = pb_heater_bang_bang_step(bang, 55.0f, 55.0f);
    assert(!bang);
    assert(!pb_heater_bang_bang_step(bang, 55.0f, 54.5f));

    // Bang-bang hard-selects the local sensor even when a fresh Bambu value is
    // present; PID restores that external sample without changing preference.
    assert(pb_heater_controller_process_variable(
               PB_HEATER_CONTROL_BANG_BANG, true, local_c, bambu_c) == local_c);
    assert(pb_heater_controller_process_variable(
               PB_HEATER_CONTROL_PID, true, local_c, bambu_c) == bambu_c);

    // Real algorithm changes are rejected while target demand or SSR output is
    // active. An accepted idle transition explicitly requires state reset.
    assert(!pb_heater_algorithm_change_allowed(
        PB_HEATER_CONTROL_PID, PB_HEATER_CONTROL_BANG_BANG, 55.0f, false));
    assert(!pb_heater_algorithm_change_allowed(
        PB_HEATER_CONTROL_PID, PB_HEATER_CONTROL_BANG_BANG, 0.0f, true));
    assert(pb_heater_algorithm_change_allowed(
        PB_HEATER_CONTROL_PID, PB_HEATER_CONTROL_BANG_BANG, 0.0f, false));
    assert(pb_heater_algorithm_change_requires_reset(
        PB_HEATER_CONTROL_PID, PB_HEATER_CONTROL_BANG_BANG));
    assert(!pb_heater_algorithm_change_requires_reset(
        PB_HEATER_CONTROL_PID, PB_HEATER_CONTROL_PID));

    pb_pid_backend_reset(&state);
    puts("pb_pid backend host checks: PASS");
    return 0;
}
