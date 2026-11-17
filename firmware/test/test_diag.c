/* On-board diagnostics: CPU load, the reset history, the writers' paging and
 * size limit, and the diag routes through the API with a mock board. */
#include <stdlib.h>

#include "check.h"
#include "hifi_api.h"
#include "hifi_diag.h"
#include "hifi_json.h"

static void history(void) {
	char h[HIFI_DIAG_HIST_CAP] = "";
	hifi_diag_hist_push(h, sizeof(h), "power_on");
	CHECK_STR(h, "power_on");
	hifi_diag_hist_push(h, sizeof(h), "panic");
	CHECK_STR(h, "panic,power_on");
	hifi_diag_hist_push(h, sizeof(h), NULL);
	CHECK_STR(h, "unknown,panic,power_on");
	for (int i = 0; i < 10; i++) hifi_diag_hist_push(h, sizeof(h), "sw");
	/* at most HIFI_DIAG_RESETS, newest first */
	CHECK_STR(h, "sw,sw,sw,sw,sw,sw,sw,sw");
	hifi_diag_hist_push(h, sizeof(h), "brownout");
	CHECK_STR(h, "brownout,sw,sw,sw,sw,sw,sw,sw");

	/* a small buffer keeps whole reasons only */
	char s[16] = "";
	hifi_diag_hist_push(s, sizeof(s), "task_wdt");
	hifi_diag_hist_push(s, sizeof(s), "panic");
	CHECK_STR(s, "panic,task_wdt");
	hifi_diag_hist_push(s, sizeof(s), "int_wdt");
	CHECK_STR(s, "int_wdt,panic");
	char t[4] = "";
	hifi_diag_hist_push(t, sizeof(t), "brownout");
	CHECK_STR(t, "");
}

static hifi_diag_task_t task(const char *name, uint32_t num, uint32_t rt, int core) {
	hifi_diag_task_t t = {.num = num, .runtime = rt, .stack_free = 1000 + num, .core = core, .prio = (int) num};
	snprintf(t.name, sizeof(t.name), "%s", name);
	return t;
}

static void cpu(void) {
	hifi_diag_task_t a[] = {task("IDLE0", 1, 100, 0), task("IDLE1", 2, 200, 1), task("wifi", 3, 0xfffffff0u, 0),
				task("gone", 4, 50, -1)};
	hifi_diag_tasks_t b = {.n = 4, .sample_ms = 1000};
	b.t[0] = task("IDLE0", 1, 100 + 600000, 0);
	b.t[1] = task("IDLE1", 2, 200 + 900000, 1);
	b.t[2] = task("wifi", 3, 0xfffffff0u + 100010u, 0); /* wraps */
	b.t[3] = task("new", 5, 250000, 1);                 /* started during the sample */
	hifi_diag_cpu(a, 4, &b, 1000000);
	CHECK_STR(b.t[0].name, "IDLE1"); /* busiest first */
	CHECK_NEAR(b.t[0].cpu_pct, 90, 0.01);
	CHECK_STR(b.t[1].name, "IDLE0");
	CHECK_NEAR(b.t[1].cpu_pct, 60, 0.01);
	CHECK_STR(b.t[2].name, "new");
	CHECK_NEAR(b.t[2].cpu_pct, 25, 0.01);
	CHECK_STR(b.t[3].name, "wifi");
	CHECK_NEAR(b.t[3].cpu_pct, 10.001, 0.01);
	CHECK_NEAR(b.idle_pct[0], 60, 0.01);
	CHECK_NEAR(b.idle_pct[1], 90, 0.01);

	/* no elapsed time: no figures, and nothing divides by zero */
	hifi_diag_tasks_t c = {.n = 1};
	c.t[0] = task("x", 1, 5, 0);
	hifi_diag_cpu(NULL, 0, &c, 0);
	CHECK(isnan(c.t[0].cpu_pct));
	CHECK(isnan(c.idle_pct[0]) && isnan(c.idle_pct[1]));

	/* a counter that ran past elapsed (rounding) is clamped */
	hifi_diag_tasks_t d = {.n = 1};
	d.t[0] = task("y", 1, 1500, 0);
	hifi_diag_cpu(NULL, 0, &d, 1000);
	CHECK_NEAR(d.t[0].cpu_pct, 100, 0.01);
}

