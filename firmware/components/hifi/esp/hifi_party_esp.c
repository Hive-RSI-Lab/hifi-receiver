/*
 * Party mode you can switch on (projects/hifi-lite-lab/party-mode.md, section
 * 8). Boards with the Hive's party secret find each other on the LAN: the
 * first switched on (or the one picked, or the one whose own source starts)
 * leads and sends what its DAC plays, 300 ms ahead, to up to
 * HIFI_PARTY_FOLLOWERS followers, which play it at the moment the leader
 * does, both timed against the access point's Wi-Fi TSF. Who leads and who
 * follows is core/hifi_party_group.c; the packet, the jitter buffer and the
 * controller are core/hifi_party.c; the clocks are core/hifi_party_sync.c.
 * All three run on the host (firmware/test). This file is the radio, the
 * output hooks (patch 0021), the output itself (patch 0032) and Bluetooth
 * (patch 0033).
 *
 *   api party                         on, role, leader, followers, why
 *   api party '{"op":"on"}'           also off, lead
 *   api party/stats                   the sync numbers and the heap
 *   api party '{"op":"key","secret":"<64 hex>"}'   the Hive only
 *
 * Security. One UDP socket bound to the station's own address, port
 * HIFI_PARTY_PORT, broadcast to the subnet for announces (never routed) and
 * unicast to boards on the same subnet; nothing is opened on the router (no
 * UPnP, no multicast group). Only datagrams from that port on this subnet
 * are looked at; before the HMAC, hifi_party_group_admit caps what strangers
 * cost. Every packet is tagged with a key made from the Hive's secret, so a
 * board without it can neither join nor be joined. The secret is set only by
 * the Hive or over USB (the LAN gets 403, and LAN config refuses every hifi_*
 * key), is never logged or shown (only a one-byte id), and the keys made
 * from it are wiped when party goes off.
 */
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_task.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "nvs_utilities.h"
#include "platform_config.h"

#include "hifi_api.h"
#include "hifi_board.h"
#include "hifi_esp.h"
#include "hifi_json.h"
#include "hifi_link.h"
#include "hifi_party.h"
#include "hifi_party_group.h"
#include "hifi_party_sync.h"

static const char *TAG = "party";

/* squeezelite-esp32, through our patches; weak as in hifi_main.c */
extern void hifi_party_sink_start(uint32_t rate) __attribute__((weak));
extern void hifi_sink_stop(void) __attribute__((weak));
extern int hifi_sink_source(void) __attribute__((weak));
extern bool hifi_bt_sink_set(bool on) __attribute__((weak));
extern bool hifi_bt_sink_is_off(void) __attribute__((weak));

#define RATE 44100
#define SRC_BT 2
#define SRC_PARTY 7           /* hifi_sink_source(): the output plays the party stream */
#define DELAY 13230           /* a leader with followers plays its stream this late (300 ms): their head start */
#define XFADE 441             /* 10 ms: the crossfade when the leader's delay changes */
#define ALONE_US 10000000     /* a leader with no followers this long plays without the delay again */
#define RING 65536            /* the leader's stream, 1.49 s; a power of two, so the per-frame index is a mask */
#define RI(x) ((uint32_t) (x) & (RING - 1))
#define TONE_N 400            /* one period of the test tone: 44100 / 400 = 110.25 Hz */
#define JB_SLOTS 128          /* the follower's jitter buffer, ~1 s */
#define HIST_SLOTS 128        /* the leader's resend history, ~1 s */
#define CLOCK_EVERY_US 200000
#define NOAUDIO_US 300000     /* no fresh CLOCK this long: the leader says it sends no audio */
#define TSF_EVERY_US 50000
#define TICK_US 20000
#define MARKS 64
#define OFFS 4096
#define STACK 6144
#define SEND_GUARD 2048       /* the largest block the output writes */
#define SEND_BURST 8          /* AUDIO packets per pass (64 ms of stream): the worker then blocks, so IDLE1 runs */
#define LATE_MARGIN 512       /* a packet sent less than this (11.6 ms: a 256-frame DMA block and the air) before its play time comes too late */
#define RESEND_MAX 24         /* NACKed packets waiting to go again, earliest play time first */
#define RCVBUF (8 * HIFI_PARTY_MAX)
#define HEAP_FLOOR 10240      /* internal RAM under this: no board leads (the leader's sends need it) */
#define GRACE_US 1000000      /* after the party output starts: the source is not yet the party's */
#define MARKS_FOR_US 3000000  /* a follower looks for markers while the stream says it carries them */

/* squeezelite's types, for the hooks' signatures (patch 0021) */
typedef uint8_t u8_t;
typedef uint32_t u32_t;
typedef uint32_t frames_t;

/* (+8191, -8191) as squeezelite's 16-bit stereo frame: left in the low half */
#define MARKER (((uint32_t) (uint16_t) (int16_t) -8191 << 16) | (uint16_t) 8191)

typedef struct {
	int64_t k;
	double tsf;
} mark_t;

/* Made once and kept, all in PSRAM (the Lite has ~13 KB of internal RAM
 * free): a hook still running as party goes off never touches freed memory. */
typedef struct {
	hifi_party_sync_t sync;
	hifi_party_jb_t jb;
	hifi_party_ctl_t ctl;
	hifi_party_hist_t hist;
	hifi_party_group_t g;
	mark_t marks[MARKS];
	float offs[OFFS];
	uint8_t pkt[HIFI_PARTY_MAX], rx[HIFI_PARTY_MAX + 64];
	uint32_t frames[HIFI_PARTY_FRAMES];
	int64_t nk[HIFI_PARTY_NACK_MAX];
	uint8_t nack_pl[HIFI_PARTY_NACK_MAX * 8];
	uint8_t secret[HIFI_PARTY_SECRET];
	hifi_link_key_t gkey, akey, lkey; /* the group's, the announces', a leave's */
	uint32_t peers[HIFI_PARTY_FOLLOWERS];
} st_t;

static st_t *st;
static uint32_t *ring;
static void *jb_mem, *hist_mem;
static SemaphoreHandle_t gmx; /* st->g, the secret and the keys: taken before mx */
static SemaphoreHandle_t mx;  /* what the hooks and the worker share */
static TaskHandle_t worker;
static StaticTask_t worker_tcb;

/* the switch: the API sets these and wakes the worker */
static volatile bool want_on, restart, have_secret, tone_on;
/* the test tone took an idle output at party's rate (the op "tone"), to give back */
static bool tone_held;
static volatile int pend_do;
static volatile bool active;   /* the worker has party on */
static volatile uint32_t ticked_ms;
static int64_t saved_term;
static bool saved_pick;
static char name[HIFI_PARTY_NAME + 1];
static volatile uint32_t party_gain = 65536;

/* the hooks' view of the group, under mx */
static volatile bool hooks_on;
static hifi_party_role_t hrole;
static uint32_t hgroup;

/* from the output task's restart hook, on or not */
static volatile int boot_buf_frames;
static int buf_frames;

/* the DAC side, under mx */
static int64_t w_pos;
static int64_t s_pos;
static uint32_t s_sec, s_tone;
static int16_t tone_tab[TONE_N];
static int64_t kw_off;
static uint32_t prev_out;
static frames_t last_block;
static int cur_delay, old_delay, want_delay, xfade;
static int64_t alone_since;
static uint32_t wrong_rate, blocks, muted, output_lost, unaligned, retaken;
static int64_t last_clock_us;
static bool clk_ok;
static int64_t clk_k, clk_t;
static uint32_t clk_rate, clk_seq;
static uint8_t clk_bssid[6];
static int marks_head, marks_n, offs_n;
static int64_t last_mark_k;
static uint32_t marks_missed;
static volatile int64_t marks_seen_us;

