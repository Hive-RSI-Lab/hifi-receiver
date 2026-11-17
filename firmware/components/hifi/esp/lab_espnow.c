/*
 * Lab experiment "espnow": an ESP-NOW link to another board, alongside the
 * Wi-Fi station (so on the access point's channel: ESP-NOW has to share the
 * one channel the radio is on).
 *
 *   api lab "{\"op\":\"start\",\"exp\":\"espnow\",\"role\":\"echo\",\"peer\":\"3076f52d742c\",\"max_s\":120}"
 *   api lab "{\"op\":\"start\",\"exp\":\"espnow\",\"role\":\"ping\",\"peer\":\"3076f52d743c\",\"rate_hz\":20}"
 *   api lab "{\"op\":\"result\",\"exp\":\"espnow\"}"
 *
 * Roles: echo answers the peer's PINGs and counts its DATA (run it on the
 * far board first); ping sends rate_hz PINGs of len bytes (round trip, loss,
 * clock offset and drift); flood sends DATA as fast as the radio takes it,
 * window frames in flight (throughput; the echo end has the loss).
 * Optional: count (frames, then the run ends itself), len (38-250),
 * rate ("1m" default, "2m", "5m", "11m", "6m", "12m", "24m", "54m",
 * "mcs0", "mcs7"), ps (Wi-Fi power save for the run: "none" (only with
 * the Bluetooth controller off: the bt experiment), "min", "max";
 * default as it is), window (flood, 1-16, default 4; a send report missing
 * for 1 s counts as lost and its slot is used again: report_timeouts).
 *
 * Ways to get frames through a receiver in modem sleep (flood):
 *   retry (0-8), retry_ms (0-500): a frame the radio reports not acked is
 *     sent again (same seq; the echo end counts the copies as dup) after
 *     retry_ms, up to retry more times: retries, recovered, gave_up.
 *   sync_us (0, or 10000-1000000), win_us, off_us: send only while the
 *     station's TSF (the access point's clock, the same on both boards)
 *     modulo sync_us is in [off_us, off_us + win_us): bursts timed to the
 *     receiver's beacon wakes. sync_us 1 (DTIM) takes the beacon interval
 *     times the DTIM period read with beacon, and off_us from its DTIM phase.
 *   beacon (any role, 1): listen 1 s for the access point's beacons first
 *     and give its beacon interval, DTIM period and DTIM phase.
 *   li (echo, 1-100): the station's listen_interval for the run (it counts
 *     in ps max only), read back after setting; put back at the stop.
 *
 * Only frames from the peer are looked at; nothing is forwarded anywhere.
 * The stop (asked, deadline, low RAM or the run's own end) takes ESP-NOW
 * down again and puts the rate back, and the power save too unless someone
 * else changed it during the run. Starts and stops come one at a time, under
 * the lab's lock: `alive` is checked, then set, with nothing in between.
 */
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_heap_caps.h"
#if CONFIG_BT_ENABLED
#include "esp_bt.h"
#endif
#include "esp_log.h"
#include "esp_now.h"
#include "esp_system.h"
#include "esp_task.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "hifi_json.h"
#include "hifi_lab_esp.h"
#include "hifi_link.h"

static const char *TAG = "lab_espnow";

typedef enum { ROLE_ECHO, ROLE_PING, ROLE_FLOOD } role_t;
static const char *const ROLE_NAMES[] = {"echo", "ping", "flood"};

typedef struct {
	uint8_t mac[6];
	uint8_t len;
	int64_t t_us;
	uint8_t data[HIFI_LINK_MAX];
} rx_item_t;

#define QLEN 32
#define STACK 4096

/* Made once and kept: a callback still running as ESP-NOW comes down never
 * touches freed memory. The Lite has ~13 KB of internal RAM free with Wi-Fi,
 * Bluetooth and the player up, so the big parts live in PSRAM: the queue's
 * storage (QLEN frames, ~8 KB) and the worker's stack. The worker is made
 * once and waits for a run, so its static stack is never reused under a
 * task that is still being deleted. Its stack is in PSRAM: it must never
 * write flash. */
static QueueHandle_t q;
static StaticQueue_t q_ctl;
static SemaphoreHandle_t window_sem;
static TaskHandle_t worker;
static StaticTask_t worker_tcb;
static hifi_link_tx_t *tx;
static hifi_link_rx_t *rx;
static hifi_link_key_t *key; /* the run's frame key, from the start op; never logged or reported */
static rx_item_t *rx_it; /* the receive callback's item, in PSRAM: ~270 bytes the Lite's internal RAM can't spare */
static uint32_t heap_start, heap_after_init, heap_min;

