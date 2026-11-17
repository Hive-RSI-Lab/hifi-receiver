/*
 * Lab experiment "party": the party mode prototype. One board leads and sends
 * what its DAC plays, 300 ms ahead, to a follower on the same LAN by UDP; the
 * follower plays it on its own DAC at the moment the leader does, both timed
 * against the access point's Wi-Fi TSF. The design and its numbers are in
 * projects/hifi-lite-lab/party-mode.md; the parts with no radio or I2S (the
 * packet, the fits, the jitter buffer, the controller) are core/hifi_party.c,
 * and the board's clocks (the two fits, their resets, the CLOCK it plays to)
 * are core/hifi_party_sync.c, which firmware/test/sim_party.c runs on the host.
 *
 *   api lab "{\"op\":\"start\",\"exp\":\"party\",\"role\":\"follower\",\"peer\":\"192.168.1.20\",\"key\":\"<32 hex>\",\"max_s\":600}"
 *   api lab "{\"op\":\"start\",\"exp\":\"party\",\"role\":\"leader\",\"peer\":\"192.168.1.21\",\"key\":\"<32 hex>\",\"max_s\":600}"
 *   api lab "{\"op\":\"result\",\"exp\":\"party\"}"
 *
 * Parameters: role leader|follower; peer, the other board's IPv4 address
 * (on this board's own subnet, not the gateway); key, 32 hex digits, the
 * same at both ends, made afresh for the run by whoever starts it; source
 * (leader) tone|any: tone starts the output as a test signal and plays a
 * quiet low tone with a 1 ms click every second, any sends whatever the
 * board is playing (Bluetooth, AirPlay, the player) and plays it 300 ms
 * late itself; dma_buf (frames per I2S DMA buffer) only if the output hasn't
 * restarted since boot and so hasn't said. The follower starts the output as
 * a test signal and plays the leader's stream at unity gain (the leader's
 * volume is in the stream).
 *
 * The marker. On the leader, stream frame k, for every k a multiple of 44100,
 * is replaced by one frame (+8191, -8191), with both sources. Each board
 * finds that frame in the block it is about to write and stamps the TSF at
 * which it reaches its DAC, from its own two fits; result gives the last 64
 * (k, TSF) pairs, so a host that polls both boards joins them by k for the
 * offset between the boards. The follower also gives its own offset against
 * the leader's CLOCK (the time the leader said k would play). Both are
 * measured by the boards themselves: they share the fits' errors and the
 * assumption that the DAC delay after a write is the same on both boards
 * (the same code on the same hardware). An independent number needs the two
 * outputs on one recorder (party-mode.md, section 5).
 *
 * Security: one UDP socket bound to the station's own address, port
 * HIFI_PARTY_PORT; only datagrams from the peer's address are read, and only
 * those whose tag checks under the run's key are used (a replayed AUDIO
 * packet is a duplicate, a CLOCK older than the last is dropped). Nothing is
 * sent anywhere but the peer, nothing is opened on the router (no UPnP, no
 * multicast), and the key is wiped when the run ends.
 */
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

#include "hifi_json.h"
#include "hifi_lab_esp.h"
#include "hifi_link.h"
#include "hifi_party.h"
#include "hifi_party_sync.h"
#include "hifi_scan.h"

static const char *TAG = "lab_party";

/* squeezelite-esp32, through our patch; weak as in hifi_main.c */
extern void hifi_sink_start(uint32_t rate) __attribute__((weak));
extern void hifi_sink_stop(void) __attribute__((weak));
extern int hifi_sink_source(void) __attribute__((weak));

#define RATE 44100
#define SRC_TEST 5            /* hifi_sink_source(): the test signal */
#define DELAY 13230           /* the leader plays its stream this late (300 ms): the followers' head start */
#define RING 65536            /* the leader's stream, 1.49 s; a power of two, so the per-frame index is a mask */
#define RI(x) ((uint32_t) (x) & (RING - 1))
#define TONE_N 400            /* one period of the tone: 44100 / 400 = 110.25 Hz */
#define JB_SLOTS 128          /* the follower's jitter buffer, ~1 s */
#define HIST_SLOTS 128        /* the leader's resend history, ~1 s */
#define CLOCK_EVERY_US 200000
#define TSF_EVERY_US 50000
#define MARKS 64
#define OFFS 4096
#define STACK 6144
#define SEND_GUARD 2048       /* the largest block the output writes */
#define SEND_BURST 8          /* AUDIO packets per pass (64 ms of stream): the worker then blocks, so IDLE1 runs (the task watchdog) */

