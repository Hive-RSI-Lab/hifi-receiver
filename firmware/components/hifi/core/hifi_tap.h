/*
 * The play button works like a headphone button: one tap play/pause, two
 * next, three previous. Taps are counted until the button has been quiet
 * for HIFI_TAP_GAP_MS; a third tap acts at once.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define HIFI_TAP_GAP_MS 300
#define HIFI_TAP_MAX 3

typedef struct {
	uint8_t count;    /* taps so far, 0 when idle */
	uint32_t last_ms; /* the last tap */
} hifi_tap_t;

void hifi_tap_init(hifi_tap_t *t);
void hifi_tap_press(hifi_tap_t *t, uint32_t now_ms);
/* The finished count (1..HIFI_TAP_MAX), once; 0 while idle or still counting */
int hifi_tap_poll(hifi_tap_t *t, uint32_t now_ms);
bool hifi_tap_pending(const hifi_tap_t *t);
