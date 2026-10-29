#include "hifi_api.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hifi_b64.h"
#include "hifi_json.h"
#include "hifi_vol.h"

#define PREFIX "/api/v1/"

/* squeezelite-esp32's 10-band equalizer */
static const int EQ_BANDS_HZ[HIFI_EQ_BANDS] = {31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};

static int error(char *out, size_t cap, int status, const char *msg) {
	hj_t j;
	hj_init(&j, out, cap);
	hj_obj(&j, NULL);
	hj_str(&j, "error", msg);
	hj_obj_end(&j);
	return status;
}

static void write_tone(hj_t *j, const char *key, const hifi_tone_params_t *p) {
	hj_obj(j, key);
	hj_str(j, "type", hifi_tone_type_name(p->type));
	if (p->type == HIFI_TONE_SINE) hj_num(j, "freq_hz", p->freq_hz, 1);
	if (p->type == HIFI_TONE_SWEEP) {
		hj_num(j, "start_hz", p->start_hz, 1);
		hj_num(j, "end_hz", p->end_hz, 1);
	}
	if (p->type != HIFI_TONE_SILENCE) hj_num(j, "level_dbfs", p->level_dbfs, 1);
	if (p->type == HIFI_TONE_SINE) hj_str(j, "channel", hifi_channel_name(p->channel));
	hj_num(j, "duration_s", p->duration_s, 1);
	hj_obj_end(j);
}

static void write_amp(hj_t *j, const char *key, const hifi_snapshot_t *s) {
	hj_obj(j, key);
	hj_bool(j, "enabled", s->amp_enabled);
	hj_bool(j, "fault", s->amp_fault);
	hj_int(j, "faults", s->amp_faults);
	hj_bool(j, "latched", s->amp_latched);
	hj_num(j, "temp_c", s->amp_temp_c, 1);
	hj_obj_end(j);
}

static void str_or_null(hj_t *j, const char *key, const char *v) { hj_str(j, key, v && *v ? v : NULL); }

static void write_playing(hj_t *j, const hifi_meta_t *m) {
	const char *st = hifi_play_name(m->state);
	if (!st) {
		hj_null(j, "playing");
		return;
	}
	hj_obj(j, "playing");
	hj_str(j, "state", st);
	str_or_null(j, "title", m->title);
	str_or_null(j, "artist", m->artist);
	str_or_null(j, "album", m->album);
	if (m->elapsed_ms >= 0) hj_int(j, "elapsed_s", m->elapsed_ms / 1000);
	else hj_null(j, "elapsed_s");
	if (m->duration_ms > 0) hj_int(j, "duration_s", m->duration_ms / 1000);
	else hj_null(j, "duration_s");
	hj_obj_end(j);
}

void hifi_api_write_state(const hifi_snapshot_t *s, char *out, size_t cap) {
	hj_t j;
	hj_init(&j, out, cap);
	hj_obj(&j, NULL);
	hj_int(&j, "uptime_s", s->uptime_s);

	hj_obj(&j, "power");
	hj_str(&j, "state", hifi_power_state_name(s->pwr_state));
	hj_num(&j, "vbus_v", s->vbus_v, 2);
	hj_bool(&j, "pg", s->pg);
	hj_obj(&j, "contract");
	if (s->contract.known) hj_str(&j, "protocol", hifi_proto_name(s->contract.proto));
	else hj_null(&j, "protocol");
	hj_bool(&j, "epr", s->contract.epr);
	hj_num(&j, "v", s->contract.v, 1);
	hj_num(&j, "i_max_a", s->contract.i_max_a, 2);
	hj_num(&j, "w", s->contract.w, 1);
	hj_obj_end(&j);
	hj_bool(&j, "pvdd_on", s->pvdd_on);
	hj_num(&j, "pvdd_v", s->pvdd_v, 2);
	hj_num(&j, "vbus_i_a", s->vbus_i_a, 2);
	hj_obj_end(&j);

	write_amp(&j, "amp", s);

	hj_obj(&j, "governor");
	hj_num(&j, "budget_w", s->gov_budget_w, 1);
	hj_num(&j, "in_w", s->gov_in_w, 1);
	hj_num(&j, "out_w", s->gov_out_w, 1);
	hj_num(&j, "gain_db", s->gov_gain_db, 1);
	hj_bool(&j, "limiting", s->gov_limiting);
	hj_int(&j, "limit_events", s->gov_events);
	hj_obj_end(&j);

	hj_obj(&j, "audio");
	hj_str(&j, "source", s->source ? s->source : "idle");
	hj_str(&j, "input", s->input == HIFI_INPUT_DJ_DIRECT ? "dj_direct" : "stream");
	if (s->tone_active) write_tone(&j, "tone", &s->tone);
	else hj_null(&j, "tone");
	hj_num(&j, "level_dbfs", s->level_dbfs, 1);
	hj_obj(&j, "sub");
	hj_bool(&j, "on", s->sub_on);
	hj_num(&j, "level_db", s->sub_db, 1);
	hj_obj_end(&j);
	hj_arr(&j, "eq");
	for (int i = 0; i < HIFI_EQ_BANDS; i++) hj_num(&j, NULL, s->eq[i], 1);
	hj_arr_end(&j);
	hj_int(&j, "volume", s->volume);
	hj_bool(&j, "mute", s->mute);
	hj_int(&j, "max_volume", s->max_volume);
	write_playing(&j, &s->playing);
	hj_obj_end(&j);

	hj_obj(&j, "net");
	hj_str(&j, "mode", s->net_mode ? s->net_mode : "sta");
	hj_str(&j, "ssid", s->ssid[0] ? s->ssid : NULL);
	hj_str(&j, "hostname", s->hostname);
	hj_str(&j, "ip", s->ip[0] ? s->ip : NULL);
	if (s->rssi) hj_int(&j, "rssi", s->rssi);
	else hj_null(&j, "rssi");
	hj_obj_end(&j);

	hj_obj(&j, "sys");
	hj_int(&j, "heap_free", s->heap_free);
	hj_int(&j, "psram_free", s->psram_free);
	hj_obj_end(&j);

	hj_obj(&j, "ir");
	hj_str(&j, "learning", s->ir_learning >= 0 ? hifi_ir_action_name(s->ir_learning) : NULL);
	hj_arr(&j, "learned");
	for (int a = 0; a < HIFI_IR_N; a++)
		if (s->ir_learned[a]) hj_str(&j, NULL, hifi_ir_action_name(a));
	hj_arr_end(&j);
	if (s->ir_have_last) {
		char code[11];
		snprintf(code, sizeof(code), "0x%08lx", (unsigned long) s->ir_last_code);
		hj_str(&j, "last_code", code);
	} else {
		hj_null(&j, "last_code");
	}
	hj_obj_end(&j);

	hj_obj_end(&j);
}

