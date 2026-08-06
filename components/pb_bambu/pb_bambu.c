// Bambu Lab LAN MQTT client. Reads the printer's live bed temperature over its
// on-device MQTT-over-TLS broker so AUTO can follow a Bambu print, mirroring the
// Moonraker path. Read-only — no control commands are ever sent to the printer.
//
// UNTESTED against real hardware (the maintainer has no Bambu printer); built from
// the OpenBambuAPI / ha-bambulab protocol spec for community validation. Protocol:
//   mqtts://<host>:8883, user "bblp", pass = LAN access code, self-signed cert
//   (CN=serial, connect by IP -> cert verification relaxed). Subscribe
//   device/<serial>/report; publish one "pushall" on connect (P1/A1 send deltas).
// See plans/control-source-bambu-ha.md.
#include "pb_bambu.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mqtt_client.h"
#include "nvs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   // strncasecmp / strcasecmp for filament zone matching

static const char *TAG = "pb_bambu";

#define NVS_NS   "app_nvs"
#define KEY_HOST "bb_host"
#define KEY_SER  "bb_serial"
#define KEY_CODE "bb_code"

// A full "pushall" report is ~10-15 KB. esp-mqtt fragments payloads larger than
// its RX buffer; we reassemble up to RX_CAP (bed_temper is near the front of the
// print object, so a truncated tail still yields the follow signal).
#define MQTT_BUF   8192
#define RX_CAP     16384

// One pushall on connect; required on P1/A1 (delta-only), harmless on X1.
static const char PUSHALL[] =
    "{\"pushing\":{\"sequence_id\":\"0\",\"command\":\"pushall\",\"version\":1,\"push_target\":1}}";

static SemaphoreHandle_t        s_lock  = NULL;
static pb_bambu_config_t        s_cfg   = {0};
static pb_bambu_status_t        s_status = {
    .state = PB_BAMBU_DISABLED, .bed_temp = NAN, .bed_target = NAN, .chamber_temp = NAN,
};
static esp_mqtt_client_handle_t s_client = NULL;

static char   s_report_topic[80]  = {0};   // device/<serial>/report
static char   s_request_topic[80] = {0};   // device/<serial>/request
static char  *s_rx = NULL;                 // RX_CAP reassembly buffer
static size_t s_rx_len = 0;
static bool   s_in_report = false;         // current inbound msg is on the report topic

// ---------- NVS ----------

static esp_err_t nvs_load(pb_bambu_config_t *out)
{
    memset(out, 0, sizeof(*out));
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    size_t sz = sizeof(out->host);
    err = nvs_get_str(h, KEY_HOST, out->host, &sz);
    if (err != ESP_OK) { nvs_close(h); return err; }   // no host = unconfigured

    sz = sizeof(out->serial);
    nvs_get_str(h, KEY_SER, out->serial, &sz);
    sz = sizeof(out->code);
    nvs_get_str(h, KEY_CODE, out->code, &sz);
    nvs_close(h);
    return ESP_OK;
}

static esp_err_t nvs_save(const pb_bambu_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, KEY_HOST, cfg->host);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_SER, cfg->serial);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_CODE, cfg->code);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// ---------- report parsing ----------

// Pull the float value of a JSON key via a targeted scan (no full JSON parse — the
// C3 can't afford a parse tree over a 15 KB payload). `key` includes the quotes,
// e.g. "\"bed_temper\"".
static bool find_float(const char *s, const char *key, float *out)
{
    const char *q = strstr(s, key);
    if (!q) return false;
    q = strchr(q, ':');
    if (!q) return false;
    char *end;
    float v = strtof(q + 1, &end);
    if (end == q + 1) return false;   // no number parsed
    *out = v;
    return true;
}

