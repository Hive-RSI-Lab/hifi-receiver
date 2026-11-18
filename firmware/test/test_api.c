#include "check.h"
#include "hifi_api.h"
#include "hifi_diag.h"
#include "hifi_json.h"
#include "hifi_vol.h"

#include <stdlib.h>

/* ------------------------------------------------------------ mock board */

static struct {
	bool amp_enabled, amp_reset;
	int tone_starts, tone_stops;
	hifi_tone_params_t tone;
	bool tone_force;
	bool power_on;
	bool snap_heap_boot_zero; /* the snapshot has no boot heap yet */
	hifi_input_t input;
	float sub_db;
	int sub_on;
	hifi_chan_t channel;
	float eq[HIFI_EQ_BANDS];
	char name[65];
	bool rebooted, reset;
	int armed, arm_fail;
	char action[16];
	hifi_vol_t vol;
	hifi_meta_t playing;
	int ir_learning, ir_forgot;
	bool ir_learned[HIFI_IR_N];
	bool core;        /* answer as Plus: the line in through the ADC */
	bool dj_guard;
	const char *dj_tripped;
	bool loud; /* the board has Boost and a heat cap */
	float boost_db, heat_w;
	int loud_calls;
} m;

static void m_info(hifi_info_t *i) {
	i->model = "hifi-lite";
	i->board = "lite-main";
	i->fw = "0.1.0";
	i->upstream = "squeezelite-esp32 master-v4.3 ee1720c";
	snprintf(i->mac, sizeof(i->mac), "24:0a:c4:12:34:56");
	snprintf(i->hostname, sizeof(i->hostname), "squeezelite-123456");
	i->flash_mb = 8;
	i->psram_mb = 8;
	snprintf(i->name, sizeof(i->name), "%s", m.name[0] ? m.name : "Living room");
	i->caps = (hifi_caps_t) {.dj_direct = true, .line = m.core, .eq_min_db = -12, .eq_max_db = 12, .sub_count = 2, .sub_max_db = m.core ? 0 : -9,
				 .zones = 1, .pdo_v_max = 20};
	if (m.loud) {
		i->caps.boost_max_db = 9;
		i->caps.heat_min_w = 2;
		i->caps.heat_max_w = 15;
		i->caps.heat_def_w = 9;
	}
}

static void m_snapshot(hifi_snapshot_t *s) {
	s->uptime_s = 812;
	s->pwr_state = m.power_on ? HIFI_PWR_ON : HIFI_PWR_WAIT_PD;
	s->vbus_v = 20.123f;
	s->pg = true;
	hifi_contract_from(true, 0x08, 65, 20.1f, &s->contract);
	s->pvdd_on = m.power_on;
	s->pvdd_v = NAN;
	s->vbus_i_a = NAN;
	s->amp_enabled = m.amp_enabled;
	s->amp_temp_c = NAN;
	s->gov_budget_w = 58.5f;
	s->gov_in_w = 7.9f;
	s->gov_out_w = 4.4f;
	s->gov_gain_db = 0;
	s->source = "lms";
	s->input = m.input;
	s->dj_guard = m.dj_guard;
	s->dj_guard_w = 12.34f;
	s->dj_guard_limit_w = 14.2f;
	s->dj_guard_tripped = m.dj_tripped;
	s->level_dbfs = -23.4f;
	s->sub_on = m.sub_on != 0;
	s->sub_db = m.sub_db;
	s->channel = m.channel;
	memcpy(s->eq, m.eq, sizeof(m.eq));
	s->volume = m.vol.volume;
	s->mute = m.vol.mute;
	s->max_volume = m.vol.max_volume;
	if (m.loud) {
		s->loudness = true;
		s->gov_why = m.heat_w > 9 ? "heat" : "";
		s->heat_cap_w = m.heat_w;
		s->heat_w = 3.25f;
		s->amp_hot = m.heat_w > 12;
		s->boost_db = m.boost_db;
		s->boost_limit_db = m.boost_db > 0 ? -1.5f : 0;
		s->boost_limiting = m.boost_db > 0;
		s->boost_events = 7;
	}
	s->playing = m.playing;
	s->ir_learning = m.ir_learning;
	memcpy(s->ir_learned, m.ir_learned, sizeof(m.ir_learned));
	s->net_mode = "sta";
	snprintf(s->ssid, sizeof(s->ssid), "home");
	snprintf(s->hostname, sizeof(s->hostname), "hifi-lite");
	snprintf(s->ip, sizeof(s->ip), "192.168.1.40");
	s->rssi = -61;
	s->heap_free = 81234;
	s->heap_min = 60123;
	s->heap_boot = m.snap_heap_boot_zero ? 0 : 17890;
	s->psram_free = 3912345;
}

static int m_tone_start(const hifi_tone_params_t *p, bool force, const char **err) {
	if (!m.power_on && !force) {
		*err = "amp isn't powered (power state wait_pd)";
		return 409;
	}
	m.tone_starts++;
	m.tone = *p;
	m.tone_force = force;
	return 0;
}
static void m_tone_stop(void) { m.tone_stops++; }
static void m_amp_enable(bool on) { m.amp_enabled = on; }
static void m_amp_reset(void) { m.amp_reset = true; }
static int m_set_input(hifi_input_t in) {
	m.input = in;
	return 0;
}
static float m_set_sub(int on, float db) {
	if (on >= 0) m.sub_on = on;
	if (!isnan(db)) m.sub_db = db > 4.5f ? 4.5f : db;
	return m.sub_db;
}
static int m_set_loudness(float b, float w) {
	m.loud_calls++;
	if (!isnan(b)) m.boost_db = b;
	if (!isnan(w)) m.heat_w = w;
	return 0;
}
static int m_set_channel(hifi_chan_t c) {
	m.channel = c;
	return 0;
}
static int m_set_eq(const float g[HIFI_EQ_BANDS]) {
	memcpy(m.eq, g, sizeof(m.eq));
	return 0;
}
static int m_set_name(const char *n) {
	snprintf(m.name, sizeof(m.name), "%s", n);
	return 0;
}
static void m_reboot(void) { m.rebooted = true; }
static void m_factory_reset(void) { m.reset = true; }
static int m_factory_arm(void) {
	if (m.arm_fail) return -1;
	m.armed++;
	return 0;
}
static int m_control(const char *a) {
	snprintf(m.action, sizeof(m.action), "%s", a);
	return strcmp(a, "volup") && strcmp(a, "toggle") ? -1 : 0;
}
static int m_set_volume(int v, int mute, int max) { return hifi_vol_set(&m.vol, v, mute, max) ? 0 : -1; }
static void m_ir_learn(int a) { m.ir_learning = a; }
static void m_ir_forget(int a) {
	m.ir_forgot = a;
	if (a < 0) memset(m.ir_learned, 0, sizeof(m.ir_learned));
	else m.ir_learned[a] = false;
}
static void m_selftest(hifi_selftest_t *t) {
	hifi_selftest_add(t, "pd_sink_i2c", true, "0x%02x acks", 0x22);
	hifi_selftest_add(t, "network", m.power_on, m.power_on ? "192.168.1.40" : "not connected");
}

static const hifi_ops_t ops = {
	.info = m_info, .snapshot = m_snapshot, .tone_start = m_tone_start, .tone_stop = m_tone_stop,
	.amp_enable = m_amp_enable, .amp_reset = m_amp_reset, .set_input = m_set_input,
	.set_sub = m_set_sub, .set_channel = m_set_channel, .set_eq = m_set_eq, .set_name = m_set_name, .reboot = m_reboot,
	.factory_reset = m_factory_reset, .control = m_control, .selftest = m_selftest,
	.set_volume = m_set_volume, .ir_learn = m_ir_learn, .ir_forget = m_ir_forget, .set_loudness = m_set_loudness};

static char out[2048];

static int call(const char *method, const char *path, const char *body) {
	int st = hifi_api_handle(&ops, method, path, body, out, sizeof(out));
	CHECK(hj_valid_object(out)); /* every reply is one JSON object */
	return st;
}

/* --------------------------------------------------------------- tests */

