/*
 * Lab experiment "flac": high-rate FLAC decoding on a task whose stack is in
 * PSRAM (as patch 0019 puts squeezelite's decode thread), or in internal RAM
 * to compare, paced at real time beside whatever else runs (an A2DP stream,
 * ESP-NOW, BLE). The lab-radio image only (CONFIG_HIFI_LAB_PSRAM_STACKS).
 *
 *   api lab "{\"op\":\"start\",\"exp\":\"flac\",\"stack\":\"psram\",\"max_s\":60}"
 *
 * The bench has no music server to send the board FLAC, so the clip is in
 * the image (lab_assets/clip96k24.flac: 0.2 s of 96 kHz, 24-bit stereo pink
 * noise and tones, compression level 8). It is copied to PSRAM, where
 * squeezelite's stream buffer is, and decoded over and over by libFLAC (the
 * one squeezelite links). The task decodes until it is lead_ms (default 500)
 * ahead of the wall clock, then sleeps: the CPU it takes is what a real
 * 96/24 stream would take. Falling behind the wall clock is an underrun
 * (late: how many times, late_ms: for how long in all). The decoded samples
 * go nowhere (no I2S): it measures the decode, not the output.
 *
 * Parameters: stack psram|internal (default psram), lead_ms 50-2000, core
 * 0|1 (default: squeezelite's pthread default; the PSRAM worker keeps the
 * core of its first run), max_s. A result read during the run is a rough
 * snapshot; the one after it is exact.
 *
 * CPU: cpu_ms and cpu_pct_per_s are the task's own run time (FreeRTOS
 * run-time counter, esp_timer clock), so time the task spends preempted by
 * Wi-Fi or Bluetooth tasks isn't counted (interrupts taken on its core are).
 * The clip is 96 kHz/24-bit stereo with blocksize 8192, about twice a
 * typical encoder's, so worst_frame_us is about twice a typical stream's. busy_ms and busy_pct_per_s are wall time
 * inside the decoder, preemption included: their gap is what the radios
 * take from it. The per-second shares start after the first sleep, so the
 * burst that builds the lead isn't in them. frames counts audio frames.
 * stack_free_min of the PSRAM worker is the lowest over every run since boot
 * (it is one task, kept); the internal one's is this run's.
 *
 * The task never touches flash (its stack may be in PSRAM): the clip is read
 * from PSRAM, and the decoder's code runs from the cache like any task's.
 */
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_pthread.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "FLAC/stream_decoder.h"

#include "hifi_json.h"
#include "hifi_lab_esp.h"

static const char *TAG = "lab_flac";

extern const uint8_t clip_start[] asm("_binary_clip96k24_flac_start");
extern const uint8_t clip_end[] asm("_binary_clip96k24_flac_end");

#define STACK 12288 /* bytes; squeezelite's decode thread has PTHREAD_STACK_MIN + 14 KB */
#define YIELD_US 50000 /* busy this long without a sleep: one tick for the idle task (watchdog) */
#define DUTY_BINS 101  /* per-second CPU share, 1% bins */

static uint8_t *clip; /* PSRAM copy */
static size_t clip_len, pos;

static StackType_t *psram_stack;
static StaticTask_t tcb;
static TaskHandle_t worker;
static volatile bool run, alive;
static uint32_t my_run;

/* the run's parameters */
static bool want_psram, on_psram;
static int lead_ms, core;

/* what it measured */
static bool started;
static uint32_t rate, bits, channels, blocksize_max;
static uint64_t samples; /* per channel, decoded this run */
static uint32_t passes, frames, errors, late, late_ms, worst_frame_us, stack_free_min;
static int64_t busy_us, t0_us, t_end_us;
static uint32_t heap_start, heap_min;
static int64_t cpu_us; /* the task's own run time this run */
typedef struct {
	uint32_t bins[DUTY_BINS], max, n;
} duty_t;
static duty_t duty_cpu, duty_wall;
static char err_run[64];

static uint32_t heap_now(void) { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }

