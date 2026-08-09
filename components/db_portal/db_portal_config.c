// SPDX-License-Identifier: MIT
#include "db_portal_config.h"

#include <stdio.h>
#include <string.h>

static esp_err_t invalid_field(const char *field, char *message, size_t message_size)
{
    snprintf(message, message_size, "Invalid or overlong value for %s.", field);
    return ESP_ERR_INVALID_ARG;
}

static esp_err_t stage_text(const db_portal_text_value_t *value, char *destination,
                            size_t destination_size, bool secret, bool *changed,
                            const char *field, char *message, size_t message_size)
{
    if (!value->present || (secret && value->value && !value->value[0])) return ESP_OK;
    if (!value->value || strlen(value->value) >= destination_size)
        return invalid_field(field, message, message_size);
    if (strcmp(destination, value->value)) {
        snprintf(destination, destination_size, "%s", value->value);
        *changed = true;
    }
    return ESP_OK;
}

static void stage_port(const db_portal_port_value_t *value, uint16_t *destination,
                       bool *changed)
{
    if (value->present && *destination != value->value) {
        *destination = value->value;
        *changed = true;
    }
}

static void stage_bool(const db_portal_bool_value_t *value, bool *destination,
                       bool *changed)
{
    if (value->present && *destination != value->value) {
        *destination = value->value;
        *changed = true;
    }
}

esp_err_t db_portal_plan_product_save(
    const db_portal_product_request_t *request,
    dc_ctl_source_t current_source,
    const dc_moonraker_config_t *current_moonraker,
    const dc_bambu_config_t *current_bambu,
    const pb_ha_config_t *current_ha,
    const db_km_config_t *current_klipper_mqtt,
    db_portal_product_plan_t *plan,
    char *message,
    size_t message_size)
{
    if (!request || !current_moonraker || !current_bambu || !current_ha ||
        !current_klipper_mqtt || !plan || !message || !message_size)
        return ESP_ERR_INVALID_ARG;
    if (request->source_present &&
        (request->source < DC_SRC_KLIPPER || request->source >= DC_SRC_MAX))
        return invalid_field("ctl_src", message, message_size);
    if ((request->mr_port.present && !request->mr_port.value) ||
        (request->ha_port.present && !request->ha_port.value) ||
        (request->km_port.present && !request->km_port.value))
        return invalid_field("port", message, message_size);

    memset(plan, 0, sizeof(*plan));
    plan->source = current_source;
    plan->moonraker = *current_moonraker;
    plan->bambu = *current_bambu;
    plan->ha = *current_ha;
    plan->klipper_mqtt = *current_klipper_mqtt;

    if (request->source_present && request->source != current_source) {
        plan->source = request->source;
        plan->source_changed = true;
    }

#define STAGE_TEXT(group, request_member, destination_member, secret) do { \
    esp_err_t err = stage_text(&request->request_member, plan->group.destination_member, \
                               sizeof(plan->group.destination_member), secret, \
                               &plan->group##_changed, #request_member, message, message_size); \
    if (err != ESP_OK) return err; \
} while (0)

    STAGE_TEXT(moonraker, mr_host, host, false);
    STAGE_TEXT(moonraker, mr_key, api_key, true);
    stage_port(&request->mr_port, &plan->moonraker.port, &plan->moonraker_changed);

    STAGE_TEXT(bambu, bb_host, host, false);
    STAGE_TEXT(bambu, bb_serial, serial, false);
    STAGE_TEXT(bambu, bb_code, code, true);

    STAGE_TEXT(ha, ha_host, host, false);
    STAGE_TEXT(ha, ha_user, user, false);
    STAGE_TEXT(ha, ha_pass, pass, true);
    STAGE_TEXT(ha, ha_topic, topic, false);
    stage_port(&request->ha_port, &plan->ha.port, &plan->ha_changed);

    STAGE_TEXT(klipper_mqtt, km_host, host, false);
    STAGE_TEXT(klipper_mqtt, km_user, user, false);
    STAGE_TEXT(klipper_mqtt, km_pass, pass, true);
    STAGE_TEXT(klipper_mqtt, km_inst, inst, false);
    STAGE_TEXT(klipper_mqtt, km_topic, topic, false);
    stage_port(&request->km_port, &plan->klipper_mqtt.port,
               &plan->klipper_mqtt_changed);
    stage_bool(&request->km_tls, &plan->klipper_mqtt.tls,
               &plan->klipper_mqtt_changed);
    stage_bool(&request->km_writeback, &plan->klipper_mqtt.writeback,
               &plan->klipper_mqtt_changed);

#undef STAGE_TEXT
    message[0] = '\0';
    return ESP_OK;
}