static void info_and_state(void) {
	CHECK(call("GET", "/api/v1/info", NULL) == 200);
	CHECK_STR(out, "{\"model\":\"hifi-lite\",\"board\":\"lite-main\",\"fw\":\"0.1.0\","
		       "\"upstream\":\"squeezelite-esp32 master-v4.3 ee1720c\",\"api\":1,\"name\":\"Living room\","
		       "\"mac\":\"24:0a:c4:12:34:56\",\"hostname\":\"squeezelite-123456\",\"flash_mb\":8,\"psram_mb\":8,"
		       "\"caps\":{\"line_modes\":[\"dj_direct\"],\"eq_bands\":[31,62,125,250,500,1000,2000,4000,8000,16000],"
		       "\"eq_min\":-12,\"eq_max\":12,\"sub\":{\"count\":2,\"crossover\":false,\"per_sub\":false,\"max_db\":-9.0},"
		       "\"zones\":1,\"eth\":false,\"pdo_v_max\":20,\"channel\":true,\"led\":false,\"backlight\":false}}");

	m.power_on = true;
	m.amp_enabled = true;
	CHECK(call("GET", "/api/v1/state?x=1", NULL) == 200);
	CHECK_STR(out,
		  "{\"uptime_s\":812,"
		  "\"power\":{\"state\":\"on\",\"vbus_v\":20.12,\"pg\":true,"
		  "\"contract\":{\"protocol\":\"pd\",\"epr\":false,\"v\":20.0,\"i_max_a\":3.25,\"w\":65.0},"
		  "\"pvdd_on\":true,\"pvdd_v\":null,\"vbus_i_a\":null},"
		  "\"amp\":{\"enabled\":true,\"fault\":false,\"faults\":0,\"latched\":false,\"temp_c\":null},"
		  "\"governor\":{\"budget_w\":58.5,\"in_w\":7.9,\"out_w\":4.4,\"gain_db\":0.0,\"limiting\":false,\"limit_events\":0},"
		  "\"audio\":{\"source\":\"lms\",\"input\":\"stream\",\"tone\":null,\"level_dbfs\":-23.4,\"channel\":\"stereo\","
		  "\"sub\":{\"on\":false,\"level_db\":0.0},\"eq\":[0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0],"
		  "\"volume\":35,\"mute\":false,\"max_volume\":100,\"playing\":null},"
		  "\"net\":{\"mode\":\"sta\",\"ssid\":\"home\",\"hostname\":\"hifi-lite\",\"ip\":\"192.168.1.40\",\"rssi\":-61},"
		  "\"sys\":{\"heap_free\":81234,\"heap_min\":60123,\"heap_boot\":17890,\"psram_free\":3912345},"
		  "\"ir\":{\"learning\":null,\"learned\":[],\"last_code\":null}}");

	CHECK(call("POST", "/api/v1/state", "{}") == 405);
	CHECK(call("GET", "/api/v1/nope", NULL) == 404);
	CHECK(call("GET", "/other", NULL) == 404);
}

static void tones(void) {
	m.power_on = true;
	CHECK(call("POST", "/api/v1/tone", "{\"type\":\"sine\",\"freq_hz\":440,\"level_dbfs\":-12,\"channel\":\"left\",\"duration_s\":2}") == 200);
	CHECK_STR(out, "{\"ok\":true,\"tone\":{\"type\":\"sine\",\"freq_hz\":440.0,\"level_dbfs\":-12.0,\"channel\":\"left\",\"duration_s\":2.0}}");
	CHECK(m.tone.type == HIFI_TONE_SINE && m.tone.channel == HIFI_CH_LEFT && m.tone.freq_hz == 440);

	/* defaults */
	CHECK(call("POST", "/api/v1/tone", "{\"type\":\"sweep\"}") == 200);
	CHECK_STR(out, "{\"ok\":true,\"tone\":{\"type\":\"sweep\",\"start_hz\":20.0,\"end_hz\":20000.0,\"level_dbfs\":-20.0,\"duration_s\":5.0}}");
	CHECK(call("POST", "/api/v1/tone", "{\"type\":\"silence\",\"duration_s\":3}") == 200);
	CHECK_STR(out, "{\"ok\":true,\"tone\":{\"type\":\"silence\",\"duration_s\":3.0}}");

	/* the -6 dBFS cap */
	int before = m.tone_starts;
	CHECK(call("POST", "/api/v1/tone", "{\"type\":\"sine\",\"level_dbfs\":-3}") == 400);
	CHECK_HAS(out, "force");
	CHECK(m.tone_starts == before);
	CHECK(call("POST", "/api/v1/tone", "{\"type\":\"sine\",\"level_dbfs\":-3,\"force\":true}") == 200);
	CHECK(m.tone_force);

	/* bad input */
	CHECK(call("POST", "/api/v1/tone", "{}") == 400);
	CHECK(call("POST", "/api/v1/tone", "{\"type\":\"square\"}") == 400);
	CHECK(call("POST", "/api/v1/tone", "{\"type\":\"sine\",\"channel\":\"middle\"}") == 400);
	CHECK(call("POST", "/api/v1/tone", "not json") == 400);
	CHECK(call("GET", "/api/v1/tone", NULL) == 405);

	/* no power: 409 unless forced */
	m.power_on = false;
	CHECK(call("POST", "/api/v1/tone", "{\"type\":\"sine\"}") == 409);
	CHECK_HAS(out, "wait_pd");
	CHECK(call("POST", "/api/v1/tone", "{\"type\":\"sine\",\"force\":true}") == 200);

	CHECK(call("POST", "/api/v1/tone", "{\"type\":\"stop\"}") == 200);
	CHECK_STR(out, "{\"ok\":true,\"tone\":null}");
	CHECK(m.tone_stops == 1);
}

