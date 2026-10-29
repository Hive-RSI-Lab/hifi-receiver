/*
 * Between the board code's files: hifi_main.c (power, codec, tones, the API's
 * ops) and hifi_remote.c (the relay, OTA writes, the trial boot).
 */
#pragma once

#include <stdbool.h>

#include "hifi_api.h"
#include "hifi_boot.h"

/* hifi_main.c: what the trial boot needs from the board */
void hifi_main_trial_inputs(hifi_trial_t *t);

/* hifi_remote.c */
extern hifi_ota_t hifi_ota_session;
void hifi_remote_start(const hifi_ops_t *ops); /* at init: the relay, SNTP and the trial boot */
void hifi_remote_status(hifi_remote_status_t *out);
int hifi_remote_set(const char *url, const char *user, const char *password, const char *prefix);
void hifi_ota_status(hifi_ota_status_t *out);
