// SPDX-License-Identifier: MIT
#pragma once

#include <math.h>

#if defined(CONFIG_PB_HEATER_PID_BACKEND_DC_PID)
#include "dc_pid.h"
typedef dc_pid_state_t pb_pid_backend_state_t;
#else
#include "pb_pid.h"
typedef pb_pid_state_t pb_pid_backend_state_t;
#endif

static inline void pb_pid_backend_reset(pb_pid_backend_state_t *state)
{
#if defined(CONFIG_PB_HEATER_PID_BACKEND_DC_PID)
    dc_pid_reset(state);
#else
    pb_pid_reset(state);
#endif
}

// Product adapter around the selected math backend. The generic controller never
// owns heater policy: DragonBreath still commands zero immediately at/above target,
// bounds duty to [0,1], and applies approach shaping and every safety veto later.
static inline float pb_pid_backend_step(pb_pid_backend_state_t *state,
                                        float target_c, float measured_c, float dt_s,
                                        float kp, float ki, float kd, float d_alpha)
{
#if defined(CONFIG_PB_HEATER_PID_BACKEND_DC_PID)
    if (!state || !isfinite(target_c) || !isfinite(measured_c) ||
        measured_c >= target_c) {
        dc_pid_reset(state);
        return 0.0f;
    }

    const dc_pid_config_t config = {
        .kp = kp,
        .ki = ki,
        .kd = kd,
        .derivative_alpha = d_alpha,
        .output_min = 0.0f,
        .output_max = 1.0f,
        .integral_min = 0.0f,
        .integral_max = 1.0f,
    };
    dc_pid_result_t result;
    if (!dc_pid_step(state, &config, target_c, measured_c, dt_s, true, &result))
        return 0.0f;
    return result.output;
#else
    return pb_pid_step(state, target_c, measured_c, dt_s, kp, ki, kd, d_alpha);
#endif
}
