/*
 * Lab experiment "crafted": hands the board's own audio decoders a packet or
 * a stream the request carries, as a stranger on the LAN would through
 * AirPlay (ALAC) or a stream it points the player at (MP3), and checks the
 * heap after. It is how security.md's decoder fixes are shown on a board: a
 * packet that overran the heap before a fix (the board restarts, and the
 * lab names "crafted" as the experiment it went down in) is refused after it.
 *
 *   api lab "{\"op\":\"start\",\"exp\":\"crafted\",\"codec\":\"alac\",\"data\":\"<base64>\"}"
 *   api lab "{\"op\":\"result\",\"exp\":\"crafted\"}"
 *
 * codec: "alac" or "mp3". data: base64, at most DATA_MAX bytes. repeat
 * (1-64, default 1): the data is fed that many times (ALAC: one packet per
 * call; MP3: one stream of the copies end to end, so a crafted frame can
 * build on the one before it).
 * A bench command is at most 512 characters, so data longer than about 300
 * bytes goes in pieces: each piece but the last says "more":true and only
 * stores its bytes (the run ends "done" by itself, with "staged" in its
 * result, after the same 250 ms wait and heap checks as a decode: wait for
 * it before the next piece, which gets a 409 till then), and "offset" says
 * where a piece goes, which must be 0 (a fresh start) or the number of
 * bytes staged so far. Staged pieces must leave room for the last one
 * (they hold at most DATA_MAX - 1 bytes). The last piece, without "more",
 * decodes everything staged plus itself, with its own codec, repeat and
 * stream parameters (the earlier pieces' are only checked):
 *
 *   api lab "{\"op\":\"start\",\"exp\":\"crafted\",\"codec\":\"mp3\",\"more\":true,\"data\":\"<300 B>\"}"
 *   api lab "{\"op\":\"start\",\"exp\":\"crafted\",\"codec\":\"mp3\",\"offset\":300,\"data\":\"<the rest>\"}"
 *
 * A decode, a piece at offset 0 or a piece whose base64 fails (it may have
 * written over the staged bytes) drops what was staged. A piece turned down
 * before that (a bad field, a 409 or a 503) keeps it, so it can be resent.
 * ALAC's stream parameters are AirPlay's (frame 352, 16 bits, 2 channels,
 * 44100 Hz) unless frame, bits and channels say otherwise. The packet goes
 * in with its length (alac_to_pcm_len, as rtp.c's alac_decode calls it) on
 * an image that has the bounds fix; an image without it has only
 * alac_to_pcm, which reads a whole PCM block's worth, so the input buffer is
 * that size, zero past the data, as rtp.c's is.
 *
 * The decode runs on one worker task with a PSRAM stack, as the player's
 * does, made at the first start and woken for each run after; a run is over
 * in milliseconds and ends "done" by itself. The result: calls, the ones the
 * decoder took (ok) and refused (refused), the frames the taken ones gave
 * (a refused ALAC packet's silence is not counted), and heap_ok_before /
 * heap_ok_after from heap_caps_check_integrity_all. The heap check sees a
 * broken block header or free list; heap poisoning is off in these images,
 * so an overrun that lands inside live data shows as a restart (or
 * nothing), not as heap_ok_after false.
 */
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "alac_wrapper.h"
#include "mad.h"

/* weak, so an image without the decoders (rtp.c, mad.c) still links and a
 * start there answers 501. This file itself is in the app only (weak in
 * hifi_lab_esp.h, -u from patch 0035): in recovery the pragmas alone did not
 * keep libmad and ALAC out of the slot. */
#pragma weak alac_create_decoder
#pragma weak alac_delete_decoder
#pragma weak alac_to_pcm
#pragma weak alac_to_pcm_len
#pragma weak mad_stream_init
#pragma weak mad_stream_finish
#pragma weak mad_stream_buffer
#pragma weak mad_stream_errorstr
#pragma weak mad_frame_init
#pragma weak mad_frame_finish
#pragma weak mad_frame_decode

#include "hifi_b64.h"
#include "hifi_json.h"
#include "hifi_lab_esp.h"

static const char *TAG = "lab_crafted";

#define STACK 16384
#define DATA_MAX 4096
#define REPEAT_MAX 64

static volatile bool alive, go; /* alive: a run is on the worker; go: keep decoding */
static TaskHandle_t worker;
static StaticTask_t worker_tcb;
static uint32_t run_no;
static bool is_mp3;
static uint8_t *data;
static size_t data_n, staged_n; /* staged_n: bytes stored by "more" pieces, waiting for the last one */
static bool stage_only;
static uint32_t repeat, frame_len, bits, channels;
static volatile uint32_t calls, ok, refused, frames_out;
static volatile bool heap_before, heap_after, finished;
static char why[48];

