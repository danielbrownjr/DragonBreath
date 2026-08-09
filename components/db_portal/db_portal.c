// SPDX-License-Identifier: MIT
#include "db_portal.h"

#include "db_klipper_mqtt.h"
#include "dc_bambu.h"
#include "dc_moonraker.h"
#include "dc_portal.h"
#include "dc_source.h"
#include "pb_ha.h"
#include "pb_httpd.h"
#include "pb_policy.h"

#include "cJSON.h"
#include "esp_log.h"
#include "nvs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "db_portal";

extern const unsigned char favicon_png_start[] asm("_binary_favicon_png_start");
extern const unsigned char favicon_png_end[] asm("_binary_favicon_png_end");

static cJSON *field(const char *key, const char *label, const char *type,
                    const char *value, bool secret)
{
    cJSON *f = cJSON_CreateObject();
    if (!f) return NULL;
    cJSON_AddStringToObject(f, "key", key);
    cJSON_AddStringToObject(f, "label", label);
    cJSON_AddStringToObject(f, "type", type);
    cJSON_AddStringToObject(f, "value", secret ? "" : (value ? value : ""));
    if (secret) cJSON_AddBoolToObject(f, "secret", true);
    return f;
}

static cJSON *boolean_field(const char *key, const char *label, bool value)
{
    cJSON *f = field(key, label, "select", value ? "1" : "0", false);
    cJSON *opts = cJSON_AddArrayToObject(f, "options");
    const char *values[] = {"1", "0"};
    const char *labels[] = {"Enabled", "Disabled"};
    for (int i = 0; i < 2; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "value", values[i]);
        cJSON_AddStringToObject(o, "label", labels[i]);
        cJSON_AddItemToArray(opts, o);
    }
    return f;
}

static cJSON *section(cJSON *root, const char *title)
{
    cJSON *sections = cJSON_GetObjectItem(root, "sections");
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "title", title);
    cJSON_AddItemToObject(s, "fields", cJSON_CreateArray());
    cJSON_AddItemToArray(sections, s);
    return s;
}

static void add_field(cJSON *s, cJSON *f)
{
    cJSON_AddItemToArray(cJSON_GetObjectItem(s, "fields"), f);
}

static cJSON *describe_product(void *ctx)
{
    (void)ctx;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "sections", cJSON_CreateArray());

    cJSON *s = section(root, "Control source");
    char selected_source[4];
    snprintf(selected_source, sizeof selected_source, "%d", dc_source_get());
    cJSON *src = field("ctl_src", "Source", "select", selected_source, false);
    cJSON *opts = cJSON_AddArrayToObject(src, "options");
    static const char *labels[] = {"Klipper (Moonraker)", "Bambu LAN", "Home Assistant", "None", "Klipper MQTT"};
    for (int i = 0; i < DC_SRC_MAX; i++) {
        cJSON *o = cJSON_CreateObject();
        char value[4];
        snprintf(value, sizeof value, "%d", i);
        cJSON_AddStringToObject(o, "value", value);
        cJSON_AddStringToObject(o, "label", labels[i]);
        cJSON_AddItemToArray(opts, o);
    }
    add_field(s, src);

    dc_moonraker_config_t mr = {0};
    dc_moonraker_get_config(&mr);
    char port[8]; snprintf(port, sizeof port, "%u", mr.port ? mr.port : 7125);
    s = section(root, "Klipper / Moonraker");
    add_field(s, field("mr_host", "Host", "text", mr.host, false));
    add_field(s, field("mr_port", "Port", "number", port, false));
    add_field(s, field("mr_key", "API key (leave blank to keep)", "password", "", true));

    dc_bambu_config_t bb = {0};
    dc_bambu_get_config(&bb);
    s = section(root, "Bambu LAN");
    add_field(s, field("bb_host", "Printer host", "text", bb.host, false));
    add_field(s, field("bb_serial", "Serial", "text", bb.serial, false));
    add_field(s, field("bb_code", "Access code (leave blank to keep)", "password", "", true));

    pb_ha_config_t ha = {0};
    pb_ha_get_config(&ha);
    snprintf(port, sizeof port, "%u", (unsigned)(ha.port ? ha.port : 1883));
    s = section(root, "Home Assistant MQTT");
    add_field(s, field("ha_host", "Broker host", "text", ha.host, false));
    add_field(s, field("ha_port", "Port", "number", port, false));
    add_field(s, field("ha_user", "Username", "text", ha.user, false));
    add_field(s, field("ha_pass", "Password (leave blank to keep)", "password", "", true));
    add_field(s, field("ha_topic", "Topic prefix", "text", ha.topic, false));

    db_km_config_t km = {0};
    db_klipper_mqtt_get_config(&km);
    snprintf(port, sizeof port, "%u", (unsigned)(km.port ? km.port : (km.tls ? 8883 : 1883)));
    s = section(root, "Klipper MQTT");
    add_field(s, field("km_host", "Broker host", "text", km.host, false));
    add_field(s, field("km_port", "Port", "number", port, false));
    add_field(s, field("km_user", "Username", "text", km.user, false));
    add_field(s, field("km_pass", "Password (leave blank to keep)", "password", "", true));
    add_field(s, field("km_inst", "Moonraker instance", "text", km.inst, false));
    add_field(s, field("km_topic", "Device topic", "text", km.topic, false));
    add_field(s, boolean_field("km_tls", "Use TLS", km.tls));
    add_field(s, boolean_field("km_writeback", "Temperature writeback", km.writeback));
    return root;
}

