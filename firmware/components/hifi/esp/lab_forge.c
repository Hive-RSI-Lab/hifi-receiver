/*
 * Lab experiment "forge": a stranger in radio range. Run it on a third board
 * while two others run espnow; it listens for the victim's ESP-NOW frames,
 * moves each one's sequence number on by jump, and sends it again under the
 * victim's MAC. A frame tag the forger can't make (it doesn't know the run's
 * key) means the far end counts these as bad and its loss stays as it was.
 *
 *   api lab "{\"op\":\"start\",\"exp\":\"forge\",\"victim\":\"3076f52d742c\",\"max_s\":60}"
 *   api lab "{\"op\":\"result\",\"exp\":\"forge\"}"
 *
 * Optional: jump (default 1000), rate_hz (forged frames a second at most,
 * 1-100, default 20), key (32 hex digits: the control, tagging with the run's
 * own key, which must then move the far end's loss; without it the forger
 * makes up a key of its own).
 *
 * ESP-NOW frames are 802.11 action frames: the 24-byte header, category 127
 * and Espressif's OUI, 4 random bytes, then a vendor element (221, length,
 * OUI, type 4, version) holding the payload. The forger keeps all of that and
 * replaces only the payload. The receive side is the promiscuous callback
 * (management frames only, on the station's channel); the station stays
 * connected throughout. Promiscuous mode goes off when the run stops.
 */
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_task.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "hifi_json.h"
#include "hifi_lab_esp.h"
#include "hifi_link.h"

static const char *TAG = "lab_forge";

#define HDR 24      /* the 802.11 management header */
#define BODY 39     /* the ESP-NOW payload's offset in the frame */
#define FCS 4       /* the frame check sequence the promiscuous length counts */
#define FRAME_MAX (BODY + HIFI_LINK_MAX)

typedef struct {
	uint16_t n; /* bytes of frame, without the FCS */
	uint8_t f[FRAME_MAX];
} item_t;

#define QLEN 4
#define STACK 4096

/* made once and kept, in PSRAM (the Lite has little internal RAM to spare):
 * a callback still running as promiscuous mode goes off never touches freed
 * memory */
static QueueHandle_t q;
static StaticQueue_t q_ctl;
static TaskHandle_t worker;
static StaticTask_t worker_tcb;
static item_t *cb_it;        /* the callback's item: on the Wi-Fi task only */
static hifi_link_key_t *key; /* never logged or reported */

static volatile bool run, alive;
static uint8_t victim[6];
static uint32_t jump, rate_hz, run_no;
static bool control;
static uint8_t channel;
static volatile uint32_t heard, q_full, other;
static uint32_t sent, send_fail, not_ours, skipped;
static char last_err[24];

static const uint8_t OUI[3] = {0x18, 0xfe, 0x34};

/* an ESP-NOW frame from the victim: its length without the FCS, 0 if not */
static size_t espnow_from_victim(const uint8_t *p, size_t n) {
	if (n < BODY + HIFI_LINK_MIN || n > FRAME_MAX) return 0;
	if (p[0] != 0xd0 || memcmp(p + 10, victim, 6)) return 0; /* action, from the victim */
	if (p[HDR] != 127 || memcmp(p + HDR + 1, OUI, 3)) return 0;
	if (p[32] != 221 || memcmp(p + 34, OUI, 3) || p[37] != 4) return 0;
	size_t body = p[33] >= 5 ? (size_t) p[33] - 5 : 0;
	if (body < HIFI_LINK_MIN || BODY + body > n) return 0;
	return BODY + body;
}

static void sniff_cb(void *buf, wifi_promiscuous_pkt_type_t type) {
	if (!run || type != WIFI_PKT_MGMT) return;
	const wifi_promiscuous_pkt_t *pk = buf;
	size_t n = pk->rx_ctrl.sig_len;
	if (n <= FCS) return;
	n = espnow_from_victim(pk->payload, n - FCS);
	if (!n) {
		other++;
		return;
	}
	heard++;
	cb_it->n = (uint16_t) n;
	memcpy(cb_it->f, pk->payload, n);
	if (xQueueSend(q, cb_it, 0) != pdTRUE) q_full++;
}

static void teardown(void) {
	run = false;
	esp_wifi_set_promiscuous(false);
	esp_wifi_set_promiscuous_rx_cb(NULL);
	if (key) memset(key, 0, sizeof(*key));
}

static void one_run(void) {
	uint32_t my_run = run_no;
	static item_t it; /* the worker's: one task, one run at a time */
	static uint8_t out[FRAME_MAX];
	int64_t gap = 1000000 / rate_hz, next = 0;
	while (run) {
		if (xQueueReceive(q, &it, pdMS_TO_TICKS(100)) != pdTRUE) continue;
		int64_t now = esp_timer_get_time();
		if (now < next) {
			skipped++;
			continue;
		}
		size_t body = it.n - BODY;
		size_t m = hifi_link_forge(key, it.f + BODY, body, jump, out + BODY, HIFI_LINK_MAX);
		if (!m) {
			not_ours++;
			continue;
		}
		memcpy(out, it.f, BODY); /* the header, with the victim as sender, and the wrapper */
		memset(out + 2, 0, 2);   /* duration: the driver's */
		esp_err_t e = esp_wifi_80211_tx(WIFI_IF_STA, out, (int) (BODY + m), true);
		if (e == ESP_OK)
			sent++;
		else {
			send_fail++;
			strlcpy(last_err, esp_err_to_name(e), sizeof(last_err));
		}
		next = now + gap;
	}
	teardown();
	ESP_LOGI(TAG, "stopped: heard %u, sent %u, failed %u", (unsigned) heard, (unsigned) sent, (unsigned) send_fail);
	(void) my_run; /* runs only end by stop or deadline */
}

