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

/* The search, made cheap for the leader's CPU (8.17: the exact search took
 * 43% of a core on a Lite). One pass over the packet keeps, for each of the
 * 4 signals and 4 orders, only the sum of the folded residuals; the order
 * and the mode come from the bits that sum predicts with its best k (as
 * FLAC picks its fixed order), and only the two signals the mode codes are
 * then made and costed exactly, their three k at once. */

/* the Rice bits of n residuals that sum to sum, with the best k: each
 * costs k + 1 and about u >> k more */
static uint64_t est_bits(uint64_t sum, int n, int *kout) {
	uint64_t mean = sum / (uint64_t) n;
	int k0 = 0;
	while (k0 < HIFI_PARTY_CODE_KMAX && (2ull << k0) <= mean) k0++;
	uint64_t b = UINT64_MAX;
	for (int k = k0 > 0 ? k0 - 1 : 0; k <= k0 + 1 && k <= HIFI_PARTY_CODE_KMAX; k++) {
		uint64_t e = (uint64_t) n * (uint64_t) (k + 1) + (sum >> k);
		if (e < b) {
			b = e;
			*kout = k;
		}
	}
	return b;
}

typedef struct {
	int order, k;
	uint32_t bits;
} choice_t;

/* the residuals of signal s at its order into u, and their exact cost at
 * the k next to the estimate's */
static choice_t make(const uint32_t *frames, int n, int s, int order, int kest, uint32_t *u) {
	int32_t a = 0, bb = 0, c = 0;
	int k0 = kest > 0 ? kest - 1 : 0, nk = 0;
	uint32_t bits[3] = {0, 0, 0};
	for (int k = k0; k <= kest + 1 && k <= HIFI_PARTY_CODE_KMAX; k++) nk++;
	for (int i = 0; i < n; i++) {
		int32_t x = sig_of(s, frames[i]);
		uint32_t w = fold(x - predict(order, a, bb, c));
		u[i] = w;
		for (int j = 0; j < nk; j++) {
			uint32_t q = w >> (k0 + j);
			bits[j] += q < ESC ? q + 1 + (uint32_t) (k0 + j) : ESC + RAW;
		}
		c = bb;
		bb = a;
		a = x;
	}
	choice_t ch = {order, k0, bits[0]};
	for (int j = 1; j < nk; j++)
		if (bits[j] < ch.bits) ch = (choice_t){order, k0 + j, bits[j]};
	return ch;
}

/* the 4 orders' folded-residual sums of the 4 signals, in one pass */
/* the estimate reads 1 frame in SUB: it picks the mode, orders and k to try,
 * make() then costs those exactly (lab noise: 0.6092 of PCM, 0.6089 at 1) */
#define SUB 4

static void sums(const uint32_t *frames, int n, uint64_t sum[SIGS][ORDERS]) {
	int32_t a[SIGS] = {0}, b[SIGS] = {0}, c[SIGS] = {0};
	uint32_t acc[SIGS][ORDERS] = {{0}}; /* 352 residuals under 2^20 each: no overflow */
	for (int i = 0; i < n; i++) {
		uint32_t f = frames[i];
		int32_t l = (int16_t) (uint16_t) f, r = (int16_t) (uint16_t) (f >> 16);
		int32_t x[SIGS] = {l, r, l - r, (l + r) >> 1};
		for (int s = 0; s < SIGS; s++) {
			int32_t d1 = x[s] - a[s], d2 = d1 - (a[s] - b[s]), d3 = d2 - (a[s] - 2 * b[s] + c[s]);
			acc[s][0] += fold(x[s]);
			acc[s][1] += fold(d1);
			acc[s][2] += fold(d2);
			acc[s][3] += fold(d3);
			c[s] = b[s];
			b[s] = a[s];
			a[s] = x[s];
		}
	}
	for (int s = 0; s < SIGS; s++)
		for (int o = 0; o < ORDERS; o++) sum[s][o] = acc[s][o];
}

typedef struct {
	uint8_t *p;
	size_t cap, len;
	uint32_t acc;
	int nacc;
	bool full;
} wr_t;

