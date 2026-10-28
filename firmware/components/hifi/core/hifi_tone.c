#include "hifi_tone.h"

#include <math.h>
#include <string.h>

#define TWO_PI 6.283185307179586

const char *hifi_tone_type_name(hifi_tone_type_t t) {
	switch (t) {
	case HIFI_TONE_SINE: return "sine";
	case HIFI_TONE_SWEEP: return "sweep";
	case HIFI_TONE_NOISE: return "noise";
	case HIFI_TONE_SILENCE: return "silence";
	default: return "none";
	}
}

hifi_tone_type_t hifi_tone_type_from(const char *s) {
	if (!strcmp(s, "sine")) return HIFI_TONE_SINE;
	if (!strcmp(s, "sweep")) return HIFI_TONE_SWEEP;
	if (!strcmp(s, "noise")) return HIFI_TONE_NOISE;
	if (!strcmp(s, "silence")) return HIFI_TONE_SILENCE;
	return HIFI_TONE_NONE;
}

const char *hifi_channel_name(hifi_channel_t c) {
	return c == HIFI_CH_LEFT ? "left" : c == HIFI_CH_RIGHT ? "right" : "both";
}

const char *hifi_tone_check(const hifi_tone_params_t *p, bool force) {
	if (p->type == HIFI_TONE_NONE) return "type must be sine, sweep, noise, silence or stop";
	if (!(p->duration_s >= 0.1f && p->duration_s <= 600.0f)) return "duration_s must be 0.1-600";
	if (p->type == HIFI_TONE_SILENCE) return NULL;
	if (!(p->level_dbfs >= -90.0f && p->level_dbfs <= 0.0f)) return "level_dbfs must be -90 to 0";
	if (p->level_dbfs > -6.0f && !force) return "level_dbfs above -6 needs \"force\": true";
	if (p->type == HIFI_TONE_SINE && !(p->freq_hz >= 10.0f && p->freq_hz <= 24000.0f))
		return "freq_hz must be 10-24000";
	if (p->type == HIFI_TONE_SWEEP) {
		if (!(p->start_hz >= 10.0f && p->start_hz <= 24000.0f && p->end_hz >= 10.0f && p->end_hz <= 24000.0f))
			return "start_hz and end_hz must be 10-24000";
		if (p->start_hz == p->end_hz) return "start_hz and end_hz must differ";
	}
	return NULL;
}

static uint32_t xorshift(uint32_t *s) {
	uint32_t x = *s;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return *s = x;
}

/* uniform in [-1, 1) */
static float white(uint32_t *s) { return (float) ((int32_t) xorshift(s)) / 2147483648.0f; }

/* Paul Kellet's refined pink filter */
static float pink(hifi_tone_t *t) {
	float w = white(&t->rng), *b = t->b;
	b[0] = 0.99886f * b[0] + w * 0.0555179f;
	b[1] = 0.99332f * b[1] + w * 0.0750759f;
	b[2] = 0.96900f * b[2] + w * 0.1538520f;
	b[3] = 0.86650f * b[3] + w * 0.3104856f;
	b[4] = 0.55000f * b[4] + w * 0.5329522f;
	b[5] = -0.7616f * b[5] - w * 0.0168980f;
	float out = b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + w * 0.5362f;
	b[6] = w * 0.115926f;
	return out;
}

void hifi_tone_start(hifi_tone_t *t, const hifi_tone_params_t *p, unsigned rate) {
	memset(t, 0, sizeof(*t));
	t->p = *p;
	t->rate = rate;
	t->total = (uint64_t) (p->duration_s * rate + 0.5f);
	t->fade = rate / 100;
	if (t->fade * 2 > t->total) t->fade = t->total / 2;
	t->amp = p->type == HIFI_TONE_SILENCE ? 0.0f : powf(10.0f, p->level_dbfs / 20.0f);
	t->rng = 0x9e3779b9u;
	if (p->type == HIFI_TONE_SWEEP) t->k_sweep = log((double) p->end_hz / p->start_hz) / p->duration_s;
	if (p->type == HIFI_TONE_NOISE) {
		/* measure the filter's RMS on a copy, then scale to a sine's RMS */
		hifi_tone_t probe = *t;
		double acc = 0;
		for (int i = 0; i < 2 * 48000; i++) {
			float v = pink(&probe);
			if (i >= 48000) acc += (double) v * v; /* after the filter settles */
		}
		float rms = (float) sqrt(acc / 48000);
		t->pink_scale = rms > 0 ? 0.70710678f / rms : 0.0f;
		/* warm the real filter the same way so it starts settled */
		for (int i = 0; i < 48000; i++) pink(t);
	}
}

bool hifi_tone_done(const hifi_tone_t *t) { return t->n >= t->total; }

void hifi_tone_stop(hifi_tone_t *t) {
	if (t->n + t->fade < t->total) t->total = t->n + t->fade;
}

static int16_t to_s16(float v, uint32_t *rng) {
	/* TPDF dither, 1 LSB peak each */
	float d = (white(rng) + white(rng)) * 0.5f;
	float s = v * 32767.0f + d;
	if (s > 32767.0f) s = 32767.0f;
	if (s < -32768.0f) s = -32768.0f;
	return (int16_t) lrintf(s);
}

size_t hifi_tone_render(hifi_tone_t *t, int16_t *out, size_t frames) {
	size_t i;
	for (i = 0; i < frames && t->n < t->total; i++, t->n++) {
		float v;
		switch (t->p.type) {
		case HIFI_TONE_SINE:
			v = (float) sin(t->phase);
			t->phase += TWO_PI * t->p.freq_hz / t->rate;
			break;
		case HIFI_TONE_SWEEP: {
			/* exponential (Farina) sweep: f(t) = f0 * exp(k t) */
			double f = t->p.start_hz * exp(t->k_sweep * ((double) t->n / t->rate));
			v = (float) sin(t->phase);
			t->phase += TWO_PI * f / t->rate;
			break;
		}
		case HIFI_TONE_NOISE:
			v = pink(t) * t->pink_scale;
			break;
		default:
			v = 0.0f;
			break;
		}
		if (t->phase > TWO_PI) t->phase -= TWO_PI;

		float env = 1.0f;
		uint64_t left = t->total - t->n - 1;
		if (t->fade) {
			if (t->n < t->fade) env = 0.5f - 0.5f * cosf((float) (M_PI * t->n / t->fade));
			else if (left < t->fade) env = 0.5f - 0.5f * cosf((float) (M_PI * left / t->fade));
		}
		v *= t->amp * env;

		if (t->p.type == HIFI_TONE_SILENCE) {
			out[2 * i] = out[2 * i + 1] = 0;
		} else {
			out[2 * i] = t->p.channel == HIFI_CH_RIGHT ? 0 : to_s16(v, &t->rng);
			out[2 * i + 1] = t->p.channel == HIFI_CH_LEFT ? 0 : to_s16(v, &t->rng);
		}
	}
	return i;
}
