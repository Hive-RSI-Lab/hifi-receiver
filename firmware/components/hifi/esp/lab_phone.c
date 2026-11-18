/*
 * Lab experiment "phone": a stand-in for a phone's Spotify app, to time the
 * handover between Bluetooth and Spotify Connect on a bench with no phone
 * (spec: newest source wins, components/hifi/core/hifi_source.h).
 *
 *   api lab "{\"op\":\"start\",\"exp\":\"phone\",\"hold_s\":30}"
 *   api lab "{\"op\":\"result\",\"exp\":\"phone\"}"
 *
 * It calls the Spotify sink's own handlers in decode_external.c (patch 0034's
 * hifi_lab_cspot), in the order cspot does when a phone picks the board: the
 * login check (CSPOT_BUSY), a track start at rate (44100 or 48000, default
 * 44100), play, then 16-bit stereo audio (a -12 dBFS 441 Hz tone at 44.1
 * kHz) as fast as the output takes it. So everything from the sink handler
 * on is the product's path: the rule, the takeover of a Bluetooth phone that
 * plays (its quiet fade, its pause and the dropped link), and the owner-only
 * writes. What it leaves out is cspot itself: the Spotify servers, the
 * zeroconf login and the decoder. A Bluetooth source that takes the output
 * back calls cspot_disconnect(), which does nothing here (no Spotify session
 * is up), so the run sees the loss as the output's owner changing.
 *
 * The run ends by itself when it has played hold_s (1-600, default 30), or
 * as soon as another source has the output ("lost"): ok either way, failed
 * when the login or the start was refused. If the output is still its own
 * it then ends as cspot does: with end "disc" (the default) a pause, then
 * the session's end; with end "stop" the playlist's end once the queued
 * audio has played out (CSPOT_STOP, up to 5 s; the session stays up). A stop
 * of the run ends it the same way, without the wait.
 *
 * Result: phase, login and start (true: accepted), before (hifi_sink_source()'s
 * number for what played when it started: 0 idle, 2 Bluetooth, 4 Spotify...),
 * start_call_ms (the start handler's own time: a takeover's fade and the
 * phone's pause run in it), own_ms (from the login to the first audio the
 * output took), bytes (audio the output took), short_writes (writes it took
 * only part of: the output was full, as it is once it plays), lost_ms (from
 * the login to another source having the output, -1 not lost), lost_to (that
 * source's number), played_ms (from the first audio to the end or the loss),
 * end, after (what the output plays once the run has ended: 0 after a stop,
 * the output let go) and now (what it plays at the time of the result).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "hifi_json.h"
#include "hifi_lab_esp.h"

static const char *TAG = "lab_phone";

/* the Spotify sink's handlers (decode_external.c, patch 0034) and what the
 * output plays; weak, so an image without the Spotify sink refuses the start */
extern bool hifi_lab_cspot(int op, uint32_t rate) __attribute__((weak));
extern uint32_t hifi_lab_cspot_data(const uint8_t *data, uint32_t len) __attribute__((weak));
extern int hifi_sink_source(void) __attribute__((weak));
enum { OP_LOGIN, OP_START, OP_PLAY, OP_PAUSE, OP_DISC, OP_STOP, OP_QUEUED };
#define SRC_SPOTIFY 4

#define STACK 4096
#define CHUNK_FRAMES 1000 /* 22.7 ms at 44.1 kHz, ten periods of the tone */

static TaskHandle_t worker;
static StaticTask_t worker_tcb;
static volatile bool run, alive;
static bool started;
static uint32_t run_no, hold_s, rate;
static const char *volatile phase = "idle";
static char why[48];
static bool login_ok, start_ok, end_stop;
static int before, lost_to, after;
static int64_t t0, t_first, t_lost, t_end;
static uint32_t start_call_ms, short_writes;
static volatile uint64_t bytes;
static int16_t chunk[CHUNK_FRAMES * 2];

static int64_t now_us(void) {
	return esp_timer_get_time();
}

static int source(void) {
	return hifi_sink_source ? hifi_sink_source() : -1;
}

/* -12 dBFS (8231/32768): a 100-frame period, 441 Hz at 44.1 kHz, the same
 * tone lab_a2dp's source sends */
static void fill_tone(void) {
	static const int16_t q[25] = {0,    517,  1032, 1543, 2047, 2542, 3027, 3499, 3956,
				      4396, 4818, 5219, 5598, 5953, 6283, 6587, 6862, 7108,
				      7324, 7509, 7662, 7782, 7870, 7924, 7942};
	for (int i = 0; i < CHUNK_FRAMES; i++) {
		int k = i % 100, v;
		if (k < 25) v = q[k];
		else if (k < 50) v = q[50 - k];
		else if (k < 75) v = -q[k - 50];
		else v = -q[100 - k];
		/* q[] tops out a little over -12.3 dBFS: scale up to 8231 */
		v = v * 8231 / 7942;
		chunk[2 * i] = chunk[2 * i + 1] = (int16_t) v;
	}
}

/* true: the run should go on; false: stopped, lost, or held long enough */
static bool feed(void) {
	const uint8_t *p = (const uint8_t *) chunk;
	uint32_t left = sizeof(chunk);
	while (left) {
		if (!run) return false;
		if (source() != SRC_SPOTIFY) {
			t_lost = now_us();
			lost_to = source();
			phase = "lost";
			return false;
		}
		uint32_t w = hifi_lab_cspot_data(p, left);
		if (w && !t_first) t_first = now_us();
		bytes += w;
		p += w;
		left -= w;
		if (left) {
			short_writes++;
			vTaskDelay(pdMS_TO_TICKS(10));
		}
		if (t_first && now_us() - t_first >= (int64_t) hold_s * 1000000) return false;
	}
	return true;
}