void hifi_selftest_add(hifi_selftest_t *t, const char *name, bool pass, const char *fmt, ...) {
	if (t->n >= HIFI_SELFTEST_MAX) return;
	hifi_check_t *c = &t->c[t->n++];
	snprintf(c->name, sizeof(c->name), "%s", name);
	c->pass = pass;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(c->value, sizeof(c->value), fmt, ap);
	va_end(ap);
}

/* ------------------------------------------------------------------ routes */

static int r_info(const hifi_ops_t *ops, char *out, size_t cap) {
	hifi_info_t i = {0};
	ops->info(&i);
	hj_t j;
	hj_init(&j, out, cap);
	hj_obj(&j, NULL);
	hj_str(&j, "model", i.model);
	hj_str(&j, "board", i.board);
	hj_str(&j, "fw", i.fw);
	hj_str(&j, "upstream", i.upstream);
	hj_int(&j, "api", HIFI_API_VERSION);
	hj_str(&j, "name", i.name);
	hj_str(&j, "mac", i.mac);
	hj_str(&j, "hostname", i.hostname);
	hj_int(&j, "flash_mb", i.flash_mb);
	hj_int(&j, "psram_mb", i.psram_mb);
	hj_obj(&j, "caps");
	hj_arr(&j, "line_modes");
	if (i.caps.dj_direct) hj_str(&j, NULL, "dj_direct");
	hj_arr_end(&j);
	hj_arr(&j, "eq_bands");
	for (int b = 0; b < HIFI_EQ_BANDS; b++) hj_int(&j, NULL, EQ_BANDS_HZ[b]);
	hj_arr_end(&j);
	hj_num(&j, "eq_min", i.caps.eq_min_db, 0);
	hj_num(&j, "eq_max", i.caps.eq_max_db, 0);
	hj_obj(&j, "sub");
	hj_int(&j, "count", i.caps.sub_count);
	hj_bool(&j, "crossover", i.caps.sub_crossover);
	hj_bool(&j, "per_sub", i.caps.sub_per_sub);
	hj_obj_end(&j);
	hj_int(&j, "zones", i.caps.zones);
	hj_bool(&j, "eth", i.caps.eth);
	hj_num(&j, "pdo_v_max", i.caps.pdo_v_max, 0);
	hj_obj_end(&j);
	hj_obj_end(&j);
	return 200;
}

static int r_state(const hifi_ops_t *ops, char *out, size_t cap) {
	hifi_snapshot_t *s = calloc(1, sizeof(*s));
	if (!s) return error(out, cap, 500, "out of memory");
	ops->snapshot(s);
	hifi_api_write_state(s, out, cap);
	free(s);
	return 200;
}

static double num_or(const char *body, const char *key, double dflt) {
	double v;
	return hj_get_num(body, key, &v) ? v : dflt;
}

