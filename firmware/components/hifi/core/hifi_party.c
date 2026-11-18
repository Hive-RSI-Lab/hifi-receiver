/* Party mode's portable core: the packet, the clock fit, the jitter buffer,
 * the follower's controller and the leader's resend history. See
 * hifi_party.h and projects/hifi-lite-lab/party-mode.md. */
#include "hifi_party.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "hifi_sha256.h"

static void put16(uint8_t *p, uint16_t v) {
	p[0] = (uint8_t) v;
	p[1] = (uint8_t) (v >> 8);
}
static void put32(uint8_t *p, uint32_t v) {
	for (int i = 0; i < 4; i++) p[i] = (uint8_t) (v >> (8 * i));
}
static void put64(uint8_t *p, int64_t v) {
	uint64_t u = (uint64_t) v;
	for (int i = 0; i < 8; i++) p[i] = (uint8_t) (u >> (8 * i));
}
static uint16_t get16(const uint8_t *p) {
	return (uint16_t) (p[0] | p[1] << 8);
}
static uint32_t get32(const uint8_t *p) {
	return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24;
}
static int64_t get64(const uint8_t *p) {
	uint64_t u = 0;
	for (int i = 7; i >= 0; i--) u = u << 8 | p[i];
	return (int64_t) u;
}

/* floor division, for stream indices that may be below zero */
static int64_t fdiv(int64_t a, int64_t b) {
	int64_t q = a / b;
	return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}

/* ------------------------------------------------------------------ packet */

/* HMAC-SHA256 of the n bytes, cut to HIFI_PARTY_TAG */
static void tag(const hifi_link_key_t *k, const uint8_t *d, size_t n, uint8_t out[HIFI_PARTY_TAG]) {
	hifi_sha256_t s = k->in;
	uint8_t h[32];
	hifi_sha256_update(&s, d, n);
	hifi_sha256_final(&s, h);
	s = k->out;
	hifi_sha256_update(&s, h, sizeof(h));
	hifi_sha256_final(&s, h);
	memcpy(out, h, HIFI_PARTY_TAG);
}

/* the payload bytes a packet of this type, n and flags carries; -1 when it
 * can't (a silence AUDIO packet carries none) */
static long payload_len(uint8_t type, uint16_t n, uint16_t flags) {
	switch (type) {
	case HIFI_PARTY_AUDIO:
		if (n < 1 || n > HIFI_PARTY_FRAMES) return -1;
		return (flags & HIFI_PARTY_F_SILENCE) ? 0 : (long) n * HIFI_PARTY_FRAME_BYTES;
	case HIFI_PARTY_CLOCK: return n == 0 ? 6 : -1;
	case HIFI_PARTY_NACK: return (n >= 1 && n <= HIFI_PARTY_NACK_MAX) ? (long) n * 8 : -1;
	case HIFI_PARTY_ANNOUNCE: return n <= HIFI_PARTY_FOLLOWERS ? HIFI_PARTY_ANNOUNCE_LEN : -1;
	case HIFI_PARTY_JOIN: return n == 0 ? 4 : -1;
	default: return -1;
	}
}

size_t hifi_party_encode(const hifi_link_key_t *key, const hifi_party_pkt_t *p, const void *payload, uint8_t *buf, size_t cap) {
	if (!key || !key->ready || !p || !buf) return 0;
	long l = payload_len(p->type, p->n, p->flags);
	if (l < 0 || (l && !payload)) return 0;
	size_t pl = (size_t) l;
	size_t len = HIFI_PARTY_HDR + pl + HIFI_PARTY_TAG;
	if (len > cap || len > HIFI_PARTY_MAX) return 0;
	buf[0] = HIFI_PARTY_MAGIC0;
	buf[1] = HIFI_PARTY_MAGIC1;
	buf[2] = HIFI_PARTY_VERSION;
	buf[3] = p->type;
	put16(buf + 4, (uint16_t) len);
	put32(buf + 6, p->group);
	put32(buf + 10, p->seq);
	put64(buf + 14, p->k);
	put64(buf + 22, p->t);
	put32(buf + 30, p->rate);
	put16(buf + 34, p->n);
	put16(buf + 36, p->flags);
	if (pl) memcpy(buf + HIFI_PARTY_HDR, payload, pl);
	tag(key, buf, HIFI_PARTY_HDR + pl, buf + HIFI_PARTY_HDR + pl);
	return len;
}

