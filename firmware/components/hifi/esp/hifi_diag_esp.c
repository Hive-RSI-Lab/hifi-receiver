/*
 * The board's side of `api diag` (core/hifi_diag.h, docs/test-api.md
 * "Diagnostics"): reads heap, tasks, Wi-Fi, NVS and the output's counters
 * from ESP-IDF and squeezelite, keeps the boot counter, the reset history and
 * the last update's timing in NVS (namespace hifi_diag), and times a burst
 * up the bench link.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/task_snapshot.h"
#include "soc/soc_memory_types.h"
#include "nvs.h"

#include "hifi_crash.h"
#include "hifi_esp.h"
#include "hifi_ws.h"

static const char TAG[] = "hifi_diag";
#define NS "hifi_diag"

static uint32_t boot_app_ms, boot_ip_ms, boots, power_cuts;
static char hist[HIFI_DIAG_HIST_CAP];
static volatile uint32_t connects, disconnects;
static volatile int last_reason;
static int64_t ip_since_us; /* 64-bit: under ip_mux */
static portMUX_TYPE ip_mux = portMUX_INITIALIZER_UNLOCKED;
static bool events_on;

static SemaphoreHandle_t task_lock;
static hifi_diag_tasks_t *last; /* the last CPU sample, PSRAM */

static uint32_t now_ms(void) { return (uint32_t) (esp_timer_get_time() / 1000); }

/* ------------------------------------------------------------------ boot */

void hifi_diag_init(void) {
	boot_app_ms = now_ms();
	task_lock = xSemaphoreCreateMutex();
	if (hifi_crash_restart_cause()) ESP_LOGW(TAG, "last restart: %s", hifi_crash_restart_cause());
	nvs_handle_t h;
	if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
		ESP_LOGW(TAG, "no NVS: no boot counter or reset history");
		hifi_diag_hist_push(hist, sizeof(hist), hifi_crash_reset_reason());
		return;
	}
	size_t n = sizeof(hist);
	if (nvs_get_str(h, "resets", hist, &n) != ESP_OK) hist[0] = '\0';
	nvs_get_u32(h, "boots", &boots);
	nvs_get_u32(h, "pwr_cuts", &power_cuts);
	if (hifi_diag_is_power_cut(hifi_crash_reset_reason(), boots)) {
		power_cuts++;
		nvs_set_u32(h, "pwr_cuts", power_cuts);
		ESP_LOGW(TAG, "restarted by a power cut (%s); %u so far", hifi_crash_reset_reason(), (unsigned) power_cuts);
	}
	boots++;
	hifi_diag_hist_push(hist, sizeof(hist), hifi_crash_reset_reason());
	nvs_set_u32(h, "boots", boots);
	nvs_set_str(h, "resets", hist);
	nvs_commit(h);
	nvs_close(h);
	ESP_LOGI(TAG, "boot %u; resets %s", (unsigned) boots, hist);
}

static void on_net(void *arg, esp_event_base_t base, int32_t id, void *data) {
	if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
		connects++;
	} else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
		disconnects++;
		last_reason = ((wifi_event_sta_disconnected_t *) data)->reason;
		portENTER_CRITICAL(&ip_mux);
		ip_since_us = 0;
		portEXIT_CRITICAL(&ip_mux);
	} else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
		int64_t t = esp_timer_get_time();
		portENTER_CRITICAL(&ip_mux);
		ip_since_us = t;
		portEXIT_CRITICAL(&ip_mux);
		if (!boot_ip_ms) boot_ip_ms = (uint32_t) (t / 1000);
	}
}

uint32_t hifi_diag_wifi_disconnects(void) { return disconnects; }

/* Registers on_net with the default event loop, which network_manager
 * creates (with ESP_ERROR_CHECK, so it can't be made earlier here). Safe to
 * run twice at once: IDF replaces a handler registered again rather than
 * adding it twice. */
