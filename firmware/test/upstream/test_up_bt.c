/* Regression: the Bluetooth pairing bug (bring-up, patch 0007). A phone whose
 * link dropped mid-song left the sink marked as playing, so the speaker held
 * on to it and no other phone could connect; nothing let an idle phone go.
 * And the source handover (patch 0034): a phone that loses the output to
 * Spotify, AirPlay or LMS is paused and let go a second later, and a phone
 * whose audio starts while another source plays, without saying it plays (a
 * notification sound), is held until it does.
 * Builds upstream's own bt_av_hdl_a2d_evt, bt_disconnect and idle_check, as
 * patched, against stubs of the ESP-IDF Bluetooth calls. */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "../check.h"

/* --- the ESP-IDF and squeezelite bits these functions touch --- */
#define ESP_LOGD(...) ((void) 0)
#define ESP_LOGI(...) ((void) 0)
#define ESP_LOGE(...) ((void) 0)
#define ESP_LOGW(...) ((void) 0)
typedef void *TimerHandle_t;
typedef void (*TimerCallbackFunction_t)(TimerHandle_t);
#define pdMS_TO_TICKS(ms) (ms)
#define pdFALSE 0
#define pdPASS 1
typedef uint8_t esp_bd_addr_t[6];
typedef enum { BT_SINK_CONNECTED, BT_SINK_DISCONNECTED, BT_SINK_AUDIO_STARTED, BT_SINK_AUDIO_STOPPED, BT_SINK_PLAY,
	       BT_SINK_STOP, BT_SINK_PAUSE, BT_SINK_RATE, BT_SINK_VOLUME, BT_SINK_METADATA, BT_SINK_PROGRESS,
	       BT_SINK_CUTS_IN } bt_sink_cmd_t;
enum { ESP_A2D_CONNECTION_STATE_EVT, ESP_A2D_AUDIO_STATE_EVT, ESP_A2D_AUDIO_CFG_EVT };
enum { ESP_A2D_CONNECTION_STATE_DISCONNECTED, ESP_A2D_CONNECTION_STATE_CONNECTING, ESP_A2D_CONNECTION_STATE_CONNECTED };
enum { ESP_A2D_AUDIO_STATE_REMOTE_SUSPEND, ESP_A2D_AUDIO_STATE_STOPPED, ESP_A2D_AUDIO_STATE_STARTED };
enum { ESP_A2D_MCT_SBC };
enum { ESP_BT_CONNECTABLE = 1, ESP_BT_NON_CONNECTABLE = 0, ESP_BT_GENERAL_DISCOVERABLE = 2, ESP_BT_NON_DISCOVERABLE = 0 };
enum { ESP_AVRC_PT_CMD_STOP = 0x45, ESP_AVRC_PT_CMD_PAUSE = 0x46, ESP_AVRC_PT_CMD_STATE_PRESSED = 0,
       ESP_AVRC_PT_CMD_STATE_RELEASED = 1 };
enum { ESP_AVRC_RN_PLAY_STATUS_CHANGE = 1, ESP_AVRC_RN_TRACK_CHANGE = 2, ESP_AVRC_RN_PLAY_POS_CHANGED = 5 };
enum { ESP_AVRC_PLAYBACK_STOPPED, ESP_AVRC_PLAYBACK_PLAYING, ESP_AVRC_PLAYBACK_PAUSED };
enum { ESP_AVRC_BIT_MASK_OP_TEST };
typedef struct { uint16_t bits; } esp_avrc_rn_evt_cap_mask_t;
typedef union { uint8_t playback; uint32_t play_pos; } esp_avrc_rn_param_t;
enum { DISPLAYER_SHUTDOWN };
typedef union {
	struct { int state; esp_bd_addr_t remote_bda; } conn_stat;
	struct { int state; } audio_stat;
	struct { struct { int type; struct { uint8_t sbc[4]; } cie; } mcc; } audio_cfg;
} esp_a2d_cb_param_t;
typedef int actrls_t[4]; /* an array of actions upstream */

