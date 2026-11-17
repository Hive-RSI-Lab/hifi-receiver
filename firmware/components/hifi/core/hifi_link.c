#include "hifi_link.h"

#include <string.h>

#include "hifi_json.h"

#define MAGIC0 'H'
#define MAGIC1 'L'
#define VERSION 1

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

/* the payload byte at offset i of a frame with this sequence number */
static uint8_t pattern(uint32_t seq, size_t i) { return (uint8_t) ((seq * 31u + i * 7u) ^ 0x5a); }

size_t hifi_link_encode(const hifi_link_frame_t *f, uint8_t *buf, size_t cap) {
	size_t n = f->len;
	if (n < HIFI_LINK_HDR || n > HIFI_LINK_MAX || n > cap) return 0;
	buf[0] = MAGIC0;
	buf[1] = MAGIC1;
	buf[2] = VERSION;
	buf[3] = f->type;
	put32(buf + 4, f->session);
	put32(buf + 8, f->seq);
	put64(buf + 12, f->t1);
	put64(buf + 20, f->t2);
	put64(buf + 28, f->t3);
	for (size_t i = HIFI_LINK_HDR; i < n; i++) buf[i] = pattern(f->seq, i);
	return n;
}

bool hifi_link_decode(const uint8_t *buf, size_t n, hifi_link_frame_t *f) {
	if (!buf || n < HIFI_LINK_HDR || n > HIFI_LINK_MAX) return false;
	if (buf[0] != MAGIC0 || buf[1] != MAGIC1 || buf[2] != VERSION) return false;
	if (buf[3] < HIFI_LINK_PING || buf[3] > HIFI_LINK_DATA) return false;
	uint32_t seq = get32(buf + 8);
	for (size_t i = HIFI_LINK_HDR; i < n; i++)
		if (buf[i] != pattern(seq, i)) return false;
	f->type = buf[3];
	f->session = get32(buf + 4);
	f->seq = seq;
	f->t1 = get64(buf + 12);
	f->t2 = get64(buf + 20);
	f->t3 = get64(buf + 28);
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
	if (r->rx == 0 || f->session != r->session) {
		uint32_t bad = r->bad, pings = r->pings;
		hifi_link_rx_init(r);
		r->bad = bad;
		r->pings = pings;
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
	int s = hifi_link_seen(&t->seen, f->seq);
	if (s != 0) {
		t->dup++;
		return;
	}
	int64_t delay = (t4 - f->t1) - (f->t3 - f->t2);
	if (delay < 0 || f->t1 > t4) {
		t->bad++;
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

bool hifi_link_tx_drift_ppm(const hifi_link_tx_t *t, double *ppm) {
	double n = t->windows;
	if (t->windows < 3) return false;
	double den = n * t->sxx - t->sx * t->sx;
	if (den <= 0) return false;
	*ppm = (n * t->sxy - t->sx * t->sy) / den; /* us per s */
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
		hj_int(j, "windows", t->windows);
		double ppm;
		if (hifi_link_tx_drift_ppm(t, &ppm))
			hj_num(j, "drift_ppm", ppm, 3);
		else
			hj_null(j, "drift_ppm");
		hj_obj_end(j);
	}
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