static volatile bool run, alive;
static role_t role;
static uint8_t peer[6], self[6];
static uint32_t rate_hz, len, count, window;
static uint8_t channel;
static char rate_name[8], ps_err[24];
static const char *ps_run = "?"; /* the power save in effect for the run */
static bool ps_changed, rate_changed;
static wifi_ps_type_t ps_before, ps_set;
static uint32_t run_no; /* the lab's number for this run, given back when it ends itself */
static volatile uint32_t q_full, other, report_timeouts, no_mem;

/* the flood's retries: the frames in flight in send order (ESP-NOW gives
 * the send reports in that order) and the ones waiting to go again, both
 * in PSRAM with the rest */
#define INFL 16
#define RETRYQ 64
typedef struct {
	uint32_t seq;
	uint8_t att;
} infl_t;
typedef struct {
	infl_t f[INFL];
	uint8_t head, n;
	struct {
		uint32_t seq;
		uint8_t att;
		int64_t due;
	} r[RETRYQ];
	uint8_t rhead, rn;
} rt_t;
static rt_t *rt;
static portMUX_TYPE rt_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t retry, retry_ms, sync_us, win_us, off_us;
static volatile uint32_t retries, recovered, gave_up, rq_full, outside_win;
/* beacon: what the access point says, read once at the start */
static bool want_beacon, beacon_seen;
static uint8_t ap_bssid[6], dtim_period, dtim_count;
static uint16_t bi_tu;
static int64_t beacon_tsf;
static uint32_t dtim_phase_us, beacons;
static uint16_t li_want, li_before, li_now;
static bool li_changed;

/* on the Wi-Fi task only: its item (rx_it) is made once, off that task's stack */
static void recv_cb(const uint8_t *mac, const uint8_t *data, int n) {
	int64_t t = esp_timer_get_time();
	if (!run || n <= 0 || n > HIFI_LINK_MAX) return;
	if (memcmp(mac, peer, 6)) {
		other++;
		return;
	}
	rx_item_t *it = rx_it;
	memcpy(it->mac, mac, 6);
	it->len = (uint8_t) n;
	it->t_us = t;
	memcpy(it->data, data, n);
	if (xQueueSend(q, it, 0) != pdTRUE) q_full++;
}

/* the oldest frame in flight leaves: acked, nacked or its report lost */
static void infl_done(bool ok) {
	portENTER_CRITICAL(&rt_mux);
	if (rt->n) {
		infl_t x = rt->f[rt->head];
		rt->head = (rt->head + 1) % INFL;
		rt->n--;
		if (ok) {
			if (x.att) recovered++;
		} else if (x.att < retry) {
			if (rt->rn < RETRYQ) {
				int i = (rt->rhead + rt->rn) % RETRYQ;
				rt->r[i].seq = x.seq;
				rt->r[i].att = x.att + 1;
				rt->r[i].due = esp_timer_get_time() + (int64_t) retry_ms * 1000;
				rt->rn++;
			} else
				rq_full++;
		} else if (retry)
			gave_up++;
	}
	portEXIT_CRITICAL(&rt_mux);
}

static void send_cb(const uint8_t *mac, esp_now_send_status_t st) {
	if (!run) return;
	hifi_link_tx_acked(tx, st == ESP_NOW_SEND_SUCCESS);
	if (role == ROLE_FLOOD) {
		infl_done(st == ESP_NOW_SEND_SUCCESS);
		xSemaphoreGive(window_sem);
	}
}

/* the next frame due to go again, or false */
static bool retry_due(int64_t now, uint32_t *seq, uint8_t *att) {
	bool got = false;
	portENTER_CRITICAL(&rt_mux);
	if (rt->rn && rt->r[rt->rhead].due <= now) {
		*seq = rt->r[rt->rhead].seq;
		*att = rt->r[rt->rhead].att;
		rt->rhead = (rt->rhead + 1) % RETRYQ;
		rt->rn--;
		got = true;
	}
	portEXIT_CRITICAL(&rt_mux);
	return got;
}

/* before esp_now_send: the report for it comes after the ones before it */
static void infl_push(uint32_t seq, uint8_t att) {
	portENTER_CRITICAL(&rt_mux);
	if (rt->n < INFL) {
		rt->f[(rt->head + rt->n) % INFL] = (infl_t){seq, att};
		rt->n++;
	}
	portEXIT_CRITICAL(&rt_mux);
}

