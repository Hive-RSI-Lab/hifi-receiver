/*
 * Lab experiment "wifidrop": the board's Wi-Fi drops and comes back, again
 * and again, to load the tasks a drop runs through (the event loop, the
 * timer task, the network manager, mDNS re-probing its name against the
 * other boards on the LAN) and read their stacks' high-water marks after.
 *
 *   api lab "{\"op\":\"start\",\"exp\":\"wifidrop\",\"cycles\":20,\"max_s\":1500}"
 *   api lab "{\"op\":\"result\",\"exp\":\"wifidrop\"}"
 *
 * Each cycle waits until the station has an address and the Hive's dial-in
 * is up, holds settle_s (default 10, 0-60) so mDNS and the dial-in finish
 * what a reconnect starts, then calls esp_wifi_disconnect(): the network
 * manager sees the drop as the router's and reconnects by itself, as it
 * would after a real one (every third drop it waits for its polling timer
 * first: its retry count is never reset on a connect). A cycle that isn't
 * back within back_s (default 120, 30-600) ends the run "failed". A
 * reconnect that fails for another reason than a timeout (the AP not found
 * in the scan, say) sends the manager to its setup AP, where it stays: only
 * the lab's offline watch, restarting the board after 15 minutes off the
 * dial-in, brings it back (a router drop does the same). Read diag/tasks
 * as soon as a run fails: the restart loses the high-water marks.
 *
 * cycles: 1-50, default 20. The run ends "done" after the last one is back.
 * The result has each cycle's time to come back (ms) and the slowest.
 */
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_task.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "hifi_esp.h"
#include "hifi_json.h"
#include "hifi_lab_esp.h"

static const char *TAG = "lab_wifidrop";

#define STACK 3072
#define CYCLES_MAX 50

static TaskHandle_t worker;
static StaticTask_t worker_tcb;
static volatile bool run, alive;
static bool started;
static uint32_t run_no, cycles, settle_s, back_s;
static volatile uint32_t done_n;
static uint32_t back_ms[CYCLES_MAX];
static const char *volatile phase = "idle";
static char why[48];

static bool up(void) {
	esp_netif_t *ni = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
	esp_netif_ip_info_t ip;
	wifi_ap_record_t ap;
	bool configured, remote;
	hifi_remote_up(&configured, &remote);
	return ni && esp_netif_is_netif_up(ni) && esp_netif_get_ip_info(ni, &ip) == ESP_OK && ip.ip.addr &&
	       esp_wifi_sta_get_ap_info(&ap) == ESP_OK && (!configured || remote);
}

/* the dial-in's flag is the relay's copy, refreshed every 2 s: after a drop
 * wait for it to fall before "up" can count, or a quick DHCP restore reads
 * the link from before the drop. A link that never falls in 10 s (it
 * survived the drop) counts as down already. */
static void wait_remote_down(void) {
	for (int t = 0; run && t < 10000; t += 250) {
		bool configured, remote;
		hifi_remote_up(&configured, &remote);
		if (!configured || !remote) return;
		vTaskDelay(pdMS_TO_TICKS(250));
	}
}

/* false: stopped, or not back in time */
static bool wait_up(uint32_t limit_ms, uint32_t *took_ms) {
	int64_t t0 = esp_timer_get_time();
	while (run) {
		if (up()) {
			if (took_ms) *took_ms = (uint32_t) ((esp_timer_get_time() - t0) / 1000);
			return true;
		}
		if (esp_timer_get_time() - t0 > (int64_t) limit_ms * 1000) return false;
		vTaskDelay(pdMS_TO_TICKS(250));
	}
	return false;
}

static bool hold(uint32_t ms) {
	for (uint32_t t = 0; run && t < ms; t += 250) vTaskDelay(pdMS_TO_TICKS(250));
	return run;
}

