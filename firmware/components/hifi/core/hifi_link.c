#include "hifi_link.h"

#include <stdlib.h>
#include <string.h>

#include "hifi_json.h"

#define MAGIC0 'H'
#define MAGIC1 'L'
#define VERSION 3 /* 3: the tag */

static void put32(uint8_t *p, uint32_t v) {
	for (int i = 0; i < 4; i++) p[i] = (uint8_t) (v >> (8 * i));
}
static void put64(uint8_t *p, int64_t v) {
	uint64_t u = (uint64_t) v;
	for (int i = 0; i < 8; i++) p[i] = (uint8_t) (u >> (8 * i));
}
static uint32_t get32(const uint8_t *p) {
	uint32_t v = 0;
	for (int i = 0; i < 4; i++) v |= (uint32_t) p[i] << (8 * i);
	return v;
}
static int64_t get64(const uint8_t *p) {
	uint64_t v = 0;
	for (int i = 0; i < 8; i++) v |= (uint64_t) p[i] << (8 * i);
	return (int64_t) v;
}

/* the payload byte at offset i of a frame with this sequence number: every
 * byte of the sequence number reaches the pattern (a multiply by an odd
 * constant carries each byte upwards), so a damaged seq is caught too */
static uint8_t pattern(uint32_t seq, size_t i) {
	uint32_t h = seq * 2654435761u;
	return (uint8_t) ((h >> (8 * (i % 4))) ^ (i * 7u) ^ 0x5a);
}

/* ------------------------------------------------------------------- tag */

void hifi_link_key_init(hifi_link_key_t *k, const uint8_t key[HIFI_LINK_KEY]) {
	uint8_t pad[64];
	memset(pad, 0x36, sizeof(pad));
	for (int i = 0; i < HIFI_LINK_KEY; i++) pad[i] ^= key[i];
	hifi_sha256_init(&k->in);
	hifi_sha256_update(&k->in, pad, sizeof(pad));
	memset(pad, 0x5c, sizeof(pad));
	for (int i = 0; i < HIFI_LINK_KEY; i++) pad[i] ^= key[i];
	hifi_sha256_init(&k->out);
	hifi_sha256_update(&k->out, pad, sizeof(pad));
	hifi_link_wipe(pad, sizeof(pad));
	/* a whole block went through each: the buffers hold key bytes, not data */
	hifi_link_wipe(k->in.buf, sizeof(k->in.buf));
	hifi_link_wipe(k->out.buf, sizeof(k->out.buf));
	k->ready = true;
}

void hifi_link_wipe(void *p, size_t n) {
	volatile uint8_t *v = p;
	while (n--) *v++ = 0;
}

void hifi_link_key_wipe(hifi_link_key_t *k) {
	if (k) hifi_link_wipe(k, sizeof(*k));
}

static int hexval(char c);

bool hifi_link_parse_key(const char *s, uint8_t key[HIFI_LINK_KEY]) {
	if (!s || strlen(s) != 2 * HIFI_LINK_KEY) return false;
	uint8_t k[HIFI_LINK_KEY];
	for (int i = 0; i < HIFI_LINK_KEY; i++) {
		int hi = hexval(s[2 * i]), lo = hexval(s[2 * i + 1]);
		if (hi < 0 || lo < 0) {
			hifi_link_wipe(k, sizeof(k));
			return false;
		}
		k[i] = (uint8_t) (hi << 4 | lo);
	}
	memcpy(key, k, sizeof(k));
	hifi_link_wipe(k, sizeof(k));
	return true;
}

void hifi_link_tag(const hifi_link_key_t *k, const uint8_t *hdr, uint8_t tag[HIFI_LINK_TAG]) {
	hifi_sha256_t s = k->in;
	uint8_t d[32];
	hifi_sha256_update(&s, hdr, HIFI_LINK_HDR);
	hifi_sha256_final(&s, d);
	s = k->out;
	hifi_sha256_update(&s, d, sizeof(d));
	hifi_sha256_final(&s, d);
	memcpy(tag, d, HIFI_LINK_TAG);
}

