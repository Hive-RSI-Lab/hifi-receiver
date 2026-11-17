/*
 * The lab build's experiments on the board (core/hifi_lab.h has the rules).
 * Only in a build with CONFIG_HIFI_LAB (board/lite-lab).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "hifi_api.h"
#include "hifi_lab.h"

/* registers the experiments and starts the watchdog task; nothing runs */
void hifi_lab_esp_init(void);
/* hifi_ops_t.lab */
int hifi_lab_esp_op(bool post, const char *body, char *out, size_t cap);
/* the frame is up: its lock, an experiment, a tick in the last 5 s (false in a drill build) */
bool hifi_lab_esp_ok(void);
/* the "lab" self-test line */
void hifi_lab_esp_selftest(hifi_selftest_t *t);
/* an experiment that ended by itself; any task, never under the lab's lock */
void hifi_lab_esp_finished(const char *name, bool ok);

/* the experiments (esp/lab_*.c) */
extern const hifi_lab_exp_t lab_espnow;