/* squeezelite's types, for the hooks' signatures (patch 0021) */
typedef uint8_t u8_t;
typedef uint32_t u32_t;
typedef uint32_t frames_t;

/* (+8191, -8191) as squeezelite's 16-bit stereo frame: left in the low half */
#define MARKER (((uint32_t) (uint16_t) (int16_t) -8191 << 16) | (uint16_t) 8191)

typedef enum { ROLE_LEADER, ROLE_FOLLOWER } role_t;

typedef struct {
	int64_t k;
	double tsf;
} mark_t;

/* Made once and kept, all in PSRAM (the Lite has ~13 KB of internal RAM
 * free): a hook still running as a run ends never touches freed memory. */
typedef struct {
	hifi_party_sync_t sync; /* the fits, and the follower's CLOCK */
	hifi_party_jb_t jb;
	hifi_party_ctl_t ctl;
	hifi_party_hist_t hist;
	mark_t marks[MARKS];
	float offs[OFFS];
	uint8_t pkt[HIFI_PARTY_MAX], rx[HIFI_PARTY_MAX + 64];
	uint32_t frames[HIFI_PARTY_FRAMES];
	int64_t nk[HIFI_PARTY_NACK_MAX];
	uint8_t nack_pl[HIFI_PARTY_NACK_MAX * 8];
} st_t;

static st_t *st;
static uint32_t *ring; /* the leader's stream, RING frames */
static void *jb_mem, *hist_mem;
static hifi_link_key_t *key; /* the run's key; never logged or reported */
static SemaphoreHandle_t mx; /* everything below that the hooks and the worker share */
static TaskHandle_t worker;
static StaticTask_t worker_tcb;

static volatile bool run, alive;
static uint32_t run_no;
static role_t role;
static bool src_any, started_sink;
static uint32_t peer_ip, self_ip;
static int sock = -1;

/* from the output task's restart hook, run or not */
static volatile int boot_buf_frames, run_buf_frames;
static volatile uint32_t boot_rate;
static int buf_frames;

/* the DAC side, under mx */
static int64_t w_pos;      /* frames written since the run began */
static int64_t s_pos;      /* leader: frames of stream made */
static uint32_t s_sec, s_tone; /* leader: s_pos mod RATE and mod TONE_N, kept by counting (no 64-bit division per frame) */
static int16_t tone_tab[TONE_N];
static int64_t kw_off;     /* leader: stream index = write position + kw_off */
static uint32_t prev_out;  /* the last frame of the last block, for the marker's edge */
static frames_t last_block; /* frames the output was about to write */
static uint32_t wrong_rate, blocks, muted, output_lost, unaligned;
static int64_t last_clock_us;
/* the leader's own clock, as it last sent it (a follower's is in st->sync) */
static bool clk_ok;
static int64_t clk_k, clk_t;
static uint32_t clk_rate, clk_seq, clk_sent_seq;
static uint8_t clk_bssid[6];
/* markers and offsets */
static int marks_head, marks_n, offs_n;
static int64_t last_mark_k;
static uint32_t marks_missed;
/* the worker's side */
static uint8_t my_bssid[6];
static bool have_bssid;
static uint32_t group, seq;
static int64_t k_sent;
static uint32_t sent, send_fail, send_skipped, resent, resend_gone, nacks_sent, rx_ok, rx_bad, rx_other, clocks_sent, groups;
static uint32_t heap_start, heap_min;

static uint32_t heap_now(void) { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }

