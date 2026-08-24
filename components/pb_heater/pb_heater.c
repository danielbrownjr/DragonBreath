// SPDX-License-Identifier: MIT
#include "pb_heater.h"
#include "pb_pid.h"
#include "pb_board.h"
#include "pb_ntc.h"

#include <math.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

static const char *TAG = "pb_heater";

// Conservative first-pass gains for the very slow printer-chamber plant. These
// intentionally start gentle; hardware logs can tune them later without weakening
// any of the independent local safety layers below.
#define PB_CHAMBER_PID_KP       0.0400f
#define PB_CHAMBER_PID_KI       0.0008f
#define PB_CHAMBER_PID_KD       0.0200f
#define PB_CHAMBER_PID_D_ALPHA  0.20f
#define PB_CHAMBER_PID_DT_S     0.50f
#define PB_CHAMBER_PID_WINDOW_US 10000000LL   // 10 s slow time-proportioning window

// Serializes fault-latch NVS writes (do_latch persist, clear_fault, tick retry)
// so a concurrent clear and a deferred-persist retry can't reorder into a stale
// write that resurrects a just-cleared latch. NULL until pb_heater_init().
static SemaphoreHandle_t s_persist_lock;

// Heater state is touched by BOTH the control task (pb_heater_tick, via the
// 2 Hz loop) and the HTTP task (set_target / notify_link_alive / clear_fault /
// getters). All of it is serialized under this spinlock — in particular the
// 64-bit s_last_link_us would tear on the 32-bit RISC-V core otherwise.
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static float       s_target_c;      // guarded by s_mux
static float       s_control_chamber_c; // guarded by s_mux; optional external regulation temp
static bool        s_latched_off;   // guarded by s_mux (set by a safety trip)
static bool        s_inhibited;     // guarded by s_mux (PERMANENT; reboot-only)
static int64_t     s_last_link_us;  // guarded by s_mux
static const char *s_fault_reason;  // guarded by s_mux (points at a string literal)
static pb_fault_reason_t s_fault_code; // guarded by s_mux (machine-readable, persisted)
static bool        s_persist_pending;  // guarded by s_mux — a latch persist not yet committed
                                       // to NVS; retried from pb_heater_tick until it lands
static int64_t     s_persist_retry_us; // guarded by s_mux — last persist-retry timestamp
static bool        s_on;            // written only by the control task; atomic read
static bool        s_heat_intent;   // control-task only — local-sensor bang-bang latch
static bool        s_fb_cut;        // control-task only — element-foldback hysteresis latch:
                                    // true while the SSR is force-cut for element over-temp
                                    // (holds until the element cools below the resume point)
static bool        s_local_cut;     // control-task only — local chamber soft-limit latch used
                                    // only while a remote chamber temperature controls regulation
static pb_pid_state_t s_pid;        // control-task only — external chamber PID state
static int64_t     s_pid_window_start_us; // control-task only — time-proportion window origin
static float       s_pid_duty;      // control-task only — last normalized PID output [0,1]
static float       s_max_target_c;      // guarded by s_mux — settable set-point ceiling
static int64_t     s_comms_timeout_us;  // guarded by s_mux — comms deadman (microseconds)
static float       s_cool_release_c;    // guarded by s_mux — residual-heat purge "cool down to" temp
static float       s_fb_cut_c;          // guarded by s_mux — user foldback-cut override (0 = auto/per-Rref)

// Persisted settings live in the shared app_nvs namespace (centi-°C / ms u32).
#define NVS_NS             "app_nvs"
#define KEY_HEAT_MAX_C     "heat_max_c"      // u32 centi-°C
#define KEY_HEAT_COMMS_MS  "heat_comms_ms"   // u32 ms
#define KEY_COOL_REL_C     "cool_rel_c"      // u32 centi-°C — cooldown-purge release temp
#define KEY_FB_CUT_C       "fb_cut_c"        // u32 centi-°C — foldback-cut override (0 = auto)
#define KEY_FAULT_LATCH    "fault_latch"     // u8 0/1 — persisted safety-fault latch
#define KEY_FAULT_CODE     "fault_code"      // u8 pb_fault_reason_t

