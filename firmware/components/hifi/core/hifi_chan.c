#include <math.h>
#include <stdio.h>

#include "hifi_chan.h"

#define SRC_STREAM_GAIN 0x38 /* squeezelite's own gain bits, as it sets them */
#define DAC_VOL_M6DB 12      /* 0.5 dB a step: mono's sum of two sides, halved */

/* which source side reaches which mixer: [LD2LO, RD2LO, LD2RO, RD2RO] */
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
	bool pick[4];
	chan_pick(c, pick);
	/* mono sums two sides into each mixer: the analog path is 6 dB down per
	 * side (3 dB a code), the DAC path through its digital volume */
	int code = direct_lin_code + (c == HIFI_CHAN_MONO && direct ? 2 : 0);
	if (code > 7) code = 7;
	for (int i = 0; i < 4; i++) {
		if (!pick[i] || !live) r->mix[i] = direct ? ((uint8_t) (code << 3)) : SRC_STREAM_GAIN;
		else r->mix[i] = (direct ? 0x40 : 0x80) | (direct ? ((uint8_t) (code << 3)) : SRC_STREAM_GAIN);
	}
	/* a stream with the DAC halved only in mono; DJ direct leaves it alone */
	r->dac_vol = c == HIFI_CHAN_MONO && !direct ? DAC_VOL_M6DB : 0;
}

void hifi_chan_gain(hifi_chan_t c, int side, float g[2]) {
	bool pick[4];
	chan_pick(c, pick);
	float k = c == HIFI_CHAN_MONO ? 0.5f : 1.0f; /* the DAC's -6 dB */
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
