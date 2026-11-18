/*
 * Party mode: several Lites play one stream in sync, with no server. One
 * board leads (it decodes whatever source it plays) and sends its output, as
 * 16-bit stereo PCM, to the others by UDP; the followers play it on their own
 * DACs at the same moment. The design and its numbers are in
 * projects/hifi-lite-lab/party-mode.md. This file is the part that needs no
 * radio or I2S, so the host tests run it:
 *
 * The packet. Three types share one header: AUDIO (HIFI_PARTY_FRAMES frames
 * of stream index k onward), CLOCK (stream frame k plays on the leader's DAC
 * at Wi-Fi TSF time t, and the stream runs at rate frames per 1000 TSF
 * seconds; the payload is the BSSID whose TSF that is) and NACK (the follower
 * asks again for the AUDIO packets whose k the payload lists). AUDIO packets
 * start on a multiple of HIFI_PARTY_FRAMES, so k names the packet. Every
 * packet ends with an HMAC-SHA256 tag over the header and the payload, under
 * the group's key, cut to HIFI_PARTY_TAG bytes: unlike hifi_link's frames the
 * payload is music, not a pattern decode can check, so the tag covers it. A
 * packet whose tag fails is dropped before anything in it is used (the magic
 * is checked first only as a cheap filter for stray datagrams).
 *
 * The clock fit. Each board stamps two things against its own esp_timer: the
 * moment a frame index leaves for the DAC (from the I2S writes that blocked,
 * see hifi_party_dac_sample) and the Wi-Fi TSF (read between two esp_timer
 * reads). Both samples arrive late by a random amount and never early, so
 * the fit keeps the best sample of each window (the lowest quality value q:
 * the lateness against a nominal line, or the width of the bracket), and
 * draws a least-squares line through the last HIFI_PARTY_FIT_MAX of them.
 * That is the lower envelope; a window whose best sample still sits far off
 * the line (a whole second of late wakes) is dropped from the line.
 *
 * The jitter buffer. A follower keeps about a second of the stream, one slot
 * per AUDIO packet, and plays from it by stream index. A packet behind the
 * play position is old; one already held is a duplicate (so a replayed packet
 * changes nothing); one a buffer or more ahead moves the window forward. A
 * gap that a later packet reveals is NACKed HIFI_PARTY_NACK1_US after it was
 * seen, and once more at HIFI_PARTY_NACK2_US. A frame still missing when it
 * is played is concealed: the last HIFI_PARTY_CONCEAL frames are repeated
 * while fading out, then silence, and the stream fades back in when the
 * packets return.
 *
 * The controller. For each block the follower is about to write, the caller
 * works out the stream index the leader's DAC plays at the moment this
 * block's first frame reaches the follower's DAC (the target). Within one
 * frame it plays on; one frame or more behind it drops one frame, ahead it
 * repeats one, at the quietest frame of the block and at most once per
 * HIFI_PARTY_SLIP_EVERY frames (488 ppm, far beyond any crystal); more than
 * the jump limit (5 ms) off, as at the start or after the clock was lost, it
 * jumps to the target with a short crossfade. The leader sends its output
 * after its volume and fades, so the group follows the leader's volume and a
 * follower plays at unity (gain stays for a per-board trim).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hifi_link.h"

#define HIFI_PARTY_MAGIC0 'H'
#define HIFI_PARTY_MAGIC1 'P'
#define HIFI_PARTY_VERSION 1
#define HIFI_PARTY_HDR 38      /* magic 2, version 1, type 1, length 2, group 4, seq 4, k 8, t 8, rate 4, n 2, flags 2 */
#define HIFI_PARTY_TAG 8
#define HIFI_PARTY_FRAMES 352  /* frames per AUDIO packet: 8 ms at 44.1 kHz, 1408 bytes */
#define HIFI_PARTY_FRAME_BYTES 4 /* 16-bit stereo */
#define HIFI_PARTY_NACK_MAX 16 /* packets one NACK asks for */
#define HIFI_PARTY_MAX (HIFI_PARTY_HDR + HIFI_PARTY_FRAMES * HIFI_PARTY_FRAME_BYTES + HIFI_PARTY_TAG) /* 1454: one UDP datagram, no IP fragments */
#define HIFI_PARTY_PORT 4951   /* the UDP port both ends use, on the LAN only */