static void paging(void) {
	hifi_diag_tasks_t *t = calloc(1, sizeof(*t));
	t->n = HIFI_DIAG_TASKS_MAX;
	t->sample_ms = 1000;
	t->idle_pct[0] = 81.5f;
	t->idle_pct[1] = NAN;
	for (int i = 0; i < t->n; i++) {
		char name[17];
		snprintf(name, sizeof(name), "task_\"long\\%02d__", i & 63); /* escapes cost room */
		t->t[i] = task(name, (uint32_t) i, 0, i % 3 - 1);
		t->t[i].cpu_pct = i % 2 ? NAN : 12.25f;
		t->t[i].stack_free = 4000000000u;
	}
	char out[4096];
	int from = 0, pages = 0, seen = 0;
	while (from < t->n && pages < 20) {
		hifi_diag_write_tasks(t, from, out, sizeof(out));
		CHECK(strlen(out) <= HIFI_DIAG_PAGE);
		CHECK(hj_valid_object(out));
		double f = -1, next = -1, n = -1;
		CHECK(hj_get_num(out, "from", &f) && f == from);
		CHECK(hj_get_num(out, "n", &n) && n == t->n);
		CHECK(hj_get_num(out, "next", &next) && next > from);
		/* every row on the page, counted by its name */
		for (const char *p = out; (p = strstr(p, "task_")); p++) seen++;
		from = (int) next;
		pages++;
	}
	CHECK(from == t->n);
	CHECK(seen == t->n);
	CHECK(pages >= 3);
	CHECK_HAS(out, "\"idle_pct\":[81.5,null]");
	CHECK_HAS(out, "\"cols\":[\"name\",\"stack_free\",\"core\",\"prio\",\"cpu_pct\"]");

	/* past the end: an empty page that says so */
	hifi_diag_write_tasks(t, 999, out, sizeof(out));
	CHECK_HAS(out, "\"rows\":[]");
	CHECK_HAS(out, "\"next\":48");

	/* one row's shape */
	hifi_diag_tasks_t one = {.n = 1, .sample_ms = 1000, .idle_pct = {NAN, NAN}};
	one.t[0] = task("loudness", 7, 0, -1);
	one.t[0].cpu_pct = 3.14f;
	hifi_diag_write_tasks(&one, 0, out, sizeof(out));
	CHECK_HAS(out, "\"rows\":[[\"loudness\",1007,null,7,3.1]],\"next\":1}");
	free(t);
}

