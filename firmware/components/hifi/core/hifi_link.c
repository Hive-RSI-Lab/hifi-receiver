#include "hifi_link.h"

#include <string.h>

#include "hifi_json.h"

#define MAGIC0 'H'
#define MAGIC1 'L'
#define VERSION 2

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

size_t hifi_link_encode(const hifi_link_frame_t *f, uint8_t *buf, size_t cap) {
	size_t n = f->len;
	if (n < HIFI_LINK_HDR || n > HIFI_LINK_MAX || n > cap) return 0;
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
	for (size_t i = HIFI_LINK_HDR; i < n; i++) buf[i] = pattern(f->seq, i);
	return n;
}

bool hifi_link_decode(const uint8_t *buf, size_t n, hifi_link_frame_t *f) {
	if (!buf || n < HIFI_LINK_HDR || n > HIFI_LINK_MAX) return false;
	if (buf[0] != MAGIC0 || buf[1] != MAGIC1 || buf[2] != VERSION) return false;
	if (buf[3] < HIFI_LINK_PING || buf[3] > HIFI_LINK_DATA) return false;
	if ((size_t) (buf[4] | buf[5] << 8) != n) return false; /* truncated (or padded) */
	uint32_t seq = get32(buf + 10);
	for (size_t i = HIFI_LINK_HDR; i < n; i++)
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
		uint32_t bad = r->bad, pings = r->pings, stray = r->stray;
		hifi_link_rx_init(r);
		r->bad = bad;
		r->pings = pings;
		r->stray = stray;
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

/* the fit's sums with the window still open added, so a run's last second
 * counts too (without closing it: more pongs may land in it) */
bool hifi_link_tx_drift_ppm(const hifi_link_tx_t *t, double *ppm) {
	double n = t->windows, sx = t->sx, sy = t->sy, sxx = t->sxx, sxy = t->sxy;
	if (t->win_any) {
		int64_t x0 = t->windows ? t->x0_us : t->win_start_us, y0 = t->windows ? t->y0_us : t->win_offset_us;
		double x = (t->win_start_us - x0) / 1e6, y = (double) (t->win_offset_us - y0);
		n++;
		sx += x;
		sy += y;
		sxx += x * x;
		sxy += x * y;
	}
	if (n < 3) return false;
	double den = n * sxx - sx * sx;
	if (den <= 0) return false;
	*ppm = (n * sxy - sx * sy) / den; /* us per s */
	return true;
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

hifi_link_got_t hifi_link_receive(hifi_link_rx_t *rx, hifi_link_tx_t *tx, bool echo, bool pongs, const uint8_t *buf,
                                  size_t n, int64_t t_us, hifi_link_frame_t *pong) {
	hifi_link_frame_t f;
	if (!hifi_link_decode(buf, n, &f)) {
		hifi_link_rx_bad(rx);
		return HIFI_LINK_GOT_BAD;
	}
	if (echo && f.type == HIFI_LINK_PING) {
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