/* the send was refused: no report will come for the newest one */
static void infl_pop_tail(void) {
	portENTER_CRITICAL(&rt_mux);
	if (rt->n) rt->n--;
	portEXIT_CRITICAL(&rt_mux);
}

/* microseconds until the send window opens; 0 inside it */
static int64_t until_window(void) {
	if (!sync_us) return 0;
	int64_t tsf = esp_wifi_get_tsf_time(WIFI_IF_STA);
	if (tsf <= 0) return 0;
	int64_t ph = (tsf - off_us) % sync_us;
	if (ph < 0) ph += sync_us;
	return ph < (int64_t) win_us ? 0 : sync_us - ph;
}

static void promisc_cb(void *buf, wifi_promiscuous_pkt_type_t type) {
	if (type != WIFI_PKT_MGMT || beacon_seen) return;
	const wifi_promiscuous_pkt_t *p = buf;
	const uint8_t *d = p->payload;
	int n = p->rx_ctrl.sig_len - 4; /* the FCS */
	if (n < 36 || d[0] != 0x80 || memcmp(d + 16, ap_bssid, 6)) return; /* a beacon from our AP */
	beacons++;
	int64_t ts = 0;
	for (int i = 7; i >= 0; i--) ts = ts << 8 | d[24 + i];
	uint16_t bi = d[32] | d[33] << 8;
	if (!bi) return; /* a beacon interval of 0 would divide by zero below */
	for (int i = 36; i + 2 <= n && i + 2 + d[i + 1] <= n; i += 2 + d[i + 1]) {
		if (d[i] == 5 && d[i + 1] >= 2) {
			bi_tu = bi;
			dtim_count = d[i + 2];
			dtim_period = d[i + 3];
			beacon_tsf = ts;
			beacon_seen = true;
			return;
		}
	}
}

/* listen up to 1 s for one beacon of the AP with its TIM */
static void read_beacon(void) {
	wifi_ap_record_t ap;
	beacon_seen = false;
	beacons = 0;
	if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return;
	memcpy(ap_bssid, ap.bssid, 6);
	wifi_promiscuous_filter_t flt = {.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT};
	esp_wifi_set_promiscuous_filter(&flt);
	esp_wifi_set_promiscuous_rx_cb(promisc_cb);
	if (esp_wifi_set_promiscuous(true) != ESP_OK) return;
	for (int i = 0; i < 100 && !beacon_seen; i++) vTaskDelay(pdMS_TO_TICKS(10));
	esp_wifi_set_promiscuous(false);
	esp_wifi_set_promiscuous_rx_cb(NULL);
	if (!beacon_seen) return;
	/* TBTTs fall where the TSF is a multiple of the beacon interval; the
	 * DTIM one is dtim_count intervals after this beacon's */
	int64_t bi_us = (int64_t) bi_tu * 1024, per = bi_us * (dtim_period ? dtim_period : 1);
	int64_t tbtt = beacon_tsf - beacon_tsf % bi_us;
	dtim_phase_us = (uint32_t) ((tbtt + (int64_t) dtim_count * bi_us) % per);
}

static bool send_frame(hifi_link_frame_t *f) {
	uint8_t buf[HIFI_LINK_MAX];
	size_t n = hifi_link_encode(key, f, buf, sizeof(buf));
	if (!n) return false;
	int64_t t = esp_timer_get_time();
	if (f->type == HIFI_LINK_PING) {
		f->t1 = t;
		n = hifi_link_encode(key, f, buf, sizeof(buf));
	} else if (f->type == HIFI_LINK_PONG) {
		f->t3 = t;
		n = hifi_link_encode(key, f, buf, sizeof(buf));
	}
	esp_err_t e = esp_now_send(peer, buf, n);
	if (e == ESP_ERR_ESPNOW_NO_MEM) no_mem++;
	hifi_link_tx_sent(tx, n, e == ESP_OK, t);
	return e == ESP_OK;
}

static void handle(const rx_item_t *it) {
	hifi_link_frame_t pong;
	switch (hifi_link_receive(key, rx, tx, role == ROLE_ECHO, role == ROLE_PING, it->data, it->len, it->t_us, &pong)) {
	case HIFI_LINK_GOT_REPLY:
		send_frame(&pong);
		break;
	case HIFI_LINK_GOT_OTHER:
		other++;
		break;
	default:
		break;
	}
}