static void writers(void) {
	char out[2048];
	hifi_diag_sys_t s = {
		.uptime_s = 4000000000u, .boot_app_ms = 812, .boot_ip_ms = 4120,
		.internal = {.free = 17743, .min = 16539, .largest = 9204, .total = 300000},
		.reset_reason = "power_on", .boots = 4000000000u,
		.nvs = {{"nvs", 4000000000u, 4000000000u, 4000000000u, 4000000000u}, {"settings", 10, 20, 30, 2}},
		.power = {.known = true, .state = "playing", .vbus_v = 19.76f, .vbus_i_a = NAN, .pvdd_v = 19.5f,
			  .proto = "pd", .contract_v = 20, .contract_a = 1.5f, .pg = true, .amp_on = true,
			  .amp_faults = 3, .amp_temp_c = NAN},
	};
	for (int i = 0; i < 12; i++) hifi_diag_hist_push(s.reset_hist, sizeof(s.reset_hist), "task_wdt_long_name");
	hifi_diag_write_sys(&s, out, sizeof(out));
	CHECK(strlen(out) <= HIFI_DIAG_PAGE);
	CHECK(hj_valid_object(out));
	CHECK_HAS(out, "\"boot_ms\":{\"app\":812,\"ip\":4120,\"link\":null}");
	CHECK_HAS(out, "\"heap\":{\"free\":17743,\"min\":16539,\"largest\":9204,\"total\":300000}");
	CHECK_HAS(out, "\"psram\":null");
	CHECK_HAS(out, "\"settings\":{\"used\":10,\"free\":20,\"total\":30,\"namespaces\":2,\"used_pct\":33.3}");
	CHECK_HAS(out, "\"vbus_v\":19.76");
	CHECK_HAS(out, "\"vbus_i_a\":null");
	CHECK_HAS(out, "\"contract_v\":20.0");
	CHECK_HAS(out, "\"amp_temp_c\":null");
	CHECK_HAS(out, "\"amp_faults\":3");

	hifi_diag_net_t n = {.rssi = -48, .channel = 6, .bssid = "aa:bb:cc:dd:ee:ff", .phy = "11n", .connects = 2,
			     .disconnects = 1, .last_reason = 8, .up_s = 100, .link_up = true, .link_drops = 1,
			     .link_up_s = 90, .tx_bytes = 5000000000ull, .rx_bytes = 12};
	hifi_diag_write_net(&n, out, sizeof(out));
	CHECK(hj_valid_object(out));
	CHECK_HAS(out, "\"rssi\":-48,\"channel\":6");
	CHECK_HAS(out, "\"last_reason\":8");
	CHECK_HAS(out, "\"tx_bytes\":5000000000");
	hifi_diag_net_t off = {0};
	hifi_diag_write_net(&off, out, sizeof(out));
	CHECK_HAS(out, "\"rssi\":null");
	CHECK_HAS(out, "\"bssid\":null");
	CHECK_HAS(out, "\"last_reason\":null");

	hifi_diag_audio_t a = {.known = true, .underruns = 2, .rate = 44100, .buffer_ms = 1800, .dma_ms = 46, .frames_ms = 65000};
	hifi_diag_write_audio(&a, out, sizeof(out));
	CHECK_STR(out, "{\"underruns\":2,\"rate\":44100,\"buffer_ms\":1800,\"dma_ms\":46,\"latency_ms\":1846,\"played_s\":65}");
	hifi_diag_write_audio(&(hifi_diag_audio_t) {0}, out, sizeof(out));
	CHECK_STR(out, "{\"underruns\":null}");

	hifi_diag_ota_t o = {.known = true, .bytes = 1500000, .total_ms = 30000, .write_ms = 6000, .version = "0.1.0", .boot = 9};
	hifi_diag_write_ota(&o, out, sizeof(out));
	CHECK_HAS(out, "\"kbps\":400.0,\"write_kbps\":2000.0,\"boot\":9");
	hifi_diag_write_ota(&(hifi_diag_ota_t) {0}, out, sizeof(out));
	CHECK_STR(out, "{\"version\":null}");

	hifi_diag_write_echo(&(hifi_diag_echo_t) {131072, 512}, out, sizeof(out));
	CHECK_STR(out, "{\"bytes\":131072,\"ms\":512,\"kbps\":2048.0}");
	hifi_diag_write_echo(&(hifi_diag_echo_t) {0}, out, sizeof(out));
	CHECK_HAS(out, "\"kbps\":null");

	/* a small buffer never overflows, and still ends in a string */
	char tiny[24];
	hifi_diag_write_sys(&s, tiny, sizeof(tiny));
	CHECK(strlen(tiny) < sizeof(tiny));
}

/* ------------------------------------------------------------ through the API */

static struct {
	bool fresh;
	int tasks_calls;
	uint32_t echo_bytes;
	int echo_status;
} md;