size_t hifi_link_encode(const hifi_link_key_t *k, const hifi_link_frame_t *f, uint8_t *buf, size_t cap) {
	size_t n = f->len;
	if (!k || !k->ready || n < HIFI_LINK_MIN || n > HIFI_LINK_MAX || n > cap) return 0;
	buf[0] = MAGIC0;
	buf[1] = MAGIC1;
	buf[2] = VERSION;
	buf[3] = f->type;
	buf[4] = (uint8_t) n;
	buf[5] = (uint8_t) (n >> 8);
	put32(buf + 6, f->session);
	put32(buf + 10, f->seq);
	put64(buf + 14, f->t1);
	put64(buf + 22, f->t2);
	put64(buf + 30, f->t3);
	for (size_t i = HIFI_LINK_HDR; i < n - HIFI_LINK_TAG; i++) buf[i] = pattern(f->seq, i);
	hifi_link_tag(k, buf, buf + n - HIFI_LINK_TAG);
	return n;
}

bool hifi_link_decode(const hifi_link_key_t *k, const uint8_t *buf, size_t n, hifi_link_frame_t *f) {
	if (!k || !k->ready || !buf || n < HIFI_LINK_MIN || n > HIFI_LINK_MAX) return false;
	/* the tag first: nothing of a frame that fails it is believed */
	uint8_t tag[HIFI_LINK_TAG], diff = 0;
	hifi_link_tag(k, buf, tag);
	for (int i = 0; i < HIFI_LINK_TAG; i++) diff |= tag[i] ^ buf[n - HIFI_LINK_TAG + i];
	if (diff) return false;
	if (buf[0] != MAGIC0 || buf[1] != MAGIC1 || buf[2] != VERSION) return false;
	if (buf[3] < HIFI_LINK_PING || buf[3] > HIFI_LINK_DATA) return false;
	if ((size_t) (buf[4] | buf[5] << 8) != n) return false; /* truncated (or padded) */
	uint32_t seq = get32(buf + 10);
	for (size_t i = HIFI_LINK_HDR; i < n - HIFI_LINK_TAG; i++)
		if (buf[i] != pattern(seq, i)) return false;
	f->type = buf[3];
	f->session = get32(buf + 6);
	f->seq = seq;
	f->t1 = get64(buf + 14);
	f->t2 = get64(buf + 22);
	f->t3 = get64(buf + 30);
	f->len = (uint16_t) n;
	return true;
}

size_t hifi_link_forge(const hifi_link_key_t *k, const uint8_t *in, size_t n, uint32_t jump, uint8_t *out, size_t cap) {
	if (!k || !k->ready || !in || !out || n < HIFI_LINK_MIN || n > HIFI_LINK_MAX || n > cap) return 0;
	if (in[0] != MAGIC0 || in[1] != MAGIC1 || in[2] != VERSION) return 0;
	if ((size_t) (in[4] | in[5] << 8) != n) return 0;
	if (!jump) { /* a replay */
		memcpy(out, in, n);
		return n;
	}
	memcpy(out, in, HIFI_LINK_HDR);
	uint32_t seq = get32(in + 10) + jump;
	put32(out + 10, seq);
	for (size_t i = HIFI_LINK_HDR; i < n - HIFI_LINK_TAG; i++) out[i] = pattern(seq, i);
	hifi_link_tag(k, out, out + n - HIFI_LINK_TAG);
	return n;
}

size_t hifi_link_espnow_body(const uint8_t *p, size_t n, const uint8_t mac[6]) {
	static const uint8_t oui[3] = {0x18, 0xfe, 0x34};
	enum { HDR = 24 };
	if (!p || !mac || n < HIFI_LINK_ESPNOW_BODY + HIFI_LINK_MIN) return 0; /* trailing bytes are fine */
	if (p[0] != 0xd0 || memcmp(p + 10, mac, 6)) return 0; /* an action frame, from mac */
	if (p[HDR] != 127 || memcmp(p + HDR + 1, oui, 3)) return 0;
	if (p[32] != 221 || memcmp(p + 34, oui, 3) || p[37] != 4) return 0;
	if (p[33] < 5) return 0; /* the element's length counts OUI, type and version */
	size_t body = (size_t) p[33] - 5;
	if (body < HIFI_LINK_MIN || body > HIFI_LINK_MAX || HIFI_LINK_ESPNOW_BODY + body > n) return 0;
	return body;
}

