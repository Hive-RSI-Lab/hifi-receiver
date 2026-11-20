/* Party mode's lossless coder (v2): fewer bytes in the air for the same
 * samples. See hifi_party.h (hifi_party_code) and
 * projects/hifi-lite-lab/party-mode.md, sections 2 and 8.16.
 *
 * The code, MSB first: 2 bits of stereo mode (0 L and R, 1 L and S = L - R,
 * 2 S and R, 3 M = (L + R) >> 1 and S, as FLAC has them), then for each of
 * the two coded channels 2 bits of predictor order (0 to 3) and 5 bits of
 * Rice parameter k (0 to HIFI_PARTY_CODE_KMAX), then the first channel's n
 * residuals and the second's. A residual is the sample less the fixed
 * polynomial prediction of that order from the samples before it, with the
 * samples before the packet taken as 0 (so packets decode on their own, as a
 * lost one must allow), folded to unsigned (0, -1, 1, -2 ... as 0, 1, 2, 3)
 * and written as q = u >> k ones, a zero and k low bits; a q of ESC or more
 * is written as ESC ones and u in RAW bits instead. The last byte is padded
 * with zero bits. Every sample a residual makes is checked against its
 * channel's range as it is made, so no input can overflow the decoder. */
#include "hifi_party.h"

#include <string.h>

#define ESC 24
#define RAW 21 /* u < 2^20 at most: an order-3 residual of S is under 8 * 65536 */
#define ORDERS 4

static uint32_t fold(int32_t e) { return e >= 0 ? (uint32_t) e << 1 : ((uint32_t) (-(e + 1)) << 1) | 1; }
static int32_t unfold(uint32_t u) { return (u & 1) ? -(int32_t) (u >> 1) - 1 : (int32_t) (u >> 1); }

static int32_t predict(int order, int32_t a, int32_t b, int32_t c) {
	switch (order) {
	case 0: return 0;
	case 1: return a;
	case 2: return 2 * a - b;
	default: return 3 * a - 3 * b + c;
	}
}

/* the 4 signals a mode can code: L, R, S and M */
enum { SIG_L, SIG_R, SIG_S, SIG_M, SIGS };
static const int mode_sig[4][2] = {{SIG_L, SIG_R}, {SIG_L, SIG_S}, {SIG_S, SIG_R}, {SIG_M, SIG_S}};

static int32_t sig_of(int s, uint32_t f) {
	int32_t l = (int16_t) (uint16_t) f, r = (int16_t) (uint16_t) (f >> 16);
	switch (s) {
	case SIG_L: return l;
	case SIG_R: return r;
	case SIG_S: return l - r;
	default: return (l + r) >> 1; /* arithmetic shift, as the decoder undoes it */
	}
}

static uint32_t rice_bits(const uint32_t *u, int n, int k) {
	uint32_t bits = 0;
	for (int i = 0; i < n; i++) {
		uint32_t q = u[i] >> k;
		bits += q < ESC ? q + 1 + (uint32_t) k : ESC + RAW;
	}
	return bits;
}

typedef struct {
	int order, k;
	uint32_t bits;
} choice_t;

/* the cheapest order and k for one signal; u gets that order's residuals */
static choice_t best(const uint32_t *frames, int n, int s, uint32_t *u, uint32_t *scratch) {
	choice_t b = {0, 0, UINT32_MAX};
	for (int o = 0; o < ORDERS; o++) {
		uint32_t *w = scratch; /* this order's residuals */
		int32_t a = 0, bb = 0, c = 0;
		uint64_t sum = 0;
		for (int i = 0; i < n; i++) {
			int32_t x = sig_of(s, frames[i]);
			w[i] = fold(x - predict(o, a, bb, c));
			sum += w[i];
			c = bb;
			bb = a;
			a = x;
		}
		/* k near log2 of the mean, then its neighbours, exactly costed */
		uint64_t mean = sum / (uint64_t) n;
		int k0 = 0;
		while (k0 < HIFI_PARTY_CODE_KMAX && (2ull << k0) <= mean) k0++;
		for (int k = k0 > 0 ? k0 - 1 : 0; k <= k0 + 1 && k <= HIFI_PARTY_CODE_KMAX; k++) {
			uint32_t bits = rice_bits(w, n, k);
			if (bits < b.bits) {
				b = (choice_t){o, k, bits};
				memcpy(u, w, (size_t) n * sizeof(*u));
			}
		}
	}
	return b;
}

typedef struct {
	uint8_t *p;
	size_t cap, len;
	uint64_t acc;
	int nacc;
	bool full;
} wr_t;

static void put(wr_t *w, uint32_t v, int bits) {
	/* bits <= 32 */
	w->acc = (w->acc << bits) | (v & (bits == 32 ? 0xffffffffu : ((1u << bits) - 1)));
	w->nacc += bits;
	while (w->nacc >= 8) {
		w->nacc -= 8;
		if (w->len >= w->cap) {
			w->full = true;
			return;
		}
		w->p[w->len++] = (uint8_t) (w->acc >> w->nacc);
	}
}

static void put_rice(wr_t *w, const uint32_t *u, int n, int k) {
	for (int i = 0; i < n && !w->full; i++) {
		uint32_t q = u[i] >> k;
		if (q < ESC) {
			/* q ones and a zero (q < ESC, so at most 24 bits), then k bits */
			put(w, ((1u << q) - 1) << 1, (int) q + 1);
			if (k) put(w, u[i], k);
		} else {
			put(w, (1u << ESC) - 1, ESC);
			put(w, u[i], RAW);
		}
	}
}