int hifi_party_decode(const hifi_link_key_t *key, const uint8_t *buf, size_t n, hifi_party_pkt_t *p, const uint8_t **payload, size_t *plen) {
	if (!key || !key->ready) return HIFI_PARTY_E_KEY;
	if (!buf || n < HIFI_PARTY_HDR + HIFI_PARTY_TAG || n > HIFI_PARTY_MAX) return HIFI_PARTY_E_SHORT;
	/* a cheap filter before the HMAC: stray datagrams on the port cost a
	 * compare, not a hash. Nothing here is believed until the tag passes. */
	if (buf[0] != HIFI_PARTY_MAGIC0 || buf[1] != HIFI_PARTY_MAGIC1 || buf[2] != HIFI_PARTY_VERSION) return HIFI_PARTY_E_MAGIC;
	uint8_t t[HIFI_PARTY_TAG], diff = 0;
	tag(key, buf, n - HIFI_PARTY_TAG, t);
	for (int i = 0; i < HIFI_PARTY_TAG; i++) diff |= t[i] ^ buf[n - HIFI_PARTY_TAG + i]; /* constant time */
	if (diff) return HIFI_PARTY_E_TAG;
	if (get16(buf + 4) != n) return HIFI_PARTY_E_SHORT; /* truncated or padded, though tagged */
	hifi_party_pkt_t q = {
		.type = buf[3],
		.group = get32(buf + 6),
		.seq = get32(buf + 10),
		.k = get64(buf + 14),
		.t = get64(buf + 22),
		.rate = get32(buf + 30),
		.n = get16(buf + 34),
		.flags = get16(buf + 36),
	};
	long l = payload_len(q.type, q.n, q.flags);
	if (l < 0 || HIFI_PARTY_HDR + (size_t) l + HIFI_PARTY_TAG != n) return HIFI_PARTY_E_FORMAT;
	size_t pl = (size_t) l;
	if (q.type == HIFI_PARTY_AUDIO && (q.k < 0 || q.k % HIFI_PARTY_FRAMES)) return HIFI_PARTY_E_FORMAT;
	if (p) *p = q;
	if (payload) *payload = buf + HIFI_PARTY_HDR;
	if (plen) *plen = pl;
	return HIFI_PARTY_OK;
}

int64_t hifi_party_nack_k(const uint8_t *payload, int i) {
	return get64(payload + 8 * (size_t) i);
}

/* --------------------------------------------------------------- clock fit */

void hifi_party_fit_init(hifi_party_fit_t *f, int64_t win_us, int cap, int min_points, double reject) {
	memset(f, 0, sizeof(*f));
	f->win_us = win_us > 0 ? win_us : 1;
	f->cap = cap < 2 ? 2 : cap > HIFI_PARTY_FIT_MAX ? HIFI_PARTY_FIT_MAX : cap;
	f->min_points = min_points < 2 ? 2 : min_points;
	f->reject = reject;
}

/* least squares through the kept points, leaving out those more than reject
 * above the line drawn with them (late windows) and drawing it again. With
 * both_sides a window that far below is left out too, but only the worst one
 * a pass: a step drags the first line, and leaving out everything far from
 * that would take the good windows with it */
/* the ring's i-th oldest window: once it is full, or the newest was dropped,
 * slots 0..n-1 are not the n kept */
static int slot(const hifi_party_fit_t *f, int i) { return (f->head - f->n + i + 2 * f->cap) % f->cap; }

