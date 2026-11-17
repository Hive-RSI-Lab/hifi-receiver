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
 * The result has each cycle's time to come back (ms) and the slowest, and
 * the free internal heap before the first drop (heap_start), after each
 * cycle's settle (heap, read just before the next drop, and after the last
 * settle) and the change over the run (heap_delta, from heap_end, the
 * highest of the last three readings: a reading can catch a few hundred
 * bytes in flight, a packet or a dial-in buffer, that a leak never gives
 * back): a reconnect that leaks shows as a heap that falls by the same step
 * every cycle. heap_ok is true once a run is done with every cycle back and
 * heap_delta no lower than -heap_tol (bytes, 0-65535, default 200):
 * tools/hil-wifidrop.json in the lab project checks it, so a reconnect leak
 * fails a test.
 *
 * On an image built with CONFIG_HEAP_TRACING_STANDALONE, "trace":1 also
 * traces every allocation from just before the first drop to the end of the
 * last settle (100 records in internal RAM, as the tracer needs, taken before
 * heap_start is read) and the result lists what is still allocated in
 * internal RAM: the totals, and up to four groups by call stack (unsorted;
 * a later, bigger allocation can evict a group, whose bytes then count only
 * in the totals), in place of the per-cycle back_ms and heap arrays, so it
 * still fits one console line: look the addresses up with addr2line on the
 * image's ELF.
 */
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#if CONFIG_HEAP_TRACING_STANDALONE
#include "esp_heap_trace.h"
#include "soc/soc_memory_layout.h"
#endif
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
static uint32_t heap_start, heap_at[CYCLES_MAX + 1], heap_tol;
static volatile uint32_t heap_n;

static uint32_t heap_int(void) {
	return heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
}

#if CONFIG_HEAP_TRACING_STANDALONE
#define TRACE_RECORDS 100
#define TRACE_GROUPS 4
typedef struct {
	void *by[CONFIG_HEAP_TRACING_STACK_DEPTH];
	uint32_t n, bytes;
} trace_group_t;
static heap_trace_record_t *trace_buf;
static bool trace_want, trace_ran;
static uint32_t trace_count, trace_int_n, trace_int_bytes, trace_ngroups;
static trace_group_t trace_groups[TRACE_GROUPS];

/* the record buffer must be in internal RAM (esp_heap_trace.h), about 6 KB:
 * it is taken before heap_start is read, so it isn't counted as a change,
 * and given back once the result is grouped (trace_end), so the board isn't
 * left under the self-test's heap floor between runs */
static bool trace_ready(void) {
	if (!trace_want) return false;
	if (!trace_buf) {
		trace_buf = heap_caps_calloc(TRACE_RECORDS, sizeof(*trace_buf), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
		if (!trace_buf) {
			ESP_LOGW(TAG, "no heap trace: out of internal RAM");
			return false;
		}
		if (heap_trace_init_standalone(trace_buf, TRACE_RECORDS) != ESP_OK) {
			ESP_LOGW(TAG, "no heap trace");
			free(trace_buf);
			trace_buf = NULL;
			return false;
		}
	}
	return true;
}

/* only call with tracing stopped. On IDF v4.4 (what this builds on) a zero
 * count makes heap_trace_start refuse until trace_ready registers a new
 * buffer; IDF v5 rejects the zero count and keeps the freed buffer
 * registered. Either way, nothing else in the firmware uses the tracer, and
 * trace_ready always registers a fresh buffer before the next start */
static void trace_release(void) {
	if (!trace_buf) return;
	heap_trace_init_standalone(trace_buf, 0);
	free(trace_buf);
	trace_buf = NULL;
}

static void trace_begin(void) {
	trace_ran = trace_ready() && heap_trace_start(HEAP_TRACE_LEAKS) == ESP_OK;
}

static void trace_reset(void) {
	trace_ran = false;
	trace_count = trace_int_n = trace_int_bytes = trace_ngroups = 0;
	memset(trace_groups, 0, sizeof(trace_groups));
}

static bool trace_shown(void) {
	return trace_want;
}

/* group what is still allocated in internal RAM by call stack, in one greedy
 * pass: up to TRACE_GROUPS groups, and a new stack that doesn't fit replaces
 * the smallest group if its first record is bigger, so this is not exactly
 * the biggest four; an evicted group's bytes stay in trace_int_bytes only */
static void trace_end(void) {
	if (!trace_ran) {
		trace_release();
		return;
	}
	heap_trace_stop();
	trace_count = heap_trace_get_count();
	trace_int_n = trace_int_bytes = trace_ngroups = 0;
	memset(trace_groups, 0, sizeof(trace_groups));
	for (uint32_t i = 0; i < trace_count; i++) {
		heap_trace_record_t r;
		if (heap_trace_get(i, &r) != ESP_OK || !esp_ptr_internal(r.address)) continue;
		trace_int_n++;
		trace_int_bytes += r.size;
		uint32_t g = 0;
		while (g < trace_ngroups && memcmp(trace_groups[g].by, r.alloced_by, sizeof(r.alloced_by))) g++;
		if (g == trace_ngroups) {
			if (trace_ngroups < TRACE_GROUPS) trace_ngroups++;
			else {
				uint32_t min = 0;
				for (uint32_t k = 1; k < TRACE_GROUPS; k++)
					if (trace_groups[k].bytes < trace_groups[min].bytes) min = k;
				if (trace_groups[min].bytes > r.size) continue;
				g = min;
			}
			memcpy(trace_groups[g].by, r.alloced_by, sizeof(r.alloced_by));
			trace_groups[g].n = trace_groups[g].bytes = 0;
		}
		trace_groups[g].n++;
		trace_groups[g].bytes += r.size;
	}
	trace_release();
}

static void trace_result(hj_t *j) {
	if (!trace_want) return;
	hj_obj(j, "trace");
	hj_bool(j, "ran", trace_ran);
	if (trace_ran) {
		hj_int(j, "records", trace_count);
		hj_bool(j, "full", trace_count >= TRACE_RECORDS);
		hj_int(j, "internal_n", trace_int_n);
		hj_int(j, "internal_bytes", trace_int_bytes);
		hj_arr(j, "groups");
		for (uint32_t g = 0; g < trace_ngroups; g++) {
			/* the stack as one string, frames apart by spaces: with
			 * at most four groups and no per-cycle arrays the result
			 * fits one console line (about 1000 characters) */
			char a[CONFIG_HEAP_TRACING_STACK_DEPTH * 9 + 1];
			size_t o = 0;
			for (int k = 0; k < CONFIG_HEAP_TRACING_STACK_DEPTH; k++)
				o += snprintf(a + o, sizeof(a) - o, "%s%x", k ? " " : "", (unsigned) (uintptr_t) trace_groups[g].by[k]);
			hj_obj(j, NULL);
			hj_int(j, "n", trace_groups[g].n);
			hj_int(j, "bytes", trace_groups[g].bytes);
			hj_str(j, "by", a);
			hj_obj_end(j);
		}
		hj_arr_end(j);
	}
	hj_obj_end(j);
}
#else
static bool trace_ready(void) { return false; }
static void trace_begin(void) {}
static void trace_end(void) {}
static void trace_reset(void) {}
static bool trace_shown(void) { return false; }
static void trace_result(hj_t *j) {}
#endif

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
		if (i == 0) {
			trace_ready();
			heap_start = heap_int();
			trace_begin();
		} else
			heap_at[heap_n++] = heap_int();
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
	/* the last reconnect's mDNS and dial-in work happens in this hold; a
	 * stop or the deadline inside it leaves no settled reading, so the run
	 * is "stopped", not "done" */
	phase = "settling";
	if (!hold(settle_s * 1000)) return false;
	heap_at[heap_n++] = heap_int();
	return true;
}

