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

    pb_pid_backend_reset(&state);
    puts("pb_pid backend host checks: PASS");
    return 0;
}