/* the worker's own */
static int sock = -1;
static uint32_t self_ip, self_mask;
static uint8_t my_bssid[6];
static bool have_bssid;
static uint32_t tx_seq;
static bool sending;
static int64_t k_sent;
static uint32_t clk_sent_seq;
static int64_t clk_sent_us, noaudio_us, grace_until, retake_at;
static uint32_t sent, send_fail, send_skipped, resent, resend_gone, nacks_sent, rx_ok, rx_bad, rx_other, clocks_sent, noaudio_sent;
/* the AUDIO packet the radio's queue had no room for (Bluetooth or a busy
 * channel took the air): the next pass sends it again to the followers it
 * hasn't reached, and nothing newer goes before it, until its play time.
 * Its bytes stay in the resend history, which nothing writes meanwhile. */
static bool held;
static int64_t held_k;
static uint32_t held_ips[HIFI_PARTY_FOLLOWERS];
static int held_n;
/* held packets that went on a later pass, and that reached their play time
 * first; resends asked for too late to be played, and dropped while the
 * queue was full */
static uint32_t held_ok, held_late, resend_late, resend_busy;
/* resends the followers asked for, not yet sent: they go before the held
 * packet and the stream, which play later. tried: the queue was full for it
 * once already, so a failure isn't counted again */
static struct {
	int64_t k;
	uint32_t ip;
	bool tried;
} rq[RESEND_MAX];
static int rq_n;
static uint32_t heap_start, heap_min;
/* the leader's failed sends by errno (ENOMEM, EAGAIN, any other) and in
 * bursts, and a follower's concealed frames in bursts: with both boards'
 * clocks in the stats, a run can tell sending trouble from the radio's */
static uint32_t txe_nomem, txe_again, txe_other;
static hifi_party_bursts_t tx_bursts, gap_bursts; /* one writer each: the radio task, the audio task */

static uint32_t heap_now(void) { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }
static uint32_t ms_now(void) { return (uint32_t) (esp_timer_get_time() / 1000); }

static void lock(void) { xSemaphoreTake(mx, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(mx); }
static void glock(void) { xSemaphoreTake(gmx, portMAX_DELAY); }
static void gunlock(void) { xSemaphoreGive(gmx); }

static int source(void) { return hifi_sink_source ? hifi_sink_source() : 0; }

static double tsf_at(int64_t w) { return hifi_party_sync_tsf_at(&st->sync, w); }

static void mark_add(int64_t k, double tsf, bool have_ref) {
	if (last_mark_k && k > last_mark_k && k - last_mark_k > RATE) marks_missed += (uint32_t) ((k - last_mark_k) / RATE - 1);
	last_mark_k = k;
	st->marks[marks_head] = (mark_t){k, tsf};
	marks_head = (marks_head + 1) % MARKS;
	if (marks_n < MARKS) marks_n++;
	const hifi_party_sync_t *c = &st->sync;
	if (have_ref && offs_n < OFFS) st->offs[offs_n++] = (float) (tsf - (c->clk_t + (double) (k - c->clk_k) * 1e9 / c->clk_rate));
}

static void counters_reset(void) {
	wrong_rate = blocks = muted = output_lost = unaligned = retaken = 0;
	marks_head = marks_n = offs_n = 0;
	last_mark_k = 0;
	marks_missed = 0;
	sent = send_fail = send_skipped = resent = resend_gone = nacks_sent = rx_ok = rx_bad = rx_other = 0;
	held_ok = held_late = resend_late = resend_busy = 0;
	clocks_sent = noaudio_sent = 0;
	txe_nomem = txe_again = txe_other = 0;
	memset(&tx_bursts, 0, sizeof(tx_bursts));
	memset(&gap_bursts, 0, sizeof(gap_bursts));
}

static void bursts_json(hj_t *j, const char *key, const hifi_party_bursts_t *r) {
	hifi_party_bursts_t c = *r; /* a copy: its task may add to it meanwhile */
	if (!c.total) return;
	hj_obj(j, key);
	hj_int(j, "n", c.total);
	hj_arr(j, "b");
	for (int i = 0; i < HIFI_PARTY_BURSTS; i++) {
		const hifi_party_burst_t *e = hifi_party_bursts_get(&c, i);
		if (!e) break;
		hj_arr(j, NULL);
		hj_int(j, NULL, e->at_ms);
		hj_int(j, NULL, e->dur_ms);
		hj_int(j, NULL, e->count);
		hj_int(j, NULL, e->code);
		hj_arr_end(j);
	}
	hj_arr_end(j);
	hj_obj_end(j);
}

/* ------------------------------------------------------------------ hooks */

void hifi_party_i2s_restart(int frames, u32_t rate) {
	boot_buf_frames = frames;
	if (!hooks_on || !mx) return;
	lock();
	buf_frames = frames;
	hifi_party_sync_restart(&st->sync, frames);
	unlock();
}

/* a quiet low tone (110 Hz, about -30 dBFS) with a 1 ms click each second */
static uint32_t tone(void) {
	if (s_sec > 0 && s_sec < 44) return (uint32_t) (uint16_t) 6000 << 16 | 6000;
	uint16_t v = (uint16_t) tone_tab[s_tone];
	return (uint32_t) v << 16 | v;
}

static uint32_t ring_at(int64_t i) { return i >= 0 ? ring[RI(i)] : 0; }

/* a and b mixed, b's weight w of XFADE, per 16-bit channel */
static uint32_t mix(uint32_t a, uint32_t b, int w) {
	int32_t al = (int16_t) (a & 0xffff), ar = (int16_t) (a >> 16), bl = (int16_t) (b & 0xffff), br = (int16_t) (b >> 16);
	int32_t l = (al * (XFADE - w) + bl * w) / XFADE, r = (ar * (XFADE - w) + br * w) / XFADE;
	return (uint32_t) (uint16_t) (int16_t) r << 16 | (uint16_t) (int16_t) l;
}

/* The leader's path, and a board with no group (listening, or leading
 * alone): what the output plays goes into the ring, and comes out DELAY
 * later while followers need the head start, at once otherwise. */
static void leader_block(uint32_t *b, int n, int64_t now) {
	bool leading = hrole == HIFI_PARTY_ROLE_LEADER;
	bool test = tone_on && leading;
	if (want_delay != cur_delay && !xfade) {
		old_delay = cur_delay;
		cur_delay = want_delay;
		xfade = XFADE;
	}
	int64_t s0 = s_pos;
	for (int i = 0; i < n; i++, s_pos++) {
		uint32_t in = test ? tone() : b[i];
		if (test && s_sec == 0) in = MARKER;
		ring[RI(s_pos)] = in;
		uint32_t out = ring_at(s_pos - cur_delay);
		if (xfade) {
			out = mix(ring_at(s_pos - old_delay), out, XFADE - xfade);
			xfade--;
		}
		b[i] = out;
		if (++s_sec == RATE) s_sec = 0;
		if (++s_tone == TONE_N) s_tone = 0;
	}
	kw_off = s0 - cur_delay - w_pos;
	if (leading && hifi_party_sync_ok(&st->sync) && !xfade) {
		if (test)
			for (int i = 0; i < n; i++) {
				int64_t k = w_pos + i + kw_off;
				if (b[i] == MARKER && (i ? b[i - 1] : prev_out) != MARKER && k >= 0 && k % RATE == 0)
					mark_add(k, tsf_at(w_pos + i), false);
			}
		if (now - last_clock_us >= CLOCK_EVERY_US &&
		    hifi_party_sync_leader_clock(&st->sync, w_pos, &clk_t, &clk_rate, clk_bssid)) {
			clk_k = w_pos + kw_off;
			clk_ok = true;
			clk_seq++;
			last_clock_us = now;
		}
	} else
		clk_ok = false;
}

static void follower_block(uint32_t *b, int n, int64_t now) {
	double target;
	int32_t gain = (int32_t) party_gain;
	if (hifi_party_sync_target(&st->sync, w_pos, now, &target)) {
		uint32_t miss0 = st->jb.missing_frames;
		hifi_party_render(&st->ctl, &st->jb, target, b, n, 65536, 65536);
		if (st->jb.missing_frames != miss0) hifi_party_bursts_add(&gap_bursts, (uint32_t) (now / 1000), st->jb.missing_frames - miss0, 0);
		if (now - marks_seen_us < MARKS_FOR_US)
			for (int i = 0; i < n; i++)
				if (b[i] == MARKER && (i ? b[i - 1] : prev_out) != MARKER) {
					int64_t k = llround((target + i) / RATE) * RATE;
					mark_add(k, tsf_at(w_pos + i), true);
				}
		prev_out = b[n - 1];
		if (gain != 65536) hifi_party_gain(b, n, gain, gain);
		return;
	}
	/* no clock to play to: fade out over the block rather than cut */
	hifi_party_mute(&st->ctl, &st->jb, b, n, gain, gain);
	muted++;
	prev_out = b[n - 1];
}

void hifi_party_i2s_block(u8_t *buf, frames_t frames, u32_t rate) {
	if (!hooks_on || !mx || !frames) return;
	if ((uintptr_t) buf & 3) {
		unaligned++;
		return;
	}
	bool follow = hrole == HIFI_PARTY_ROLE_FOLLOWER;
	if (follow && source() != SRC_PARTY) {
		output_lost++; /* something else has the output: leave it alone */
		return;
	}
	lock();
	if (rate != RATE) {
		wrong_rate++;
		clk_ok = false;
		unlock();
		return;
	}
	blocks++;
	int64_t now = esp_timer_get_time();
	uint32_t *b = (uint32_t *) buf;
	if (follow) follower_block(b, (int) frames, now);
	else {
		leader_block(b, (int) frames, now);
		prev_out = b[frames - 1];
	}
	last_block = frames;
	unlock();
}

void hifi_party_i2s_wrote(frames_t frames, int64_t t0, int64_t t1) {
	if (!hooks_on || !mx) return;
	lock();
	int64_t w0 = w_pos;
	w_pos += frames;
	hifi_party_sync_wrote(&st->sync, w0, w_pos, t0, t1, (int) last_block);
	last_block = 0;
	unlock();
}

/* ------------------------------------------------------------------ worker: the radio */

/* 0 when the packet went, else the errno (-1 for a short send or no socket).
 * count: false for a packet that already failed once (a held one, a queued
 * resend), so each packet the queue turned away counts once */
static int send_one(uint32_t ip, const uint8_t *p, size_t n, bool count) {
	if (sock < 0) return -1;
	struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(HIFI_PARTY_PORT), .sin_addr.s_addr = htonl(ip)};
	int r = sendto(sock, p, n, 0, (struct sockaddr *) &to, sizeof(to));
	if (r == (int) n) {
		sent++;
		return 0;
	}
	int e = r < 0 ? errno : 0;
	if (!count) return e ? e : -1;
	send_fail++; /* mostly the radio's queue is full: a follower asks again */
	if (e == ENOMEM) txe_nomem++;
	else if (e == EAGAIN || e == EWOULDBLOCK) txe_again++;
	else txe_other++;
	hifi_party_bursts_add(&tx_bursts, ms_now(), 1, e);
	return e ? e : -1;
}

