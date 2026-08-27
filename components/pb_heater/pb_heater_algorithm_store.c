// SPDX-License-Identifier: MIT
#include "pb_heater_algorithm_store.h"

#include "nvs.h"

pb_heater_control_algorithm_t pb_heater_algorithm_load_persisted(void)
{
    pb_heater_control_algorithm_t algorithm = PB_HEATER_CONTROL_BANG_BANG;
    nvs_handle_t handle;
    if (nvs_open(PB_HEATER_ALGORITHM_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK)
        return algorithm;

    uint8_t stored = 0;
    if (nvs_get_u8(handle, PB_HEATER_ALGORITHM_NVS_KEY, &stored) == ESP_OK &&
        stored < PB_HEATER_CONTROL__COUNT)
        algorithm = (pb_heater_control_algorithm_t)stored;
    nvs_close(handle);
    return algorithm;
}

esp_err_t pb_heater_algorithm_persist(pb_heater_control_algorithm_t algorithm)
{
    if (algorithm < PB_HEATER_CONTROL_PID || algorithm >= PB_HEATER_CONTROL__COUNT)
        return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(PB_HEATER_ALGORITHM_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(handle, PB_HEATER_ALGORITHM_NVS_KEY, (uint8_t)algorithm);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}