/* --------------------------------------------------------------- dup check */

static void bit_clear(hifi_link_seen_t *s, uint32_t seq) {
	uint32_t i = seq % HIFI_LINK_WINDOW;
	s->bits[i / 64] &= ~(1ull << (i % 64));
}

int hifi_link_seen(hifi_link_seen_t *s, uint32_t seq) {
	uint32_t i = seq % HIFI_LINK_WINDOW;
	uint64_t mask = 1ull << (i % 64);
	/* top is seq + 1, so the last value would wrap it to 0 and reset
	 * "expected"; no real sender gets there (49 days at 1000 frames/s) */
	if (seq == UINT32_MAX) return -1;
	if (seq >= s->top) {
		/* move the window up: forget what falls out of it */
		if ((uint64_t) seq - s->top >= HIFI_LINK_WINDOW)
			memset(s->bits, 0, sizeof(s->bits));
		else
			for (uint32_t q = s->top; q < seq; q++) bit_clear(s, q);
		bit_clear(s, seq);
		s->bits[i / 64] |= mask;
		s->top = seq + 1;
		return 0;
	}
	if (s->top - seq > HIFI_LINK_WINDOW) return -1;
	if (s->bits[i / 64] & mask) return 1;
	s->bits[i / 64] |= mask;
	return 0;
}

/* ---------------------------------------------------------- receiving end */

void hifi_link_rx_init(hifi_link_rx_t *r) { memset(r, 0, sizeof(*r)); }

void hifi_link_rx_data(hifi_link_rx_t *r, const hifi_link_frame_t *f, int64_t now_us) {
	if (r->rx && f->session != r->session && now_us - r->last_us < HIFI_LINK_SESSION_IDLE_US) {
		r->stray++;
		return;
	}
	if (r->rx == 0 || f->session != r->session) {
		/* the DATA counts start again; the echo's side stays */
		uint32_t bad = r->bad, pings = r->pings, ping_dup = r->ping_dup, stray = r->stray, ps = r->ping_session;
		hifi_link_seen_t pseen = r->ping_seen;
		hifi_link_rx_init(r);
		r->bad = bad;
		r->pings = pings;
		r->ping_dup = ping_dup;
		r->stray = stray;
		r->ping_session = ps;
		r->ping_seen = pseen;
		r->session = f->session;
		r->first_us = now_us;
	}
	uint32_t top = r->seen.top;
	int s = hifi_link_seen(&r->seen, f->seq);
	if (s < 0) {
		r->old++;
		return;
	}
	if (s > 0) {
		r->dup++;
		return;
	}
	if (top && f->seq + 1 < top) r->reorder++;
	r->rx++;
	r->bytes += f->len;
	r->last_us = now_us;
}

void hifi_link_rx_bad(hifi_link_rx_t *r) { r->bad++; }

uint32_t hifi_link_rx_expected(const hifi_link_rx_t *r) { return r->seen.top; }

double hifi_link_rx_kbps(const hifi_link_rx_t *r) {
	int64_t us = r->last_us - r->first_us;
	if (r->rx < 2 || us <= 0) return 0;
	return r->bytes * 8.0 * 1000.0 / us; /* bits/us*1000 = kbit/s */
}