static const char BT_AV_TAG[] = "BT_AV";
static const char *s_a2d_conn_state_str[] = {"Disconnected", "Connecting", "Connected", "Disconnecting"};
static const char *s_a2d_audio_state_str[] = {"Suspended", "Stopped", "Started"};
static int s_volume, abs_volume, sink_volume;
static enum { AUDIO_IDLE, AUDIO_CONNECTED, AUDIO_PLAYING } s_audio = AUDIO_IDLE;
static int s_sample_rate;
static int tl;
static const actrls_t controls;
static esp_bd_addr_t s_remote_bda;
static volatile bool s_remote_connected;
static int s_idle_s;
static esp_avrc_rn_evt_cap_mask_t s_avrc_peer_rn_cap;

/* what the sink asked for */
static int sent[16], n_sent, n_disconnect, n_stop_cmd, n_pause_cmd, n_unset, scan_mode, decoder_free = 1;
static int other_plays; /* the answer to BT_SINK_CUTS_IN: another source holds the output */
static bool cmd_cb(bt_sink_cmd_t cmd, ...) {
	if (n_sent < 16) sent[n_sent++] = cmd;
	if (cmd == BT_SINK_CUTS_IN) return other_plays;
	return cmd == BT_SINK_AUDIO_STARTED ? decoder_free : true;
}
static bool (*bt_app_a2d_cmd_cb)(bt_sink_cmd_t cmd, ...) = cmd_cb;
static int was_sent(int cmd) {
	for (int i = 0; i < n_sent; i++)
		if (sent[i] == cmd) return 1;
	return 0;
}
static void esp_a2d_sink_disconnect(uint8_t *bda) { (void) bda; n_disconnect++; }
static void displayer_control(int c) { (void) c; }
static void esp_avrc_ct_send_passthrough_cmd(int t, int cmd, int st) {
	(void) t;
	if (st != ESP_AVRC_PT_CMD_STATE_PRESSED) return; /* a button's release */
	if (cmd == ESP_AVRC_PT_CMD_STOP) n_stop_cmd++;
	if (cmd == ESP_AVRC_PT_CMD_PAUSE) n_pause_cmd++;
}
static void actrls_unset(void) { n_unset++; }
static void actrls_set(const int *c, bool o, void *a, void *b) { (void) c, (void) o, (void) a, (void) b; }
static void actrls_ir_action(void) {}
static void esp_bt_gap_set_scan_mode(int c, int d) { scan_mode = c | d; }
static void update_metadata(bool f) { (void) f; }
static void bt_av_new_track(void) {}
static void bt_av_playback_changed(void) {}
static void bt_av_play_pos_changed(void) {}
static int peer_reports; /* the phone can report its play status (AVRCP) */
static bool esp_avrc_rn_evt_bit_mask_operation(int op, esp_avrc_rn_evt_cap_mask_t *m, int evt) {
	(void) op, (void) m;
	return evt == ESP_AVRC_RN_PLAY_STATUS_CHANGE && peer_reports;
}
/* the BT task's work queue: run at once, as the tests run on one thread */
typedef void (*bt_app_cb_t)(uint16_t event, void *param);
static bool bt_app_work_dispatch(bt_app_cb_t cb, uint16_t ev, void *p, int len, void *copy) {
	(void) len, (void) copy;
	cb(ev, p);
	return true;
}
/* one-shot timers: armed, and fired by the test */
struct timer { TimerCallbackFunction_t cb; int armed, period; };
static struct timer timers[2];
static int n_timers;
static TimerHandle_t s_let_go_timer, s_hold_timer;
static bool s_held, s_rc_playing;
static TimerHandle_t xTimerCreate(const char *n, int t, int r, void *id, TimerCallbackFunction_t cb) {
	(void) n, (void) r, (void) id;
	timers[n_timers] = (struct timer) {cb, 0, t};
	return &timers[n_timers++];
}
static int xTimerReset(TimerHandle_t h, int t) { (void) t; ((struct timer *) h)->armed = 1; return pdPASS; }
static int xTimerStop(TimerHandle_t h, int t) { (void) t; ((struct timer *) h)->armed = 0; return pdPASS; }
static int change_fail;
static int xTimerChangePeriod(TimerHandle_t h, int period, int t) {
	(void) t;
	if (change_fail) return pdFALSE;
	((struct timer *) h)->period = period;
	((struct timer *) h)->armed = 1; /* as FreeRTOS: changing the period starts it */
	return pdPASS;
}
static int armed(TimerHandle_t h) { return h && ((struct timer *) h)->armed; }
static void fire(TimerHandle_t h) {
	struct timer *tm = h;
	if (!tm || !tm->armed) return;
	tm->armed = 0;
	tm->cb(h);
}
#define timer_armed armed(s_let_go_timer)
static void fire_timer(void) { fire(s_let_go_timer); }