static void teardown(void) {
	run = false;
	esp_now_unregister_recv_cb();
	esp_now_unregister_send_cb();
	/* the rate while ESP-NOW is still up, as it was set */
	if (rate_changed) esp_wifi_config_espnow_rate(WIFI_IF_STA, WIFI_PHY_RATE_1M_L);
	esp_now_deinit();
	wifi_ps_type_t now_ps;
	/* the power save only if it is still ours: someone may have set it since */
	if (ps_changed && esp_wifi_get_ps(&now_ps) == ESP_OK && now_ps == ps_set) esp_wifi_set_ps(ps_before);
	if (li_changed) {
		wifi_config_t c;
		if (esp_wifi_get_config(WIFI_IF_STA, &c) == ESP_OK && c.sta.listen_interval == li_now) {
			c.sta.listen_interval = li_before;
			esp_wifi_set_storage(WIFI_STORAGE_RAM); /* not into NVS */
			esp_wifi_set_config(WIFI_IF_STA, &c);
			esp_wifi_set_storage(WIFI_STORAGE_FLASH);
		}
	}
	rate_changed = ps_changed = li_changed = false;
	if (key) memset(key, 0, sizeof(*key)); /* the run's key goes with the run */
}

static uint32_t heap_now(void) { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }

static void one_run(void) {
	uint32_t my_run = run_no;
	rx_item_t it;
	uint32_t seq = 0;
	int64_t period = 1000000 / (rate_hz ? rate_hz : 1);
	int64_t next = esp_timer_get_time(), done_at = 0;
	hifi_link_frame_t f = {0};
	f.session = tx->session;
	f.len = (uint16_t) len;
	while (run) {
		int64_t now = esp_timer_get_time();
		uint32_t h = heap_now();
		if (h < heap_min) heap_min = h;
		if (role == ROLE_FLOOD) {
			if (count && seq >= count && !(retry && (rt->rn || rt->n))) {
				/* let the last send reports in (and their retries go) */
				if (!done_at) done_at = now + 500000 + (int64_t) retry_ms * 1000;
				if (now >= done_at) break;
				vTaskDelay(pdMS_TO_TICKS(20));
				continue;
			}
			int64_t w = until_window();
			if (w) {
				outside_win++;
				int64_t ms = w / 1000 > 100 ? 100 : w / 1000; /* a stop is seen within 100 ms */
				TickType_t t = ms / portTICK_PERIOD_MS;
				vTaskDelay(t ? t : 1); /* never spin: idle on this core feeds the watchdog */
				continue;
			}
			/* a send report missing for 1 s: count it lost, use its slot */
			if (xSemaphoreTake(window_sem, pdMS_TO_TICKS(1000)) != pdTRUE) {
				/* if that report comes after all, it retires the next frame
				 * in flight instead: the counts are off by one from here,
				 * so a run with report_timeouts > 0 is suspect */
				report_timeouts++;
				infl_done(false);
			}
			done_at = 0; /* retries still pending: the grace time starts again after them */
			uint32_t rseq;
			uint8_t att = 0;
			bool again = retry && retry_due(now, &rseq, &att);
			if (!again && count && seq >= count) { /* all sent: only retries may go, when due */
				xSemaphoreGive(window_sem);
				vTaskDelay(1);
				continue;
			}
			f.type = HIFI_LINK_DATA;
			f.seq = again ? rseq : seq;
			infl_push(f.seq, att);
			if (send_frame(&f)) {
				if (again) retries++;
				else seq++;
			} else {
				infl_pop_tail();
				if (again) { /* back to the front of the queue */
					portENTER_CRITICAL(&rt_mux);
					if (rt->rn < RETRYQ) {
						rt->rhead = (rt->rhead + RETRYQ - 1) % RETRYQ;
						rt->r[rt->rhead].seq = rseq;
						rt->r[rt->rhead].att = att;
						rt->r[rt->rhead].due = now;
						rt->rn++;
					} else
						rq_full++;
					portEXIT_CRITICAL(&rt_mux);
				}
				xSemaphoreGive(window_sem);
				vTaskDelay(1);
			}
			continue;
		}
		TickType_t wait = pdMS_TO_TICKS(100);
		if (role == ROLE_PING && !done_at) {
			if (now >= next) {
				if (count && seq >= count)
					done_at = now + 1000000; /* a second for the last pongs */
				else {
					f.type = HIFI_LINK_PING;
					f.seq = seq++;
					f.t2 = f.t3 = 0;
					send_frame(&f);
					next += period;
					if (next < now) next = now + period; /* fell behind: don't burst */
				}
				continue;
			}
			wait = (next - now) / 1000 / portTICK_PERIOD_MS;
			if (wait < 1) wait = 1; /* never spin: idle on this core feeds the watchdog */
		}
		if (done_at && now >= done_at) break;
		if (xQueueReceive(q, &it, wait) == pdTRUE) handle(&it);
	}
	bool ended_itself = run;
	teardown();
	if (ended_itself) hifi_lab_esp_finished("espnow", my_run, true);
	ESP_LOGI(TAG, "%s stopped: sent %u, pongs %u, rx %u", ROLE_NAMES[role], (unsigned) tx->sent, (unsigned) tx->pongs,
		 (unsigned) rx->rx);
}