static void lock(void) { xSemaphoreTake(mx, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(mx); }

/* the TSF at which write position w reaches the DAC (as the fits put it) */
static double tsf_at(int64_t w) { return hifi_party_sync_tsf_at(&st->sync, w); }

static void mark_add(int64_t k, double tsf, bool have_ref) {
	if (last_mark_k && k > last_mark_k && k - last_mark_k > RATE) marks_missed += (uint32_t) ((k - last_mark_k) / RATE - 1);
	last_mark_k = k;
	st->marks[marks_head] = (mark_t){k, tsf};
	marks_head = (marks_head + 1) % MARKS;
	if (marks_n < MARKS) marks_n++;
	const hifi_party_sync_t *c = &st->sync; /* the follower's CLOCK */
	if (have_ref && offs_n < OFFS) st->offs[offs_n++] = (float) (tsf - (c->clk_t + (double) (k - c->clk_k) * 1e9 / c->clk_rate));
}

/* ------------------------------------------------------------------ hooks */

void hifi_party_i2s_restart(int frames, u32_t rate) {
	boot_buf_frames = frames;
	boot_rate = rate;
	if (!run || !mx) return;
	lock();
	run_buf_frames = frames;
	buf_frames = frames;
	hifi_party_sync_restart(&st->sync, frames);
	unlock();
}

/* a quiet low tone (110 Hz, about -30 dBFS) with a 1 ms click each second; the
 * output task runs this per frame on core 0, so it is table and counter only */
static uint32_t tone(void) {
	if (s_sec > 0 && s_sec < 44) return (uint32_t) (uint16_t) 6000 << 16 | 6000;
	uint16_t v = (uint16_t) tone_tab[s_tone];
	return (uint32_t) v << 16 | v;
}

static void leader_block(uint32_t *b, int n, int64_t now) {
	int64_t s0 = s_pos;
	for (int i = 0; i < n; i++, s_pos++) {
		uint32_t in = src_any ? b[i] : tone();
		if (s_sec == 0) in = MARKER;
		ring[RI(s_pos)] = in;
		b[i] = s_pos >= DELAY ? ring[RI(s_pos - DELAY)] : 0;
		if (++s_sec == RATE) s_sec = 0;
		if (++s_tone == TONE_N) s_tone = 0;
	}
	kw_off = s0 - DELAY - w_pos;
	if (hifi_party_sync_ok(&st->sync)) {
		for (int i = 0; i < n; i++) {
			int64_t k = w_pos + i + kw_off;
			if (b[i] == MARKER && (i ? b[i - 1] : prev_out) != MARKER && k >= 0 && k % RATE == 0)
				mark_add(k, tsf_at(w_pos + i), false);
		}
		if (now - last_clock_us >= CLOCK_EVERY_US) {
			if (hifi_party_sync_leader_clock(&st->sync, w_pos, &clk_t, &clk_rate, clk_bssid)) {
				clk_k = w_pos + kw_off;
				clk_ok = true;
				clk_seq++;
				last_clock_us = now;
			}
		}
	} else
		clk_ok = false;
}

static void follower_block(uint32_t *b, int n, int64_t now) {
	double target;
	if (hifi_party_sync_target(&st->sync, w_pos, now, &target)) {
		hifi_party_render(&st->ctl, &st->jb, target, b, n, 65536, 65536);
		for (int i = 0; i < n; i++)
			if (b[i] == MARKER && (i ? b[i - 1] : prev_out) != MARKER) {
				int64_t k = llround((target + i) / RATE) * RATE;
				mark_add(k, tsf_at(w_pos + i), true);
			}
	} else {
		/* no clock to play to: fade out over the block rather than cut (a
		 * click), and keep the buffer and controller in step with the time */
		hifi_party_mute(&st->ctl, &st->jb, b, n, 65536, 65536);
		muted++;
	}
}

void hifi_party_i2s_block(u8_t *buf, frames_t frames, u32_t rate) {
	if (!run || !mx || !frames) return;
	if ((uintptr_t) buf & 3) {
		unaligned++;
		return;
	}
	if (role == ROLE_FOLLOWER && hifi_sink_source && hifi_sink_source() != SRC_TEST) {
		output_lost++; /* something else took the output: leave it alone */
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
	if (role == ROLE_LEADER) leader_block(b, (int) frames, now);
	else follower_block(b, (int) frames, now);
	prev_out = b[frames - 1];
	last_block = frames;
	unlock();
}

void hifi_party_i2s_wrote(frames_t frames, int64_t t0, int64_t t1) {
	if (!run || !mx) return;
	lock();
	int64_t w0 = w_pos;
	w_pos += frames;
	hifi_party_sync_wrote(&st->sync, w0, w_pos, t0, t1, (int) last_block);
	last_block = 0;
	unlock();
}

/* ------------------------------------------------------------------ worker */

static void send_pkt(const uint8_t *p, size_t n) {
	struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(HIFI_PARTY_PORT), .sin_addr.s_addr = htonl(peer_ip)};
	if (sendto(sock, p, n, 0, (struct sockaddr *) &to, sizeof(to)) == (int) n) sent++;
	else send_fail++; /* the radio's queue is full: the follower asks again */
}

static void tsf_sample(void) {
	int64_t a = esp_timer_get_time();
	int64_t tsf = esp_wifi_get_tsf_time(WIFI_IF_STA);
	int64_t b = esp_timer_get_time();
	lock();
	/* a zero TSF (not associated) or another AP's starts the TSF fit again */
	hifi_party_sync_tsf(&st->sync, a, b, tsf, have_bssid ? my_bssid : NULL);
	unlock();
}

static void leader_send(void) {
	for (int burst = 0; burst < SEND_BURST; burst++) {
		lock();
		int64_t avail = s_pos - k_sent;
		if (avail > RING - SEND_GUARD) { /* the worker fell more than a ring behind */
			int64_t to = (s_pos - DELAY) / HIFI_PARTY_FRAMES * HIFI_PARTY_FRAMES;
			if (to > k_sent) {
				send_skipped += (uint32_t) ((to - k_sent) / HIFI_PARTY_FRAMES);
				k_sent = to;
			}
			avail = s_pos - k_sent;
		}
		if (avail < HIFI_PARTY_FRAMES) {
			unlock();
			break;
		}
		for (int i = 0; i < HIFI_PARTY_FRAMES; i++) st->frames[i] = ring[RI(k_sent + i)];
		unlock();
		hifi_party_pkt_t p = {.type = HIFI_PARTY_AUDIO, .group = group, .seq = seq++, .k = k_sent, .rate = RATE, .n = HIFI_PARTY_FRAMES};
		size_t n = hifi_party_encode(key, &p, st->frames, st->pkt, sizeof(st->pkt));
		if (n) {
			hifi_party_hist_put(&st->hist, k_sent, st->pkt, n);
			send_pkt(st->pkt, n);
		}
		k_sent += HIFI_PARTY_FRAMES;
	}
	lock();
	bool have = clk_ok && clk_seq != clk_sent_seq;
	hifi_party_pkt_t p = {.type = HIFI_PARTY_CLOCK, .group = group, .seq = seq++, .k = clk_k, .t = clk_t, .rate = clk_rate};
	clk_sent_seq = clk_seq;
	uint8_t bss[6];
	memcpy(bss, clk_bssid, 6); /* the AP whose TSF the clock is in */
	unlock();
	if (have) {
		size_t n = hifi_party_encode(key, &p, bss, st->pkt, sizeof(st->pkt));
		if (n) {
			send_pkt(st->pkt, n);
			clocks_sent++;
		}
	}
}

static void follower_nack(int64_t now) {
	lock();
	int m = hifi_party_jb_nacks(&st->jb, now, st->nk, HIFI_PARTY_NACK_MAX);
	unlock();
	if (!m || !group) return;
	for (int i = 0; i < m; i++)
		for (int j = 0; j < 8; j++) st->nack_pl[i * 8 + j] = (uint8_t) ((uint64_t) st->nk[i] >> (8 * j));
	hifi_party_pkt_t p = {.type = HIFI_PARTY_NACK, .group = group, .seq = seq++, .n = (uint16_t) m};
	size_t n = hifi_party_encode(key, &p, st->nack_pl, st->pkt, sizeof(st->pkt));
	if (n) {
		send_pkt(st->pkt, n);
		nacks_sent++;
	}
}

static void follower_reset(uint32_t g) {
	lock();
	hifi_party_jb_init(&st->jb, jb_mem, JB_SLOTS);
	hifi_party_ctl_init(&st->ctl, RATE);
	hifi_party_sync_forget_clock(&st->sync);
	group = g;
	unlock();
	groups++;
}

static void got(const uint8_t *d, size_t len, int64_t now) {
	hifi_party_pkt_t p;
	const uint8_t *pl;
	size_t pn;
	if (hifi_party_decode(key, d, len, &p, &pl, &pn) != HIFI_PARTY_OK) {
		rx_bad++;
		return;
	}
	rx_ok++;
	if (role == ROLE_LEADER) {
		if (p.type != HIFI_PARTY_NACK || p.group != group) return;
		for (int i = 0; i < p.n; i++) {
			size_t n;
			const uint8_t *q = hifi_party_hist_get(&st->hist, hifi_party_nack_k(pl, i), &n);
			if (q) {
				send_pkt(q, n);
				resent++;
			} else
				resend_gone++;
		}
		return;
	}
	if (p.type == HIFI_PARTY_NACK) return;
	if (p.group != group) follower_reset(p.group); /* the leader started a new run */
	if (p.type == HIFI_PARTY_AUDIO) {
		lock();
		hifi_party_jb_put(&st->jb, p.k, pl, p.n, now);
		unlock();
	} else if (p.type == HIFI_PARTY_CLOCK) {
		lock();
		hifi_party_sync_clock(&st->sync, p.k, p.t, p.rate, p.seq, pl, now); /* drops a replay, or one late */
		unlock();
	}
}

static void teardown(void) {
	run = false;
	if (sock >= 0) {
		close(sock);
		sock = -1;
	}
	if (started_sink && hifi_sink_stop && hifi_sink_source && hifi_sink_source() == SRC_TEST) hifi_sink_stop();
	started_sink = false;
	if (key) hifi_link_key_wipe(key); /* the run's key goes with the run */
}

static void one_run(void) {
	uint32_t my_run = run_no;
	int64_t next_tsf = 0, next_sec = 0;
	while (run) {
		int64_t now = esp_timer_get_time();
		if (now >= next_tsf) {
			tsf_sample();
			next_tsf = now + TSF_EVERY_US;
		}
		if (now >= next_sec) {
			wifi_ap_record_t ap;
			have_bssid = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
			if (have_bssid) memcpy(my_bssid, ap.bssid, 6);
			next_sec = now + 1000000;
		}
		uint32_t h = heap_now();
		if (h < heap_min) heap_min = h;
		if (role == ROLE_LEADER) leader_send();
		else follower_nack(now);
		fd_set r;
		FD_ZERO(&r);
		FD_SET(sock, &r);
		struct timeval tv = {.tv_sec = 0, .tv_usec = 10000};
		if (select(sock + 1, &r, NULL, NULL, &tv) <= 0) continue;
		for (int i = 0; i < 32; i++) {
			struct sockaddr_in from;
			socklen_t fl = sizeof(from);
			int n = recvfrom(sock, st->rx, sizeof(st->rx), MSG_DONTWAIT, (struct sockaddr *) &from, &fl);
			if (n <= 0) break;
			if (from.sin_addr.s_addr != htonl(peer_ip) || from.sin_port != htons(HIFI_PARTY_PORT)) {
				rx_other++;
				continue;
			}
			got(st->rx, (size_t) n, esp_timer_get_time());
		}
		vTaskDelay(1); /* select didn't block: give IDLE1 its tick anyway */
	}
	bool ended_itself = run;
	teardown();
	if (ended_itself) hifi_lab_esp_finished("party", my_run, true);
	ESP_LOGI(TAG, "stopped: sent %u, received %u", (unsigned) sent, (unsigned) rx_ok);
}

static void task_main(void *arg) {
	for (;;) {
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		one_run();
		alive = false;
	}
}

/* ------------------------------------------------------------------ start, stop, result */

static uint32_t num(const char *body, const char *k, uint32_t dflt) {
	double v;
	if (!hj_get_num(body, k, &v) || !(v >= 0)) return dflt;
	return v >= 4294967295.0 ? UINT32_MAX : (uint32_t) v;
}

static bool parse_ip(const char *s, uint32_t *ip) {
	unsigned a, b, c, d;
	char end;
	if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &end) != 4 || a > 255 || b > 255 || c > 255 || d > 255) return false;
	*ip = a << 24 | b << 16 | c << 8 | d;
	return true;
}

