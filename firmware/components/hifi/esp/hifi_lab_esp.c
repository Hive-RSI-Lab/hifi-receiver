/*
 * The lab frame on the board: one lock around the lab's state, a task that
 * ticks it once a second (deadlines, low RAM), a watch on a timer of its own
 * (the dial-in, and the tick itself: a start or stop that hangs holding the
 * lock stops the tick, never the watch), and the name of a running (or just
 * stopped) experiment kept where a crash's restart doesn't clear it.
 *
 * The tick and the lab's HTTP handler can each wait up to an experiment's
 * stop timeout (3 s for ESP-NOW) under the lock while a stop winds down.
 */
#include "hifi_lab_esp.h"

#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hifi_crash.h"
#include "hifi_esp.h"

static const char *TAG = "hifi_lab";

/* plain data, only touched from tasks: in PSRAM to spare the boot heap */
static EXT_RAM_ATTR hifi_lab_t lab;
static SemaphoreHandle_t lab_lock;

/* what ran when the board went down: survives a panic or watchdog restart */
#define KEPT_MAGIC 0x1ab5a7e1u
static RTC_NOINIT_ATTR uint32_t kept_magic;
static RTC_NOINIT_ATTR char kept_name[16];
/* the lab restarted the board itself: off the dial-in too long, or its tick stuck */
#define OFFLINE_MAGIC 0x0ff11e5eu
#define STUCK_MAGIC 0x57c4ed01u
static RTC_NOINIT_ATTR uint32_t kept_offline;

/* experiments that ended themselves, handed over to whoever next holds the
 * lab's lock (the tick or a request); the run number drops a late one */
typedef struct {
	const char *name;
	uint32_t run;
	bool ok;
} finish_t;
#define FINISH_RING 4
static finish_t finishes[FINISH_RING];
static unsigned finish_head, finish_n;
static portMUX_TYPE finish_mux = portMUX_INITIALIZER_UNLOCKED;

static volatile uint32_t ticked_ms; /* the tick task's last pass */
/* The tick's stack lives in PSRAM: 3 KB of internal RAM is a quarter of
 * what the Lite has left with Wi-Fi, Bluetooth and the player up, and the
 * self-test wants 12000 B. A task with a PSRAM stack must never write flash:
 * the tick only touches the lab's state and the experiments' stop (radio
 * calls, as the ESP-NOW worker makes from its own PSRAM stack). */
#define TICK_STACK 3072
static StaticTask_t tick_tcb;
static esp_timer_handle_t watch_timer;

static uint32_t now_ms(void) { return (uint32_t) (esp_timer_get_time() / 1000); }
static uint32_t heap_now(void) { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }

static void keep_running(void) {
	const char *r = hifi_lab_recent(&lab, now_ms());
	strlcpy(kept_name, r, sizeof(kept_name));
	kept_magic = KEPT_MAGIC;
}

void hifi_lab_esp_finished(const char *name, uint32_t run, bool ok) {
	portENTER_CRITICAL_SAFE(&finish_mux);
	if (finish_n == FINISH_RING) { /* full: the oldest goes */
		finish_head = (finish_head + 1) % FINISH_RING;
		finish_n--;
	}
	finishes[(finish_head + finish_n) % FINISH_RING] = (finish_t) {name, run, ok};
	finish_n++;
	portEXIT_CRITICAL_SAFE(&finish_mux);
}

/* under the lab's lock */
static void apply_finishes(void) {
	for (;;) {
		finish_t f;
		portENTER_CRITICAL_SAFE(&finish_mux);
		bool any = finish_n > 0;
		if (any) {
			f = finishes[finish_head];
			finish_head = (finish_head + 1) % FINISH_RING;
			finish_n--;
		}
		portEXIT_CRITICAL_SAFE(&finish_mux);
		if (!any) return;
		hifi_lab_finished(&lab, f.name, f.run, now_ms(), f.ok);
	}
}