static uint32_t num(const char *body, const char *k, uint32_t dflt, uint32_t lo, uint32_t hi, bool *bad) {
	double v;
	if (!hj_get_num(body, k, &v)) {
		/* present but not a number ("300", null, true) is an error, not the
		 * default: a quoted offset read as 0 would start the data over */
		const char *r;
		size_t rn;
		if (hj_num_bad(body, k) || hj_get_raw(body, k, &r, &rn)) *bad = true;
		return dflt;
	}
	if (!(v >= lo && v <= hi) || v != (uint32_t) v) {
		*bad = true;
		return dflt;
	}
	return (uint32_t) v;
}

static void run_alac(void) {
	/* rtp.c's ALACSpecificConfig, big-endian, as the ANNOUNCE's fmtp builds it */
	uint8_t c[24] = {0};
	c[0] = frame_len >> 24, c[1] = frame_len >> 16, c[2] = frame_len >> 8, c[3] = frame_len;
	c[5] = bits;
	c[6] = 40, c[7] = 10, c[8] = 14; /* pb, mb, kb */
	c[9] = channels;
	c[10] = 0, c[11] = 255; /* maxRun */
	c[20] = 0, c[21] = 0, c[22] = 0xac, c[23] = 0x44; /* 44100 */
	unsigned char sample_size, ch;
	unsigned rate, block;
	struct alac_codec_s *a = alac_create_decoder(sizeof(c), c, &sample_size, &rate, &ch, &block);
	if (!a) {
		snprintf(why, sizeof(why), "no decoder for these parameters");
		return;
	}
	size_t in_n = block > data_n ? block : data_n;
	uint8_t *in = calloc(1, in_n + 64);
	uint8_t *out = calloc(1, block);
	if (!in || !out) {
		snprintf(why, sizeof(why), "out of memory");
	} else {
		memcpy(in, data, data_n);
		for (uint32_t i = 0; i < repeat && go; i++) {
			unsigned f = 0;
			calls++;
			bool took = alac_to_pcm_len ? alac_to_pcm_len(a, in, data_n, out, (char) channels, &f)
						    : alac_to_pcm(a, in, out, (char) channels, &f);
			if (took) {
				ok++;
				frames_out += f;
			} else {
				refused++;
			}
		}
	}
	free(in);
	free(out);
	alac_delete_decoder(a);
}

static void run_mp3(void) {
	size_t n = data_n * repeat;
	uint8_t *buf = calloc(1, n + MAD_BUFFER_GUARD);
	struct mad_stream *s = calloc(1, sizeof(*s));
	struct mad_frame *fr = calloc(1, sizeof(*fr));
	if (!buf || !s || !fr) {
		snprintf(why, sizeof(why), "out of memory");
		goto out;
	}
	for (uint32_t i = 0; i < repeat; i++) memcpy(buf + i * data_n, data, data_n);
	mad_stream_init(s);
	mad_frame_init(fr);
	mad_stream_buffer(s, buf, n + MAD_BUFFER_GUARD);
	/* squeezelite's mad.c: a recoverable error skips on, anything else ends it */
	for (int guard = 0; guard < 4096 && go; guard++) {
		calls++;
		if (mad_frame_decode(fr, s) == 0) {
			ok++;
			frames_out += 32 * MAD_NSBSAMPLES(&fr->header);
			continue;
		}
		refused++;
		if (!MAD_RECOVERABLE(s->error)) {
			if (s->error != MAD_ERROR_BUFLEN) snprintf(why, sizeof(why), "%s", mad_stream_errorstr(s));
			break;
		}
	}
	mad_frame_finish(fr);
	mad_stream_finish(s);
out:
	free(fr);
	free(s);
	free(buf);
}

static void task_main(void *arg) {
	for (;;) {
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		/* the lab keeps the running experiment's name (RTC memory, for the
		 * restart's "crashed_in") only once start() has returned: a packet
		 * that panics the board at once must not beat it there */
		vTaskDelay(pdMS_TO_TICKS(250));
		heap_before = heap_caps_check_integrity_all(true);
		if (!stage_only)
			ESP_LOGW(TAG, "%s: %u bytes x %u", is_mp3 ? "mp3" : "alac", (unsigned) data_n, (unsigned) repeat);
		if (go && !stage_only) {
			if (is_mp3) run_mp3();
			else run_alac();
		}
		heap_after = heap_caps_check_integrity_all(true);
		ESP_LOGW(TAG, "%u calls, %u ok, %u refused, heap %s", (unsigned) calls, (unsigned) ok,
			 (unsigned) refused, heap_after ? "intact" : "CORRUPT");
		finished = go;
		uint32_t r = run_no;
		bool was_go = go;
		go = false;
		if (was_go) hifi_lab_esp_finished("crafted", r, heap_after);
		alive = false;
	}
}