static void refit(hifi_party_fit_t *f) {
	int newest = (f->head + f->cap - 1) % f->cap;
	f->xb = f->px[newest];
	f->yb = f->py[newest];
	bool use[HIFI_PARTY_FIT_MAX];
	for (int i = 0; i < f->n; i++) use[i] = true;
	f->ok = false;
	int passes = f->both_sides ? f->n + 1 : 3; /* at most n - min_points are left out */
	for (int pass = 0; pass < passes; pass++) {
		double sx = 0, sy = 0;
		int m = 0;
		for (int i = 0; i < f->n; i++)
			if (use[i]) {
				sx += (double) (f->px[slot(f, i)] - f->xb);
				sy += (double) (f->py[slot(f, i)] - f->yb);
				m++;
			}
		if (m < 2) return;
		double mx = sx / m, my = sy / m, sxx = 0, sxy = 0;
		for (int i = 0; i < f->n; i++)
			if (use[i]) {
				double dx = (double) (f->px[slot(f, i)] - f->xb) - mx, dy = (double) (f->py[slot(f, i)] - f->yb) - my;
				sxx += dx * dx;
				sxy += dx * dy;
			}
		if (sxx <= 0) return;
		f->mx = mx;
		f->my = my;
		f->slope = sxy / sxx;
		double ss = 0, worst_r = 0;
		int worst = -1;
		bool dropped = false;
		for (int i = 0; i < f->n; i++) {
			if (!use[i]) continue;
			double r = (double) (f->py[slot(f, i)] - f->yb) - (my + f->slope * ((double) (f->px[slot(f, i)] - f->xb) - mx));
			if (f->both_sides) {
				if (f->reject > 0 && fabs(r) > f->reject && m > f->min_points && fabs(r) > worst_r) {
					worst_r = fabs(r);
					worst = i;
				}
				ss += r * r;
			} else if (f->reject > 0 && r > f->reject && m > f->min_points) {
				use[i] = false;
				dropped = true;
			} else {
				ss += r * r;
			}
		}
		if (worst >= 0) {
			use[worst] = false;
			dropped = true;
			continue; /* draw it again without the worst; rms and used come from the last line */
		}
		f->used = 0;
		for (int i = 0; i < f->n; i++) f->used += use[i];
		f->rejected = f->n - f->used;
		f->rms = sqrt(ss / (f->used ? f->used : 1));
		if (!dropped) break;
	}
	f->ok = f->used >= f->min_points;
}

bool hifi_party_fit_add(hifi_party_fit_t *f, int64_t x, int64_t y, double q, int64_t now_us) {
	int64_t win = fdiv(now_us, f->win_us);
	bool closed = false;
	if (f->have_cur && win != f->cur_win) {
		f->px[f->head] = f->cur_x;
		f->py[f->head] = f->cur_y;
		f->head = (f->head + 1) % f->cap;
		if (f->n < f->cap) f->n++;
		refit(f);
		f->have_cur = false;
		closed = true;
	}
	if (!f->have_cur || q < f->cur_q) {
		f->have_cur = true;
		f->cur_win = win;
		f->cur_x = x;
		f->cur_y = y;
		f->cur_q = q;
	}
	return closed;
}

void hifi_party_fit_drop_newest(hifi_party_fit_t *f) {
	if (f->n <= 0) return;
	f->head = (f->head + f->cap - 1) % f->cap;
	f->n--;
	refit(f);
}

double hifi_party_fit_y(const hifi_party_fit_t *f, double x) {
	return (double) f->yb + f->my + f->slope * ((x - (double) f->xb) - f->mx);
}

double hifi_party_fit_x(const hifi_party_fit_t *f, double y) {
	if (f->slope == 0) return (double) f->xb + f->mx;
	return (double) f->xb + f->mx + ((y - (double) f->yb) - f->my) / f->slope;
}