#include "bt_fns.c"

static void reset(void) {
	n_sent = n_disconnect = n_stop_cmd = n_pause_cmd = n_unset = 0;
}
static void conn(int state) {
	esp_a2d_cb_param_t p = {0};
	p.conn_stat.state = state;
	memset(p.conn_stat.remote_bda, 0xAB, 6);
	bt_av_hdl_a2d_evt(ESP_A2D_CONNECTION_STATE_EVT, &p);
}
static void audio(int state) {
	esp_a2d_cb_param_t p = {0};
	p.audio_stat.state = state;
	bt_av_hdl_a2d_evt(ESP_A2D_AUDIO_STATE_EVT, &p);
}
static void play_status(int playback) {
	esp_avrc_rn_param_t p = {.playback = playback};
	bt_av_notify_evt_handler(ESP_AVRC_RN_PLAY_STATUS_CHANGE, &p);
}

int main(void) {
	/* bt_sink_init makes both timers once */
	s_let_go_timer = xTimerCreate("bt_let_go", LET_GO_MS, pdFALSE, NULL, let_go_check);
	s_hold_timer = xTimerCreate("bt_hold", HOLD_MAX_MS, pdFALSE, NULL, hold_check);

	/* phone A connects and plays */
	conn(ESP_A2D_CONNECTION_STATE_CONNECTED);
	CHECK(s_remote_connected && scan_mode == 0); /* hidden while a phone holds it */
	audio(ESP_A2D_AUDIO_STATE_STARTED);
	CHECK(s_audio == AUDIO_PLAYING);

	/* its link drops mid-song with no audio-stop first */
	reset();
	conn(ESP_A2D_CONNECTION_STATE_DISCONNECTED);
	CHECK(!s_remote_connected);
	CHECK(s_audio == AUDIO_IDLE);
	CHECK(was_sent(BT_SINK_AUDIO_STOPPED));
	CHECK(was_sent(BT_SINK_DISCONNECTED));
	CHECK(n_unset == 1);
	CHECK(scan_mode == (ESP_BT_CONNECTABLE | ESP_BT_GENERAL_DISCOVERABLE)); /* others can find it again */

	/* phone B connects: not marked busy by A's song, so idle release works */
	reset();
	conn(ESP_A2D_CONNECTION_STATE_CONNECTED);
	CHECK(s_audio == AUDIO_IDLE);
	for (int t = 0; t < IDLE_RELEASE_S / IDLE_CHECK_S - 1; t++) idle_check(NULL);
	CHECK(n_disconnect == 0);
	idle_check(NULL);
	CHECK(n_disconnect == 1); /* let go after IDLE_RELEASE_S of silence */
	CHECK(IDLE_RELEASE_S >= 60 && IDLE_RELEASE_S <= 600);
	CHECK(IDLE_CHECK_S > 0 && IDLE_RELEASE_S % IDLE_CHECK_S == 0);

	/* a phone that plays is never let go, and playing resets the clock */
	reset();
	audio(ESP_A2D_AUDIO_STATE_STARTED);
	for (int t = 0; t < 100; t++) idle_check(NULL);
	CHECK(n_disconnect == 0);
	audio(ESP_A2D_AUDIO_STATE_STOPPED);
	CHECK(s_audio == AUDIO_IDLE);
	idle_check(NULL);
	CHECK(n_disconnect == 0 && s_idle_s == IDLE_CHECK_S);

	/* no phone: nothing to let go */
	reset();
	conn(ESP_A2D_CONNECTION_STATE_DISCONNECTED);
	for (int t = 0; t < 100; t++) idle_check(NULL);
	CHECK(n_disconnect == 0);

	/* another source takes over while the phone plays: it is paused (not
	 * stopped, so it can come back), stops counting as playing, and is let go
	 * a second later, so the next phone or source finds the speaker free */
	conn(ESP_A2D_CONNECTION_STATE_CONNECTED);
	audio(ESP_A2D_AUDIO_STATE_STARTED);
	reset();
	bt_disconnect();
	CHECK(n_pause_cmd == 1 && n_stop_cmd == 0);
	CHECK(s_audio != AUDIO_PLAYING);
	CHECK(timer_armed && n_disconnect == 0);
	CHECK(LET_GO_MS >= 200 && LET_GO_MS <= 5000);
	fire_timer();
	CHECK(n_disconnect == 1);

	/* the phone starts again before the second is up: it keeps the speaker */
	conn(ESP_A2D_CONNECTION_STATE_CONNECTED);
	audio(ESP_A2D_AUDIO_STATE_STARTED);
	reset();
	bt_sink_let_go();
	audio(ESP_A2D_AUDIO_STATE_STARTED);
	fire_timer();
	CHECK(n_disconnect == 0 && s_audio == AUDIO_PLAYING);

	/* a connected phone that isn't playing: no pause, let go all the same */
	audio(ESP_A2D_AUDIO_STATE_STOPPED);
	reset();
	bt_sink_let_go();
	CHECK(n_pause_cmd == 0);
	fire_timer();
	CHECK(n_disconnect == 1);

	/* no timer to be had: let go at once rather than hold on */
	TimerHandle_t let_go = s_let_go_timer;
	s_let_go_timer = NULL;
	reset();
	bt_sink_let_go();
	CHECK(n_disconnect == 1);
	s_let_go_timer = let_go;

	/* no phone connected: nothing to do */
	conn(ESP_A2D_CONNECTION_STATE_DISCONNECTED);
	reset();
	bt_sink_let_go();
	CHECK(!timer_armed && n_disconnect == 0);

	/* audio starting re-sends the phone's volume once the output is its own */
	conn(ESP_A2D_CONNECTION_STATE_CONNECTED);
	reset();
	audio(ESP_A2D_AUDIO_STATE_STARTED);
	CHECK(was_sent(BT_SINK_AUDIO_STARTED) && was_sent(BT_SINK_VOLUME));

	/* decoder busy at audio start: the phone is told to stop, not marked playing */
	conn(ESP_A2D_CONNECTION_STATE_DISCONNECTED);
	conn(ESP_A2D_CONNECTION_STATE_CONNECTED);
	reset();
	decoder_free = 0;
	audio(ESP_A2D_AUDIO_STATE_STARTED);
	CHECK(s_audio == AUDIO_CONNECTED && n_stop_cmd == 1);
	decoder_free = 1;

	/* --- a phone's audio while another source plays (Opus review of #249) --- */
	CHECK(HOLD_NO_RN_MS >= 500 && HOLD_NO_RN_MS < HOLD_MAX_MS && HOLD_MAX_MS <= 10000);

	/* a notification sound: A2DP starts with no play report while Spotify
	 * plays. Held: it doesn't take the output, and ending drops the hold */
	conn(ESP_A2D_CONNECTION_STATE_DISCONNECTED);
	conn(ESP_A2D_CONNECTION_STATE_CONNECTED);
	other_plays = 1, peer_reports = 1;
	reset();
	audio(ESP_A2D_AUDIO_STATE_STARTED);
	CHECK(was_sent(BT_SINK_CUTS_IN) && !was_sent(BT_SINK_AUDIO_STARTED));
	CHECK(s_held && s_audio == AUDIO_CONNECTED && n_stop_cmd == 0);
	CHECK(armed(s_hold_timer) && ((struct timer *) s_hold_timer)->period == HOLD_MAX_MS);
	audio(ESP_A2D_AUDIO_STATE_STOPPED);
	CHECK(!s_held && !armed(s_hold_timer) && s_audio == AUDIO_IDLE);
	fire(s_hold_timer);
	CHECK(!was_sent(BT_SINK_AUDIO_STARTED));

	/* the phone presses play: its play report takes the output at once */
	reset();
	audio(ESP_A2D_AUDIO_STATE_STARTED);
	CHECK(s_held && !was_sent(BT_SINK_AUDIO_STARTED));
	play_status(ESP_AVRC_PLAYBACK_PLAYING);
	CHECK(was_sent(BT_SINK_AUDIO_STARTED) && s_audio == AUDIO_PLAYING);
	CHECK(!s_held && !armed(s_hold_timer));

	/* the play report comes first: the audio start takes over without a hold */
	audio(ESP_A2D_AUDIO_STATE_STOPPED);
	play_status(ESP_AVRC_PLAYBACK_PAUSED);
	play_status(ESP_AVRC_PLAYBACK_PLAYING);
	reset();
	audio(ESP_A2D_AUDIO_STATE_STARTED);
	CHECK(!was_sent(BT_SINK_CUTS_IN) && was_sent(BT_SINK_AUDIO_STARTED) && !s_held);

	/* a phone that reports late: the 5 s backstop takes over */
	audio(ESP_A2D_AUDIO_STATE_STOPPED);
	play_status(ESP_AVRC_PLAYBACK_PAUSED);
	reset();
	audio(ESP_A2D_AUDIO_STATE_STARTED);
	CHECK(s_held);
	fire(s_hold_timer);
	CHECK(was_sent(BT_SINK_AUDIO_STARTED) && s_audio == AUDIO_PLAYING && !s_held);

	/* a phone that can't report play status: taken after the short hold */
	audio(ESP_A2D_AUDIO_STATE_STOPPED);
	s_rc_playing = false, peer_reports = 0;
	reset();
	audio(ESP_A2D_AUDIO_STATE_STARTED);
	CHECK(s_held && ((struct timer *) s_hold_timer)->period == HOLD_NO_RN_MS);
	fire(s_hold_timer);
	CHECK(was_sent(BT_SINK_AUDIO_STARTED) && s_audio == AUDIO_PLAYING);

	/* nothing else plays: no hold, as before */
	audio(ESP_A2D_AUDIO_STATE_STOPPED);
	s_rc_playing = false, other_plays = 0;
	reset();
	audio(ESP_A2D_AUDIO_STATE_STARTED);
	CHECK(was_sent(BT_SINK_CUTS_IN) && was_sent(BT_SINK_AUDIO_STARTED) && !s_held);

	/* no hold timer to be had: take over rather than hold for ever */
	audio(ESP_A2D_AUDIO_STATE_STOPPED);
	other_plays = 1, change_fail = 1;
	reset();
	audio(ESP_A2D_AUDIO_STATE_STARTED);
	CHECK(was_sent(BT_SINK_AUDIO_STARTED) && !s_held);
	change_fail = 0;

	/* the link drops while held: the hold is gone, and a stale timer does nothing */
	audio(ESP_A2D_AUDIO_STATE_STOPPED);
	s_rc_playing = false;
	reset();
	audio(ESP_A2D_AUDIO_STATE_STARTED);
	CHECK(s_held);
	conn(ESP_A2D_CONNECTION_STATE_DISCONNECTED);
	CHECK(!s_held && !armed(s_hold_timer));
	s_held = true; /* a timer already queued when it dropped */
	hold_expired(0, NULL);
	CHECK(!was_sent(BT_SINK_AUDIO_STARTED));
	s_held = false;

	/* another source taking over drops a held phone's hold and its play report */
	conn(ESP_A2D_CONNECTION_STATE_CONNECTED);
	reset();
	audio(ESP_A2D_AUDIO_STATE_STARTED);
	s_rc_playing = true;
	CHECK(s_held);
	bt_sink_let_go();
	CHECK(!s_held && !s_rc_playing && !armed(s_hold_timer) && n_pause_cmd == 0);
	other_plays = 0;

	return check_report("up_bt");
}
