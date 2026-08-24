#pragma once
#include "esp_err.h"

typedef enum {
    DC_SRC_KLIPPER = 0,
    DC_SRC_BAMBU = 1,
    DC_SRC_HA = 2,
    DC_SRC_NONE = 3,
    DC_SRC_KLIPPER_MQTT = 4,
    DC_SRC_PRUSA = 5,
    DC_SRC_MAX,
} dc_ctl_source_t;