static int r_tone(const hifi_ops_t *ops, const char *body, char *out, size_t cap) {
	char type[16] = "", channel[8] = "both";
	bool force = false;
	if (!hj_get_str(body, "type", type, sizeof(type))) return error(out, cap, 400, "type is required");
	hj_get_bool(body, "force", &force);

	hj_t j;
	if (!strcmp(type, "stop")) {
		ops->tone_stop();
		hj_init(&j, out, cap);
		hj_obj(&j, NULL);
		hj_bool(&j, "ok", true);
		hj_null(&j, "tone");
		hj_obj_end(&j);
		return 200;
	}

	hifi_tone_params_t p = {
		.type = hifi_tone_type_from(type),
		.freq_hz = (float) num_or(body, "freq_hz", 1000),
		.start_hz = (float) num_or(body, "start_hz", 20),
		.end_hz = (float) num_or(body, "end_hz", 20000),
		.level_dbfs = (float) num_or(body, "level_dbfs", -20),
		.duration_s = (float) num_or(body, "duration_s", 5),
	};
	if (hj_get_str(body, "channel", channel, sizeof(channel))) {
		if (!strcmp(channel, "left")) p.channel = HIFI_CH_LEFT;
		else if (!strcmp(channel, "right")) p.channel = HIFI_CH_RIGHT;
		else if (strcmp(channel, "both")) return error(out, cap, 400, "channel must be both, left or right");
	}
	const char *why = hifi_tone_check(&p, force);
	if (why) return error(out, cap, 400, why);

	const char *err = "can't play now";
	int st = ops->tone_start(&p, force, &err);
	if (st) return error(out, cap, st, err);

	hj_init(&j, out, cap);
	hj_obj(&j, NULL);
	hj_bool(&j, "ok", true);
	write_tone(&j, "tone", &p);
	hj_obj_end(&j);
	return 200;
}

static int r_amp(const hifi_ops_t *ops, const char *body, char *out, size_t cap) {
	bool v, any = false;
	if (hj_get_bool(body, "reset", &v)) {
		if (v) ops->amp_reset();
		any = true;
	}
	if (hj_get_bool(body, "enable", &v)) {
		ops->amp_enable(v);
		any = true;
	}
	if (!any) return error(out, cap, 400, "send {\"enable\": true|false} or {\"reset\": true}");

	hifi_snapshot_t *s = calloc(1, sizeof(*s));
	if (!s) return error(out, cap, 500, "out of memory");
	ops->snapshot(s);
	hj_t j;
	hj_init(&j, out, cap);
	write_amp(&j, NULL, s);
	free(s);
	return 200;
}

static int r_input(const hifi_ops_t *ops, const char *body, char *out, size_t cap) {
	char in[16];
	hifi_input_t v;
	if (!hj_get_str(body, "input", in, sizeof(in))) return error(out, cap, 400, "input is required");
	if (!strcmp(in, "stream")) v = HIFI_INPUT_STREAM;
	else if (!strcmp(in, "dj_direct")) v = HIFI_INPUT_DJ_DIRECT;
	else return error(out, cap, 400, "input must be stream or dj_direct");
	if (ops->set_input(v)) return error(out, cap, 500, "codec didn't take it");
	hj_t j;
	hj_init(&j, out, cap);
	hj_obj(&j, NULL);
	hj_bool(&j, "ok", true);
	hj_str(&j, "input", in);
	hj_obj_end(&j);
	return 200;
}

static int r_sub(const hifi_ops_t *ops, const char *body, char *out, size_t cap) {
	double db = NAN;
	bool on, has_on = hj_get_bool(body, "on", &on), has_db = hj_get_num(body, "level_db", &db);
	if (!has_on && !has_db) return error(out, cap, 400, "send level_db and/or on");
	if (has_db && (db < -30 || db > 6)) return error(out, cap, 400, "level_db must be -30 to +6");
	ops->set_sub(has_on ? on : -1, (float) db);
	return r_state(ops, out, cap);
}

static int r_eq(const hifi_ops_t *ops, const char *body, char *out, size_t cap) {
	double g[HIFI_EQ_BANDS];
	hifi_info_t i = {0};
	ops->info(&i);
	if (hj_get_num_array(body, "gains", g, HIFI_EQ_BANDS) != HIFI_EQ_BANDS)
		return error(out, cap, 400, "gains must be 10 numbers, 31 Hz to 16 kHz");
	float f[HIFI_EQ_BANDS];
	for (int b = 0; b < HIFI_EQ_BANDS; b++) {
		if (g[b] < i.caps.eq_min_db || g[b] > i.caps.eq_max_db) return error(out, cap, 400, "a gain is out of range");
		f[b] = (float) g[b];
	}
	if (ops->set_eq(f)) return error(out, cap, 500, "couldn't save the equalizer");
	return r_state(ops, out, cap);
}

