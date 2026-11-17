#include "hifi_boost.h"

#include <math.h>
#include <string.h>

#define RAMP_S 0.05f  /* the boost moves to a new setting over ~50 ms */
#define SHOW_S 1.0f   /* the shown reduction decays over ~1 s */
#define LIMIT_DB -0.5f /* deeper than this counts as limiting */
#define HOLD_S 0.025f

void hifi_boost_init(hifi_boost_t *b) {
	memset(b, 0, sizeof(*b));
	b->gain = 1.0f;
	b->ceiling = 1.0f;
	b->release_s = 0.15f;
	b->gr_min = b->gr_show = 1.0f;
}

float hifi_boost_set_db(hifi_boost_t *b, float db) {
	if (!(db > 0)) db = 0; /* NaN too */
	if (db > HIFI_BOOST_MAX_DB) db = HIFI_BOOST_MAX_DB;
	b->db = roundf(db * 10.0f) / 10.0f;
	return b->db;
}

void hifi_boost_set_ceiling(hifi_boost_t *b, float ceiling) {
	if (!(ceiling > 0.05f)) ceiling = 0.05f;
	if (ceiling > 1.0f) ceiling = 1.0f;
	b->ceiling = ceiling;
}

bool hifi_boost_active(const hifi_boost_t *b) { return b->db > 0 || b->gain != 1.0f; }

static inline float lim(hifi_boost_t *b, float pk, float rel, uint32_t hold, float *gmin) {
	/* instant attack: the envelope is never below this sample's peak */
	if (pk >= b->env) {
		b->env = pk;
		b->hold = hold;
	} else if (b->hold) {
		b->hold--;
	} else {
		float e = b->env * rel;
		b->env = pk > e ? pk : e;
	}
	if (b->env <= b->ceiling) return 1.0f;
	float g = b->ceiling / b->env;
	if (g < *gmin) *gmin = g;
	return g;
}

void hifi_boost_process(hifi_boost_t *b, void *buf, size_t frames, int bytes_per_frame, unsigned rate) {
	if (!frames || !rate || !hifi_boost_active(b)) return;
	float dt = (float) frames / rate;
	float want = powf(10.0f, b->db / 20.0f);
	float g0 = b->gain;
	float g1 = g0 + (want - g0) * (1.0f - expf(-dt / RAMP_S));
	if (fabsf(g1 - want) < 1e-3f * want) g1 = want;
	float step = (g1 - g0) / frames;
	float rel = expf(-1.0f / (b->release_s * rate));
	float gmin = 1.0f;
	uint32_t hold = (uint32_t) (HOLD_S * rate);

	if (bytes_per_frame == 8) {
		int32_t *s = buf;
		const float k = 1.0f / 2147483648.0f;
		for (size_t i = 0; i < frames; i++) {
			float bg = g0 + step * (i + 1);
			float l = s[2 * i] * k * bg, r = s[2 * i + 1] * k * bg;
			float al = fabsf(l), ar = fabsf(r);
			float g = lim(b, al > ar ? al : ar, rel, hold, &gmin);
			l *= g;
			r *= g;
			/* the ceiling is <= 1, so this only catches float rounding */
			l = l > 0.99999994f ? 0.99999994f : l < -1.0f ? -1.0f : l;
			r = r > 0.99999994f ? 0.99999994f : r < -1.0f ? -1.0f : r;
			s[2 * i] = (int32_t) ((double) l * 2147483648.0);
			s[2 * i + 1] = (int32_t) ((double) r * 2147483648.0);
		}
	} else {
		int16_t *s = buf;
		const float k = 1.0f / 32768.0f;
		for (size_t i = 0; i < frames; i++) {
			float bg = g0 + step * (i + 1);
			float l = s[2 * i] * k * bg, r = s[2 * i + 1] * k * bg;
			float al = fabsf(l), ar = fabsf(r);
			float g = lim(b, al > ar ? al : ar, rel, hold, &gmin);
			long L = lrintf(l * g * 32768.0f), R = lrintf(r * g * 32768.0f);
			s[2 * i] = (int16_t) (L > 32767 ? 32767 : L < -32768 ? -32768 : L);
			s[2 * i + 1] = (int16_t) (R > 32767 ? 32767 : R < -32768 ? -32768 : R);
		}
	}
	b->gain = g1;
	if (g1 == 1.0f && b->db <= 0) b->env = 0; /* off again: start clean next time */

	b->gr_min = gmin;
	float shown = b->gr_show + (1.0f - b->gr_show) * (1.0f - expf(-dt / SHOW_S));
	b->gr_show = gmin < shown ? gmin : shown;
	/* on the held figure, so a limiter that works on every beat is one event */
	bool limiting = 20.0f * log10f(b->gr_show) < LIMIT_DB;
	if (limiting && !b->limiting) b->events++;
	b->limiting = limiting;
}

float hifi_boost_reduction_db(const hifi_boost_t *b) {
	if (!(b->gr_show > 0)) return 0;
	float db = 20.0f * log10f(b->gr_show);
	return db > -0.05f ? 0.0f : db;
}
