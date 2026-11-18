#include "hifi_pair.h"

#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------ pairing mode */

void hifi_pair_init(hifi_pair_t *p) { memset(p, 0, sizeof(*p)); }

void hifi_pair_start(hifi_pair_t *p, uint32_t now_ms, int secs) {
	if (secs < 1) secs = 1;
	p->on = true;
	p->until_ms = now_ms + (uint32_t) secs * 1000u;
}

bool hifi_pair_stop(hifi_pair_t *p) {
	bool was = p->on;
	p->on = false;
	return was;
}

bool hifi_pair_expired(hifi_pair_t *p, uint32_t now_ms) {
	if (!p->on || (int32_t) (now_ms - p->until_ms) < 0) return false;
	p->on = false;
	return true;
}

int hifi_pair_left_s(const hifi_pair_t *p, uint32_t now_ms) {
	if (!p->on) return 0;
	int32_t left = (int32_t) (p->until_ms - now_ms);
	return left <= 0 ? 0 : (int) ((left + 999) / 1000);
}

/* ------------------------------------------------------------------ the button combo */

void hifi_combo_init(hifi_combo_t *c) { memset(c, 0, sizeof(*c)); }

bool hifi_combo_poll(hifi_combo_t *c, bool down_a, bool down_b, uint32_t now_ms) {
	if (!down_a && !down_b) {
		c->armed = true;
		c->both = c->fired = false;
		/* both up: the combo still owns the release events in flight for a moment */
		if (c->active) {
			if (!c->tail) {
				c->tail = true;
				c->up_ms = now_ms;
			} else if (now_ms - c->up_ms >= HIFI_COMBO_TAIL_MS) {
				c->active = c->tail = false;
			}
		}
		return false;
	}
	c->tail = false;
	if (!c->armed) return false;
	if (down_a && down_b) {
		if (!c->both) {
			c->both = true;
			c->since_ms = now_ms;
		}
		c->active = true;
		if (!c->fired && now_ms - c->since_ms >= HIFI_COMBO_HOLD_MS) {
			c->fired = true;
			return true;
		}
		return false;
	}
	/* one let go: this hold is over, but the combo lasts until both are up */
	c->both = false;
	return false;
}

bool hifi_combo_owns(const hifi_combo_t *c) { return c->active; }

/* ------------------------------------------------------------------ chimes */

typedef struct {
	float hz;
	uint16_t ms;
} note_t;

/* C6 E6 G6 up; G5 C6 up; C6 G5 down: short, bright, and below any squeal */
static const note_t PAIR[] = {{1046.5f, 110}, {1318.5f, 110}, {1568.0f, 220}};
static const note_t CONNECT[] = {{784.0f, 110}, {1046.5f, 220}};
static const note_t DISCONNECT[] = {{1046.5f, 110}, {784.0f, 220}};

static const note_t *notes_of(hifi_chime_kind_t k, uint8_t *n) {
	switch (k) {
	case HIFI_CHIME_PAIR: *n = 3; return PAIR;
	case HIFI_CHIME_CONNECT: *n = 2; return CONNECT;
	case HIFI_CHIME_DISCONNECT: *n = 2; return DISCONNECT;
	default: *n = 0; return NULL;
	}
}

const char *hifi_chime_name(hifi_chime_kind_t k) {
	switch (k) {
	case HIFI_CHIME_PAIR: return "pair";
	case HIFI_CHIME_CONNECT: return "connect";
	case HIFI_CHIME_DISCONNECT: return "disconnect";
	default: return "none";
	}
}

float hifi_chime_amp(uint32_t gain) {
	if (!gain) return 0;
	float g = gain / 65536.0f;
	float base = powf(10.0f, HIFI_CHIME_DBFS / 20.0f);
	float cap = powf(10.0f, HIFI_CHIME_CAP_DBFS / 20.0f);
	float amp = base * g > cap ? cap / g : base;
	return amp > 1.0f ? 1.0f : amp;
}

uint32_t hifi_chime_ms(hifi_chime_kind_t kind) {
	uint8_t n;
	const note_t *t = notes_of(kind, &n);
	uint32_t ms = 0;
	for (int i = 0; i < n; i++) ms += t[i].ms;
	return ms;
}

static void note_begin(hifi_chime_t *c) {
	uint8_t n;
	const note_t *t = notes_of(c->kind, &n);
	c->n = 0;
	c->len = (uint32_t) ((uint64_t) t[c->note].ms * c->rate / 1000);
	double w = 2.0 * M_PI * t[c->note].hz / c->rate;
	c->re = 1;
	c->im = 0;
	c->cr = (float) cos(w);
	c->ci = (float) sin(w);
}

bool hifi_chime_start(hifi_chime_t *c, hifi_chime_kind_t kind, float amp, unsigned rate) {
	memset(c, 0, sizeof(*c));
	uint8_t n;
	if (!notes_of(kind, &n) || !(amp > 0) || !rate) return false;
	c->kind = kind;
	c->rate = rate;
	c->amp = amp > 1 ? 1 : amp;
	c->notes = n;
	note_begin(c);
	return true;
}

size_t hifi_chime_render(hifi_chime_t *c, int16_t *out, size_t frames) {
	size_t i = 0;
	uint32_t attack = c->rate / 200; /* 5 ms */
	while (i < frames && c->note < c->notes) {
		if (c->n >= c->len) {
			if (++c->note >= c->notes) break;
			note_begin(c);
			continue;
		}
		/* a quick rise, then a fall to exactly 0 at the note's end: a soft
		 * bell, with no click at either end */
		float t = (float) c->n / c->len;
		float env = (c->n < attack ? (float) c->n / attack : 1.0f) * (1 - t) * (1 - t);
		float v = c->amp * env * c->im * 32767.0f;
		int16_t s = (int16_t) (v > 32767 ? 32767 : v < -32767 ? -32767 : v);
		out[2 * i] = out[2 * i + 1] = s;
		float re = c->re * c->cr - c->im * c->ci;
		c->im = c->re * c->ci + c->im * c->cr;
		c->re = re;
		if (!(c->n & 63)) { /* hold its length at 1 against float drift */
			float k = 1.5f - 0.5f * (c->re * c->re + c->im * c->im);
			c->re *= k;
			c->im *= k;
		}
		c->n++;
		i++;
	}
	return i;
}
