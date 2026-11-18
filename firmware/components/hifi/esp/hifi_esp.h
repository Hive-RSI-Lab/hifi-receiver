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
#include "esp_http_client.h"
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
/* The quiet stop (hifi_quiet.h): ramps the outputs down, mutes the DAC and
 * puts the amps into shutdown, once, and holds them there. Every planned
 * restart (the esp_restart wrap, hifi_crash.c) and every update (the OTA
 * writer, before the slot opens) calls it first. From an ISR, or with the
 * scheduler stopped, it only drops the amp. why goes to the log; restart
 * marks it final, so no release can undo it. */
void hifi_quiet_stop(const char *why, bool restart);
/* An update that failed or was abandoned: the DAC unmutes, the routing goes
 * back on and the amp comes back after a short hold. */
void hifi_quiet_release(void);
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
/* upstream's IP-change reboot asks here first (patch 0030): true while the
 * image is on trial, and the restart then runs once the trial keeps it */
bool hifi_restart_after_trial(const char *why);

/* hifi_diag_esp.c: the board's side of `api diag` */
extern const hifi_diag_ops_t hifi_diag_ops;
void hifi_diag_init(void); /* at hifi_init: the boot counter and reset history */
void hifi_diag_tick(void); /* every few seconds, from the relay's task */
/* the station's disconnects since boot (each WIFI_EVENT_STA_DISCONNECTED) */
uint32_t hifi_diag_wifi_disconnects(void);
/* a finished update over the relay: its size, time and the time spent in
 * flash writes, kept for `diag ota` */
void hifi_diag_ota_done(uint32_t bytes, uint32_t total_ms, uint32_t write_ms, const char *version);

/* hifi_dl.c: the URL update's download with two buffers, the next chunk read
 * while this one is written. Weak, so only the app links it (patch 0036's -u
 * hifi_dl_pump); in recovery it is NULL and download() reads with one buffer.
 * buf is the caller's 4 KB buffer, used as one of the two. Returns 0 when the
 * image is all written, -1 when it couldn't start (out of memory: nothing
 * read, so the caller reads with one buffer), or else the error's status with
 * *err set and the session aborted. */
int hifi_dl_pump(hifi_ota_t *o, esp_http_client_handle_t h, uint8_t *buf, int *progress, const char **err)
	__attribute__((weak));

/* hifi_party_esp.c: party mode, several boards playing one stream in step.
 * Weak, so only the app links it (patch 0031's -u hifi_party_op); in recovery
 * every one is NULL, and callers check before calling. */
void hifi_party_init(void) __attribute__((weak));   /* at hifi_init, after the settings are read */
void hifi_party_poll(void) __attribute__((weak));   /* every few seconds, from the relay's task */
void hifi_party_selftest(hifi_selftest_t *t) __attribute__((weak));
int hifi_party_op(const char *sub, bool post, bool lan, const char *body, char *out, size_t cap) __attribute__((weak));
bool hifi_party_saved_on(void) __attribute__((weak)); /* party was on when the board last ran */
void hifi_party_volume(uint32_t gain) __attribute__((weak)); /* the board's volume, for the leader's stream */