static void amp_input_sub_control(void) {
	CHECK(call("POST", "/api/v1/amp", "{\"enable\":false}") == 200);
	CHECK(!m.amp_enabled);
	CHECK_STR(out, "{\"enabled\":false,\"fault\":false,\"faults\":0,\"latched\":false,\"temp_c\":null}");
	CHECK(call("POST", "/api/v1/amp", "{\"reset\":true,\"enable\":true}") == 200);
	CHECK(m.amp_enabled && m.amp_reset);
	CHECK(call("POST", "/api/v1/amp", "{}") == 400);
	CHECK(call("POST", "/api/v1/amp", NULL) == 400);

	CHECK(call("POST", "/api/v1/input", "{\"input\":\"dj_direct\"}") == 200);
	CHECK(m.input == HIFI_INPUT_DJ_DIRECT);
	CHECK_STR(out, "{\"ok\":true,\"input\":\"dj_direct\"}");
	CHECK(call("POST", "/api/v1/input", "{\"input\":\"phono\"}") == 400);
	/* the governed line in is the core board's: Lite has no ADC path for it */
	CHECK(call("POST", "/api/v1/input", "{\"input\":\"line\"}") == 409);
	CHECK(m.input == HIFI_INPUT_DJ_DIRECT);
	m.core = true;
	CHECK(call("GET", "/api/v1/info", NULL) == 200);
	CHECK_HAS(out, "\"line_modes\":[\"dj_direct\",\"line\"]");
	CHECK(call("POST", "/api/v1/input", "{\"input\":\"line\"}") == 200);
	CHECK(m.input == HIFI_INPUT_LINE);
	CHECK_STR(out, "{\"ok\":true,\"input\":\"line\"}");
	CHECK(call("GET", "/api/v1/state", NULL) == 200);
	CHECK_HAS(out, "\"input\":\"line\",\"tone\":");
	m.dj_guard = true;
	m.dj_tripped = "otw";
	CHECK(call("GET", "/api/v1/state", NULL) == 200);
	CHECK_HAS(out, "\"input\":\"line\",\"dj_guard\":{\"in_w\":12.3,\"limit_w\":14.2,\"tripped\":\"otw\"},\"tone\":");
	m.dj_tripped = NULL;
	CHECK(call("GET", "/api/v1/state", NULL) == 200);
	CHECK_HAS(out, "\"tripped\":null}");
	m.core = m.dj_guard = false;
	m.input = HIFI_INPUT_DJ_DIRECT;

	CHECK(call("POST", "/api/v1/sub", "{\"level_db\":-8.5}") == 400); /* over caps.sub.max_db */
	CHECK_HAS(out, "-30 to -9.0 on this model");
	CHECK(call("POST", "/api/v1/sub", "{\"level_db\":-10.5}") == 200);
	CHECK_HAS(out, "\"sub\":{\"on\":false,\"level_db\":-10.5}"); /* what the codec could do */
	CHECK(call("POST", "/api/v1/sub", "{\"on\":true}") == 200);
	CHECK(m.sub_on == 1 && m.sub_db == -10.5f);
	CHECK(call("POST", "/api/v1/sub", "{\"level_db\":-40}") == 400);
	CHECK(call("POST", "/api/v1/sub", "{}") == 400);

	/* channel mode: stereo by default, shown in /state, set by the route */
	CHECK(call("GET", "/api/v1/state", NULL) == 200);
	CHECK_HAS(out, "\"channel\":\"stereo\",\"sub\":");
	CHECK(call("POST", "/api/v1/channel", "{\"channel\":\"left\"}") == 200);
	CHECK(m.channel == HIFI_CHAN_LEFT);
	CHECK_HAS(out, "\"channel\":\"left\"");
	CHECK(call("POST", "/api/v1/channel", "{\"channel\":\"mono\"}") == 200 && m.channel == HIFI_CHAN_MONO);
	CHECK(call("POST", "/api/v1/channel", "{\"channel\":\"right\"}") == 200 && m.channel == HIFI_CHAN_RIGHT);
	CHECK(call("POST", "/api/v1/channel", "{\"channel\":\"centre\"}") == 400 && m.channel == HIFI_CHAN_RIGHT);
	CHECK(call("POST", "/api/v1/channel", "{}") == 400);
	CHECK(call("GET", "/api/v1/channel", NULL) == 405);
	CHECK(call("POST", "/api/v1/channel", "{\"channel\":\"stereo\"}") == 200 && m.channel == HIFI_CHAN_STEREO);

	CHECK(call("POST", "/api/v1/eq", "{\"gains\":[1,2,3,4,5,6,7,8,9,-12]}") == 200);
	CHECK(m.eq[0] == 1 && m.eq[9] == -12);
	CHECK_HAS(out, "\"eq\":[1.0,2.0,3.0,4.0,5.0,6.0,7.0,8.0,9.0,-12.0]");
	CHECK(call("POST", "/api/v1/eq", "{\"gains\":[1,2,3]}") == 400);
	CHECK(call("POST", "/api/v1/eq", "{\"gains\":[1,2,3,4,5,6,7,8,9,13]}") == 400);
	CHECK(call("POST", "/api/v1/eq", "{\"gains\":[1,2,3,4,5,6,7,8,9,\"x\"]}") == 400);

	CHECK(call("POST", "/api/v1/name", "{\"name\":\"Kitchen\"}") == 200);
	CHECK_STR(m.name, "Kitchen");
	CHECK(call("POST", "/api/v1/name", "{\"name\":\"\"}") == 400);
	/* the name goes into squeezelite's command line: no quote or backslash */
	CHECK(call("POST", "/api/v1/name", "{\"name\":\"a\\\" -o x\"}") == 400);
	CHECK(call("POST", "/api/v1/name", "{\"name\":\"a\\\\b\"}") == 400);
	CHECK(call("POST", "/api/v1/name", "{\"name\":\"a\x7f" "b\"}") == 400);
	CHECK(call("POST", "/api/v1/name", "{\"name\":\"0123456789012345678901234567890123\"}") == 400);
	/* escapes are decoded before the check: a CR used to be saved as "r" */
	CHECK(call("POST", "/api/v1/name", "{\"name\":\"Kitchen\\r\"}") == 400);
	CHECK_HAS(out, "control character");
	CHECK(call("POST", "/api/v1/name", "{\"name\":\"a\\u001fb\"}") == 400);
	/* \u0000 and a bad escape aren't text we take: the body is refused */
	CHECK(call("POST", "/api/v1/name", "{\"name\":\"a\\u0000b\"}") == 400);
	CHECK_HAS(out, "JSON object");
	CHECK(call("POST", "/api/v1/name", "{\"name\":\"a\\qb\"}") == 400);
	CHECK_STR(m.name, "Kitchen");
	CHECK(call("POST", "/api/v1/name", "{\"name\":\"Caf\\u00e9 \\/ Bar\"}") == 200);
	CHECK_STR(m.name, "Caf\xc3\xa9 / Bar");
	/* 32 bytes, not characters, as the mock counts: 16 two-byte letters fit, 17 don't */
	CHECK(call("POST", "/api/v1/name", "{\"name\":\"\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9"
					    "\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\"}") == 200);
	CHECK(call("POST", "/api/v1/name", "{\"name\":\"\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9"
					    "\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\"}") == 400);
	/* 67 bytes decoded, past the reader's 64-byte buffer: refused as too long
	 * (the length is checked before the control characters), the name kept */
	CHECK(call("POST", "/api/v1/name", "{\"name\":\"Kitchen"
					    "\\r\\r\\r\\r\\r\\r\\r\\r\\r\\r\\r\\r\\r\\r\\r\\r\\r\\r\\r\\r"
					    "\\r\\r\\r\\r\\r\\r\\r\\r\\r\\r\\r\\r\\r\\r\\r\\r\\r\\r\\r\\r"
					    "\\r\\r\\r\\r\\r\\r\\r\\r\\r\\r\\r\\r\\r\\r\\r\\r\\r\\r\\r\\r\"}") == 400);
	CHECK_HAS(out, "32 characters");
	CHECK_STR(m.name, "\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9");
	CHECK(call("POST", "/api/v1/name", "{\"name\":\"Kitchen\"}") == 200);

	CHECK(call("POST", "/api/v1/reset", "{}") == 400);
	CHECK(!m.reset);
	/* L6: from the LAN a factory reset only arms (the erase waits for a
	 * power-on start); with no way to arm, the LAN can't reset at all */
	CHECK(hifi_api_handle_from(&ops, HIFI_SRC_LAN, "POST", "/api/v1/reset", "{\"factory\":true}", out, sizeof(out)) == 403);
	CHECK(!m.reset && strstr(out, "USB"));
	{
		hifi_ops_t armable = ops;
		armable.factory_arm = m_factory_arm;
		CHECK(hifi_api_handle_from(&armable, HIFI_SRC_LAN, "POST", "/api/v1/reset", "{}", out, sizeof(out)) == 400);
		CHECK(m.armed == 0);
		CHECK(hifi_api_handle_from(&armable, HIFI_SRC_LAN, "POST", "/api/v1/reset", "{\"factory\":true}", out, sizeof(out)) == 200);
		CHECK(m.armed == 1 && !m.reset);
		CHECK_STR(out, "{\"armed\":true,\"within_s\":600,\"next\":\"unplug the receiver and plug it back in\"}");
		m.arm_fail = 1;
		CHECK(hifi_api_handle_from(&armable, HIFI_SRC_LAN, "POST", "/api/v1/reset", "{\"factory\":true}", out, sizeof(out)) == 403);
		m.arm_fail = 0;
		CHECK(m.armed == 1 && !m.reset);
		/* the USB console and the relay (trusted) erase at once */
		CHECK(hifi_api_handle(&armable, "POST", "/api/v1/reset", "{\"factory\":true}", out, sizeof(out)) == 200);
		CHECK(m.reset && m.armed == 1);
		m.reset = false;
	}
	CHECK(call("POST", "/api/v1/reset", "{\"factory\":true}") == 200);
	CHECK(m.reset);
	CHECK(call("POST", "/api/v1/reboot", NULL) == 200);
	CHECK(m.rebooted);

	CHECK(call("POST", "/api/v1/control", "{\"action\":\"volup\"}") == 200);
	CHECK_STR(m.action, "volup");
	CHECK(call("POST", "/api/v1/control", "{\"action\":\"explode\"}") == 400);
}