// Canonical, stable strings for each fault code (index == code). Kept identical to
// the historical trip strings so the API contract doesn't change for existing
// causes. Any out-of-range/corrupt code maps to a generic latched-fault string.
static const char *const k_fault_str[PB_FAULT__COUNT] = {
    [PB_FAULT_NONE]             = "none",
    [PB_FAULT_PTC_OVERTEMP]     = "PTC element over-temp",
    [PB_FAULT_CHAMBER_OVERTEMP] = "chamber over-temp",
    [PB_FAULT_CHAMBER_SENSOR]   = "chamber sensor fault",
    [PB_FAULT_PTC_SENSOR]       = "PTC sensor fault",
    [PB_FAULT_LINK_LOST]        = "controller link lost",
    [PB_FAULT_PANIC_OFF]        = "panic-off",
    [PB_FAULT_INHIBITED]        = "inhibited",
    [PB_FAULT_EMERGENCY]        = "safety trip",
    [PB_FAULT_NVS_UNREADABLE]   = "persisted fault state unreadable",
};

const char *pb_heater_fault_str(pb_fault_reason_t code)
{
    if ((unsigned)code >= PB_FAULT__COUNT || !k_fault_str[code]) return "latched fault";
    return k_fault_str[code];
}
// pb_heater_fault_decide() is a pure header inline (see pb_heater.h) so the
// boot-time fail-safe logic can be host-tested without an NVS backend.

static float centi_to_c(uint32_t v) { return (float)v / 100.0f; }
static uint32_t c_to_centi(float c)
{
    if (c < 0.0f)   c = 0.0f;
    if (c > 200.0f) c = 200.0f;
    return (uint32_t)(c * 100.0f + 0.5f);
}

static void ssr_set(bool on)        // control-task context only
{
#ifndef CONFIG_PB_HIL_DEVBOARD
    gpio_set_level(PB_GPIO_RELAY, on ? 1 : 0);
#endif
    s_on = on;
}

static void pid_runtime_reset(void)
{
    pb_pid_reset(&s_pid);
    s_pid_window_start_us = 0;
    s_pid_duty = 0.0f;
}

esp_err_t pb_heater_init(void)
{
    if (!s_persist_lock) s_persist_lock = xSemaphoreCreateMutex();
#ifndef CONFIG_PB_HIL_DEVBOARD
    const gpio_config_t io = {
        .pin_bit_mask = (1ULL << PB_GPIO_RELAY),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) return err;
#endif
    ssr_set(false);                  // guaranteed OFF before any request
    taskENTER_CRITICAL(&s_mux);
    s_target_c = 0.0f;
    s_control_chamber_c = NAN;
    s_latched_off = false;
    s_fault_reason = NULL;
    s_fault_code = PB_FAULT_NONE;
    s_last_link_us = esp_timer_get_time();
    // Conservative defaults ONLY — nvs isn't up yet; pb_heater_load_config()
    // applies persisted values later (called after nvs_init in app_main).
    s_max_target_c = PB_HEATER_MAX_TARGET_C_DEFAULT;
    s_comms_timeout_us = (int64_t)PB_HEATER_COMMS_TIMEOUT_MS_DEFAULT * 1000;
    s_cool_release_c = PB_HEATER_COOL_RELEASE_C_DEFAULT;
    s_fb_cut_c = 0.0f;   // auto (per-Rref default) until a user override is loaded/set
    taskEXIT_CRITICAL(&s_mux);
    s_heat_intent = false;
    s_fb_cut = false;
    s_local_cut = false;
    pid_runtime_reset();
#ifdef CONFIG_PB_HIL_DEVBOARD
    ESP_LOGW(TAG, "HIL dev-board backend: relay GPIO compiled out");
#else
    ESP_LOGI(TAG, "init: SSR forced OFF");
#endif
    return ESP_OK;
}

