#pragma once
#include <stdint.h>

typedef struct {
    char     host[64];
    uint16_t port;
    char     api_key[65];
} dc_prusa_config_t;
