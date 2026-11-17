#include "hifi_chan.h"

#define SRC_STREAM_GAIN 0x38 /* squeezelite's own gain bits, as it sets them */
#define DAC_VOL_M6DB 12      /* 0.5 dB a step: mono's sum of two sides, halved */

void hifi_chan_regs(hifi_chan_t c, bool direct, int direct_lin_code, bool live, hifi_chan_regs_t *r) {
	/* which source side reaches which mixer: [LD2LO, RD2LO, LD2RO, RD2RO] */
	bool pick[4] = {true, false, false, true}; /* stereo: left to left, right to right */
	switch (c) {
	case HIFI_CHAN_LEFT: pick[1] = false; pick[2] = true; pick[3] = false; break;
	case HIFI_CHAN_RIGHT: pick[0] = false; pick[1] = true; break;
	case HIFI_CHAN_MONO: pick[1] = pick[2] = true; break;
	default: break;
	}
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
