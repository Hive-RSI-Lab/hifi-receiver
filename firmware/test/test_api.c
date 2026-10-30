#include "check.h"
#include "hifi_api.h"
#include "hifi_json.h"
#include "hifi_vol.h"

/* ------------------------------------------------------------ mock board */

static struct {
	bool amp_enabled, amp_reset;
	int tone_starts, tone_stops;
	hifi_tone_params_t tone;
	bool tone_force;
	bool power_on;
	hifi_input_t input;
	float sub_db;
	int sub_on;
	float eq[HIFI_EQ_BANDS];
	char name[65];
	bool rebooted, reset;
	char action[16];
	hifi_vol_t vol;
	hifi_meta_t playing;
	int ir_learning, ir_forgot;
	bool ir_learned[HIFI_IR_N];
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
	i->caps = (hifi_caps_t) {.dj_direct = true, .eq_min_db = -12, .eq_max_db = 12, .sub_count = 2,
				 .zones = 1, .pdo_v_max = 20};
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
	s->level_dbfs = -23.4f;
	s->sub_on = m.sub_on != 0;
	s->sub_db = m.sub_db;
	memcpy(s->eq, m.eq, sizeof(m.eq));
	s->volume = m.vol.volume;
	s->mute = m.vol.mute;
	s->max_volume = m.vol.max_volume;
	s->playing = m.playing;
	s->ir_learning = m.ir_learning;
	memcpy(s->ir_learned, m.ir_learned, sizeof(m.ir_learned));
	s->net_mode = "sta";
	snprintf(s->ssid, sizeof(s->ssid), "home");
	snprintf(s->hostname, sizeof(s->hostname), "hifi-lite");
	snprintf(s->ip, sizeof(s->ip), "192.168.1.40");
	s->rssi = -61;
	s->heap_free = 81234;
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
	.set_sub = m_set_sub, .set_eq = m_set_eq, .set_name = m_set_name, .reboot = m_reboot,
	.factory_reset = m_factory_reset, .control = m_control, .selftest = m_selftest,
	.set_volume = m_set_volume, .ir_learn = m_ir_learn, .ir_forget = m_ir_forget};

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
		       "\"eq_min\":-12,\"eq_max\":12,\"sub\":{\"count\":2,\"crossover\":false,\"per_sub\":false},"
		       "\"zones\":1,\"eth\":false,\"pdo_v_max\":20}}");

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
		  "\"audio\":{\"source\":\"lms\",\"input\":\"stream\",\"tone\":null,\"level_dbfs\":-23.4,"
		  "\"sub\":{\"on\":false,\"level_db\":0.0},\"eq\":[0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0],"
		  "\"volume\":35,\"mute\":false,\"max_volume\":100,\"playing\":null},"
		  "\"net\":{\"mode\":\"sta\",\"ssid\":\"home\",\"hostname\":\"hifi-lite\",\"ip\":\"192.168.1.40\",\"rssi\":-61},"
		  "\"sys\":{\"heap_free\":81234,\"psram_free\":3912345},"
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

	CHECK(call("POST", "/api/v1/sub", "{\"level_db\":6}") == 200);
	CHECK_HAS(out, "\"sub\":{\"on\":false,\"level_db\":4.5}"); /* what the codec could do */
	CHECK(call("POST", "/api/v1/sub", "{\"on\":true}") == 200);
	CHECK(m.sub_on == 1 && m.sub_db == 4.5f);
	CHECK(call("POST", "/api/v1/sub", "{\"level_db\":-40}") == 400);
	CHECK(call("POST", "/api/v1/sub", "{}") == 400);

	CHECK(call("POST", "/api/v1/eq", "{\"gains\":[1,2,3,4,5,6,7,8,9,-12]}") == 200);
	CHECK(m.eq[0] == 1 && m.eq[9] == -12);
	CHECK_HAS(out, "\"eq\":[1.0,2.0,3.0,4.0,5.0,6.0,7.0,8.0,9.0,-12.0]");
	CHECK(call("POST", "/api/v1/eq", "{\"gains\":[1,2,3]}") == 400);
	CHECK(call("POST", "/api/v1/eq", "{\"gains\":[1,2,3,4,5,6,7,8,9,13]}") == 400);
	CHECK(call("POST", "/api/v1/eq", "{\"gains\":[1,2,3,4,5,6,7,8,9,\"x\"]}") == 400);

	CHECK(call("POST", "/api/v1/name", "{\"name\":\"Kitchen\"}") == 200);
	CHECK_STR(m.name, "Kitchen");
	CHECK(call("POST", "/api/v1/name", "{\"name\":\"\"}") == 400);
	CHECK(call("POST", "/api/v1/name", "{\"name\":\"0123456789012345678901234567890123\"}") == 400);

	CHECK(call("POST", "/api/v1/reset", "{}") == 400);
	CHECK(!m.reset);
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
	char body[256], msg[512];