#define HIFI_PARTY_FIT_MAX 32  /* windows the clock fit keeps */
#define HIFI_PARTY_NACK1_US 20000
#define HIFI_PARTY_NACK2_US 60000
#define HIFI_PARTY_CONCEAL 220 /* 5 ms at 44.1 kHz: the stretch repeated over a gap, and the jump's crossfade */
#define HIFI_PARTY_FADE_IN 64
#define HIFI_PARTY_SLIP_EVERY 2048

/* ANNOUNCE and JOIN find and keep a group (hifi_party_group.h): ANNOUNCE
 * (to the subnet's broadcast, under the announce key) has k the term, t the
 * leader's TSF when sent, n its followers, flags F_PICKED, and a payload of
 * HIFI_PARTY_ANNOUNCE_LEN (the leader's IPv4, three follower IPv4s, 0 for an
 * empty place, and its name, 16 bytes, NUL padded); JOIN (to the leader,
 * under the group key) has t the follower's TSF, flags F_LEAVE, and the
 * follower's IPv4 as its payload. IPv4s are in network order. */
typedef enum {
	HIFI_PARTY_AUDIO = 1,
	HIFI_PARTY_CLOCK = 2,
	HIFI_PARTY_NACK = 3,
	HIFI_PARTY_ANNOUNCE = 4,
	HIFI_PARTY_JOIN = 5,
} hifi_party_type_t;

#define HIFI_PARTY_FOLLOWERS 3 /* the most a leader sends to (the airtime limit) */
#define HIFI_PARTY_NAME 16
#define HIFI_PARTY_ANNOUNCE_LEN (4 + 4 * HIFI_PARTY_FOLLOWERS + HIFI_PARTY_NAME)
#define HIFI_PARTY_F_PICKED 1 /* ANNOUNCE: the lead was picked by hand */
#define HIFI_PARTY_F_LEAVE 1  /* JOIN: the follower leaves */

/* flags of an AUDIO packet */
#define HIFI_PARTY_F_SILENCE 1 /* silence (the leader's player paused or idle): n frames of it, no samples sent */
#define HIFI_PARTY_F_MARKS 2   /* the stream carries the test tone's markers (a follower looks for them) */
/* flags of a CLOCK */
#define HIFI_PARTY_F_NOAUDIO 1 /* the leader sends no audio (its output stopped, or a rate party can't carry) */

typedef struct {
	uint8_t type;
	uint32_t group, seq;
	int64_t k;     /* AUDIO: the stream index of the first frame. CLOCK: k_ref */
	int64_t t;     /* CLOCK: the TSF (µs) at which frame k plays on the leader's DAC */
	uint32_t rate; /* CLOCK: frames per 1000 s of TSF. AUDIO: the nominal sample rate in Hz */
	uint16_t n;    /* AUDIO: frames in the payload. NACK: k values in the payload */
	uint16_t flags;
} hifi_party_pkt_t;

/* Writes the packet with its payload (AUDIO: n frames, none with
 * F_SILENCE; CLOCK: the 6-byte
 * BSSID; NACK: n little-endian k values, 8 bytes each; ANNOUNCE and JOIN:
 * as above) and tag into buf.
 * Returns its length, or 0 if the key isn't ready, cap is too small or the
 * payload doesn't fit the type. */
size_t hifi_party_encode(const hifi_link_key_t *key, const hifi_party_pkt_t *p, const void *payload, uint8_t *buf, size_t cap);

typedef enum {
	HIFI_PARTY_OK = 0,
	HIFI_PARTY_E_SHORT = -1,   /* shorter than a header and tag, or than its own length */
	HIFI_PARTY_E_MAGIC = -2,   /* not a party packet, or another version */
	HIFI_PARTY_E_TAG = -3,     /* the tag fails: not from a board with the group key, or changed on the way */
	HIFI_PARTY_E_FORMAT = -4,  /* tagged right but its fields don't fit its type */
	HIFI_PARTY_E_KEY = -5,     /* no key yet */
} hifi_party_err_t;

/* Checks the tag first, then the fields. On HIFI_PARTY_OK fills p and points
 * *payload into buf (*plen bytes). */
int hifi_party_decode(const hifi_link_key_t *key, const uint8_t *buf, size_t n, hifi_party_pkt_t *p, const uint8_t **payload, size_t *plen);

