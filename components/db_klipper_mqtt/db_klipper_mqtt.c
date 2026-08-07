// Klipper-over-MQTT control source. Subscribes to the Moonraker MQTT split-status
// for the DRAGONBREATH (desired-state) and DB_LINK (heartbeat) macros, runs the
// retained-aware arming machine (db_klipper_mqtt_arm.h), and holds a pb_policy lease
// while armed — exactly like pb_ha, but commanded by Klipper macros over the printer's
// own Moonraker broker. Publishes device telemetry for a Moonraker [sensor] and an
// on/off state for a Moonraker [power] device. See
// plans/mqtt-klipper-implementation-design.md.
//
// Threading: the esp-mqtt event handler (mqtt task) updates arm/connection state under
// s_lock; db_klipper_mqtt_tick() (control task) evaluates the machine, calls pb_policy,
// and publishes. esp_mqtt_client_publish() is thread-safe; shared state is guarded.
#include "db_klipper_mqtt.h"
#include "db_klipper_mqtt_arm.h"
#include "pb_policy.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mqtt_client.h"
#include "nvs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "db_km";

#define NVS_NS    "app_nvs"
#define KEY_HOST  "km_host"
#define KEY_PORT  "km_port"
#define KEY_USER  "km_user"
#define KEY_PASS  "km_pass"
#define KEY_INST  "km_inst"
#define KEY_TOPIC "km_topic"
#define KEY_TLS   "km_tls"
#define KEY_WB    "km_wb"

#define DEFAULT_PORT      1883
#define DEFAULT_TLS_PORT  8883
#define DEFAULT_PREFIX    "dragonbreath"
#define STATE_PERIOD_US   (2 * 1000 * 1000)    // telemetry + power state ~2 s
#define HB_PERIOD_US      (5 * 1000 * 1000)    // lease heartbeat ~5 s (<< watchdog min)
#define LIVE_TIMEOUT_US   ((int64_t)15 * 1000 * 1000)  // 3 x 5 s missed heartbeats
#define TARGET_MIN_C      20.0f
#define TARGET_MAX_C      70.0f

static SemaphoreHandle_t        s_lock   = NULL;
static db_km_config_t           s_cfg    = {0};
static db_km_status_t           s_status = { .conn = DB_KM_DISABLED };
static esp_mqtt_client_handle_t s_client = NULL;

// Shared control state (guarded by s_lock).
static db_km_arm_t        s_arm         = {0};
static bool               s_have_lease  = false;
static pb_policy_lease_t  s_lease       = {0};
static bool               s_power_on    = true;   // Moonraker [power] master enable
static bool               s_mrk_online  = true;   // Moonraker availability

// Pacing / writeback (control task only).
static int64_t  s_last_state_us = 0;
static int64_t  s_last_hb_us    = 0;
static uint32_t s_rpc_id        = 0;
static bool     s_last_power_pub = true;

// Topics built once at start (must outlive esp_mqtt_client_init for the LWT).
static char s_avail_topic[80]  = {0};   // <base>/status  (device availability, LWT)

static const char *base(void) { return s_cfg.topic[0] ? s_cfg.topic : DEFAULT_PREFIX; }

// ---------- NVS ----------

static esp_err_t nvs_load(db_km_config_t *out)
{
    memset(out, 0, sizeof(*out));
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    size_t sz = sizeof(out->host);
    err = nvs_get_str(h, KEY_HOST, out->host, &sz);
    if (err != ESP_OK) { nvs_close(h); return err; }   // no host = unconfigured

    uint16_t p = 0;
    uint8_t tls = 0, wb = 0;
    nvs_get_u8(h, KEY_TLS, &tls);
    out->tls = tls != 0;
    if (nvs_get_u16(h, KEY_PORT, &p) == ESP_OK && p > 0) out->port = p;
    else out->port = out->tls ? DEFAULT_TLS_PORT : DEFAULT_PORT;
    nvs_get_u8(h, KEY_WB, &wb);
    out->writeback = wb != 0;

    sz = sizeof(out->user);  nvs_get_str(h, KEY_USER, out->user, &sz);
    sz = sizeof(out->pass);  nvs_get_str(h, KEY_PASS, out->pass, &sz);
    sz = sizeof(out->inst);  nvs_get_str(h, KEY_INST, out->inst, &sz);
    sz = sizeof(out->topic); nvs_get_str(h, KEY_TOPIC, out->topic, &sz);
    nvs_close(h);
    return ESP_OK;
}