static FLAC__StreamDecoderReadStatus rd(const FLAC__StreamDecoder *d, FLAC__byte buf[], size_t *n, void *u) {
	(void)d;
	(void)u;
	size_t left = clip_len - pos;
	if (!left) {
		*n = 0;
		return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
	}
	if (*n > left) *n = left;
	memcpy(buf, clip + pos, *n);
	pos += *n;
	return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

static volatile int32_t sink; /* keeps the samples "used" */

static FLAC__StreamDecoderWriteStatus wr(const FLAC__StreamDecoder *d, const FLAC__Frame *f,
					 const FLAC__int32 *const buf[], void *u) {
	(void)d;
	(void)u;
	samples += f->header.blocksize;
	frames++;
	sink = buf[0][f->header.blocksize - 1];
	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void md(const FLAC__StreamDecoder *d, const FLAC__StreamMetadata *m, void *u) {
	(void)d;
	(void)u;
	if (m->type != FLAC__METADATA_TYPE_STREAMINFO) return;
	rate = m->data.stream_info.sample_rate;
	bits = m->data.stream_info.bits_per_sample;
	channels = m->data.stream_info.channels;
	blocksize_max = m->data.stream_info.max_blocksize;
}

static void er(const FLAC__StreamDecoder *d, FLAC__StreamDecoderErrorStatus s, void *u) {
	(void)d;
	(void)u;
	errors++;
	if (!err_run[0]) snprintf(err_run, sizeof(err_run), "decode: %s", FLAC__StreamDecoderErrorStatusString[s]);
}

static void duty_add(duty_t *t, uint32_t pct) {
	if (pct > 100) pct = 100;
	t->bins[pct]++;
	t->n++;
	if (pct > t->max) t->max = pct;
}

/* this task's run time so far, in esp_timer microseconds (up to its last
 * switch in; a uint32 that wraps at 71 min, so only differences are used) */
static uint32_t run_time_now(void) {
	TaskStatus_t st;
	vTaskGetInfo(NULL, &st, pdFALSE, eRunning);
	return st.ulRunTimeCounter;
}

static void decode_run(void) {
	FLAC__StreamDecoder *d = FLAC__stream_decoder_new();
	if (!d || FLAC__stream_decoder_init_stream(d, rd, NULL, NULL, NULL, NULL, wr, md, er, NULL) !=
			  FLAC__STREAM_DECODER_INIT_STATUS_OK) {
		snprintf(err_run, sizeof(err_run), "the decoder didn't start (no memory?)");
		if (d) FLAC__stream_decoder_delete(d);
		return;
	}
	pos = 0;
	t0_us = esp_timer_get_time();
	uint32_t rt0 = run_time_now(), sec_rt = rt0;
	int64_t sec_start = t0_us, sec_busy = 0, since_sleep = 0;
	bool behind = false, slept = false;
	uint32_t spins = 0;
	int64_t behind_since = 0;
	while (run) {
		int64_t now = esp_timer_get_time();
		/* the audio decoded so far, against the wall clock */
		int64_t audio_us = rate ? (int64_t) (samples * 1000000ull / rate) : 0;
		int64_t ahead = audio_us - (now - t0_us);
		if (rate && samples) {
			if (ahead < 0 && !behind) {
				behind = true;
				behind_since = now;
				late++;
			} else if (ahead >= 0 && behind) {
				behind = false;
				late_ms += (uint32_t) ((now - behind_since) / 1000);
			}
		}
		if (slept && now - sec_start >= 1000000) {
			uint32_t rt = run_time_now();
			duty_add(&duty_cpu, (uint32_t) ((uint64_t) (uint32_t) (rt - sec_rt) * 100 / (now - sec_start)));
			duty_add(&duty_wall, (uint32_t) (sec_busy * 100 / (now - sec_start)));
			sec_start = now;
			sec_rt = rt;
			sec_busy = 0;
		}
		if (rate && ahead > (int64_t) lead_ms * 1000) {
			/* ahead enough: sleep until a quarter of the lead is used */
			TickType_t t = pdMS_TO_TICKS(lead_ms / 4);
			vTaskDelay(t ? t : 1);
			since_sleep = 0;
			if (!slept) {
				/* the lead is built: the per-second shares start here */
				slept = true;
				sec_start = esp_timer_get_time();
				sec_rt = run_time_now();
				sec_busy = 0;
			}
			continue;
		}
		if (since_sleep > YIELD_US) {
			vTaskDelay(1);
			since_sleep = 0;
		}
		int64_t a = esp_timer_get_time();
		bool ok = FLAC__stream_decoder_process_single(d);
		int64_t took = esp_timer_get_time() - a;
		busy_us += took;
		sec_busy += took;
		since_sleep += took;
		if (took > worst_frame_us) worst_frame_us = (uint32_t) took;
		uint32_t h = heap_now();
		if (h < heap_min) heap_min = h;
		FLAC__StreamDecoderState st = FLAC__stream_decoder_get_state(d);
		if (st == FLAC__STREAM_DECODER_END_OF_STREAM) {
			passes++;
			pos = 0;
			if (!FLAC__stream_decoder_reset(d)) {
				snprintf(err_run, sizeof(err_run), "the decoder didn't reset");
				break;
			}
		} else if (!ok || st > FLAC__STREAM_DECODER_END_OF_STREAM) {
			if (!err_run[0]) snprintf(err_run, sizeof(err_run), "decoder: %s", FLAC__StreamDecoderStateString[st]);
			break;
		}
		if (!(++spins & 31)) cpu_us = (uint32_t) (run_time_now() - rt0); /* for a result read mid-run */
	}
	if (behind) late_ms += (uint32_t) ((esp_timer_get_time() - behind_since) / 1000);
	t_end_us = esp_timer_get_time();
	cpu_us = (uint32_t) (run_time_now() - rt0);
	FLAC__stream_decoder_delete(d);
}

static void work(void) {
	uint32_t this_run = my_run; /* a start after alive goes false sets the next */
	decode_run();
	bool ended_itself = run; /* not stopped: an error ended it */
	stack_free_min = uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);
	ESP_LOGI(TAG, "stopped: %u passes, %u errors, busy %lld ms, late %u", (unsigned) passes, (unsigned) errors,
		 (long long) (busy_us / 1000), (unsigned) late);
	alive = false;
	if (ended_itself) hifi_lab_esp_finished("flac", this_run, false);
}

/* the PSRAM-stack worker stays, waiting for the next run */
static void psram_main(void *arg) {
	(void)arg;
	for (;;) {
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		work();
	}
}

/* the internal-stack one ends with its run, giving its stack back */
static void internal_main(void *arg) {
	(void)arg;
	work();
	vTaskDelete(NULL);
}

static int start(uint32_t run_no, const char *body, char *err, size_t n) {
	if (alive) {
		snprintf(err, n, "already running");
		return 409;
	}
	char s[12] = "psram";
	hj_get_str(body, "stack", s, sizeof(s));
	bool psram;
	if (!strcmp(s, "psram")) psram = true;
	else if (!strcmp(s, "internal")) psram = false;
	else {
		snprintf(err, n, "stack must be psram or internal");
		return 400;
	}
	double v = 500;
	if (hj_num_bad(body, "lead_ms") || (hj_get_num(body, "lead_ms", &v) && !(v >= 50 && v <= 2000))) {
		snprintf(err, n, "lead_ms must be 50 to 2000");
		return 400;
	}
	esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
	int want_core = cfg.pin_to_core;
	double c;
	if (hj_num_bad(body, "core") || (hj_get_num(body, "core", &c) && !(c == 0 || c == 1))) {
		snprintf(err, n, "core must be 0 or 1");
		return 400;
	}
	if (hj_get_num(body, "core", &c)) want_core = (int) c;
	if (!clip) {
		clip_len = (size_t) (clip_end - clip_start);
		clip = heap_caps_malloc(clip_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
		if (!clip) {
			snprintf(err, n, "no PSRAM for the clip (%u B)", (unsigned) clip_len);
			return 503;
		}
		memcpy(clip, clip_start, clip_len);
	}
	if (!psram && heap_now() < STACK + 512 + HIFI_LAB_HEAP_FLOOR) {
		snprintf(err, n, "an internal %u B stack would leave under %u B of internal RAM (free %u B)", (unsigned) STACK,
			 (unsigned) HIFI_LAB_HEAP_FLOOR, (unsigned) heap_now());
		return 503;
	}
	if (psram && !psram_stack && !(psram_stack = heap_caps_malloc(STACK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT))) {
		snprintf(err, n, "no PSRAM for the stack");
		return 503;
	}
	/* the internal worker of the last run may not be cleaned up yet: its
	 * own memory, freed by the idle task; a new one is a new task */
	want_psram = psram;
	lead_ms = (int) v;
	core = want_core == tskNO_AFFINITY ? -1 : want_core;
	my_run = run_no;
	rate = bits = channels = blocksize_max = 0;
	samples = 0;
	passes = frames = errors = late = late_ms = worst_frame_us = stack_free_min = 0;
	busy_us = cpu_us = t0_us = t_end_us = 0;
	memset(&duty_cpu, 0, sizeof(duty_cpu));
	memset(&duty_wall, 0, sizeof(duty_wall));
	err_run[0] = 0;
	heap_start = heap_min = heap_now();
	run = alive = true;
	if (psram) {
		/* the PSRAM worker's core is fixed when it is made (the first run) */
		if (!worker) {
			worker = xTaskCreateStaticPinnedToCore(psram_main, "lab_flac", STACK, NULL, cfg.prio, psram_stack, &tcb,
							       want_core);
			if (!worker) {
				run = alive = false;
				snprintf(err, n, "no worker task");
				return 503;
			}
		}
		core = xTaskGetAffinity(worker) == tskNO_AFFINITY ? -1 : (int) xTaskGetAffinity(worker);
		on_psram = true;
		xTaskNotifyGive(worker);
	} else {
		if (xTaskCreatePinnedToCore(internal_main, "lab_flac_i", STACK, NULL, cfg.prio, NULL, want_core) != pdPASS) {
			run = alive = false;
			snprintf(err, n, "no internal RAM for a %u B stack", (unsigned) STACK);
			return 503;
		}
		on_psram = false;
	}
	started = true;
	ESP_LOGI(TAG, "decoding %u B of FLAC, stack in %s, core %d, lead %d ms", (unsigned) clip_len,
		 psram ? "PSRAM" : "internal RAM", core, lead_ms);
	return 0;
}

static bool stop(void) {
	run = false;
	for (int i = 0; i < 300 && alive; i++) vTaskDelay(pdMS_TO_TICKS(10));
	if (alive) ESP_LOGE(TAG, "the task didn't stop in 3 s");
	return !alive;
}

static uint32_t duty_pct(const duty_t *t, uint32_t p) {
	if (!t->n) return 0;
	uint32_t want = (uint32_t) (((uint64_t) p * t->n + 99) / 100), seen = 0;
	for (uint32_t i = 0; i < DUTY_BINS; i++)
		if ((seen += t->bins[i]) >= want) return i;
	return DUTY_BINS - 1;
}

static void duty_json(hj_t *j, const char *name, const duty_t *t) {
	hj_obj(j, name);
	hj_int(j, "n", t->n);
	if (!t->n) { hj_obj_end(j); return; }   // no second ended in a sleep (the decoder never got ahead): no shares, not 0%
	hj_int(j, "p50", duty_pct(t, 50));
	hj_int(j, "p99", duty_pct(t, 99));
	hj_int(j, "max", t->max);
	hj_obj_end(j);
}

static void result(void *hj) {
	hj_t *j = hj;
	if (!started) return;
	hj_str(j, "stack", on_psram ? "psram" : "internal");
	hj_int(j, "stack_bytes", STACK);
	if (!alive) {
		hj_int(j, "stack_free_min", stack_free_min);
		hj_str(j, "stack_free_min_over", on_psram ? "every run since boot" : "this run");
	}
	hj_int(j, "core", core);
	hj_int(j, "lead_ms", lead_ms);
	hj_int(j, "clip_bytes", clip_len);
	hj_int(j, "rate", rate);
	hj_int(j, "bits", bits);
	hj_int(j, "channels", channels);
	hj_int(j, "blocksize_max", blocksize_max);
	int64_t end = alive ? esp_timer_get_time() : t_end_us;
	double wall_s = t0_us ? (end - t0_us) / 1e6 : 0;
	double audio_s = rate ? (double) samples / rate : 0;
	hj_num(j, "wall_s", wall_s, 1);
	hj_num(j, "audio_s", audio_s, 1);
	hj_int(j, "passes", passes);
	hj_int(j, "frames", frames);
	/* the task's own CPU time, and the wall time inside the decoder
	 * (preemption included); speed_x: how many times faster than real time
	 * it decodes on the CPU it gets */
	hj_int(j, "cpu_ms", cpu_us / 1000);
	hj_int(j, "busy_ms", busy_us / 1000);
	hj_num(j, "speed_x", cpu_us ? audio_s / (cpu_us / 1e6) : 0, 2);
	hj_num(j, "cpu_pct", wall_s > 0 ? cpu_us / 1e4 / wall_s : 0, 1);
	hj_num(j, "busy_pct", wall_s > 0 ? busy_us / 1e4 / wall_s : 0, 1);
	duty_json(j, "cpu_pct_per_s", &duty_cpu);
	duty_json(j, "busy_pct_per_s", &duty_wall);
	hj_int(j, "worst_frame_us", worst_frame_us);
	hj_int(j, "late", late);
	hj_int(j, "late_ms", late_ms);
	hj_int(j, "errors", errors);
	if (err_run[0]) hj_str(j, "error", err_run);
	hj_obj(j, "heap");
	hj_int(j, "start", heap_start);
	hj_int(j, "min", heap_min);
	hj_obj_end(j);
	hj_bool(j, "active", alive);
}

const hifi_lab_exp_t lab_flac = {
	.name = "flac",
	.help = "96/24 FLAC decoded at real time on a PSRAM or internal stack: stack psram|internal, lead_ms, core "
		"(the PSRAM worker's stack_free_min is over every run since boot)",
	.start = start,
	.stop = stop,
	.result = result,
};