static void task_main(void *arg) {
	for (;;) {
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		uint32_t my_run = run_no;
		bool ok = drops();
		trace_end();
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
	uint32_t tol = num(body, "heap_tol", 200);
#if CONFIG_HEAP_TRACING_STANDALONE
	/* set only once every check passes: a refused start leaves the last
	 * result's shape alone */
	bool tw = num(body, "trace", 0) != 0;
#else
	if (num(body, "trace", 0)) {
		snprintf(err, n, "this image has no heap tracing");
		return 400;
	}
#endif
	if (c < 1 || c > CYCLES_MAX || s > 60 || b < 30 || b > 600 || tol > 65535) {
		snprintf(err, n, "cycles 1-%d, settle_s 0-60, back_s 30-600, heap_tol 0-65535", CYCLES_MAX);
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
	heap_tol = tol;
#if CONFIG_HEAP_TRACING_STANDALONE
	trace_want = tw;
#endif
	done_n = heap_n = heap_start = 0;
	memset(back_ms, 0, sizeof(back_ms));
	trace_reset();
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
	/* a traced run leaves out the per-cycle arrays, to fit one line */
	bool arrays = !trace_shown();
	uint32_t n = done_n, worst = 0;
	for (uint32_t i = 0; i < n && i < CYCLES_MAX; i++)
		if (back_ms[i] > worst) worst = back_ms[i];
	if (arrays) {
		hj_arr(j, "back_ms");
		for (uint32_t i = 0; i < n && i < CYCLES_MAX; i++) hj_int(j, NULL, back_ms[i]);
		hj_arr_end(j);
	}
	hj_int(j, "worst_ms", worst);
	if (heap_start) {
		uint32_t m = heap_n;
		hj_int(j, "heap_start", heap_start);
		if (m) {
			uint32_t end = 0;
			for (uint32_t i = m > 3 ? m - 3 : 0; i < m; i++)
				if (heap_at[i] > end) end = heap_at[i];
			long long d = (long long) end - heap_start;
			hj_int(j, "heap_end", end);
			hj_int(j, "heap_delta", d);
			/* the check for a test plan: every cycle ran and the heap came
			 * back to within heap_tol of where it started */
			hj_bool(j, "heap_ok", !strcmp(phase, "done") && done_n == cycles && d >= -(long long) heap_tol);
		}
		if (arrays) {
			hj_arr(j, "heap");
			for (uint32_t i = 0; i < m; i++) hj_int(j, NULL, heap_at[i]);
			hj_arr_end(j);
		}
	}
	trace_result(j);
}

const hifi_lab_exp_t lab_wifidrop = {
	.name = "wifidrop",
	.help = "Wi-Fi drops and comes back: cycles (1-50, 20), settle_s (0-60, 10), back_s (30-600, 120), heap_tol (B, 200), trace (0/1)",
	.start = start,
	.stop = stop,
	.result = result,
};