static void volume_playing_ir(void) {
	CHECK(call("POST", "/api/v1/volume", "{\"volume\":60}") == 200);
	CHECK_HAS(out, "\"volume\":60,\"mute\":false,\"max_volume\":100,");
	CHECK(call("POST", "/api/v1/volume", "{\"max_volume\":50}") == 200);
	CHECK_HAS(out, "\"volume\":50,\"mute\":false,\"max_volume\":50,");
	CHECK(call("POST", "/api/v1/volume", "{\"volume\":80}") == 200); /* held to the maximum, as the app's mock does */
	CHECK_HAS(out, "\"volume\":50,");
	CHECK(call("POST", "/api/v1/volume", "{\"mute\":true,\"volume\":20,\"max_volume\":100}") == 200);
	CHECK_HAS(out, "\"volume\":20,\"mute\":true,\"max_volume\":100,");
	CHECK(m.vol.mute && m.vol.volume == 20);

	/* refused, and nothing changes */
	CHECK(call("POST", "/api/v1/volume", "{}") == 400);
	CHECK(call("POST", "/api/v1/volume", "{\"volume\":101}") == 400);
	CHECK(call("POST", "/api/v1/volume", "{\"volume\":-1}") == 400);
	CHECK(call("POST", "/api/v1/volume", "{\"volume\":20.5}") == 400);
	CHECK(call("POST", "/api/v1/volume", "{\"volume\":\"50\"}") == 400);
	CHECK(call("POST", "/api/v1/volume", "{\"max_volume\":29}") == 400);
	CHECK(call("POST", "/api/v1/volume", "{\"mute\":\"yes\"}") == 400);
	CHECK(call("POST", "/api/v1/volume", "{\"mute\":false,\"volume\":true}") == 400);
	CHECK(call("GET", "/api/v1/volume", NULL) == 405);
	CHECK(m.vol.mute && m.vol.volume == 20 && m.vol.max_volume == 100);
	CHECK(call("POST", "/api/v1/volume", "{\"volume\":null,\"mute\":false}") == 200); /* null leaves it */
	CHECK(!m.vol.mute && m.vol.volume == 20);

	/* now playing */
	hifi_meta_active(&m.playing);
	CHECK(call("GET", "/api/v1/state", NULL) == 200);
	CHECK_HAS(out, "\"playing\":{\"state\":\"stop\",\"title\":null,\"artist\":null,\"album\":null,"
		       "\"elapsed_s\":null,\"duration_s\":null}}");
	hifi_meta_track(&m.playing, "Bj\xc3\xb6rk", "Homogenic", "J\xc3\xb3ga \"live\"");
	hifi_meta_timer(&m.playing, 61500, 305000, 0);
	hifi_meta_play(&m.playing, 0);
	CHECK(call("GET", "/api/v1/state", NULL) == 200);
	CHECK_HAS(out, "\"playing\":{\"state\":\"play\",\"title\":\"J\xc3\xb3ga \\\"live\\\"\",\"artist\":\"Bj\xc3\xb6rk\","
		       "\"album\":\"Homogenic\",\"elapsed_s\":61,\"duration_s\":305}}");

	/* the remote */
	m.ir_learning = -1;
	CHECK(call("POST", "/api/v1/ir/learn", "{\"action\":\"mute\"}") == 200);
	CHECK(m.ir_learning == HIFI_IR_MUTE);
	CHECK_HAS(out, "\"ir\":{\"learning\":\"mute\",\"learned\":[],\"last_code\":null}");
	m.ir_learning = -1;
	m.ir_learned[HIFI_IR_MUTE] = m.ir_learned[HIFI_IR_VOLUP] = true;
	CHECK(call("GET", "/api/v1/state", NULL) == 200);
	CHECK_HAS(out, "\"ir\":{\"learning\":null,\"learned\":[\"volup\",\"mute\"],");
	CHECK(call("POST", "/api/v1/ir/learn", "{\"action\":\"explode\"}") == 400);
	CHECK(call("POST", "/api/v1/ir/learn", "{}") == 400);
	CHECK(call("POST", "/api/v1/ir/forget", "{\"action\":\"volup\"}") == 200);
	CHECK(m.ir_forgot == HIFI_IR_VOLUP && !m.ir_learned[HIFI_IR_VOLUP] && m.ir_learned[HIFI_IR_MUTE]);
	CHECK(call("POST", "/api/v1/ir/forget", "{\"action\":\"nope\"}") == 400);
	CHECK(call("POST", "/api/v1/ir/forget", NULL) == 200);
	CHECK(m.ir_forgot == -1 && !m.ir_learned[HIFI_IR_MUTE]);
	CHECK_HAS(out, "\"learned\":[]");

	hifi_snapshot_t s = {.ir_learning = -1, .ir_have_last = true, .ir_last_code = 0x00ff40bf};
	char st[2048];
	hifi_api_write_state(&s, st, sizeof(st));
	CHECK_HAS(st, "\"last_code\":\"0x00ff40bf\"");
	hifi_meta_idle(&m.playing);
}

/* /state/<part>: one section, so a serial line (the bench cuts at 1000
 * characters) holds what a test saves even as the whole state grows */
static void state_parts(void) {
	m.power_on = true;
	CHECK(call("GET", "/api/v1/state/audio", NULL) == 200);
	CHECK_STR(out, "{\"audio\":{\"source\":\"lms\",\"input\":\"dj_direct\",\"tone\":null,\"level_dbfs\":-23.4,\"channel\":\"stereo\","
		       "\"sub\":{\"on\":true,\"level_db\":-10.5},\"eq\":[1.0,2.0,3.0,4.0,5.0,6.0,7.0,8.0,9.0,-12.0],"
		       "\"volume\":20,\"mute\":false,\"max_volume\":100,\"playing\":null}}");
	CHECK(call("GET", "/api/v1/state/sys", NULL) == 200);
	CHECK_STR(out, "{\"sys\":{\"heap_free\":81234,\"heap_min\":60123,\"heap_boot\":17890,\"psram_free\":3912345}}");
	/* the boot heap isn't taken yet: null, not 0 */
	m.snap_heap_boot_zero = true;
	CHECK(call("GET", "/api/v1/state/sys", NULL) == 200);
	CHECK_HAS(out, "\"heap_boot\":null,");
	m.snap_heap_boot_zero = false;
	CHECK(call("GET", "/api/v1/state/amp?x=1", NULL) == 200);
	CHECK_HAS(out, "{\"amp\":{\"enabled\":");
	/* a section this board doesn't have now: an empty answer */
	CHECK(call("GET", "/api/v1/state/update", NULL) == 200);
	CHECK_STR(out, "{}");
	CHECK(call("GET", "/api/v1/state/uptime_s", NULL) == 404);
	CHECK(call("GET", "/api/v1/state/", NULL) == 404);
	CHECK(call("GET", "/api/v1/state/audio/x", NULL) == 404);
	CHECK(call("POST", "/api/v1/state/audio", "{}") == 405);

	/* every section on its own makes up the whole state */
	hifi_snapshot_t s;
	m_snapshot(&s);
	static char whole[4096], part[1024];
	hifi_api_write_state(&s, whole, sizeof(whole));
	static const char *const parts[] = {"power", "amp", "governor", "audio", "net", "sys", "ir"};
	for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
		hifi_api_write_state_part(&s, parts[i], part, sizeof(part));
		size_t n = strlen(part);
		CHECK(n > 2 && part[0] == '{' && part[n - 1] == '}');
		part[n - 1] = '\0';
		CHECK_HAS(whole, part + 1);
	}

	/* the size guard: with every field at its longest (track text at
	 * its limit, a tone, the DJ guard, both zones, every IR action, the
	 * controls, an update with an error), each section stays under 800
	 * characters, leaving the bench's 1000-character line room to grow */
	memset(&s, 0, sizeof(s));
	s.uptime_s = 4294967295u;
	s.pwr_state = HIFI_PWR_NO_PVDD;
	s.vbus_v = s.pvdd_v = 48.88f;
	s.vbus_i_a = 5.55f;
	s.contract = (hifi_contract_t) {.known = true, .proto = HIFI_PROTO_PD, .epr = true, .v = 48.0f, .i_max_a = 5.0f, .w = 240.0f};
	s.amp_faults = 2147483647;
	s.amp_temp_c = -100.5f;
	s.gov_budget_w = s.gov_in_w = s.gov_out_w = -999.9f;
	s.gov_gain_db = -99.9f;
	s.gov_events = 4294967295u;
	s.source = "spotify";
	s.input = HIFI_INPUT_DJ_DIRECT;
	s.dj_guard = true;
	s.dj_guard_w = s.dj_guard_limit_w = 999.9f;
	s.dj_guard_tripped = "contract";
	s.tone_active = true;
	s.tone = (hifi_tone_params_t) {.type = HIFI_TONE_SWEEP, .start_hz = 20000, .end_hz = 20000, .level_dbfs = -120, .duration_s = 3600, .channel = HIFI_CH_RIGHT};
	s.level_dbfs = -120.5f;
	s.sub_db = -12.5f;
	for (int i = 0; i < HIFI_EQ_BANDS; i++) s.eq[i] = -12.5f;
	s.volume = s.max_volume = 100;
	s.zones = 2;
	s.playing.state = HIFI_PLAY_PAUSE;
	/* '"' escapes to two characters, the most any track text can cost now
	 * that hifi_utf8_copy turns control characters into spaces */
	memset(s.playing.title, '"', sizeof(s.playing.title) - 1);
	memset(s.playing.artist, '"', sizeof(s.playing.artist) - 1);
	memset(s.playing.album, '"', sizeof(s.playing.album) - 1);
	s.playing.elapsed_ms = s.playing.duration_ms = 2147483647;
	s.ir_learning = 0;
	for (int a = 0; a < HIFI_IR_N; a++) s.ir_learned[a] = true;
	s.ir_have_last = true;
	s.ir_last_code = 0xffffffffu;
	s.exp_present = s.front_present = true;
	memset(s.last_control, 'c', sizeof(s.last_control) - 1);
	s.last_control_ago_s = 4294967295u;
	s.backlight = "auto";
	s.net_mode = "sta";
	s.eth_present = s.eth_up = true;
	memset(s.ssid, 's', sizeof(s.ssid) - 1);
	memset(s.hostname, 'h', sizeof(s.hostname) - 1);
	snprintf(s.ip, sizeof(s.ip), "255.255.255.255");
	s.rssi = -100;
	s.heap_free = s.heap_min = s.heap_boot = s.psram_free = 4294967295u;
	s.reset_reason = "task_watchdog";
	static char cause[HIFI_RESTART_CAUSE_CAP];
	memset(cause, 'c', sizeof(cause) - 1);
	s.restart_cause = cause;
	s.update = (hifi_update_status_t) {.state = HIFI_UPD_ERROR, .progress = 100};
	memset(s.update.error, 'e', sizeof(s.update.error) - 1);
	static const char *const all[] = {"power", "amp", "governor", "audio", "net", "sys", "ir", "controls", "update"};
	for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
		hifi_api_write_state_part(&s, all[i], part, sizeof(part));
		size_t n = strlen(part);
		if (getenv("STATE_SIZES") || n > 850) fprintf(stderr, "state/%s is %zu characters\n", all[i], n);
		CHECK(n > 2 && n <= 850 && part[n - 1] == '}');
	}
	hifi_meta_idle(&m.playing);
}

