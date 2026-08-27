// SPDX-License-Identifier: MIT
#pragma once

#include "esp_err.h"

#include <math.h>
#include <stdbool.h>

// Persisted setup selector restored from DragonBreath PR #89. The absent or
// malformed value is deliberately OFF, keeping the local chamber NTC as the
// process variable on fresh devices and upgrades.
#define DB_BAMBU_CHAMBER_CONTROL_NVS_NAMESPACE "app_nvs"
#define DB_BAMBU_CHAMBER_CONTROL_NVS_KEY       "bb_ch_ctl"

void db_bambu_chamber_control_load(void);
bool db_bambu_chamber_control_get(void);
esp_err_t db_bambu_chamber_control_set(bool enabled);

// Return an eligible Bambu process-variable sample, or NAN to select the local
// chamber NTC. dc_bambu invalidates stale chamber samples, while the connected
// gate also covers missing/disconnected telemetry.
static inline float db_bambu_chamber_control_source(bool enabled,
                                                     bool connected,
                                                     float bambu_chamber_c)
{
    return enabled && connected && isfinite(bambu_chamber_c)
        ? bambu_chamber_c
        : NAN;
}
