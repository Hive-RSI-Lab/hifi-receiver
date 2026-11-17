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
void hifi_main_note_boot_heap(void); /* takes the boot heap (hifi_boot.h), once */
esp_err_t hifi_i2c_read(uint8_t addr, uint8_t reg, uint8_t *val);
esp_err_t hifi_i2c_write(uint8_t addr, uint8_t reg, uint8_t val);
void hifi_i2c_ensure(void);       /* installs the I2C driver if squeezelite hasn't */
void hifi_main_action(int action); /* a hifi_ir_action_t, from a button */
void hifi_main_knob(int detents);
bool hifi_main_amp_latched(void);
bool hifi_main_audio_active(void); /* something plays above about -77 dBFS */
void hifi_main_gov_amps(int n);    /* stereo amps playing: 2 with Ultra's zone B */
/* The address the box is reached at: the active network interface's (Wi-Fi
 * station or Ethernet; never the setup access point's). False, and ip "",
 * with none up. ip may be NULL. */
bool hifi_net_ip(char *ip, size_t n);

/* hifi_net.c */
int hifi_net_wifi_scan(hifi_wifi_ap_t *aps, int max);
int hifi_net_wifi_forget(void);
int hifi_net_update_check(hifi_update_offer_t *out, const char **err);
int hifi_net_update_start(const char *url, const char *sha256, bool need_sig, const char **err);
void hifi_net_update_status(hifi_update_status_t *out);
esp_err_t hifi_net_upload_handler(httpd_req_t *req); /* POST /api/v1/update/upload */

/* hifi_listen_esp.c: "cannot open the home network" (core/hifi_listen.h) */
void hifi_listen_tick(bool up);          /* every couple of seconds; scans every 10 s */
void hifi_listen_trial(hifi_trial_t *t); /* fills listen_wait / listen_open */
int hifi_listen_now(char *why, size_t n); /* a fresh scan for the self-test: strays */
void hifi_listen_diag(hifi_diag_net_t *n); /* L4: the running report, for /api/diag */

/* hifi_relay.c */
extern hifi_ota_t hifi_ota_session;
void hifi_remote_start(const hifi_ops_t *ops); /* at init: the relay, SNTP and the trial boot */
void hifi_remote_status(hifi_remote_status_t *out);
void hifi_remote_up(bool *configured, bool *up); /* the two bools of the status, light */
int hifi_remote_set(const char *url, const char *user, const char *password, const char *prefix, const char *key);
void hifi_ota_status(hifi_ota_status_t *out);
/* the remote link's state and counters into n (link_*, tx/rx bytes), and
 * when it first came up, ms from power-up (0: not yet) */
void hifi_remote_link(hifi_diag_net_t *n, uint32_t *first_up_ms);

/* hifi_diag_esp.c: the board's side of `api diag` */
extern const hifi_diag_ops_t hifi_diag_ops;
void hifi_diag_init(void); /* at hifi_init: the boot counter and reset history */
void hifi_diag_tick(void); /* every few seconds, from the relay's task */
/* a finished update over the relay: its size, time and the time spent in
 * flash writes, kept for `diag ota` */
void hifi_diag_ota_done(uint32_t bytes, uint32_t total_ms, uint32_t write_ms, const char *version);