void hifi_link_rx_json(const hifi_link_rx_t *r, void *hj) {
	hj_t *j = hj;
	uint32_t exp = hifi_link_rx_expected(r);
	hj_int(j, "session", r->session);
	hj_int(j, "rx", r->rx);
	hj_int(j, "expected", exp);
	hj_int(j, "lost", exp > r->rx ? exp - r->rx : 0);
	hj_num(j, "loss_pct", exp ? 100.0 * (exp > r->rx ? exp - r->rx : 0) / exp : 0, 3);
	hj_int(j, "dup", r->dup);
	hj_int(j, "reorder", r->reorder);
	hj_int(j, "old", r->old);
	hj_int(j, "bad", r->bad);
	hj_int(j, "stray", r->stray);
	hj_int(j, "pings", r->pings);
	hj_int(j, "ping_dup", r->ping_dup);
	hj_int(j, "bytes", (long long) r->bytes);
	hj_num(j, "seconds", r->rx > 1 ? (r->last_us - r->first_us) / 1e6 : 0, 3);
	hj_num(j, "kbps", hifi_link_rx_kbps(r), 1);
}

void hifi_link_pong_for(const hifi_link_frame_t *ping, int64_t t2, int64_t t3, hifi_link_frame_t *pong) {
	*pong = *ping;
	pong->type = HIFI_LINK_PONG;
	pong->t2 = t2;
	pong->t3 = t3;
}

/* ------------------------------------------------------------ sending end */

void hifi_link_tx_init(hifi_link_tx_t *t, uint32_t session) {
	memset(t, 0, sizeof(*t));
	t->session = session;
	t->rtt_min_us = -1;
}

void hifi_link_tx_sent(hifi_link_tx_t *t, size_t len, bool ok, int64_t now_us) {
	if (!ok) {
		t->send_fail++;
		return;
	}
	if (!t->sent) t->first_us = now_us;
	t->sent++;
	t->bytes += len;
	t->last_us = now_us;
}

void hifi_link_tx_acked(hifi_link_tx_t *t, bool acked) {
	if (acked)
		t->acked++;
	else
		t->nacked++;
}

static void close_window(hifi_link_tx_t *t) {
	if (!t->win_any) return;
	if (!t->windows) {
		t->x0_us = t->win_start_us;
		t->y0_us = t->win_offset_us;
	}
	double x = (t->win_start_us - t->x0_us) / 1e6;
	/* the offset relative to the first window keeps the sums small */
	double y = (double) (t->win_offset_us - t->y0_us);
	t->sx += x;
	t->sy += y;
	t->sxx += x * x;
	t->sxy += x * y;
	t->windows++;
	t->offset_us = t->win_offset_us;
	t->offset_delay_us = t->win_delay_us;
	t->win_any = false;
}

void hifi_link_tx_pong(hifi_link_tx_t *t, const hifi_link_frame_t *f, int64_t t4) {
	if (f->type != HIFI_LINK_PONG || f->session != t->session) {
		t->stray++;
		return;
	}
	/* the times come off the radio: each clock counts up from its boot, so a
	 * time outside [0, 2^62) or running backwards is refused before any
	 * arithmetic, and a round trip over HIFI_LINK_RTT_MAX_US too, so
	 * rtt_sum_us can't overflow either. Checked before the sequence number,
	 * so a forged PONG doesn't use up the seq of the real one. */
	const int64_t lim = (int64_t) 1 << 62;
	if (f->t1 < 0 || f->t2 < 0 || f->t3 < 0 || t4 < 0 || f->t1 >= lim || f->t2 >= lim || f->t3 >= lim ||
	    t4 >= lim || f->t1 > t4 || f->t2 > f->t3) {
		t->bad++;
		return;
	}
	int64_t delay = (t4 - f->t1) - (f->t3 - f->t2);
	if (delay < 0 || delay > HIFI_LINK_RTT_MAX_US) {
		t->bad++;
		return;
	}
	int s = hifi_link_seen(&t->seen, f->seq);
	if (s != 0) {
		t->dup++;
		return;
	}
	t->pongs++;
	int64_t bin = delay / HIFI_LINK_RTT_BIN_US;
	if (bin >= HIFI_LINK_RTT_BINS) bin = HIFI_LINK_RTT_BINS - 1;
	t->rtt_bins[bin]++;
	if (t->rtt_min_us < 0 || delay < t->rtt_min_us) t->rtt_min_us = delay;
	if (delay > t->rtt_max_us) t->rtt_max_us = delay;
	t->rtt_sum_us += delay;
	if (t->ow) {
		uint32_t n = t->ow_n;
		if (n < t->ow_cap) {
			hifi_link_ow_t *o = &t->ow[n];
			o->t1 = f->t1;
			o->fwd_raw = f->t2 - f->t1;
			o->back_raw = t4 - f->t3;
			/* the sample is whole before the count says so */
			__atomic_store_n(&t->ow_n, n + 1, __ATOMIC_RELEASE);
		} else
			t->ow_dropped++;
	}

	int64_t offset = ((f->t2 - f->t1) + (f->t3 - t4)) / 2;
	if (t->win_any && f->t1 - t->win_start_us >= 1000000) close_window(t);
	if (!t->win_any) {
		t->win_any = true;
		t->win_start_us = f->t1;
		t->win_delay_us = delay;
		t->win_offset_us = offset;
	} else if (delay < t->win_delay_us) {
		t->win_delay_us = delay;
		t->win_offset_us = offset;
	}
	if (!t->windows) { /* until a window closes, report the best so far */
		t->offset_us = t->win_offset_us;
		t->offset_delay_us = t->win_delay_us;
	}
}