static void task_main(void *arg) {
	for (;;) {
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		one_run(); /* a run stopped before it began still tears ESP-NOW down */
		alive = false;
	}
}

static const struct {
	const char *name;
	wifi_phy_rate_t rate;
} RATES[] = {
	{"1m", WIFI_PHY_RATE_1M_L}, {"2m", WIFI_PHY_RATE_2M_L}, {"5m", WIFI_PHY_RATE_5M_L},
	{"11m", WIFI_PHY_RATE_11M_L}, {"6m", WIFI_PHY_RATE_6M}, {"12m", WIFI_PHY_RATE_12M},
	{"24m", WIFI_PHY_RATE_24M}, {"54m", WIFI_PHY_RATE_54M}, {"mcs0", WIFI_PHY_RATE_MCS0_LGI},
	{"mcs7", WIFI_PHY_RATE_MCS7_LGI},
};

static const char *ps_str(wifi_ps_type_t p) {
	return p == WIFI_PS_NONE ? "none" : p == WIFI_PS_MIN_MODEM ? "min" : p == WIFI_PS_MAX_MODEM ? "max" : "?";
}

static uint32_t num(const char *body, const char *key, uint32_t dflt) {
	double v;
	if (!hj_get_num(body, key, &v) || !(v >= 0)) return dflt;
	return v >= 4294967295.0 ? UINT32_MAX : (uint32_t) v; /* clamped first: the cast of a bigger one is undefined */
}

