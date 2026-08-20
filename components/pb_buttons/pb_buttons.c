// SPDX-License-Identifier: MIT
#include "pb_buttons.h"
#include "pb_buttons_sm.h"
#include "pb_board.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#ifndef CONFIG_PB_HIL_DEVBOARD
#include "driver/gpio.h"
#endif

#include <stdatomic.h>

static const char *TAG = "pb_buttons";

typedef struct {
    pb_button_id_t id;
    gpio_num_t     pin;
    pb_btn_sm_t    sm;
} btn_t;

static btn_t s_btns[PB_BUTTON_COUNT] = {
    { PB_BUTTON_POWER, PB_GPIO_BTN_POWER, { .active_low = true } },
    { PB_BUTTON_AUTO,  PB_GPIO_BTN_AUTO,  { .active_low = true } },
    { PB_BUTTON_ON,    PB_GPIO_BTN_ON,    { .active_low = true } },
    { PB_BUTTON_DRY,   PB_GPIO_BTN_DRY,   { .active_low = true } },
};

static pb_button_cb_t s_cb;
static TaskHandle_t   s_task;

#ifdef CONFIG_PB_HIL_DEVBOARD
// Injected electrical levels; default 1 (idle high, matching the pull-up) so a
// button reads released until a scenario drives it low.
static _Atomic int s_hil_level[PB_BUTTON_COUNT] = { 1, 1, 1, 1 };

static int sample(const btn_t *b) { return atomic_load(&s_hil_level[b->id]); }

void pb_buttons_hil_set_level(pb_button_id_t id, int level)
{
    if (id >= 0 && id < PB_BUTTON_COUNT)
        atomic_store(&s_hil_level[id], level ? 1 : 0);
}

bool pb_buttons_hil_pressed(pb_button_id_t id)
{
    if (id < 0 || id >= PB_BUTTON_COUNT) return false;
    return s_btns[id].sm.pressed;
}
#else
static int sample(const btn_t *b) { return gpio_get_level(b->pin); }

static void configure_pin(gpio_num_t pin)
{
    // Internal pull-up only — NEVER pull-down. GPIO9/8/2 are strapping pins that
    // must read HIGH at reset; a pull-down would fight the strap.
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
}
#endif

#define PB_BTN_RESET_COMBO_TICKS (PB_BTN_RESET_COMBO_MS / PB_BTN_TICK_MS)

static void button_task(void *arg)
{
    (void)arg;
    int combo_ticks = 0;
    for (;;) {
        // Step every button first so debounce/long-press timing is unaffected,
        // capturing this tick's event per button.
        pb_btn_event_t evs[PB_BUTTON_COUNT];
        for (int i = 0; i < PB_BUTTON_COUNT; ++i)
            evs[i] = pb_buttons_sm_step(&s_btns[i].sm, sample(&s_btns[i]));

        // Recovery combo: Power + Auto both debounced-held. While it's building we
        // swallow those two buttons' own short/long events (so the hold doesn't
        // also fire Power's panic-off or Auto's mode toggle); at the threshold we
        // emit exactly one PB_BUTTON_RESET. esp_restart() in the handler means the
        // == test can never re-fire.
        bool combo = s_btns[PB_BUTTON_POWER].sm.pressed &&
                     s_btns[PB_BUTTON_AUTO].sm.pressed;
        if (combo) {
            if (++combo_ticks == PB_BTN_RESET_COMBO_TICKS) {
                ESP_LOGW(TAG, "Power+Auto held %d ms: emitting reset combo",
                         PB_BTN_RESET_COMBO_MS);
                if (s_cb) s_cb(PB_BUTTON_POWER, PB_BUTTON_RESET);
            }
        } else {
            combo_ticks = 0;
        }

        for (int i = 0; i < PB_BUTTON_COUNT; ++i) {
            if (evs[i] == PB_BTN_EV_NONE || !s_cb) continue;
            if (combo && (i == PB_BUTTON_POWER || i == PB_BUTTON_AUTO)) continue;
            s_cb(s_btns[i].id,
                 evs[i] == PB_BTN_EV_LONG ? PB_BUTTON_LONG : PB_BUTTON_SHORT);
        }
        vTaskDelay(pdMS_TO_TICKS(PB_BTN_TICK_MS));
    }
}

esp_err_t pb_buttons_start(pb_button_cb_t cb)
{
    if (s_task) return ESP_ERR_INVALID_STATE;
    s_cb = cb;

    for (int i = 0; i < PB_BUTTON_COUNT; ++i) {
#ifndef CONFIG_PB_HIL_DEVBOARD
        configure_pin(s_btns[i].pin);
#endif
        // Seed from a live sample so a button held through boot (or a shorted
        // line) is ignored until it releases.
        pb_buttons_sm_seed(&s_btns[i].sm, sample(&s_btns[i]));
    }

    if (xTaskCreate(button_task, "pb_buttons", 3072, NULL, 4, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
#ifdef CONFIG_PB_HIL_DEVBOARD
    ESP_LOGW(TAG, "HIL dev-board backend: button levels injected over serial");
#else
    ESP_LOGI(TAG, "front-panel buttons running (poll %d ms)", PB_BTN_TICK_MS);
#endif
    return ESP_OK;
}