double hifi_link_tx_rtt_pct(const hifi_link_tx_t *t, double p) {
	if (!t->pongs) return -1;
	if (p <= 0) return (double) t->rtt_min_us;
	if (p >= 100) return (double) t->rtt_max_us;
	double want = p / 100.0 * t->pongs;
	uint32_t acc = 0;
	for (int i = 0; i < HIFI_LINK_RTT_BINS; i++) {
		if (!t->rtt_bins[i]) continue;
		if (acc + t->rtt_bins[i] >= want) {
			/* interpolate inside the bin, clamped to what was seen */
			double frac = (want - acc) / t->rtt_bins[i];
			double v = (i + frac) * HIFI_LINK_RTT_BIN_US;
			if (v < t->rtt_min_us) v = (double) t->rtt_min_us;
			if (v > t->rtt_max_us) v = (double) t->rtt_max_us;
			return v;
		}
		acc += t->rtt_bins[i];
	}
	return (double) t->rtt_max_us;
}

/* the fit's line with the window still open added, so a run's last second
 * counts too (without closing it: more pongs may land in it): the offset at
 * time x0 + x seconds is y0 + b + m * x, m in us per s */
static bool fit_line(const hifi_link_tx_t *t, double *m, double *b, int64_t *x0, int64_t *y0) {
	double n = t->windows, sx = t->sx, sy = t->sy, sxx = t->sxx, sxy = t->sxy;
	*x0 = t->windows ? t->x0_us : t->win_start_us;
	*y0 = t->windows ? t->y0_us : t->win_offset_us;
	if (t->win_any) {
		double x = (t->win_start_us - *x0) / 1e6, y = (double) (t->win_offset_us - *y0);
		n++;
		sx += x;
		sy += y;
		sxx += x * x;
		sxy += x * y;
	}
	if (n < 3) return false;
	double den = n * sxx - sx * sx;
	if (den <= 0) return false;
	*m = (n * sxy - sx * sy) / den;
	*b = (sy - *m * sx) / n;
	return true;
}

bool hifi_link_tx_drift_ppm(const hifi_link_tx_t *t, double *ppm) {
	double m, b;
	int64_t x0, y0;
	if (!fit_line(t, &m, &b, &x0, &y0)) return false;
	*ppm = m; /* us per s */
	return true;
}

void hifi_link_tx_keep_oneway(hifi_link_tx_t *t, hifi_link_ow_t *buf, uint32_t cap) {
	t->ow = buf;
	t->ow_cap = buf ? cap : 0;
	t->ow_n = t->ow_dropped = 0;
}

static int cmp_i32(const void *a, const void *b) {
	int32_t x = *(const int32_t *) a, y = *(const int32_t *) b;
	return (x > y) - (x < y);
}

static int32_t clamp32(double v) {
	if (v > 1e9) return 1000000000;
	if (v < -1e9) return -1000000000;
	return (int32_t) (v < 0 ? v - 0.5 : v + 0.5);
}

