/* Party mode: one board's clocks. See hifi_party_sync.h. */
#include "hifi_party_sync.h"

#include <math.h>
#include <string.h>

/* the fits' settings: one window a second, 30 s of them, a line from 4; a
 * TSF window 100 µs off the line (dropped from it), or a DAC window 500 µs late,
 * is off the line */
static void dac_reset(hifi_party_sync_t *s) {
	hifi_party_fit_init(&s->dac, 1000000, 30, 4, 500);
	s->dac_bad = 0;
}

static void tsf_reset(hifi_party_sync_t *s) {
	hifi_party_fit_init(&s->tsf, 1000000, 30, 4, 100);
	s->tsf.both_sides = true; /* a TSF steps back as well as forward: keep a step out of the line until the reset */
	s->tsf_bad = 0;
	s->tsf_have_prev = s->tsf_step_open = false;
	s->clk_ok = false; /* it was in the old TSF */
}

void hifi_party_sync_init(hifi_party_sync_t *s, int buf_frames) {
	memset(s, 0, sizeof(*s));
	s->buf_frames = buf_frames;
	dac_reset(s);
	tsf_reset(s);
}

void hifi_party_sync_restart(hifi_party_sync_t *s, int buf_frames) {
	s->buf_frames = buf_frames;
	s->dac_resets++;
	dac_reset(s);
}

/* the newest window, as fit_add just closed it, against the line */
static double newest_off(const hifi_party_fit_t *f) {
	int last = (f->head + f->cap - 1) % f->cap;
	return (double) f->py[last] - hifi_party_fit_y(f, (double) f->px[last]);
}

void hifi_party_sync_tsf(hifi_party_sync_t *s, int64_t a_us, int64_t b_us, int64_t tsf, const uint8_t bssid[6]) {
	if (tsf <= 0) {
		s->tsf_zero++;
		s->have_bssid = false; /* not associated: whichever AP comes next is checked afresh */
		if (s->tsf.n || s->tsf_have_prev) {
			/* nor is its TSF the old one's: a fit built while beacons were
			 * missed, the TSF stepped, would play off when they come back */
			s->tsf_resets++;
			tsf_reset(s);
		}
		return;
	}
	if (bssid) {
		if (s->have_bssid && memcmp(s->bssid, bssid, 6)) {
			/* a roam: another AP's TSF */
			s->bssid_changes++;
			s->tsf_resets++;
			tsf_reset(s);
		}
		memcpy(s->bssid, bssid, 6);
		s->have_bssid = true;
	}
	hifi_party_fit_t *f = &s->tsf;
	if (!hifi_party_fit_add(f, (a_us + b_us) / 2, tsf, (double) (b_us - a_us), b_us)) return;
	int last = (f->head + f->cap - 1) % f->cap;
	int64_t x = f->px[last], y = f->py[last];
	/* a jump from the window before: the TSF and esp_timer run within
	 * HIFI_PARTY_SYNC_PPM of each other, so between two windows the TSF moves
	 * as far as the local clock give or take that. Unlike the distance from
	 * the line, a young line that took the step in can't hide it (nor bend
	 * the test, as its slope would) */
	bool jump = false;
	if (s->tsf_have_prev) {
		double dx = (double) (x - s->tsf_prev_x), d = (double) (y - s->tsf_prev_y) - dx;
		jump = fabs(d) > f->reject + fabs(dx) * HIFI_PARTY_SYNC_PPM * 1e-6;
	}
	bool off = f->ok && fabs(newest_off(f)) > f->reject;
	bool reset = false;
	if (jump && !f->ok)
		reset = true; /* no line to keep the step out of: start in the new TSF */
	else if (jump && off) {
		s->tsf_step_open = true; /* the TSF stepped; the line kept it out */
		reset = ++s->tsf_bad >= HIFI_PARTY_SYNC_BAD;
	} else if (jump) {
		if (s->tsf_step_open) {
			s->tsf_step_open = false; /* back onto the line: the step is over */
			s->tsf_bad = 0;
		} else
			reset = true; /* the line took the step in */
	} else if (off)
		reset = ++s->tsf_bad >= HIFI_PARTY_SYNC_BAD; /* the TSF stepped; three windows running */
	else if (s->tsf_step_open)
		reset = true; /* still stepped, but the line has moved onto it */
	else
		s->tsf_bad = 0;
	if (reset) {
		s->tsf_resets++;
		tsf_reset(s);
	} else if (off)
		hifi_party_fit_drop_newest(f); /* keep a stepped or drifting window out, or a few of them bend the line onto it */
	s->tsf_prev_x = x;
	s->tsf_prev_y = y;
	s->tsf_have_prev = true;
}