static esp_err_t nvs_save(const db_km_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, KEY_HOST, cfg->host);
    if (err == ESP_OK) err = nvs_set_u16(h, KEY_PORT, cfg->port);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_USER, cfg->user);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_PASS, cfg->pass);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_INST, cfg->inst);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_TOPIC, cfg->topic);
    if (err == ESP_OK) err = nvs_set_u8(h, KEY_TLS, cfg->tls ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(h, KEY_WB, cfg->writeback ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// ---------- policy mapping (mirrors pb_ha) ----------

static void start_heat(float target_c)
{
    if (target_c < TARGET_MIN_C) target_c = TARGET_MIN_C;
    if (target_c > TARGET_MAX_C) target_c = TARGET_MAX_C;
    pb_policy_lease_t lease = {0};
    pb_policy_result_t r = pb_policy_set_power_on(
        target_c, DB_SOURCE_WEB, "klipper-mqtt", PB_POLICY_REVISION_ANY, &lease);
    if (r == PB_POLICY_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_lease = lease;
        s_have_lease = true;
        s_last_hb_us = esp_timer_get_time();
        s_status.engaged = true;
        s_status.comms_lost = false;
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "klipper-mqtt -> heat %.0f C", (double)target_c);
    } else {
        ESP_LOGW(TAG, "heat rejected (policy result %d)", (int)r);
    }
}

static void stop_heat(bool comms_lost)
{
    pb_policy_set_mode_off(DB_SOURCE_WEB);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_have_lease = false;
    s_status.engaged = false;
    if (comms_lost) s_status.comms_lost = true;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "klipper-mqtt -> off%s", comms_lost ? " (comms lost)" : "");
}

// Force the arming machine back to disarmed (master-power off / Moonraker offline).
static void force_disarm(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_arm.engaged = false;
    xSemaphoreGive(s_lock);
    stop_heat(false);
}

// ---------- inbound classification ----------

static void handle_field(const char *topic, const char *payload)
{
    db_km_field_t f = db_km_field_of(topic);
    if (f == DB_KM_F_NONE) return;
    char val[24];
    if (!db_km_value(payload, val, sizeof val)) return;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    switch (f) {
    case DB_KM_F_SEQ:       db_km_arm_seq(&s_arm, strtol(val, NULL, 10)); break;
    case DB_KM_F_TARGET:    db_km_arm_target(&s_arm, strtof(val, NULL)); break;
    case DB_KM_F_MODE:      db_km_arm_mode(&s_arm, val); break;
    case DB_KM_F_FAN:       db_km_arm_fan(&s_arm, (int)strtol(val, NULL, 10)); break;
    case DB_KM_F_ARMED:     db_km_arm_armed(&s_arm, (strcmp(val, "true") == 0) ? 1 : (int)strtol(val, NULL, 10)); break;
    case DB_KM_F_PURGE:     (void)db_km_arm_purge(&s_arm, strtol(val, NULL, 10)); break;
    case DB_KM_F_HEARTBEAT: db_km_arm_hb(&s_arm, strtol(val, NULL, 10), esp_timer_get_time()); break;
    default: break;
    }
    xSemaphoreGive(s_lock);
}

static void handle_data(const char *topic, int tlen, const char *data, int dlen)
{
    char t[128], d[64];
    int n = tlen < (int)sizeof(t) - 1 ? tlen : (int)sizeof(t) - 1;
    memcpy(t, topic, n); t[n] = '\0';
    n = dlen < (int)sizeof(d) - 1 ? dlen : (int)sizeof(d) - 1;
    memcpy(d, data, n); d[n] = '\0';

    // Moonraker availability: {"server":"online"|"offline"}
    if (strstr(t, "/moonraker/status")) {
        if (strstr(d, "offline")) { s_mrk_online = false; force_disarm(); }
        else if (strstr(d, "online")) s_mrk_online = true;
        return;
    }
    // Master power command from Moonraker [power]: payload "on"/"off".
    char pset[80];
    snprintf(pset, sizeof pset, "%s/power/set", base());
    if (strcmp(t, pset) == 0) {
        bool on = (strstr(d, "on") != NULL) && (strstr(d, "off") == NULL);
        s_power_on = on;
        if (!on) force_disarm();
        s_last_state_us = 0;   // force a prompt power/state echo
        return;
    }
    // Desired-state / heartbeat split-status.
    handle_field(t, d);
}

// ---------- publish ----------