bool hifi_party_dac_sample(int64_t w0, int64_t w1, int64_t t0, int64_t t1, int buf_frames, int64_t min_us, int64_t *x) {
	if (buf_frames <= 0 || w1 <= w0) return false;
	if (min_us > 0 && t1 - t0 < min_us) return false;
	int64_t b = fdiv(w1 - 1, buf_frames) * buf_frames; /* the last boundary below w1 */
	if (b < w0) return false;
	if (x) *x = b;
	return true;
}

double hifi_party_target(int64_t k_ref, int64_t t_ref, uint32_t rate_mhz, double t) {
	return (double) k_ref + (t - (double) t_ref) * (double) rate_mhz / 1e9;
}

/* ----------------------------------------------------------- jitter buffer */

#define SLOT_BYTES ((size_t) HIFI_PARTY_FRAMES * HIFI_PARTY_FRAME_BYTES)

size_t hifi_party_jb_bytes(int nslots) {
	if (nslots < 1) return 0;
	return (size_t) nslots * (SLOT_BYTES + 3 * sizeof(int64_t) + 1);
}

void hifi_party_jb_init(hifi_party_jb_t *jb, void *mem, int nslots) {
	memset(jb, 0, sizeof(*jb));
	uint8_t *m = mem;
	jb->nslots = nslots;
	jb->frames = (uint32_t *) m;
	m += (size_t) nslots * SLOT_BYTES; /* a multiple of 8 */
	jb->have = (int64_t *) m;
	m += (size_t) nslots * sizeof(int64_t);
	jb->miss = (int64_t *) m;
	m += (size_t) nslots * sizeof(int64_t);
	jb->miss_at = (int64_t *) m;
	m += (size_t) nslots * sizeof(int64_t);
	jb->nacks = m;
	memset(jb->frames, 0, (size_t) nslots * SLOT_BYTES);
	for (int i = 0; i < nslots; i++) {
		jb->have[i] = -1;
		jb->miss[i] = -1;
		jb->miss_at[i] = 0;
		jb->nacks[i] = 0;
	}
	jb->floor = -1;
	jb->top = -1;
}

int hifi_party_jb_put(hifi_party_jb_t *jb, int64_t k, const void *frames, int n, int64_t now_us) {
	if (k < 0 || k % HIFI_PARTY_FRAMES || n != HIFI_PARTY_FRAMES) {
		jb->bad++;
		return HIFI_PARTY_PUT_BAD;
	}
	int64_t p = k / HIFI_PARTY_FRAMES;
	if (jb->floor < 0) jb->floor = p;
	if (p < jb->floor) {
		jb->old++;
		return HIFI_PARTY_PUT_OLD;
	}
	if (p >= jb->floor + jb->nslots) {
		/* far ahead (the play position stalled, or a long outage): the
		 * window moves up to it and what it passes is given up */
		int64_t nf = p - jb->nslots + 1;
		jb->skipped += (uint32_t) (nf - jb->floor);
		jb->floor = nf;
	}
	int s = (int) (p % jb->nslots);
	if (jb->have[s] == p) {
		jb->dup++;
		return HIFI_PARTY_PUT_DUP;
	}
	if (frames)
		memcpy(jb->frames + (size_t) s * HIFI_PARTY_FRAMES, frames, SLOT_BYTES);
	else
		memset(jb->frames + (size_t) s * HIFI_PARTY_FRAMES, 0, SLOT_BYTES); /* a silence packet */
	jb->have[s] = p;
	if (jb->miss[s] == p) jb->miss[s] = -1;
	if (jb->top >= 0 && p > jb->top + 1) {
		int64_t from = jb->top + 1 > jb->floor ? jb->top + 1 : jb->floor;
		for (int64_t q = from; q < p; q++) {
			int qs = (int) (q % jb->nslots);
			if (jb->have[qs] == q) continue;
			jb->miss[qs] = q;
			jb->miss_at[qs] = now_us;
			jb->nacks[qs] = 0;
			jb->gaps++;
		}
	}
	if (p > jb->top) jb->top = p;
	jb->got++;
	return HIFI_PARTY_PUT_OK;
}

