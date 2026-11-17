/*
 * Lab experiment "a2dp": a real Bluetooth audio stream between two lab
 * boards, to see what the other radios (ESP-NOW, a BLE link) do to it.
 *
 *   api lab "{\"op\":\"start\",\"exp\":\"a2dp\",\"role\":\"sink\",\"max_s\":300}"
 *   api lab "{\"op\":\"start\",\"exp\":\"a2dp\",\"role\":\"source\",\"peer\":\"30:76:f5:2d:73:e2\",\"max_s\":300}"
 *
 * role "sink": the receiver's own A2DP sink, as it is. Nothing is sent or
 * changed: the run only counts what arrives (packets, bytes, the gaps
 * between them, bytes the output buffer couldn't take) and times the output's
 * dry spells (running with nothing to play: an underrun the player's own
 * count leaves out for Bluetooth, where a pause plays silence too). Spells
 * count only once the output has played since the start, so a stream that
 * hasn't begun isn't one long dropout. The result names the board's
 * Bluetooth address, for the source's peer.
 *
 * role "source": the board stops being a sink for the run and streams a
 * tone (441 Hz, -12 dBFS, 44.1 kHz stereo) to the peer's sink. Its A2DP
 * sink is taken down first (Bluedroid has one A2DP role at a time; a phone
 * connected to this board is dropped), then the source is set up, connects
 * and starts. The stop suspends the stream, disconnects, takes the source
 * down, sets the sink up again as the receiver had it (patch 0018's
 * hifi_bt_sink_rearm) and forgets the peer's pairing on this board. A link
 * that drops stays dropped: the run keeps its counts until its stop, which
 * cleans up the same way. Refused while Bluetooth plays here.
 *
 * Only Bluetooth between the two boards: no Wi-Fi traffic and no ports. The
 * peer's own pairing record of this board stays in its NVS (one entry).
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_a2dp_api.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "hifi_json.h"
#include "hifi_lab_esp.h"

static const char *TAG = "lab_a2dp";

/* Weak, as in lab_bt.c: the recovery image has no Bluetooth host, and a
 * strong reference would pull Bluedroid into it. The app has them all from
 * the sink (and libbt's source half, which only this file reaches). */
#pragma weak esp_bluedroid_get_status
#pragma weak esp_bt_dev_get_address
#pragma weak esp_bt_gap_remove_bond_device
#pragma weak esp_a2d_register_callback
#pragma weak esp_a2d_sink_deinit
#pragma weak esp_a2d_source_init
#pragma weak esp_a2d_source_deinit
#pragma weak esp_a2d_source_register_data_callback
#pragma weak esp_a2d_source_connect
#pragma weak esp_a2d_source_disconnect
#pragma weak esp_a2d_media_ctrl

extern int hifi_sink_source(void) __attribute__((weak));       /* 2: Bluetooth plays (decode_external.c) */
extern esp_err_t hifi_bt_sink_rearm(void) __attribute__((weak)); /* bt_app_sink.c, patch 0018 */

typedef enum { ROLE_NONE, ROLE_SINK, ROLE_SOURCE } role_t;

static role_t role;
static bool started, active;
static int64_t t_start;

/* --- sink: the counts the hooks in decode_external.c and output.c feed --- */

static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool armed;
static struct {
	uint32_t packets, gaps50, gaps100, gaps250, dropped;
	uint64_t bytes;
	int64_t first_rx, last_rx, max_gap;
	bool played, dry;
	int64_t dry_since;
	uint32_t spells, spells20;
	int64_t dry_us, dry_max;
} k;

/* the Bluetooth sink's data, from its task: len bytes came, written went
 * into the output buffer (the rest it had no room for in 10 ms) */
void hifi_lab_a2dp_rx(uint32_t len, uint32_t written) {
	if (!armed) return;
	int64_t now = esp_timer_get_time();
	portENTER_CRITICAL(&mux);
	if (k.last_rx) {
		int64_t gap = now - k.last_rx;
		if (gap > k.max_gap) k.max_gap = gap;
		if (gap > 50000) k.gaps50++;
		if (gap > 100000) k.gaps100++;
		if (gap > 250000) k.gaps250++;
	} else k.first_rx = now;
	k.last_rx = now;
	k.packets++;
	k.bytes += len;
	if (written < len) k.dropped += len - written;
	portEXIT_CRITICAL(&mux);
}