static int send_to(uint32_t ip, const uint8_t *p, size_t n) { return send_one(ip, p, n, true); }

/* the radio's transmit buffers are all taken: worth trying again shortly */
static bool queue_full(int e) { return e == ENOMEM || e == EAGAIN || e == EWOULDBLOCK; }

static int64_t tsf_now(void) { return esp_wifi_get_tsf_time(WIFI_IF_STA); }

static void tsf_sample(void) {
	int64_t a = esp_timer_get_time();
	int64_t tsf = tsf_now();
	int64_t b = esp_timer_get_time();
	lock();
	hifi_party_sync_tsf(&st->sync, a, b, tsf, have_bssid ? my_bssid : NULL);
	unlock();
}

static void sock_close(void) {
	if (sock >= 0) close(sock);
	sock = -1;
}

/* the station's address now; a new one rebinds the socket */
static void net_refresh(void) {
	esp_netif_t *ni = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
	esp_netif_ip_info_t ip;
	uint32_t a = 0, m = 0;
	if (ni && esp_netif_get_ip_info(ni, &ip) == ESP_OK) {
		a = ntohl(ip.ip.addr);
		m = ntohl(ip.netmask.addr);
	}
	wifi_ap_record_t ap;
	have_bssid = a && esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
	if (have_bssid) memcpy(my_bssid, ap.bssid, 6);
	if (a == self_ip && m == self_mask && (sock >= 0 || !a)) return;
	sock_close();
	self_ip = a;
	self_mask = m;
	glock();
	int r = hifi_party_group_set_ip(&st->g, ms_now(), a, m, esp_random());
	pend_do |= r;
	gunlock();
	if (!a) return;
	int so = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	struct sockaddr_in me = {.sin_family = AF_INET, .sin_port = htons(HIFI_PARTY_PORT), .sin_addr.s_addr = htonl(a)};
	int yes = 1, rb = RCVBUF;
	if (so < 0 || bind(so, (struct sockaddr *) &me, sizeof(me)) ||
	    setsockopt(so, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes))) {
		if (so >= 0) close(so);
		ESP_LOGW(TAG, "no UDP socket on port %d", HIFI_PARTY_PORT);
		return;
	}
	setsockopt(so, SOL_SOCKET, SO_RCVBUF, &rb, sizeof(rb)); /* a burst of resends, a second's announces */
	sock = so;
}

/* a key made from the secret, under gmx */
static void make_group_key(hifi_link_key_t *k, uint32_t group) {
	uint8_t raw[HIFI_LINK_KEY];
	hifi_party_group_key(st->secret, group, raw);
	hifi_link_key_init(k, raw);
	hifi_link_wipe(raw, sizeof(raw));
}

static void follower_start(void) {
	lock();
	hifi_party_jb_init(&st->jb, jb_mem, JB_SLOTS);
	hifi_party_ctl_init(&st->ctl, RATE);
	hifi_party_sync_forget_clock(&st->sync);
	unlock();
	if (source() != SRC_PARTY && hifi_party_sink_start) {
		hifi_party_sink_start(RATE);
		grace_until = esp_timer_get_time() + GRACE_US;
	}
}

