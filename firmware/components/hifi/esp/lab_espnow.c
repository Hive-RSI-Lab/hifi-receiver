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
 * "mcs0", "mcs7"), ps (Wi-Fi power save for the run: "none", "min", "max";
 * default as it is), window (flood, 1-16, default 4; a send report missing
 * for 1 s counts as lost and its slot is used again: report_timeouts).
 *
 * Only frames from the peer are looked at; nothing is forwarded anywhere.
 * The stop (asked, deadline, low RAM or the run's own end) takes ESP-NOW
 * down again and puts the rate back, and the power save too unless someone
 * else changed it during the run. Starts and stops come one at a time, under
 * the lab's lock: `alive` is checked, then set, with nothing in between.
 */
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
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

/* on the Wi-Fi task only: its item is static (~270 bytes off that task's stack) */
static void recv_cb(const uint8_t *mac, const uint8_t *data, int n) {
	int64_t t = esp_timer_get_time();
	if (!run || n <= 0 || n > HIFI_LINK_MAX) return;
	if (memcmp(mac, peer, 6)) {
		other++;
		return;
	}
	static rx_item_t it;
	memcpy(it.mac, mac, 6);
	it.len = (uint8_t) n;
	it.t_us = t;
	memcpy(it.data, data, n);
	if (xQueueSend(q, &it, 0) != pdTRUE) q_full++;
}

static void send_cb(const uint8_t *mac, esp_now_send_status_t st) {
	if (!run) return;
	hifi_link_tx_acked(tx, st == ESP_NOW_SEND_SUCCESS);
	if (role == ROLE_FLOOD) xSemaphoreGive(window_sem);
}

static bool send_frame(hifi_link_frame_t *f) {
	uint8_t buf[HIFI_LINK_MAX];
	size_t n = hifi_link_encode(f, buf, sizeof(buf));
	if (!n) return false;
	int64_t t = esp_timer_get_time();
	if (f->type == HIFI_LINK_PING) {
		f->t1 = t;
		n = hifi_link_encode(f, buf, sizeof(buf));
	} else if (f->type == HIFI_LINK_PONG) {
		f->t3 = t;
		n = hifi_link_encode(f, buf, sizeof(buf));
	}
	esp_err_t e = esp_now_send(peer, buf, n);
	if (e == ESP_ERR_ESPNOW_NO_MEM) no_mem++;
	hifi_link_tx_sent(tx, n, e == ESP_OK, t);
	return e == ESP_OK;
}

static void handle(const rx_item_t *it) {
	hifi_link_frame_t f;
	if (!hifi_link_decode(it->data, it->len, &f)) {
		hifi_link_rx_bad(rx);
		return;
	}
	if (role == ROLE_ECHO && f.type == HIFI_LINK_PING) {
		hifi_link_frame_t pong;
		hifi_link_pong_for(&f, it->t_us, 0, &pong);
		rx->pings++;
		send_frame(&pong);
	} else if (role == ROLE_ECHO && f.type == HIFI_LINK_DATA)
		hifi_link_rx_data(rx, &f, it->t_us);
	else if (role == ROLE_PING && f.type == HIFI_LINK_PONG)
		hifi_link_tx_pong(tx, &f, it->t_us);
	else
		other++;
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
	rate_changed = ps_changed = false;
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
			if (count && seq >= count) {
				/* let the last send reports in */
				if (!done_at) done_at = now + 500000;
				if (now >= done_at) break;
				vTaskDelay(pdMS_TO_TICKS(20));
				continue;
			}
			/* a send report missing for 1 s: count it lost, use its slot */
			if (xSemaphoreTake(window_sem, pdMS_TO_TICKS(1000)) != pdTRUE) report_timeouts++;
			f.type = HIFI_LINK_DATA;
			f.seq = seq;
			if (send_frame(&f))
				seq++;
			else {
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
	rate_hz = num(body, "rate_hz", 20);
	len = num(body, "len", role == ROLE_FLOOD ? HIFI_LINK_MAX : 64);
	count = num(body, "count", 0);
	window = num(body, "window", 4);
	if (rate_hz < 1 || rate_hz > 100 || len < HIFI_LINK_HDR || len > HIFI_LINK_MAX || window < 1 || window > 16) {
		snprintf(err, n, "rate_hz 1-100, len %d-%d, window 1-16", HIFI_LINK_HDR, HIFI_LINK_MAX);
		return 400;
	}
	wifi_phy_rate_t rate = WIFI_PHY_RATE_1M_L;
	strlcpy(rate_name, "1m", sizeof(rate_name));
	if (hj_get_str(body, "rate", s, sizeof(s))) {
		size_t i;
		for (i = 0; i < sizeof(RATES) / sizeof(RATES[0]) && strcmp(RATES[i].name, s); i++)
			;
		if (i == sizeof(RATES) / sizeof(RATES[0])) {
			snprintf(err, n, "rate: 1m 2m 5m 11m 6m 12m 24m 54m mcs0 mcs7");
			return 400;
		}
		rate = RATES[i].rate;
		strlcpy(rate_name, s, sizeof(rate_name));
	}
	char ps[8] = "";
	hj_get_str(body, "ps", ps, sizeof(ps));
	if (ps[0] && strcmp(ps, "none") && strcmp(ps, "min") && strcmp(ps, "max")) {
		snprintf(err, n, "ps must be none, min or max");
		return 400;
	}

	if (!worker) {
		uint8_t *qmem = heap_caps_malloc(QLEN * sizeof(rx_item_t), MALLOC_CAP_SPIRAM);
		StackType_t *stack = heap_caps_malloc(STACK, MALLOC_CAP_SPIRAM);
		if (!window_sem) window_sem = xSemaphoreCreateCounting(16, 0);
		if (!tx) tx = heap_caps_calloc(1, sizeof(*tx), MALLOC_CAP_SPIRAM);
		if (!rx) rx = heap_caps_calloc(1, sizeof(*rx), MALLOC_CAP_SPIRAM);
		if (!qmem || !stack || !window_sem || !tx || !rx) {
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
	ps_err[0] = 0;
#if CONFIG_BT_ENABLED
	/* With Bluetooth in the image, the coexistence code in the Wi-Fi blob
	 * aborts a few seconds after modem sleep is turned off ("Should enable
	 * WiFi modem sleep when both WiFi and Bluetooth are enabled"): lab3 on
	 * Lite 2 panicked on every ps:none start. */
	if (!strcmp(ps, "none")) {
		snprintf(err, n, "ps none panics the Wi-Fi/Bluetooth coexistence (modem sleep is required with Bluetooth on); use min or max");
		return 400;
	}
#endif

	wifi_second_chan_t second;
	if (esp_wifi_get_channel(&channel, &second) != ESP_OK) channel = 0;
	esp_wifi_get_mac(WIFI_IF_STA, self);
	esp_err_t e = esp_now_init();
	if (e != ESP_OK) {
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
		snprintf(err, n, "esp_now: %s", esp_err_to_name(e));
		return 503;
	}
	if (rate != WIFI_PHY_RATE_1M_L) {
		e = esp_wifi_config_espnow_rate(WIFI_IF_STA, rate);
		if (e != ESP_OK) {
			esp_now_deinit();
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
	if (role == ROLE_FLOOD) hj_int(j, "report_timeouts", report_timeouts);
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
	.help = "ESP-NOW link beside Wi-Fi: role echo|ping|flood, peer, rate_hz, len, count, rate, ps, window",
	.start = start,
	.stop = stop,
	.result = result,
};