void hifi_party_sync_wrote(hifi_party_sync_t *s, int64_t w0, int64_t w1, int64_t t0, int64_t t1, int last_block) {
	if (last_block && w1 - w0 < last_block) {
		/* the DMA took less than the block: where the rest goes is unknown */
		s->short_writes++;
		s->dac_resets++;
		dac_reset(s);
		return;
	}
	int64_t x;
	if (s->buf_frames > 0 && hifi_party_dac_sample(w0, w1, t0, t1, s->buf_frames, 0, &x)) {
		double q = (double) t1 - (double) x * 1e6 / HIFI_PARTY_SYNC_RATE;
		hifi_party_fit_t *f = &s->dac;
		if (hifi_party_fit_add(f, x, t1, q, t1)) {
			/* the newest window far above the line three times running: the
			 * DMA ran dry and every frame plays later now; start the fit again */
			if (f->ok && newest_off(f) > f->reject) {
				if (++s->dac_bad >= HIFI_PARTY_SYNC_BAD) {
					s->dac_resets++;
					dac_reset(s);
				}
			} else
				s->dac_bad = 0;
		}
	}
}

bool hifi_party_sync_ok(const hifi_party_sync_t *s) { return s->tsf.ok && s->dac.ok; }

double hifi_party_sync_tsf_at(const hifi_party_sync_t *s, int64_t w) {
	return hifi_party_fit_y(&s->tsf, hifi_party_fit_y(&s->dac, (double) w));
}

bool hifi_party_sync_leader_clock(const hifi_party_sync_t *s, int64_t w, int64_t *t_ref, uint32_t *rate, uint8_t bssid[6]) {
	if (!hifi_party_sync_ok(s) || !s->have_bssid) return false;
	double a = s->dac.slope, b = s->tsf.slope; /* µs per frame; TSF µs per µs */
	double r = 1e9 / (a * b), nominal = 1e3 * HIFI_PARTY_SYNC_RATE;
	if (!(a > 0 && b > 0) || !(fabs(r - nominal) <= nominal * 1e-3)) return false; /* a fit gone wild */
	*t_ref = llround(hifi_party_sync_tsf_at(s, w));
	*rate = (uint32_t) llround(r);
	memcpy(bssid, s->bssid, 6);
	return true;
}

bool hifi_party_sync_clock(hifi_party_sync_t *s, int64_t k_ref, int64_t t_ref, uint32_t rate, uint32_t seq, const uint8_t bssid[6], int64_t now) {
	if (s->clk_seq && (int32_t) (seq - s->clk_seq) <= 0) {
		s->old_clocks++; /* a replay, or late */
		return false;
	}
	s->clk_k = k_ref;
	s->clk_t = t_ref;
	s->clk_rate = rate;
	s->clk_seq = seq;
	s->clk_at = now;
	s->clk_ok = true;
	memcpy(s->clk_bssid, bssid, 6);
	if (s->have_bssid && memcmp(s->clk_bssid, s->bssid, 6)) s->bssid_wrong++;
	s->clocks++;
	return true;
}

void hifi_party_sync_forget_clock(hifi_party_sync_t *s) {
	s->clk_ok = false;
	s->clk_seq = 0;
}

bool hifi_party_sync_target(hifi_party_sync_t *s, int64_t w, int64_t now, double *target) {
	if (!s->clk_ok || !hifi_party_sync_ok(s) || !s->have_bssid || memcmp(s->clk_bssid, s->bssid, 6)) return false;
	if (now - s->clk_at >= HIFI_PARTY_SYNC_STALE_US) {
		s->stale++;
		return false;
	}
	*target = hifi_party_target(s->clk_k, s->clk_t, s->clk_rate, hifi_party_sync_tsf_at(s, w));
	return true;
}
