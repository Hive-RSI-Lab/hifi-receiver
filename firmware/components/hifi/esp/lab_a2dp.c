/*
 * Lab experiment "a2dp": a real Bluetooth audio stream between two lab
 * boards, to see what the other radios (ESP-NOW, a BLE link) do to it.
 *
 *   api lab "{\"op\":\"start\",\"exp\":\"a2dp\",\"role\":\"sink\",\"max_s\":300}"
 *   api lab "{\"op\":\"start\",\"exp\":\"a2dp\",\"role\":\"source\",\"peer\":\"30:76:f5:2d:73:e2\",\"max_s\":300}"
 *
 * role "sink": the receiver's own A2DP sink, as it is. Nothing is sent or
 * changed: the run only counts what arrives (packets, bytes, the gaps
 * between their arrivals, bytes the output buffer had no room for, bytes the
 * sink threw away on purpose because it isn't the source that plays) and
 * times the output's dry spells (running with nothing to play: an underrun
 * the player's own count leaves out for Bluetooth, where a pause plays
 * silence too). Spells count only once the output has played since the
 * start, so a stream that hasn't begun isn't one long dropout. The gap count
 * starts over whenever the output isn't running Bluetooth; a phone's pause
 * (packets stop, the output plays silence) is one long gap, but lab runs
 * never pause. The result names the board's Bluetooth address, for the
 * source's peer.
 *
 * role "source": the board stops being a sink for the run and streams a
 * tone (-12 dBFS stereo, a 100-frame period: 441 Hz at the 44.1 kHz the
 * source asks for) to the peer's sink. Refused while a phone is connected
 * here (it would be dropped), Bluetooth plays, the bt experiment runs, or the
 * last source run couldn't give the sink back (that needs a reboot). Its A2DP sink is taken down
 * first (Bluedroid has one A2DP role at a time), the board goes
 * non-connectable and its AVRCP callbacks drop every event for the run (so
 * the peer can't change this board's volume or playback), then the source is set up,
 * connects and starts. Only the peer's A2DP events count. The stop suspends
 * the stream, disconnects, takes the source down, sets the sink up again and
 * waits for it (patch 0018's hifi_bt_sink_rearm), puts the sink's own
 * callbacks back, makes the board connectable and discoverable as an idle
 * sink is, and forgets the peer's pairing on this board (sink_back in the
 * result says how it went). A link that drops stays dropped: the run keeps
 * its counts until its stop.
 *
 * Bluedroid answers on its own task. If a role's teardown doesn't answer in
 * 2 s, the run still stops (and a failed start still fails), and the rest of
 * the hand-back runs when the answer comes, from a short task of its own;
 * sink_back says "pending" until then, and starts are refused. A teardown
 * still pending after 10 s is given up on at the next start: if Bluedroid
 * says no A2DP role is up, the hand-back runs then; if not, the start is
 * refused until a reboot. A source init that doesn't answer is taken down if
 * Bluedroid says it came up, and otherwise counted as failed. While the sink is up,
 * the lab's A2DP callback hands its events to the sink's own
 * (bt_app_a2d_cb), so a sink whose teardown never answered still works.
 *
 * Only Bluetooth between the two boards: no Wi-Fi traffic and no ports. The
 * peer's own pairing record of this board stays in its NVS (one entry).
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
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
#pragma weak esp_bt_gap_set_scan_mode
#pragma weak esp_a2d_register_callback
#pragma weak esp_a2d_sink_deinit
#pragma weak esp_a2d_source_init
#pragma weak esp_a2d_source_deinit
#pragma weak esp_a2d_source_register_data_callback
#pragma weak esp_a2d_source_connect
#pragma weak esp_a2d_source_disconnect
#pragma weak esp_a2d_media_ctrl
#pragma weak esp_avrc_ct_register_callback
#pragma weak esp_avrc_tg_register_callback

extern int hifi_sink_source(void) __attribute__((weak)); /* 2: Bluetooth plays (decode_external.c) */
/* bt_app_sink.c: the sink's own callbacks, and patch 0018's hooks */
extern void bt_app_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param) __attribute__((weak));
extern void bt_app_rc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param) __attribute__((weak));
extern void bt_app_rc_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param) __attribute__((weak));
extern esp_err_t hifi_bt_sink_rearm(void) __attribute__((weak));
extern bool hifi_bt_sink_phone(void) __attribute__((weak));
/* btc_av.c: an A2DP role is up (on_init), or none is (on_deinit); written on
 * Bluedroid's task */