static int r_name(const hifi_ops_t *ops, const char *body, char *out, size_t cap) {
	char name[65];
	if (!hj_get_str(body, "name", name, sizeof(name)) || !name[0]) return error(out, cap, 400, "name is required");
	if (strlen(name) > 32) return error(out, cap, 400, "name must be 32 characters or fewer");
	for (const char *c = name; *c; c++)
		if ((unsigned char) *c < 0x20) return error(out, cap, 400, "name has a control character");
	if (ops->set_name(name)) return error(out, cap, 500, "couldn't save the name");
	return r_state(ops, out, cap);
}

static int r_reboot(const hifi_ops_t *ops, const char *body, char *out, size_t cap, bool reset) {
	if (reset) {
		bool factory = false;
		if (!hj_get_bool(body, "factory", &factory) || !factory)
			return error(out, cap, 400, "send {\"factory\": true} to erase every setting");
		ops->factory_reset();
	} else {
		ops->reboot();
	}
	return r_state(ops, out, cap);
}

static int r_control(const hifi_ops_t *ops, const char *body, char *out, size_t cap) {
	char action[16];
	if (!hj_get_str(body, "action", action, sizeof(action))) return error(out, cap, 400, "action is required");
	if (ops->control(action)) return error(out, cap, 400, "unknown action");
	hj_t j;
	hj_init(&j, out, cap);
	hj_obj(&j, NULL);
	hj_bool(&j, "ok", true);
	hj_obj_end(&j);
	return 200;
}

/* true if the body has `key` with a value of another type than the caller read */
static bool other_type(const char *body, const char *key) {
	char t[2];
	bool b;
	double d;
	return hj_get_str(body, key, t, sizeof(t)) || hj_get_bool(body, key, &b) || hj_get_num(body, key, &d);
}

/* *out is a whole number lo..hi, or -1 when the key is missing (or null) */
static bool get_int(const char *body, const char *key, int lo, int hi, int *out) {
	double v;
	*out = -1;
	if (!hj_get_num(body, key, &v)) return !other_type(body, key);
	if (v != floor(v) || v < lo || v > hi) return false;
	*out = (int) v;
	return true;
}

static int r_volume(const hifi_ops_t *ops, const char *body, char *out, size_t cap) {
	int vol, max, m = -1;
	bool mute;
	if (hj_get_bool(body, "mute", &mute)) m = mute;
	else if (other_type(body, "mute")) return error(out, cap, 400, "mute must be true or false");
	if (!get_int(body, "volume", 0, 100, &vol)) return error(out, cap, 400, "volume must be a whole number 0 to 100");
	if (!get_int(body, "max_volume", HIFI_VOL_MAX_MIN, 100, &max))
		return error(out, cap, 400, "max_volume must be a whole number 30 to 100");
	if (vol == -1 && max == -1 && m == -1) return error(out, cap, 400, "send volume, mute and/or max_volume");
	if (ops->set_volume(vol, m, max)) return error(out, cap, 400, "volume out of range");
	return r_state(ops, out, cap);
}

static int r_ir(const hifi_ops_t *ops, const char *body, char *out, size_t cap, bool learn) {
	char name[16] = "";
	bool has = hj_get_str(body, "action", name, sizeof(name));
	int a = has ? hifi_ir_action_from(name) : -1;
	if ((learn || has) && a < 0)
		return error(out, cap, 400, "action must be volup, voldown, mute, toggle, next, prev, input or power");
	if (learn) ops->ir_learn(a);
	else ops->ir_forget(a);
	return r_state(ops, out, cap);
}

static int r_selftest(const hifi_ops_t *ops, char *out, size_t cap) {
	hifi_selftest_t *t = calloc(1, sizeof(*t));
	if (!t) return error(out, cap, 500, "out of memory");
	ops->selftest(t);
	bool pass = t->n > 0;
	for (int i = 0; i < t->n; i++) pass &= t->c[i].pass;
	hj_t j;
	hj_init(&j, out, cap);
	hj_obj(&j, NULL);
	hj_bool(&j, "pass", pass);
	hj_arr(&j, "checks");
	for (int i = 0; i < t->n; i++) {
		hj_obj(&j, NULL);
		hj_str(&j, "name", t->c[i].name);
		hj_bool(&j, "pass", t->c[i].pass);
		hj_str(&j, "value", t->c[i].value);
		hj_obj_end(&j);
	}
	hj_arr_end(&j);
	hj_obj_end(&j);
	free(t);
	return 200;
}

/* ------------------------------------------------------------------ remote bring-up */

