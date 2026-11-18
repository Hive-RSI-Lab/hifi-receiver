#include "hifi_boost.h"

#include <math.h>
#include <string.h>

#define RAMP_S 0.05f  /* the boost moves to a new setting over ~50 ms */
#define SHOW_S 1.0f   /* the shown reduction decays over ~1 s */
#define LIMIT_DB -0.5f /* deeper than this counts as limiting */
#define HOLD_S 0.025f
#define QUIET_FS 1e-4f /* -80 dBFS: dropping this much audio is inaudible */
#define LA HIFI_BOOST_LA
#define XF (4 * LA)    /* the crossfade into the delayed path as it comes in */
#define SNAP 1e-6f     /* a request this close to 1 is 1 */
#define CEIL_HYST 0.02f /* ceiling moves smaller than this are ADC noise */

static void unprime(hifi_boost_t *b) {
	b->req = 1.0f;
	b->req_d = 0;
	b->hold = 0;
	b->t = 0;
	b->pos = 0;
	memset(b->delay, 0, sizeof(b->delay));
	for (int i = 0; i < LA; i++) b->box[i] = 1.0f;
	b->box_sum = LA;
	b->mq_head = b->mq_n = 0;
	b->quiet = 0;
	b->fill = 0;
	b->primed = false;
}

void hifi_boost_init(hifi_boost_t *b) {
	memset(b, 0, sizeof(*b));
	b->gain = 1.0f;
	b->ceiling = 1.0f;
	b->release_s = 0.15f;
	b->gr_min = b->gr_show = 1.0f;
	unprime(b);
}

void hifi_boost_bypass(hifi_boost_t *b) {
	if (b->primed) unprime(b);
	/* back in, the boost ramps up from unity again, not from where it was */
	b->gain = 1.0f;
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
	/* while it plays, the PVDD reading (8 ADC samples every 10 ms) jitters
	 * by about 1%: moves within 2% are noise, which clip_fs's 0.5 dB margin
	 * covers. Following them would pile up rescales below */
	if (b->primed && fabsf(ceiling - b->ceiling) <= CEIL_HYST * b->ceiling) return;
	/* a real drop (the supply sagged) rescales what is already judged, so
	 * the frames in the delay stay under the new ceiling: one gain step of
	 * the drop's size instead of clamped peaks */
	if (ceiling < b->ceiling && b->primed) {
		float k = ceiling / b->ceiling;
		b->req *= k;
		b->req_d = 1.0f - b->req;
		for (uint32_t i = 0; i < b->mq_n; i++) b->mq_v[(b->mq_head + i) % (LA + 1)] *= k;
		for (int i = 0; i < LA; i++) b->box[i] *= k;
		b->box_sum *= k;
	}
	b->ceiling = ceiling;
}

bool hifi_boost_active(const hifi_boost_t *b) { return b->db > 0 || b->gain != 1.0f || b->primed; }