/* Does what the group asked (the DO bits). Called with no lock held. */
static void act(int r) {
	if (!r) return;
	hifi_party_pkt_t p;
	uint8_t pl[HIFI_PARTY_ANNOUNCE_LEN];
	if (r & HIFI_PARTY_DO_LEAVE) {
		glock();
		uint32_t to = st->g.leave_ip;
		hifi_party_join_build(&st->g, tsf_now(), true, ++tx_seq, &p, pl);
		make_group_key(&st->lkey, st->g.leave_group);
		size_t n = hifi_party_encode(&st->lkey, &p, pl, st->pkt, sizeof(st->pkt));
		hifi_link_key_wipe(&st->lkey);
		gunlock();
		if (n && to) send_to(to, st->pkt, n);
	}
	if (r & HIFI_PARTY_DO_ROLE) {
		glock();
		hifi_party_role_t was, now = st->g.role;
		uint32_t group = st->g.group;
		if (now == HIFI_PARTY_ROLE_LEADER || now == HIFI_PARTY_ROLE_FOLLOWER) make_group_key(&st->gkey, group);
		else hifi_link_key_wipe(&st->gkey);
		gunlock();
		lock();
		was = hrole;
		hrole = now;
		hgroup = group;
		if (now == HIFI_PARTY_ROLE_LEADER && was != HIFI_PARTY_ROLE_LEADER) hifi_party_hist_init(&st->hist, hist_mem, HIST_SLOTS);
		clk_ok = false;
		unlock();
		sending = held = false;
		rq_n = 0;
		if (now == HIFI_PARTY_ROLE_FOLLOWER) follower_start();
		else if ((was == HIFI_PARTY_ROLE_FOLLOWER || now != HIFI_PARTY_ROLE_LEADER) && source() == SRC_PARTY && hifi_sink_stop) {
			hifi_sink_stop();
			tone_held = false;
		}
		ESP_LOGI(TAG, "%s, group %08x", hifi_party_role_name(now), (unsigned) group);
	}
	if (r & HIFI_PARTY_DO_LOST) {
		lock();
		hifi_party_sync_forget_clock(&st->sync);
		unlock();
	}
	if (r & HIFI_PARTY_DO_ANNOUNCE && sock >= 0) {
		glock();
		hifi_party_announce_build(&st->g, tsf_now(), name, &p, pl);
		size_t n = hifi_party_encode(&st->akey, &p, pl, st->pkt, sizeof(st->pkt));
		gunlock();
		/* the subnet's broadcast: lwIP never routes it, and the router opens nothing */
		if (n) send_to(self_ip | ~self_mask, st->pkt, n);
	}
	if (r & HIFI_PARTY_DO_JOIN && sock >= 0) {
		glock();
		uint32_t to = st->g.leader_ip;
		hifi_party_join_build(&st->g, tsf_now(), false, ++tx_seq, &p, pl);
		size_t n = hifi_party_encode(&st->gkey, &p, pl, st->pkt, sizeof(st->pkt));
		gunlock();
		if (n && to) send_to(to, st->pkt, n);
	}
}

static int peers_now(uint32_t *ips) {
	glock();
	int n = st->g.role == HIFI_PARTY_ROLE_LEADER ? st->g.npeers : 0;
	memcpy(ips, st->g.peers, sizeof(uint32_t) * (size_t) n);
	gunlock();
	return n;
}

/* packet k is due while it can still reach a follower LATE_MARGIN before
 * its first frame plays: the follower plays frame k as the leader writes
 * frame k + DELAY (pos) */
static bool too_late(int64_t k, int64_t pos) { return k + DELAY - LATE_MARGIN < pos; }

/* the first packet still due at pos */
static int64_t first_due(int64_t pos) {
	int64_t s = pos - DELAY + LATE_MARGIN;
	return s > 0 ? (s + HIFI_PARTY_FRAMES - 1) / HIFI_PARTY_FRAMES * HIFI_PARTY_FRAMES : 0;
}

/* a NACKed k waits for its turn; the list full, it is dropped */
static void resend_add(int64_t k, uint32_t ip) {
	for (int i = 0; i < rq_n; i++)
		if (rq[i].k == k && rq[i].ip == ip) return;
	if (rq_n == RESEND_MAX) {
		resend_busy++;
		return;
	}
	rq[rq_n].k = k;
	rq[rq_n].ip = ip;
	rq[rq_n].tried = false;
	rq_n++;
}

/* the queued resends, earliest play time first: false while the queue is full */
static bool resend_drain(int64_t pos) {
	while (rq_n) {
		int b = 0;
		for (int i = 1; i < rq_n; i++)
			if (rq[i].k < rq[b].k) b = i;
		int64_t k = rq[b].k;
		uint32_t ip = rq[b].ip;
		bool tried = rq[b].tried;
		rq[b] = rq[--rq_n];
		if (too_late(k, pos)) {
			resend_late++;
			continue;
		}
		size_t n;
		const uint8_t *q = hifi_party_hist_get(&st->hist, k, &n);
		if (!q) {
			resend_gone++;
			continue;
		}
		int e = send_one(ip, q, n, !tried);
		if (!e) {
			resent++;
			continue;
		}
		if (queue_full(e)) {
			rq[rq_n].k = k;
			rq[rq_n].ip = ip;
			rq[rq_n].tried = true;
			rq_n++;
			return false;
		}
	}
	return true;
}

/* AUDIO packet k to each of ips; with the queue full at one, it and the
 * rest are held for the next pass (false). Another error moves on: the
 * follower asks again. */
static bool send_all(int64_t k, const uint8_t *p, size_t n, const uint32_t *ips, int np, bool retry) {
	for (int i = 0; i < np; i++) {
		if (!queue_full(send_one(ips[i], p, n, !retry || i > 0))) continue;
		held = true;
		held_k = k;
		held_n = np - i;
		memmove(held_ips, ips + i, sizeof(uint32_t) * (size_t) held_n);
		return false;
	}
	return true;
}

/* the held packet again: false while the queue is still full */
static bool held_send(int64_t pos) {
	if (!held) return true;
	held = false;
	size_t n;
	const uint8_t *q = hifi_party_hist_get(&st->hist, held_k, &n);
	if (!q || too_late(held_k, pos)) {
		held_late++;
		return true;
	}
	if (!send_all(held_k, q, n, held_ips, held_n, true)) return false;
	held_ok++;
	return true;
}

static void leader_send(int64_t now) {
	uint32_t *ips = st->peers;
	int np = peers_now(ips);
	lock();
	if (np) alone_since = 0;
	else if (!alone_since) alone_since = now;
	want_delay = np || (alone_since && now - alone_since < ALONE_US) ? DELAY : 0;
	if (hrole != HIFI_PARTY_ROLE_LEADER) want_delay = 0;
	bool ready = np && cur_delay == DELAY && !xfade;
	if (ready && !sending) k_sent = first_due(s_pos);
	unlock();
	sending = ready;
	if (!sending) {
		held = false;
		rq_n = 0;
		return;
	}
	uint32_t group = hgroup;
	bool marks = tone_on;
	for (int burst = 0; burst < SEND_BURST; burst++) {
		lock();
		int64_t pos = s_pos;
		unlock();
		/* earliest play time first: resends, the held packet, the stream.
		 * The queue full, the next pass tries again */
		if (!resend_drain(pos) || !held_send(pos)) return;
		lock();
		/* past its play time (the queue held the stream back, or the worker
		 * fell behind): what the followers can't play any more isn't sent */
		if (too_late(k_sent, s_pos)) {
			int64_t to = first_due(s_pos);
			send_skipped += (uint32_t) ((to - k_sent) / HIFI_PARTY_FRAMES);
			k_sent = to;
		}
		int64_t avail = s_pos - k_sent;
		if (avail < HIFI_PARTY_FRAMES) {
			unlock();
			break;
		}
		uint32_t any = 0;
		for (int i = 0; i < HIFI_PARTY_FRAMES; i++) any |= st->frames[i] = ring[RI(k_sent + i)];
		unlock();
		hifi_party_pkt_t p = {.type = HIFI_PARTY_AUDIO, .group = group, .seq = ++tx_seq, .k = k_sent, .rate = RATE,
				      .n = HIFI_PARTY_FRAMES, .flags = (any ? 0 : HIFI_PARTY_F_SILENCE) | (marks ? HIFI_PARTY_F_MARKS : 0)};
		size_t n = hifi_party_encode(&st->gkey, &p, st->frames, st->pkt, sizeof(st->pkt));
		int64_t k = k_sent;
		k_sent += HIFI_PARTY_FRAMES;
		if (n) {
			hifi_party_hist_put(&st->hist, k, st->pkt, n);
			if (!send_all(k, st->pkt, n, ips, np, false)) return;
		}
	}
}

/* the CLOCK to each follower, or, with none fresh for NOAUDIO_US (the output
 * stopped, or plays a rate party doesn't carry), a CLOCK saying so */
