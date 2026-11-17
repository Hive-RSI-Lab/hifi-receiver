/* What the radio fuzzers share: a reader that turns the input into a stream
 * of received frames, and the checks on the link's counts after each one.
 * A frame is either raw bytes (anything a nearby radio sends) or built with
 * hifi_link_encode from fields in the input, under the run's key so the
 * fuzzer gets past the tag, header and payload checks into the counting, or
 * under another key: a forger's frame, which must be counted bad and move
 * nothing else. */
#pragma once

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hifi_json.h"
#include "hifi_link.h"

#define FUZZ_ASSERT(c)                                                                   \
	do {                                                                             \
		if (!(c)) {                                                              \
			fprintf(stderr, "%s:%d: FUZZ_ASSERT(%s) failed\n", __FILE__, __LINE__, #c); \
			abort();                                                         \
		}                                                                        \
	} while (0)

typedef struct {
	const uint8_t *d;
	size_t n, at;
} fz_t;

static uint8_t fz_u8(fz_t *z) { return z->at < z->n ? z->d[z->at++] : 0; }
static uint32_t fz_u32(fz_t *z) {
	uint32_t v = 0;
	for (int i = 0; i < 4; i++) v |= (uint32_t) fz_u8(z) << (8 * i);
	return v;
}
static int64_t fz_i64(fz_t *z) {
	uint64_t v = 0;
	for (int i = 0; i < 8; i++) v |= (uint64_t) fz_u8(z) << (8 * i);
	return (int64_t) v;
}
static bool fz_more(const fz_t *z) { return z->at < z->n; }

#define FZ_SESSION 0x5e55107u

/* the run's key and a forger's, one bit apart */
static hifi_link_key_t fz_key, fz_forger;
static void fz_keys(void) {
	static bool done;
	if (done) return;
	uint8_t k[HIFI_LINK_KEY];
	for (int i = 0; i < HIFI_LINK_KEY; i++) k[i] = (uint8_t) (0xa0 + i);
	hifi_link_key_init(&fz_key, k);
	k[0] ^= 1;
	hifi_link_key_init(&fz_forger, k);
	done = true;
}

/* One frame from the input into buf (cap bytes): its length (it may be 0, or
 * longer than any radio carries: the receive side mustn't trust it). Times
 * near now_us when the input asks, raw 64-bit values otherwise. *forged: built
 * under the forger's key (flag bit 5). */
static size_t fz_frame(fz_t *z, uint8_t *buf, size_t cap, int64_t now_us, uint32_t rx_session, bool *forged) {
	uint8_t k = fz_u8(z);
	size_t n;
	*forged = false;
	if (k & 1) { /* built */
		hifi_link_frame_t f = {0};
		f.type = (uint8_t) (1 + fz_u8(z) % 3);
		uint8_t s = fz_u8(z);
		f.session = s % 4 == 0 ? FZ_SESSION : s % 4 == 1 ? rx_session : s % 4 == 2 ? FZ_SESSION + 1 : fz_u32(z);
		f.seq = k & 4 ? fz_u32(z) : fz_u8(z);
		if (k & 2) { /* plausible: a ping sent a little before now, answered in between */
			int64_t back = fz_u32(z) % 3000000, turn = fz_u8(z) * 50;
			int64_t off = (int64_t) (int32_t) fz_u32(z);
			f.t1 = now_us - back;
			f.t2 = f.t1 + back / 2 + off;
			f.t3 = f.t2 + turn;
		} else {
			f.t1 = fz_i64(z);
			f.t2 = fz_i64(z);
			f.t3 = fz_i64(z);
		}
		f.len = (uint16_t) (HIFI_LINK_MIN + fz_u8(z) % (HIFI_LINK_MAX - HIFI_LINK_MIN + 1));
		*forged = k & 32;
		const hifi_link_key_t *key = *forged ? &fz_forger : &fz_key;
		n = hifi_link_encode(key, &f, buf, cap);
		FUZZ_ASSERT(n == f.len);
		hifi_link_frame_t back;
		FUZZ_ASSERT(hifi_link_decode(key, buf, n, &back));
		FUZZ_ASSERT(back.type == f.type && back.session == f.session && back.seq == f.seq && back.t1 == f.t1 &&
		            back.t2 == f.t2 && back.t3 == f.t3 && back.len == f.len);
		if (*forged) FUZZ_ASSERT(!hifi_link_decode(&fz_key, buf, n, &back));
		if (k & 8) { /* damaged in the air: one byte, or the length */
			uint8_t at = fz_u8(z), x = fz_u8(z);
			if (k & 16)
				n = at % (n + 8);
			else if (x)
				buf[at % n] ^= x;
		}
	} else { /* raw */
		n = fz_u8(z);
		if (k & 2) n |= (size_t) (fz_u8(z) & 1) << 8; /* up to 511, past every radio's limit */
		if (n > cap) n = cap;
		for (size_t i = 0; i < n; i++) buf[i] = fz_u8(z);
	}
	return n;
}

/* A frame that decodes is exactly what encode makes of it: nothing in a
 * received frame is ignored */
static void fz_canonical(const uint8_t *buf, size_t n) {
	hifi_link_frame_t f;
	if (!hifi_link_decode(&fz_key, buf, n, &f)) return;
	uint8_t again[HIFI_LINK_MAX];
	FUZZ_ASSERT(hifi_link_encode(&fz_key, &f, again, sizeof(again)) == n && !memcmp(again, buf, n));
}

typedef struct {
	uint32_t bad, reply, used, other; /* what hifi_link_receive said */
	uint32_t pongs_in;                /* PONGs handed to tx */
} fz_counts_t;

/* A forged frame: refused, counted bad, and nothing else in either end moved
 * (rx0 and tx0: the ends just before it) */
static void fz_refused(hifi_link_got_t g, const hifi_link_rx_t *rx0, const hifi_link_tx_t *tx0, const hifi_link_rx_t *rx,
                       const hifi_link_tx_t *tx) {
	FUZZ_ASSERT(g == HIFI_LINK_GOT_BAD && rx->bad == rx0->bad + 1);
	hifi_link_rx_t r;
	memcpy(&r, rx, sizeof(r));
	r.bad = rx0->bad;
	FUZZ_ASSERT(!memcmp(&r, rx0, sizeof(r)));
	FUZZ_ASSERT(!memcmp(tx, tx0, sizeof(*tx)));
}

/* after every frame */
static void fz_check(const hifi_link_rx_t *rx, const hifi_link_tx_t *tx, const fz_counts_t *c) {
	FUZZ_ASSERT(rx->bad == c->bad);
	FUZZ_ASSERT(rx->pings == c->reply);
	/* frames counted new in this session: never more than the sequence numbers below the top */
	FUZZ_ASSERT(rx->rx <= hifi_link_rx_expected(rx));
	FUZZ_ASSERT(tx->pongs + tx->dup + tx->stray + tx->bad == c->pongs_in);
	if (tx->pongs) {
		FUZZ_ASSERT(tx->rtt_min_us >= 0 && tx->rtt_min_us <= tx->rtt_max_us);
		/* the sum of the round trips: no wrap */
		FUZZ_ASSERT(tx->rtt_sum_us >= 0 && (double) tx->rtt_sum_us <= (double) tx->pongs * (double) tx->rtt_max_us);
		double p50 = hifi_link_tx_rtt_pct(tx, 50), p99 = hifi_link_tx_rtt_pct(tx, 99);
		FUZZ_ASSERT(p50 >= tx->rtt_min_us && p50 <= p99 && p99 <= tx->rtt_max_us);
		double ppm;
		if (hifi_link_tx_drift_ppm(tx, &ppm)) FUZZ_ASSERT(isfinite(ppm));
	}
	FUZZ_ASSERT(hifi_link_rx_kbps(rx) >= 0 && hifi_link_tx_kbps(tx) >= 0);
}

/* the result both ends report, as the lab's result JSON holds it */
static void fz_json(const hifi_link_rx_t *rx, const hifi_link_tx_t *tx) {
	static char out[4096];
	hj_t j;
	hj_init(&j, out, sizeof(out));
	hj_obj(&j, NULL);
	hj_obj(&j, "tx");
	hifi_link_tx_json(tx, &j);
	hj_obj_end(&j);
	hj_obj(&j, "rx");
	hifi_link_rx_json(rx, &j);
	hj_obj_end(&j);
	hj_obj_end(&j);
	FUZZ_ASSERT(!j.overflow);
	FUZZ_ASSERT(hj_valid_object(out));
}

/* hifi_link_receive's answer, checked against what the frame was */
static void fz_got(hifi_link_got_t g, const uint8_t *buf, size_t n, bool echo, bool pongs, const hifi_link_frame_t *pong,
                   int64_t now_us, fz_counts_t *c) {
	hifi_link_frame_t f;
	bool ok = hifi_link_decode(&fz_key, buf, n, &f);
	switch (g) {
	case HIFI_LINK_GOT_BAD:
		FUZZ_ASSERT(!ok);
		c->bad++;
		break;
	case HIFI_LINK_GOT_REPLY: {
		FUZZ_ASSERT(ok && echo && f.type == HIFI_LINK_PING);
		FUZZ_ASSERT(pong->type == HIFI_LINK_PONG && pong->session == f.session && pong->seq == f.seq &&
		            pong->t1 == f.t1 && pong->t2 == now_us && pong->len == f.len);
		/* the answer goes out as the send stamps it */
		hifi_link_frame_t out = *pong;
		out.t3 = now_us + 1;
		uint8_t b[HIFI_LINK_MAX];
		FUZZ_ASSERT(hifi_link_encode(&fz_key, &out, b, sizeof(b)) == out.len);
		c->reply++;
		break;
	}
	case HIFI_LINK_GOT_USED:
		FUZZ_ASSERT(ok && ((echo && f.type == HIFI_LINK_DATA) || (pongs && f.type == HIFI_LINK_PONG)));
		if (f.type == HIFI_LINK_PONG) c->pongs_in++;
		c->used++;
		break;
	case HIFI_LINK_GOT_OTHER:
		FUZZ_ASSERT(ok);
		c->other++;
		break;
	default:
		FUZZ_ASSERT(0);
	}
}