static void m_snapshot(hifi_snapshot_t *s) {
	s->pwr_state = 0;
	s->vbus_v = 19.8f;
	s->vbus_i_a = NAN;
	s->pvdd_v = NAN;
	s->amp_temp_c = NAN;
	s->contract = (hifi_contract_t) {.known = true, .v = 20, .i_max_a = 1.5f};
}
static void m_sys(hifi_diag_sys_t *s) {
	s->uptime_s = 60;
	s->internal = (hifi_diag_heap_t) {1, 2, 3, 4};
	s->reset_reason = "sw";
	snprintf(s->reset_hist, sizeof(s->reset_hist), "sw,power_on");
	s->boots = 5;
}
static int m_tasks(hifi_diag_tasks_t *t, bool fresh) {
	md.fresh = fresh;
	md.tasks_calls++;
	t->n = 30;
	t->sample_ms = 1000;
	for (int i = 0; i < t->n; i++) t->t[i] = task("some_task_name", (uint32_t) i, 0, 0);
	return 0;
}
static void m_net(hifi_diag_net_t *n) { n->rssi = -50; }
static void m_audio(hifi_diag_audio_t *a) { a->known = true, a->underruns = 4; }
static void m_ota(hifi_diag_ota_t *o) { (void) o; }
static int m_echo(uint32_t bytes, hifi_diag_echo_t *e, const char **err) {
	md.echo_bytes = bytes;
	if (md.echo_status) {
		*err = "the remote link is down";
		return md.echo_status;
	}
	e->bytes = bytes;
	e->ms = 100;
	return 0;
}

static const hifi_diag_ops_t m_diag = {m_sys, m_tasks, m_net, m_audio, m_ota, m_echo};

static void api(void) {
	hifi_ops_t ops = {.snapshot = m_snapshot};
	char out[4096];
	CHECK(hifi_api_handle(&ops, "GET", "/api/v1/diag", NULL, out, sizeof(out)) == 404);
	ops.diag = &m_diag;
	CHECK(hifi_api_handle(&ops, "GET", "/api/v1/diag", NULL, out, sizeof(out)) == 200);
	CHECK_HAS(out, "\"resets\":\"sw,power_on\",\"boots\":5");
	CHECK_HAS(out, "\"power\":{\"state\":");
	CHECK_HAS(out, "\"contract_v\":20.0,\"contract_a\":1.50");
	CHECK(hifi_api_handle(&ops, "POST", "/api/v1/diag", "{}", out, sizeof(out)) == 200);
	CHECK(hifi_api_handle(&ops, "PUT", "/api/v1/diag", NULL, out, sizeof(out)) == 405);
	CHECK(hifi_api_handle(&ops, "GET", "/api/v1/diagnose", NULL, out, sizeof(out)) == 404);
	CHECK(hifi_api_handle(&ops, "GET", "/api/v1/diag/nope", NULL, out, sizeof(out)) == 404);

	CHECK(hifi_api_handle(&ops, "GET", "/api/v1/diag/tasks", NULL, out, sizeof(out)) == 200);
	CHECK(md.fresh);
	double next = 0;
	CHECK(hj_get_num(out, "next", &next) && next > 0 && next < 30);
	char body[32];
	snprintf(body, sizeof(body), "{\"from\":%d}", (int) next);
	CHECK(hifi_api_handle(&ops, "POST", "/api/v1/diag/tasks", body, out, sizeof(out)) == 200);
	CHECK(!md.fresh); /* later pages read the same sample */
	CHECK(hifi_api_handle(&ops, "POST", "/api/v1/diag/tasks", "{\"from\":0}", out, sizeof(out)) == 200);
	CHECK(md.fresh);
	CHECK(hifi_api_handle(&ops, "POST", "/api/v1/diag/tasks", "{\"from\":-1}", out, sizeof(out)) == 400);
	CHECK(hifi_api_handle(&ops, "POST", "/api/v1/diag/tasks", "{\"from\":1.5}", out, sizeof(out)) == 400);
	CHECK(hifi_api_handle(&ops, "POST", "/api/v1/diag/tasks", "{\"from\":\"x\"}", out, sizeof(out)) == 400);

	CHECK(hifi_api_handle(&ops, "GET", "/api/v1/diag/net", NULL, out, sizeof(out)) == 200);
	CHECK_HAS(out, "\"rssi\":-50");
	CHECK(hifi_api_handle(&ops, "GET", "/api/v1/diag/audio", NULL, out, sizeof(out)) == 200);
	CHECK_HAS(out, "\"underruns\":4");
	CHECK(hifi_api_handle(&ops, "GET", "/api/v1/diag/ota", NULL, out, sizeof(out)) == 200);
	CHECK_STR(out, "{\"version\":null}");

	CHECK(hifi_api_handle(&ops, "GET", "/api/v1/diag/echo", NULL, out, sizeof(out)) == 405);
	CHECK(hifi_api_handle(&ops, "POST", "/api/v1/diag/echo", "{}", out, sizeof(out)) == 200);
	CHECK(md.echo_bytes == HIFI_DIAG_ECHO_DEFAULT);
	CHECK(hifi_api_handle(&ops, "POST", "/api/v1/diag/echo", "{\"bytes\":1000}", out, sizeof(out)) == 200);
	CHECK_STR(out, "{\"bytes\":1000,\"ms\":100,\"kbps\":80.0}");
	CHECK(hifi_api_handle(&ops, "POST", "/api/v1/diag/echo", "{\"bytes\":0}", out, sizeof(out)) == 400);
	CHECK(hifi_api_handle(&ops, "POST", "/api/v1/diag/echo", "{\"bytes\":2000000}", out, sizeof(out)) == 400);
	md.echo_status = 409;
	CHECK(hifi_api_handle(&ops, "POST", "/api/v1/diag/echo", "{}", out, sizeof(out)) == 409);
	CHECK_HAS(out, "the remote link is down");

	/* a board with only some of them */
	hifi_diag_ops_t part = {.sys = m_sys};
	ops.diag = &part;
	CHECK(hifi_api_handle(&ops, "GET", "/api/v1/diag/tasks", NULL, out, sizeof(out)) == 404);
	CHECK(hifi_api_handle(&ops, "GET", "/api/v1/diag/net", NULL, out, sizeof(out)) == 404);
	CHECK(hifi_api_handle(&ops, "POST", "/api/v1/diag/echo", "{}", out, sizeof(out)) == 404);
}