static void events_register(void) {
	static volatile bool wifi_on, ip_on;
	if (!wifi_on) wifi_on = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_net, NULL) == ESP_OK;
	if (!ip_on) ip_on = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_net, NULL) == ESP_OK;
	events_on = wifi_on && ip_on;
}

/* The hook in patches/0010: network_manager calls it right after it creates
 * the default event loop, before Wi-Fi or Ethernet start, so the first
 * connect and the first address reach on_net. */
void hifi_net_loop_ready(void) {
	events_register();
	if (!events_on) ESP_LOGW(TAG, "Wi-Fi events not registered at loop start; retrying from the tick");
}

void hifi_diag_tick(void) {
	/* a fallback: a tree without patch 0010, or a register that failed */
	events_register();
	/* an association and an address that came before the handlers did (only
	 * by the fallback): once (later losses and gains reach on_net). The
	 * connect it missed counts; boot_ip_ms stays 0 (unknown). */
	static bool caught_up;
	wifi_ap_record_t ap;
	if (events_on && !caught_up && hifi_net_ip(NULL, 0) && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
		caught_up = true;
		if (!connects) connects = 1;
		int64_t t = esp_timer_get_time();
		portENTER_CRITICAL(&ip_mux);
		if (!ip_since_us) ip_since_us = t;
		portEXIT_CRITICAL(&ip_mux);
	}
}

/* ------------------------------------------------------------------ sys */

static void heap(hifi_diag_heap_t *o, uint32_t caps) {
	multi_heap_info_t i;
	heap_caps_get_info(&i, caps);
	o->free = i.total_free_bytes;
	o->min = i.minimum_free_bytes;
	o->largest = i.largest_free_block;
	o->total = i.total_free_bytes + i.total_allocated_bytes;
}

static void nvs(hifi_diag_nvs_t *o, const char *part, const char *name) {
	nvs_stats_t s;
	if (nvs_get_stats(part, &s) != ESP_OK) return;
	o->name = name;
	o->used = s.used_entries;
	o->free = s.free_entries;
	o->total = s.total_entries;
	o->namespaces = s.namespace_count;
}