static void selftest(void) {
	m.power_on = true;
	CHECK(call("POST", "/api/v1/selftest", NULL) == 200);
	CHECK_STR(out, "{\"pass\":true,\"checks\":[{\"name\":\"pd_sink_i2c\",\"pass\":true,\"value\":\"0x22 acks\"},"
		       "{\"name\":\"network\",\"pass\":true,\"value\":\"192.168.1.40\"}]}");
	m.power_on = false;
	CHECK(call("POST", "/api/v1/selftest", "{}") == 200);
	CHECK_HAS(out, "{\"pass\":false,");
}

static void console(void) {
	const char *method, *path;
	char body[256], msg[1024];
#define CLI(...)                                                                     \
	({                                                                           \
		const char *argv_[] = {__VA_ARGS__};                                 \
		hifi_cli_request(sizeof(argv_) / sizeof(argv_[0]), argv_, &method, &path, body, sizeof(body), msg, sizeof(msg)); \
	})

	CHECK(CLI("state"));
	CHECK_STR(method, "GET");
	CHECK_STR(path, "/api/v1/state");
	CHECK(CLI("state", "audio"));
	CHECK_STR(method, "GET");
	CHECK_STR(path, "/api/v1/state/audio");
	CHECK(!CLI("state", "nope"));

	CHECK(CLI("tone", "sine", "1000", "-20"));
	CHECK_STR(method, "POST");
	CHECK_STR(path, "/api/v1/tone");
	CHECK_STR(body, "{\"type\":\"sine\",\"freq_hz\":1000,\"level_dbfs\":-20,\"channel\":\"both\",\"duration_s\":5}");
	CHECK(call(method, path, body) == 200 || m.power_on == false);

	CHECK(CLI("tone", "sine", "440", "-12", "right", "3"));
	CHECK_HAS(body, "\"channel\":\"right\",\"duration_s\":3");
	CHECK(CLI("tone", "sweep", "20", "20000", "-20"));
	CHECK_STR(body, "{\"type\":\"sweep\",\"start_hz\":20,\"end_hz\":20000,\"level_dbfs\":-20,\"duration_s\":10}");
	CHECK(CLI("tone", "noise", "-30", "8"));
	CHECK_STR(body, "{\"type\":\"noise\",\"level_dbfs\":-30,\"duration_s\":8}");
	CHECK(CLI("tone", "silence"));
	CHECK(CLI("tone", "stop"));
	CHECK(CLI("amp", "reset"));
	CHECK_STR(body, "{\"reset\":true}");
	CHECK(CLI("channel", "left"));
	CHECK_STR(method, "POST");
	CHECK_STR(path, "/api/v1/channel");
	CHECK_STR(body, "{\"channel\":\"left\"}");
	CHECK(!CLI("channel", "centre"));
	CHECK(!CLI("channel"));
	CHECK(CLI("input", "dj_direct"));
	CHECK(CLI("input", "line"));
	CHECK(CLI("sub", "-6"));
	CHECK_STR(body, "{\"level_db\":-6}");
	CHECK(CLI("control", "volup"));
	CHECK(CLI("selftest"));
	CHECK(CLI("info"));
	CHECK(CLI("volume", "40"));
	CHECK_STR(path, "/api/v1/volume");
	CHECK_STR(body, "{\"volume\":40}");
	CHECK(CLI("maxvol", "70"));
	CHECK_STR(body, "{\"max_volume\":70}");
	CHECK(CLI("mute", "on"));
	CHECK_STR(body, "{\"mute\":true}");
	CHECK(call(method, path, body) == 200);
	CHECK(CLI("ir", "learn", "volup"));
	CHECK_STR(path, "/api/v1/ir/learn");
	CHECK_STR(body, "{\"action\":\"volup\"}");
	CHECK(CLI("ir", "forget"));
	CHECK_STR(path, "/api/v1/ir/forget");
	CHECK_STR(body, "{}");
	CHECK(CLI("ir", "forget", "mute"));
	CHECK(!CLI("ir", "learn"));
	CHECK(!CLI("ir", "learn", "x\"y"));
	CHECK(!CLI("ir", "dance"));
	CHECK(!CLI("volume", "loud"));
	CHECK(!CLI("mute", "maybe"));
	CHECK(CLI("led", "test"));
	CHECK_STR(path, "/api/v1/led");
	CHECK_STR(body, "{\"mode\":\"test\"}");
	CHECK(CLI("backlight", "off"));
	CHECK_STR(path, "/api/v1/backlight");
	CHECK_STR(body, "{\"mode\":\"off\"}");
	CHECK(!CLI("led", "on"));
	CHECK(!CLI("backlight", "test"));
	CHECK(!CLI("backlight"));
	CHECK(CLI("party"));
	CHECK_STR(method, "GET");
	CHECK_STR(path, "/api/v1/party");
	CHECK(CLI("party", "stats"));
	CHECK_STR(method, "GET");
	CHECK_STR(path, "/api/v1/party/stats");
	CHECK(CLI("party", "lead"));
	CHECK_STR(method, "POST");
	CHECK_STR(path, "/api/v1/party");
	CHECK_STR(body, "{\"op\":\"lead\"}");
	CHECK(CLI("party", "off"));
	CHECK_STR(body, "{\"op\":\"off\"}");
	CHECK(!CLI("party", "key"));
	CHECK(!CLI("party", "on", "now"));
	/* i2c isn't an API request: the console handles it (hifi_i2c_cli) */
	CHECK(!CLI("i2c", "0x20", "0"));
	CHECK_HAS(msg, "i2c <addr> <reg> [val]");

	/* garbage doesn't become a request, and can't inject JSON */
	CHECK(!CLI("tone", "sine", "1000", "-20\",\"force\":true"));
	CHECK_HAS(msg, "usage");
	CHECK(!CLI("tone", "sine", "abc", "-20"));
	CHECK(!CLI("tone", "sine", "1000", "-20", "middle"));
	CHECK(!CLI("amp", "maybe"));
	CHECK(!CLI("input", "phono"));
	CHECK(!CLI("sub", "loud"));
	CHECK(!CLI("control", "a\"b"));
	CHECK(!CLI("state", "extra"));
	CHECK(!CLI("bogus"));
	CHECK(!hifi_cli_request(0, NULL, &method, &path, body, sizeof(body), msg, sizeof(msg)));
}

/* ------------------------------------------------- the core board's routes */

static struct {
	bool zone_b, forgot;
	int scan_busy;
	char url[192], sha[72];
	bool sha_null, need_sig;
	hifi_update_status_t upd;
	bool controls;
	char led[8], backlight[8];
	const char *ota_state;
} c;

