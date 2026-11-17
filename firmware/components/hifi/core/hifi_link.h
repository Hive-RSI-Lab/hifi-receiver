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
 *
 * Every frame ends with a tag: HMAC-SHA256 of the header under the run's key,
 * cut to HIFI_LINK_TAG bytes. The payload needs no covering: it is a function
 * of seq and the length, both in the header, and decode checks every byte.
 * A frame whose tag fails is bad before its session or seq is looked at, so a
 * stranger in radio range (any MAC, any seq) can't make a frame of their own:
 * all a forged frame moves is bad. The key is given at the start of each run
 * (never a constant in the code); the ipad and opad states are computed once
 * per key, so a frame costs two SHA-256 blocks whatever its length.
 *
 * What the tag doesn't stop: a stranger sending again, within the same run,
 * a genuine frame they heard (a replay; across runs it fails, as each run has
 * a fresh key). A replayed DATA frame is a dup (or old, or fills a gap the
 * flooder's real frame left: lost goes down by one, reorder up). A replayed
 * PING is answered once only: the echo remembers the seqs it answered and
 * counts a repeat as ping_dup. A replayed PONG is a dup at the pinger, unless
 * the pinger missed the real one: then it is taken, late, and lowers the
 * loss, raises the round trip and skews that second's clock offset. A
 * per-run counter in the tagged header can't stop that last case without a
 * receive window tighter than the radio's own jitter, so the lab says it
 * rather than hides it.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hifi_sha256.h"

#define HIFI_LINK_HDR 38  /* magic 2, version 1, type 1, length 2, session 4, seq 4, t1 t2 t3 8 each */
#define HIFI_LINK_TAG 8   /* the HMAC-SHA256 tag at the end of the frame, truncated */
#define HIFI_LINK_MIN (HIFI_LINK_HDR + HIFI_LINK_TAG) /* the shortest frame */
#define HIFI_LINK_MAX 250 /* ESP-NOW's largest payload */
#define HIFI_LINK_KEY 16  /* bytes of the run's key */
/* the MTU the BLE link asks for, and the largest value a write or a
 * notification then carries (the ATT header takes 3) */
#define HIFI_LINK_BLE_MTU 247
#define HIFI_LINK_BLE_PAYLOAD (HIFI_LINK_BLE_MTU - 3)
#define HIFI_LINK_WINDOW 1024 /* sequence numbers the duplicate check remembers */
#define HIFI_LINK_RTT_BIN_US 100
#define HIFI_LINK_RTT_BINS 1000 /* 0 to 100 ms; slower ones land in the last bin */
#define HIFI_LINK_RTT_MAX_US 10000000 /* a PONG slower than this answers no ping of the run: bad */

typedef enum { HIFI_LINK_PING = 1, HIFI_LINK_PONG = 2, HIFI_LINK_DATA = 3 } hifi_link_type_t;

typedef struct {
	uint8_t type;
	uint32_t session, seq;
	int64_t t1, t2, t3; /* microseconds on the clock of the board that wrote each */
	uint16_t len;       /* the whole frame with its tag, HIFI_LINK_MIN..HIFI_LINK_MAX */
} hifi_link_frame_t;

/* the run's key, ready to tag with: the HMAC inner and outer states. Until
 * key_init (and after key_wipe) it isn't ready, and encode, decode and forge
 * refuse it: an all-zero state is a key anyone could compute. */
typedef struct {
	hifi_sha256_t in, out;
	bool ready;
} hifi_link_key_t;

void hifi_link_key_init(hifi_link_key_t *k, const uint8_t key[HIFI_LINK_KEY]);
void hifi_link_key_wipe(hifi_link_key_t *k);
/* zeroes n bytes in a way the compiler keeps, for key bytes on their way out */
void hifi_link_wipe(void *p, size_t n);
/* 32 hex digits; false (key untouched) for anything else */
bool hifi_link_parse_key(const char *s, uint8_t key[HIFI_LINK_KEY]);
/* the tag of a frame's header (its first HIFI_LINK_HDR bytes) */
void hifi_link_tag(const hifi_link_key_t *k, const uint8_t *hdr, uint8_t tag[HIFI_LINK_TAG]);

/* the frame's bytes, f->len of them (payload filled, tag at the end); 0 when
 * it doesn't fit */
size_t hifi_link_encode(const hifi_link_key_t *k, const hifi_link_frame_t *f, uint8_t *buf, size_t cap);
/* false: not one of ours, damaged, or not tagged with this key */
bool hifi_link_decode(const hifi_link_key_t *k, const uint8_t *buf, size_t n, hifi_link_frame_t *f);
/* The lab's forger (lab_forge.c): a frame heard on the air, its seq moved on
 * by jump, its payload made to match and its tag made again under k (the
 * forger's own key, or the run's for the control). The tag of the frame heard
 * isn't checked: the forger doesn't know the key. jump 0 is a replay: the
 * frame as heard, its own tag kept. n bytes into out (cap at least n); 0 when
 * it isn't shaped like one of ours. */
size_t hifi_link_forge(const hifi_link_key_t *k, const uint8_t *in, size_t n, uint32_t jump, uint8_t *out, size_t cap);

typedef struct {
	uint32_t top; /* the highest sequence number seen + 1; 0 none */
	uint64_t bits[HIFI_LINK_WINDOW / 64];
} hifi_link_seen_t;

/* 0 new, 1 a duplicate, -1 too old to tell (and seq UINT32_MAX, which
 * would wrap top) */
int hifi_link_seen(hifi_link_seen_t *s, uint32_t seq);

/* An ESP-NOW frame as the air carries it (an 802.11 action frame: the 24-byte
 * header, category 127 and Espressif's OUI, 4 random bytes, then a vendor
 * element: 221, length, OUI, type 4, version, and the payload), n bytes
 * without the FCS. The payload's length when it was sent by mac and is long
 * enough to be a link frame (it starts at HIFI_LINK_ESPNOW_BODY); 0 if not. */
#define HIFI_LINK_ESPNOW_BODY 39
#define HIFI_LINK_ESPNOW_MAX (HIFI_LINK_ESPNOW_BODY + HIFI_LINK_MAX)
size_t hifi_link_espnow_body(const uint8_t *p, size_t n, const uint8_t mac[6]);

/* ------------------------------------------------------------ receiving end */

typedef struct {
	uint32_t session;
	uint32_t rx, dup, reorder, old, bad;
	uint32_t stray; /* DATA of another session while this one is live */
	uint32_t pings; /* PINGs answered (echo) */
	uint32_t ping_dup; /* PINGs already answered: a replay gets no second PONG */
	uint64_t bytes;
	int64_t first_us, last_us;
	hifi_link_seen_t seen;
	/* the session of the PINGs in ping_seen. A PING from another session
	 * resets it, so the dedup holds only while each run has a fresh key (the
	 * lab's drivers make one per run): with a reused key, replays of two runs'
	 * PINGs taken in turn would each be answered. */
	uint32_t ping_session;
	hifi_link_seen_t ping_seen;
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
/* a PONG that arrived at t4. One whose times no echo could have written
 * (any outside [0, 2^62), t1 after t4, t3 before t2, a round trip below 0
 * or over HIFI_LINK_RTT_MAX_US) counts as bad, before its seq is looked at. */
void hifi_link_tx_pong(hifi_link_tx_t *t, const hifi_link_frame_t *f, int64_t t4);
/* round trip at percentile p (0..100), in microseconds; -1 none */
double hifi_link_tx_rtt_pct(const hifi_link_tx_t *t, double p);
/* drift of the echo's clock against ours in ppm; false: under 3 windows */
bool hifi_link_tx_drift_ppm(const hifi_link_tx_t *t, double *ppm);
double hifi_link_tx_kbps(const hifi_link_tx_t *t);
void hifi_link_tx_json(const hifi_link_tx_t *t, void *hj);

/* ------------------------------------------------------- one received frame */

typedef enum {
	HIFI_LINK_GOT_USED,  /* counted (DATA, a PONG) */
	HIFI_LINK_GOT_REPLY, /* a PING: send *pong back (its t3 stamped at the send) */
	HIFI_LINK_GOT_BAD,   /* not one of ours, or damaged: counted in rx->bad */
	HIFI_LINK_GOT_OTHER, /* a good frame this end has no use for (or a PING it answered already) */
} hifi_link_got_t;

/* A radio callback's copy of a received value into its queue item's buffer
 * (cap bytes): the length copied, 0 (nothing copied) when there is nothing
 * or it is longer than cap. The item then goes to hifi_link_receive, where
 * a length of 0 counts as bad. */
size_t hifi_link_take(uint8_t *dst, size_t cap, const uint8_t *src, size_t n);

/* What an end does with the bytes of one frame from the radio, whatever
 * carried them (n as received: never trusted). An echo end answers PINGs and
 * counts DATA into rx; an end that takes PONGs (pongs) counts them into tx.
 * rx is always needed, tx only with pongs. t_us: the arrival time. A frame
 * not tagged with k is bad, whatever it says. */
hifi_link_got_t hifi_link_receive(const hifi_link_key_t *k, hifi_link_rx_t *rx, hifi_link_tx_t *tx, bool echo, bool pongs, const uint8_t *buf,
                                  size_t n, int64_t t_us, hifi_link_frame_t *pong);

/* "30:76:f5:2d:74:3c", "30-76-..." or "3076f52d743c" (a board's device id) */
bool hifi_link_parse_mac(const char *s, uint8_t mac[6]);