static void leader_clock(int64_t now) {
	uint32_t *ips = st->peers;
	int np = peers_now(ips);
	if (!np) return;
	lock();
	bool fresh = clk_ok && clk_seq != clk_sent_seq;
	hifi_party_pkt_t p = {.type = HIFI_PARTY_CLOCK, .group = hgroup, .k = clk_k, .t = clk_t, .rate = clk_rate};
	uint8_t bss[6];
	memcpy(bss, clk_bssid, 6);
	if (fresh) clk_sent_seq = clk_seq;
	unlock();
	if (fresh) clk_sent_us = now;
	else if (now - clk_sent_us >= NOAUDIO_US && now - noaudio_us >= CLOCK_EVERY_US) {
		p = (hifi_party_pkt_t){.type = HIFI_PARTY_CLOCK, .group = hgroup, .flags = HIFI_PARTY_F_NOAUDIO};
		memcpy(bss, have_bssid ? my_bssid : (const uint8_t[6]){0}, 6);
		noaudio_us = now;
		noaudio_sent++;
	} else
		return;
	for (int i = 0; i < np; i++) {
		p.seq = ++tx_seq;
		size_t n = hifi_party_encode(&st->gkey, &p, bss, st->pkt, sizeof(st->pkt));
		if (n) send_to(ips[i], st->pkt, n);
	}
	if (fresh) clocks_sent++;
}

static void follower_nack(int64_t now) {
	lock();
	int m = hifi_party_jb_nacks(&st->jb, now, st->nk, HIFI_PARTY_NACK_MAX);
	uint32_t group = hgroup;
	unlock();
	if (!m) return;
	glock();
	uint32_t to = st->g.leader_ip;
	gunlock();
	for (int i = 0; i < m; i++)
		for (int j = 0; j < 8; j++) st->nack_pl[i * 8 + j] = (uint8_t) ((uint64_t) st->nk[i] >> (8 * j));
	hifi_party_pkt_t p = {.type = HIFI_PARTY_NACK, .group = group, .seq = ++tx_seq, .n = (uint16_t) m};
	size_t n = hifi_party_encode(&st->gkey, &p, st->nack_pl, st->pkt, sizeof(st->pkt));
	if (n && to) {
		send_to(to, st->pkt, n);
		nacks_sent++;
	}
}

static void got(uint32_t src, const uint8_t *d, size_t len, int64_t now) {
	uint32_t ms = (uint32_t) (now / 1000);
	if (len < 4) {
		rx_other++;
		return;
	}
	uint8_t type = d[3];
	glock();
	bool admit = hifi_party_group_admit(&st->g, ms, src, type);
	gunlock();
	if (!admit) {
		rx_other++;
		return;
	}
	hifi_party_pkt_t p;
	const uint8_t *pl;
	size_t pn;
	/* the keys change only on this task, so decoding needs no lock */
	if (hifi_party_decode(type == HIFI_PARTY_ANNOUNCE ? &st->akey : &st->gkey, d, len, &p, &pl, &pn) != HIFI_PARTY_OK) {
		rx_bad++;
		return;
	}
	rx_ok++;
	int r = 0;
	switch (p.type) {
	case HIFI_PARTY_ANNOUNCE: {
		hifi_party_announce_t a;
		int64_t tsf = tsf_now();
		glock();
		int ok = hifi_party_announce_parse(&p, pl, pn, src, self_ip, self_mask, tsf, &a);
		if (ok > 0) r = hifi_party_group_announce(&st->g, ms, &a);
		else if (ok < 0) hifi_party_group_other_ap(&st->g, ms, src, p.t);
		gunlock();
		break;
	}
	case HIFI_PARTY_JOIN: {
		int64_t tsf_them;
		bool leave;
		int64_t tsf = tsf_now();
		glock();
		if (st->g.role == HIFI_PARTY_ROLE_LEADER && p.group == st->g.group &&
		    hifi_party_join_parse(&p, pl, pn, src, self_ip, self_mask, tsf, &tsf_them, &leave))
			hifi_party_group_join(&st->g, ms, src, tsf_them, leave);
		gunlock();
		break;
	}
	case HIFI_PARTY_CLOCK: {
		glock();
		bool ok = st->g.role == HIFI_PARTY_ROLE_FOLLOWER && p.group == st->g.group && hifi_party_group_heard(&st->g, ms, src, p.seq);
		gunlock();
		if (!ok) break;
		lock();
		if (p.flags & HIFI_PARTY_F_NOAUDIO) hifi_party_sync_forget_clock(&st->sync); /* the leader plays nothing: mute */
		else hifi_party_sync_clock(&st->sync, p.k, p.t, p.rate, p.seq, pl, now);
		unlock();
		break;
	}
	case HIFI_PARTY_AUDIO: {
		glock();
		bool ok = st->g.role == HIFI_PARTY_ROLE_FOLLOWER && p.group == st->g.group && src == st->g.leader_ip;
		gunlock();
		if (!ok) break;
		if (p.flags & HIFI_PARTY_F_MARKS) marks_seen_us = now;
		lock();
		hifi_party_jb_put(&st->jb, p.k, (p.flags & HIFI_PARTY_F_SILENCE) ? NULL : pl, p.n, now);
		unlock();
		break;
	}
	case HIFI_PARTY_NACK: {
		glock();
		bool ok = st->g.role == HIFI_PARTY_ROLE_LEADER && p.group == st->g.group && hifi_party_group_nack_ok(&st->g, src, p.seq);
		gunlock();
		if (!ok) break;
		/* a resend goes only while it can still be played (a late one
		 * would only stand in the queue before packets that are still
		 * due), earliest play time first, and waits in rq while the
		 * queue is full. k comes off the air: outside what was sent, it
		 * is gone */
		lock();
		int64_t pos = s_pos;
		unlock();
		for (int i = 0; i < p.n; i++) {
			int64_t k = hifi_party_nack_k(pl, i);
			if (k < 0 || k > pos) resend_gone++;
			else if (too_late(k, pos)) resend_late++;
			else resend_add(k, src);
		}
		if (!held) resend_drain(pos);
		break;
	}
	}
	act(r);
}

/* something of this board's own plays (hifi_party_local_source: never the
 * test signal, and line in only when the board isn't following) */
static bool local_playing(int64_t now, hifi_party_role_t role) {
	if (now < grace_until) return false;
	return hifi_party_local_source(role, source());
}

static void load_name(void) {
	char *s = config_alloc_get_str("hifi_name", NULL, NULL);
	if (!s || !*s) {
		free(s);
		s = config_alloc_get_str("airplay_name", NULL, HIFI_MODEL);
	}
	strlcpy(name, s ? s : HIFI_MODEL, sizeof(name));
	free(s);
}

static void party_up(void) {
	load_name();
	self_ip = self_mask = 0;
	sock_close();
	have_bssid = false;
	tx_seq = esp_random() >> 8; /* a reboot doesn't start the seqs from 0 again */
	sending = held = false;
	rq_n = 0;
	clk_sent_us = noaudio_us = grace_until = retake_at = 0;
	glock();
	uint8_t raw[HIFI_LINK_KEY];
	hifi_party_announce_key(st->secret, raw);
	hifi_link_key_init(&st->akey, raw);
	hifi_link_wipe(raw, sizeof(raw));
	hifi_party_group_init(&st->g, 0, 0, saved_term, saved_pick);
	int r = hifi_party_group_on(&st->g, ms_now(), esp_random());
	gunlock();
	lock();
	int bf = boot_buf_frames;
	buf_frames = bf > 0 ? bf : 0;
	hifi_party_sync_init(&st->sync, buf_frames);
	hifi_party_jb_init(&st->jb, jb_mem, JB_SLOTS);
	hifi_party_ctl_init(&st->ctl, RATE);
	hifi_party_hist_init(&st->hist, hist_mem, HIST_SLOTS);
	w_pos = s_pos = kw_off = 0;
	s_sec = s_tone = 0;
	prev_out = 0;
	last_block = 0;
	cur_delay = old_delay = want_delay = xfade = 0;
	alone_since = 0;
	last_clock_us = 0;
	clk_ok = false;
	clk_k = clk_t = 0;
	clk_rate = clk_seq = clk_sent_seq = 0;
	hrole = HIFI_PARTY_ROLE_LISTENING;
	hgroup = 0;
	counters_reset();
	heap_start = heap_min = heap_now();
	hooks_on = true;
	unlock();
	active = true;
	act(r);
	ESP_LOGI(TAG, "on");
}