static void c_snapshot(hifi_snapshot_t *s) {
	m_snapshot(s);
	s->zones = 2;
	s->zone_b_on = c.zone_b;
	s->eth_present = true;
	s->eth_up = true;
	s->update = c.upd;
	if (c.controls) {
		s->exp_present = s->front_present = true;
		s->exp_in = 0xbf7e;
		s->front_in = 0xf7;
		snprintf(s->last_control, sizeof(s->last_control), "button toggle");
		s->last_control_ago_s = 12;
		s->backlight = c.backlight[0] ? c.backlight : "auto";
	}
}
static int c_set_led(const char *mode) {
	if (strcmp(mode, "test") && strcmp(mode, "auto")) return -1;
	snprintf(c.led, sizeof(c.led), "%s", mode);
	return 0;
}
static int c_set_backlight(const char *mode) {
	if (strcmp(mode, "on") && strcmp(mode, "off") && strcmp(mode, "auto")) return -1;
	snprintf(c.backlight, sizeof(c.backlight), "%s", mode);
	return 0;
}
static char party_sub[16];
static bool party_post, party_lan;
static int c_party(const char *sub, bool post, bool lan, const char *body, char *o, size_t cap) {
	(void)body;
	snprintf(party_sub, sizeof(party_sub), "%s", sub);
	party_post = post, party_lan = lan;
	snprintf(o, cap, "{\"on\":false}");
	return 200;
}
static int c_wifi_scan(hifi_wifi_ap_t *aps, int max) {
	if (c.scan_busy) return -1;
	CHECK(max == HIFI_WIFI_SCAN_MAX);
	aps[0] = (hifi_wifi_ap_t) {.ssid = "home", .rssi = -48, .secure = true};
	aps[1] = (hifi_wifi_ap_t) {.ssid = "", .rssi = -70, .secure = true}; /* hidden */
	aps[2] = (hifi_wifi_ap_t) {.ssid = "cafe \"free\"", .rssi = -81, .secure = false};
	return 3;
}
static int c_wifi_forget(void) {
	c.forgot = true;
	return 0;
}
static void c_set_zone(bool on) { c.zone_b = on; }
static void c_ota_status(hifi_ota_status_t *o) {
	o->slot = "ota_1";
	o->state = c.ota_state ? c.ota_state : "valid";
}
static int c_update_check(hifi_update_offer_t *o, const char **err) {
	return hifi_update_parse("{\"latest\":\"0.2.0\",\"url\":\"plus.bin\",\"notes\":\"Zone B\"}",
				 "http://hive.local/fw/manifest.json", "hifi-plus", o, err) ? 502 : 0;
}
static int c_update_start(const char *url, const char *sha, bool need_sig, const char **err) {
	c.need_sig = need_sig;
	if (c.upd.state == HIFI_UPD_DOWNLOADING) return *err = "an update is already running", 409;
	snprintf(c.url, sizeof(c.url), "%s", url);
	c.sha_null = !sha;
	snprintf(c.sha, sizeof(c.sha), "%s", sha ? sha : "");
	c.upd = (hifi_update_status_t) {.state = HIFI_UPD_DOWNLOADING, .progress = 0};
	return 0;
}

static const hifi_ops_t core_ops = {
	.info = m_info, .snapshot = c_snapshot, .tone_start = m_tone_start, .tone_stop = m_tone_stop,
	.amp_enable = m_amp_enable, .amp_reset = m_amp_reset, .set_input = m_set_input,
	.set_sub = m_set_sub, .set_eq = m_set_eq, .set_name = m_set_name, .reboot = m_reboot,
	.factory_reset = m_factory_reset, .control = m_control, .selftest = m_selftest,
	.set_volume = m_set_volume, .ir_learn = m_ir_learn, .ir_forget = m_ir_forget,
	.wifi_scan = c_wifi_scan, .wifi_forget = c_wifi_forget, .set_zone = c_set_zone,
	.update_check = c_update_check, .update_start = c_update_start, .set_led = c_set_led,
	.set_backlight = c_set_backlight, .ota_status = c_ota_status, .party = c_party};

static int ccall(const char *method, const char *path, const char *body) {
	int st = hifi_api_handle(&core_ops, method, path, body, out, sizeof(out));
	CHECK(hj_valid_object(out));
	return st;
}

static void core_routes(void) {
	/* Lite answers none of them */
	CHECK(call("GET", "/api/v1/wifi/scan", NULL) == 404);
	CHECK(call("POST", "/api/v1/wifi/forget", "{}") == 404);
	CHECK(call("POST", "/api/v1/zone", "{\"zone\":\"b\",\"on\":true}") == 404);
	CHECK_STR(out, "{\"error\":\"this model has one zone\"}");
	CHECK(call("GET", "/api/v1/update/check", NULL) == 404);
	CHECK(call("POST", "/api/v1/update", "{\"url\":\"http://h/a.bin\"}") == 404);
	CHECK(call("GET", "/api/v1/party", NULL) == 404);
	CHECK_HAS(out, "no party mode");

	/* party: the route hands the sub-path and the source to the op */
	CHECK(ccall("GET", "/api/v1/party", NULL) == 200 && !party_post && !strcmp(party_sub, ""));
	CHECK(ccall("GET", "/api/v1/party/stats", NULL) == 200 && !strcmp(party_sub, "stats"));
	CHECK(ccall("POST", "/api/v1/party", "{\"op\":\"on\"}") == 200 && party_post && !party_lan);
	CHECK(hifi_api_handle_from(&core_ops, HIFI_SRC_LAN, "POST", "/api/v1/party", "{\"op\":\"off\"}", out, sizeof(out)) == 200);
	CHECK(party_lan);
	CHECK(ccall("DELETE", "/api/v1/party", NULL) == 405);
	CHECK(ccall("GET", "/api/v1/partyx", NULL) == 404);

	/* info says which of the settings routes the model has (k2391) */
	CHECK(ccall("GET", "/api/v1/info", NULL) == 200);
	CHECK_HAS(out, "\"channel\":false,\"led\":true,\"backlight\":true");
	CHECK(ccall("POST", "/api/v1/channel", "{\"channel\":\"left\"}") == 404);

	CHECK(ccall("GET", "/api/v1/wifi/scan", NULL) == 200);
	CHECK_STR(out, "{\"networks\":[{\"ssid\":\"home\",\"rssi\":-48,\"secure\":true},"
		       "{\"ssid\":\"cafe \\\"free\\\"\",\"rssi\":-81,\"secure\":false}]}");
	c.scan_busy = 1;
	CHECK(ccall("GET", "/api/v1/wifi/scan", NULL) == 409);
	c.scan_busy = 0;
	CHECK(ccall("POST", "/api/v1/wifi/scan", "{}") == 405);
	CHECK(ccall("POST", "/api/v1/wifi/forget", NULL) == 200 && c.forgot);

	CHECK(ccall("POST", "/api/v1/zone", "{\"zone\":\"b\",\"on\":true}") == 200 && c.zone_b);
	CHECK(ccall("POST", "/api/v1/zone", "{\"zone\":\"b\",\"volume\":40}") == 400);
	CHECK_HAS(out, "plays at the main volume");
	CHECK(ccall("POST", "/api/v1/zone", "{\"zone\":\"b\",\"on\":true,\"volume\":1e999}") == 400);
	CHECK(ccall("POST", "/api/v1/zone", "{\"zone\":\"a\",\"on\":true}") == 400);
	CHECK(ccall("POST", "/api/v1/zone", "{\"zone\":\"b\"}") == 400);
	CHECK(c.zone_b);

	CHECK(ccall("GET", "/api/v1/update/check", NULL) == 200);
	CHECK_STR(out, "{\"current\":\"0.1.0\",\"latest\":\"0.2.0\",\"url\":\"http://hive.local/fw/plus.bin\","
		       "\"sha256\":null,\"notes\":\"Zone B\"}");
	CHECK(ccall("POST", "/api/v1/update", "{\"url\":\"ftp://h/a.bin\"}") == 400);
	CHECK(ccall("POST", "/api/v1/channel", "{\"channel\":\"left\"}") == 404);
	CHECK(ccall("POST", "/api/v1/update", "{}") == 400);
	CHECK(ccall("POST", "/api/v1/update", "{\"url\":\"http://hive.local/fw/plus.bin\"}") == 200);
	CHECK_STR(c.url, "http://hive.local/fw/plus.bin");
	CHECK(c.sha_null);
	CHECK(!c.need_sig); /* a trusted caller (the relay, the console) */
	CHECK(ccall("POST", "/api/v1/update", "{\"url\":\"http://h/b.bin\"}") == 409);
	CHECK_HAS(out, "already running");
	/* from the LAN the image must be signed (security.md) */
	c.upd = (hifi_update_status_t) {0};
	CHECK(hifi_api_handle_from(&core_ops, HIFI_SRC_LAN, "POST", "/api/v1/update",
				   "{\"url\":\"http://hive.local/fw/plus.bin\"}", out, sizeof(out)) == 200);
	CHECK(c.need_sig);
	c.upd = (hifi_update_status_t) {0};
	/* a typo in the hash refuses the update: it must not read as no hash */
	c.url[0] = '\0';
	CHECK(ccall("POST", "/api/v1/update", "{\"url\":\"http://hive.local/fw/plus.bin\",\"sha256\":\"\\q0\"}") == 400);
	CHECK_HAS(out, "JSON object");
	CHECK_STR(c.url, "");
	CHECK(c.upd.state != HIFI_UPD_DOWNLOADING);

	/* new firmware on trial: a lost Wi-Fi rolls it back, so the LAN may not
	 * change or forget the Wi-Fi, or erase settings, until it is kept */
	c.ota_state = "pending_verify";
	c.forgot = false;
	CHECK(hifi_api_handle_from(&core_ops, HIFI_SRC_LAN, "POST", "/api/v1/wifi/forget", "{}", out, sizeof(out)) == 409);
	CHECK_HAS(out, "on trial");
	CHECK(!c.forgot);
	CHECK(hifi_api_handle_from(&core_ops, HIFI_SRC_LAN, "POST", "/api/v1/wifi",
				   "{\"ssid\":\"x\",\"password\":\"12345678\"}", out, sizeof(out)) == 409);
	CHECK(hifi_api_handle_from(&core_ops, HIFI_SRC_LAN, "POST", "/api/v1/reset", "{\"factory\":true}", out, sizeof(out)) == 409);
	CHECK(hifi_api_handle_from(&core_ops, HIFI_SRC_LAN, "POST", "/api/v1/reboot", "{}", out, sizeof(out)) == 409);
	CHECK(hifi_api_handle_from(&core_ops, HIFI_SRC_LAN, "POST", "/api/v1/bttest", "{\"sink\":\"x\"}", out, sizeof(out)) == 409);
	CHECK(hifi_api_handle_from(&core_ops, HIFI_SRC_LAN, "POST", "/api/v1/reboot?x=1", "{}", out, sizeof(out)) == 409);
	CHECK(hifi_api_handle_from(&core_ops, HIFI_SRC_LAN, "GET", "/api/v1/wifi/scan", NULL, out, sizeof(out)) == 200);
	CHECK(hifi_api_handle_from(&core_ops, HIFI_SRC_LAN, "GET", "/api/v1/state", NULL, out, sizeof(out)) == 200);
	CHECK(ccall("POST", "/api/v1/wifi/forget", NULL) == 200 && c.forgot); /* USB or the relay still can */
	c.ota_state = NULL;
	c.forgot = false;
	CHECK(hifi_api_handle_from(&core_ops, HIFI_SRC_LAN, "POST", "/api/v1/wifi/forget", "{}", out, sizeof(out)) == 200);
	CHECK(c.forgot);
	c.forgot = false;

	/* the state: zone B, Ethernet and the update, which Lite's leaves out */
	c.upd = (hifi_update_status_t) {.state = HIFI_UPD_DOWNLOADING, .progress = 42};
	CHECK(ccall("GET", "/api/v1/state", NULL) == 200);
	CHECK_HAS(out, "\"playing\":null,\"zone_b\":{\"on\":true,\"volume\":null}},");
	CHECK_HAS(out, "\"rssi\":-61,\"eth\":true},");
	CHECK_HAS(out, "\"update\":{\"state\":\"downloading\",\"progress\":42,\"error\":null}}");
	c.upd = (hifi_update_status_t) {.state = HIFI_UPD_ERROR, .progress = 10, .error = "sha256 mismatch"};
	CHECK(ccall("GET", "/api/v1/state", NULL) == 200);
	CHECK_HAS(out, "\"update\":{\"state\":\"error\",\"progress\":10,\"error\":\"sha256 mismatch\"}}");
	c.upd = (hifi_update_status_t) {0};

	/* bring-up: the LED test and the backlight; Lite has neither */
	CHECK(call("POST", "/api/v1/led", "{\"mode\":\"test\"}") == 404);
	CHECK_HAS(out, "doesn't drive this model's LED");
	CHECK(call("POST", "/api/v1/backlight", "{\"mode\":\"on\"}") == 404);
	CHECK(ccall("POST", "/api/v1/led", "{\"mode\":\"test\"}") == 200);
	CHECK_STR(c.led, "test");
	CHECK(ccall("POST", "/api/v1/led", "{\"mode\":\"blink\"}") == 400);
	CHECK_STR(out, "{\"error\":\"mode must be test or auto\"}");
	CHECK(ccall("POST", "/api/v1/led", "{}") == 400);
	CHECK(ccall("GET", "/api/v1/led", NULL) == 405);
	CHECK(ccall("POST", "/api/v1/led", "{\"mode\":\"auto\"}") == 200);
	CHECK_STR(c.led, "auto");
	CHECK(ccall("POST", "/api/v1/backlight", "{\"mode\":\"off\"}") == 200);
	CHECK_STR(c.backlight, "off");
	CHECK(ccall("POST", "/api/v1/backlight", "{\"mode\":\"dim\"}") == 400);
	CHECK_HAS(out, "on, off or auto");

	/* the controls in the state: only where the board has them */
	CHECK(ccall("GET", "/api/v1/state", NULL) == 200);
	CHECK(!strstr(out, "\"controls\""));
	CHECK(call("GET", "/api/v1/state", NULL) == 200);
	CHECK(!strstr(out, "\"controls\""));
	c.controls = true;
	CHECK(ccall("GET", "/api/v1/state", NULL) == 200);
	CHECK_HAS(out, "\"controls\":{\"expander\":\"0xbf7e\",\"front\":\"0xf7\",\"last\":\"button toggle\","
		       "\"last_ago_s\":12,\"backlight\":\"off\"}");
	c.controls = false;
}