static const char *string_value(const cJSON *values, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(values, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static bool bool_value(const cJSON *values, const char *key, bool fallback)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(values, key);
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
    if (cJSON_IsString(v)) return !strcmp(v->valuestring, "true") || !strcmp(v->valuestring, "1") || !strcmp(v->valuestring, "on");
    return fallback;
}

static bool port_value(const char *text, uint16_t *port)
{
    if (!text || !*text) return true;
    char *end = NULL;
    long n = strtol(text, &end, 10);
    if (*end || n < 1 || n > 65535) return false;
    *port = (uint16_t)n;
    return true;
}

static bool port_json(const cJSON *values, const char *key, uint16_t *port, bool *present)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(values, key);
    *present = v != NULL;
    if (!v) return true;
    if (cJSON_IsNumber(v)) {
        if (v->valuedouble < 1 || v->valuedouble > 65535 || v->valuedouble != (int)v->valuedouble) return false;
        *port = (uint16_t)v->valueint;
        return true;
    }
    return cJSON_IsString(v) && port_value(v->valuestring, port);
}

#define COPY_IF(dst, value) do { if ((value)) { snprintf((dst), sizeof(dst), "%s", (value)); changed = true; } } while (0)

static esp_err_t apply_product(const cJSON *values, void *ctx, char *message, size_t message_size)
{
    (void)ctx;
    const char *v = string_value(values, "ctl_src");
    if (v) {
        char *end = NULL;
        long src = strtol(v, &end, 10);
        if (!*v || *end || src < 0 || src >= DC_SRC_MAX) goto invalid;
        esp_err_t err = dc_source_set((dc_ctl_source_t)src);
        if (err != ESP_OK) return err;
    }

    dc_moonraker_config_t mr = {0}; dc_moonraker_get_config(&mr);
    bool changed = false;
    COPY_IF(mr.host, string_value(values, "mr_host"));
    bool present = false;
    if (!port_json(values, "mr_port", &mr.port, &present)) goto invalid;
    if (present) changed = true;
    v = string_value(values, "mr_key"); if (v && *v) COPY_IF(mr.api_key, v);
    if (changed && dc_moonraker_set_config(&mr) != ESP_OK) return ESP_FAIL;

    dc_bambu_config_t bb = {0}; dc_bambu_get_config(&bb); changed = false;
    COPY_IF(bb.host, string_value(values, "bb_host"));
    COPY_IF(bb.serial, string_value(values, "bb_serial"));
    v = string_value(values, "bb_code"); if (v && *v) COPY_IF(bb.code, v);
    if (changed && dc_bambu_set_config(&bb) != ESP_OK) return ESP_FAIL;

    pb_ha_config_t ha = {0}; pb_ha_get_config(&ha); changed = false;
    COPY_IF(ha.host, string_value(values, "ha_host"));
    if (!port_json(values, "ha_port", &ha.port, &present)) goto invalid;
    if (present) changed = true;
    COPY_IF(ha.user, string_value(values, "ha_user"));
    v = string_value(values, "ha_pass"); if (v && *v) COPY_IF(ha.pass, v);
    COPY_IF(ha.topic, string_value(values, "ha_topic"));
    if (changed && pb_ha_set_config(&ha) != ESP_OK) return ESP_FAIL;

    db_km_config_t km = {0}; db_klipper_mqtt_get_config(&km); changed = false;
    COPY_IF(km.host, string_value(values, "km_host"));
    if (!port_json(values, "km_port", &km.port, &present)) goto invalid;
    if (present) changed = true;
    COPY_IF(km.user, string_value(values, "km_user"));
    v = string_value(values, "km_pass"); if (v && *v) COPY_IF(km.pass, v);
    COPY_IF(km.inst, string_value(values, "km_inst"));
    COPY_IF(km.topic, string_value(values, "km_topic"));
    if (cJSON_GetObjectItemCaseSensitive(values, "km_tls")) { km.tls = bool_value(values, "km_tls", km.tls); changed = true; }
    if (cJSON_GetObjectItemCaseSensitive(values, "km_writeback")) { km.writeback = bool_value(values, "km_writeback", km.writeback); changed = true; }
    if (changed && db_klipper_mqtt_set_config(&km) != ESP_OK) return ESP_FAIL;

    snprintf(message, message_size, "Configuration saved; restart to apply source changes.");
    return ESP_OK;
invalid:
    snprintf(message, message_size, "Invalid source or port value.");
    return ESP_ERR_INVALID_ARG;
}

