// SPDX-License-Identifier: MIT
#pragma once

#include "pb_heater.h"

#define PB_HEATER_ALGORITHM_NVS_NAMESPACE "app_nvs"
#define PB_HEATER_ALGORITHM_NVS_KEY       "heat_ctl_alg"

// Read the persisted controller selection. Missing, malformed, or unreadable
// state defaults safely to PID, which is the shipped/current behavior.
pb_heater_control_algorithm_t pb_heater_algorithm_load_persisted(void);

// Persist a validated controller selection. Runtime state is changed only after
// this succeeds, so the setup API never reports a choice that was not committed.
esp_err_t pb_heater_algorithm_persist(pb_heater_control_algorithm_t algorithm);
