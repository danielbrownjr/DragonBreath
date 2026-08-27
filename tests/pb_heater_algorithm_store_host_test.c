// SPDX-License-Identifier: MIT
#include "pb_heater_algorithm_store.h"

#include "nvs.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static bool s_present;
static uint8_t s_stored;
static uint8_t s_staged;
static bool s_staged_present;
static bool s_fail_commit;

esp_err_t nvs_open(const char *namespace_name, nvs_open_mode_t mode,
                   nvs_handle_t *out_handle)
{
    assert(strcmp(namespace_name, PB_HEATER_ALGORITHM_NVS_NAMESPACE) == 0);
    (void)mode;
    *out_handle = 1;
    return ESP_OK;
}

esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out_value)
{
    assert(handle == 1);
    assert(strcmp(key, PB_HEATER_ALGORITHM_NVS_KEY) == 0);
    if (!s_present) return ESP_ERR_NVS_NOT_FOUND;
    *out_value = s_stored;
    return ESP_OK;
}

esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    assert(handle == 1);
    assert(strcmp(key, PB_HEATER_ALGORITHM_NVS_KEY) == 0);
    s_staged = value;
    s_staged_present = true;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == 1);
    if (s_fail_commit) return ESP_FAIL;
    if (s_staged_present) {
        s_stored = s_staged;
        s_present = true;
        s_staged_present = false;
    }
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    assert(handle == 1);
}

int main(void)
{
    // Empty flash preserves the known-good shipped PID default.
    assert(pb_heater_algorithm_load_persisted() == PB_HEATER_CONTROL_PID);

    assert(pb_heater_algorithm_persist(PB_HEATER_CONTROL_BANG_BANG) == ESP_OK);
    assert(pb_heater_algorithm_load_persisted() == PB_HEATER_CONTROL_BANG_BANG);

    // A failed commit cannot replace the durable selection.
    s_fail_commit = true;
    assert(pb_heater_algorithm_persist(PB_HEATER_CONTROL_PID) == ESP_FAIL);
    s_fail_commit = false;
    s_staged_present = false;
    assert(pb_heater_algorithm_load_persisted() == PB_HEATER_CONTROL_BANG_BANG);

    assert(pb_heater_algorithm_persist(PB_HEATER_CONTROL_PID) == ESP_OK);
    assert(pb_heater_algorithm_load_persisted() == PB_HEATER_CONTROL_PID);

    // A malformed future/corrupt byte fails back to PID.
    s_stored = PB_HEATER_CONTROL__COUNT;
    assert(pb_heater_algorithm_load_persisted() == PB_HEATER_CONTROL_PID);
    assert(pb_heater_algorithm_persist(PB_HEATER_CONTROL__COUNT) == ESP_ERR_INVALID_ARG);

    puts("heater control-algorithm persistence checks: PASS");
    return 0;
}
