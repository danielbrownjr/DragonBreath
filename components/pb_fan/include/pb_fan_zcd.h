// SPDX-License-Identifier: MIT
// Pure zero-cross edge qualification shared by the ISR and host tests.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// The shortest legitimate half-cycle is about 8.33 ms at 60 Hz.  A 4 ms
// reject window blocks the observed ~1 ms duplicate/noise edge while retaining
// more than 4.3 ms of margin for normal detector and mains timing jitter.
#define PB_FAN_ZCD_MIN_SPACING_US 4000U

typedef struct {
    uint64_t last_accepted_us;
    bool have_accepted;
} pb_fan_zcd_filter_t;

// Qualify a raw positive edge.  Unsigned subtraction deliberately preserves
// correct elapsed time across uint64_t timestamp wrap.  interval_us is zero for
// the first accepted edge and otherwise reports accepted-to-accepted spacing.
static inline bool pb_fan_zcd_accept(pb_fan_zcd_filter_t *filter,
                                     uint64_t now_us,
                                     uint32_t *interval_us)
{
    uint64_t elapsed_us = 0;
    if (filter->have_accepted) {
        elapsed_us = now_us - filter->last_accepted_us;
        if (elapsed_us < PB_FAN_ZCD_MIN_SPACING_US) return false;
    }

    filter->last_accepted_us = now_us;
    filter->have_accepted = true;
    if (interval_us) {
        *interval_us = elapsed_us > UINT32_MAX
            ? UINT32_MAX
            : (uint32_t)elapsed_us;
    }
    return true;
}