int hifi_party_jb_nacks(hifi_party_jb_t *jb, int64_t now_us, int64_t *k, int max) {
	int c = 0;
	if (jb->floor < 0 || jb->top < 0) return 0;
	int64_t end = jb->top < jb->floor + jb->nslots - 1 ? jb->top : jb->floor + jb->nslots - 1;
	for (int64_t q = jb->floor; q <= end && c < max; q++) {
		int s = (int) (q % jb->nslots);
		if (jb->miss[s] != q || jb->have[s] == q) continue;
		int64_t age = now_us - jb->miss_at[s];
		if ((jb->nacks[s] == 0 && age >= HIFI_PARTY_NACK1_US) || (jb->nacks[s] == 1 && age >= HIFI_PARTY_NACK2_US)) {
			jb->nacks[s]++;
			k[c++] = q * HIFI_PARTY_FRAMES;
		}
	}
	jb->nacked += (uint32_t) c;
	return c;
}

static int16_t lo(uint32_t f) { return (int16_t) (uint16_t) f; }
static int16_t hi(uint32_t f) { return (int16_t) (uint16_t) (f >> 16); }
static uint32_t pack(int32_t l, int32_t r) { return (uint32_t) (uint16_t) (int16_t) l | (uint32_t) (uint16_t) (int16_t) r << 16; }

/* a frame times a / b, per channel */
static uint32_t scale(uint32_t f, int32_t a, int32_t b) {
	return pack(lo(f) * a / b, hi(f) * a / b);
}

/* one concealed frame: the last stretch played again, fading out, then silence */
static uint32_t conceal(hifi_party_jb_t *jb) {
	int j = jb->gap_run++;
	if (jb->gap_run > HIFI_PARTY_CONCEAL) jb->gap_run = HIFI_PARTY_CONCEAL + 1; /* no overflow on long silence */
	return j < HIFI_PARTY_CONCEAL ? scale(jb->hist[(jb->hist_pos + j) % HIFI_PARTY_CONCEAL], HIFI_PARTY_CONCEAL - j, HIFI_PARTY_CONCEAL) : 0;
}

int hifi_party_jb_read(hifi_party_jb_t *jb, int64_t k, uint32_t *out, int n) {
	int missing = 0;
	for (int i = 0; i < n; i++) {
		int64_t idx = k + i, p = fdiv(idx, HIFI_PARTY_FRAMES);
		int s = jb->nslots > 0 ? (int) (((p % jb->nslots) + jb->nslots) % jb->nslots) : 0;
		if (idx >= 0 && jb->nslots > 0 && jb->have[s] == p) {
			uint32_t f = jb->frames[(size_t) s * HIFI_PARTY_FRAMES + (size_t) (idx - p * HIFI_PARTY_FRAMES)];
			if (jb->gap_run) {
				jb->gap_run = 0;
				jb->fade_in = HIFI_PARTY_FADE_IN;
			}
			jb->hist[jb->hist_pos] = f;
			jb->hist_pos = (jb->hist_pos + 1) % HIFI_PARTY_CONCEAL;
			if (jb->fade_in > 0) {
				f = scale(f, HIFI_PARTY_FADE_IN - jb->fade_in + 1, HIFI_PARTY_FADE_IN + 1);
				jb->fade_in--;
			}
			out[i] = f;
		} else {
			out[i] = conceal(jb);
			missing++;
		}
	}
	jb->missing_frames += (uint32_t) missing;
	/* let go of what is wholly behind k, keeping one packet for a slip back */
	int64_t nf = fdiv(k, HIFI_PARTY_FRAMES) - 1;
	if (jb->floor >= 0 && nf > jb->floor) jb->floor = nf;
	else if (jb->floor >= 0 && nf < jb->floor) {
		/* a jump back (the leader's stream stepped back, or a slow
		 * follower's first jump): take late and resent packets for the new
		 * place again, not as old. The floor stays within a buffer of the
		 * top, so no two wanted packets share a slot. */
		int64_t lowest = jb->top - jb->nslots + 1;
		if (nf < lowest) nf = lowest;
		if (nf < 0) nf = 0;
		if (nf < jb->floor) jb->floor = nf;
	}
	return missing;
}