static int r_ota_get(const hifi_ops_t *ops, char *out, size_t cap) {
	if (!ops->ota || !ops->ota_status) return error(out, cap, 404, "no OTA in this build");
	hifi_ota_status_t st = {0};
	ops->ota_status(&st);
	const hifi_ota_t *o = ops->ota;
	hifi_info_t i = {0};
	ops->info(&i);
	hj_t j;
	hj_init(&j, out, cap);
	hj_obj(&j, NULL);
	hj_str(&j, "slot", st.slot);
	hj_str(&j, "fw", i.fw);
	hj_str(&j, "state", st.state ? st.state : "unknown");
	if (o->active || o->done) {
		hj_obj(&j, "update");
		hj_bool(&j, "active", o->active);
		hj_int(&j, "size", (long long) o->size);
		hj_int(&j, "written", (long long) o->written);
		hj_str(&j, "version", o->version);
		hj_str(&j, "slot", o->slot);
		hj_obj_end(&j);
	} else {
		hj_null(&j, "update");
	}
	if (st.last_from[0] || st.last_reason[0]) {
		hj_obj(&j, "last_rollback");
		hj_str(&j, "from", st.last_from);
		hj_str(&j, "reason", st.last_reason);
		hj_obj_end(&j);
	} else {
		hj_null(&j, "last_rollback");
	}
	hj_obj_end(&j);
	return 200;
}

static int ok_reply(char *out, size_t cap) {
	snprintf(out, cap, "{\"ok\":true}");
	return 200;
}

static int r_ota(const hifi_ops_t *ops, const char *what, const char *body, char *out, size_t cap) {
	hifi_ota_t *o = ops->ota;
	if (!o) return error(out, cap, 404, "no OTA in this build");
	const char *err = "update failed";
	int st;
	hj_t j;
	if (!strcmp(what, "begin")) {
		double size;
		char sha[72] = "", version[32] = "";
		if (!hj_get_num(body, "size", &size) || size <= 0) return error(out, cap, 400, "size is required");
		hj_get_str(body, "sha256", sha, sizeof(sha));
		hj_get_str(body, "version", version, sizeof(version));
		if ((st = hifi_ota_begin(o, (size_t) size, sha, version, &err)) != 200) return error(out, cap, st, err);
		hj_init(&j, out, cap);
		hj_obj(&j, NULL);
		hj_bool(&j, "ok", true);
		hj_str(&j, "slot", o->slot);
		hj_int(&j, "chunk_max", HIFI_OTA_CHUNK_MAX);
		hj_obj_end(&j);
		return 200;
	}
	if (!strcmp(what, "chunk")) {
		double off;
		const char *data;
		size_t dlen;
		if (!hj_get_num(body, "off", &off) || off < 0) return error(out, cap, 400, "off is required");
		if (!hj_get_str_span(body, "data", &data, &dlen)) return error(out, cap, 400, "data (base64) is required");
		uint8_t *buf = malloc(HIFI_OTA_CHUNK_MAX);
		if (!buf) return error(out, cap, 500, "out of memory");
		long n = dlen <= hifi_b64_len(HIFI_OTA_CHUNK_MAX) ? hifi_b64_decode(data, dlen, buf, HIFI_OTA_CHUNK_MAX) : -1;
		st = n < 0 ? (err = "data isn't base64 of at most 4096 bytes", 400)
			   : hifi_ota_chunk(o, (size_t) off, buf, (size_t) n, &err);
		free(buf);
		hj_init(&j, out, cap);
		hj_obj(&j, NULL);
		if (st == 200) hj_bool(&j, "ok", true);
		else hj_str(&j, "error", err);
		hj_int(&j, "next", (long long) o->written); /* where the sender goes on from */
		hj_obj_end(&j);
		return st;
	}
	if (!strcmp(what, "finish")) {
		bool reboot = true;
		hj_get_bool(body, "reboot", &reboot);
		if ((st = hifi_ota_finish(o, &err)) != 200) return error(out, cap, st, err);
		if (reboot) ops->reboot(); /* after the reply has gone */
		hj_init(&j, out, cap);
		hj_obj(&j, NULL);
		hj_bool(&j, "ok", true);
		hj_bool(&j, "reboot", reboot);
		hj_str(&j, "slot", o->slot);
		hj_obj_end(&j);
		return 200;
	}
	if (!strcmp(what, "abort")) {
		hifi_ota_abort(o);
		return ok_reply(out, cap);
	}
	return error(out, cap, 404, "no such path");
}

static int capture_frames(const char *body) {
	double f;
	if (!body || !hj_get_num(body, "frames", &f)) return HIFI_CAPTURE_FRAMES_DEFAULT;
	return (int) f;
}