/* nearest rank */
static void ow_stats(int32_t *v, uint32_t n, hifi_link_ow_stats_t *s) {
	qsort(v, n, sizeof(*v), cmp_i32);
	s->n = n;
	s->min = v[0];
	s->max = v[n - 1];
	const int pct[3] = {50, 90, 99};
	int32_t *out[3] = {&s->p50, &s->p90, &s->p99};
	for (int i = 0; i < 3; i++) {
		uint64_t r = ((uint64_t) pct[i] * n + 99) / 100; /* ceil(p n / 100), 1-based */
		*out[i] = v[r ? r - 1 : 0];
	}
}

bool hifi_link_tx_oneway(const hifi_link_tx_t *t, int32_t *scratch, hifi_link_ow_stats_t *fwd,
                         hifi_link_ow_stats_t *back, bool *fit) {
	/* one read of the count for both sides (a pong may land meanwhile) */
	uint32_t n = __atomic_load_n(&t->ow_n, __ATOMIC_ACQUIRE);
	if (!t->ow || !n || !scratch) return false;
	double m = 0, b = 0;
	int64_t x0 = 0, y0 = t->offset_us;
	*fit = fit_line(t, &m, &b, &x0, &y0);
	if (!*fit) {
		m = b = 0;
		x0 = 0;
		y0 = t->offset_us;
	}
	/* the echo's clock reads ours + off(t1): its t2 on ours is t2 - off,
	 * our t4 on its is t4 + off */
	for (uint32_t i = 0; i < n; i++) {
		const hifi_link_ow_t *o = &t->ow[i];
		double off = (double) y0 + b + m * ((o->t1 - x0) / 1e6);
		scratch[i] = clamp32((double) o->fwd_raw - off);
	}
	ow_stats(scratch, n, fwd);
	for (uint32_t i = 0; i < n; i++) {
		const hifi_link_ow_t *o = &t->ow[i];
		double off = (double) y0 + b + m * ((o->t1 - x0) / 1e6);
		scratch[i] = clamp32((double) o->back_raw + off);
	}
	ow_stats(scratch, n, back);
	return true;
}

static void ow_json(hj_t *j, const char *name, const hifi_link_ow_stats_t *s) {
	hj_obj(j, name);
	hj_int(j, "min", s->min);
	hj_int(j, "p50", s->p50);
	hj_int(j, "p90", s->p90);
	hj_int(j, "p99", s->p99);
	hj_int(j, "max", s->max);
	hj_int(j, "jitter_p99", (long long) s->p99 - s->p50); /* how late the 99th percentile comes, past the median */
	hj_obj_end(j);
}

void hifi_link_tx_oneway_json(const hifi_link_tx_t *t, int32_t *scratch, void *hj) {
	hj_t *j = hj;
	hifi_link_ow_stats_t fwd, back;
	bool fit;
	hj_int(j, "kept", t->ow_n);
	hj_int(j, "dropped", t->ow_dropped);
	if (!hifi_link_tx_oneway(t, scratch, &fwd, &back, &fit)) return;
	hj_str(j, "clock", fit ? "drift line" : "best offset");
	ow_json(j, "fwd", &fwd);
	ow_json(j, "back", &back);
}

double hifi_link_tx_kbps(const hifi_link_tx_t *t) {
	int64_t us = t->last_us - t->first_us;
	if (t->sent < 2 || us <= 0) return 0;
	return t->bytes * 8.0 * 1000.0 / us;
}