/* esp_restart runs the shutdown handlers first, esp_wifi_stop among them,
 * which waits on the Wi-Fi task: if the hang is in the Wi-Fi driver, the
 * restart hangs there too. The task watchdog backs it: the timer task joins
 * it here and never feeds it, so a stuck restart panics in 15 s. */
static void restart_why(uint32_t magic, const char *why) {
	ESP_LOGE(TAG, "%s: restarting", why);
	kept_offline = magic;
	esp_task_wdt_add(NULL);
	hifi_restart_note(magic == OFFLINE_MAGIC ? "lab_offline" : "lab_stuck");
	esp_restart();
}

/* on esp_timer, never under the lab's lock: a hang inside an experiment's
 * start or stop holds the lock, and that is a hang this has to see */
static void watch(void *arg) {
	static hifi_lab_link_t w;
	bool configured, up;
	hifi_remote_up(&configured, &up);
	uint32_t now = now_ms();
	if (hifi_lab_link_tick(&w, now, configured, up)) restart_why(OFFLINE_MAGIC, "off the dial-in too long");
	uint32_t t = ticked_ms;
	if (t && now - t >= HIFI_LAB_STUCK_MS) restart_why(STUCK_MAGIC, "the lab's tick is stuck");
}

#ifdef CONFIG_HIFI_LAB_DRILL_STUCK
/* the drill: once per power-up, 5 min after boot the tick hangs holding the
 * lock, as a stuck experiment stop would; the watch must restart the board */
static void drill_stuck(void) {
	static RTC_NOINIT_ATTR uint32_t drilled;
	if (drilled == STUCK_MAGIC || lab.stuck_restart || now_ms() < 300000u) return;
	drilled = STUCK_MAGIC;
	ESP_LOGW(TAG, "drill: the tick hangs holding the lab's lock");
	for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}
#endif

static void tick_task(void *arg) {
	for (;;) {
		vTaskDelay(pdMS_TO_TICKS(1000));
		xSemaphoreTake(lab_lock, portMAX_DELAY);
#ifdef CONFIG_HIFI_LAB_DRILL_STUCK
		drill_stuck();
#endif
		apply_finishes();
		int n = hifi_lab_tick(&lab, now_ms(), heap_now());
		ticked_ms = now_ms();
		keep_running();
		xSemaphoreGive(lab_lock);
		if (n) ESP_LOGW(TAG, "stopped %d experiment(s) (deadline or low RAM)", n);
	}
}

