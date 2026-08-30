// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <math.h>

#include "dc_pid.h"

// DragonBreath-owned chamber-controller policy. dc_pid supplies only the generic
// PID math; sensor selection, safety inhibition, approach limiting, and the SSR
// time-proportioning actuator remain product responsibilities.
#define PB_HEATER_PID_KP         0.0250f
#define PB_HEATER_PID_KI         0.0003f
#define PB_HEATER_PID_KD         0.0400f
#define PB_HEATER_PID_D_ALPHA    0.20f
#define PB_HEATER_PID_DT_S       0.50f
#define PB_HEATER_PID_WINDOW_US  10000000LL

typedef struct {
    dc_pid_state_t controller;
    int64_t window_start_us;
    bool window_initialized;
    bool source_known;
    bool using_external;
} pb_heater_pid_state_t;

// Select the effective process variable while keeping source policy explicit.
// The caller decides whether external telemetry is eligible; NAN represents
// unavailable/stale/not-authorized and therefore falls back to the local NTC.
static inline float pb_heater_pid_process_variable(float local_chamber_c,
                                                    float external_chamber_c,
                                                    bool *using_external)
{
    bool external = isfinite(external_chamber_c);
    if (using_external) *using_external = external;
    return external ? external_chamber_c : local_chamber_c;
}

static inline void pb_heater_pid_reset(pb_heater_pid_state_t *state)
{
    if (!state) return;
    dc_pid_reset(&state->controller);
    state->window_start_us = 0;
    state->window_initialized = false;
    state->source_known = false;
    state->using_external = false;
}

// Prevent derivative/integral history from crossing between physically distinct
// chamber sensors. Returns true when a new source became active.
static inline bool pb_heater_pid_set_source(pb_heater_pid_state_t *state,
                                            bool using_external)
{
    if (!state) return false;
    if (state->source_known && state->using_external == using_external)
        return false;

    pb_heater_pid_reset(state);
    state->source_known = true;
    state->using_external = using_external;
    return true;
}

static inline float pb_heater_pid_approach_max_duty(float error_c)
{
    if (error_c <= 0.0f) return 0.0f;
    if (error_c < 5.0f)  return 0.40f;
    if (error_c < 10.0f) return 0.70f;
    return 1.0f;
}

// Advance the common chamber PID path. When integrate is false, dc_pid still
// updates measurement/derivative history but holds the integral so product
// safety governors cannot hide accumulating demand behind an inhibited heater.
// DragonBreath's heater-only policy commands zero at/above target without
// discarding valid controller history.
static inline bool pb_heater_pid_step(pb_heater_pid_state_t *state,
                                      float target_c, float measurement_c,
                                      bool integrate, float *duty)
{
    if (duty) *duty = 0.0f;
    if (!state || !duty) return false;

    const dc_pid_config_t config = {
        .kp = PB_HEATER_PID_KP,
        .ki = PB_HEATER_PID_KI,
        .kd = PB_HEATER_PID_KD,
        .derivative_alpha = PB_HEATER_PID_D_ALPHA,
        .output_min = 0.0f,
        .output_max = 1.0f,
        .integral_min = 0.0f,
        .integral_max = 1.0f,
    };
    dc_pid_result_t result;
    if (!dc_pid_step(&state->controller, &config, target_c, measurement_c,
                     PB_HEATER_PID_DT_S, integrate, &result))
        return false;

    if (measurement_c >= target_c)
        return true;

    float approach_cap = pb_heater_pid_approach_max_duty(target_c - measurement_c);
    *duty = result.output < approach_cap ? result.output : approach_cap;
    return true;
}

// Convert normalized duty into zero-cross-SSR time proportioning. `now_us`
// may jump backwards across a timer reset; that simply starts a fresh window.
static inline bool pb_heater_pid_window_on(pb_heater_pid_state_t *state,
                                           float duty, int64_t now_us)
{
    if (!state || duty <= 0.0f) return false;
    if (duty > 1.0f) duty = 1.0f;

    if (!state->window_initialized || now_us < state->window_start_us) {
        state->window_start_us = now_us;
        state->window_initialized = true;
    }

    int64_t elapsed = now_us - state->window_start_us;
    if (elapsed >= PB_HEATER_PID_WINDOW_US) {
        int64_t windows = elapsed / PB_HEATER_PID_WINDOW_US;
        state->window_start_us += windows * PB_HEATER_PID_WINDOW_US;
        elapsed = now_us - state->window_start_us;
    }

    int64_t on_us = (int64_t)(duty * (float)PB_HEATER_PID_WINDOW_US);
    return on_us > 0 && elapsed < on_us;
}