/* -------------------------------------------------------------- controller */

void hifi_party_ctl_init(hifi_party_ctl_t *c, int rate) {
	memset(c, 0, sizeof(*c));
	c->jump_frames = rate > 0 ? rate / 200 : 220; /* 5 ms */
	if (c->jump_frames < 2) c->jump_frames = 2;
	c->since_slip = HIFI_PARTY_SLIP_EVERY;
}

/* the quietest frame of [0, n) */
static int quietest(const uint32_t *f, int n) {
	int best = 0;
	int32_t bv = INT32_MAX;
	for (int i = 0; i < n; i++) {
		int32_t v = abs(lo(f[i])) + abs(hi(f[i]));
		if (v < bv) {
			bv = v;
			best = i;
		}
	}
	return best;
}

void hifi_party_render(hifi_party_ctl_t *c, hifi_party_jb_t *jb, double target, uint32_t *out, int n, int32_t gain_l, int32_t gain_r) {
	if (n <= 0) return;
	if (!isfinite(target)) target = (double) c->next;
	if (!c->started) {
		c->next = llround(target);
		c->started = true;
	}
	double err = (double) c->next - target; /* > 0: ahead of the leader */
	c->last_err = err;
	if (fabs(err) >= c->jump_frames) {
		/* far off: jump, crossfading from where the stream was */
		int m = n < HIFI_PARTY_CONCEAL ? n : HIFI_PARTY_CONCEAL;
		hifi_party_jb_read(jb, c->next, c->xfade, m);
		int64_t to = llround(target);
		hifi_party_jb_read(jb, to, out, n);
		for (int i = 0; i < m; i++) {
			int32_t a = i + 1, b = m + 1;
			out[i] = pack((lo(c->xfade[i]) * (b - a) + lo(out[i]) * a) / b, (hi(c->xfade[i]) * (b - a) + hi(out[i]) * a) / b);
		}
		c->next = to + n;
		c->jumps++;
		c->since_slip = 0;
	} else if (err >= 1 && n >= 2 && c->since_slip >= HIFI_PARTY_SLIP_EVERY) {
		/* ahead: play one frame twice, at the quietest point */
		hifi_party_jb_read(jb, c->next, out, n - 1);
		int q = quietest(out, n - 1);
		memmove(out + q + 2, out + q + 1, (size_t) (n - 2 - q) * sizeof(*out));
		out[q + 1] = out[q];
		c->next += n - 1;
		c->repeats++;
		c->since_slip = 0;
	} else if (err <= -1 && n >= 2 && c->since_slip >= HIFI_PARTY_SLIP_EVERY) {
		/* behind: leave one frame out, at the quietest point */
		uint32_t extra;
		hifi_party_jb_read(jb, c->next, out, n);
		hifi_party_jb_read(jb, c->next + n, &extra, 1);
		int q = quietest(out, n);
		memmove(out + q, out + q + 1, (size_t) (n - 1 - q) * sizeof(*out));
		out[n - 1] = extra;
		c->next += n + 1;
		c->drops++;
		c->since_slip = 0;
	} else {
		hifi_party_jb_read(jb, c->next, out, n);
		c->next += n;
	}
	c->since_slip += n;
	hifi_party_gain(out, n, gain_l, gain_r);
}

