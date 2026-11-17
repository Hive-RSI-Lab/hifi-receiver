/* Regression: the Bluetooth pairing bug (bring-up, patch 0007). A phone whose
 * link dropped mid-song left the sink marked as playing, so the speaker held
 * on to it and no other phone could connect; nothing let an idle phone go.
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
typedef void *TimerHandle_t;
typedef uint8_t esp_bd_addr_t[6];
typedef enum { BT_SINK_CONNECTED, BT_SINK_DISCONNECTED, BT_SINK_AUDIO_STARTED, BT_SINK_AUDIO_STOPPED, BT_SINK_PLAY,
	       BT_SINK_STOP, BT_SINK_PAUSE, BT_SINK_RATE, BT_SINK_VOLUME, BT_SINK_METADATA, BT_SINK_PROGRESS } bt_sink_cmd_t;
enum { ESP_A2D_CONNECTION_STATE_EVT, ESP_A2D_AUDIO_STATE_EVT, ESP_A2D_AUDIO_CFG_EVT };
enum { ESP_A2D_CONNECTION_STATE_DISCONNECTED, ESP_A2D_CONNECTION_STATE_CONNECTING, ESP_A2D_CONNECTION_STATE_CONNECTED };
enum { ESP_A2D_AUDIO_STATE_REMOTE_SUSPEND, ESP_A2D_AUDIO_STATE_STOPPED, ESP_A2D_AUDIO_STATE_STARTED };
enum { ESP_A2D_MCT_SBC };
enum { ESP_BT_CONNECTABLE = 1, ESP_BT_NON_CONNECTABLE = 0, ESP_BT_GENERAL_DISCOVERABLE = 2, ESP_BT_NON_DISCOVERABLE = 0 };
enum { ESP_AVRC_PT_CMD_STOP = 0x45, ESP_AVRC_PT_CMD_STATE_PRESSED = 0 };
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

/* what the sink asked for */
static int sent[16], n_sent, n_disconnect, n_stop_cmd, n_unset, scan_mode, decoder_free = 1;
static bool cmd_cb(bt_sink_cmd_t cmd, ...) {
	if (n_sent < 16) sent[n_sent++] = cmd;
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
static void esp_avrc_ct_send_passthrough_cmd(int t, int cmd, int st) { (void) t, (void) st; if (cmd == ESP_AVRC_PT_CMD_STOP) n_stop_cmd++; }
static void actrls_unset(void) { n_unset++; }
static void actrls_set(const int *c, bool o, void *a, void *b) { (void) c, (void) o, (void) a, (void) b; }
static void actrls_ir_action(void) {}
static void esp_bt_gap_set_scan_mode(int c, int d) { scan_mode = c | d; }
static void update_metadata(bool f) { (void) f; }

#include "bt_fns.c"

static void reset(void) {
	n_sent = n_disconnect = n_stop_cmd = n_unset = 0;
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

int main(void) {
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

	/* another source takes over while the phone plays: it stops counting as playing */
	conn(ESP_A2D_CONNECTION_STATE_CONNECTED);
	audio(ESP_A2D_AUDIO_STATE_STARTED);
	reset();
	bt_disconnect();
	CHECK(n_stop_cmd == 1);
	CHECK(s_audio != AUDIO_PLAYING);
	for (int t = 0; t < IDLE_RELEASE_S / IDLE_CHECK_S; t++) idle_check(NULL);
	CHECK(n_disconnect == 1);

	/* decoder busy at audio start: the phone is told to stop, not marked playing */
	conn(ESP_A2D_CONNECTION_STATE_DISCONNECTED);
	conn(ESP_A2D_CONNECTION_STATE_CONNECTED);
	reset();
	decoder_free = 0;
	audio(ESP_A2D_AUDIO_STATE_STARTED);
	CHECK(s_audio == AUDIO_CONNECTED && n_stop_cmd == 1);
	decoder_free = 1;

	return check_report("up_bt");
}