static void party_down(void) {
	glock();
	int r = hifi_party_group_off(&st->g);
	gunlock();
	act(r & HIFI_PARTY_DO_LEAVE); /* the leave first, while the socket is open */
	lock();
	bool was_follower = hrole == HIFI_PARTY_ROLE_FOLLOWER;
	hooks_on = false;
	hrole = HIFI_PARTY_ROLE_OFF;
	hgroup = 0;
	clk_ok = false;
	unlock();
	sock_close();
	if ((was_follower || tone_held) && source() == SRC_PARTY && hifi_sink_stop) hifi_sink_stop();
	tone_held = false;
	glock();
	hifi_link_key_wipe(&st->gkey);
	hifi_link_key_wipe(&st->akey);
	hifi_link_key_wipe(&st->lkey);
	gunlock();
	sending = held = false;
	rq_n = 0;
	active = false;
	ESP_LOGI(TAG, "off: sent %u, received %u", (unsigned) sent, (unsigned) rx_ok);
}

static void task_main(void *arg) {
	int64_t next_tsf = 0, next_sec = 0, next_tick = 0;
	for (;;) {
		ticked_ms = ms_now();
		bool on = want_on && have_secret;
		if (active && (!on || restart)) party_down();
		restart = false;
		if (!on) {
			ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
			continue;
		}
		if (!active) {
			party_up();
			next_tsf = next_sec = next_tick = 0;
		}
		int64_t now = esp_timer_get_time();
		if (now >= next_sec) {
			net_refresh();
			next_sec = now + 1000000;
		}
		if (now >= next_tsf) {
			tsf_sample();
			next_tsf = now + TSF_EVERY_US;
		}
		uint32_t h = heap_now();
		if (h < heap_min) heap_min = h;
		int r = __atomic_exchange_n(&pend_do, 0, __ATOMIC_SEQ_CST); /* a Lead from the API, a new address */
		if (now >= next_tick) {
			glock();
			r |= hifi_party_group_tick(&st->g, (uint32_t) (now / 1000), esp_random(), local_playing(now, st->g.role), h < HEAP_FLOOR);
			gunlock();
			next_tick = now + TICK_US;
		}
		act(r);
		/* a follower whose output something else took and then let go (a test
		 * tone ends with the output idle) takes it back, at most once a second.
		 * It also takes it from line in, which can start first when a tone
		 * ends: a following board stays a follower (hifi_party_retakes) */
		if (now >= grace_until && now >= retake_at && hifi_party_retakes(hrole, source())) {
			retake_at = now + 1000000;
			retaken++;
			follower_start();
		}
		if (sock < 0) {
			vTaskDelay(pdMS_TO_TICKS(20));
			continue;
		}
		hifi_party_role_t role = hrole;
		if (role == HIFI_PARTY_ROLE_FOLLOWER) follower_nack(now);
		else {
			leader_send(now);
			if (role == HIFI_PARTY_ROLE_LEADER) leader_clock(now);
		}
		fd_set rd;
		FD_ZERO(&rd);
		FD_SET(sock, &rd);
		struct timeval tv = {.tv_sec = 0, .tv_usec = 10000};
		if (select(sock + 1, &rd, NULL, NULL, &tv) > 0)
			for (int i = 0; i < 32; i++) {
				struct sockaddr_in from;
				socklen_t fl = sizeof(from);
				int n = recvfrom(sock, st->rx, sizeof(st->rx), MSG_DONTWAIT, (struct sockaddr *) &from, &fl);
				if (n <= 0) break;
				uint32_t src = ntohl(from.sin_addr.s_addr);
				if (from.sin_port != htons(HIFI_PARTY_PORT) || src == self_ip || !hifi_party_same_subnet(src, self_ip, self_mask)) {
					rx_other++;
					continue;
				}
				got(src, st->rx, (size_t) n, esp_timer_get_time());
			}
		vTaskDelay(1); /* select may not have blocked: give IDLE1 its tick anyway */
	}
}

/* ------------------------------------------------------------------ setup */

static bool alloc_once(void) {
	if (!st) st = heap_caps_calloc(1, sizeof(*st), MALLOC_CAP_SPIRAM);
	if (!ring) ring = heap_caps_calloc(RING, 4, MALLOC_CAP_SPIRAM);
	if (!jb_mem) jb_mem = heap_caps_calloc(1, hifi_party_jb_bytes(JB_SLOTS), MALLOC_CAP_SPIRAM);
	if (!hist_mem) hist_mem = heap_caps_calloc(1, hifi_party_hist_bytes(HIST_SLOTS), MALLOC_CAP_SPIRAM);
	if (!mx) mx = xSemaphoreCreateMutex();
	if (!gmx) gmx = xSemaphoreCreateMutex();
	if (!tone_tab[1])
		for (int i = 0; i < TONE_N; i++) tone_tab[i] = (int16_t) lrintf(1000.0f * sinf(2.0f * (float) M_PI * (float) i / TONE_N));
	if (!st || !ring || !jb_mem || !hist_mem || !mx || !gmx) return false;
	if (!worker) {
		StackType_t *stack = heap_caps_malloc(STACK, MALLOC_CAP_SPIRAM);
		if (!stack) return false;
		worker = xTaskCreateStaticPinnedToCore(task_main, "party", STACK, NULL, ESP_TASK_PRIO_MIN + 5, stack, &worker_tcb, 1);
		if (!worker) {
			free(stack);
			return false;
		}
	}
	return true;
}

static bool nvs_flag(const char *key) {
	char *s = config_alloc_get_str(key, NULL, "0");
	bool v = s && !strcmp(s, "1");
	free(s);
	return v;
}

/* the secret NVS keeps, into st->secret; false when there is none */
static bool load_secret(void) {
	char *s = config_alloc_get_str("hifi_party_key", NULL, NULL);
	bool ok = s && hifi_party_parse_secret(s, st->secret);
	if (s) {
		hifi_link_wipe(s, strlen(s));
		free(s);
	}
	return ok;
}

bool hifi_party_saved_on(void) {
	if (!nvs_flag("hifi_party_on")) return false;
	char *s = config_alloc_get_str("hifi_party_key", NULL, NULL);
	uint8_t tmp[HIFI_PARTY_SECRET];
	bool ok = s && hifi_party_parse_secret(s, tmp);
	hifi_link_wipe(tmp, sizeof(tmp));
	if (s) {
		hifi_link_wipe(s, strlen(s));
		free(s);
	}
	return ok;
}

void hifi_party_init(void) {
	char *s = config_alloc_get_str("hifi_party_term", NULL, "0");
	saved_term = s ? strtoll(s, NULL, 10) : 0;
	free(s);
	if (saved_term < 0) saved_term = 0;
	saved_pick = nvs_flag("hifi_party_pick");
	if (!nvs_flag("hifi_party_on")) return; /* nothing is made until party is first switched on */
	if (!alloc_once()) {
		ESP_LOGE(TAG, "out of memory");
		return;
	}
	glock();
	have_secret = load_secret();
	gunlock();
	want_on = true;
	xTaskNotifyGive(worker);
}

