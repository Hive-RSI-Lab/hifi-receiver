/*
 * Link measurement between two boards, whatever carries the frames
 * (ESP-NOW, a BLE GATT characteristic, UDP): the frame both ends speak and
 * the statistics each end keeps. Plain C, so the host tests run it.
 *
 * A pinger sends PING frames carrying its send time t1; the echo writes its
 * receive time t2 and its send time t3 into a PONG; the pinger notes t4 on
 * receipt. That gives the round trip (t4 - t1) - (t3 - t2) and, as NTP does,
 * the echo's clock offset ((t2 - t1) + (t3 - t4)) / 2. The lowest-delay
 * sample of each second goes into a least-squares line, whose slope is the
 * drift between the two crystals in ppm: what synced multiroom audio needs.
 *
 * A flooder sends DATA frames as fast as it is let; the sink counts what
 * arrived, duplicates, reordering and gaps, and the bytes per second.
 *
 * Frames carry a session number (a new run starts a new one), a sequence
 * number, its own length (a truncated frame is caught) and a payload filled
 * with a pattern derived from the sequence, so a corrupt frame is caught
 * rather than counted.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HIFI_LINK_HDR 38  /* magic 2, version 1, type 1, length 2, session 4, seq 4, t1 t2 t3 8 each */
#define HIFI_LINK_MAX 250 /* ESP-NOW's largest payload */
#define HIFI_LINK_WINDOW 1024 /* sequence numbers the duplicate check remembers */
#define HIFI_LINK_RTT_BIN_US 100
#define HIFI_LINK_RTT_BINS 1000 /* 0 to 100 ms; slower ones land in the last bin */

typedef enum { HIFI_LINK_PING = 1, HIFI_LINK_PONG = 2, HIFI_LINK_DATA = 3 } hifi_link_type_t;

typedef struct {
	uint8_t type;
	uint32_t session, seq;
	int64_t t1, t2, t3; /* microseconds on the clock of the board that wrote each */
	uint16_t len;       /* the whole frame, HIFI_LINK_HDR..HIFI_LINK_MAX */
} hifi_link_frame_t;

/* the frame's bytes, f->len of them (the payload filled); 0 when it doesn't fit */
size_t hifi_link_encode(const hifi_link_frame_t *f, uint8_t *buf, size_t cap);
/* false: not one of ours, or damaged */
bool hifi_link_decode(const uint8_t *buf, size_t n, hifi_link_frame_t *f);

typedef struct {
	uint32_t top; /* the highest sequence number seen + 1; 0 none */
	uint64_t bits[HIFI_LINK_WINDOW / 64];
} hifi_link_seen_t;

/* 0 new, 1 a duplicate, -1 too old to tell */
int hifi_link_seen(hifi_link_seen_t *s, uint32_t seq);

/* ------------------------------------------------------------ receiving end */

typedef struct {
	uint32_t session;
	uint32_t rx, dup, reorder, old, bad;
	uint32_t stray; /* DATA of another session while this one is live */
	uint32_t pings; /* PINGs answered (echo) */
	uint64_t bytes;
	int64_t first_us, last_us;
	hifi_link_seen_t seen;
} hifi_link_rx_t;

void hifi_link_rx_init(hifi_link_rx_t *r);
/* a DATA frame. The first frame's session is adopted; a frame of another
 * session starts the counts again only once this one has been silent for
 * HIFI_LINK_SESSION_IDLE_US (a late frame of the last run is counted as
 * stray, not taken for a new run: sessions are random, not ordered). */
#define HIFI_LINK_SESSION_IDLE_US 1000000
void hifi_link_rx_data(hifi_link_rx_t *r, const hifi_link_frame_t *f, int64_t now_us);
/* a frame that failed to decode */
void hifi_link_rx_bad(hifi_link_rx_t *r);
uint32_t hifi_link_rx_expected(const hifi_link_rx_t *r);
double hifi_link_rx_kbps(const hifi_link_rx_t *r);
/* the members of the sink's result, into an open JSON object (an hj_t *) */
void hifi_link_rx_json(const hifi_link_rx_t *r, void *hj);

/* An echo's answer: the PONG for a PING, t2 the time it arrived and t3 now */
void hifi_link_pong_for(const hifi_link_frame_t *ping, int64_t t2, int64_t t3, hifi_link_frame_t *pong);

/* ------------------------------------------------------------ sending end */

typedef struct {
	uint32_t session;
	uint32_t sent, send_fail; /* handed to the radio; refused by it */
	uint32_t acked, nacked;   /* the radio's own (MAC) acknowledgement */
	uint32_t pongs, dup, stray, bad;
	uint64_t bytes;
	int64_t first_us, last_us;
	/* round trips */
	uint32_t rtt_bins[HIFI_LINK_RTT_BINS];
	int64_t rtt_min_us, rtt_max_us, rtt_sum_us;
	/* clock offset and drift */
	int64_t win_start_us;          /* t1 of the current second's window */
	int64_t win_delay_us, win_offset_us;
	bool win_any;
	int64_t x0_us, y0_us;          /* the first window's t1 and offset: x, y are relative */
	double sx, sy, sxx, sxy;       /* least squares of offset (us) on time (s) */
	uint32_t windows;
	int64_t offset_us;             /* the newest window's best offset */
	int64_t offset_delay_us;
	hifi_link_seen_t seen;
} hifi_link_tx_t;

void hifi_link_tx_init(hifi_link_tx_t *t, uint32_t session);
/* a frame handed to the radio (ok) or refused by it, at now_us */
void hifi_link_tx_sent(hifi_link_tx_t *t, size_t len, bool ok, int64_t now_us);
/* the radio's send report: acknowledged by the peer or not */
void hifi_link_tx_acked(hifi_link_tx_t *t, bool acked);
/* a PONG that arrived at t4 */
void hifi_link_tx_pong(hifi_link_tx_t *t, const hifi_link_frame_t *f, int64_t t4);
/* round trip at percentile p (0..100), in microseconds; -1 none */
double hifi_link_tx_rtt_pct(const hifi_link_tx_t *t, double p);
/* drift of the echo's clock against ours in ppm; false: under 3 windows */
bool hifi_link_tx_drift_ppm(const hifi_link_tx_t *t, double *ppm);
double hifi_link_tx_kbps(const hifi_link_tx_t *t);
void hifi_link_tx_json(const hifi_link_tx_t *t, void *hj);

/* "30:76:f5:2d:74:3c", "30-76-..." or "3076f52d743c" (a board's device id) */
bool hifi_link_parse_mac(const char *s, uint8_t mac[6]);