static void cli(void) {
	const char *method, *path;
	char body[256], out[1024];
	const char *a1[] = {"diag"};
	CHECK(hifi_cli_request(1, a1, &method, &path, body, sizeof(body), out, sizeof(out)));
	CHECK_STR(method, "GET");
	CHECK_STR(path, "/api/v1/diag");
	const char *a2[] = {"diag", "tasks", "12"};
	CHECK(hifi_cli_request(3, a2, &method, &path, body, sizeof(body), out, sizeof(out)));
	CHECK_STR(method, "POST");
	CHECK_STR(path, "/api/v1/diag/tasks");
	CHECK_STR(body, "{\"from\":12}");
	const char *a3[] = {"diag", "net"};
	CHECK(hifi_cli_request(2, a3, &method, &path, body, sizeof(body), out, sizeof(out)));
	CHECK_STR(method, "GET");
	CHECK_STR(path, "/api/v1/diag/net");
	const char *a4[] = {"diag", "echo"};
	CHECK(hifi_cli_request(2, a4, &method, &path, body, sizeof(body), out, sizeof(out)));
	CHECK_STR(method, "POST");
	CHECK_STR(body, "{}");
	const char *a5[] = {"diag", "echo", "65536"};
	CHECK(hifi_cli_request(3, a5, &method, &path, body, sizeof(body), out, sizeof(out)));
	CHECK_STR(body, "{\"bytes\":65536}");
	const char *b1[] = {"diag", "net", "3"};
	CHECK(!hifi_cli_request(3, b1, &method, &path, body, sizeof(body), out, sizeof(out)));
	const char *b2[] = {"diag", "bogus"};
	CHECK(!hifi_cli_request(2, b2, &method, &path, body, sizeof(body), out, sizeof(out)));
	const char *b3[] = {"diag", "tasks", "x"};
	CHECK(!hifi_cli_request(3, b3, &method, &path, body, sizeof(body), out, sizeof(out)));
	CHECK_HAS(out, "diag [tasks");
}

int main(void) {
	history();
	cpu();
	paging();
	writers();
	api();
	cli();
	return check_report("diag");
}
