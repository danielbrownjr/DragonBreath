// SPDX-License-Identifier: MIT
#include "db_bambu_chamber_control.h"
#include "pb_heater.h"

#include "nvs.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static bool s_present;
static uint8_t s_stored;
static uint8_t s_staged;
static bool s_staged_present;
static bool s_fail_commit;
static bool s_mirrored_preference;

void pb_heater_set_bambu_chamber_preference(bool enabled)
{
    s_mirrored_preference = enabled;
}

esp_err_t nvs_open(const char *namespace_name, nvs_open_mode_t mode,
                   nvs_handle_t *out_handle)
{
    assert(strcmp(namespace_name, DB_BAMBU_CHAMBER_CONTROL_NVS_NAMESPACE) == 0);
    (void)mode;
    *out_handle = 1;
    return ESP_OK;
}

esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out_value)
{
    assert(handle == 1);
    assert(strcmp(key, DB_BAMBU_CHAMBER_CONTROL_NVS_KEY) == 0);
    if (!s_present) return ESP_ERR_NVS_NOT_FOUND;
    *out_value = s_stored;
    return ESP_OK;
}

esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    assert(handle == 1);
    assert(strcmp(key, DB_BAMBU_CHAMBER_CONTROL_NVS_KEY) == 0);
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
    // Absent preference defaults OFF. Both controllers therefore use the local
    // chamber NTC even with a fresh Bambu sample available.
    db_bambu_chamber_control_load();
    assert(!db_bambu_chamber_control_get());
    assert(!s_mirrored_preference);
    float selected = db_bambu_chamber_control_source(
        db_bambu_chamber_control_get(), true, 42.0f);
    assert(isnan(selected));
    assert(pb_heater_select_process_variable(true, 55.0f, selected) == 55.0f);

    // ON + connected + finite/fresh Bambu telemetry selects the Bambu sample for
    // bang-bang, while PID remains authoritatively local.
    assert(db_bambu_chamber_control_set(true) == ESP_OK);
    assert(db_bambu_chamber_control_get());
    assert(s_mirrored_preference);
    selected = db_bambu_chamber_control_source(
        db_bambu_chamber_control_get(), true, 42.0f);
    assert(selected == 42.0f);
    assert(pb_heater_select_process_variable(true, 55.0f, selected) == 42.0f);

    // Enabling PID suspends EFFECTIVE Bambu use without touching the separately
    // persisted bang-bang preference. Disabling PID immediately restores the
    // fresh Bambu sample because no "previous state" NVS bookkeeping is needed.
    assert(pb_heater_controller_process_variable(
               PB_HEATER_CONTROL_BANG_BANG, true, 55.0f, selected) == 42.0f);
    assert(db_bambu_chamber_control_get());
    assert(pb_heater_controller_process_variable(
               PB_HEATER_CONTROL_PID, true, 55.0f, selected) == 55.0f);
    assert(db_bambu_chamber_control_get());
    assert(pb_heater_controller_process_variable(
               PB_HEATER_CONTROL_BANG_BANG, true, 55.0f, selected) == 42.0f);

    // Disconnected, missing, invalid, and freshness-expired (NAN) telemetry all
    // select NAN at the source seam and therefore fall back to the local NTC.
    selected = db_bambu_chamber_control_source(true, false, 42.0f);
    assert(isnan(selected));
    assert(pb_heater_select_process_variable(true, 55.0f, selected) == 55.0f);
    selected = db_bambu_chamber_control_source(true, true, NAN);
    assert(isnan(selected));
    assert(pb_heater_select_process_variable(true, 55.0f, selected) == 55.0f);
    selected = db_bambu_chamber_control_source(true, true, INFINITY);
    assert(isnan(selected));

    // The persisted setting survives a simulated reboot/load. A failed commit
    // must not make the setup API claim a selection that was not saved.
    db_bambu_chamber_control_load();
    assert(db_bambu_chamber_control_get());
    s_fail_commit = true;
    assert(db_bambu_chamber_control_set(false) == ESP_FAIL);
    assert(db_bambu_chamber_control_get());
    s_fail_commit = false;
    s_staged_present = false;
    assert(db_bambu_chamber_control_set(false) == ESP_OK);
    db_bambu_chamber_control_load();
    assert(!db_bambu_chamber_control_get());
    assert(!s_mirrored_preference);

    // Preference OFF stays OFF across both controller selections and is never
    // implicitly changed by a mode switch.
    selected = db_bambu_chamber_control_source(
        db_bambu_chamber_control_get(), true, 42.0f);
    assert(isnan(selected));
    assert(pb_heater_controller_process_variable(
               PB_HEATER_CONTROL_BANG_BANG, true, 55.0f, selected) == 55.0f);
    assert(pb_heater_controller_process_variable(
               PB_HEATER_CONTROL_PID, true, 55.0f, selected) == 55.0f);

    // A malformed persisted byte fails safe to the documented OFF default.
    s_stored = 2;
    db_bambu_chamber_control_load();
    assert(!db_bambu_chamber_control_get());

    puts("Bambu chamber-control selection/persistence checks: PASS");
    return 0;
}
