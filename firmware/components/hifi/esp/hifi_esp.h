/*
 * Between the board code's files: hifi_main.c (power, tones, volume, the
 * API's ops), board_*.c (hifi_bsp.h), hifi_net.c (Wi-Fi scan and the app's
 * updates) and hifi_relay.c (the relay, OTA writes, the trial boot).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

#include "hifi_api.h"
#include "hifi_boot.h"

/* hifi_main.c */
extern const hifi_ops_t hifi_ops;
void hifi_main_trial_inputs(hifi_trial_t *t); /* what the trial boot needs from the board */
esp_err_t hifi_i2c_read(uint8_t addr, uint8_t reg, uint8_t *val);
esp_err_t hifi_i2c_write(uint8_t addr, uint8_t reg, uint8_t val);
void hifi_i2c_ensure(void);       /* installs the I2C driver if squeezelite hasn't */
void hifi_main_action(int action); /* a hifi_ir_action_t, from a button */
void hifi_main_knob(int detents);
bool hifi_main_amp_latched(void);
void hifi_main_gov_amps(int n);    /* stereo amps playing: 2 with Ultra's zone B */
/* The address the box is reached at: the active network interface's (Wi-Fi
 * station or Ethernet; never the setup access point's). False, and ip "",
 * with none up. ip may be NULL. */
bool hifi_net_ip(char *ip, size_t n);

/* hifi_net.c */
int hifi_net_wifi_scan(hifi_wifi_ap_t *aps, int max);
int hifi_net_wifi_forget(void);
int hifi_net_update_check(hifi_update_offer_t *out, const char **err);
int hifi_net_update_start(const char *url, const char *sha256, const char **err);
void hifi_net_update_status(hifi_update_status_t *out);
esp_err_t hifi_net_upload_handler(httpd_req_t *req); /* POST /api/v1/update/upload */

/* hifi_relay.c */
extern hifi_ota_t hifi_ota_session;
void hifi_remote_start(const hifi_ops_t *ops); /* at init: the relay, SNTP and the trial boot */
void hifi_remote_status(hifi_remote_status_t *out);
int hifi_remote_set(const char *url, const char *user, const char *password, const char *prefix);
void hifi_ota_status(hifi_ota_status_t *out);