/* `hifi i2c` on the console */
static struct {
	uint8_t regs[256];
	int reads, writes;
	uint8_t addr;
} bus;
static int b_rd(uint8_t addr, uint8_t reg, uint8_t *v) {
	bus.reads++;
	bus.addr = addr;
	if (addr != 0x20) return -1;
	*v = bus.regs[reg];
	return 0;
}
static int b_wr(uint8_t addr, uint8_t reg, uint8_t v) {
	bus.writes++;
	bus.addr = addr;
	if (addr != 0x20) return -1;
	bus.regs[reg] = v;
	return 0;
}

static void i2c_console(void) {
	char o[256];
#define I2C(busy, ...)                                                                    \
	({                                                                                \
		const char *argv_[] = {__VA_ARGS__};                                      \
		hifi_i2c_cli(sizeof(argv_) / sizeof(argv_[0]), argv_, busy, b_rd, b_wr, o, sizeof(o)); \
	})
	bus.regs[0] = 0x3f;
	CHECK(I2C(NULL, "i2c", "0x20", "0") == 200);
	CHECK_STR(o, "{\"addr\":\"0x20\",\"reg\":\"0x00\",\"val\":\"0x3f\"}");
	CHECK(I2C(NULL, "i2c", "32", "0x06", "0xfe") == 200);
	CHECK_STR(o, "{\"addr\":\"0x20\",\"reg\":\"0x06\",\"val\":\"0xfe\",\"wrote\":true}");
	CHECK(bus.regs[6] == 0xfe && bus.writes == 1);

	/* reads are fine while audio plays; writes aren't */
	CHECK(I2C("a source plays", "i2c", "0x20", "6") == 200);
	CHECK_HAS(o, "\"val\":\"0xfe\"");
	CHECK(I2C("a source plays", "i2c", "0x20", "6", "0") == 409);
	CHECK_HAS(o, "no writes while a source plays");
	CHECK(bus.regs[6] == 0xfe && bus.writes == 1);

	/* no ack */
	CHECK(I2C(NULL, "i2c", "0x48", "1") == 502);
	CHECK_STR(o, "{\"error\":\"0x48 doesn't answer\"}");

	/* nothing reaches the bus from a bad line */
	int r = bus.reads, w = bus.writes;
	CHECK(I2C(NULL, "i2c", "0x20") == 400);
	CHECK_HAS(o, "usage: i2c");
	CHECK(I2C(NULL, "i2c", "0x78", "0") == 400);  /* reserved addresses */
	CHECK(I2C(NULL, "i2c", "0x07", "0") == 400);
	CHECK(I2C(NULL, "i2c", "0x20", "256") == 400);
	CHECK(I2C(NULL, "i2c", "0x20", "0", "0x100") == 400);
	CHECK(I2C(NULL, "i2c", "0x20", "0", "-1") == 400);
	CHECK(I2C(NULL, "i2c", "0x2g", "0") == 400);
	CHECK(I2C(NULL, "i2c", "0x20", "", "1") == 400);
	CHECK(I2C(NULL, "i2c", "0x20", "0", "1", "2") == 400);
	CHECK(I2C(NULL, "amp", "0x20", "0") == 400);
	CHECK(bus.reads == r && bus.writes == w);
}

static const char *x_reason(void) { return "panic"; }
static size_t x_log(char *o, size_t cap) {
	/* colour codes and stray bytes dropped, newlines kept, cut to cap */
	static const char log[] = "\x1b[0;31mE (1) bt: boom\x1b[0m\n\x01" "Backtrace: 0x4008\"\\\n";
	size_t n = sizeof(log) - 1 < cap - 1 ? sizeof(log) - 1 : cap - 1;
	memcpy(o, log, n);
	o[n] = '\0';
	return n;
}

static size_t x_long_log(char *o, size_t cap) {
	/* 3000 bytes, a third of them newlines (6 escaped) */
	size_t n = 3000 < cap - 1 ? 3000 : cap - 1;
	for (size_t i = 0; i < n; i++) o[i] = i % 3 == 2 ? '\n' : 'a';
	o[n] = '\0';
	return n;
}