/* false: refused at the login or the start */
static bool play(void) {
	before = source();
	t0 = now_us();
	login_ok = hifi_lab_cspot(OP_LOGIN, 0);
	if (!login_ok) {
		snprintf(why, sizeof(why), "login refused (output %d)", before);
		return false;
	}
	int64_t ts = now_us();
	start_ok = hifi_lab_cspot(OP_START, rate);
	start_call_ms = (uint32_t) ((now_us() - ts) / 1000);
	if (!start_ok) {
		snprintf(why, sizeof(why), "start refused (output %d)", source());
		return false;
	}
	hifi_lab_cspot(OP_PLAY, 0);
	phase = "playing";
	while (feed()) {
	}
	return true;
}

static void task_main(void *arg) {
	for (;;) {
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		uint32_t my_run = run_no;
		bool ok = play();
		/* end the session as cspot does, if the output is still ours (a
		 * source that took it has ended it already); a playlist's end waits
		 * for the queued audio, up to 5 s */
		if (start_ok && source() == SRC_SPOTIFY) {
			if (end_stop) {
				for (int i = 0; i < 50 && run && hifi_lab_cspot(OP_QUEUED, 0); i++)
					vTaskDelay(pdMS_TO_TICKS(100));
				hifi_lab_cspot(OP_STOP, 0);
			} else {
				hifi_lab_cspot(OP_PAUSE, 0);
				hifi_lab_cspot(OP_DISC, 0);
			}
		}
		after = source();
		t_end = now_us();
		bool ended_itself = run;
		if (!ok) phase = "failed";
		else if (strcmp(phase, "lost")) phase = ended_itself ? "done" : "stopped";
		run = false;
		if (ended_itself) hifi_lab_esp_finished("phone", my_run, ok);
		ESP_LOGI(TAG, "%s: %llu bytes, lost to %d", phase, (unsigned long long) bytes, lost_to);
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
	if (!hifi_lab_cspot || !hifi_lab_cspot_data || !hifi_sink_source) {
		snprintf(err, n, "this image has no Spotify sink");
		return 400;
	}
	uint32_t h = num(body, "hold_s", 30), r = num(body, "rate", 44100);
	char end[8] = "disc";
	hj_get_str(body, "end", end, sizeof(end));
	if (h < 1 || h > 600 || (r != 44100 && r != 48000) || (strcmp(end, "disc") && strcmp(end, "stop"))) {
		snprintf(err, n, "hold_s 1-600, rate 44100 or 48000, end disc or stop");
		return 400;
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
		worker = xTaskCreateStaticPinnedToCore(task_main, "lab_phone", STACK, NULL, ESP_TASK_PRIO_MIN + 5, stack,
						       &worker_tcb, 0);
		if (!worker) {
			free(stack);
			snprintf(err, n, "no worker task");
			return 503;
		}
		fill_tone();
	}
	hold_s = h;
	rate = r;
	end_stop = !strcmp(end, "stop");
	login_ok = start_ok = false;
	before = lost_to = after = -1;
	t0 = t_first = t_lost = t_end = 0;
	start_call_ms = short_writes = 0;
	bytes = 0;
	why[0] = 0;
	phase = "starting";
	run_no = run_n;
	started = run = alive = true;
	xTaskNotifyGive(worker);
	return 0;
}

/* the worker checks run between writes, at least every 10 ms; the end of
 * the session (pause, disconnect) takes a few ms */
static bool stop(void) {
	run = false;
	for (int i = 0; i < 20 && alive; i++) vTaskDelay(pdMS_TO_TICKS(100));
	return !alive;
}

static int ms_since(int64_t from, int64_t to) {
	return from && to ? (int) ((to - from) / 1000) : -1;
}

static void result(void *hj) {
	hj_t *j = hj;
	if (!started) return;
	int64_t end = alive ? now_us() : t_end;
	hj_str(j, "phase", phase);
	if (why[0]) hj_str(j, "why", why);
	hj_int(j, "rate", rate);
	hj_int(j, "hold_s", hold_s);
	hj_bool(j, "login", login_ok);
	hj_bool(j, "start", start_ok);
	hj_int(j, "before", before);
	hj_int(j, "start_call_ms", start_call_ms);
	hj_int(j, "own_ms", ms_since(t0, t_first));
	hj_int(j, "bytes", (long long) bytes);
	hj_int(j, "short_writes", short_writes);
	hj_int(j, "lost_ms", ms_since(t0, t_lost));
	hj_int(j, "lost_to", lost_to);
	hj_int(j, "played_ms", ms_since(t_first, t_lost ? t_lost : end));
	hj_str(j, "end", end_stop ? "stop" : "disc");
	hj_int(j, "after", after);
	hj_int(j, "now", source());
}

/* weak in hifi_lab_esp.h: only the app links it (patch 0034 adds -u
 * lab_phone), so recovery leaves out the reach into squeezelite */
const hifi_lab_exp_t lab_phone = {
	.name = "phone",
	.help = "a stand-in Spotify app plays a tone through the Spotify sink: hold_s (1-600, 30), rate (44100/48000), end (disc/stop)",
	.start = start,
	.stop = stop,
	.result = result,
};