static bool alloc_once(void) {
	if (!st) st = heap_caps_calloc(1, sizeof(*st), MALLOC_CAP_SPIRAM);
	if (!ring) ring = heap_caps_calloc(RING, 4, MALLOC_CAP_SPIRAM);
	if (!jb_mem) jb_mem = heap_caps_calloc(1, hifi_party_jb_bytes(JB_SLOTS), MALLOC_CAP_SPIRAM);
	if (!hist_mem) hist_mem = heap_caps_calloc(1, hifi_party_hist_bytes(HIST_SLOTS), MALLOC_CAP_SPIRAM);
	if (!key) key = heap_caps_calloc(1, sizeof(*key), MALLOC_CAP_SPIRAM);
	if (!mx) mx = xSemaphoreCreateMutex();
	if (!tone_tab[1])
		for (int i = 0; i < TONE_N; i++) tone_tab[i] = (int16_t) lrintf(1000.0f * sinf(2.0f * (float) M_PI * (float) i / TONE_N));
	if (!st || !ring || !jb_mem || !hist_mem || !key || !mx) return false;
	if (!worker) {
		StackType_t *stack = heap_caps_malloc(STACK, MALLOC_CAP_SPIRAM);
		if (!stack) return false;
		worker = xTaskCreateStaticPinnedToCore(task_main, "lab_party", STACK, NULL, ESP_TASK_PRIO_MIN + 5, stack, &worker_tcb, 1);
		if (!worker) {
			free(stack);
			return false;
		}
	}
	return true;
}