static bool drops(void) {
	if (!wait_up(back_s * 1000, NULL)) {
		if (run) snprintf(why, sizeof(why), "not up at the start");
		return false;
	}
	for (uint32_t i = 0; i < cycles; i++) {
		phase = "settling";
		if (!hold(settle_s * 1000)) return false;
		phase = "down";
		/* it may already be down (a real drop in the hold): wait it out */
		if (!up() && !wait_up(back_s * 1000, NULL)) {
			if (run) snprintf(why, sizeof(why), "cycle %u: down before the drop", (unsigned) i + 1);
			return false;
		}
		esp_err_t e = esp_wifi_disconnect();
		if (e != ESP_OK) {
			if (run) snprintf(why, sizeof(why), "esp_wifi_disconnect: %s", esp_err_to_name(e));
			return false;
		}
		/* the station loses its AP at once; the dial-in's flag lags */
		int64_t t_drop = esp_timer_get_time();
		vTaskDelay(pdMS_TO_TICKS(500));
		wait_remote_down();
		phase = "coming back";
		if (!wait_up(back_s * 1000, NULL)) {
			if (run) snprintf(why, sizeof(why), "cycle %u: not back in %u s", (unsigned) i + 1, (unsigned) back_s);
			return false;
		}
		back_ms[i] = (uint32_t) ((esp_timer_get_time() - t_drop) / 1000);
		done_n = i + 1;
		ESP_LOGI(TAG, "cycle %u/%u back in %u ms", (unsigned) i + 1, (unsigned) cycles, (unsigned) back_ms[i]);
	}
	/* the last reconnect's mDNS and dial-in work happens in this hold */
	phase = "settling";
	hold(settle_s * 1000);
	return true;
}

static void task_main(void *arg) {
	for (;;) {
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		uint32_t my_run = run_no;
		bool ok = drops();
		bool ended_itself = run;
		phase = ok ? "done" : run ? "failed" : "stopped";
		run = false;
		if (ended_itself) hifi_lab_esp_finished("wifidrop", my_run, ok);
		ESP_LOGI(TAG, "%s after %u cycle(s)", phase, (unsigned) done_n);
		alive = false;
	}
}

static uint32_t num(const char *body, const char *key, uint32_t dflt) {
	double v;
	if (!hj_get_num(body, key, &v) || !(v >= 0)) return dflt;
	return v >= 4294967295.0 ? UINT32_MAX : (uint32_t) v;
}

static int start(uint32_t run_n, const char *body, char *err, size_t n) {
	if (alive) {
		snprintf(err, n, "the last run is still stopping");
		return 409;
	}
	uint32_t c = num(body, "cycles", 20), s = num(body, "settle_s", 10), b = num(body, "back_s", 120);
	if (c < 1 || c > CYCLES_MAX || s > 60 || b < 30 || b > 600) {
		snprintf(err, n, "cycles 1-%d, settle_s 0-60, back_s 30-600", CYCLES_MAX);
		return 400;
	}
	if (!up()) {
		snprintf(err, n, "Wi-Fi or the dial-in isn't up");
		return 503;
	}
	if (!worker) {
#if CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
		StackType_t *stack = heap_caps_malloc(STACK, MALLOC_CAP_SPIRAM);
#else
		StackType_t *stack = heap_caps_malloc(STACK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif
		if (!stack) {
			snprintf(err, n, "out of memory");
			return 503;
		}
		worker = xTaskCreateStaticPinnedToCore(task_main, "lab_wifidrop", STACK, NULL, ESP_TASK_PRIO_MIN + 2, stack,
						       &worker_tcb, 0);
		if (!worker) {
			free(stack);
			snprintf(err, n, "no worker task");
			return 503;
		}
	}
	cycles = c;
	settle_s = s;
	back_s = b;
	done_n = 0;
	memset(back_ms, 0, sizeof(back_ms));
	why[0] = 0;
	phase = "starting";
	run_no = run_n;
	started = run = alive = true;
	xTaskNotifyGive(worker);
	return 0;
}

/* the worker checks run every 250 ms; a drop in progress finishes by itself
 * (the network manager reconnects whether or not the run goes on) */
static bool stop(void) {
	run = false;
	for (int i = 0; i < 20 && alive; i++) vTaskDelay(pdMS_TO_TICKS(100));
	return !alive;
}

static void result(void *hj) {
	hj_t *j = hj;
	if (!started) return;
	hj_str(j, "phase", phase);
	hj_int(j, "cycles", cycles);
	hj_int(j, "done", done_n);
	if (why[0]) hj_str(j, "why", why);
	uint32_t n = done_n, worst = 0;
	hj_arr(j, "back_ms");
	for (uint32_t i = 0; i < n && i < CYCLES_MAX; i++) {
		hj_int(j, NULL, back_ms[i]);
		if (back_ms[i] > worst) worst = back_ms[i];
	}
	hj_arr_end(j);
	hj_int(j, "worst_ms", worst);
}

const hifi_lab_exp_t lab_wifidrop = {
	.name = "wifidrop",
	.help = "Wi-Fi drops and comes back: cycles (1-50, 20), settle_s (0-60, 10), back_s (30-600, 120)",
	.start = start,
	.stop = stop,
	.result = result,
};