static int start(uint32_t run_n, const char *body, char *err, size_t n) {
	if (alive) {
		snprintf(err, n, "the last run is still stopping");
		return 409;
	}
	char s[24];
	if (!hj_get_str(body, "role", s, sizeof(s))) s[0] = 0;
	if (!strcmp(s, "echo")) role = ROLE_ECHO;
	else if (!strcmp(s, "ping")) role = ROLE_PING;
	else if (!strcmp(s, "flood")) role = ROLE_FLOOD;
	else {
		snprintf(err, n, "role must be echo, ping or flood");
		return 400;
	}
	if (!hj_get_str(body, "peer", s, sizeof(s)) || !hifi_link_parse_mac(s, peer)) {
		snprintf(err, n, "peer (the other board's MAC or device id) is required");
		return 400;
	}
	/* both ends get the same key, made afresh for the run by whoever starts it */
	uint8_t k[HIFI_LINK_KEY];
	char ks[2 * HIFI_LINK_KEY + 2];
	bool have_key = hj_get_str(body, "key", ks, sizeof(ks)) && hifi_link_parse_key(ks, k);
	memset(ks, 0, sizeof(ks));
	if (!have_key) {
		snprintf(err, n, "key (32 hex digits, the same at both ends) is required");
		return 400;
	}
	rate_hz = num(body, "rate_hz", 20);
	len = num(body, "len", role == ROLE_FLOOD ? HIFI_LINK_MAX : 64);
	count = num(body, "count", 0);
	window = num(body, "window", 4);
	if (rate_hz < 1 || rate_hz > 100 || len < HIFI_LINK_MIN || len > HIFI_LINK_MAX || window < 1 || window > 16) {
		memset(k, 0, sizeof(k));
		snprintf(err, n, "rate_hz 1-100, len %d-%d, window 1-16", HIFI_LINK_MIN, HIFI_LINK_MAX);
		return 400;
	}
	retry = num(body, "retry", 0);
	retry_ms = num(body, "retry_ms", 0);
	sync_us = num(body, "sync_us", 0);
	win_us = num(body, "win_us", 0);
	off_us = num(body, "off_us", 0);
	want_beacon = num(body, "beacon", 0) || sync_us == 1;
	li_want = (uint16_t) num(body, "li", 0);
	if (retry > 8 || retry_ms > 500 || (sync_us > 1 && (sync_us < 10000 || sync_us > 1000000)) ||
	    (sync_us > 1 && (win_us < 10000 || win_us > sync_us || off_us >= sync_us)) || num(body, "li", 0) > 100 ||
	    ((retry || sync_us) && role != ROLE_FLOOD) || (li_want && role != ROLE_ECHO)) {
		memset(k, 0, sizeof(k));
		snprintf(err, n, "retry 0-8, retry_ms 0-500, sync_us 1 or 10000-1000000 with win_us 10000-sync_us and off_us below it (flood); li 1-100 (echo)");
		return 400;
	}
	/* the tick is 10 ms (HZ 100): a shorter window can't be kept */
	if (sync_us == 1 && (win_us < 10000 || win_us > 1000000 || off_us > 1000000)) {
		memset(k, 0, sizeof(k));
		snprintf(err, n, "sync_us dtim needs win_us 10000-1000000 and off_us 0-1000000");
		return 400;
	}
	wifi_phy_rate_t rate = WIFI_PHY_RATE_1M_L;
	strlcpy(rate_name, "1m", sizeof(rate_name));
	if (hj_get_str(body, "rate", s, sizeof(s))) {
		size_t i;
		for (i = 0; i < sizeof(RATES) / sizeof(RATES[0]) && strcmp(RATES[i].name, s); i++)
			;
		if (i == sizeof(RATES) / sizeof(RATES[0])) {
			memset(k, 0, sizeof(k));
			snprintf(err, n, "rate: 1m 2m 5m 11m 6m 12m 24m 54m mcs0 mcs7");
			return 400;
		}
		rate = RATES[i].rate;
		strlcpy(rate_name, s, sizeof(rate_name));
	}
	char ps[8] = "";
	hj_get_str(body, "ps", ps, sizeof(ps));
	if (ps[0] && strcmp(ps, "none") && strcmp(ps, "min") && strcmp(ps, "max")) {
		memset(k, 0, sizeof(k));
		snprintf(err, n, "ps must be none, min or max");
		return 400;
	}

	if (!worker) {
		uint8_t *qmem = heap_caps_malloc(QLEN * sizeof(rx_item_t), MALLOC_CAP_SPIRAM);
		StackType_t *stack = heap_caps_malloc(STACK, MALLOC_CAP_SPIRAM);
		if (!window_sem) window_sem = xSemaphoreCreateCounting(16, 0);
		if (!tx) tx = heap_caps_calloc(1, sizeof(*tx), MALLOC_CAP_SPIRAM);
		if (!rx) rx = heap_caps_calloc(1, sizeof(*rx), MALLOC_CAP_SPIRAM);
		if (!rx_it) rx_it = heap_caps_calloc(1, sizeof(*rx_it), MALLOC_CAP_SPIRAM);
		if (!key) key = heap_caps_calloc(1, sizeof(*key), MALLOC_CAP_SPIRAM);
		if (!rt) rt = heap_caps_calloc(1, sizeof(*rt), MALLOC_CAP_SPIRAM);
		if (!qmem || !stack || !window_sem || !tx || !rx || !rx_it || !rt || !key) {
			memset(k, 0, sizeof(k));
			free(qmem);
			free(stack);
			snprintf(err, n, "out of memory");
			return 503;
		}
		if (!q) q = xQueueCreateStatic(QLEN, sizeof(rx_item_t), qmem, &q_ctl);
		else free(qmem); /* made on an earlier try whose task failed */
		worker = xTaskCreateStaticPinnedToCore(task_main, "lab_espnow", STACK, NULL, ESP_TASK_PRIO_MIN + 5, stack,
						       &worker_tcb, 0);
		if (!worker) { /* static creation can't fail on memory; if it does, the stack goes back */
			free(stack);
			memset(k, 0, sizeof(k));
			snprintf(err, n, "no worker task");
			return 503;
		}
	}
	heap_start = heap_now();
	xQueueReset(q);
	while (xSemaphoreTake(window_sem, 0) == pdTRUE)
		;
	for (uint32_t i = 0; i < window; i++) xSemaphoreGive(window_sem);
	hifi_link_tx_init(tx, esp_random());
	hifi_link_rx_init(rx);
	q_full = other = report_timeouts = no_mem = 0;
	retries = recovered = gave_up = rq_full = outside_win = 0;
	memset(rt, 0, sizeof(*rt));
	ps_err[0] = 0;
	beacon_seen = false;
	dtim_period = 0;
	if (want_beacon) read_beacon();
	if (sync_us == 1) {
		if (!beacon_seen) {
			memset(k, 0, sizeof(k));
			snprintf(err, n, "sync_us dtim: no beacon of the access point in 1 s (%u seen)", (unsigned) beacons);
			return 503;
		}
		int64_t per = (int64_t) bi_tu * 1024 * (dtim_period ? dtim_period : 1);
		if (per < 10000 || per > 1000000) {
			memset(k, 0, sizeof(k));
			snprintf(err, n, "sync_us dtim: the DTIM period %lld us is outside 10000-1000000", (long long) per);
			return 503;
		}
		off_us = (uint32_t) (((int64_t) dtim_phase_us + off_us) % per); /* off_us: after the DTIM TBTT */
		sync_us = (uint32_t) per;
		if (win_us > per) win_us = per;
	}
#if CONFIG_BT_ENABLED
	/* With the Bluetooth controller on, the coexistence code in the Wi-Fi
	 * blob aborts a few seconds after modem sleep is turned off ("Should
	 * enable WiFi modem sleep when both WiFi and Bluetooth are enabled"):
	 * lab3 on Lite 2 panicked on every ps:none start. The bt experiment
	 * (mode off) disables the controller, and then ps none is allowed. */
	if (!strcmp(ps, "none") && esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED) {
		memset(k, 0, sizeof(k));
		snprintf(err, n, "ps none panics the Wi-Fi/Bluetooth coexistence while the controller is on; use min or max, or the bt experiment's mode off first");
		return 400;
	}
#endif

	wifi_second_chan_t second;
	if (esp_wifi_get_channel(&channel, &second) != ESP_OK) channel = 0;
	esp_wifi_get_mac(WIFI_IF_STA, self);
	esp_err_t e = esp_now_init();
	if (e != ESP_OK) {
		memset(k, 0, sizeof(k));
		snprintf(err, n, "esp_now_init: %s", esp_err_to_name(e));
		return 503;
	}
	heap_after_init = heap_min = heap_now();
	esp_now_peer_info_t p = {0};
	memcpy(p.peer_addr, peer, 6);
	p.channel = 0; /* the channel the station is on */
	p.ifidx = WIFI_IF_STA;
	p.encrypt = false;
	if ((e = esp_now_add_peer(&p)) != ESP_OK || (e = esp_now_register_recv_cb(recv_cb)) != ESP_OK ||
	    (e = esp_now_register_send_cb(send_cb)) != ESP_OK) {
		esp_now_deinit();
		memset(k, 0, sizeof(k));
		snprintf(err, n, "esp_now: %s", esp_err_to_name(e));
		return 503;
	}
	if (rate != WIFI_PHY_RATE_1M_L) {
		e = esp_wifi_config_espnow_rate(WIFI_IF_STA, rate);
		if (e != ESP_OK) {
			esp_now_deinit();
			memset(k, 0, sizeof(k));
			snprintf(err, n, "rate %s: %s", rate_name, esp_err_to_name(e));
			return 400;
		}
		rate_changed = true;
	}
	if (esp_wifi_get_ps(&ps_before) != ESP_OK) ps_before = WIFI_PS_MIN_MODEM;
	ps_set = ps_before;
	if (ps[0]) {
		wifi_ps_type_t want = !strcmp(ps, "none") ? WIFI_PS_NONE : !strcmp(ps, "min") ? WIFI_PS_MIN_MODEM : WIFI_PS_MAX_MODEM;
		e = esp_wifi_set_ps(want);
		if (e != ESP_OK)
			strlcpy(ps_err, esp_err_to_name(e), sizeof(ps_err)); /* carry on with the one it had */
		else {
			ps_set = want;
			ps_changed = want != ps_before;
		}
	}
	ps_run = ps_str(ps_set);
	li_changed = false;
	if (li_want) {
		wifi_config_t c;
		if (esp_wifi_get_config(WIFI_IF_STA, &c) == ESP_OK) {
			li_before = c.sta.listen_interval;
			c.sta.listen_interval = li_want;
			esp_wifi_set_storage(WIFI_STORAGE_RAM); /* a lab setting: not into NVS, a reboot undoes it */
			if (esp_wifi_set_config(WIFI_IF_STA, &c) == ESP_OK) li_changed = li_want != li_before;
			esp_wifi_set_storage(WIFI_STORAGE_FLASH);
			li_now = esp_wifi_get_config(WIFI_IF_STA, &c) == ESP_OK ? c.sta.listen_interval : 0;
		}
	}

	hifi_link_key_init(key, k); /* the worker is idle: no frame is being tagged */
	memset(k, 0, sizeof(k));
	run_no = run_n;
	run = alive = true;
	xTaskNotifyGive(worker);
	ESP_LOGI(TAG, "%s with %02x:%02x:%02x:%02x:%02x:%02x on channel %u", ROLE_NAMES[role], peer[0], peer[1], peer[2],
		 peer[3], peer[4], peer[5], channel);
	return 0;
}