void hifi_link_tx_json(const hifi_link_tx_t *t, void *hj) {
	hj_t *j = hj;
	hj_int(j, "session", t->session);
	hj_int(j, "sent", t->sent);
	hj_int(j, "send_fail", t->send_fail);
	hj_int(j, "acked", t->acked);
	hj_int(j, "nacked", t->nacked);
	hj_num(j, "seconds", t->sent > 1 ? (t->last_us - t->first_us) / 1e6 : 0, 3);
	hj_num(j, "kbps", hifi_link_tx_kbps(t), 1);
	if (t->pongs || t->stray || t->dup) {
		hj_int(j, "pongs", t->pongs);
		hj_int(j, "lost", t->sent > t->pongs ? t->sent - t->pongs : 0);
		hj_num(j, "loss_pct", t->sent ? 100.0 * (t->sent > t->pongs ? t->sent - t->pongs : 0) / t->sent : 0, 3);
		hj_int(j, "dup", t->dup);
		hj_int(j, "stray", t->stray);
		hj_int(j, "bad", t->bad);
	}
	if (t->pongs) {
		hj_obj(j, "rtt_ms");
		hj_num(j, "min", t->rtt_min_us / 1000.0, 3);
		hj_num(j, "p50", hifi_link_tx_rtt_pct(t, 50) / 1000.0, 3);
		hj_num(j, "p95", hifi_link_tx_rtt_pct(t, 95) / 1000.0, 3);
		hj_num(j, "p99", hifi_link_tx_rtt_pct(t, 99) / 1000.0, 3);
		hj_num(j, "max", t->rtt_max_us / 1000.0, 3);
		hj_num(j, "mean", (double) t->rtt_sum_us / t->pongs / 1000.0, 3);
		hj_obj_end(j);
		hj_obj(j, "clock");
		hj_int(j, "offset_us", (long long) t->offset_us);
		hj_int(j, "offset_delay_us", (long long) t->offset_delay_us);
		hj_int(j, "windows", t->windows + (t->win_any ? 1 : 0));
		double ppm;
		if (hifi_link_tx_drift_ppm(t, &ppm))
			hj_num(j, "drift_ppm", ppm, 3);
		else
			hj_null(j, "drift_ppm");
		hj_obj_end(j);
	}
}

size_t hifi_link_take(uint8_t *dst, size_t cap, const uint8_t *src, size_t n) {
	if (!dst || !src || !n || n > cap) return 0;
	memcpy(dst, src, n);
	return n;
}

hifi_link_got_t hifi_link_receive(const hifi_link_key_t *k, hifi_link_rx_t *rx, hifi_link_tx_t *tx, bool echo, bool pongs,
                                  const uint8_t *buf, size_t n, int64_t t_us, hifi_link_frame_t *pong) {
	hifi_link_frame_t f;
	if (!hifi_link_decode(k, buf, n, &f)) {
		hifi_link_rx_bad(rx);
		return HIFI_LINK_GOT_BAD;
	}
	if (echo && f.type == HIFI_LINK_PING) {
		/* one answer per PING: a replayed one gets none */
		if (f.session != rx->ping_session || !rx->pings) {
			memset(&rx->ping_seen, 0, sizeof(rx->ping_seen));
			rx->ping_session = f.session;
		}
		if (hifi_link_seen(&rx->ping_seen, f.seq) != 0) {
			rx->ping_dup++;
			return HIFI_LINK_GOT_OTHER;
		}
		hifi_link_pong_for(&f, t_us, 0, pong);
		rx->pings++;
		return HIFI_LINK_GOT_REPLY;
	}
	if (echo && f.type == HIFI_LINK_DATA) {
		hifi_link_rx_data(rx, &f, t_us);
		return HIFI_LINK_GOT_USED;
	}
	if (pongs && f.type == HIFI_LINK_PONG) {
		hifi_link_tx_pong(tx, &f, t_us);
		return HIFI_LINK_GOT_USED;
	}
	return HIFI_LINK_GOT_OTHER;
}

static int hexval(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

bool hifi_link_parse_mac(const char *s, uint8_t mac[6]) {
	if (!s) return false;
	size_t n = strlen(s);
	bool sep = n == 17;
	if (!sep && n != 12) return false;
	for (int i = 0; i < 6; i++) {
		const char *p = s + i * (sep ? 3 : 2);
		int hi = hexval(p[0]), lo = hexval(p[1]);
		if (hi < 0 || lo < 0) return false;
		if (sep && i < 5 && p[2] != ':' && p[2] != '-') return false;
		mac[i] = (uint8_t) (hi << 4 | lo);
	}
	return true;
}