static void sys(hifi_diag_sys_t *o) {
	o->uptime_s = (uint32_t) (esp_timer_get_time() / 1000000);
	o->boot_app_ms = boot_app_ms;
	o->boot_ip_ms = boot_ip_ms;
	hifi_diag_net_t n = {0};
	hifi_remote_link(&n, &o->boot_link_ms);
	/* byte-addressable internal RAM, as `state` and the self-test count it
	 * (MALLOC_CAP_INTERNAL alone adds IRAM that takes only 32-bit access) */
	heap(&o->internal, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
	heap(&o->psram, MALLOC_CAP_SPIRAM);
	o->reset_reason = hifi_crash_reset_reason();
	o->restart_cause = hifi_crash_restart_cause();
	memcpy(o->reset_hist, hist, sizeof(hist));
	o->boots = boots;
	o->power_cuts = power_cuts;
	nvs(&o->nvs[0], NULL, "nvs");
	nvs(&o->nvs[1], "settings", "settings");
}

/* ------------------------------------------------------------------ tasks */

/* One sample into out (unsorted, no CPU figures); the run-time total in *total. */
static bool sample(hifi_diag_tasks_t *out, TaskStatus_t *st, uint32_t *total) {
	UBaseType_t n = uxTaskGetSystemState(st, HIFI_DIAG_TASKS_MAX, total);
	if (!n) {
		out->n = -(int) uxTaskGetNumberOfTasks(); /* more than fit */
		return false;
	}
	out->n = (int) n;
	for (UBaseType_t i = 0; i < n; i++) {
		hifi_diag_task_t *t = &out->t[i];
		strlcpy(t->name, st[i].pcTaskName, sizeof(t->name));
		t->num = st[i].xTaskNumber;
		t->runtime = st[i].ulRunTimeCounter;
		t->stack_free = st[i].usStackHighWaterMark; /* bytes on ESP-IDF */
		/* the stack's size and where it is, to see what uses internal RAM. The
		 * snapshot only reads the TCB (it checks the handle points at internal
		 * RAM); a task gone since gives a size that fails the range check. */
		const uint8_t *base = (const uint8_t *) st[i].pxStackBase;
		TaskSnapshot_t snap;
		t->stack_size = 0;
		if (vTaskGetSnapshot(st[i].xHandle, &snap) == pdTRUE) {
			const uint8_t *end = (const uint8_t *) snap.pxEndOfStack;
			if (base && end > base && end - base < 256 * 1024) t->stack_size = (uint32_t) (end - base) + sizeof(StackType_t);
		}
		t->stack_ram = !base ? 0 : esp_ptr_external_ram(base) ? 'p' : esp_ptr_internal(base) ? 'i' : 0;
#ifdef CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID
		/* from the snapshot: the handle may belong to a task gone since */
		t->core = st[i].xCoreID == tskNO_AFFINITY ? -1 : (int) st[i].xCoreID;
#else
		t->core = -1;
#endif
		t->prio = (int) st[i].uxCurrentPriority;
		t->cpu_pct = NAN;
	}
	return true;
}

static int tasks(hifi_diag_tasks_t *out, bool fresh) {
	if (!task_lock) return -1;
	xSemaphoreTake(task_lock, portMAX_DELAY);
	int r = 0;
	if (!last) last = heap_caps_calloc(1, sizeof(*last), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if (last && (fresh || !last->n)) {
		TaskStatus_t *st = heap_caps_malloc(sizeof(TaskStatus_t) * HIFI_DIAG_TASKS_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
		hifi_diag_task_t *a = heap_caps_malloc(sizeof(hifi_diag_task_t) * HIFI_DIAG_TASKS_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
		hifi_diag_tasks_t *b = heap_caps_calloc(1, sizeof(*b), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
		uint32_t t0 = 0, t1 = 0;
		if (st && a && b && sample(b, st, &t0)) {
			int na = b->n;
			memcpy(a, b->t, sizeof(a[0]) * (size_t) na);
			vTaskDelay(pdMS_TO_TICKS(1000));
			if (sample(b, st, &t1)) {
				/* the counter is esp_timer µs, the same on both cores */
				uint32_t el = t1 - t0;
				b->sample_ms = el / 1000;
				hifi_diag_cpu(a, na, b, el);
				*last = *b;
			} else {
				r = b->n < 0 ? -b->n : -2;
			}
		} else {
			r = !(st && a && b) ? -2 : b->n < 0 ? -b->n : -1;
		}
		free(st);
		free(a);
		free(b);
	}
	if (!last) r = -1;
	if (!r && !last->n) r = -1;
	if (!r) *out = *last;
	xSemaphoreGive(task_lock);
	return r;
}

/* ------------------------------------------------------------------ net */

static void net(hifi_diag_net_t *o) {
	wifi_ap_record_t ap;
	if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
		o->rssi = ap.rssi;
		o->channel = ap.primary;
		snprintf(o->bssid, sizeof(o->bssid), "%02x:%02x:%02x:%02x:%02x:%02x", ap.bssid[0], ap.bssid[1], ap.bssid[2], ap.bssid[3],
			 ap.bssid[4], ap.bssid[5]);
		o->phy = ap.phy_11n ? "11n" : ap.phy_11g ? "11g" : ap.phy_11b ? "11b" : NULL;
	}
	o->connects = connects;
	o->disconnects = disconnects;
	o->last_reason = last_reason;
	portENTER_CRITICAL(&ip_mux);
	int64_t ip = ip_since_us;
	portEXIT_CRITICAL(&ip_mux);
	o->up_s = ip ? (uint32_t) ((esp_timer_get_time() - ip) / 1000000) : 0;
	uint32_t first;
	hifi_remote_link(o, &first);
	hifi_listen_diag(o);
}

/* ------------------------------------------------------------------ audio */

/* patches/0009: squeezelite's output (absent in a build without it) */
extern void hifi_output_stats(uint32_t *underruns, uint32_t *rate, uint32_t *buffer_ms, uint32_t *dma_ms, uint32_t *played_s)
	__attribute__((weak));

static void audio(hifi_diag_audio_t *o) {
	if (!hifi_output_stats) return;
	hifi_output_stats(&o->underruns, &o->rate, &o->buffer_ms, &o->dma_ms, &o->played_s);
	o->known = true;
}

/* ------------------------------------------------------------------ ota */

typedef struct {
	uint32_t bytes, total_ms, write_ms, boot;
	char version[41];
} ota_rec_t;

void hifi_diag_ota_done(uint32_t bytes, uint32_t total_ms, uint32_t write_ms, const char *version) {
	ota_rec_t r = {.bytes = bytes, .total_ms = total_ms, .write_ms = write_ms, .boot = boots};
	strlcpy(r.version, version ? version : "", sizeof(r.version));
	nvs_handle_t h;
	if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
	nvs_set_blob(h, "ota", &r, sizeof(r));
	nvs_commit(h);
	nvs_close(h);
	ESP_LOGI(TAG, "update %s: %u B in %u ms, %u ms of it in flash writes", r.version, (unsigned) bytes, (unsigned) total_ms,
		 (unsigned) write_ms);
}

static void ota(hifi_diag_ota_t *o) {
	nvs_handle_t h;
	if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
	ota_rec_t r;
	size_t n = sizeof(r);
	if (nvs_get_blob(h, "ota", &r, &n) == ESP_OK && n == sizeof(r)) {
		o->known = true;
		o->bytes = r.bytes;
		o->total_ms = r.total_ms;
		o->write_ms = r.write_ms;
		o->boot = r.boot;
		r.version[sizeof(r.version) - 1] = '\0';
		memcpy(o->version, r.version, sizeof(o->version));
	}
	nvs_close(h);
}

/* ------------------------------------------------------------------ echo */

#define ECHO_FRAME 8192 /* bytes per message: the bench takes up to 256 KB */

static int echo(uint32_t bytes, hifi_diag_echo_t *o, const char **err) {
	if (!hifi_ws_up()) return *err = "the throughput test needs the wss:// bench link up", 409;
	char *f = heap_caps_malloc(ECHO_FRAME, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if (!f) return *err = "no memory for the test", 503;
	/* {"t":"sink","pad":"xxxx"}: the bench reads and drops a message of a
	 * type it doesn't know */
	static const char head[] = "{\"t\":\"sink\",\"pad\":\"";
	size_t hl = sizeof(head) - 1;
	memcpy(f, head, hl);
	memset(f + hl, 'x', ECHO_FRAME - hl - 2);
	memcpy(f + ECHO_FRAME - 2, "\"}", 2);
	uint32_t sent = 0;
	int64_t t0 = esp_timer_get_time();
	int st = 0;
	while (sent < bytes) {
		if (esp_timer_get_time() - t0 > HIFI_DIAG_ECHO_BUDGET_S * 1000000LL) break; /* a slow link: what went, in the time */
		uint32_t n = bytes - sent < ECHO_FRAME ? bytes - sent : ECHO_FRAME;
		if (n < hl + 2) n = hl + 2;
		f[n - 2] = '"';
		f[n - 1] = '}';
		bool ok = hifi_ws_send(f, n);
		f[n - 2] = f[n - 1] = 'x';
		if (!ok) {
			*err = "the link dropped during the test";
			st = 503;
			break;
		}
		sent += n;
	}
	int64_t us = esp_timer_get_time() - t0;
	free(f);
	o->bytes = sent;
	o->ms = (uint32_t) ((us + 500) / 1000);
	if (!o->ms && sent) o->ms = 1;
	return st;
}

/* ------------------------------------------------------------------ ops */

const hifi_diag_ops_t hifi_diag_ops = {
	.sys = sys,
	.tasks = tasks,
	.net = net,
	.audio = audio,
	.ota = ota,
	.echo = echo,
};
