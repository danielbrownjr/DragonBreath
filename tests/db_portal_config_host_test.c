#include "db_portal_config.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void current_configs(dc_moonraker_config_t *mr, dc_bambu_config_t *bb,
                            pb_ha_config_t *ha, db_km_config_t *km)
{
    snprintf(mr->host, sizeof(mr->host), "moonraker.lan");
    mr->port = 7125;
    snprintf(mr->api_key, sizeof(mr->api_key), "moon-secret");
    snprintf(bb->host, sizeof(bb->host), "bambu.lan");
    snprintf(bb->serial, sizeof(bb->serial), "01P00SERIAL");
    snprintf(bb->code, sizeof(bb->code), "bambu-secret");
    snprintf(ha->host, sizeof(ha->host), "ha-broker.lan");
    ha->port = 1883;
    snprintf(ha->user, sizeof(ha->user), "ha-user");
    snprintf(ha->pass, sizeof(ha->pass), "ha-secret");
    snprintf(ha->topic, sizeof(ha->topic), "dragonbreath");
    snprintf(km->host, sizeof(km->host), "km-broker.lan");
    km->port = 1883;
    snprintf(km->user, sizeof(km->user), "km-user");
    snprintf(km->pass, sizeof(km->pass), "km-secret");
    snprintf(km->inst, sizeof(km->inst), "printer");
    snprintf(km->topic, sizeof(km->topic), "dragonbreath");
    km->tls = false;
    km->writeback = true;
}

static esp_err_t plan_with_control(const db_portal_product_request_t *request,
                                   pb_heater_control_algorithm_t algorithm,
                                   bool heater_active,
                                   db_portal_product_plan_t *out,
                                   char *message, size_t size)
{
    dc_moonraker_config_t mr = {0};
    dc_bambu_config_t bb = {0};
    pb_ha_config_t ha = {0};
    db_km_config_t km = {0};
    current_configs(&mr, &bb, &ha, &km);
    return db_portal_plan_product_save(request, DC_SRC_KLIPPER, &mr, &bb, &ha,
                                       &km, algorithm, heater_active,
                                       out, message, size);
}

static esp_err_t plan(const db_portal_product_request_t *request,
                      db_portal_product_plan_t *out, char *message, size_t size)
{
    return plan_with_control(request, PB_HEATER_CONTROL_PID, false,
                             out, message, size);
}