static bool authorize(httpd_req_t *req, void *ctx)
{
    (void)ctx;
    return pb_httpd_auth_ok(req);
}

static esp_err_t guard_operation(dc_portal_operation_t operation, void *ctx,
                                 char *message, size_t message_size)
{
    (void)ctx;
    pb_policy_snapshot_t snap;
    pb_policy_get_snapshot(&snap);
    if (snap.mode == PB_MODE_OFF && !snap.heater_output) return ESP_OK;
    snprintf(message, message_size, "Turn the heater off before %s.",
             operation == DC_PORTAL_OPERATION_OTA ? "updating" : "a factory reset");
    return ESP_ERR_INVALID_STATE;
}

static esp_err_t validate_image(const esp_app_desc_t *image, void *ctx,
                                char *message, size_t message_size)
{
    (void)ctx;
    if (!strcmp(image->project_name, "dragonbreath") || !strcmp(image->project_name, "panda_breath"))
        return ESP_OK;
    snprintf(message, message_size, "Not a DragonBreath or stock Panda Breath image.");
    return ESP_ERR_INVALID_ARG;
}

static esp_err_t factory_reset(void *ctx)
{
    (void)ctx;
    nvs_handle_t h;
    esp_err_t err = nvs_open("app_nvs", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_erase_all(h);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t favicon_get(httpd_req_t *req)
{
    // These linker labels bound one target_add_binary_data blob.
    // cppcheck-suppress comparePointers
    const size_t length = (size_t)(favicon_png_end - favicon_png_start);
    httpd_resp_set_type(req, "image/png");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
    return httpd_resp_send(req, (const char *)favicon_png_start, length);
}

static esp_err_t register_product_routes(httpd_handle_t server, void *ctx)
{
    (void)ctx;
    esp_err_t err = pb_httpd_register(server);
    if (err != ESP_OK) return err;
    const httpd_uri_t favicon = { .uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_get };
    return httpd_register_uri_handler(server, &favicon);
}

esp_err_t db_portal_start(void)
{
    httpd_config_t httpd = HTTPD_DEFAULT_CONFIG();
    httpd.lru_purge_enable = true;
    httpd.max_uri_handlers = 48;
    httpd.stack_size = 8192;
    httpd.keep_alive_enable = true;
    httpd.keep_alive_idle = 10;
    httpd.keep_alive_interval = 5;
    httpd.keep_alive_count = 3;

    const dc_portal_config_t cfg = {
        .product = "dragonbreath",
        .display_name = "DragonBreath",
        .register_product_routes = register_product_routes,
        .describe_product = describe_product,
        .apply_product = apply_product,
        .authorize = authorize,
        .guard_operation = guard_operation,
        .validate_image = validate_image,
        .factory_reset = factory_reset,
        .httpd_config = &httpd,
    };
    esp_err_t err = dc_portal_start(&cfg);
    if (err == ESP_OK) ESP_LOGI(TAG, "shared Dragon portal started");
    return err;
}