/* bits <= 32 into acc (nacc < 32 bits held, in its low bits), 4 bytes out
 * at a time; 32-bit arithmetic only, cheap on the Lite's Xtensa */
static void put(wr_t *w, uint32_t v, int bits) {
	if (bits < 32) v &= (1u << bits) - 1;
	if (w->nacc + bits < 32) {
		w->acc = (w->acc << bits) | v;
		w->nacc += bits;
		return;
	}
	int r = w->nacc + bits - 32; /* the bits of v that stay behind */
	uint32_t x = (w->nacc ? w->acc << (32 - w->nacc) : 0) | (v >> r);
	w->acc = r ? v & ((1u << r) - 1) : 0;
	w->nacc = r;
	if (w->len + 4 > w->cap) {
		w->full = true;
		return;
	}
	w->p[w->len] = (uint8_t) (x >> 24);
	w->p[w->len + 1] = (uint8_t) (x >> 16);
	w->p[w->len + 2] = (uint8_t) (x >> 8);
	w->p[w->len + 3] = (uint8_t) x;
	w->len += 4;
}

/* what acc still holds, then zero bits to the byte */
static void flush(wr_t *w) {
	while (w->nacc > 0 && !w->full) {
		if (w->len >= w->cap) {
			w->full = true;
			return;
		}
		int sh = w->nacc - 8;
		w->p[w->len++] = (uint8_t) (sh >= 0 ? w->acc >> sh : w->acc << -sh);
		w->nacc = sh > 0 ? sh : 0;
	}
}

static void put_rice(wr_t *w, const uint32_t *u, int n, int k) {
	uint32_t kmask = k ? (1u << k) - 1 : 0;
	for (int i = 0; i < n && !w->full; i++) {
		uint32_t q = u[i] >> k;
		if (q < ESC && q + 1 + (uint32_t) k <= 32) {
			/* q ones, a zero and k bits, in one go */
			uint32_t v = (((1u << q) - 1) << 1) << k | (u[i] & kmask);
			put(w, v, (int) q + 1 + k);
		} else if (q < ESC) {
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
	static uint32_t u[2][HIFI_PARTY_FRAMES]; /* one task codes: the leader's */
	uint64_t sum[SIGS][ORDERS], est[SIGS];
	int ord[SIGS], kk[SIGS];
	int ns = (n + SUB - 1) / SUB; /* the packet's first ns frames */
	sums(frames, ns, sum);
	for (int s = 0; s < SIGS; s++)
		for (int o = 0; o < ORDERS; o++) sum[s][o] = sum[s][o] * (uint64_t) n / (uint64_t) ns;
	for (int s = 0; s < SIGS; s++) {
		est[s] = UINT64_MAX;
		for (int o = 0; o < ORDERS; o++) {
			int k = 0;
			uint64_t e = est_bits(sum[s][o], n, &k);
			if (e < est[s]) {
				est[s] = e;
				ord[s] = o;
				kk[s] = k;
			}
		}
	}
	int mode = 0;
	uint64_t mb = UINT64_MAX;
	for (int m = 0; m < 4; m++) {
		uint64_t b = est[mode_sig[m][0]] + est[mode_sig[m][1]];
		if (b < mb) {
			mb = b;
			mode = m;
		}
	}
	choice_t c[2];
	for (int j = 0; j < 2; j++) {
		int s = mode_sig[mode][j];
		c[j] = make(frames, n, s, ord[s], kk[s], u[j]);
	}
	if ((2 + 2 * 7 + (uint64_t) c[0].bits + c[1].bits + 7) / 8 > cap) return 0;
	wr_t w = {.p = out, .cap = cap};
	put(&w, (uint32_t) mode, 2);
	for (int j = 0; j < 2; j++) {
		put(&w, (uint32_t) c[j].order, 2);
		put(&w, (uint32_t) c[j].k, 5);
	}
	for (int j = 0; j < 2; j++) put_rice(&w, u[j], n, c[j].k);
	flush(&w);
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