extern bool g_a2dp_on_init __attribute__((weak));
extern bool g_a2dp_on_deinit __attribute__((weak));

typedef enum { ROLE_NONE, ROLE_SINK, ROLE_SOURCE } role_t;

static role_t role;
static bool started, active;
static int64_t t_start;
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

/* --- sink: the counts the hooks in decode_external.c and output.c feed --- */

static volatile bool armed;
static struct {
	uint32_t packets, gaps50, gaps100, gaps250, dropped, discarded;
	uint64_t bytes;
	int64_t first_rx, last_rx, max_gap;
	bool played, dry;
	int64_t dry_since;
	uint32_t spells, spells20;
	int64_t dry_us, dry_max;
} k;

/* the Bluetooth sink's data, from its task: len bytes came at t (taken
 * before the output buffer was tried, so its back-pressure waits don't count
 * as radio gaps), written went into the buffer; discarded: none was meant to
 * (Bluetooth isn't the source that plays, or the sink was told to drop it) */
void hifi_lab_a2dp_rx(int64_t t, uint32_t len, uint32_t written, bool discarded) {
	if (!armed) return;
	portENTER_CRITICAL(&mux);
	if (k.last_rx) {
		int64_t gap = t - k.last_rx;
		if (gap > k.max_gap) k.max_gap = gap;
		if (gap > 50000) k.gaps50++;
		if (gap > 100000) k.gaps100++;
		if (gap > 250000) k.gaps250++;
	}
	if (!k.first_rx) k.first_rx = t;
	k.last_rx = t;
	k.packets++;
	k.bytes += len;
	if (written < len) {
		if (discarded) k.discarded += len - written;
		else k.dropped += len - written;
	}
	portEXIT_CRITICAL(&mux);
}

static void dry_end(int64_t now) {
	int64_t d = now - k.dry_since;
	k.dry = false;
	k.dry_us += d;
	if (d > k.dry_max) k.dry_max = d;
	if (d >= 20000) k.spells20++;
}

/* the output thread, each time it takes frames: bt_running is the output
 * running with Bluetooth as its source; running with none to take is a dry
 * spell. Not running ends a spell, and the stream: the next packet starts
 * the gap count over. */
void hifi_lab_a2dp_out(bool bt_running, uint32_t frames) {
	if (!armed) return;
	int64_t now = esp_timer_get_time();
	portENTER_CRITICAL(&mux);
	if (bt_running && frames) {
		k.played = true;
		if (k.dry) dry_end(now);
	} else if (bt_running && k.played && !k.dry) {
		k.dry = true;
		k.dry_since = now;
		k.spells++;
	} else if (!bt_running) {
		if (k.dry) dry_end(now);
		k.last_rx = 0;
	}
	portEXIT_CRITICAL(&mux);
}

/* --- source: a tone to the peer --- */

#define EV_INIT BIT0
#define EV_DEINIT BIT1
#define WAIT_MS 2000
static EventGroupHandle_t ev;

/* who the lab's A2DP callback serves: the sink (its events to
 * bt_app_a2d_cb), the source (the peer's events), or nobody (no role up) */
typedef enum { MODE_SINK, MODE_SOURCE, MODE_NONE } a2d_mode_t;
static volatile a2d_mode_t mode = MODE_SINK;
/* under mux: a role's teardown answered (deinit_seen), or didn't in time
 * and its answer runs the rest of the hand-back (orphan) */
static bool deinit_seen, orphan;
static int64_t pending_since; /* under mux: when orphan was set */
#define PENDING_GIVE_UP_US 10000000