/* the k values of a NACK payload, and a CLOCK's BSSID */
int64_t hifi_party_nack_k(const uint8_t *payload, int i);

/* ------------------------------------------------------------ clock fit */

typedef struct {
	int64_t win_us;   /* one sample kept per window of this length */
	double reject;    /* a window more than this far (y units) above the line (or either side, with both_sides) is left out */
	bool both_sides;  /* ...or below it: set after init for a fit whose y can step either way (the TSF) */
	int min_points;   /* windows needed before the line is used */
	/* the window being filled */
	bool have_cur;
	int64_t cur_win, cur_x, cur_y;
	double cur_q;
	/* the kept windows, a ring */
	int64_t px[HIFI_PARTY_FIT_MAX], py[HIFI_PARTY_FIT_MAX];
	int n, head, cap;
	/* the line: y = yb + my + slope * (x - xb - mx) */
	bool ok;
	int64_t xb, yb;
	double mx, my, slope, rms;
	int used, rejected;
} hifi_party_fit_t;

/* cap windows (at most HIFI_PARTY_FIT_MAX) of win_us each */
void hifi_party_fit_init(hifi_party_fit_t *f, int64_t win_us, int cap, int min_points, double reject);
/* A sample (x, y) of quality q (lower is better) taken at time now (µs).
 * Returns true when it closed a window and the line was drawn again. */
bool hifi_party_fit_add(hifi_party_fit_t *f, int64_t x, int64_t y, double q, int64_t now_us);
/* Takes the newest closed window back out of the line (one off it that the
 * caller would keep out) and draws the line again. */
void hifi_party_fit_drop_newest(hifi_party_fit_t *f);
double hifi_party_fit_y(const hifi_party_fit_t *f, double x);
double hifi_party_fit_x(const hifi_party_fit_t *f, double y);

/* An I2S write of frames [w0, w1) that began at t0 and returned at t1 (µs).
 * IDF 4.4's i2s_write (and i2s_write_expand) takes a fresh buffer of
 * buf_frames frames from the DMA's free queue at each multiple of
 * buf_frames, waiting when the queue is empty. The last boundary the write
 * crossed, the largest multiple of buf_frames in [w0, w1), is the sample's x
 * and t1 its y: the write returned no earlier than the DMA freed that
 * buffer, and the boundary's frame plays a whole ring of buffers after that,
 * as long as the DMA never ran dry. So every sample is late or exact, never
 * early, and the fit's lower envelope finds the line; the delay is the same
 * on every Lite running this code, so it cancels between boards. min_us > 0
 * also leaves out calls that took less (they found room and didn't wait), a
 * filter that only saves the fit some late samples. */
bool hifi_party_dac_sample(int64_t w0, int64_t w1, int64_t t0, int64_t t1, int buf_frames, int64_t min_us, int64_t *x);

/* the stream index that plays at TSF time t, from a CLOCK's k_ref, t_ref and rate */
double hifi_party_target(int64_t k_ref, int64_t t_ref, uint32_t rate_mhz, double t);

/* ------------------------------------------------------------ jitter buffer */

typedef enum {
	HIFI_PARTY_PUT_OK = 0,
	HIFI_PARTY_PUT_DUP = 1,  /* held already */
	HIFI_PARTY_PUT_OLD = 2,  /* behind the play position */
	HIFI_PARTY_PUT_BAD = 3,  /* not a whole packet on a packet boundary */
} hifi_party_put_t;

typedef struct {
	uint32_t *frames; /* nslots * HIFI_PARTY_FRAMES frames, the caller's (PSRAM on the board) */
	int64_t *have;    /* per slot: the packet it holds, or -1 */
	int64_t *miss;    /* per slot: the packet seen missing there, or -1 */
	int64_t *miss_at; /* and when the gap was seen */
	uint8_t *nacks;   /* and how often it was asked for */
	int nslots;
	int64_t floor;    /* the lowest packet still wanted (k / HIFI_PARTY_FRAMES) */
	int64_t top;      /* the highest packet received, or -1 */
	/* concealment */
	uint32_t hist[HIFI_PARTY_CONCEAL];
	int hist_pos;
	int gap_run, fade_in;
	/* counters */
	uint32_t got, dup, old, bad, skipped, nacked, missing_frames, gaps;
} hifi_party_jb_t;

