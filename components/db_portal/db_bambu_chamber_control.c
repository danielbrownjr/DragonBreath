// SPDX-License-Identifier: MIT
#include "db_bambu_chamber_control.h"

#include "nvs.h"

#include <stdint.h>

static volatile bool s_enabled;

void db_bambu_chamber_control_load(void)
{
    uint8_t enabled = 0;
    nvs_handle_t handle;
    if (nvs_open(DB_BAMBU_CHAMBER_CONTROL_NVS_NAMESPACE,
                 NVS_READONLY, &handle) == ESP_OK) {
        if (nvs_get_u8(handle, DB_BAMBU_CHAMBER_CONTROL_NVS_KEY,
                       &enabled) != ESP_OK || enabled > 1) {
            enabled = 0;
        }
        nvs_close(handle);
    }
    s_enabled = enabled == 1;
}

bool db_bambu_chamber_control_get(void)
{
    return s_enabled;
}

esp_err_t db_bambu_chamber_control_set(bool enabled)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(DB_BAMBU_CHAMBER_CONTROL_NVS_NAMESPACE,
                             NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_u8(handle, DB_BAMBU_CHAMBER_CONTROL_NVS_KEY,
                     enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);

    // The setup API reports the active value. Do not change it unless the new
    // selection was durably committed.
    if (err == ESP_OK) s_enabled = enabled;
    return err;
}