typedef enum { SRC_DOWN, SRC_CONNECTING, SRC_CONNECTED, SRC_STARTING, SRC_STREAMING, SRC_LOST } src_state_t;
static const char *const src_names[] = {"down", "connecting", "connected", "starting", "streaming", "lost"};

static esp_bd_addr_t peer;
static bool bonded; /* the source ran: the peer's pairing to forget */
static volatile bool stopping;
static volatile src_state_t sst; /* written on Bluedroid's task, read under mux */
static struct { /* under mux: the BTC and media tasks write, result() reads */
	uint32_t calls, gaps50, gaps100, disconnects, acks_failed;
	uint64_t bytes;
	int64_t last, max_gap, t_connect, t_connected, t_stream, stream_len;
} sc;
static char src_err[48];
static const char *volatile sink_back; /* the hand-back: "yes", "pending" or the error */
static char sink_back_err[40];
static const char SINK_PENDING[] = "pending";
/* the last hand-back failed (sink_back_err says how): the sink is down until
 * a reboot, and source starts are refused */
static volatile bool sink_lost;
static bool sink_taken; /* this start took the sink down: its result counts */

/* one period of the tone: 100 frames, 16-bit stereo */
#define TONE_FRAMES 100
static int16_t tone[TONE_FRAMES * 2];
static size_t tone_pos; /* bytes into the table */

