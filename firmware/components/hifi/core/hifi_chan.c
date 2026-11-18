#include <math.h>
#include <stdio.h>

#include "hifi_chan.h"

#define SRC_STREAM_GAIN 0x38 /* squeezelite's own gain bits, as it sets them */

/* which source side reaches which output: [L to L, R to L, L to R, R to R] */
static void chan_pick(hifi_chan_t c, bool pick[4]) {
	pick[0] = pick[3] = true; /* stereo: left to left, right to right */
	pick[1] = pick[2] = false;
	switch (c) {
	case HIFI_CHAN_LEFT: pick[1] = false; pick[2] = true; pick[3] = false; break;
	case HIFI_CHAN_RIGHT: pick[0] = false; pick[1] = true; break;
	case HIFI_CHAN_MONO: pick[1] = pick[2] = true; break;
	default: break;
	}
}

void hifi_chan_regs(hifi_chan_t c, bool direct, int direct_lin_code, bool live, hifi_chan_regs_t *r) {
	/* only the same-side paths: the cross ones (0x28, 0x29) do nothing on
	 * the ES8388. The DAC path is always stereo (hifi_chan_mix does the
	 * channel); DJ direct can only leave a side out */
	bool pick[4];
	chan_pick(direct ? c : HIFI_CHAN_STEREO, pick);
	pick[1] = pick[2] = false;
	for (int i = 0; i < 4; i++) {
		uint8_t gain = direct ? ((uint8_t) (direct_lin_code << 3)) : SRC_STREAM_GAIN;
		r->mix[i] = gain | (pick[i] && live ? (direct ? 0x40 : 0x80) : 0);
	}
	r->dac_vol = 0;
}

void hifi_chan_mix(hifi_chan_t c, void *buf, size_t frames, int bytes_per_frame) {
	if (c != HIFI_CHAN_LEFT && c != HIFI_CHAN_RIGHT && c != HIFI_CHAN_MONO) return;
	if (bytes_per_frame == 8) {
		int32_t *s = buf;
		for (size_t i = 0; i < frames; i++, s += 2) {
			int32_t v = c == HIFI_CHAN_LEFT ? s[0] : c == HIFI_CHAN_RIGHT ? s[1] : (int32_t) (((int64_t) s[0] + s[1]) / 2);
			s[0] = s[1] = v;
		}
	} else {
		int16_t *s = buf;
		for (size_t i = 0; i < frames; i++, s += 2) {
			int16_t v = c == HIFI_CHAN_LEFT ? s[0] : c == HIFI_CHAN_RIGHT ? s[1] : (int16_t) (((int32_t) s[0] + s[1]) / 2);
			s[0] = s[1] = v;
		}
	}
}

void hifi_chan_gain(hifi_chan_t c, int side, float g[2]) {
	bool pick[4];
	chan_pick(c, pick);
	float k = c == HIFI_CHAN_MONO ? 0.5f : 1.0f; /* hifi_chan_mix's (L + R) / 2 */
	g[0] = pick[side ? 1 : 0] ? k : 0;
	g[1] = pick[side ? 3 : 2] ? k : 0;
}

bool hifi_loop_judge(hifi_chan_t c, const float got[2][2], float want_fs, float tol_db, char *out, size_t n) {
	bool ok = want_fs > 0;
	float db[2][2];
	for (int side = 0; side < 2; side++) {
		float g[2];
		hifi_chan_gain(c, side, g);
		for (int leg = 0; leg < 2; leg++) {
			float r = want_fs > 0 && got[side][leg] > 0 ? 20 * log10f(got[side][leg] / want_fs) : -99;
			db[side][leg] = r < -99 ? -99 : r;
			if (g[leg] > 0) ok &= fabsf(db[side][leg] - 20 * log10f(g[leg])) <= tol_db;
			else ok &= db[side][leg] <= -HIFI_LOOP_OFF_DB;
		}
	}
	snprintf(out, n, "%s: L %+.1f/%+.1f, R %+.1f/%+.1f dB", hifi_chan_name(c), db[0][0], db[0][1], db[1][0], db[1][1]);
	return ok;
}