// Copy the string value of a JSON key ("key":"value") into out. `key` includes the
// quotes. Returns true if a (possibly empty) string value was found.
static bool find_string(const char *s, const char *key, char *out, size_t outsz)
{
    const char *q = strstr(s, key);
    if (!q) return false;
    q = strchr(q, ':');
    if (!q) return false;
    q++;
    while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
    if (*q != '"') return false;
    q++;
    size_t i = 0;
    while (*q && *q != '"' && i + 1 < outsz) out[i++] = *q++;
    out[i] = '\0';
    return true;
}

// Resolve the ACTIVE filament type from a report. Bambu lists loaded filaments in
// print.ams (AMS units, tray[].tray_type) and print.vt_tray (external spool), with
// print.ams.tray_now selecting the active global tray index (254 = external spool,
// 255/absent = none loaded). Targeted scan — no full JSON parse over ~15 KB.
static bool active_filament(const char *json, char *out, size_t outsz)
{
    out[0] = '\0';
    char trayn[8] = {0};
    long now = find_string(json, "\"tray_now\"", trayn, sizeof trayn) ? strtol(trayn, NULL, 10) : -1;
    const char *vt = strstr(json, "\"vt_tray\"");
    char tt[16];

    // AMS slot active: take the (now)th tray_type inside the "ams" object.
    if (now >= 0 && now < 250) {
        const char *ams = strstr(json, "\"ams\"");
        if (ams) {
            const char *p = ams; long idx = -1;
            while ((p = strstr(p, "\"tray_type\"")) != NULL) {
                if (++idx == now) {
                    if (find_string(p, "\"tray_type\"", tt, sizeof tt) && tt[0]) {
                        snprintf(out, outsz, "%s", tt); return true;
                    }
                    break;
                }
                p += 11;
            }
        }
    }
    // External spool (tray_now 254, or as a fallback): vt_tray.tray_type.
    if (vt && find_string(vt, "\"tray_type\"", tt, sizeof tt) && tt[0]) {
        snprintf(out, outsz, "%s", tt); return true;
    }
    return false;
}

static void parse_report(const char *json)
{
    float bed, bedtgt, cham;
    bool got_bed  = find_float(json, "\"bed_temper\"", &bed);
    // bed_target_temper is the commanded setpoint AUTO triggers on. Match the
    // "_target_temper" so it can't be confused with "bed_temper" (strstr finds the
    // first hit; searching the more specific key avoids the prefix collision).
    bool got_tgt  = find_float(json, "bed_target_temper", &bedtgt);
    bool got_cham = find_float(json, "\"chamber_temper\"", &cham);
    // NOTE(phase 2b): H2/newer moved chamber temp to a packed device.ctc.info.temp
    // field; only the legacy flat chamber_temper is read here. Bed follow (the
    // goal) works on all models via bed_temper/bed_target_temper.
    char fila[16];
    bool got_fila = active_filament(json, fila, sizeof fila);   // active AMS/spool filament

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (got_bed) {
        s_status.bed_temp  = bed;
        s_status.state     = PB_BAMBU_SUBSCRIBED;   // we have live data now
        s_status.connected = true;
    }
    if (got_tgt)  s_status.bed_target = bedtgt;
    if (got_cham) s_status.chamber_temp = cham;
    bool fila_changed = got_fila && strcmp(fila, s_status.filament) != 0;
    if (fila_changed) snprintf(s_status.filament, sizeof s_status.filament, "%s", fila);
    xSemaphoreGive(s_lock);

    if (fila_changed) ESP_LOGI(TAG, "active filament: %s", fila);
    if (got_bed) ESP_LOGD(TAG, "bed=%.1f chamber=%.1f", bed, got_cham ? cham : NAN);
}

static bool topic_is_report(const char *topic, int len)
{
    return len > 0 && (size_t)len == strlen(s_report_topic)
        && strncmp(topic, s_report_topic, (size_t)len) == 0;
}

// ---------- mqtt events ----------