static int start(uint32_t run, const char *body, char *err, size_t n) {
	if (alive) {
		snprintf(err, n, "the last run is still decoding");
		return 409;
	}
	char codec[8];
	if (!hj_get_str(body, "codec", codec, sizeof(codec)) || (strcmp(codec, "alac") && strcmp(codec, "mp3"))) {
		snprintf(err, n, "codec: alac or mp3");
		return 400;
	}
	if (strcmp(codec, "mp3") ? !alac_create_decoder || !alac_to_pcm || !alac_delete_decoder
				 : !mad_stream_init || !mad_stream_buffer || !mad_frame_init || !mad_frame_decode ||
				       !mad_frame_finish || !mad_stream_finish || !mad_stream_errorstr) {
		snprintf(err, n, "this image has no %s decoder", codec);
		return 501;
	}
	bool bad = false;
	uint32_t rep = num(body, "repeat", 1, 1, REPEAT_MAX, &bad);
	uint32_t fl = num(body, "frame", 352, 1, 4096, &bad);
	uint32_t bt = num(body, "bits", 16, 16, 32, &bad);
	uint32_t chn = num(body, "channels", 2, 1, 2, &bad);
	if (bad) {
		snprintf(err, n, "repeat 1-%d, frame 1-4096, bits 16-32, channels 1-2", REPEAT_MAX);
		return 400;
	}
	uint32_t off = num(body, "offset", 0, 0, DATA_MAX - 1, &bad);
	bool more = false;
	const char *mv;
	size_t mv_n;
	if (bad || (hj_get_raw(body, "more", &mv, &mv_n) && !hj_get_bool(body, "more", &more))) {
		snprintf(err, n, "offset 0-%d, more true or false", DATA_MAX - 1);
		return 400;
	}
	if (off && off != staged_n) {
		snprintf(err, n, "offset must be 0 or %u (the bytes staged)", (unsigned) staged_n);
		return 400;
	}
	const char *b64;
	size_t b64_n;
	if (!hj_get_str_span(body, "data", &b64, &b64_n) || !b64_n) {
		snprintf(err, n, "data: base64");
		return 400;
	}
	/* everything that can fail with 503 comes before the decode into data:
	 * after it, only a 400 can turn the run down (and data_n, the result's
	 * "bytes", stays the last run's then) */
	if (!data && !(data = malloc(DATA_MAX))) {
		snprintf(err, n, "out of memory");
		return 503;
	}
	if (!worker) {
		/* the stack lives as long as the worker (PSRAM, as the player's) */
		StackType_t *stack = heap_caps_malloc(STACK, MALLOC_CAP_SPIRAM);
		if (!stack) {
			snprintf(err, n, "out of PSRAM");
			return 503;
		}
		worker = xTaskCreateStaticPinnedToCore(task_main, "lab_crafted", STACK, NULL, ESP_TASK_PRIO_MIN + 5,
						       stack, &worker_tcb, 1);
		if (!worker) {
			free(stack);
			snprintf(err, n, "no task");
			return 503;
		}
	}
	long got = hifi_b64_decode(b64, b64_n, data + off, DATA_MAX - off);
	if (got <= 0) {
		staged_n = 0; /* the piece may have written over what was staged */
		snprintf(err, n, "data: base64 of 1-%u bytes", (unsigned) (DATA_MAX - off));
		return 400;
	}
	if (more && off + got >= DATA_MAX) {
		staged_n = 0;
		snprintf(err, n, "staged pieces hold at most %d bytes, so the last one has room", DATA_MAX - 1);
		return 400;
	}
	data_n = off + got;
	stage_only = more;
	staged_n = more ? data_n : 0;
	is_mp3 = !strcmp(codec, "mp3");
	repeat = rep, frame_len = fl, bits = bt, channels = chn;
	calls = ok = refused = frames_out = 0;
	heap_before = heap_after = finished = false;
	why[0] = 0;
	run_no = run;
	alive = go = true;
	xTaskNotifyGive(worker);
	return 0;
}

static bool stop(void) {
	go = false; /* the loops look at it between calls */
	for (int i = 0; i < 300 && alive; i++) vTaskDelay(pdMS_TO_TICKS(10));
	return !alive;
}

static void result(void *hj) {
	hj_t *j = hj;
	hj_str(j, "codec", is_mp3 ? "mp3" : "alac");
	hj_int(j, "bytes", data_n);
	if (stage_only && staged_n) hj_bool(j, "staged", true);
	hj_int(j, "repeat", repeat);
	hj_bool(j, "finished", finished);
	hj_int(j, "calls", calls);
	hj_int(j, "ok", ok);
	hj_int(j, "refused", refused);
	hj_int(j, "frames", frames_out);
	hj_bool(j, "heap_ok_before", heap_before);
	hj_bool(j, "heap_ok_after", heap_after);
	if (why[0]) hj_str(j, "why", why);
}

const hifi_lab_exp_t lab_crafted = {
	.name = "crafted",
	.help = "feeds a crafted packet or stream to the board's decoders: codec (alac|mp3), data (base64), repeat; "
		"long data in pieces: more:true stores a piece, offset = bytes staged so far; "
		"heap_ok_after sees a broken heap header, not every overrun (no heap poisoning)",
	.start = start,
	.stop = stop,
	.result = result,
};
