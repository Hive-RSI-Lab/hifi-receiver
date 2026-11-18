/*
 * The lab build's experiments on the board (core/hifi_lab.h has the rules).
 * Only in a build with CONFIG_HIFI_LAB (board/lite-lab).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "hifi_api.h"
#include "hifi_lab.h"

/* registers the experiments, starts the tick task and the watch timer; nothing runs */
void hifi_lab_esp_init(void);
/* hifi_ops_t.lab */
int hifi_lab_esp_op(bool post, const char *body, char *out, size_t cap);
/* the frame is up: its lock, an experiment, a tick in the last 5 s (false in a drill build) */
bool hifi_lab_esp_ok(void);
/* the "lab" self-test line */
void hifi_lab_esp_selftest(hifi_selftest_t *t);
/* an experiment that ended by itself; run: the number its start was given.
 * Any task (not an ISR), never under the lab's lock. */
void hifi_lab_esp_finished(const char *name, uint32_t run, bool ok);

/* the experiments (esp/lab_*.c). All but lab_i2c are weak: only the app
 * pulls them in (patch 0035 adds -u for espnow, forge, lanscan, wifidrop and
 * crafted; 0017, 0027 and 0034 the rest), so the recovery image keeps the lab
 * frame and the read-only I2C one and stays inside its slot. */
extern const hifi_lab_exp_t lab_espnow __attribute__((weak));
extern const hifi_lab_exp_t lab_forge __attribute__((weak)); /* a stranger forging the espnow run's frames */
/* lab_bt, lab_a2dp and lab_ble are weak: only the app pulls them in (patch
 * 0017 adds -u lab_bt/lab_a2dp/lab_ble to its link), so the recovery image leaves them out and,
 * with them, the Bluedroid host they reach (it would not fit its slot). */
extern const hifi_lab_exp_t lab_bt __attribute__((weak));  /* images with Classic Bluetooth (CONFIG_BT_CLASSIC_ENABLED) */
extern const hifi_lab_exp_t lab_a2dp __attribute__((weak)); /* the same images: a real A2DP stream between two boards */
extern const hifi_lab_exp_t lab_lanscan __attribute__((weak));
extern const hifi_lab_exp_t lab_wifidrop __attribute__((weak));
extern const hifi_lab_exp_t lab_crafted __attribute__((weak)); /* a crafted packet or stream for the ALAC and MP3 decoders */
extern const hifi_lab_exp_t lab_i2c; /* read-only: the board's I2C chips (esp/lab_i2c.c) */
extern const hifi_lab_exp_t lab_ble __attribute__((weak)); /* the BLE lab image only (CONFIG_BT_BLE_ENABLED) */
/* the lab-radio image only (CONFIG_HIFI_LAB_PSRAM_STACKS): weak and pulled
 * into the app by patch 0027, so recovery leaves out libFLAC and the clip */
extern const hifi_lab_exp_t lab_flac __attribute__((weak));
/* images with the Spotify sink (CONFIG_CSPOT_SINK): weak and pulled into the
 * app by patch 0034, so recovery leaves out the reach into squeezelite */
extern const hifi_lab_exp_t lab_phone __attribute__((weak));
/* lab_bt and lab_a2dp's source both change Bluetooth's state, so each refuses
 * to start while the other runs (weak: either may be left out) */
bool hifi_lab_bt_busy(void) __attribute__((weak));
bool hifi_lab_a2dp_busy(void) __attribute__((weak));