static int32_t tone_cb(uint8_t *buf, int32_t len) {
	if (len < 0 || !buf) return 0; /* the source flushing */
	int64_t now = esp_timer_get_time();
	portENTER_CRITICAL(&mux);
	if (sc.last) {
		int64_t gap = now - sc.last;
		if (gap > sc.max_gap) sc.max_gap = gap;
		if (gap > 50000) sc.gaps50++;
		if (gap > 100000) sc.gaps100++;
	}
	sc.last = now;
	sc.calls++;
	sc.bytes += len;
	portEXIT_CRITICAL(&mux);
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

static void set_state(src_state_t s) {
	portENTER_CRITICAL(&mux);
	int64_t now = esp_timer_get_time();
	if (s == SRC_STREAMING && sst != SRC_STREAMING) sc.t_stream = now;
	if (s != SRC_STREAMING && sst == SRC_STREAMING) sc.stream_len += now - sc.t_stream;
	if (s == SRC_CONNECTED && !sc.t_connected) sc.t_connected = now;
	sst = s;
	portEXIT_CRITICAL(&mux);
}

static void give_back(void);
/* the late hand-back waits for Bluedroid (up to 2 s): not on Bluedroid's own
 * task, and not the timer task's */
static void give_back_task(void *a) {
	(void)a;
	give_back();
	vTaskDelete(NULL);
}

/* the AVRCP callbacks while the source runs: the peer's commands (play,
 * stop, its volume) are not this board's to follow */
static void rc_ct_quiet(esp_avrc_ct_cb_event_t e, esp_avrc_ct_cb_param_t *p) { (void)e, (void)p; }
static void rc_tg_quiet(esp_avrc_tg_cb_event_t e, esp_avrc_tg_cb_param_t *p) { (void)e, (void)p; }

/* Bluedroid's BTC task: keep it short, never wait here */
static void a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *p) {
	if (event == ESP_A2D_PROF_STATE_EVT) {
		if (p->a2d_prof_stat.init_state == ESP_A2D_INIT_SUCCESS) {
			xEventGroupSetBits(ev, EV_INIT);
			return;
		}
		portENTER_CRITICAL(&mux);
		deinit_seen = true;
		bool late = orphan;
		orphan = false;
		portEXIT_CRITICAL(&mux);
		xEventGroupSetBits(ev, EV_DEINIT);
		if (late) {
			/* a teardown that timed out has now answered: the hand-back
			 * waits for Bluedroid, so it runs off this task */
			mode = MODE_NONE;
			if (xTaskCreate(give_back_task, "lab_a2dp_back", 3072, NULL, 5, NULL) != pdPASS) {
				snprintf(sink_back_err, sizeof(sink_back_err), "no memory for the hand-back task");
				sink_lost = true;
				sink_back = sink_back_err;
			}
		}
		return;
	}
	if (mode == MODE_SINK) {
		if (bt_app_a2d_cb) bt_app_a2d_cb(event, p);
		return;
	}
	if (mode != MODE_SOURCE) return;
	switch (event) {
	case ESP_A2D_CONNECTION_STATE_EVT:
		if (memcmp(p->conn_stat.remote_bda, peer, sizeof(peer))) break;
		if (p->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
			set_state(SRC_CONNECTED);
			if (!stopping) esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
		} else if (p->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
			if (sst != SRC_CONNECTING && sst != SRC_DOWN && sst != SRC_LOST) {
				portENTER_CRITICAL(&mux);
				sc.disconnects++;
				portEXIT_CRITICAL(&mux);
			}
			if (!stopping) {
				if (sst == SRC_CONNECTING) err_once("connect failed, reason", p->conn_stat.disc_rsn);
				else err_once("link lost, reason", p->conn_stat.disc_rsn);
			}
			set_state(stopping ? SRC_DOWN : SRC_LOST);
		}
		break;
	case ESP_A2D_AUDIO_STATE_EVT:
		if (memcmp(p->audio_stat.remote_bda, peer, sizeof(peer))) break;
		if (p->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) set_state(SRC_STREAMING);
		else if (sst == SRC_STREAMING) set_state(SRC_CONNECTED);
		break;
	case ESP_A2D_MEDIA_CTRL_ACK_EVT:
		if (p->media_ctrl_stat.status != ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
			portENTER_CRITICAL(&mux);
			sc.acks_failed++;
			portEXIT_CRITICAL(&mux);
			err_once("media ctrl refused, cmd", p->media_ctrl_stat.cmd);
		} else if (p->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY && !stopping) {
			set_state(SRC_STARTING);
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

/* before a role's deinit call: its answer may come before the call returns */
static void deinit_arm(void) {
	portENTER_CRITICAL(&mux);
	deinit_seen = orphan = false;
	portEXIT_CRITICAL(&mux);
	xEventGroupClearBits(ev, EV_DEINIT);
}

/* true: the deinit answered. False: it hasn't, and its answer will run the
 * hand-back (sink_back says "pending" until it has) */
static bool deinit_wait(void) {
	if (wait_bit(EV_DEINIT, WAIT_MS)) return true;
	portENTER_CRITICAL(&mux);
	bool came = deinit_seen;
	if (!came) {
		orphan = true;
		pending_since = esp_timer_get_time();
		sink_back = SINK_PENDING;
	}
	portEXIT_CRITICAL(&mux);
	if (!came) ESP_LOGW(TAG, "A2DP deinit: no answer in %d ms, the sink comes back when it does", WAIT_MS);
	return came;
}

static bool pending(void) {
	portENTER_CRITICAL(&mux);
	bool p = orphan || sink_back == SINK_PENDING;
	portEXIT_CRITICAL(&mux);
	return p;
}

static bool role_up(void) { return &g_a2dp_on_init && *(volatile bool *)&g_a2dp_on_init; }
static bool none_up(void) { return &g_a2dp_on_deinit && *(volatile bool *)&g_a2dp_on_deinit; }

/* A teardown pending past 10 s whose answer Bluedroid has already dealt with
 * (no A2DP role up): the hand-back runs now. True: nothing is pending (any
 * more). Not for a teardown the hand-back task already took. */
static bool pending_give_up(void) {
	if (!pending()) return true;
	portENTER_CRITICAL(&mux);
	bool late = orphan && esp_timer_get_time() - pending_since > PENDING_GIVE_UP_US && none_up();
	if (late) orphan = false; /* the answer, if it ever comes, finds nothing to do */
	portEXIT_CRITICAL(&mux);
	if (!late) return false;
	ESP_LOGW(TAG, "A2DP teardown: no answer in 10 s and no role up: the sink comes back now");
	mode = MODE_NONE;
	give_back();
	return true;
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

static const char *back_err(const char *what, esp_err_t e) {
	snprintf(sink_back_err, sizeof(sink_back_err), "%s: %s", what, esp_err_to_name(e));
	ESP_LOGW(TAG, "sink not back: %s", sink_back_err);
	return sink_back_err;
}

/* No A2DP role is up (mode NONE): the sink back as the receiver had it,
 * waited for; its own callbacks back; connectable and discoverable, as an
 * idle sink is; the peer's pairing forgotten here. From the stop, a failed
 * start, or the timer task when a late teardown answers. */
static void give_back(void) {
	const char *how = "yes";
	xEventGroupClearBits(ev, EV_INIT);
	esp_err_t e = hifi_bt_sink_rearm();
	if (e != ESP_OK) how = back_err("rearm", e);
	else if (!wait_bit(EV_INIT, WAIT_MS)) how = back_err("rearm", ESP_ERR_TIMEOUT);
	mode = MODE_SINK;
	esp_a2d_register_callback(bt_app_a2d_cb);
	esp_avrc_ct_register_callback(bt_app_rc_ct_cb);
	esp_avrc_tg_register_callback(bt_app_rc_tg_cb);
	esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
	if (bonded) esp_bt_gap_remove_bond_device(peer);
	bonded = false;
	sink_lost = how != NULL && strcmp(how, "yes");
	sink_back = how; /* last: a start waits for it */
	ESP_LOGI(TAG, "sink back: %s", how);
}

/* The source down. True: it is, and the caller gives the sink back. False:
 * its teardown is late (the hand-back runs when it answers) or failed (the
 * source stays up and the sink can't come back: sink_back says so). */
static bool source_down(void) {
	stopping = true;
	if (sst == SRC_STREAMING) {
		esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND);
		vTaskDelay(pdMS_TO_TICKS(200));
	}
	if (sst != SRC_DOWN && sst != SRC_LOST) {
		/* connecting too: a link half up is cut (or the deinit drops it) */
		esp_a2d_source_disconnect(peer);
		for (int i = 0; i < 50 && sst != SRC_DOWN && sst != SRC_LOST; i++) vTaskDelay(pdMS_TO_TICKS(100));
		if (sst != SRC_DOWN && sst != SRC_LOST) {
			if (!src_err[0]) snprintf(src_err, sizeof(src_err), "disconnect: no answer in 5 s");
			ESP_LOGW(TAG, "disconnect: no answer in 5 s, the deinit takes it down");
		}
	}
	if (sst == SRC_STREAMING) set_state(SRC_CONNECTED); /* the stream's length ends here */
	deinit_arm();
	esp_err_t e = esp_a2d_source_deinit();
	if (e != ESP_OK) {
		sink_lost = true;
		sink_back = back_err("source deinit", e);
		return false;
	}
	if (!deinit_wait()) return false;
	mode = MODE_NONE;
	return true;
}

static int start_source(const char *body, char *err, size_t n) {
	char p[24] = "";
	hj_get_str(body, "peer", p, sizeof(p));
	if (!parse_bda(p, peer)) {
		snprintf(err, n, "peer must be the sink's Bluetooth address, aa:bb:cc:dd:ee:ff");
		return 400;
	}
	if (!esp_a2d_source_init || !esp_a2d_sink_deinit || !hifi_bt_sink_rearm || !hifi_bt_sink_phone ||
	    !bt_app_a2d_cb || !bt_app_rc_ct_cb || !bt_app_rc_tg_cb || !esp_bt_gap_remove_bond_device ||
	    !&g_a2dp_on_init || !&g_a2dp_on_deinit) {
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
	if (hifi_bt_sink_phone()) {
		snprintf(err, n, "a phone is connected here");
		return 409;
	}
	if (hifi_lab_bt_busy && hifi_lab_bt_busy()) {
		snprintf(err, n, "the bt experiment runs; stop it first");
		return 409;
	}
	if (sink_lost) {
		snprintf(err, n, "sink not back from the last run (%s): reboot", sink_back_err);
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
	portENTER_CRITICAL(&mux);
	memset(&sc, 0, sizeof(sc));
	sst = SRC_DOWN;
	portEXIT_CRITICAL(&mux);
	src_err[0] = 0;
	sink_back = NULL;
	stopping = false;
	xEventGroupClearBits(ev, EV_INIT);

	/* the lab's callback serves the sink (mode SINK) until the sink is down */
	mode = MODE_SINK;
	esp_err_t e;
	if ((e = esp_a2d_register_callback(a2d_cb)) != ESP_OK) {
		snprintf(err, n, "esp_a2d_register_callback: %s", esp_err_to_name(e));
		return 503;
	}
	/* Bluedroid takes a second A2DP role only once the first is gone
	 * (btc_av_init returns at once while its block is still there) */
	deinit_arm();
	if ((e = esp_a2d_sink_deinit()) != ESP_OK) {
		esp_a2d_register_callback(bt_app_a2d_cb); /* still a sink */
		snprintf(err, n, "sink deinit: %s", esp_err_to_name(e));
		return 503;
	}
	sink_taken = true;
	if (!deinit_wait()) {
		/* still a sink, served through the lab's callback; if it goes
		 * down later, its answer brings it back */
		snprintf(err, n, "sink deinit: no answer in %d ms; sink back: pending", WAIT_MS);
		return 503;
	}
	mode = MODE_NONE;
	if (hifi_bt_sink_phone()) {
		/* a phone came in before the sink went down */
		give_back();
		snprintf(err, n, "a phone connected here; sink back: %s", sink_back);
		return 409;
	}
	esp_avrc_ct_register_callback(rc_ct_quiet);
	esp_avrc_tg_register_callback(rc_tg_quiet);
	esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
	esp_a2d_source_register_data_callback(tone_cb);
	if ((e = esp_a2d_source_init()) != ESP_OK || !wait_bit(EV_INIT, WAIT_MS)) {
		int len = snprintf(err, n, "source init: %s", e == ESP_OK ? "no answer" : esp_err_to_name(e));
		/* an init that failed outright left nothing up. One that didn't
		 * answer: Bluedroid's flag says whether it came up after all (take
		 * it down first) or failed on its task, which sends no answer.
		 * Its task runs in order, so a late init is given 2 s more. */
		bool up = false;
		if (e == ESP_OK) {
			for (int i = 0; i < 20 && !(up = role_up()); i++) vTaskDelay(pdMS_TO_TICKS(100));
			if (!up) ESP_LOGW(TAG, "source init: no answer and no role up: failed");
		}
		if (!up || source_down()) give_back();
		const char *how = sink_back;
		if (len > 0 && (size_t)len < n) snprintf(err + len, n - len, "; sink back: %s", how ? how : "?");
		return 503;
	}
	mode = MODE_SOURCE;
	bonded = true;
	portENTER_CRITICAL(&mux);
	sc.t_connect = esp_timer_get_time();
	portEXIT_CRITICAL(&mux);
	set_state(SRC_CONNECTING);
	if ((e = esp_a2d_source_connect(peer)) != ESP_OK) err_once("esp_a2d_source_connect", e);
	return 0;
}

static int start(uint32_t run, const char *body, char *err, size_t n) {
	(void)run;
	if (!pending_give_up()) {
		portENTER_CRITICAL(&mux);
		int64_t for_s = orphan ? (esp_timer_get_time() - pending_since) / 1000000 : 0;
		portEXIT_CRITICAL(&mux);
		if (for_s * 1000000 > PENDING_GIVE_UP_US)
			snprintf(err, n, "the last run's Bluetooth teardown hasn't answered in %d s: reboot", (int)for_s);
		else
			snprintf(err, n, "the last run's Bluetooth teardown hasn't answered yet; try again");
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
		sink_taken = false;
		int rc = start_source(body, err, n);
		if (rc) {
			if (sink_taken) { /* the result shows how the sink came back */
				role = ROLE_SOURCE;
				started = true;
				active = false;
			}
			return rc;
		}
	}
	role = want;
	started = active = true;
	ESP_LOGI(TAG, "%s armed", want == ROLE_SINK ? "sink" : "source");
	return 0;
}

/* Always true: a source teardown that is late finishes by itself (the
 * result says sink_back "pending" until it has) */
static bool stop(void) {
	if (!active) return true;
	active = false;
	if (role == ROLE_SINK) {
		armed = false;
		portENTER_CRITICAL(&mux);
		if (k.dry) dry_end(esp_timer_get_time());
		portEXIT_CRITICAL(&mux);
		return true;
	}
	if (source_down()) give_back();
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
		hj_int(j, "bytes", (long long)c.bytes);
		int64_t span = c.last_rx - c.first_rx;
		hj_int(j, "kbps", c.last_rx && span > 0 ? (long long)(c.bytes * 8000 / span) : 0);
		hj_int(j, "dropped_bytes", c.dropped);
		hj_int(j, "discarded_bytes", c.discarded);
		hj_int(j, "max_gap_ms", c.max_gap / 1000);
		hj_int(j, "gaps_over_50ms", c.gaps50);
		hj_int(j, "gaps_over_100ms", c.gaps100);
		hj_int(j, "gaps_over_250ms", c.gaps250);
		hj_int(j, "since_rx_ms", c.last_rx ? (now - c.last_rx) / 1000 : -1);
		hj_bool(j, "played", c.played);
		hj_int(j, "dry_spells", c.spells);
		hj_int(j, "dry_spells_20ms", c.spells20);
		hj_int(j, "dry_ms", c.dry_us / 1000);
		hj_int(j, "dry_max_ms", c.dry_max / 1000);
	} else {
		typeof(sc) c;
		src_state_t s;
		portENTER_CRITICAL(&mux);
		c = sc;
		s = sst;
		portEXIT_CRITICAL(&mux);
		char p[18];
		snprintf(p, sizeof(p), "%02x:%02x:%02x:%02x:%02x:%02x", peer[0], peer[1], peer[2], peer[3], peer[4],
			 peer[5]);
		hj_str(j, "peer", p);
		hj_str(j, "state", src_names[s]);
		if (src_err[0]) hj_str(j, "error", src_err);
		hj_int(j, "connect_ms", c.t_connected ? (c.t_connected - c.t_connect) / 1000 : -1);
		int64_t streamed = c.stream_len + (s == SRC_STREAMING ? now - c.t_stream : 0);
		hj_int(j, "stream_ms", streamed / 1000);
		hj_int(j, "cb_calls", c.calls);
		hj_int(j, "bytes", (long long)c.bytes);
		hj_int(j, "kbps", streamed > 0 ? (long long)(c.bytes * 8000 / streamed) : 0);
		hj_int(j, "max_cb_gap_ms", c.max_gap / 1000);
		hj_int(j, "cb_gaps_over_50ms", c.gaps50);
		hj_int(j, "cb_gaps_over_100ms", c.gaps100);
		hj_int(j, "disconnects", c.disconnects);
		if (c.acks_failed) hj_int(j, "ctrl_refused", c.acks_failed);
		const char *how = sink_back;
		if (how) hj_str(j, "sink_back", how);
	}
	hj_int(j, "run_ms", (now - t_start) / 1000);
}

/* lab_bt refuses to start while the source runs or its teardown is pending */
bool hifi_lab_a2dp_busy(void) { return (active && role == ROLE_SOURCE) || pending(); }

const hifi_lab_exp_t lab_a2dp = {
	.name = "a2dp",
	.help = "a Bluetooth audio stream between two boards: role sink (count gaps and dry output) or source "
		"(peer aa:bb:..: a 441 Hz tone to that sink; the sink comes back at the stop)",
	.start = start,
	.stop = stop,
	.result = result,
};