static void dry_end(int64_t now) {
	int64_t d = now - k.dry_since;
	k.dry = false;
	k.dry_us += d;
	if (d > k.dry_max) k.dry_max = d;
	if (d >= 20000) k.spells20++;
}

/* the output thread, each time it takes frames while Bluetooth is the
 * source: running with none to take is a dry spell */
void hifi_lab_a2dp_out(bool running, uint32_t frames) {
	if (!armed) return;
	int64_t now = esp_timer_get_time();
	portENTER_CRITICAL(&mux);
	if (running && frames) {
		k.played = true;
		if (k.dry) dry_end(now);
	} else if (running && k.played && !k.dry) {
		k.dry = true;
		k.dry_since = now;
		k.spells++;
	} else if (!running && k.dry)
		dry_end(now);
	portEXIT_CRITICAL(&mux);
}

/* --- source: a tone to the peer --- */

#define EV_INIT BIT0
#define EV_DEINIT BIT1
static EventGroupHandle_t ev;

typedef enum { SRC_DOWN, SRC_CONNECTING, SRC_CONNECTED, SRC_STARTING, SRC_STREAMING, SRC_LOST } src_state_t;
static const char *const src_names[] = {"down", "connecting", "connected", "starting", "streaming", "lost"};

static esp_bd_addr_t peer;
static volatile src_state_t sst;
static volatile bool stopping, src_up, stop_pending;
static volatile uint32_t cb_calls, cb_gaps50, cb_gaps100, disconnects, acks_failed;
static volatile uint64_t cb_bytes;
static volatile int64_t cb_last, cb_max_gap, t_connect, t_connected, t_stream;
static uint32_t connect_ms;
static char src_err[48];
static const char *sink_back; /* the stop's hand-back: "yes" or the error */
static char sink_back_err[32];

/* one period of 441 Hz at 44.1 kHz: 100 frames, 16-bit stereo */
#define TONE_FRAMES 100
static int16_t tone[TONE_FRAMES * 2];
static size_t tone_pos; /* bytes into the table */

static int32_t tone_cb(uint8_t *buf, int32_t len) {
	if (len < 0 || !buf) return 0; /* the source flushing */
	int64_t now = esp_timer_get_time();
	if (cb_last) {
		int64_t gap = now - cb_last;
		if (gap > cb_max_gap) cb_max_gap = gap;
		if (gap > 50000) cb_gaps50++;
		if (gap > 100000) cb_gaps100++;
	}
	cb_last = now;
	cb_calls++;
	cb_bytes += len;
	const uint8_t *t = (const uint8_t *)tone;
	for (int32_t i = 0; i < len;) {
		size_t n = sizeof(tone) - tone_pos;
		if (n > (size_t)(len - i)) n = len - i;
		memcpy(buf + i, t + tone_pos, n);
		i += n;
		tone_pos = (tone_pos + n) % sizeof(tone);
	}
	return len;
}

static void err_once(const char *what, int v) {
	if (!src_err[0]) snprintf(src_err, sizeof(src_err), "%s %d", what, v);
	ESP_LOGW(TAG, "%s %d", what, v);
}

/* Bluedroid's BTC task: keep it short, never wait here */
static void a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *p) {
	switch (event) {
	case ESP_A2D_PROF_STATE_EVT:
		if (p->a2d_prof_stat.init_state == ESP_A2D_INIT_SUCCESS) xEventGroupSetBits(ev, EV_INIT);
		else xEventGroupSetBits(ev, EV_DEINIT);
		break;
	case ESP_A2D_CONNECTION_STATE_EVT:
		if (p->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
			t_connected = esp_timer_get_time();
			connect_ms = (uint32_t)((t_connected - t_connect) / 1000);
			sst = SRC_CONNECTED;
			if (!stopping) esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
		} else if (p->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
			if (sst != SRC_DOWN && sst != SRC_LOST) disconnects++;
			if (!stopping) {
				if (sst == SRC_CONNECTING) err_once("connect failed, reason", p->conn_stat.disc_rsn);
				else err_once("link lost, reason", p->conn_stat.disc_rsn);
			}
			if (sst == SRC_STREAMING && t_stream) t_stream = esp_timer_get_time() - t_stream; /* now a length */
			sst = stopping ? SRC_DOWN : SRC_LOST;
		}
		break;
	case ESP_A2D_AUDIO_STATE_EVT:
		if (p->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED && sst != SRC_STREAMING) {
			sst = SRC_STREAMING;
			t_stream = esp_timer_get_time();
		}
		break;
	case ESP_A2D_MEDIA_CTRL_ACK_EVT:
		if (p->media_ctrl_stat.status != ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
			acks_failed++;
			err_once("media ctrl refused, cmd", p->media_ctrl_stat.cmd);
		} else if (p->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY && !stopping) {
			sst = SRC_STARTING;
			esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
		}
		break;
	default:
		break;
	}
}