static int r_capture(const hifi_ops_t *ops, const char *body, char *out, size_t cap) {
	if (!ops->capture) return error(out, cap, 404, "no capture in this build");
	char src[16] = "loopback";
	hj_get_str(body, "source", src, sizeof(src));
	hifi_cap_src_t s;
	if (!strcmp(src, "loopback")) s = HIFI_CAP_LOOPBACK;
	else if (!strcmp(src, "line")) s = HIFI_CAP_LINE;
	else return error(out, cap, 400, "source must be loopback or line");
	int frames = capture_frames(body);
	if (frames < 64 || frames > HIFI_CAPTURE_FRAMES_MAX) return error(out, cap, 400, "frames must be 64 to 16384");
	size_t need = hifi_b64_len((size_t) frames * 4) + 1024;
	if (cap < need) return error(out, cap, 500, "reply buffer too small for this capture");

	int16_t *pcm = calloc((size_t) frames * 2, sizeof(int16_t));
	if (!pcm) return error(out, cap, 500, "out of memory");
	hifi_cap_info_t info = {.rate = 48000, .v_per_fs = {NAN, NAN}};
	const char *err = "can't capture now";
	int st = ops->capture(s, pcm, frames, &info, &err);
	if (st) {
		free(pcm);
		return error(out, cap, st, err);
	}
	hj_t j;
	hj_init(&j, out, cap);
	hj_obj(&j, NULL);
	hj_str(&j, "source", src);
	hj_int(&j, "rate", info.rate);
	hj_int(&j, "frames", frames);
	hj_arr(&j, "channels");
	for (int c = 0; c < 2; c++) hj_str(&j, NULL, info.ch[c]);
	hj_arr_end(&j);
	hj_arr(&j, "v_per_fs");
	for (int c = 0; c < 2; c++) hj_num(&j, NULL, info.v_per_fs[c], 3);
	hj_arr_end(&j);
	hj_arr(&j, "stats");
	for (int c = 0; c < 2; c++) {
		hifi_cap_stats_t cs;
		hifi_capture_stats(pcm, frames, c, &cs);
		hj_obj(&j, NULL);
		hj_num(&j, "rms_fs", cs.rms_fs, 6);
		hj_num(&j, "peak_fs", cs.peak_fs, 6);
		hj_num(&j, "dc_fs", cs.dc_fs, 6);
		hj_int(&j, "clipped", cs.clipped);
		hj_obj_end(&j);
	}
	hj_arr_end(&j);
	/* the samples, as one base64 string, written straight into the reply */
	hj_str(&j, "pcm", "");
	if (!j.overflow && j.len >= 2) {
		size_t at = j.len - 1; /* just after the opening quote */
		size_t room = cap - at;
		size_t n = hifi_b64_encode((const uint8_t *) pcm, (size_t) frames * 4, out + at, room);
		if (n && at + n + 3 < cap) {
			out[at + n] = '"';
			j.len = at + n + 1;
			out[j.len] = '\0';
		} else {
			j.overflow = true;
		}
	}
	hj_obj_end(&j);
	free(pcm);
	if (j.overflow) return error(out, cap, 500, "reply buffer too small for this capture");
	return 200;
}

static int r_wifi(const hifi_ops_t *ops, const char *body, char *out, size_t cap) {
	if (!ops->set_wifi) return error(out, cap, 404, "no Wi-Fi setup in this build");
	char ssid[33], pw[65] = "";
	if (!hj_get_str(body, "ssid", ssid, sizeof(ssid)) || !ssid[0]) return error(out, cap, 400, "ssid is required");
	hj_get_str(body, "password", pw, sizeof(pw));
	if (ops->set_wifi(ssid, pw)) return error(out, cap, 500, "couldn't start joining");
	return ok_reply(out, cap);
}

static int r_remote(const hifi_ops_t *ops, bool post, const char *body, char *out, size_t cap) {
	if (!ops->remote_status) return error(out, cap, 404, "no relay in this build");
	if (post) {
		char url[96] = "", user[48] = "", pw[96] = "", prefix[24] = "hifi";
		if (!ops->remote_set) return error(out, cap, 404, "no relay in this build");
		hj_get_str(body, "url", url, sizeof(url));
		if (url[0] && strncmp(url, "mqtts://", 8) && strncmp(url, "mqtt://", 7))
			return error(out, cap, 400, "url must be mqtts://host:port (or mqtt:// on a LAN), or empty to turn it off");
		hj_get_str(body, "user", user, sizeof(user));
		hj_get_str(body, "password", pw, sizeof(pw));
		hj_get_str(body, "prefix", prefix, sizeof(prefix));
		if (!prefix[0] || strpbrk(prefix, "+#/ ")) return error(out, cap, 400, "prefix must be one topic level");
		if (ops->remote_set(url, user, pw, prefix)) return error(out, cap, 500, "couldn't save the relay settings");
	}
	hifi_remote_status_t r = {0};
	ops->remote_status(&r);
	hj_t j;
	hj_init(&j, out, cap);
	hj_obj(&j, NULL);
	hj_bool(&j, "configured", r.configured);
	hj_bool(&j, "connected", r.connected);
	hj_str(&j, "url", r.url[0] ? r.url : NULL);
	hj_str(&j, "user", r.user[0] ? r.user : NULL);
	hj_str(&j, "prefix", r.prefix);
	hj_str(&j, "dev", r.dev);
	hj_int(&j, "keys", r.keys);
	hj_int(&j, "accepted", r.accepted);
	hj_int(&j, "rejected", r.rejected);
	hj_obj_end(&j);
	return 200;
}

