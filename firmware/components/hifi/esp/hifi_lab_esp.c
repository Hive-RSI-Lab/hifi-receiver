/*
 * The lab frame on the board: one lock around the lab's state, a task that
 * ticks it once a second (deadlines, low RAM), and the name of a running
 * experiment kept where a crash's restart doesn't clear it.
 */
#include "hifi_lab_esp.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "hifi_lab";

static hifi_lab_t lab;
static SemaphoreHandle_t lab_lock;

/* what ran when the board went down: survives a panic or watchdog restart */
#define KEPT_MAGIC 0x1ab5a7e1u
static RTC_NOINIT_ATTR uint32_t kept_magic;
static RTC_NOINIT_ATTR char kept_name[16];

/* an experiment that ended itself, handed over to the tick */
static volatile const char *finished_name;
static volatile bool finished_ok;

static volatile uint32_t ticked_ms; /* the tick task's last pass */

static uint32_t now_ms(void) { return (uint32_t) (esp_timer_get_time() / 1000); }
static uint32_t heap_now(void) { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }

static void keep_running(void) {
	const char *r = hifi_lab_running(&lab);
	strlcpy(kept_name, r, sizeof(kept_name));
	kept_magic = KEPT_MAGIC;
}

void hifi_lab_esp_finished(const char *name, bool ok) {
	finished_ok = ok;
	finished_name = name;
}

static void tick_task(void *arg) {
	for (;;) {
		vTaskDelay(pdMS_TO_TICKS(1000));
		xSemaphoreTake(lab_lock, portMAX_DELAY);
		const char *f = (const char *) finished_name;
		if (f) {
			finished_name = NULL;
			hifi_lab_finished(&lab, f, now_ms(), finished_ok);
		}
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
	kept_magic = KEPT_MAGIC;
	kept_name[0] = 0;
	hifi_lab_register(&lab, &lab_espnow);
	lab_lock = xSemaphoreCreateMutex();
	xTaskCreatePinnedToCore(tick_task, "hifi_lab", 3072, NULL, ESP_TASK_PRIO_MIN + 2, NULL, 0);
	ESP_LOGI(TAG, "lab build: %d experiment(s), none running", lab.n);
}

int hifi_lab_esp_op(bool post, const char *body, char *out, size_t cap) {
	if (!lab_lock) return 503;
	xSemaphoreTake(lab_lock, portMAX_DELAY);
	int st = hifi_lab_request(&lab, post, body, now_ms(), heap_now(), out, cap);
	keep_running();
	xSemaphoreGive(lab_lock);
	return st;
}

bool hifi_lab_esp_ok(void) {
#ifdef CONFIG_HIFI_LAB_DRILL
	return false;
#else
	return lab_lock && lab.n > 0 && ticked_ms && now_ms() - ticked_ms < 5000;
#endif
}

void hifi_lab_esp_selftest(hifi_selftest_t *t) {
	char v[64];
	bool ok = false;
	if (lab_lock && xSemaphoreTake(lab_lock, pdMS_TO_TICKS(500))) {
		ok = hifi_lab_selftest(&lab, v, sizeof(v)) && hifi_lab_esp_ok();
		xSemaphoreGive(lab_lock);
	} else
		strlcpy(v, "not started", sizeof(v));
	hifi_selftest_add(t, "lab", ok, "%s", v);
}