esp_err_t pb_heater_set_target_c(float target_c)
{
    if (!isfinite(target_c)) {
        ESP_LOGW(TAG, "rejected non-finite target");
        return ESP_ERR_INVALID_ARG;
    }
    if (target_c < 0.0f) target_c = 0.0f;

    esp_err_t r = ESP_OK;
    taskENTER_CRITICAL(&s_mux);
    if (target_c > s_max_target_c) target_c = s_max_target_c;
    if ((s_latched_off || s_inhibited) && target_c > 0.0f) {
        r = ESP_ERR_INVALID_STATE;
    } else {
        s_target_c = target_c;
    }
    taskEXIT_CRITICAL(&s_mux);

    if (r == ESP_OK)
        ESP_LOGI(TAG, "target set to %.1f C", target_c);
    else
        ESP_LOGW(TAG, "target %.1f rejected: fault latched (clear fault, then issue a fresh command)", target_c);
    return r;
}

float pb_heater_get_target_c(void)
{
    taskENTER_CRITICAL(&s_mux);
    float t = s_target_c;
    taskEXIT_CRITICAL(&s_mux);
    return t;
}

void pb_heater_set_control_chamber_c(float temp_c)
{
    taskENTER_CRITICAL(&s_mux);
    s_control_chamber_c = isfinite(temp_c) ? temp_c : NAN;
    taskEXIT_CRITICAL(&s_mux);
}

void pb_heater_load_config(void)
{
    float    max_c    = PB_HEATER_MAX_TARGET_C_DEFAULT;
    uint32_t comms_ms = PB_HEATER_COMMS_TIMEOUT_MS_DEFAULT;
    float    cool_c   = PB_HEATER_COOL_RELEASE_C_DEFAULT;
    float    fb_cut   = 0.0f;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint32_t v;
        if (nvs_get_u32(h, KEY_HEAT_MAX_C, &v) == ESP_OK)    max_c    = centi_to_c(v);
        if (nvs_get_u32(h, KEY_HEAT_COMMS_MS, &v) == ESP_OK) comms_ms = v;
        if (nvs_get_u32(h, KEY_COOL_REL_C, &v) == ESP_OK)    cool_c   = centi_to_c(v);
        if (nvs_get_u32(h, KEY_FB_CUT_C, &v) == ESP_OK)      fb_cut   = centi_to_c(v);
        nvs_close(h);
    }
    if (max_c < PB_HEATER_MIN_TARGET_C)     max_c = PB_HEATER_MIN_TARGET_C;
    if (max_c > PB_HEATER_ABS_MAX_TARGET_C) max_c = PB_HEATER_ABS_MAX_TARGET_C;
    if (comms_ms < PB_HEATER_COMMS_TIMEOUT_MS_MIN) comms_ms = PB_HEATER_COMMS_TIMEOUT_MS_MIN;
    if (comms_ms > PB_HEATER_COMMS_TIMEOUT_MS_MAX) comms_ms = PB_HEATER_COMMS_TIMEOUT_MS_MAX;
    if (cool_c < PB_HEATER_COOL_RELEASE_MIN_C) cool_c = PB_HEATER_COOL_RELEASE_MIN_C;
    if (cool_c > PB_HEATER_COOL_RELEASE_MAX_C) cool_c = PB_HEATER_COOL_RELEASE_MAX_C;
    if (fb_cut > 0.0f) {
        if (fb_cut < PB_HEATER_FB_CUT_MIN_C) fb_cut = PB_HEATER_FB_CUT_MIN_C;
        if (fb_cut > PB_HEATER_FB_CUT_MAX_C) fb_cut = PB_HEATER_FB_CUT_MAX_C;
    }
    taskENTER_CRITICAL(&s_mux);
    s_max_target_c = max_c;
    s_comms_timeout_us = (int64_t)comms_ms * 1000;
    s_cool_release_c = cool_c;
    s_fb_cut_c = fb_cut;
    if (s_target_c > s_max_target_c) s_target_c = s_max_target_c;
    taskEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "config: max_target=%.1fC comms_timeout=%ums cool_release=%.1fC fb_cut=%.1fC",
             max_c, (unsigned)comms_ms, cool_c, fb_cut);
}