size_t hifi_api_reply_cap(const char *path, const char *body) {
	if (path && strstr(path, PREFIX "capture") == path) {
		int f = capture_frames(body);
		if (f < 64) f = 64;
		if (f > HIFI_CAPTURE_FRAMES_MAX) f = HIFI_CAPTURE_FRAMES_MAX;
		return hifi_b64_len((size_t) f * 4) + 1024;
	}
	return 4096;
}

int hifi_api_handle(const hifi_ops_t *ops, const char *method, const char *path,
		    const char *body, char *out, size_t cap) {
	char route[32];
	size_t plen = strlen(PREFIX);
	if (strncmp(path, PREFIX, plen)) return error(out, cap, 404, "no such path");
	snprintf(route, sizeof(route), "%s", path + plen);
	route[strcspn(route, "?")] = '\0';
	bool get = !strcmp(method, "GET"), post = !strcmp(method, "POST");

	if (post && (!body || !*body)) body = "{}";
	if (post && !hj_valid_object(body)) return error(out, cap, 400, "body must be a JSON object");

	int status;
	if (!strcmp(route, "info")) status = get ? r_info(ops, out, cap) : -1;
	else if (!strcmp(route, "state")) status = get ? r_state(ops, out, cap) : -1;
	else if (!strcmp(route, "tone")) status = post ? r_tone(ops, body, out, cap) : -1;
	else if (!strcmp(route, "amp")) status = post ? r_amp(ops, body, out, cap) : -1;
	else if (!strcmp(route, "input")) status = post ? r_input(ops, body, out, cap) : -1;
	else if (!strcmp(route, "sub")) status = post ? r_sub(ops, body, out, cap) : -1;
	else if (!strcmp(route, "eq")) status = post ? r_eq(ops, body, out, cap) : -1;
	else if (!strcmp(route, "name")) status = post ? r_name(ops, body, out, cap) : -1;
	else if (!strcmp(route, "reboot")) status = post ? r_reboot(ops, body, out, cap, false) : -1;
	else if (!strcmp(route, "reset")) status = post ? r_reboot(ops, body, out, cap, true) : -1;
	else if (!strcmp(route, "control")) status = post ? r_control(ops, body, out, cap) : -1;
	else if (!strcmp(route, "volume")) status = post ? r_volume(ops, body, out, cap) : -1;
	else if (!strcmp(route, "ir/learn")) status = post ? r_ir(ops, body, out, cap, true) : -1;
	else if (!strcmp(route, "ir/forget")) status = post ? r_ir(ops, body, out, cap, false) : -1;
	else if (!strcmp(route, "selftest")) status = post ? r_selftest(ops, out, cap) : -1;
	else if (!strcmp(route, "ota")) status = get ? r_ota_get(ops, out, cap) : -1;
	else if (!strncmp(route, "ota/", 4)) status = post ? r_ota(ops, route + 4, body, out, cap) : -1;
	else if (!strcmp(route, "capture")) status = post ? r_capture(ops, body, out, cap) : -1;
	else if (!strcmp(route, "wifi")) status = post ? r_wifi(ops, body, out, cap) : -1;
	else if (!strcmp(route, "remote")) status = r_remote(ops, post, body, out, cap);
	else return error(out, cap, 404, "no such path");

	if (status < 0) return error(out, cap, 405, "wrong method for this path");
	return status;
}

/* ------------------------------------------------------------------ console */

static const char USAGE[] =
	"usage: hifi info|state|selftest | tone sine <hz> <dBFS> [both|left|right] [s] | "
	"tone sweep <from> <to> <dBFS> [s] | tone noise <dBFS> [s] | tone silence [s] | tone stop | "
	"amp on|off|reset | input stream|dj_direct | sub <dB> | control <action> | "
	"volume <0-100> | mute on|off | maxvol <30-100> | ir learn <action> | ir forget [action]";

static bool usage(char *out, size_t cap) {
	snprintf(out, cap, "%s", USAGE);
	return false;
}

static bool is_num(const char *s) {
	char *end;
	if (!s || !*s) return false;
	strtod(s, &end);
	return *end == '\0';
}