void hifi_party_volume(uint32_t gain) { party_gain = gain; }

/* From the relay's task, every couple of seconds: keeps the term and the
 * pick across a reboot, and puts Bluetooth where party wants it. */
void hifi_party_poll(void) {
	if (!st || !gmx) return;
	glock();
	bool dirty = st->g.dirty;
	int64_t term = st->g.max_term;
	bool pick = st->g.picked;
	hifi_party_role_t role = st->g.role;
	st->g.dirty = false;
	gunlock();
	if (dirty) {
		char v[24];
		snprintf(v, sizeof(v), "%lld", (long long) term);
		config_set_value(NVS_TYPE_STR, "hifi_party_term", v);
		config_set_value(NVS_TYPE_STR, "hifi_party_pick", pick ? "1" : "0");
		saved_term = term;
		saved_pick = pick;
	}
	/* A phone can't join a follower (it would take the board out of the
	 * group), nor a board that may yet follow: Bluetooth stays on only for a
	 * picked leader, or while it plays (the phone came first). */
	if (!hifi_bt_sink_set || !hifi_bt_sink_is_off) return;
	bool on = active;
	bool want = !on || (role != HIFI_PARTY_ROLE_FOLLOWER && (pick || source() == SRC_BT));
	if (want == hifi_bt_sink_is_off()) hifi_bt_sink_set(want);
}

void hifi_party_selftest(hifi_selftest_t *t) {
	if (!want_on) {
		hifi_selftest_add(t, "party", true, "off");
		return;
	}
	if (!have_secret) {
		hifi_selftest_add(t, "party", false, "on, but no party key");
		return;
	}
	bool alive = worker && active && ms_now() - ticked_ms < 3000;
	hifi_party_role_t role = HIFI_PARTY_ROLE_OFF;
	int np = 0;
	if (gmx && xSemaphoreTake(gmx, pdMS_TO_TICKS(500))) {
		role = st->g.role;
		np = st->g.npeers;
		xSemaphoreGive(gmx);
	}
	hifi_selftest_add(t, "party", alive, "%s%s, %d followers", alive ? "" : "stuck, ", hifi_party_role_name(role), np);
}

/* ------------------------------------------------------------------ the API */

static void ip_str(char *s, size_t n, uint32_t ip) {
	snprintf(s, n, "%u.%u.%u.%u", (unsigned) (ip >> 24), (unsigned) (ip >> 16 & 255), (unsigned) (ip >> 8 & 255), (unsigned) (ip & 255));
}

static int err(char *out, size_t cap, int status, const char *msg) {
	hj_t j;
	hj_init(&j, out, cap);
	hj_obj(&j, NULL);
	hj_str(&j, "error", msg);
	hj_obj_end(&j);
	return status;
}

static void state_json(hj_t *j) {
	char ip[16];
	bool on = want_on && have_secret;
	hj_bool(j, "on", on);
	if (!st || !gmx) {
		hj_str(j, "role", "off");
		hj_bool(j, "key", false);
		return;
	}
	glock();
	const hifi_party_group_t *g = &st->g;
	hj_str(j, "role", active ? hifi_party_role_name(g->role) : "off");
	if (have_secret) hj_int(j, "key_id", hifi_party_secret_id(st->secret));
	hj_bool(j, "key", have_secret);
	hj_int(j, "term", active ? g->term : saved_term);
	hj_bool(j, "picked", active ? g->picked : saved_pick);
	if (active && g->role == HIFI_PARTY_ROLE_FOLLOWER) {
		hj_str(j, "leader", g->leader_name);
		ip_str(ip, sizeof(ip), g->leader_ip);
		hj_str(j, "leader_ip", ip);
	} else if (active && g->role == HIFI_PARTY_ROLE_LEADER)
		hj_str(j, "leader", name);
	hj_arr(j, "followers");
	if (active && g->role == HIFI_PARTY_ROLE_LEADER)
		for (int i = 0; i < g->npeers; i++) {
			ip_str(ip, sizeof(ip), g->peers[i]);
			hj_str(j, NULL, ip);
		}
	hj_arr_end(j);
	hj_str(j, "why", active ? (g->why ? g->why : "") : (want_on && !have_secret ? "no party key" : ""));
	if (active && g->full) hj_bool(j, "full", true);
	if (active && g->other_ap) hj_bool(j, "other_ap", true);
	if (active) {
		/* a leader joined with no CLOCK: its announces skipped this much longer */
		uint32_t left = hifi_party_group_skip_ms(g, ms_now());
		if (left) hj_int(j, "skip_s", (int) ((left + 999) / 1000));
	}
	gunlock();
	if (hifi_bt_sink_is_off) hj_bool(j, "bt", !hifi_bt_sink_is_off());
}

static void fit_json(hj_t *j, const char *nm, const hifi_party_fit_t *f) {
	hj_obj(j, nm);
	hj_int(j, "ok", f->ok);
	hj_num(j, "rms", f->rms, 1);
	hj_int(j, "n", f->n);
	hj_int(j, "rej", f->rejected);
	hj_obj_end(j);
}

static int cmp_float(const void *a, const void *b) {
	float x = *(const float *) a, y = *(const float *) b;
	return x < y ? -1 : x > y;
}

/* The lab's result, for the bench's runs (party-mode.md, section 5): one
 * console line of at most 1000 characters, so the keys are short and only
 * the last MARKS_SHOWN markers go out. */
#define MARKS_SHOWN 8

