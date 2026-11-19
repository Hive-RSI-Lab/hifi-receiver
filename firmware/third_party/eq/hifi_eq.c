/*
 * hifi-receiver's own 10-band equalizer (third_party/eq/README.md): one
 * peaking biquad per band (Robert Bristow-Johnson's Audio EQ Cookbook), an
 * octave wide, run in cascade on interleaved signed 16-bit samples in place.
 * A band at 0 dB is skipped, and so is one at or above 0.45 of the sample
 * rate (16 kHz at 22050 Hz). Coefficients are worked out again when a gain or
 * the sample rate changes.
 *
 * Copyright (c) 2026 Hive-RSI-Lab. MIT licence (third_party/eq/README.md).
 */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "esp_equalizer.h"

#define EQ_MAX_BANDS 10
#define EQ_MAX_CH 2
#define EQ_Q 1.41421356f /* one octave between the -3 dB points */

static const float eq_freq[EQ_MAX_BANDS] = { 31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };

typedef struct {
	float b0, b1, b2, a1, a2; /* normalised by a0 */
	float x1, x2, y1, y2;
	int on;
} eq_biquad;

typedef struct {
	int nch, rate, n_band, dirty;
	float gain[EQ_MAX_CH][EQ_MAX_BANDS];
	eq_biquad bq[EQ_MAX_CH][EQ_MAX_BANDS];
} eq_t;

static void eq_design(eq_t *e) {
	for (int c = 0; c < e->nch; c++)
		for (int i = 0; i < e->n_band; i++) {
			eq_biquad *b = &e->bq[c][i];
			float g = e->gain[c][i];
			int on = g != 0 && eq_freq[i] < 0.45f * e->rate;
			if (!on) {
				b->on = 0;
				continue;
			}
			float A = powf(10.0f, g / 40.0f);
			float w = 2.0f * (float)M_PI * eq_freq[i] / e->rate;
			float alpha = sinf(w) / (2.0f * EQ_Q), cw = cosf(w);
			float a0 = 1.0f + alpha / A;
			b->b0 = (1.0f + alpha * A) / a0;
			b->b1 = -2.0f * cw / a0;
			b->b2 = (1.0f - alpha * A) / a0;
			b->a1 = b->b1;
			b->a2 = (1.0f - alpha / A) / a0;
			if (!b->on) b->x1 = b->x2 = b->y1 = b->y2 = 0; /* a band coming on starts quiet */
			b->on = 1;
		}
	e->dirty = 0;
}

void *esp_equalizer_init(int nch, int g_rate, int n_band, int use_xmms_original_freqs) {
	(void)use_xmms_original_freqs;
	if (nch < 1 || nch > EQ_MAX_CH || g_rate <= 0 || n_band < 1 || n_band > EQ_MAX_BANDS) return NULL;
	eq_t *e = calloc(1, sizeof(eq_t));
	if (!e) return NULL;
	e->nch = nch;
	e->rate = g_rate;
	e->n_band = n_band;
	e->dirty = 1;
	return e;
}

void esp_equalizer_uninit(void *handle) {
	free(handle);
}

void esp_equalizer_set_band_value(void *handle, float value, int index, int nch) {
	eq_t *e = handle;
	if (!e || index < 0 || index >= e->n_band || nch < 0 || nch >= e->nch) return;
	if (value > 20) value = 20;
	if (value < -20) value = -20;
	if (e->gain[nch][index] != value) {
		e->gain[nch][index] = value;
		e->dirty = 1;
	}
}

int esp_equalizer_get_band_count(void *handle) {
	return handle ? ((eq_t *)handle)->n_band : 0;
}

float esp_equalizer_get_band_value(void *handle, int index, int nch) {
	eq_t *e = handle;
	if (!e || index < 0 || index >= e->n_band || nch < 0 || nch >= e->nch) return 0;
	return e->gain[nch][index];
}

int esp_equalizer_process(void *handle, unsigned char *pcm_buf, int length, int g_rate, int nch) {
	eq_t *e = handle;
	if (!e || !pcm_buf || length < 0 || nch != e->nch || g_rate <= 0) return -1;
	if (g_rate != e->rate) {
		e->rate = g_rate;
		e->dirty = 1;
	}
	if (e->dirty) eq_design(e);
	int16_t *s = (int16_t *)pcm_buf;
	int frames = length / (2 * nch);
	for (int c = 0; c < nch; c++) {
		eq_biquad *on[EQ_MAX_BANDS];
		int k = 0;
		for (int i = 0; i < e->n_band; i++)
			if (e->bq[c][i].on) on[k++] = &e->bq[c][i];
		if (!k) continue;
		/* the bands run in cascade in float and the sample is rounded and
		 * clipped to 16 bits once, after the last */
		int16_t *p = s + c;
		for (int n = 0; n < frames; n++, p += nch) {
			float x = *p;
			for (int j = 0; j < k; j++) {
				eq_biquad *b = on[j];
				float y = b->b0 * x + b->b1 * b->x1 + b->b2 * b->x2 - b->a1 * b->y1 - b->a2 * b->y2;
				b->x2 = b->x1; b->x1 = x; b->y2 = b->y1; b->y1 = y;
				x = y;
			}
			*p = x > 32767.0f ? 32767 : x < -32768.0f ? -32768 : (int16_t)lrintf(x);
		}
	}
	return 0;
}