static int start(uint32_t run_n, const char *body, char *err, size_t n) {
	if (alive) {
		snprintf(err, n, "the last run is still stopping");
		return 409;
	}
	char s[24];
	if (!hj_get_str(body, "role", s, sizeof(s))) s[0] = 0;
	if (!strcmp(s, "leader")) role = ROLE_LEADER;
	else if (!strcmp(s, "follower")) role = ROLE_FOLLOWER;
	else {
		snprintf(err, n, "role must be leader or follower");
		return 400;
	}
	if (!hj_get_str(body, "source", s, sizeof(s))) strcpy(s, "tone");
	if (strcmp(s, "tone") && strcmp(s, "any")) {
		snprintf(err, n, "source must be tone or any");
		return 400;
	}
	src_any = !strcmp(s, "any");
	if (src_any && role != ROLE_LEADER) {
		snprintf(err, n, "source is the leader's");
		return 400;
	}
	uint32_t pip;
	if (!hj_get_str(body, "peer", s, sizeof(s)) || !parse_ip(s, &pip)) {
		snprintf(err, n, "peer (the other board's IPv4 address) is required");
		return 400;
	}
	esp_netif_t *ni = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
	esp_netif_ip_info_t ip;
	if (!ni || esp_netif_get_ip_info(ni, &ip) != ESP_OK || !ip.ip.addr) {
		snprintf(err, n, "the station has no address");
		return 503;
	}
	/* the peer only on this board's own LAN: never the gateway or past it */
	if (!hifi_scan_target_ok(ntohl(ip.ip.addr), ntohl(ip.netmask.addr), ntohl(ip.gw.addr), pip, err, n)) return 400;
	int bf = run_buf_frames ? run_buf_frames : boot_buf_frames;
	if (!bf) bf = (int) num(body, "dma_buf", 0);
	if (bf < 64 || bf > 4096) {
		snprintf(err, n, "the output hasn't said its DMA buffer size since boot: give dma_buf (frames)");
		return 400;
	}
	uint8_t k[HIFI_LINK_KEY];
	char ks[2 * HIFI_LINK_KEY + 2];
	bool have_key = hj_get_str(body, "key", ks, sizeof(ks)) && hifi_link_parse_key(ks, k);
	hifi_link_wipe(ks, sizeof(ks));
	if (!have_key) {
		snprintf(err, n, "key (32 hex digits, the same at both ends) is required");
		return 400;
	}
	if (role == ROLE_FOLLOWER || !src_any) {
		if (!hifi_sink_start || !hifi_sink_source) {
			hifi_link_wipe(k, sizeof(k));
			snprintf(err, n, "this image has no test signal output");
			return 503;
		}
		int src = hifi_sink_source();
		if (src != 0 && src != SRC_TEST) {
			hifi_link_wipe(k, sizeof(k));
			snprintf(err, n, "the output is playing something else (source %d)", src);
			return 409;
		}
	}
	if (!alloc_once()) {
		hifi_link_wipe(k, sizeof(k));
		snprintf(err, n, "out of memory");
		return 503;
	}
	int so = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	struct sockaddr_in me = {.sin_family = AF_INET, .sin_port = htons(HIFI_PARTY_PORT), .sin_addr.s_addr = ip.ip.addr};
	if (so < 0 || bind(so, (struct sockaddr *) &me, sizeof(me))) {
		if (so >= 0) close(so);
		hifi_link_wipe(k, sizeof(k));
		snprintf(err, n, "no UDP socket on port %d", HIFI_PARTY_PORT);
		return 503;
	}

	/* the worker is idle and run is false: the hooks touch nothing */
	lock();
	sock = so;
	peer_ip = pip;
	self_ip = ntohl(ip.ip.addr);
	buf_frames = bf;
	run_buf_frames = 0;
	hifi_party_sync_init(&st->sync, bf);
	hifi_party_jb_init(&st->jb, jb_mem, JB_SLOTS);
	hifi_party_ctl_init(&st->ctl, RATE);
	hifi_party_hist_init(&st->hist, hist_mem, HIST_SLOTS);
	w_pos = s_pos = kw_off = k_sent = 0;
	s_sec = s_tone = 0;
	prev_out = 0;
	last_block = 0;
	wrong_rate = blocks = muted = output_lost = unaligned = 0;
	last_clock_us = 0;
	clk_ok = false;
	clk_k = clk_t = 0;
	clk_rate = clk_seq = clk_sent_seq = 0;
	marks_head = marks_n = offs_n = 0;
	last_mark_k = 0;
	marks_missed = 0;
	have_bssid = false;
	group = role == ROLE_LEADER ? (esp_random() | 1) : 0;
	seq = 1;
	memset(clk_bssid, 0, 6);
	sent = send_fail = send_skipped = resent = resend_gone = nacks_sent = rx_ok = rx_bad = rx_other = 0;
	clocks_sent = groups = 0;
	heap_start = heap_min = heap_now();
	hifi_link_key_init(key, k);
	hifi_link_wipe(k, sizeof(k));
	unlock();

	started_sink = false;
	if (role == ROLE_FOLLOWER || !src_any) {
		if (hifi_sink_source() != SRC_TEST) {
			hifi_sink_start(RATE);
			started_sink = true;
		}
	}
	run_no = run_n;
	run = alive = true;
	xTaskNotifyGive(worker);
	ESP_LOGI(TAG, "%s, peer %u.%u.%u.%u, source %s", role == ROLE_LEADER ? "leader" : "follower", (unsigned) (pip >> 24),
		 (unsigned) (pip >> 16 & 255), (unsigned) (pip >> 8 & 255), (unsigned) (pip & 255), src_any ? "any" : "tone");
	return 0;
}

