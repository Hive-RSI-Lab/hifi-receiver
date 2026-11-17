/* A whole board for the fuzzers: every op in hifi_ops_t, deterministic,
 * no hardware. fuzz_ops(full) gives all of them, or (full false) only the
 * ones every build has, so the "not in this build" paths are fuzzed too.
 * The first part is test_api.c's mock board. */
#pragma once

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hifi_api.h"
#include "hifi_ota.h"
#include "hifi_vol.h"

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
	bool core;        /* answer as Plus: the line in through the ADC */
	bool dj_guard;
	const char *dj_tripped;
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
	i->caps = (hifi_caps_t) {.dj_direct = true, .line = m.core, .eq_min_db = -12, .eq_max_db = 12, .sub_count = 2,
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
	s->dj_guard = m.dj_guard;
	s->dj_guard_w = 12.34f;
	s->dj_guard_limit_w = 14.2f;
	s->dj_guard_tripped = m.dj_tripped;
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
	s->heap_min = 60123;
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

static int m_capture(hifi_cap_src_t src, int16_t *pcm, int frames, hifi_cap_info_t *info, const char **err) {
	if (src == HIFI_CAP_LINE && !m.core) return *err = "no line in on this model", 409;
	for (int i = 0; i < 2 * frames; i++) pcm[i] = (int16_t) (i * 37);
	info->rate = 48000;
	info->ch[0] = "spk_l+", info->ch[1] = "spk_r+";
	info->v_per_fs[0] = 20.0f, info->v_per_fs[1] = NAN;
	return 0;
}
static int m_set_wifi(const char *ssid, const char *pw) { return ssid[0] && strlen(pw) < 64 ? 0 : -1; }
static void m_remote_status(hifi_remote_status_t *r) {
	r->configured = r->connected = true;
	snprintf(r->url, sizeof(r->url), "wss://hive.example/relay");
	snprintf(r->user, sizeof(r->user), "board");
	snprintf(r->dev, sizeof(r->dev), "240ac4123456");
	r->keys = 1;
}
static int m_remote_set(const char *url, const char *user, const char *pw, const char *prefix, const char *key) {
	(void) user, (void) pw, (void) prefix;
	return url[0] && (!key || strlen(key) <= 64) ? 0 : -1;
}
static void m_ota_status(hifi_ota_status_t *o) {
	o->slot = "ota_0";
	o->state = "valid";
	snprintf(o->last_from, sizeof(o->last_from), "ota_1");
	snprintf(o->last_reason, sizeof(o->last_reason), "task watchdog");
}
static int m_wifi_scan(hifi_wifi_ap_t *aps, int max) {
	int n = max < 3 ? max : 3;
	for (int i = 0; i < n; i++) {
		snprintf(aps[i].ssid, sizeof(aps[i].ssid), "net\"%d\\", i);
		aps[i].rssi = -40 - 10 * i;
		aps[i].secure = i & 1;
	}
	return n;
}
static int m_wifi_forget(void) { return 0; }
static void m_set_zone(bool on) { (void) on; }
static int m_update_check(hifi_update_offer_t *o, const char **err) {
	(void) err;
	snprintf(o->latest, sizeof(o->latest), "0.9.0");
	snprintf(o->url, sizeof(o->url), "http://hive.local/fw/lite.bin");
	snprintf(o->notes, sizeof(o->notes), "notes with \"quotes\"");
	return 0;
}
static int m_update_start(const char *url, const char *sha, bool need_sig, const char **err) {
	(void) sha, (void) need_sig;
	if (!url[0]) return *err = "no url", 400;
	return 0;
}
static int m_mode(const char *mode) { return strcmp(mode, "test") && strcmp(mode, "auto") ? -1 : 0; }
static const char *m_reset_reason(void) { return "panic"; }
static size_t m_last_log(char *out, size_t cap) { return (size_t) snprintf(out, cap, "E (812) boom \"x\"\n") % cap; }
static int m_bt_test(const char *sink, const char **err) { return sink[0] ? 0 : (*err = "no sink", -1); }

/* the flash an OTA writes to: a slot of 64 KiB that keeps nothing */
static int f_open(void *c, size_t size, const char **slot, const char **err) {
	(void) c, (void) size, (void) err;
	*slot = "ota_1";
	return 0;
}
static int f_write(void *c, const uint8_t *d, size_t n) { return (void) c, (void) d, (void) n, 0; }
static int f_commit(void *c, const char **err) { return (void) c, *err = "image does not verify", -1; }
static void f_abort(void *c) { (void) c; }
static const hifi_ota_io_t fuzz_io = {.open = f_open, .write = f_write, .commit = f_commit, .abort = f_abort,
				      .slot_size = 65536};
static hifi_ota_t fuzz_ota;

static hifi_ops_t fuzz_ops_v;

/* the sha256 an OTA the fuzzer opens says it has */
#define FUZZ_SHA "abababababababababababababababababababababababababababababababab"

/* a fresh board for each input, so one input never depends on another */
static const hifi_ops_t *fuzz_ops(bool full) {
	memset(&m, 0, sizeof(m));
	hifi_vol_init(&m.vol, 35, 100);
	m.ir_learning = -1;
	m.power_on = true;
	hifi_ota_init(&fuzz_ota, &fuzz_io);
	fuzz_ops_v = (hifi_ops_t) {
		.info = m_info, .snapshot = m_snapshot, .tone_start = m_tone_start, .tone_stop = m_tone_stop,
		.amp_enable = m_amp_enable, .amp_reset = m_amp_reset, .set_input = m_set_input,
		.set_sub = m_set_sub, .set_eq = m_set_eq, .set_name = m_set_name, .reboot = m_reboot,
		.factory_reset = m_factory_reset, .control = m_control, .selftest = m_selftest,
		.set_volume = m_set_volume, .ir_learn = m_ir_learn, .ir_forget = m_ir_forget};
	if (full) {
		fuzz_ops_v.capture = m_capture, fuzz_ops_v.set_wifi = m_set_wifi;
		fuzz_ops_v.remote_status = m_remote_status, fuzz_ops_v.remote_set = m_remote_set;
		fuzz_ops_v.ota = &fuzz_ota, fuzz_ops_v.ota_status = m_ota_status;
		fuzz_ops_v.wifi_scan = m_wifi_scan, fuzz_ops_v.wifi_forget = m_wifi_forget;
		fuzz_ops_v.set_zone = m_set_zone, fuzz_ops_v.update_check = m_update_check;
		fuzz_ops_v.update_start = m_update_start, fuzz_ops_v.set_led = m_mode, fuzz_ops_v.set_backlight = m_mode;
		fuzz_ops_v.reset_reason = m_reset_reason, fuzz_ops_v.last_log = m_last_log, fuzz_ops_v.bt_test = m_bt_test;
		m.core = true;
	}
	return &fuzz_ops_v;
}

/* a copy of the input with a NUL after it (the parsers take C strings) */
static char *fuzz_str(const uint8_t *d, size_t n) {
	char *s = malloc(n + 1);
	memcpy(s, d, n);
	s[n] = '\0';
	return s;
}

/* a property a fuzzer checks: a failure is a crash libFuzzer keeps */
#define FUZZ_ASSERT(c)                                                                   \
	do {                                                                             \
		if (!(c)) {                                                              \
			fprintf(stderr, "%s:%d: FUZZ_ASSERT(%s) failed\n", __FILE__, __LINE__, #c); \
			abort();                                                         \
		}                                                                        \
	} while (0)