static bool wait_bit(EventBits_t bit, int ms) {
	return xEventGroupWaitBits(ev, bit, pdTRUE, pdFALSE, pdMS_TO_TICKS(ms)) & bit;
}

static bool parse_bda(const char *s, esp_bd_addr_t out) {
	unsigned b[6];
	char end;
	if (sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x%c", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &end) != 6) return false;
	for (int i = 0; i < 6; i++) out[i] = b[i];
	return true;
}

static bool bt_up(void) {
	return esp_bluedroid_get_status && esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED;
}

/* the sink back as the receiver had it; the peer's pairing forgotten here */
static void give_back(void) {
	esp_err_t e = hifi_bt_sink_rearm ? hifi_bt_sink_rearm() : ESP_ERR_NOT_SUPPORTED;
	if (e == ESP_OK) sink_back = "yes";
	else {
		snprintf(sink_back_err, sizeof(sink_back_err), "rearm: %s", esp_err_to_name(e));
		sink_back = sink_back_err;
		ESP_LOGW(TAG, "%s", sink_back_err);
	}
	esp_bt_gap_remove_bond_device(peer);
}

static int start_source(const char *body, char *err, size_t n) {
	char p[24] = "";
	hj_get_str(body, "peer", p, sizeof(p));
	if (!parse_bda(p, peer)) {
		snprintf(err, n, "peer must be the sink's Bluetooth address, aa:bb:cc:dd:ee:ff");
		return 400;
	}
	if (!esp_a2d_source_init || !esp_a2d_sink_deinit || !hifi_bt_sink_rearm) {
		snprintf(err, n, "no A2DP source or sink hand-back in this image");
		return 503;
	}
	if (!bt_up()) {
		snprintf(err, n, "Bluedroid isn't up");
		return 409;
	}
	if (hifi_sink_source && hifi_sink_source() == 2) {
		snprintf(err, n, "Bluetooth is playing here");
		return 409;
	}
	if (!ev && !(ev = xEventGroupCreate())) {
		snprintf(err, n, "no memory");
		return 503;
	}
	for (int i = 0; i < TONE_FRAMES; i++) {
		int16_t v = (int16_t)lrintf(8192.0f * sinf(2.0f * (float)M_PI * i / TONE_FRAMES)); /* -12 dBFS */
		tone[2 * i] = tone[2 * i + 1] = v;
	}
	tone_pos = 0;
	cb_calls = cb_gaps50 = cb_gaps100 = disconnects = acks_failed = 0;
	cb_bytes = 0;
	cb_last = cb_max_gap = t_connected = t_stream = 0;
	connect_ms = 0;
	src_err[0] = 0;
	sink_back = NULL;
	stopping = false;
	sst = SRC_DOWN;
	xEventGroupClearBits(ev, EV_INIT | EV_DEINIT);

	esp_err_t e;
	if ((e = esp_a2d_register_callback(a2d_cb)) != ESP_OK) {
		snprintf(err, n, "esp_a2d_register_callback: %s", esp_err_to_name(e));
		return 503;
	}
	/* Bluedroid takes a second A2DP role only once the first is gone
	 * (btc_av_init returns at once while its block is still there) */
	if ((e = esp_a2d_sink_deinit()) != ESP_OK || !wait_bit(EV_DEINIT, 3000)) {
		snprintf(err, n, "sink deinit: %s", e == ESP_OK ? "no answer in 3 s" : esp_err_to_name(e));
		if (e != ESP_OK) give_back(); /* still a sink: its callback back */
		else {
			stop_pending = true; /* may yet go down: the next start gives it back */
			active = true;
			role = ROLE_SOURCE;
			started = true;
			stopping = true;
		}
		return 503;
	}
	esp_a2d_source_register_data_callback(tone_cb);
	if ((e = esp_a2d_source_init()) != ESP_OK || !wait_bit(EV_INIT, 3000)) {
		snprintf(err, n, "source init: %s", e == ESP_OK ? "no answer in 3 s" : esp_err_to_name(e));
		if (e == ESP_OK) {
			/* half up: take it down before the sink comes back */
			if (esp_a2d_source_deinit() == ESP_OK) wait_bit(EV_DEINIT, 3000);
		}
		give_back();
		return 503;
	}
	src_up = true;
	t_connect = esp_timer_get_time();
	sst = SRC_CONNECTING;
	if ((e = esp_a2d_source_connect(peer)) != ESP_OK) err_once("esp_a2d_source_connect", e);
	return 0;
}

static bool stop(void);

static int start(uint32_t run, const char *body, char *err, size_t n) {
	(void)run;
	/* the lab forgets a run whose stop got stuck: its teardown is retried here */
	if (active && !stop()) {
		snprintf(err, n, "the last source run's teardown still hasn't finished; try again");
		return 409;
	}
	char r[8] = "";
	hj_get_str(body, "role", r, sizeof(r));
	role_t want;
	if (!strcmp(r, "sink")) want = ROLE_SINK;
	else if (!strcmp(r, "source")) want = ROLE_SOURCE;
	else {
		snprintf(err, n, "role must be sink or source");
		return 400;
	}
	t_start = esp_timer_get_time();
	if (want == ROLE_SINK) {
		armed = false;
		portENTER_CRITICAL(&mux);
		memset(&k, 0, sizeof(k));
		portEXIT_CRITICAL(&mux);
		armed = true;
	} else {
		int rc = start_source(body, err, n);
		if (rc) return rc;
	}
	role = want;
	started = active = true;
	ESP_LOGI(TAG, "%s armed", want == ROLE_SINK ? "sink" : "source");
	return 0;
}

static bool stop(void) {
	if (!active) return true;
	if (role == ROLE_SINK) {
		armed = false;
		portENTER_CRITICAL(&mux);
		if (k.dry) dry_end(esp_timer_get_time());
		portEXIT_CRITICAL(&mux);
		active = false;
		return true;
	}
	stopping = true;
	if (stop_pending) {
		/* an earlier step didn't answer in time: see whether it has since */
		if (!(xEventGroupGetBits(ev) & EV_DEINIT)) return false;
		xEventGroupClearBits(ev, EV_DEINIT);
		stop_pending = false;
		src_up = false;
		give_back();
		active = false;
		return true;
	}
	if (src_up) {
		if (sst == SRC_STREAMING) {
			esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND);
			vTaskDelay(pdMS_TO_TICKS(200));
		}
		if (sst != SRC_DOWN && sst != SRC_LOST) {
			esp_a2d_source_disconnect(peer);
			for (int i = 0; i < 30 && sst != SRC_DOWN && sst != SRC_LOST; i++) vTaskDelay(pdMS_TO_TICKS(100));
		}
		xEventGroupClearBits(ev, EV_DEINIT);
		esp_err_t e = esp_a2d_source_deinit();
		if (e != ESP_OK || !wait_bit(EV_DEINIT, 3000)) {
			ESP_LOGW(TAG, "source deinit: %s", e == ESP_OK ? "no answer in 3 s" : esp_err_to_name(e));
			if (e == ESP_OK) {
				stop_pending = true;
				return false;
			}
			/* refused outright: the sink can't come back over it */
			snprintf(sink_back_err, sizeof(sink_back_err), "source deinit: %s", esp_err_to_name(e));
			sink_back = sink_back_err;
			active = false;
			return true;
		}
		src_up = false;
	}
	give_back();
	active = false;
	ESP_LOGI(TAG, "source down, sink back: %s", sink_back);
	return true;
}