/* One frame in (l, r: boosted, FS), the frame LA back out, limited. */
static inline void frame(hifi_boost_t *b, float *l, float *r, float rel, uint32_t hold, float *gmin) {
	float al = fabsf(*l), ar = fabsf(*r);
	float pk = al > ar ? al : ar;
	b->quiet = pk < QUIET_FS ? b->quiet + 1 : 0;

	/* the request: the gain that keeps this frame under the ceiling, held
	 * and then released toward 1 (never above what this frame needs) */
	float need = pk > b->ceiling ? b->ceiling / pk : 1.0f;
	if (need <= b->req) {
		b->req = need;
		b->req_d = 1.0f - need;
		b->hold = hold;
	} else if (b->hold) {
		b->hold--;
	} else {
		/* released as the distance from 1, which keeps shrinking where
		 * 1 - d would round back to the same float short of unity */
		float d = b->req_d * rel;
		if (d < SNAP) d = 0;
		float up = 1.0f - d;
		if (need < up) {
			b->req = need;
			b->req_d = 1.0f - need;
		} else {
			b->req = up;
			b->req_d = d;
		}
	}

	/* the minimum request over the last LA + 1 frames */
	uint32_t t = b->t++;
	if (b->mq_n && t - b->mq_t[b->mq_head] > LA) { /* left the window [t - LA, t] */
		b->mq_head = (b->mq_head + 1) % (LA + 1);
		b->mq_n--;
	}
	while (b->mq_n && b->mq_v[(b->mq_head + b->mq_n - 1) % (LA + 1)] >= b->req) b->mq_n--;
	uint32_t at = (b->mq_head + b->mq_n) % (LA + 1);
	b->mq_v[at] = b->req;
	b->mq_t[at] = t;
	b->mq_n++;
	float m = b->mq_v[b->mq_head];

	/* averaged over the last LA minimums: each covers the frame leaving
	 * now, so the average is at or below its own request */
	uint32_t p = b->pos;
	b->box_sum += m - b->box[p];
	b->box[p] = m;
	float g = b->box_sum * (1.0f / LA);
	if (g > 1.0f) g = 1.0f;
	if (g < *gmin) *gmin = g;

	float ol = b->delay[p][0] * g, or_ = b->delay[p][1] * g;
	if (b->fill < LA + XF) {
		/* coming in: the live frame until the delay is full, then a
		 * crossfade to the delayed one, instead of 1 ms of silence. The
		 * live frame takes this frame's own request (no lookahead, but at
		 * or below what it needs), so both sides and their blend stay
		 * under the ceiling */
		float a = b->fill < LA ? 0.0f : (float) (b->fill - LA) / XF;
		float ll = *l * b->req, lr = *r * b->req;
		ol = ll + (ol - ll) * a;
		or_ = lr + (or_ - lr) * a;
		b->fill++;
	}
	b->delay[p][0] = *l;
	b->delay[p][1] = *r;
	b->pos = p + 1 == LA ? 0 : p + 1;
	/* the gain already keeps it under; this only catches float rounding */
	float c = b->ceiling;
	*l = ol > c ? c : ol < -c ? -c : ol;
	*r = or_ > c ? c : or_ < -c ? -c : or_;
}

void hifi_boost_process(hifi_boost_t *b, void *buf, size_t frames, int bytes_per_frame, unsigned rate) {
	if (!frames || !rate || !hifi_boost_active(b)) return;
	b->primed = true;
	float dt = (float) frames / rate;
	float want = powf(10.0f, b->db / 20.0f);
	float g0 = b->gain;
	float g1 = g0 + (want - g0) * (1.0f - expf(-dt / RAMP_S));
	if (fabsf(g1 - want) < 1e-3f * want) g1 = want;
	float step = (g1 - g0) / frames;
	float rel = expf(-1.0f / (b->release_s * rate));
	float gmin = 1.0f;
	uint32_t hold = (uint32_t) (HOLD_S * rate);
	/* the running sum drifts in float over hours: add it up afresh */
	float sum = 0;
	for (int i = 0; i < LA; i++) sum += b->box[i];
	b->box_sum = sum;

	if (bytes_per_frame == 8) {
		int32_t *s = buf;
		const float k = 1.0f / 2147483648.0f;
		for (size_t i = 0; i < frames; i++) {
			float bg = g0 + step * (i + 1);
			float l = s[2 * i] * k * bg, r = s[2 * i + 1] * k * bg;
			frame(b, &l, &r, rel, hold, &gmin);
			/* the ceiling is <= 1; this keeps l * 2^31 within int32 (float,
			 * not double: the ESP32 has no double FPU; 0.99999994 * 2^31 is
			 * 2^31 - 128) */
			l = l > 0.99999994f ? 0.99999994f : l;
			r = r > 0.99999994f ? 0.99999994f : r;
			s[2 * i] = (int32_t) (l * 2147483648.0f);
			s[2 * i + 1] = (int32_t) (r * 2147483648.0f);
		}
	} else {
		int16_t *s = buf;
		const float k = 1.0f / 32768.0f;
		for (size_t i = 0; i < frames; i++) {
			float bg = g0 + step * (i + 1);
			float l = s[2 * i] * k * bg, r = s[2 * i + 1] * k * bg;
			frame(b, &l, &r, rel, hold, &gmin);
			long L = lrintf(l * 32768.0f), R = lrintf(r * 32768.0f);
			s[2 * i] = (int16_t) (L > 32767 ? 32767 : L < -32768 ? -32768 : L);
			s[2 * i + 1] = (int16_t) (R > 32767 ? 32767 : R < -32768 ? -32768 : R);
		}
	}
	b->gain = g1;
	/* Boost is off again: the delay leaves once what it holds is silent */
	if (g1 == 1.0f && b->db <= 0 && b->quiet >= LA) unprime(b);

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