esp_err_t pb_heater_set_max_target_c(float max_c)
{
    if (!isfinite(max_c)) return ESP_ERR_INVALID_ARG;
    if (max_c < PB_HEATER_MIN_TARGET_C)     max_c = PB_HEATER_MIN_TARGET_C;
    if (max_c > PB_HEATER_ABS_MAX_TARGET_C) max_c = PB_HEATER_ABS_MAX_TARGET_C;
    taskENTER_CRITICAL(&s_mux);
    s_max_target_c = max_c;
    if (s_target_c > s_max_target_c) s_target_c = s_max_target_c;
    taskEXIT_CRITICAL(&s_mux);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, KEY_HEAT_MAX_C, c_to_centi(max_c));
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "max_target set to %.1f C", max_c);
    return ESP_OK;
}

float pb_heater_get_max_target_c(void)
{
    taskENTER_CRITICAL(&s_mux);
    float m = s_max_target_c;
    taskEXIT_CRITICAL(&s_mux);
    return m;
}

esp_err_t pb_heater_set_comms_timeout_ms(uint32_t ms)
{
    if (ms < PB_HEATER_COMMS_TIMEOUT_MS_MIN) ms = PB_HEATER_COMMS_TIMEOUT_MS_MIN;
    if (ms > PB_HEATER_COMMS_TIMEOUT_MS_MAX) ms = PB_HEATER_COMMS_TIMEOUT_MS_MAX;
    taskENTER_CRITICAL(&s_mux);
    s_comms_timeout_us = (int64_t)ms * 1000;
    taskEXIT_CRITICAL(&s_mux);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, KEY_HEAT_COMMS_MS, ms);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "comms_timeout set to %u ms", (unsigned)ms);
    return ESP_OK;
}

uint32_t pb_heater_get_comms_timeout_ms(void)
{
    taskENTER_CRITICAL(&s_mux);
    int64_t us = s_comms_timeout_us;
    taskEXIT_CRITICAL(&s_mux);
    return (uint32_t)(us / 1000);
}

esp_err_t pb_heater_set_cool_release_c(float c)
{
    if (!isfinite(c)) return ESP_ERR_INVALID_ARG;
    if (c < PB_HEATER_COOL_RELEASE_MIN_C) c = PB_HEATER_COOL_RELEASE_MIN_C;
    if (c > PB_HEATER_COOL_RELEASE_MAX_C) c = PB_HEATER_COOL_RELEASE_MAX_C;
    taskENTER_CRITICAL(&s_mux);
    s_cool_release_c = c;
    taskEXIT_CRITICAL(&s_mux);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, KEY_COOL_REL_C, c_to_centi(c));
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "cool_release set to %.1f C", c);
    return ESP_OK;
}

float pb_heater_get_cool_release_c(void)
{
    taskENTER_CRITICAL(&s_mux);
    float c = s_cool_release_c;
    taskEXIT_CRITICAL(&s_mux);
    return c;
}

esp_err_t pb_heater_set_fb_cut_c(float c)
{
    if (!isfinite(c)) return ESP_ERR_INVALID_ARG;
    if (c <= 0.0f) {
        c = 0.0f;
    } else {
        if (c < PB_HEATER_FB_CUT_MIN_C) c = PB_HEATER_FB_CUT_MIN_C;
        if (c > PB_HEATER_FB_CUT_MAX_C) c = PB_HEATER_FB_CUT_MAX_C;
    }
    taskENTER_CRITICAL(&s_mux);
    s_fb_cut_c = c;
    taskEXIT_CRITICAL(&s_mux);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, KEY_FB_CUT_C, c_to_centi(c));
        nvs_commit(h);
        nvs_close(h);
    }
    if (c > 0.0f) ESP_LOGI(TAG, "foldback cut override set to %.1f C", c);
    else          ESP_LOGI(TAG, "foldback cut override cleared (auto/per-Rref)");
    return ESP_OK;
}