static void result(void *hj) {
	hj_t *j = hj;
	if (!started) return;
	int64_t now = esp_timer_get_time();
	hj_str(j, "role", role == ROLE_SINK ? "sink" : "source");
	hj_bool(j, "active", active);
	if (esp_bt_dev_get_address && bt_up()) {
		const uint8_t *a = esp_bt_dev_get_address();
		char s[18];
		if (a) {
			snprintf(s, sizeof(s), "%02x:%02x:%02x:%02x:%02x:%02x", a[0], a[1], a[2], a[3], a[4], a[5]);
			hj_str(j, "bt_addr", s);
		}
	}
	if (role == ROLE_SINK) {
		typeof(k) c;
		portENTER_CRITICAL(&mux);
		c = k;
		portEXIT_CRITICAL(&mux);
		if (c.dry) { /* a spell still running counts up to now */
			int64_t d = now - c.dry_since;
			c.dry_us += d;
			if (d > c.dry_max) c.dry_max = d;
		}
		hj_bool(j, "playing", hifi_sink_source && hifi_sink_source() == 2);
		hj_int(j, "packets", c.packets);
		hj_int(j, "bytes", (int)(c.bytes > INT32_MAX ? INT32_MAX : c.bytes));
		int64_t span = c.last_rx - c.first_rx;
		hj_int(j, "kbps", span > 0 ? (int)(c.bytes * 8000 / span) : 0);
		hj_int(j, "dropped_bytes", c.dropped);
		hj_int(j, "max_gap_ms", (int)(c.max_gap / 1000));
		hj_int(j, "gaps_over_50ms", c.gaps50);
		hj_int(j, "gaps_over_100ms", c.gaps100);
		hj_int(j, "gaps_over_250ms", c.gaps250);
		hj_int(j, "since_rx_ms", c.last_rx ? (int)((now - c.last_rx) / 1000) : -1);
		hj_bool(j, "played", c.played);
		hj_int(j, "dry_spells", c.spells);
		hj_int(j, "dry_spells_20ms", c.spells20);
		hj_int(j, "dry_ms", (int)(c.dry_us / 1000));
		hj_int(j, "dry_max_ms", (int)(c.dry_max / 1000));
	} else {
		char p[18];
		snprintf(p, sizeof(p), "%02x:%02x:%02x:%02x:%02x:%02x", peer[0], peer[1], peer[2], peer[3], peer[4],
			 peer[5]);
		hj_str(j, "peer", p);
		hj_str(j, "state", src_names[sst]);
		if (stop_pending) hj_bool(j, "stop_pending", true);
		if (src_err[0]) hj_str(j, "error", src_err);
		hj_int(j, "connect_ms", t_connected ? (int)connect_ms : -1);
		int64_t streamed = sst == SRC_STREAMING ? now - t_stream : (sst == SRC_DOWN || sst == SRC_LOST) ? t_stream : 0;
		hj_int(j, "stream_ms", (int)(streamed / 1000));
		hj_int(j, "cb_calls", cb_calls);
		hj_int(j, "bytes", (int)(cb_bytes > INT32_MAX ? INT32_MAX : cb_bytes));
		hj_int(j, "kbps", streamed > 0 ? (int)(cb_bytes * 8000 / streamed) : 0);
		hj_int(j, "max_cb_gap_ms", (int)(cb_max_gap / 1000));
		hj_int(j, "cb_gaps_over_50ms", cb_gaps50);
		hj_int(j, "cb_gaps_over_100ms", cb_gaps100);
		hj_int(j, "disconnects", disconnects);
		if (acks_failed) hj_int(j, "ctrl_refused", acks_failed);
		if (sink_back) hj_str(j, "sink_back", sink_back);
	}
	hj_int(j, "run_ms", (int)((now - t_start) / 1000));
}

const hifi_lab_exp_t lab_a2dp = {
	.name = "a2dp",
	.help = "a Bluetooth audio stream between two boards: role sink (count gaps and dry output) or source "
		"(peer aa:bb:..: a 441 Hz tone to that sink; the sink comes back at the stop)",
	.start = start,
	.stop = stop,
	.result = result,
};