static void task_main(void *arg) {
	for (;;) {
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		one_run();
		alive = false;
	}
}

static uint32_t num(const char *body, const char *k, uint32_t dflt) {
	double v;
	if (!hj_get_num(body, k, &v) || !(v >= 0)) return dflt;
	return v >= 4294967295.0 ? UINT32_MAX : (uint32_t) v;
}

static int start(uint32_t run_n, const char *body, char *err, size_t n) {
	if (alive) {
		snprintf(err, n, "the last run is still stopping");
		return 409;
	}
	char s[2 * HIFI_LINK_KEY + 2];
	if (!hj_get_str(body, "victim", s, sizeof(s)) || !hifi_link_parse_mac(s, victim)) {
		snprintf(err, n, "victim (the sending board's MAC or device id) is required");
		return 400;
	}
	jump = num(body, "jump", 1000);
	rate_hz = num(body, "rate_hz", 20);
	if (jump < 1 || rate_hz < 1 || rate_hz > 100) {
		snprintf(err, n, "jump at least 1, rate_hz 1-100");
		return 400;
	}
	uint8_t k[HIFI_LINK_KEY];
	control = hj_get_str(body, "key", s, sizeof(s));
	if (control && !hifi_link_parse_key(s, k)) {
		memset(s, 0, sizeof(s));
		snprintf(err, n, "key: 32 hex digits");
		return 400;
	}
	memset(s, 0, sizeof(s));
	if (!control) esp_fill_random(k, sizeof(k)); /* a stranger's key */

	if (!worker) {
		uint8_t *qmem = heap_caps_malloc(QLEN * sizeof(item_t), MALLOC_CAP_SPIRAM);
		StackType_t *stack = heap_caps_malloc(STACK, MALLOC_CAP_SPIRAM);
		if (!cb_it) cb_it = heap_caps_calloc(1, sizeof(*cb_it), MALLOC_CAP_SPIRAM);
		if (!key) key = heap_caps_calloc(1, sizeof(*key), MALLOC_CAP_SPIRAM);
		if (!qmem || !stack || !cb_it || !key) {
			memset(k, 0, sizeof(k));
			free(qmem);
			free(stack);
			snprintf(err, n, "out of memory");
			return 503;
		}
		if (!q) q = xQueueCreateStatic(QLEN, sizeof(item_t), qmem, &q_ctl);
		else free(qmem);
		worker = xTaskCreateStaticPinnedToCore(task_main, "lab_forge", STACK, NULL, ESP_TASK_PRIO_MIN + 5, stack,
						       &worker_tcb, 0);
		if (!worker) {
			free(stack);
			memset(k, 0, sizeof(k));
			snprintf(err, n, "no worker task");
			return 503;
		}
	}
	xQueueReset(q);
	heard = q_full = other = 0;
	sent = send_fail = not_ours = skipped = 0;
	last_err[0] = 0;
	wifi_second_chan_t second;
	if (esp_wifi_get_channel(&channel, &second) != ESP_OK) channel = 0;
	hifi_link_key_init(key, k); /* the worker is idle */
	memset(k, 0, sizeof(k));

	wifi_promiscuous_filter_t flt = {.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT};
	esp_err_t e;
	run = true; /* before the callback goes on, so it doesn't drop the first frames */
	if ((e = esp_wifi_set_promiscuous_filter(&flt)) != ESP_OK || (e = esp_wifi_set_promiscuous_rx_cb(sniff_cb)) != ESP_OK ||
	    (e = esp_wifi_set_promiscuous(true)) != ESP_OK) {
		teardown();
		snprintf(err, n, "promiscuous: %s", esp_err_to_name(e));
		return 503;
	}
	run_no = run_n;
	alive = true;
	xTaskNotifyGive(worker);
	ESP_LOGI(TAG, "forging as %02x:%02x:%02x:%02x:%02x:%02x on channel %u%s", victim[0], victim[1], victim[2],
		 victim[3], victim[4], victim[5], channel, control ? " (control: the run's key)" : "");
	return 0;
}

static bool stop(void) {
	run = false;
	for (int i = 0; i < 300 && alive; i++) vTaskDelay(pdMS_TO_TICKS(10));
	if (alive) ESP_LOGE(TAG, "the task didn't stop in 3 s");
	return !alive;
}

static void result(void *hj) {
	hj_t *j = hj;
	char m[18];
	snprintf(m, sizeof(m), "%02x:%02x:%02x:%02x:%02x:%02x", victim[0], victim[1], victim[2], victim[3], victim[4],
		 victim[5]);
	hj_str(j, "victim", m);
	hj_bool(j, "control", control); /* tagged with the run's key, not a stranger's */
	hj_int(j, "jump", jump);
	hj_int(j, "rate_hz", rate_hz);
	hj_int(j, "channel", channel);
	hj_int(j, "heard", heard);         /* the victim's ESP-NOW frames */
	hj_int(j, "sent", sent);           /* forged frames on the air */
	hj_int(j, "send_fail", send_fail); /* esp_wifi_80211_tx refused */
	if (last_err[0]) hj_str(j, "last_error", last_err);
	hj_int(j, "skipped", skipped);   /* heard within the rate cap */
	hj_int(j, "not_ours", not_ours); /* not a lab frame */
	hj_int(j, "queue_full", q_full);
	hj_int(j, "other", other); /* other management frames */
}

const hifi_lab_exp_t lab_forge = {
	.name = "forge",
	.help = "a stranger's forged ESP-NOW frames: victim (MAC), jump, rate_hz, key (the control only)",
	.start = start,
	.stop = stop,
	.result = result,
};