int main(void)
{
    db_portal_product_plan_t result;
    char message[128];

    // Selecting None is an unbind only: every saved credential remains available.
    db_portal_product_request_t unbind = {
        .source_present = true,
        .source = DC_SRC_NONE,
    };
    assert(plan(&unbind, &result, message, sizeof(message)) == ESP_OK);
    assert(result.source_changed && result.source == DC_SRC_NONE);
    assert(!result.moonraker_changed && !result.bambu_changed &&
           !result.ha_changed && !result.klipper_mqtt_changed);
    assert(strcmp(result.moonraker.api_key, "moon-secret") == 0);
    assert(strcmp(result.bambu.code, "bambu-secret") == 0);
    assert(strcmp(result.ha.pass, "ha-secret") == 0);
    assert(strcmp(result.klipper_mqtt.pass, "km-secret") == 0);

    // A full SPA echo of unchanged fields must produce no NVS writes.
    db_portal_product_request_t unchanged = {
        .source_present = true, .source = DC_SRC_KLIPPER,
        .mr_host = {true, "moonraker.lan"}, .mr_port = {true, 7125},
        .mr_key = {true, ""},
        .bb_host = {true, "bambu.lan"}, .bb_serial = {true, "01P00SERIAL"},
        .bb_code = {true, ""},
        .ha_host = {true, "ha-broker.lan"}, .ha_port = {true, 1883},
        .ha_user = {true, "ha-user"}, .ha_pass = {true, ""},
        .ha_topic = {true, "dragonbreath"},
        .km_host = {true, "km-broker.lan"}, .km_port = {true, 1883},
        .km_user = {true, "km-user"}, .km_pass = {true, ""},
        .km_inst = {true, "printer"}, .km_topic = {true, "dragonbreath"},
        .km_tls = {true, false}, .km_writeback = {true, true},
    };
    assert(plan(&unchanged, &result, message, sizeof(message)) == ESP_OK);
    assert(!result.source_changed && !result.moonraker_changed &&
           !result.bambu_changed && !result.ha_changed &&
           !result.klipper_mqtt_changed);

    // Blank secrets retain credentials while ordinary changed fields are staged.
    db_portal_product_request_t changed = {
        .mr_host = {true, "new-moonraker.lan"},
        .mr_key = {true, ""},
        .km_port = {true, 8883},
        .km_pass = {true, ""},
        .km_tls = {true, true},
    };
    assert(plan(&changed, &result, message, sizeof(message)) == ESP_OK);
    assert(result.moonraker_changed && result.klipper_mqtt_changed);
    assert(strcmp(result.moonraker.host, "new-moonraker.lan") == 0);
    assert(strcmp(result.moonraker.api_key, "moon-secret") == 0);
    assert(result.klipper_mqtt.port == 8883 && result.klipper_mqtt.tls);
    assert(strcmp(result.klipper_mqtt.pass, "km-secret") == 0);

    // An edit to one inactive-source field must retain every untouched value
    // supplied by that source's persisted config snapshot.
    db_portal_product_request_t partial_bambu = {
        .bb_host = {true, "new-bambu.lan"},
        .bb_code = {true, ""},
    };
    assert(plan(&partial_bambu, &result, message, sizeof(message)) == ESP_OK);
    assert(result.bambu_changed);
    assert(strcmp(result.bambu.host, "new-bambu.lan") == 0);
    assert(strcmp(result.bambu.serial, "01P00SERIAL") == 0);
    assert(strcmp(result.bambu.code, "bambu-secret") == 0);
    assert(!result.moonraker_changed && !result.ha_changed &&
           !result.klipper_mqtt_changed && !result.source_changed);

    // Validation fails in the pure planning phase, before any setter can run.
    char too_long[80];
    memset(too_long, 'x', sizeof(too_long));
    too_long[sizeof(too_long) - 1] = '\0';
    db_portal_product_request_t invalid = {.ha_host = {true, too_long}};
    assert(plan(&invalid, &result, message, sizeof(message)) == ESP_ERR_INVALID_ARG);
    assert(strstr(message, "ha_host") != NULL);

    db_portal_product_request_t invalid_source = {
        .source_present = true,
        .source = DC_SRC_MAX,
    };
    assert(plan(&invalid_source, &result, message, sizeof(message)) == ESP_ERR_INVALID_ARG);
    assert(strstr(message, "ctl_src") != NULL);

    db_portal_product_request_t invalid_port = {.ha_port = {true, 0}};
    assert(plan(&invalid_port, &result, message, sizeof(message)) == ESP_ERR_INVALID_ARG);
    assert(strstr(message, "port") != NULL);

    // Persisted PID can be switched back to the default bang-bang controller while
    // idle; the same mutation is rejected before any setter runs if either heater
    // demand or physical output is active.
    db_portal_product_request_t bang_bang = {
        .control_algorithm_present = true,
        .control_algorithm = PB_HEATER_CONTROL_BANG_BANG,
    };
    assert(plan(&bang_bang, &result, message, sizeof(message)) == ESP_OK);
    assert(result.control_algorithm_changed);
    assert(result.control_algorithm == PB_HEATER_CONTROL_BANG_BANG);
    assert(plan_with_control(&bang_bang, PB_HEATER_CONTROL_PID, true,
                             &result, message, sizeof(message)) == ESP_ERR_INVALID_STATE);
    assert(strstr(message, "Turn the heater off") != NULL);

    db_portal_product_request_t invalid_algorithm = {
        .control_algorithm_present = true,
        .control_algorithm = PB_HEATER_CONTROL__COUNT,
    };
    assert(plan(&invalid_algorithm, &result, message, sizeof(message)) == ESP_ERR_INVALID_ARG);
    assert(strstr(message, "heat_ctl_alg") != NULL);

    puts("db portal config planning tests: PASS");
    return 0;
}