static void mqtt_event_handler(void *args, esp_event_base_t base, int32_t id, void *data)
{
    (void)args; (void)base;
    esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected; subscribing %s", s_report_topic);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.state = PB_BAMBU_CONNECTED;   // not SUBSCRIBED until first report
        xSemaphoreGive(s_lock);
        esp_mqtt_client_subscribe(s_client, s_report_topic, 0);
        esp_mqtt_client_publish(s_client, s_request_topic, PUSHALL, 0, 0, 0);
        break;

    case MQTT_EVENT_DISCONNECTED:
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.state     = PB_BAMBU_DISCONNECTED;
        s_status.connected = false;
        xSemaphoreGive(s_lock);
        break;

    case MQTT_EVENT_DATA: {
        // Reassemble a possibly-fragmented payload. The topic is present only on
        // the first fragment (offset 0); track whether this message is the report.
        if (e->current_data_offset == 0) {
            s_in_report = topic_is_report(e->topic, e->topic_len);
            s_rx_len = 0;
        }
        if (!s_in_report || s_rx == NULL) break;
        size_t off = (size_t)e->current_data_offset;
        if (off < RX_CAP - 1) {
            size_t copy = (size_t)e->data_len;
            if (off + copy > RX_CAP - 1) copy = (RX_CAP - 1) - off;
            memcpy(s_rx + off, e->data, copy);
            s_rx_len = off + copy;
        }
        if (e->current_data_offset + e->data_len >= e->total_data_len) {
            s_rx[s_rx_len] = '\0';
            parse_report(s_rx);
        }
        break;
    }

    default:
        break;
    }
}

// ---------- lifecycle ----------

esp_err_t pb_bambu_start(void)
{
    if (s_lock != NULL) return ESP_ERR_INVALID_STATE;
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;

    if (nvs_load(&s_cfg) != ESP_OK || s_cfg.host[0] == '\0') {
        ESP_LOGI(TAG, "no Bambu config saved; idle");
        s_status.state = PB_BAMBU_DISABLED;
        return ESP_OK;
    }
    if (s_cfg.serial[0] == '\0' || s_cfg.code[0] == '\0') {
        ESP_LOGW(TAG, "Bambu needs host + serial + access code; idle");
        s_status.state = PB_BAMBU_DISABLED;
        return ESP_OK;
    }

    s_rx = malloc(RX_CAP);
    if (s_rx == NULL) return ESP_ERR_NO_MEM;
    snprintf(s_report_topic,  sizeof s_report_topic,  "device/%s/report",  s_cfg.serial);
    snprintf(s_request_topic, sizeof s_request_topic, "device/%s/request", s_cfg.serial);

    char uri[96];
    snprintf(uri, sizeof uri, "mqtts://%s:8883", s_cfg.host);
    esp_mqtt_client_config_t mc = {
        .broker.address.uri = uri,
        // Self-signed per-device cert (CN=serial) reached by IP: no CA to verify
        // against, so a read-only LAN client connects WITHOUT server-cert
        // verification. With no CA/bundle set, esp-tls falls through to VERIFY_NONE
        // only because CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY is enabled (see
        // sdkconfig.defaults) — otherwise it errors "No server verification option
        // set" and the connect fails. skip the CN check too since IP != serial.
        .broker.verification.skip_cert_common_name_check = true,
        .broker.verification.use_global_ca_store = false,
        .credentials.username = "bblp",
        .credentials.authentication.password = s_cfg.code,
        .buffer.size = MQTT_BUF,
    };

    s_client = esp_mqtt_client_init(&mc);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed");
        free(s_rx); s_rx = NULL;
        s_status.state = PB_BAMBU_DISCONNECTED;
        return ESP_FAIL;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_err_t err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_mqtt_client_start: %s", esp_err_to_name(err));
        s_status.state = PB_BAMBU_DISCONNECTED;
        return err;
    }
    s_status.state = PB_BAMBU_CONNECTING;
    ESP_LOGI(TAG, "connecting to %s (serial %s)", uri, s_cfg.serial);
    return ESP_OK;
}