float pb_heater_get_fb_cut_c(void)
{
    taskENTER_CRITICAL(&s_mux);
    float c = s_fb_cut_c;
    taskEXIT_CRITICAL(&s_mux);
    return c;
}

void pb_heater_notify_link_alive(void)
{
    int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_mux);
    s_last_link_us = now;
    taskEXIT_CRITICAL(&s_mux);
}

static esp_err_t persist_fault(bool latched, pb_fault_reason_t code)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) { ESP_LOGE(TAG, "fault persist: nvs_open %s", esp_err_to_name(err)); return err; }
    err = nvs_set_u8(h, KEY_FAULT_LATCH, latched ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(h, KEY_FAULT_CODE, (uint8_t)code);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGE(TAG, "fault persist: %s", esp_err_to_name(err));
    return err;
}

static void do_latch(pb_fault_reason_t code, const char *reason,
                     bool inhibit, bool drive_ssr, bool persist)
{
    if (drive_ssr) ssr_set(false);
    bool transition;
    taskENTER_CRITICAL(&s_mux);
    transition = !s_latched_off;
    s_target_c = 0.0f;
    s_latched_off = true;
    if (inhibit) s_inhibited = true;
    s_fault_code = code;
    s_fault_reason = reason ? reason : pb_heater_fault_str(code);
    taskEXIT_CRITICAL(&s_mux);
    pid_runtime_reset();
    if (persist && transition) {
        if (s_persist_lock) xSemaphoreTake(s_persist_lock, portMAX_DELAY);
        esp_err_t pe = persist_fault(true, code);
        if (s_persist_lock) xSemaphoreGive(s_persist_lock);
        if (pe != ESP_OK) {
            taskENTER_CRITICAL(&s_mux);
            s_persist_pending = true;
            taskEXIT_CRITICAL(&s_mux);
            ESP_LOGW(TAG, "fault persist deferred (will retry): %s", pb_heater_fault_str(code));
        }
    }
}

static void trip(pb_fault_reason_t code)
{
    do_latch(code, NULL, false, true, true);
    ESP_LOGW(TAG, "EMERGENCY OFF: %s", pb_heater_fault_str(code));
}

void pb_heater_emergency_off(const char *reason)
{
    do_latch(PB_FAULT_EMERGENCY, reason, false, true, true);
    ESP_LOGW(TAG, "EMERGENCY OFF: %s", reason ? reason : "(unspecified)");
}

void pb_heater_request_panic_off(const char *reason)
{
    do_latch(PB_FAULT_PANIC_OFF, reason, false, false, false);
}

esp_err_t pb_heater_clear_fault(void)
{
    bool was;
    taskENTER_CRITICAL(&s_mux);
    if (s_inhibited) {
        taskEXIT_CRITICAL(&s_mux);
        ESP_LOGW(TAG, "clear ignored: heater permanently inhibited (reboot required)");
        return ESP_ERR_INVALID_STATE;
    }
    was = s_latched_off;
    taskEXIT_CRITICAL(&s_mux);

    if (s_persist_lock) xSemaphoreTake(s_persist_lock, portMAX_DELAY);
    esp_err_t err = persist_fault(false, PB_FAULT_NONE);
    if (err != ESP_OK) {
        if (s_persist_lock) xSemaphoreGive(s_persist_lock);
        ESP_LOGE(TAG, "clear rejected: NVS clear failed (%s) — fault stays latched", esp_err_to_name(err));
        return err;
    }
    taskENTER_CRITICAL(&s_mux);
    s_latched_off = false;
    s_target_c = 0.0f;
    s_fault_reason = NULL;
    s_fault_code = PB_FAULT_NONE;
    s_persist_pending = false;
    taskEXIT_CRITICAL(&s_mux);
    pid_runtime_reset();
    if (s_persist_lock) xSemaphoreGive(s_persist_lock);
    if (was) ESP_LOGW(TAG, "fault latch cleared; target reset to 0 (send a fresh target to resume)");
    return ESP_OK;
}