/* bytes of caller memory a buffer of nslots needs */
size_t hifi_party_jb_bytes(int nslots);
/* lays the buffer out in mem (hifi_party_jb_bytes(nslots) bytes, 8-aligned) */
void hifi_party_jb_init(hifi_party_jb_t *jb, void *mem, int nslots);
/* frames NULL: a silence packet, held as n frames of zeros */
int hifi_party_jb_put(hifi_party_jb_t *jb, int64_t k, const void *frames, int n, int64_t now_us);
/* fills out with stream frames [k, k + n), concealing the missing ones;
 * returns how many were missing. Packets more than one behind k's are let go
 * (one is kept for a repeated frame or a short jump back). */
int hifi_party_jb_read(hifi_party_jb_t *jb, int64_t k, uint32_t *out, int n);
/* the packets (their k) to NACK now; at most max */
int hifi_party_jb_nacks(hifi_party_jb_t *jb, int64_t now_us, int64_t *k, int max);

/* ------------------------------------------------------------ controller */

typedef struct {
	int64_t next;      /* the stream index the follower plays next */
	bool started;
	int jump_frames;   /* off by this much or more: jump */
	int64_t since_slip;
	uint32_t drops, repeats, jumps;
	double last_err;
	uint32_t xfade[HIFI_PARTY_CONCEAL];
} hifi_party_ctl_t;

void hifi_party_ctl_init(hifi_party_ctl_t *c, int rate);
/* Writes n frames for the block whose first frame should be stream index
 * target, from the jitter buffer, slipping or jumping as needed, and applies
 * the follower's gain (65536 = unity, per channel). */
void hifi_party_render(hifi_party_ctl_t *c, hifi_party_jb_t *jb, double target, uint32_t *out, int n, int32_t gain_l, int32_t gain_r);
/* A block with no target (no fresh CLOCK, the fits not ready): what played
 * last fades out, then silence, and the next render starts afresh where the
 * leader is, fading in. A hard cut (zeros) would click. */
void hifi_party_mute(hifi_party_ctl_t *c, hifi_party_jb_t *jb, uint32_t *out, int n, int32_t gain_l, int32_t gain_r);
/* the gain alone, saturating, as squeezelite's 16-bit output applies it */
void hifi_party_gain(uint32_t *frames, int n, int32_t gain_l, int32_t gain_r);

/* ------------------------------------------------------------ leader's resend history */

typedef struct {
	uint8_t *pkts;   /* nslots * HIFI_PARTY_MAX bytes, the caller's */
	int64_t *k;      /* per slot: the AUDIO packet held, or -1 */
	uint16_t *len;
	int nslots;
} hifi_party_hist_t;

size_t hifi_party_hist_bytes(int nslots);
void hifi_party_hist_init(hifi_party_hist_t *h, void *mem, int nslots);
void hifi_party_hist_put(hifi_party_hist_t *h, int64_t k, const uint8_t *pkt, size_t len);
/* the encoded packet for k, or NULL if it is no longer held */
const uint8_t *hifi_party_hist_get(const hifi_party_hist_t *h, int64_t k, size_t *len);

/* ------------------------------------------------------------ bursts */

/* A short log of trouble in bursts, for telling one side's problems from the
 * other's: the leader logs its failed sends (with the errno), a follower the
 * frames it had to conceal. Events closer than HIFI_PARTY_BURST_GAP_MS join
 * one burst; the newest HIFI_PARTY_BURSTS are kept. Times are the board's
 * own milliseconds since boot. */
#define HIFI_PARTY_BURSTS 12
#define HIFI_PARTY_BURST_GAP_MS 250

typedef struct {
	uint32_t at_ms, dur_ms, count;
	int32_t code; /* the last nonzero code seen in it (an errno), or 0 */
} hifi_party_burst_t;

typedef struct {
	hifi_party_burst_t b[HIFI_PARTY_BURSTS];
	int head, n;
	uint32_t total; /* bursts ever started, so a reader sees how many fell off */
} hifi_party_bursts_t;

void hifi_party_bursts_add(hifi_party_bursts_t *r, uint32_t now_ms, uint32_t count, int32_t code);
/* the i-th newest burst (0 = the newest), or NULL past the end */
const hifi_party_burst_t *hifi_party_bursts_get(const hifi_party_bursts_t *r, int i);