static void stats_json(hj_t *j) {
	static float *sorted;
	if (!sorted) sorted = heap_caps_malloc(sizeof(st->offs), MALLOC_CAP_SPIRAM);
	lock();
	bool lead = hrole != HIFI_PARTY_ROLE_FOLLOWER;
	hj_str(j, "role", hifi_party_role_name(hrole));
	hj_int(j, "buf", buf_frames);
	hj_int(j, "w", w_pos);
	hj_int(j, "d", cur_delay);
	const hifi_party_sync_t *sy = &st->sync;
	hj_int(j, "re", sy->dac_resets);
	if (sy->tsf_resets) hj_int(j, "tr", sy->tsf_resets);
	if (sy->short_writes) hj_int(j, "sw", sy->short_writes);
	if (wrong_rate) hj_int(j, "wr", wrong_rate);
	if (unaligned) hj_int(j, "ua", unaligned);
	if (sy->tsf_zero) hj_int(j, "tz", sy->tsf_zero);
	fit_json(j, "tsf", &sy->tsf);
	fit_json(j, "dac", &sy->dac);
	hj_arr(j, "clk");
	hj_int(j, NULL, lead ? clk_seq : sy->clk_seq);
	hj_int(j, NULL, lead ? clk_rate : sy->clk_rate);
	hj_int(j, NULL, lead ? clocks_sent : sy->clocks);
	hj_arr_end(j);
	hj_obj(j, "net");
	hj_int(j, "tx", sent);
	hj_int(j, "txf", send_fail);
	if (send_fail) {
		hj_arr(j, "txe");
		hj_int(j, NULL, txe_nomem);
		hj_int(j, NULL, txe_again);
		hj_int(j, NULL, txe_other);
		hj_arr_end(j);
	}
	hj_int(j, "rx", rx_ok);
	hj_int(j, "bad", rx_bad);
	hj_int(j, "oth", rx_other);
	if (lead) {
		hj_int(j, "rs", resent);
		hj_int(j, "gone", resend_gone);
		hj_int(j, "skip", send_skipped);
		hj_int(j, "rsl", resend_late);
		hj_int(j, "rsb", resend_busy);
		hj_arr(j, "hold");
		hj_int(j, NULL, held_ok);
		hj_int(j, NULL, held_late);
		hj_arr_end(j);
		if (noaudio_sent) hj_int(j, "na", noaudio_sent);
	} else {
		hj_int(j, "nack", nacks_sent);
		hj_int(j, "oldc", sy->old_clocks);
		hj_int(j, "bss", sy->bssid_wrong);
		if (sy->stale) hj_int(j, "stale", sy->stale);
	}
	hj_obj_end(j);
	if (!lead) {
		hj_obj(j, "jb");
		hj_int(j, "got", st->jb.got);
		hj_int(j, "dup", st->jb.dup);
		hj_int(j, "old", st->jb.old);
		hj_int(j, "skip", st->jb.skipped);
		hj_int(j, "gaps", st->jb.gaps);
		hj_int(j, "miss", st->jb.missing_frames);
		hj_obj_end(j);
		hj_obj(j, "ctl");
		hj_int(j, "dr", st->ctl.drops);
		hj_int(j, "rp", st->ctl.repeats);
		hj_int(j, "j", st->ctl.jumps);
		hj_num(j, "err", st->ctl.last_err, 1);
		hj_obj_end(j);
		hj_int(j, "mute", muted);
		if (output_lost) hj_int(j, "lost", output_lost);
		if (retaken) hj_int(j, "retake", retaken);
	}
	hj_arr(j, "heap"); /* internal RAM free: when party came on, the least since, now */
	hj_int(j, NULL, heap_start);
	hj_int(j, NULL, heap_min);
	hj_int(j, NULL, heap_now());
	hj_arr_end(j);
	if (marks_missed) hj_int(j, "mm", marks_missed);
	/* the bursts, newest first: "evt" failed sends, "evg" concealed frames,
	 * each [at ms, length ms, count, errno]; "ms" is now on the same clock,
	 * so a poller can place them in wall time */
	hj_int(j, "ms", ms_now());
	bursts_json(j, "evt", &tx_bursts);
	if (!lead) bursts_json(j, "evg", &gap_bursts);
	int on = offs_n;
	if (on && sorted) {
		double sum = 0, mx_abs = 0;
		for (int i = 0; i < on; i++) {
			sorted[i] = fabsf(st->offs[i]);
			sum += st->offs[i];
			if (sorted[i] > mx_abs) mx_abs = sorted[i];
		}
		float last = st->offs[on - 1];
		unlock();
		qsort(sorted, on, sizeof(float), cmp_float);
		hj_obj(j, "off");
		hj_int(j, "n", on);
		hj_num(j, "mean", sum / on, 1);
		hj_num(j, "last", last, 1);
		hj_num(j, "p50", sorted[on / 2], 1);
		hj_num(j, "p95", sorted[(on * 95) / 100], 1);
		hj_num(j, "max", mx_abs, 1);
		hj_obj_end(j);
		lock();
	}
	hj_arr(j, "m");
	int show = marks_n < MARKS_SHOWN ? marks_n : MARKS_SHOWN;
	for (int i = marks_n - show; i < marks_n; i++) {
		const mark_t *m = &st->marks[(marks_head - marks_n + i + MARKS) % MARKS];
		hj_arr(j, NULL);
		hj_int(j, NULL, m->k / RATE);
		hj_int(j, NULL, llround(m->tsf));
		hj_arr_end(j);
	}
	hj_arr_end(j);
	unlock();
}

static void wake(void) {
	if (worker) xTaskNotifyGive(worker);
}

/* party (GET: the state; POST: op on|off|lead, and from the Hive or USB
 * only, key and tone) and party/stats. Returns the HTTP status. */
int hifi_party_op(const char *sub, bool post, bool lan, const char *body, char *out, size_t cap) {
	hj_t j;
	if (sub && !strcmp(sub, "stats")) {
		if (post) return err(out, cap, 405, "party/stats is read with GET");
		if (!active) return err(out, cap, 409, "party is off");
		hj_init(&j, out, cap);
		hj_obj(&j, NULL);
		stats_json(&j);
		hj_obj_end(&j);
		return 200;
	}
	if (sub && *sub) return err(out, cap, 404, "no such party route");
	if (post) {
		char op[16];
		if (!body || !hj_get_str(body, "op", op, sizeof(op))) return err(out, cap, 400, "op must be on, off, lead, key or tone");
		bool key = !strcmp(op, "key"), tone = !strcmp(op, "tone");
		if ((key || tone) && lan) return err(out, cap, 403, "the party key and the test tone are set by the Hive or over USB, not from the LAN");
		if (!alloc_once()) return err(out, cap, 503, "out of memory");
		if (key) {
			char hex[2 * HIFI_PARTY_SECRET + 2];
			uint8_t sec[HIFI_PARTY_SECRET];
			bool ok = hj_get_str(body, "secret", hex, sizeof(hex)) && hifi_party_parse_secret(hex, sec);
			if (ok) {
				config_set_value(NVS_TYPE_STR, "hifi_party_key", hex);
				glock();
				memcpy(st->secret, sec, sizeof(sec));
				have_secret = true;
				gunlock();
				restart = active; /* the keys made from the old secret go */
				wake();
			}
			hifi_link_wipe(hex, sizeof(hex));
			hifi_link_wipe(sec, sizeof(sec));
			if (!ok) return err(out, cap, 400, "secret must be 64 hex digits, not all zeros");
		} else if (tone) {
			bool v;
			if (!hj_get_bool(body, "on", &v)) return err(out, cap, 400, "on (true or false) is required");
			tone_on = v;
			/* the tone is written in the I2S block hook, so the output has to
			 * run at party's rate: an idle leader takes it as a follower does
			 * (the idle output keeps whatever rate it last played) */
			if (v && !tone_held && hrole == HIFI_PARTY_ROLE_LEADER && source() == 0 && hifi_party_sink_start) {
				hifi_party_sink_start(RATE);
				tone_held = true;
			} else if (!v && tone_held) {
				if (hrole != HIFI_PARTY_ROLE_FOLLOWER && source() == SRC_PARTY && hifi_sink_stop) hifi_sink_stop();
				tone_held = false;
			}
		} else if (!strcmp(op, "on")) {
#ifdef HIFI_PARTY_NO_AUDIO
			return err(out, cap, 409, "this image plays 32-bit frames, which party mode doesn't carry");
#endif
			if (!have_secret) {
				glock();
				have_secret = load_secret();
				gunlock();
			}
			if (!have_secret) return err(out, cap, 409, "this board has no party key yet: the Hive gives it");
			want_on = true;
			config_set_value(NVS_TYPE_STR, "hifi_party_on", "1");
			wake();
		} else if (!strcmp(op, "off")) {
			want_on = false;
			config_set_value(NVS_TYPE_STR, "hifi_party_on", "0");
			wake();
		} else if (!strcmp(op, "lead")) {
			if (!active) return err(out, cap, 409, "party is off");
			glock();
			int r = hifi_party_group_lead(&st->g, ms_now(), esp_random());
			if (r >= 0) __atomic_or_fetch(&pend_do, r, __ATOMIC_SEQ_CST);
			gunlock();
			if (r == HIFI_PARTY_E_SOON) return err(out, cap, 429, "a lead was taken a moment ago: try again in a few seconds");
			if (r < 0) return err(out, cap, 409, "this board can't lead now (party is off, or memory is low)");
			wake();
		} else
			return err(out, cap, 400, "op must be on, off, lead, key or tone");
		/* the worker takes on and off within its 200 ms wait */
		for (int i = 0; i < 30 && (want_on && have_secret) != active; i++) vTaskDelay(pdMS_TO_TICKS(20));
	}
	hj_init(&j, out, cap);
	hj_obj(&j, NULL);
	state_json(&j);
	hj_obj_end(&j);
	return 200;
}
