// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <math.h>

// Conservative heater-only PID core for slow chamber regulation. Output is a
// normalized duty fraction [0,1] consumed by pb_heater's slow time-proportioning
// SSR window. Safety cutoffs/foldbacks remain outside this controller.
typedef struct {
    float integral;
    float prev_error;
    float d_filtered;
    bool initialized;
} pb_pid_state_t;

static inline void pb_pid_reset(pb_pid_state_t *s)
{
    if (!s) return;
    s->integral = 0.0f;
    s->prev_error = 0.0f;
    s->d_filtered = 0.0f;
    s->initialized = false;
}

static inline float pb_pid_clamp01(float v)
{
    if (v <= 0.0f) return 0.0f;
    if (v >= 1.0f) return 1.0f;
    return v;
}

// One PID update. Gains use output-fraction units:
//   kp: fraction / degC
//   ki: fraction / (degC*s)
//   kd: fraction*s / degC
// d_alpha is the derivative low-pass coefficient in (0,1].
//
// The controller is intentionally heater-only: once measurement >= target it
// commands zero and bleeds integral down, rather than using stored integral to
// continue heating into an overshoot. Conditional integration provides basic
// anti-windup while saturated.
static inline float pb_pid_step(pb_pid_state_t *s,
                                float target_c, float measured_c, float dt_s,
                                float kp, float ki, float kd, float d_alpha)
{
    if (!s || !isfinite(target_c) || !isfinite(measured_c) ||
        !isfinite(dt_s) || dt_s <= 0.0f ||
        !isfinite(kp) || !isfinite(ki) || !isfinite(kd)) {
        if (s) pb_pid_reset(s);
        return 0.0f;
    }

    if (d_alpha <= 0.0f) d_alpha = 0.01f;
    if (d_alpha > 1.0f) d_alpha = 1.0f;

    float error = target_c - measured_c;
    if (!s->initialized) {
        s->prev_error = error;
        s->d_filtered = 0.0f;
        s->initialized = true;
    }

    float d_raw = (error - s->prev_error) / dt_s;
    s->d_filtered += d_alpha * (d_raw - s->d_filtered);
    s->prev_error = error;

    // Heating can only push temperature upward. At/above target, command zero
    // immediately and unwind stored integral so the next heat request starts sane.
    if (error <= 0.0f) {
        s->integral *= 0.90f;
        if (fabsf(s->integral) < 0.0001f) s->integral = 0.0f;
        return 0.0f;
    }

    float p = kp * error;
    float d = kd * s->d_filtered;
    float candidate_i = s->integral + ki * error * dt_s;
    float candidate = p + candidate_i + d;

    // Conditional integration: accept I when unsaturated, or when it would move
    // a saturated output back toward the legal range. Positive-error heating only
    // means the useful anti-windup case here is avoiding further growth at 100%.
    if (candidate < 1.0f || error < 0.0f)
        s->integral = candidate_i;

    return pb_pid_clamp01(p + s->integral + d);
}
