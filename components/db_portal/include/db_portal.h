// SPDX-License-Identifier: MIT
#pragma once
#include "esp_err.h"

// Starts dragon-core's shared SPA/provisioning service with DragonBreath's
// product schema, API routes, authorization, OTA identity and heat-safety policy.
esp_err_t db_portal_start(void);