void pb_heater_inhibit(const char *reason)
{
    do_latch(PB_FAULT_INHIBITED, reason, true, true, false);
    ESP_LOGE(TAG, "HEATER INHIBITED (reboot-only): %s", reason ? reason : "(unspecified)");
}

bool pb_heater_is_inhibited(void) { return s_inhibited; }
bool pb_heater_is_faulted(void) { return s_latched_off || s_inhibited; }
bool pb_heater_is_on(void) { return s_on; }

const char *pb_heater_fault_reason(void)
{
    taskENTER_CRITICAL(&s_mux);
    const char *r = s_fault_reason;
    taskEXIT_CRITICAL(&s_mux);
    return r;
}

pb_fault_reason_t pb_heater_fault_code(void)
{
    taskENTER_CRITICAL(&s_mux);
    pb_fault_reason_t c = s_fault_code;
    taskEXIT_CRITICAL(&s_mux);
    return c;
}

void pb_heater_load_fault(void)
{
    nvs_handle_t h;
    esp_err_t oe = nvs_open(NVS_NS, NVS_READONLY, &h);
    bool ns_not_found = (oe == ESP_ERR_NVS_NOT_FOUND);
    bool open_ok = (oe == ESP_OK);
    uint8_t latch_val = 0, code_val = PB_FAULT_NONE;
    bool latch_read_ok = false, latch_not_found = false;
    if (open_ok) {
        esp_err_t le = nvs_get_u8(h, KEY_FAULT_LATCH, &latch_val);
        latch_not_found = (le == ESP_ERR_NVS_NOT_FOUND);
        latch_read_ok = (le == ESP_OK);
        nvs_get_u8(h, KEY_FAULT_CODE, &code_val);
        nvs_close(h);
    }
    pb_fault_reason_t code;
    bool latched = pb_heater_fault_decide(open_ok, ns_not_found, latch_read_ok,
                                          latch_not_found, latch_val, code_val, &code);
    if (latched) {
        do_latch(code, NULL, false, false, false);
        ESP_LOGW(TAG, "boot: restored latched fault: %s", pb_heater_fault_str(code));
    }
}

bool pb_heater_heat_mode(void)
{
    taskENTER_CRITICAL(&s_mux);
    bool m = (s_target_c > 0.0f && !s_latched_off);
    taskEXIT_CRITICAL(&s_mux);
    return m;
}