void hifi_party_mute(hifi_party_ctl_t *c, hifi_party_jb_t *jb, uint32_t *out, int n, int32_t gain_l, int32_t gain_r) {
	if (n <= 0) return;
	for (int i = 0; i < n; i++) out[i] = conceal(jb); /* not missing: nothing was due */
	c->started = false;
	hifi_party_gain(out, n, gain_l, gain_r);
}

static int32_t sat16(int64_t v) {
	return v > 32767 ? 32767 : v < -32768 ? -32768 : (int32_t) v;
}

void hifi_party_gain(uint32_t *frames, int n, int32_t gain_l, int32_t gain_r) {
	if (gain_l == 65536 && gain_r == 65536) return;
	for (int i = 0; i < n; i++)
		frames[i] = pack(sat16(((int64_t) lo(frames[i]) * gain_l) >> 16), sat16(((int64_t) hi(frames[i]) * gain_r) >> 16));
}

/* ------------------------------------------------------- resend history */

size_t hifi_party_hist_bytes(int nslots) {
	if (nslots < 1) return 0;
	return (size_t) nslots * (sizeof(int64_t) + HIFI_PARTY_MAX + sizeof(uint16_t));
}

void hifi_party_hist_init(hifi_party_hist_t *h, void *mem, int nslots) {
	uint8_t *m = mem;
	h->nslots = nslots;
	h->k = (int64_t *) m;
	m += (size_t) nslots * sizeof(int64_t);
	h->pkts = m;
	m += (size_t) nslots * HIFI_PARTY_MAX;
	h->len = (uint16_t *) m; /* HIFI_PARTY_MAX is even, so this is 2-aligned */
	for (int i = 0; i < nslots; i++) {
		h->k[i] = -1;
		h->len[i] = 0;
	}
}

void hifi_party_hist_put(hifi_party_hist_t *h, int64_t k, const uint8_t *pkt, size_t len) {
	if (k < 0 || k % HIFI_PARTY_FRAMES || len > HIFI_PARTY_MAX || h->nslots < 1) return;
	int s = (int) ((k / HIFI_PARTY_FRAMES) % h->nslots);
	memcpy(h->pkts + (size_t) s * HIFI_PARTY_MAX, pkt, len);
	h->k[s] = k;
	h->len[s] = (uint16_t) len;
}

const uint8_t *hifi_party_hist_get(const hifi_party_hist_t *h, int64_t k, size_t *len) {
	if (k < 0 || k % HIFI_PARTY_FRAMES || h->nslots < 1) return NULL;
	int s = (int) ((k / HIFI_PARTY_FRAMES) % h->nslots);
	if (h->k[s] != k) return NULL;
	if (len) *len = h->len[s];
	return h->pkts + (size_t) s * HIFI_PARTY_MAX;
}

/* ---------------------------------------------------------------- bursts */

void hifi_party_bursts_add(hifi_party_bursts_t *r, uint32_t now_ms, uint32_t count, int32_t code) {
	if (r->n) {
		hifi_party_burst_t *l = &r->b[(r->head + HIFI_PARTY_BURSTS - 1) % HIFI_PARTY_BURSTS];
		uint32_t end = l->at_ms + l->dur_ms;
		if (now_ms - end <= HIFI_PARTY_BURST_GAP_MS && now_ms - l->at_ms < 0x80000000u) {
			l->dur_ms = now_ms - l->at_ms;
			l->count += count;
			if (code) l->code = code;
			return;
		}
	}
	r->b[r->head] = (hifi_party_burst_t){now_ms, 0, count, code};
	r->head = (r->head + 1) % HIFI_PARTY_BURSTS;
	if (r->n < HIFI_PARTY_BURSTS) r->n++;
	r->total++;
}

const hifi_party_burst_t *hifi_party_bursts_get(const hifi_party_bursts_t *r, int i) {
	if (i < 0 || i >= r->n) return NULL;
	return &r->b[(r->head + HIFI_PARTY_BURSTS - 1 - i) % HIFI_PARTY_BURSTS];
}