static bool stop(void) {
	run = false;
	for (int i = 0; i < 300 && alive; i++) vTaskDelay(pdMS_TO_TICKS(10));
	if (alive) ESP_LOGE(TAG, "the task didn't stop in 3 s");
	return !alive;
}

static int cmp_float(const void *a, const void *b) {
	float x = *(const float *) a, y = *(const float *) b;
	return x < y ? -1 : x > y;
}

/* The result is one console line, which the bench caps at 1000 characters,
 * so the keys are short (party-mode.md, section 5, says what each is) and
 * only the last MARKS_SHOWN markers go out: poll faster than one per
 * MARKS_SHOWN seconds to see every one. */
#define MARKS_SHOWN 8

static void fit_json(hj_t *j, const char *name, const hifi_party_fit_t *f) {
	hj_obj(j, name);
	hj_int(j, "ok", f->ok);
	hj_num(j, "rms", f->rms, 1);
	hj_int(j, "n", f->n);
	hj_int(j, "rej", f->rejected);
	hj_obj_end(j);
}

static void result(void *hj) {
	hj_t *j = hj;
	if (!st || !mx) return;
	static float *sorted; /* PSRAM: the offsets, sorted for the percentiles */
	if (!sorted) sorted = heap_caps_malloc(sizeof(st->offs), MALLOC_CAP_SPIRAM);
	lock();
	hj_str(j, "role", role == ROLE_LEADER ? (src_any ? "leader-any" : "leader-tone") : "follower");
	hj_int(j, "buf", buf_frames);
	hj_int(j, "w", w_pos);
	const hifi_party_sync_t *sy = &st->sync;
	bool lead = role == ROLE_LEADER;
	hj_int(j, "re", sy->dac_resets); /* the DAC fit started again: an output restart, a short write, a lasting jump */
	if (sy->tsf_resets) hj_int(j, "tr", sy->tsf_resets); /* the TSF fit started again: a step, a roam, a disconnect */
	if (sy->short_writes) hj_int(j, "sw", sy->short_writes);
	if (wrong_rate) hj_int(j, "wr", wrong_rate);
	if (unaligned) hj_int(j, "ua", unaligned);
	if (sy->tsf_zero) hj_int(j, "tz", sy->tsf_zero);
	fit_json(j, "tsf", &sy->tsf);
	fit_json(j, "dac", &sy->dac);
	hj_arr(j, "clk"); /* seq, frames per 1000 s of TSF, CLOCK packets */
	hj_int(j, NULL, lead ? clk_seq : sy->clk_seq);
	hj_int(j, NULL, lead ? clk_rate : sy->clk_rate);
	hj_int(j, NULL, lead ? clocks_sent : sy->clocks);
	hj_arr_end(j);
	hj_obj(j, "net");
	hj_int(j, "tx", sent);
	hj_int(j, "txf", send_fail);
	hj_int(j, "rx", rx_ok);
	hj_int(j, "bad", rx_bad);
	hj_int(j, "oth", rx_other);
	if (role == ROLE_LEADER) {
		hj_int(j, "rs", resent);
		hj_int(j, "gone", resend_gone);
		hj_int(j, "skip", send_skipped);
	} else {
		hj_int(j, "nack", nacks_sent);
		hj_int(j, "oldc", sy->old_clocks);
		hj_int(j, "grp", groups);
		hj_int(j, "bss", sy->bssid_wrong);
		if (sy->stale) hj_int(j, "stale", sy->stale);
	}
	hj_obj_end(j);
	if (role == ROLE_FOLLOWER) {
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
	}
	hj_arr(j, "heap"); /* internal RAM free: at the start, the least during the run */
	hj_int(j, NULL, heap_start);
	hj_int(j, NULL, heap_min);
	hj_arr_end(j);
	if (marks_missed) hj_int(j, "mm", marks_missed);
	int on = offs_n;
	if (on && sorted) {
		/* the follower's offset against the leader's CLOCK, µs */
		double sum = 0, mx_abs = 0;
		for (int i = 0; i < on; i++) {
			sorted[i] = fabsf(st->offs[i]);
			sum += st->offs[i];
			if (sorted[i] > mx_abs) mx_abs = sorted[i];
		}
		float last = st->offs[on - 1];
		unlock(); /* the sort holds nothing the hooks need */
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
	/* the last markers, oldest first: [k / 44100, the TSF (µs, rounded) at which it reached the DAC] */
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

const hifi_lab_exp_t lab_party = {
	.name = "party",
	.help = "party mode prototype: role leader|follower, peer (IPv4 on this LAN), key (32 hex, both ends), source tone|any (leader), dma_buf",
	.start = start,
	.stop = stop,
	.result = result,
};