void pb_heater_tick(void)
{
    bool pending;
    taskENTER_CRITICAL(&s_mux);
    pending = s_persist_pending;
    taskEXIT_CRITICAL(&s_mux);
    if (pending) {
        int64_t now = esp_timer_get_time();
        taskENTER_CRITICAL(&s_mux);
        bool due = (now - s_persist_retry_us) >= 2000000;
        taskEXIT_CRITICAL(&s_mux);
        if (due && s_persist_lock && xSemaphoreTake(s_persist_lock, 0) == pdTRUE) {
            taskENTER_CRITICAL(&s_mux);
            bool still = s_latched_off;
            pb_fault_reason_t code = s_fault_code;
            s_persist_retry_us = now;
            taskEXIT_CRITICAL(&s_mux);
            esp_err_t pr = still ? persist_fault(true, code) : ESP_OK;
            if (pr == ESP_OK) {
                taskENTER_CRITICAL(&s_mux);
                s_persist_pending = false;
                taskEXIT_CRITICAL(&s_mux);
                ESP_LOGI(TAG, "deferred fault persist committed");
            }
            xSemaphoreGive(s_persist_lock);
        }
    }

    float target;
    float control_chamber_c;
    bool latched;
    int64_t last_link;
    int64_t comms_timeout_us;
    taskENTER_CRITICAL(&s_mux);
    target = s_target_c;
    control_chamber_c = s_control_chamber_c;
    latched = s_latched_off;
    last_link = s_last_link_us;
    comms_timeout_us = s_comms_timeout_us;
    taskEXIT_CRITICAL(&s_mux);

    const bool armed = (target > 0.0f);
    float ptc_c = 0.0f, chamber_c = 0.0f;
    pb_ntc_status_t ps = pb_ntc_read(PB_NTC_PTC, &ptc_c);
    pb_ntc_status_t cs = pb_ntc_read(PB_NTC_CHAMBER, &chamber_c);

    int64_t now_us = esp_timer_get_time();
    pb_fault_reason_t trip_code = pb_heater_eval_trip(
        ps == PB_NTC_OK, ptc_c, cs == PB_NTC_OK, chamber_c, armed,
        (now_us - last_link) > comms_timeout_us);
    if (trip_code != PB_FAULT_NONE) {
        trip(trip_code);
        return;
    }

    if (latched || !armed) {
        if (s_on) ssr_set(false);
        s_heat_intent = false;
        s_fb_cut = false;
        s_local_cut = false;
        pid_runtime_reset();
        return;
    }

    const bool external_regulation = isfinite(control_chamber_c);

    // Local chamber soft foldback for REMOTE regulation. This remains independent
    // of PID and wins unconditionally over its requested duty.
    if (external_regulation)
        s_local_cut = pb_heater_local_foldback_cut(cs == PB_NTC_OK, chamber_c, s_local_cut);
    else
        s_local_cut = false;

    // Element foldback is likewise an independent output veto beneath the fixed
    // 105 C latching cutoff.
    float fb_cut, fb_resume;
    pb_heater_effective_foldback(pb_heater_get_fb_cut_c(), pb_ntc_rref_kohm(), &fb_cut, &fb_resume);
    s_fb_cut = pb_heater_foldback_cut(ps == PB_NTC_OK, ptc_c, s_fb_cut, fb_cut, fb_resume);

    bool drive = false;
    if (external_regulation) {
        // External printer chamber telemetry gets slow time-proportioning PID rather
        // than bang-bang. A 10 s window is intentionally glacial for a chamber and
        // kind to a zero-cross SSR: e.g. 0.30 duty means roughly 3 s ON / 7 s OFF.
        // If either local soft limiter intervenes, reset PID state so integral cannot
        // accumulate behind a blocked output and launch a full-power pulse on release.
        if (s_local_cut || s_fb_cut) {
            pid_runtime_reset();
            drive = false;
        } else {
            s_pid_duty = pb_pid_step(&s_pid, target, control_chamber_c,
                                     PB_CHAMBER_PID_DT_S,
                                     PB_CHAMBER_PID_KP, PB_CHAMBER_PID_KI,
                                     PB_CHAMBER_PID_KD, PB_CHAMBER_PID_D_ALPHA);

            if (s_pid_window_start_us == 0 || now_us < s_pid_window_start_us)
                s_pid_window_start_us = now_us;
            int64_t elapsed = now_us - s_pid_window_start_us;
            if (elapsed >= PB_CHAMBER_PID_WINDOW_US) {
                int64_t windows = elapsed / PB_CHAMBER_PID_WINDOW_US;
                s_pid_window_start_us += windows * PB_CHAMBER_PID_WINDOW_US;
                elapsed = now_us - s_pid_window_start_us;
            }
            int64_t on_us = (int64_t)(s_pid_duty * (float)PB_CHAMBER_PID_WINDOW_US);
            drive = (on_us > 0 && elapsed < on_us);
            s_heat_intent = s_pid_duty > 0.0f;
        }
    } else {
        // No usable external measurement: preserve the proven local-NTC bang-bang
        // fallback exactly as before and discard any old remote-controller state.
        pid_runtime_reset();
        if (chamber_c < (target - PB_HEATER_HYSTERESIS_C))
            s_heat_intent = true;
        else if (chamber_c >= target)
            s_heat_intent = false;
        drive = s_heat_intent && !s_fb_cut;
    }

    if (drive != s_on) ssr_set(drive);
}
