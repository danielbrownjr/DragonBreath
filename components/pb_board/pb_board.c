// SPDX-License-Identifier: MIT
#include "pb_board.h"
#include "esp_log.h"
#include "esp_rom_sys.h"   // esp_rom_delay_us — let the strap settle before sampling

static const char *TAG = "pb_board";

void pb_board_init(void)
{
#ifdef CONFIG_PB_DEVBOARD_SAFE
    ESP_LOGW(TAG, "safe dev-board target: production board GPIO init compiled out");
#else
    const gpio_config_t leds = {
        .pin_bit_mask = (1ULL << PB_GPIO_LED_K1) |
                        (1ULL << PB_GPIO_LED_K2) |
                        (1ULL << PB_GPIO_LED_K3),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&leds);
    gpio_set_level(PB_GPIO_LED_K1, 0);
    gpio_set_level(PB_GPIO_LED_K2, 0);
    gpio_set_level(PB_GPIO_LED_K3, 0);
    ESP_LOGI(TAG, "board init: LEDs off; heater/fan owned by their components");
#endif
}

// Fail-safe Rref used when the strap can't be trusted (see below). 33 kOhm is the
// SMALLER reference: a too-small Rref biases BOTH NTC readings WARM, so the fixed
// 105 C / 85 C over-temp cutoffs trip early rather than late. (A too-large Rref would
// read cold and let the element run hotter than the firmware believes.) It is also
// the value the one observed floating board — Panda Breath V1.0 — actually needs.
#define PB_RREF_FLOATING_DEFAULT_KOHM  33

int pb_board_rref_kohm(void)
{
#ifdef CONFIG_PB_DEVBOARD_SAFE
    return 82;
#else
    // The Rref strap on GPIO19 selects the divider reference resistor (level 0 -> 82k,
    // level 1 -> 33k) and is read once at boot. It MUST be read robustly: not every
    // board firmly straps the pin, and a FLOATING input latches an arbitrary level
    // that can differ between a cold power-on and a warm (OTA) reboot — silently
    // picking the wrong Rref and shifting BOTH NTC readings by ~15 C (observed on a
    // V1.0 board: 27 C after a cold flash, 12 C after an OTA reboot).
    //
    // Detect a floating pin by sampling it under an internal pull-up AND an internal
    // pull-down: a FIRMLY strapped pin is driven by the board and reads the SAME both
    // ways (trust it); a FLOATING pin follows whichever pull is enabled and the two
    // reads DISAGREE (don't trust it — fall back to the fail-safe default).
    gpio_config_t strap = {
        .pin_bit_mask = (1ULL << PB_GPIO_RREF_STRAP),
        .mode = GPIO_MODE_INPUT,
        .intr_type = GPIO_INTR_DISABLE,
    };

    strap.pull_up_en = GPIO_PULLUP_ENABLE;
    strap.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&strap);
    esp_rom_delay_us(2000);                       // settle through the ~45k internal pull
    int with_pu = gpio_get_level(PB_GPIO_RREF_STRAP);

    strap.pull_up_en = GPIO_PULLUP_DISABLE;
    strap.pull_down_en = GPIO_PULLDOWN_ENABLE;
    gpio_config(&strap);
    esp_rom_delay_us(2000);
    int with_pd = gpio_get_level(PB_GPIO_RREF_STRAP);

    // Leave the pin as a plain high-Z input so we don't load the strap net at runtime.
    strap.pull_up_en = GPIO_PULLUP_DISABLE;
    strap.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&strap);

    if (with_pu == with_pd) {
        int level = with_pu;
        int rref = (level == 0) ? 82 : 33;
        ESP_LOGI(TAG, "Rref strap (GPIO19)=%d -> %d kOhm (firm)", level, rref);
        return rref;
    }

    ESP_LOGW(TAG, "Rref strap (GPIO19) FLOATING (pu=%d pd=%d) -> defaulting to %d kOhm (fail-safe)",
             with_pu, with_pd, PB_RREF_FLOATING_DEFAULT_KOHM);
    return PB_RREF_FLOATING_DEFAULT_KOHM;
#endif
}
