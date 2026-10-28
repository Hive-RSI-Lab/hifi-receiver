#pragma once

#include "esp_http_server.h"
#include "hifi_api.h"

/* Registers /api/v1/* and the web app's files on squeezelite's server. */
void hifi_http_start(httpd_handle_t server, const hifi_ops_t *ops);