size_t hifi_party_code(const uint32_t *frames, int n, uint8_t *out, size_t cap) {
	if (!frames || !out || n < 1 || n > HIFI_PARTY_FRAMES) return 0;
	static uint32_t u[SIGS][HIFI_PARTY_FRAMES], scratch[HIFI_PARTY_FRAMES]; /* one task codes: the leader's */
	choice_t c[SIGS];
	for (int s = 0; s < SIGS; s++) c[s] = best(frames, n, s, u[s], scratch);
	int mode = 0;
	uint64_t mb = UINT64_MAX;
	for (int m = 0; m < 4; m++) {
		uint64_t b = (uint64_t) c[mode_sig[m][0]].bits + c[mode_sig[m][1]].bits;
		if (b < mb) {
			mb = b;
			mode = m;
		}
	}
	if ((2 + 2 * 7 + mb + 7) / 8 > cap) return 0;
	wr_t w = {.p = out, .cap = cap};
	put(&w, (uint32_t) mode, 2);
	for (int j = 0; j < 2; j++) {
		const choice_t *ch = &c[mode_sig[mode][j]];
		put(&w, (uint32_t) ch->order, 2);
		put(&w, (uint32_t) ch->k, 5);
	}
	for (int j = 0; j < 2; j++) put_rice(&w, u[mode_sig[mode][j]], n, c[mode_sig[mode][j]].k);
	if (!w.full && w.nacc) put(&w, 0, 8 - w.nacc);
	return w.full ? 0 : w.len;
}

typedef struct {
	const uint8_t *p;
	size_t len, pos; /* pos in bits */
} rd_t;

static bool get(rd_t *r, int bits, uint32_t *v) {
	if (r->pos + (size_t) bits > r->len * 8) return false;
	uint32_t x = 0;
	for (int i = 0; i < bits; i++, r->pos++) x = x << 1 | ((r->p[r->pos >> 3] >> (7 - (r->pos & 7))) & 1);
	*v = x;
	return true;
}

/* the ones before the next zero, at most ESC (then no zero follows) */
static bool get_unary(rd_t *r, uint32_t *q) {
	uint32_t n = 0;
	while (n < ESC) {
		if (r->pos >= r->len * 8) return false;
		int bit = (r->p[r->pos >> 3] >> (7 - (r->pos & 7))) & 1;
		r->pos++;
		if (!bit) break;
		n++;
	}
	*q = n;
	return true;
}

static bool in_range(int s, int32_t x) {
	if (s == SIG_S) return x >= -65535 && x <= 65535;
	return x >= -32768 && x <= 32767;
}

bool hifi_party_uncode(const uint8_t *in, size_t len, int n, uint32_t *frames) {
	if (!in || !frames || n < 1 || n > HIFI_PARTY_FRAMES || len < 2) return false;
	rd_t r = {.p = in, .len = len};
	uint32_t mode, order[2], k[2];
	if (!get(&r, 2, &mode)) return false;
	for (int j = 0; j < 2; j++)
		if (!get(&r, 2, &order[j]) || !get(&r, 5, &k[j]) || k[j] > HIFI_PARTY_CODE_KMAX) return false;
	/* channel 0 goes into frames (as int32) and channel 1 here: half the
	 * stack two arrays would take on the party task */
	int32_t x1[HIFI_PARTY_FRAMES];
	int32_t *x[2] = {(int32_t *) frames, x1};
	for (int j = 0; j < 2; j++) {
		int s = mode_sig[mode][j];
		int32_t a = 0, b = 0, c = 0;
		for (int i = 0; i < n; i++) {
			uint32_t q, lowbits = 0, u;
			if (!get_unary(&r, &q)) return false;
			if (q < ESC) {
				if (k[j] && !get(&r, (int) k[j], &lowbits)) return false;
				u = q << k[j] | lowbits;
			} else if (!get(&r, RAW, &u)) {
				return false;
			}
			int32_t v = unfold(u) + predict((int) order[j], a, b, c);
			if (!in_range(s, v)) return false;
			x[j][i] = v;
			c = b;
			b = a;
			a = v;
		}
	}
	/* only zero padding, under a byte, may follow */
	if (len * 8 - r.pos >= 8) return false;
	uint32_t pad;
	if (r.pos < len * 8 && (!get(&r, (int) (len * 8 - r.pos), &pad) || pad)) return false;
	for (int i = 0; i < n; i++) {
		int32_t l, rr;
		int32_t a = x[0][i], b = x[1][i];
		switch (mode) {
		case 0: l = a, rr = b; break;
		case 1: l = a, rr = a - b; break;
		case 2: l = a + b, rr = b; break;
		default: {
			int32_t m = (int32_t) ((uint32_t) a << 1) | (b & 1);
			l = (m + b) >> 1;
			rr = (m - b) >> 1;
		}
		}
		if (l < -32768 || l > 32767 || rr < -32768 || rr > 32767) return false;
		frames[i] = (uint32_t) (uint16_t) (int16_t) l | (uint32_t) (uint16_t) (int16_t) rr << 16;
	}
	return true;
}

static uint32_t xs32(uint32_t *s) {
	uint32_t x = *s ? *s : 0x9e3779b9u;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return *s = x;
}

static int32_t clamp16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }

uint32_t hifi_party_noise(hifi_party_noise_t *st) {
	int32_t w1 = (int32_t) (xs32(&st->r) >> 20) - 2048; /* 12 bits */
	int32_t w2 = (int32_t) (xs32(&st->r) >> 22) - 512;  /* 10 bits */
	st->c += w1 - (st->c >> 4);
	st->d += w2 - (st->d >> 4);
	int32_t l = clamp16((st->c + st->d) >> 2), r = clamp16((st->c - st->d) >> 2);
	return (uint32_t) (uint16_t) (int16_t) l | (uint32_t) (uint16_t) (int16_t) r << 16;
}