bool hifi_cli_request(int argc, const char *const *argv, const char **method,
		      const char **path, char *body, size_t bc, char *out, size_t cap) {
	*method = "POST";
	body[0] = '\0';
	if (argc < 1) return usage(out, cap);
	const char *cmd = argv[0];

	if (!strcmp(cmd, "info") || !strcmp(cmd, "state")) {
		*method = "GET";
		*path = !strcmp(cmd, "info") ? PREFIX "info" : PREFIX "state";
		return argc == 1 || usage(out, cap);
	}
	if (!strcmp(cmd, "selftest")) {
		*path = PREFIX "selftest";
		snprintf(body, bc, "{}");
		return argc == 1 || usage(out, cap);
	}
	if (!strcmp(cmd, "amp") && argc == 2) {
		*path = PREFIX "amp";
		if (!strcmp(argv[1], "on")) snprintf(body, bc, "{\"enable\":true}");
		else if (!strcmp(argv[1], "off")) snprintf(body, bc, "{\"enable\":false}");
		else if (!strcmp(argv[1], "reset")) snprintf(body, bc, "{\"reset\":true}");
		else return usage(out, cap);
		return true;
	}
	if (!strcmp(cmd, "input") && argc == 2) {
		*path = PREFIX "input";
		snprintf(body, bc, "{\"input\":\"%s\"}", argv[1]);
		return !strcmp(argv[1], "stream") || !strcmp(argv[1], "dj_direct") || usage(out, cap);
	}
	if (!strcmp(cmd, "sub") && argc == 2 && is_num(argv[1])) {
		*path = PREFIX "sub";
		snprintf(body, bc, "{\"level_db\":%s}", argv[1]);
		return true;
	}
	if (!strcmp(cmd, "control") && argc == 2 && strlen(argv[1]) < 16 && !strpbrk(argv[1], "\"\\")) {
		*path = PREFIX "control";
		snprintf(body, bc, "{\"action\":\"%s\"}", argv[1]);
		return true;
	}
	if (!strcmp(cmd, "volume") && argc == 2 && is_num(argv[1])) {
		*path = PREFIX "volume";
		snprintf(body, bc, "{\"volume\":%s}", argv[1]);
		return true;
	}
	if (!strcmp(cmd, "maxvol") && argc == 2 && is_num(argv[1])) {
		*path = PREFIX "volume";
		snprintf(body, bc, "{\"max_volume\":%s}", argv[1]);
		return true;
	}
	if (!strcmp(cmd, "mute") && argc == 2) {
		*path = PREFIX "volume";
		if (!strcmp(argv[1], "on")) snprintf(body, bc, "{\"mute\":true}");
		else if (!strcmp(argv[1], "off")) snprintf(body, bc, "{\"mute\":false}");
		else return usage(out, cap);
		return true;
	}
	if (!strcmp(cmd, "ir") && argc >= 2 && argc <= 3) {
		bool learn = !strcmp(argv[1], "learn");
		if (!learn && strcmp(argv[1], "forget")) return usage(out, cap);
		if (learn && argc != 3) return usage(out, cap);
		if (argc == 3 && hifi_ir_action_from(argv[2]) < 0) return usage(out, cap);
		*path = learn ? PREFIX "ir/learn" : PREFIX "ir/forget";
		if (argc == 3) snprintf(body, bc, "{\"action\":\"%s\"}", argv[2]);
		else snprintf(body, bc, "{}");
		return true;
	}
	if (!strcmp(cmd, "tone") && argc >= 2) {
		const char *t = argv[1];
		int n = argc - 2;
		const char *const *a = argv + 2;
		*path = PREFIX "tone";
		for (int i = 0; i < n; i++) {
			bool chan = !strcmp(t, "sine") && i == 2;
			if (!chan && !is_num(a[i])) return usage(out, cap);
		}
		if (!strcmp(t, "stop") && n == 0) {
			snprintf(body, bc, "{\"type\":\"stop\"}");
		} else if (!strcmp(t, "sine") && n >= 2 && n <= 4) {
			const char *ch = n >= 3 ? a[2] : "both";
			if (strcmp(ch, "both") && strcmp(ch, "left") && strcmp(ch, "right")) return usage(out, cap);
			snprintf(body, bc, "{\"type\":\"sine\",\"freq_hz\":%s,\"level_dbfs\":%s,\"channel\":\"%s\",\"duration_s\":%s}",
				 a[0], a[1], ch, n == 4 ? a[3] : "5");
		} else if (!strcmp(t, "sweep") && n >= 3 && n <= 4) {
			snprintf(body, bc, "{\"type\":\"sweep\",\"start_hz\":%s,\"end_hz\":%s,\"level_dbfs\":%s,\"duration_s\":%s}",
				 a[0], a[1], a[2], n == 4 ? a[3] : "10");
		} else if (!strcmp(t, "noise") && n >= 1 && n <= 2) {
			snprintf(body, bc, "{\"type\":\"noise\",\"level_dbfs\":%s,\"duration_s\":%s}", a[0], n == 2 ? a[1] : "5");
		} else if (!strcmp(t, "silence") && n <= 1) {
			snprintf(body, bc, "{\"type\":\"silence\",\"duration_s\":%s}", n == 1 ? a[0] : "5");
		} else {
			return usage(out, cap);
		}
		return true;
	}
	return usage(out, cap);
}
