#include "check.h"
#include "hifi_api.h"
#include "hifi_json.h"

/* ------------------------------------------------------------ mock board */

static struct {
	bool amp_enabled, amp_reset;
	int tone_starts, tone_stops;
	hifi_tone_params_t tone;
	bool tone_force;
	bool power_on;
	hifi_input_t input;
	float sub_db;
	char action[16];
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
static float m_set_sub_db(float db) {
	m.sub_db = db;
	return db > 4.5f ? 4.5f : db;
}
static int m_control(const char *a) {
	snprintf(m.action, sizeof(m.action), "%s", a);
	return strcmp(a, "volup") && strcmp(a, "toggle") ? -1 : 0;
}
static void m_selftest(hifi_selftest_t *t) {
	hifi_selftest_add(t, "pd_sink_i2c", true, "0x%02x acks", 0x22);
	hifi_selftest_add(t, "network", m.power_on, m.power_on ? "192.168.1.40" : "not connected");
}

static const hifi_ops_t ops = {m_info, m_snapshot, m_tone_start, m_tone_stop, m_amp_enable,
			       m_amp_reset, m_set_input, m_set_sub_db, m_control, m_selftest};

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
		       "\"upstream\":\"squeezelite-esp32 master-v4.3 ee1720c\",\"api\":1,"
		       "\"mac\":\"24:0a:c4:12:34:56\",\"hostname\":\"squeezelite-123456\",\"flash_mb\":8,\"psram_mb\":8}");

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
		  "\"audio\":{\"source\":\"lms\",\"input\":\"stream\",\"tone\":null,\"level_dbfs\":-23.4},"
		  "\"net\":{\"ip\":\"192.168.1.40\",\"rssi\":-61},"
		  "\"sys\":{\"heap_free\":81234,\"psram_free\":3912345}}");

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
	CHECK_STR(out, "{\"ok\":true,\"level_db\":4.5}"); /* what the codec could do */
	CHECK(call("POST", "/api/v1/sub", "{\"level_db\":-40}") == 400);

	CHECK(call("POST", "/api/v1/control", "{\"action\":\"volup\"}") == 200);
	CHECK_STR(m.action, "volup");
	CHECK(call("POST", "/api/v1/control", "{\"action\":\"explode\"}") == 400);
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

int main(void) {
	info_and_state();
	tones();
	amp_input_sub_control();
	selftest();
	console();
	return check_report("api");
}