static void publish_power_state(bool on)
{
    char topic[80];
    snprintf(topic, sizeof topic, "%s/power/state", base());
    esp_mqtt_client_publish(s_client, topic, on ? "on" : "off", 0, 0, 1);  // qos0, retain
    s_last_power_pub = on;
}

static void publish_telemetry(void)
{
    pb_policy_snapshot_t snap;
    pb_policy_get_snapshot(&snap);

    char cb[16], pb[16];
    if (isfinite(snap.chamber_c)) snprintf(cb, sizeof cb, "%.1f", snap.chamber_c); else strcpy(cb, "null");
    if (isfinite(snap.ptc_c))     snprintf(pb, sizeof pb, "%.1f", snap.ptc_c);     else strcpy(pb, "null");
    const char *mode = (snap.mode == PB_MODE_OFF) ? "off" : "heat";

    long seq_ack; bool armed; bool comms;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    seq_ack = s_arm.acted_seq;
    armed   = s_status.engaged;
    comms   = s_status.comms_lost;
    xSemaphoreGive(s_lock);

    char buf[200];
    snprintf(buf, sizeof buf,
        "{\"v\":1,\"chamber_temperature\":%s,\"element_temperature\":%s,"
        "\"mode\":\"%s\",\"target\":%.0f,\"armed\":%s,\"seq_ack\":%ld,\"fault\":\"%s\"}",
        cb, pb, mode, (double)snap.effective_target_c,
        armed ? "true" : "false", seq_ack, comms ? "comms_lost" : "");

    char topic[80];
    snprintf(topic, sizeof topic, "%s/telemetry", base());
    esp_mqtt_client_publish(s_client, topic, buf, 0, 0, 0);   // qos0, non-retained

    // Optional device->Klipper writeback of live chamber temp into a macro variable,
    // so macro logic can read it (Moonraker [sensor] values are invisible to macros).
    // QoS 0 (no mqtt_timestamp needed); rate = telemetry cadence (~2 s).
    if (s_cfg.writeback && isfinite(snap.chamber_c)) {
        char rpc[220], areq[96];
        snprintf(rpc, sizeof rpc,
            "{\"jsonrpc\":\"2.0\",\"method\":\"printer.gcode.script\",\"id\":%u,"
            "\"params\":{\"script\":\"SET_GCODE_VARIABLE MACRO=DRAGONBREATH "
            "VARIABLE=temperature VALUE=%.1f\"}}",
            (unsigned)(++s_rpc_id), snap.chamber_c);
        snprintf(areq, sizeof areq, "%s/moonraker/api/request", s_cfg.inst);
        esp_mqtt_client_publish(s_client, areq, rpc, 0, 0, 0);
    }
}

// ---------- mqtt events ----------

static void subscribe_all(void)
{
    char sub[128];
    snprintf(sub, sizeof sub, "%s/klipper/state/gcode_macro DRAGONBREATH/#", s_cfg.inst);
    esp_mqtt_client_subscribe(s_client, sub, 0);
    snprintf(sub, sizeof sub, "%s/klipper/state/gcode_macro DB_LINK/#", s_cfg.inst);
    esp_mqtt_client_subscribe(s_client, sub, 0);
    snprintf(sub, sizeof sub, "%s/moonraker/status", s_cfg.inst);
    esp_mqtt_client_subscribe(s_client, sub, 0);
    snprintf(sub, sizeof sub, "%s/power/set", base());
    esp_mqtt_client_subscribe(s_client, sub, 0);
}

static void mqtt_event_handler(void *args, esp_event_base_t evbase, int32_t id, void *data)
{
    (void)args; (void)evbase;
    esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected to broker");
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.conn = DB_KM_CONNECTED;
        s_status.connected = true;
        db_km_arm_reset(&s_arm);      // retained state must NOT arm — start disarmed
        s_mrk_online = true;
        xSemaphoreGive(s_lock);
        esp_mqtt_client_publish(s_client, s_avail_topic, "online", 0, 1, 1);
        publish_power_state(s_power_on);
        subscribe_all();
        s_last_state_us = 0;          // prompt first telemetry
        break;
    case MQTT_EVENT_DISCONNECTED:
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.conn = DB_KM_DISCONNECTED;
        s_status.connected = false;
        xSemaphoreGive(s_lock);
        force_disarm();               // no broker -> can't heartbeat -> fail safe off
        break;
    case MQTT_EVENT_DATA:
        if (e->topic_len > 0 && e->data_len >= 0)
            handle_data(e->topic, e->topic_len, e->data, e->data_len);
        break;
    default:
        break;
    }
}

