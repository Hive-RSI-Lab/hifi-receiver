#include <string.h>

#include "hifi_exp.h"

void hifi_exp_init(hifi_exp_t *e, const hifi_exp_cfg_t *cfg) {
	memset(e, 0, sizeof(*e));
	e->cfg = *cfg;
	if (e->cfg.edges_per_detent < 1) e->cfg.edges_per_detent = 4;
}

static bool bit(uint16_t pins, int n) { return n >= 0 && n < 16 && (pins >> n & 1); }

/* Quadrature steps from (old << 2) | new, A the high bit: 00 -> 01 -> 11 ->
 * 10 is +1 each. A jump of both bits is a missed edge and counts nothing. */
static const int8_t QUAD[16] = {0, 1, -1, 0, -1, 0, 0, 1, 1, 0, 0, -1, 0, -1, 1, 0};

int hifi_exp_input(hifi_exp_t *e, uint16_t pins, uint32_t now, hifi_exp_event_t *ev, int max) {
	const hifi_exp_cfg_t *c = &e->cfg;
	int n = 0;

	if (c->enc_a >= 0 && c->enc_b >= 0) {
		int ab = bit(pins, c->enc_a) << 1 | bit(pins, c->enc_b);
		if (!e->started) {
			e->ab = ab;
		} else if (ab != e->ab) {
			e->edges += QUAD[e->ab << 2 | ab];
			e->ab = ab;
			/* a detent is a rest position: both contacts open (11) and, for a
			 * knob with half a cycle a click, both closed (00) too. Count the
			 * click there, from at least half its edges, so a missed edge
			 * doesn't lose it or push the next one over */
			int per = c->edges_per_detent;
			bool rest = ab == 3 || (per <= 2 && ab == 0);
			if (rest) {
				int steps = e->edges >= 0 ? (e->edges + per / 2) / per : -((-e->edges + per / 2) / per);
				e->edges = 0;
				if (steps && n < max) ev[n++] = (hifi_exp_event_t) {.kind = HIFI_EXP_KNOB, .steps = steps};
			}
		}
	}

	for (int b = 0; b < HIFI_EXP_BUTTONS; b++) {
		if (c->buttons[b] < 0) continue;
		bool pressed = !bit(pins, c->buttons[b]); /* pulled up: 0 is pressed */
		if (!e->started) {
			e->raw[b] = e->down[b] = pressed;
			e->long_sent[b] = pressed; /* held at boot: no event for it */
			e->t_raw[b] = e->t_down[b] = now;
			continue;
		}
		if (pressed != e->raw[b]) {
			e->raw[b] = pressed;
			e->t_raw[b] = now;
		}
		if (e->raw[b] != e->down[b] && (uint32_t) (now - e->t_raw[b]) >= HIFI_EXP_DEBOUNCE_MS) {
			e->down[b] = e->raw[b];
			if (e->down[b]) {
				e->t_down[b] = now;
				e->long_sent[b] = false;
			} else if (!e->long_sent[b] && n < max) {
				ev[n++] = (hifi_exp_event_t) {.kind = HIFI_EXP_PRESS, .button = b};
			}
		}
		if (e->down[b] && !e->long_sent[b] && (uint32_t) (now - e->t_down[b]) >= HIFI_EXP_LONG_MS && n < max) {
			e->long_sent[b] = true;
			ev[n++] = (hifi_exp_event_t) {.kind = HIFI_EXP_LONG, .button = b};
		}
	}
	e->started = true;
	return n;
}