#define CLI(...)                                                                     \
	({                                                                           \
		const char *argv_[] = {__VA_ARGS__};                                 \
		hifi_cli_request(sizeof(argv_) / sizeof(argv_[0]), argv_, &method, &path, body, sizeof(body), msg, sizeof(msg)); \
	})

	CHECK(CLI("state"));
	CHECK_STR(method, "GET");
	CHECK_STR(path, "/api/v1/state");

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
	CHECK(CLI("input", "dj_direct"));
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
	bool sha_null;
	hifi_update_status_t upd;
} c;

static void c_snapshot(hifi_snapshot_t *s) {
	m_snapshot(s);
	s->zones = 2;
	s->zone_b_on = c.zone_b;
	s->eth_present = true;
	s->eth_up = true;
	s->update = c.upd;
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
static int c_update_check(hifi_update_offer_t *o, const char **err) {
	return hifi_update_parse("{\"latest\":\"0.2.0\",\"url\":\"plus.bin\",\"notes\":\"Zone B\"}",
				 "http://hive.local/fw/manifest.json", "hifi-plus", o, err) ? 502 : 0;
}
static int c_update_start(const char *url, const char *sha, const char **err) {
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
	.update_check = c_update_check, .update_start = c_update_start};

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
	CHECK(ccall("POST", "/api/v1/zone", "{\"zone\":\"a\",\"on\":true}") == 400);
	CHECK(ccall("POST", "/api/v1/zone", "{\"zone\":\"b\"}") == 400);
	CHECK(c.zone_b);

	CHECK(ccall("GET", "/api/v1/update/check", NULL) == 200);
	CHECK_STR(out, "{\"current\":\"0.1.0\",\"latest\":\"0.2.0\",\"url\":\"http://hive.local/fw/plus.bin\","
		       "\"sha256\":null,\"notes\":\"Zone B\"}");
	CHECK(ccall("POST", "/api/v1/update", "{\"url\":\"ftp://h/a.bin\"}") == 400);
	CHECK(ccall("POST", "/api/v1/update", "{}") == 400);
	CHECK(ccall("POST", "/api/v1/update", "{\"url\":\"http://hive.local/fw/plus.bin\"}") == 200);
	CHECK_STR(c.url, "http://hive.local/fw/plus.bin");
	CHECK(c.sha_null);
	CHECK(ccall("POST", "/api/v1/update", "{\"url\":\"http://h/b.bin\"}") == 409);
	CHECK_HAS(out, "already running");

	/* the state: zone B, Ethernet and the update, which Lite's leaves out */
	c.upd = (hifi_update_status_t) {.state = HIFI_UPD_DOWNLOADING, .progress = 42};
	CHECK(ccall("GET", "/api/v1/state", NULL) == 200);
	CHECK_HAS(out, "\"playing\":null,\"zone_b\":{\"on\":true,\"volume\":null}},");
	CHECK_HAS(out, "\"rssi\":-61,\"eth\":true},");
	CHECK_HAS(out, "\"update\":{\"state\":\"downloading\",\"progress\":42,\"error\":null}}");
	c.upd = (hifi_update_status_t) {.state = HIFI_UPD_ERROR, .progress = 10, .error = "sha256 mismatch"};
	CHECK(ccall("GET", "/api/v1/state", NULL) == 200);
	CHECK_HAS(out, "\"update\":{\"state\":\"error\",\"progress\":10,\"error\":\"sha256 mismatch\"}}");
}

int main(void) {
	hifi_vol_init(&m.vol, 35, 100);
	m.ir_learning = -1;
	info_and_state();
	tones();
	amp_input_sub_control();
	volume_playing_ir();
	selftest();
	console();
	core_routes();
	return check_report("api");
}