// ---------- lifecycle ----------

esp_err_t db_klipper_mqtt_start(void)
{
    if (s_lock != NULL) return ESP_ERR_INVALID_STATE;
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;

    if (nvs_load(&s_cfg) != ESP_OK || s_cfg.host[0] == '\0') {
        ESP_LOGI(TAG, "no Klipper-MQTT config saved; idle");
        s_status.conn = DB_KM_DISABLED;
        return ESP_OK;
    }

    snprintf(s_avail_topic, sizeof s_avail_topic, "%s/status", base());

    char uri[128];
    snprintf(uri, sizeof uri, "%s://%s:%u", s_cfg.tls ? "mqtts" : "mqtt",
             s_cfg.host, (unsigned)s_cfg.port);
    esp_mqtt_client_config_t mc = {
        .broker.address.uri = uri,
        .session.last_will = {
            .topic = s_avail_topic, .msg = "offline", .msg_len = 7, .qos = 1, .retain = 1,
        },
    };
    if (s_cfg.tls) mc.broker.verification.skip_cert_common_name_check = true;
    if (s_cfg.user[0]) mc.credentials.username = s_cfg.user;
    if (s_cfg.pass[0]) mc.credentials.authentication.password = s_cfg.pass;

    s_client = esp_mqtt_client_init(&mc);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed");
        s_status.conn = DB_KM_DISCONNECTED;
        return ESP_FAIL;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_err_t err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_mqtt_client_start: %s", esp_err_to_name(err));
        s_status.conn = DB_KM_DISCONNECTED;
        return err;
    }
    s_status.conn = DB_KM_CONNECTING;
    ESP_LOGI(TAG, "connecting to %s (instance '%s', base '%s')", uri, s_cfg.inst, base());
    return ESP_OK;
}

void db_klipper_mqtt_tick(void)
{
    if (s_client == NULL) return;
    bool connected;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    connected = s_status.connected;
    xSemaphoreGive(s_lock);
    if (!connected) return;

    int64_t now = esp_timer_get_time();

    // Evaluate the arming machine under lock (it mutates arm state).
    float target = 0.0f;
    db_km_action_t action;
    bool allow, hb_due = false;
    pb_policy_lease_t lease = {0};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    allow = s_power_on && s_mrk_online;
    action = db_km_arm_eval(&s_arm, now, LIVE_TIMEOUT_US, &target);
    if (action == DB_KM_ENGAGE && !allow) {   // master-off / Moonraker-offline gate
        s_arm.engaged = false;
        action = DB_KM_DISENGAGE;
    }
    if (action == DB_KM_HOLD && s_arm.engaged && s_have_lease && now - s_last_hb_us >= HB_PERIOD_US) {
        lease = s_lease; s_last_hb_us = now; hb_due = true;
    }
    xSemaphoreGive(s_lock);

    switch (action) {
    case DB_KM_ENGAGE:    start_heat(target); break;
    case DB_KM_DISENGAGE: stop_heat(false);   break;
    case DB_KM_COMMS_LOST: stop_heat(true);   break;
    case DB_KM_HOLD:
        if (hb_due && pb_policy_heartbeat(&lease) != PB_POLICY_OK) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_have_lease = false;
            xSemaphoreGive(s_lock);
        }
        break;
    }

    if (now - s_last_state_us >= STATE_PERIOD_US) {
        s_last_state_us = now;
        if (s_power_on != s_last_power_pub) publish_power_state(s_power_on);
        publish_telemetry();
    }
}

esp_err_t db_klipper_mqtt_set_config(const db_km_config_t *cfg)
{
    if (cfg == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t err = nvs_save(cfg);
    if (err != ESP_OK) return err;
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_cfg = *cfg;
        xSemaphoreGive(s_lock);
    }
    return ESP_OK;   // takes effect on next boot (matches pb_ha / pb_moonraker)
}

esp_err_t db_klipper_mqtt_get_config(db_km_config_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cfg;
    if (s_lock) xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t db_klipper_mqtt_get_status(db_km_status_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    if (s_lock) xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t db_klipper_mqtt_clear_config(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_erase_key(h, KEY_HOST); nvs_erase_key(h, KEY_PORT); nvs_erase_key(h, KEY_USER);
    nvs_erase_key(h, KEY_PASS); nvs_erase_key(h, KEY_INST); nvs_erase_key(h, KEY_TOPIC);
    nvs_erase_key(h, KEY_TLS);  nvs_erase_key(h, KEY_WB);
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}
