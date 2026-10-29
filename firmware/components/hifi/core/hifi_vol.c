#include "hifi_vol.h"

#include <math.h>

static int clampi(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }

uint32_t hifi_vol_to_gain(int volume) {
	double r = clampi(volume, 0, 100) / 100.0;
	uint32_t g = (uint32_t) lrint(HIFI_VOL_UNITY * r * r * r);
	/* the bottom few would round together (and 1 to silence): keep each its own */
	return g < (uint32_t) clampi(volume, 0, 100) ? (uint32_t) clampi(volume, 0, 100) : g;
}

int hifi_vol_from_gain(uint32_t gain) {
	double r = (double) gain / HIFI_VOL_UNITY;
	if (r > 1) r = 1;
	int v = clampi((int) lrint(100 * cbrt(r)), 0, 100);
	/* the lowest volume whose gain reaches it: exact for our own gains */
	while (v > 0 && hifi_vol_to_gain(v - 1) >= gain) v--;
	while (v < 100 && hifi_vol_to_gain(v) < gain) v++;
	return v;
}

void hifi_vol_init(hifi_vol_t *v, int volume, int max_volume) {
	v->max_volume = clampi(max_volume, HIFI_VOL_MAX_MIN, 100);
	v->volume = clampi(volume, 0, v->max_volume);
	v->gain = hifi_vol_to_gain(v->volume);
	v->mute = false;
}

uint32_t hifi_vol_source(hifi_vol_t *v, uint32_t gain) {
	uint32_t cap = hifi_vol_to_gain(v->max_volume);
	if (gain > cap) gain = cap;
	v->gain = gain;
	v->volume = clampi(hifi_vol_from_gain(gain), 0, v->max_volume);
	return hifi_vol_gain(v);
}

bool hifi_vol_set(hifi_vol_t *v, int volume, int mute, int max_volume) {
	if (volume != -1 && (volume < 0 || volume > 100)) return false;
	if (max_volume != -1 && (max_volume < HIFI_VOL_MAX_MIN || max_volume > 100)) return false;
	if (max_volume != -1) {
		v->max_volume = max_volume;
		if (v->volume > max_volume || v->gain > hifi_vol_to_gain(max_volume)) {
			v->volume = max_volume;
			v->gain = hifi_vol_to_gain(max_volume);
		}
	}
	if (volume != -1) {
		v->volume = clampi(volume, 0, v->max_volume);
		v->gain = hifi_vol_to_gain(v->volume);
	}
	if (mute != -1) v->mute = mute != 0;
	return true;
}

void hifi_vol_step(hifi_vol_t *v, int dir) {
	hifi_vol_set(v, clampi(v->volume + (dir > 0 ? HIFI_VOL_STEP : -HIFI_VOL_STEP), 0, v->max_volume), 0, -1);
}

uint32_t hifi_vol_gain(const hifi_vol_t *v) { return v->mute ? 0 : v->gain; }

int hifi_vol_codec_code(const hifi_vol_t *v, int full_code) {
	uint32_t g = hifi_vol_gain(v);
	if (!g) return -1;
	double db = 20 * log10((double) g / HIFI_VOL_UNITY);
	return clampi(full_code + (int) lrint(db / 1.5), 0, full_code);
}