void hifi_lab_esp_init(void) {
	hifi_lab_init(&lab);
	esp_reset_reason_t why = esp_reset_reason();
	bool crash = why == ESP_RST_PANIC || why == ESP_RST_INT_WDT || why == ESP_RST_TASK_WDT || why == ESP_RST_WDT ||
		     why == ESP_RST_BROWNOUT;
	if (kept_magic == KEPT_MAGIC && crash && kept_name[0]) {
		kept_name[sizeof(kept_name) - 1] = 0;
		hifi_lab_set_crashed(&lab, kept_name);
		ESP_LOGW(TAG, "the last restart came during the %s experiment", kept_name);
	}
	/* the lab's own restart: a plain one, or the watchdog's when a shutdown
	 * handler hung (see restart_why); name the experiment it was in */
	bool own = why == ESP_RST_SW || why == ESP_RST_TASK_WDT || why == ESP_RST_PANIC;
	if (own && (kept_offline == OFFLINE_MAGIC || kept_offline == STUCK_MAGIC)) {
		if (kept_offline == OFFLINE_MAGIC) lab.offline_restart = true;
		else lab.stuck_restart = true;
		kept_name[sizeof(kept_name) - 1] = 0;
		if (kept_magic == KEPT_MAGIC && kept_name[0]) hifi_lab_set_crashed(&lab, kept_name);
		ESP_LOGW(TAG, "the last restart was the lab's own (%s%s): %s", why == ESP_RST_SW ? "" : "watchdog, ",
			 kept_name[0] ? kept_name : "no experiment",
			 kept_offline == OFFLINE_MAGIC ? "off the dial-in too long" : "its tick was stuck");
	}
	kept_offline = 0;
	kept_magic = KEPT_MAGIC;
	kept_name[0] = 0;
	/* weak, pulled into the app by patch 0035: recovery keeps only lab_i2c,
	 * so its self-test still counts one experiment */
	if (&lab_espnow) hifi_lab_register(&lab, &lab_espnow);
	if (&lab_lanscan) hifi_lab_register(&lab, &lab_lanscan);
	if (&lab_forge) hifi_lab_register(&lab, &lab_forge);
	if (&lab_wifidrop) hifi_lab_register(&lab, &lab_wifidrop);
	if (&lab_crafted) hifi_lab_register(&lab, &lab_crafted);
	hifi_lab_register(&lab, &lab_i2c);
#if CONFIG_BT_CLASSIC_ENABLED
	if (&lab_bt) hifi_lab_register(&lab, &lab_bt); /* NULL in recovery */
	if (&lab_a2dp) hifi_lab_register(&lab, &lab_a2dp);
#endif
#if CONFIG_BT_BLE_ENABLED
	if (&lab_ble) hifi_lab_register(&lab, &lab_ble); /* NULL in recovery */
#endif
#if CONFIG_HIFI_LAB_PSRAM_STACKS
	if (&lab_flac) hifi_lab_register(&lab, &lab_flac); /* the lab-radio app only (NULL in recovery) */
#endif
#if CONFIG_CSPOT_SINK
	if (&lab_phone) hifi_lab_register(&lab, &lab_phone); /* NULL in recovery */
#endif
	lab_lock = xSemaphoreCreateMutex();
	StackType_t *stack = NULL;
#if CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
	stack = heap_caps_malloc(TICK_STACK, MALLOC_CAP_SPIRAM);
#endif
	if (!stack || !xTaskCreateStaticPinnedToCore(tick_task, "hifi_lab", TICK_STACK, NULL, ESP_TASK_PRIO_MIN + 2, stack,
						     &tick_tcb, 0)) {
		free(stack); /* no PSRAM: an internal stack, as before */
		xTaskCreatePinnedToCore(tick_task, "hifi_lab", TICK_STACK, NULL, ESP_TASK_PRIO_MIN + 2, NULL, 0);
	}
	const esp_timer_create_args_t wa = {.callback = watch, .name = "hifi_lab_watch"};
	if (esp_timer_create(&wa, &watch_timer) == ESP_OK) esp_timer_start_periodic(watch_timer, 1000000);
	else ESP_LOGE(TAG, "no watch timer: the lab can't restart a stuck board");
	ESP_LOGI(TAG, "lab build: %d experiment(s), none running", lab.n);
}

int hifi_lab_esp_op(bool post, const char *body, char *out, size_t cap) {
	if (!lab_lock) return 503;
	xSemaphoreTake(lab_lock, portMAX_DELAY);
	apply_finishes();
	int st = hifi_lab_request(&lab, post, body, now_ms(), heap_now(), out, cap);
	keep_running();
	xSemaphoreGive(lab_lock);
	return st;
}

bool hifi_lab_esp_ok(void) {
#ifdef CONFIG_HIFI_LAB_DRILL
	return false;
#else
	return lab_lock && watch_timer && lab.n > 0 && ticked_ms && now_ms() - ticked_ms < 5000;
#endif
}

void hifi_lab_esp_selftest(hifi_selftest_t *t) {
	char v[64]; /* hifi_check_t.value */
	bool ok = false;
	if (lab_lock && xSemaphoreTake(lab_lock, pdMS_TO_TICKS(500))) {
		ok = hifi_lab_selftest(&lab, v, sizeof(v)) && hifi_lab_esp_ok();
		xSemaphoreGive(lab_lock);
	} else
		strlcpy(v, "not started", sizeof(v));
	hifi_selftest_add(t, "lab", ok, "%s", v);
}
