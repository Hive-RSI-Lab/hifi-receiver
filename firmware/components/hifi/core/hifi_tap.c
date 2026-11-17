#include "hifi_tap.h"

void hifi_tap_init(hifi_tap_t *t) {
	t->count = 0;
	t->last_ms = 0;
}

void hifi_tap_press(hifi_tap_t *t, uint32_t now_ms) {
	if (t->count < HIFI_TAP_MAX) t->count++;
	t->last_ms = now_ms;
}

int hifi_tap_poll(hifi_tap_t *t, uint32_t now_ms) {
	if (!t->count) return 0;
	if (t->count < HIFI_TAP_MAX && (uint32_t) (now_ms - t->last_ms) < HIFI_TAP_GAP_MS) return 0;
	int n = t->count;
	t->count = 0;
	return n;
}

bool hifi_tap_pending(const hifi_tap_t *t) {
	return t->count != 0;
}