/* false: the worker is still winding down after 3 s (a start is refused
 * until it is down) */
static bool stop(void) {
	run = false;
	for (int i = 0; i < 300 && alive; i++) vTaskDelay(pdMS_TO_TICKS(10));
	if (alive) ESP_LOGE(TAG, "the task didn't stop in 3 s");
	return !alive;
}

static void result(void *hj) {
	hj_t *j = hj;
	if (!tx) return;
	char m[18];
	snprintf(m, sizeof(m), "%02x:%02x:%02x:%02x:%02x:%02x", peer[0], peer[1], peer[2], peer[3], peer[4], peer[5]);
	hj_str(j, "role", ROLE_NAMES[role]);
	hj_str(j, "peer", m);
	snprintf(m, sizeof(m), "%02x:%02x:%02x:%02x:%02x:%02x", self[0], self[1], self[2], self[3], self[4], self[5]);
	hj_str(j, "self", m);
	hj_int(j, "channel", channel);
	hj_str(j, "ps", ps_run); /* as set at the start; the board's own may differ now */
	if (ps_err[0]) hj_str(j, "ps_error", ps_err);
	hj_str(j, "rate", rate_name);
	hj_int(j, "len", len);
	if (role == ROLE_PING) hj_int(j, "rate_hz", rate_hz);
	if (role == ROLE_FLOOD) hj_int(j, "window", window);
	hj_int(j, "queue_full", q_full);
	hj_int(j, "other", other);
	hj_int(j, "no_mem", no_mem);
	hj_obj(j, "heap"); /* internal RAM: before ESP-NOW, after its init, the least during the run */
	hj_int(j, "start", heap_start);
	hj_int(j, "after_init", heap_after_init);
	hj_int(j, "min", heap_min);
	hj_obj_end(j);
	if (role == ROLE_FLOOD) {
		hj_int(j, "report_timeouts", report_timeouts);
		if (retry) {
			hj_obj(j, "retry");
			hj_int(j, "max", retry);
			hj_int(j, "ms", retry_ms);
			hj_int(j, "retries", retries);
			hj_int(j, "recovered", recovered);
			hj_int(j, "gave_up", gave_up);
			hj_int(j, "queue_full", rq_full);
			hj_int(j, "waiting", rt->rn); /* still queued at the end: lost too */
			hj_obj_end(j);
		}
		if (sync_us) {
			hj_obj(j, "sync");
			hj_int(j, "period_us", sync_us);
			hj_int(j, "win_us", win_us);
			hj_int(j, "off_us", off_us);
			hj_int(j, "waits", outside_win);
			hj_obj_end(j);
		}
	}
	if (beacon_seen) {
		hj_obj(j, "beacon");
		hj_int(j, "interval_tu", bi_tu);
		hj_int(j, "dtim_period", dtim_period);
		hj_int(j, "dtim_phase_us", dtim_phase_us);
		hj_obj_end(j);
	} else if (want_beacon)
		hj_int(j, "beacon_none_of", beacons);
	if (li_want) {
		hj_obj(j, "listen_interval");
		hj_int(j, "asked", li_want);
		hj_int(j, "before", li_before);
		hj_int(j, "read_back", li_now);
		hj_obj_end(j);
	}
	hj_obj(j, "tx");
	hifi_link_tx_json(tx, j);
	hj_obj_end(j);
	if (role == ROLE_ECHO) {
		hj_obj(j, "rx");
		hifi_link_rx_json(rx, j);
		hj_obj_end(j);
	}
}

const hifi_lab_exp_t lab_espnow = {
	.name = "espnow",
	.help = "ESP-NOW link beside Wi-Fi: role echo|ping|flood, peer, key (32 hex, both ends), rate_hz, len, count, rate, ps, window, retry, retry_ms, sync_us, win_us, off_us, beacon, li",
	.start = start,
	.stop = stop,
	.result = result,
};