static void crash(void) {
	CHECK(call("GET", "/api/v1/crash", NULL) == 404); /* no recorder */
	hifi_ops_t x = ops;
	x.reset_reason = x_reason;
	x.last_log = x_log;
	CHECK(hifi_api_handle(&x, "GET", "/api/v1/crash", NULL, out, sizeof(out)) == 200);
	CHECK_STR(out, "{\"reset_reason\":\"panic\",\"log\":\"E (1) bt: boom\\u000aBacktrace: 0x4008\\\"\\\\\\u000a\"}");
	CHECK(hifi_api_reply_cap("/api/v1/crash", NULL) > 6000);
	/* a page at a time, for a bench that caps a console line */
	CHECK(hifi_api_handle(&x, "POST", "/api/v1/crash", "{}", out, sizeof(out)) == 400);
	CHECK(hifi_api_handle(&x, "POST", "/api/v1/crash", "{\"from\":-1}", out, sizeof(out)) == 400);
	/* fuzzing: NaN passed from < 0 and was cast and used as an offset into the log */
	CHECK(hifi_api_handle(&x, "POST", "/api/v1/crash", "{\"from\":-nan}", out, sizeof(out)) == 400);
	CHECK(hifi_api_handle(&x, "POST", "/api/v1/crash", "{\"from\":1e300}", out, sizeof(out)) == 200);
	CHECK_HAS(out, "\"log\":\"\"");
	/* frames past int were cast before the 64..16384 check */
	CHECK(hifi_api_reply_cap("/api/v1/capture", "{\"frames\":1e30}") ==
	      hifi_api_reply_cap("/api/v1/capture", "{\"frames\":16384}"));
	CHECK(hifi_api_reply_cap("/api/v1/capture", "{\"frames\":-1e30}") ==
	      hifi_api_reply_cap("/api/v1/capture", "{\"frames\":64}"));
	/* a number that isn't finite (1e999, -inf, -nan) is refused, not read as missing */
	CHECK(call("POST", "/api/v1/volume", "{\"volume\":1e999,\"mute\":true}") == 400);
	CHECK(call("POST", "/api/v1/tone", "{\"type\":\"sine\",\"freq_hz\":1e999}") == 400);
	CHECK(call("POST", "/api/v1/tone", "{\"type\":\"noise\",\"level_dbfs\":-nan}") == 400);
	CHECK(call("POST", "/api/v1/sub", "{\"level_db\":-inf}") == 400);
	CHECK(hifi_api_reply_cap("/api/v1/capture", "{\"frames\":1e999}") ==   /* refused, not the default */
	      hifi_api_reply_cap("/api/v1/capture", "{\"frames\":64}"));
	CHECK(hifi_api_handle(&x, "POST", "/api/v1/crash", "{\"from\":15}", out, sizeof(out)) == 200);
	CHECK_STR(out, "{\"reset_reason\":\"panic\",\"len\":35,\"from\":15,\"next\":35,"
		       "\"log\":\"Backtrace: 0x4008\\\"\\\\\\u000a\"}");
	CHECK(hifi_api_handle(&x, "POST", "/api/v1/crash", "{\"from\":99}", out, sizeof(out)) == 200);
	CHECK_HAS(out, "\"from\":35,\"next\":35,\"log\":\"\"");
	x.last_log = x_long_log;
	CHECK(hifi_api_handle(&x, "POST", "/api/v1/crash", "{\"from\":0}", out, sizeof(out)) == 200);
	CHECK(strlen(out) < 900);
	CHECK_HAS(out, "\"len\":3000,\"from\":0,\"next\":");
	const char *argv[] = {"crash"}, *method, *path;
	char body[64];
	CHECK(hifi_cli_request(1, argv, &method, &path, body, sizeof(body), out, sizeof(out)));
	CHECK_STR(method, "GET");
	CHECK_STR(path, "/api/v1/crash");
	const char *argv2[] = {"crash", "700"};
	CHECK(hifi_cli_request(2, argv2, &method, &path, body, sizeof(body), out, sizeof(out)));
	CHECK_STR(method, "POST");
	CHECK_STR(path, "/api/v1/crash");
	CHECK_STR(body, "{\"from\":700}");
}

static char bt_sink[40];
static int x_bt_test(const char *sink, const char **err) {
	if (!strcmp(sink, "busy")) {
		*err = "already set up";
		return -1;
	}
	snprintf(bt_sink, sizeof(bt_sink), "%s", sink);
	return 0;
}

static void bttest(void) {
	CHECK(call("POST", "/api/v1/bttest", "{\"sink\":\"HiFi Lite 3\"}") == 404); /* no test */
	hifi_ops_t x = ops;
	x.bt_test = x_bt_test;
	CHECK(hifi_api_handle(&x, "POST", "/api/v1/bttest", "{}", out, sizeof(out)) == 400);
	CHECK(hifi_api_handle(&x, "POST", "/api/v1/bttest", "{\"sink\":\"\"}", out, sizeof(out)) == 400);
	CHECK(hifi_api_handle(&x, "POST", "/api/v1/bttest", "{\"sink\":\"busy\"}", out, sizeof(out)) == 409);
	CHECK_HAS(out, "already set up");
	CHECK(hifi_api_handle(&x, "GET", "/api/v1/bttest", NULL, out, sizeof(out)) == 405);
	CHECK(!bt_sink[0]);
	CHECK(hifi_api_handle(&x, "POST", "/api/v1/bttest", "{\"sink\":\"HiFi Lite 3\"}", out, sizeof(out)) == 200);
	CHECK_STR(bt_sink, "HiFi Lite 3");
}

static void loudness(void) {
	/* a board without Boost: no caps entry, no state fields, 404 */
	CHECK(call("GET", "/api/v1/info", NULL) == 200);
	CHECK(!strstr(out, "loudness"));
	CHECK(call("POST", "/api/v1/loudness", "{\"boost_db\":3}") == 404);
	CHECK(m.loud_calls == 0);
	m.loud = true;
	m.heat_w = 9;
	CHECK(call("GET", "/api/v1/info", NULL) == 200);
	CHECK_HAS(out, "\"loudness\":{\"boost_max_db\":9.0,\"heat_cap_w\":{\"min\":2.0,\"max\":15.0,\"default\":9.0}}");
	CHECK(call("GET", "/api/v1/loudness", NULL) == 405);
	CHECK(call("POST", "/api/v1/loudness", "{}") == 400);
	CHECK(call("POST", "/api/v1/loudness", "{\"boost_db\":9.5}") == 400);
	CHECK(call("POST", "/api/v1/loudness", "{\"boost_db\":-1}") == 400);
	CHECK(call("POST", "/api/v1/loudness", "{\"boost_db\":1e999}") == 400);
	CHECK(call("POST", "/api/v1/loudness", "{\"heat_cap_w\":1}") == 400);
	CHECK(call("POST", "/api/v1/loudness", "{\"heat_cap_w\":15.5}") == 400);
	CHECK(call("POST", "/api/v1/loudness", "{\"boost_db\":\"6\"}") == 400);
	CHECK(call("POST", "/api/v1/loudness", "{\"boost_db\":3,\"heat_cap_w\":30}") == 400);
	CHECK(m.loud_calls == 0 && m.boost_db == 0); /* a bad half sets neither */
	CHECK(call("POST", "/api/v1/loudness", "{\"boost_db\":6}") == 200);
	CHECK(m.boost_db == 6 && m.heat_w == 9);
	CHECK_HAS(out, "\"boost\":{\"db\":6.0,\"limit_db\":-1.5,\"limiting\":true,\"limit_events\":7}");
	CHECK_HAS(out, "\"reason\":null,\"heat_cap_w\":9.0,\"heat_w\":3.2");
	CHECK_HAS(out, "\"hot\":false");
	CHECK(call("POST", "/api/v1/loudness", "{\"heat_cap_w\":13}") == 200);
	CHECK(m.boost_db == 6 && m.heat_w == 13);
	CHECK_HAS(out, "\"reason\":\"heat\",\"heat_cap_w\":13.0");
	CHECK_HAS(out, "\"hot\":true");
	CHECK(call("POST", "/api/v1/loudness", "{\"boost_db\":0,\"heat_cap_w\":9}") == 200);
	CHECK(m.boost_db == 0 && m.heat_w == 9 && m.loud_calls == 3);
	m.loud = false;
}

int main(void) {
	hifi_vol_init(&m.vol, 35, 100);
	m.ir_learning = -1;
	info_and_state();
	tones();
	amp_input_sub_control();
	volume_playing_ir();
	state_parts();
	selftest();
	console();
	core_routes();
	i2c_console();
	crash();
	bttest();
	loudness();
	return check_report("api");
}
