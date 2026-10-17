#pragma once

#include "esp_http_server.h"

#define DB_PORTAL_HTML_CACHE "no-store"
// URLs are stable across OTA. Revalidate static JS on every load against the
// firmware-build ETag so fresh HTML can never pair with pre-OTA JavaScript.
#define DB_PORTAL_JS_CACHE "no-cache"

esp_err_t db_portal_send_text(httpd_req_t *req, const char *content_type,
                               const char *cache_control,
                               const unsigned char *start,
                               const unsigned char *end);
