#pragma once

#include "esp_http_server.h"
#include "hifi_api.h"

/* Registers the /api/v1/ handlers and the web app's files on squeezelite's server. */
void hifi_http_start(httpd_handle_t server, const hifi_ops_t *ops);

/* Registers u behind the LAN gate (core/hifi_gate.h): a request whose Host or
 * Origin is not this board gets 403. squeezelite's and cspot's handlers come
 * through here too (firmware/patches/0009). */
esp_err_t hifi_lan_register(httpd_handle_t server, const httpd_uri_t *u);