esp_err_t pb_bambu_set_config(const pb_bambu_config_t *cfg)
{
    if (cfg == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t err = nvs_save(cfg);
    if (err != ESP_OK) return err;
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_cfg = *cfg;
        xSemaphoreGive(s_lock);
    }
    return ESP_OK;   // takes effect on next boot (matches pb_moonraker semantics)
}

esp_err_t pb_bambu_get_config(pb_bambu_config_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cfg;
    if (s_lock) xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t pb_bambu_get_status(pb_bambu_status_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    if (s_lock) xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t pb_bambu_clear_config(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_erase_key(h, KEY_HOST);
    nvs_erase_key(h, KEY_SER);
    nvs_erase_key(h, KEY_CODE);
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

// ---------- filament chamber zones (issue #64, Bambu only) ----------

// Base filament type -> default chamber target (°C). 0 = no zone (off). Opinionated
// but sane; all user-overridable via NVS. PLA/TPU default off (a hot chamber hurts
// PLA); PETG/ABS/ASA/PC want warmth for layer adhesion / reduced warping.
static const struct { const char *name; const char *key; uint8_t def_c; }
ZONES[PB_BAMBU_ZONE_COUNT] = {
    { "PLA",  "zone_pla",  0  },
    { "PETG", "zone_petg", 40 },
    { "ABS",  "zone_abs",  45 },
    { "ASA",  "zone_asa",  45 },
    { "PC",   "zone_pc",   50 },
    { "TPU",  "zone_tpu",  0  },
};

static uint8_t s_zone_c[PB_BAMBU_ZONE_COUNT];   // RAM cache of the resolved targets
static bool    s_zones_loaded = false;

// Load the zone targets (NVS override, else default) into the RAM cache.
static void zones_load(void)
{
    nvs_handle_t h;
    bool have = (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK);
    for (int i = 0; i < PB_BAMBU_ZONE_COUNT; i++) {
        uint8_t v = ZONES[i].def_c;
        if (have) nvs_get_u8(h, ZONES[i].key, &v);   // leaves default if key absent
        s_zone_c[i] = v;
    }
    if (have) nvs_close(h);
    s_zones_loaded = true;
}

uint8_t pb_bambu_zone_target(const char *filament)
{
    if (!filament || !filament[0]) return 0;
    if (!s_zones_loaded) zones_load();
    // Case-insensitive prefix match so "PETG-CF" / "PLA Basic" resolve to their base.
    for (int i = 0; i < PB_BAMBU_ZONE_COUNT; i++)
        if (strncasecmp(filament, ZONES[i].name, strlen(ZONES[i].name)) == 0)
            return s_zone_c[i];
    return 0;
}

int pb_bambu_zone_get_all(pb_bambu_zone_t *out, int max)
{
    if (!s_zones_loaded) zones_load();
    int n = 0;
    for (int i = 0; i < PB_BAMBU_ZONE_COUNT && n < max; i++, n++) {
        snprintf(out[n].name, sizeof out[n].name, "%s", ZONES[i].name);
        out[n].target_c = s_zone_c[i];
    }
    return n;
}

esp_err_t pb_bambu_zone_set(const char *name, uint8_t target_c)
{
    if (!name) return ESP_ERR_INVALID_ARG;
    int idx = -1;
    for (int i = 0; i < PB_BAMBU_ZONE_COUNT; i++)
        if (strcasecmp(name, ZONES[i].name) == 0) { idx = i; break; }
    if (idx < 0) return ESP_ERR_NOT_FOUND;
    if (target_c > 70) target_c = 70;   // settable ceiling; pb_policy enforces hard cutoffs
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, ZONES[idx].key, target_c);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) { if (!s_zones_loaded) zones_load(); s_zone_c[idx] = target_c; }
    return err;
}
