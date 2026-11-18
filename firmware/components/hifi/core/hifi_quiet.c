#include "hifi_quiet.h"

#include <math.h>

void hifi_quiet_run(const hifi_quiet_ops_t *q) {
	if (q->level) {
		for (int i = 1; i <= HIFI_QUIET_STEPS; i++) {
			q->level(q->ctx, 1.0f - (float) i / HIFI_QUIET_STEPS);
			q->sleep_ms(q->ctx, HIFI_QUIET_RAMP_MS / HIFI_QUIET_STEPS);
		}
	}
	if (q->mute) {
		q->mute(q->ctx);
		q->sleep_ms(q->ctx, HIFI_QUIET_MUTE_MS);
	}
	q->amp_off(q->ctx);
	q->sleep_ms(q->ctx, HIFI_QUIET_SETTLE_MS);
}

int hifi_quiet_code(int start, float frac) {
	if (start <= 0 || !(frac > 0)) return 0;
	if (frac >= 1) return start;
	return (int) lroundf(start * frac);
}

bool hifi_quiet_amp_allowed(bool codec_up, uint32_t hold_until, bool quiet, uint32_t now_ms) {
	return codec_up && !quiet && (int32_t) (hold_until - now_ms) <= 0;
}
