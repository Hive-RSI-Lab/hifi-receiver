#include "hifi_api.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hifi_b64.h"
#include "hifi_gate.h"
#include "hifi_json.h"
#include "hifi_vol.h"
#include "hifi_ws_proto.h"

#define PREFIX "/api/v1/"

/* squeezelite-esp32's 10-band equalizer */
static const int EQ_BANDS_HZ[HIFI_EQ_BANDS] = {31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};

static const char *const INPUT_NAMES[] = {"stream", "dj_direct", "line"};

const char *hifi_input_name(hifi_input_t in) {
	return (unsigned) in < sizeof(INPUT_NAMES) / sizeof(INPUT_NAMES[0]) ? INPUT_NAMES[in] : "stream";
}

bool hifi_input_parse(const char *s, hifi_input_t *in) {
	for (unsigned i = 0; s && i < sizeof(INPUT_NAMES) / sizeof(INPUT_NAMES[0]); i++)
		if (!strcmp(s, INPUT_NAMES[i])) {
			*in = (hifi_input_t) i;
			return true;
		}
	return false;
}

static const char *const CHAN_NAMES[] = {"stereo", "left", "right", "mono"};

const char *hifi_chan_name(hifi_chan_t c) {
	return (unsigned) c < sizeof(CHAN_NAMES) / sizeof(CHAN_NAMES[0]) ? CHAN_NAMES[c] : "stereo";
}

bool hifi_chan_parse(const char *s, hifi_chan_t *c) {
	for (unsigned i = 0; s && i < sizeof(CHAN_NAMES) / sizeof(CHAN_NAMES[0]); i++)
		if (!strcmp(s, CHAN_NAMES[i])) {
			*c = (hifi_chan_t) i;
			return true;
		}
	return false;
}

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
	if (s->loudness) hj_bool(j, "hot", s->amp_hot);
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

static const char *const STATE_PARTS[] = {"power", "amp", "governor", "audio", "net", "sys", "ir", "controls", "update"};

bool hifi_api_state_part_known(const char *part) {
	if (!part) return true;
	for (size_t i = 0; i < sizeof(STATE_PARTS) / sizeof(STATE_PARTS[0]); i++)
		if (!strcmp(part, STATE_PARTS[i])) return true;
	return false;
}

void hifi_api_write_state(const hifi_snapshot_t *s, char *out, size_t cap) { hifi_api_write_state_part(s, NULL, out, cap); }

/* part NULL: the whole state; else only that top-level section, as
 * {"<part>":{...}}, or {} when the board has none of it now (controls, update) */
void hifi_api_write_state_part(const hifi_snapshot_t *s, const char *part, char *out, size_t cap) {
#define WANT(name) (!part || !strcmp(part, name))
	hj_t j;
	hj_init(&j, out, cap);
	hj_obj(&j, NULL);
	if (!part) hj_int(&j, "uptime_s", s->uptime_s);

	if (WANT("power")) {
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
	}

	if (WANT("amp")) write_amp(&j, "amp", s);

	if (WANT("governor")) {
		hj_obj(&j, "governor");
		hj_num(&j, "budget_w", s->gov_budget_w, 1);
		hj_num(&j, "in_w", s->gov_in_w, 1);
		hj_num(&j, "out_w", s->gov_out_w, 1);
		hj_num(&j, "gain_db", s->gov_gain_db, 1);
		hj_bool(&j, "limiting", s->gov_limiting);
		hj_int(&j, "limit_events", s->gov_events);
		if (s->loudness) {
			str_or_null(&j, "reason", s->gov_why);
			hj_num(&j, "heat_cap_w", s->heat_cap_w, 1);
			hj_num(&j, "heat_w", s->heat_w, 1);
		}
		hj_obj_end(&j);
	}

	if (WANT("audio")) {
		hj_obj(&j, "audio");
		hj_str(&j, "source", s->source ? s->source : "idle");
		hj_str(&j, "input", hifi_input_name(s->input));
		if (s->dj_guard) {
			hj_obj(&j, "dj_guard");
			hj_num(&j, "in_w", s->dj_guard_w, 1);
			hj_num(&j, "limit_w", s->dj_guard_limit_w, 1);
			if (s->dj_guard_tripped) hj_str(&j, "tripped", s->dj_guard_tripped);
			else hj_null(&j, "tripped");
			hj_obj_end(&j);
		}
		if (s->tone_active) write_tone(&j, "tone", &s->tone);
		else hj_null(&j, "tone");
		hj_num(&j, "level_dbfs", s->level_dbfs, 1);
		hj_str(&j, "channel", hifi_chan_name(s->channel));
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
		if (s->loudness) {
			hj_obj(&j, "boost");
			hj_num(&j, "db", s->boost_db, 1);
			hj_num(&j, "limit_db", s->boost_limit_db, 1);
			hj_bool(&j, "limiting", s->boost_limiting);
			hj_int(&j, "limit_events", s->boost_events);
			hj_obj_end(&j);
		}
		write_playing(&j, &s->playing);
		if (s->zones > 1) {
			hj_obj(&j, "zone_b");
			hj_bool(&j, "on", s->zone_b_on);
			hj_null(&j, "volume"); /* it plays at the main volume */
			hj_obj_end(&j);
		}
		hj_obj_end(&j);
	}

	if (WANT("net")) {
		hj_obj(&j, "net");
		hj_str(&j, "mode", s->net_mode ? s->net_mode : "sta");
		hj_str(&j, "ssid", s->ssid[0] ? s->ssid : NULL);
		hj_str(&j, "hostname", s->hostname);
		hj_str(&j, "ip", s->ip[0] ? s->ip : NULL);
		if (s->rssi) hj_int(&j, "rssi", s->rssi);
		else hj_null(&j, "rssi");
		if (s->eth_present) hj_bool(&j, "eth", s->eth_up);
		hj_obj_end(&j);
	}

	if (WANT("sys")) {
		hj_obj(&j, "sys");
		hj_int(&j, "heap_free", s->heap_free);
		hj_int(&j, "heap_min", s->heap_min);
		if (s->heap_boot) hj_int(&j, "heap_boot", s->heap_boot);
		else hj_null(&j, "heap_boot");
		hj_int(&j, "psram_free", s->psram_free);
		if (s->reset_reason) hj_str(&j, "reset_reason", s->reset_reason);
		if (s->restart_cause) hj_str(&j, "restart_cause", s->restart_cause);
		hj_obj_end(&j);
	}

	if (WANT("ir")) {
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
	}

	if (WANT("controls") && (s->exp_present || s->front_present || s->last_control[0] || s->backlight)) {
		char hex[8];
		hj_obj(&j, "controls");
		if (s->exp_present) {
			snprintf(hex, sizeof(hex), "0x%04x", s->exp_in);
			hj_str(&j, "expander", hex);
		}
		if (s->front_present) {
			snprintf(hex, sizeof(hex), "0x%02x", s->front_in);
			hj_str(&j, "front", hex);
		}
		if (s->last_control[0]) {
			hj_str(&j, "last", s->last_control);
			hj_int(&j, "last_ago_s", s->last_control_ago_s);
		} else {
			hj_null(&j, "last");
		}
		if (s->backlight) hj_str(&j, "backlight", s->backlight);
		hj_obj_end(&j);
	}

	if (WANT("update") && s->update.state != HIFI_UPD_IDLE) {
		hj_obj(&j, "update");
		hj_str(&j, "state", hifi_update_state_name(s->update.state));
		hj_int(&j, "progress", s->update.progress);
		hj_str(&j, "error", s->update.error[0] ? s->update.error : NULL);
		hj_obj_end(&j);
	}

	hj_obj_end(&j);
#undef WANT
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
	if (i.caps.line) hj_str(&j, NULL, "line");
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
	hj_num(&j, "max_db", i.caps.sub_max_db, 1);
	hj_obj_end(&j);
	hj_int(&j, "zones", i.caps.zones);
	hj_bool(&j, "eth", i.caps.eth);
	hj_num(&j, "pdo_v_max", i.caps.pdo_v_max, 0);
	/* the settings the model has a route for (ask k2391), so the app shows
	 * only those: the speaker side (POST channel), the LED test, the backlight */
	hj_bool(&j, "channel", ops->set_channel != NULL);
	hj_bool(&j, "led", ops->set_led != NULL);
	hj_bool(&j, "backlight", ops->set_backlight != NULL);
	if (i.caps.boost_max_db > 0 && ops->set_loudness) {
		hj_obj(&j, "loudness");
		hj_num(&j, "boost_max_db", i.caps.boost_max_db, 1);
		hj_obj(&j, "heat_cap_w");
		hj_num(&j, "min", i.caps.heat_min_w, 1);
		hj_num(&j, "max", i.caps.heat_max_w, 1);
		hj_num(&j, "default", i.caps.heat_def_w, 1);
		hj_obj_end(&j);
		hj_obj_end(&j);
	}
	hj_obj_end(&j);
	hj_obj_end(&j);
	return 200;
}

static int r_state(const hifi_ops_t *ops, const char *part, char *out, size_t cap) {
	if (!hifi_api_state_part_known(part))
		return error(out, cap, 404, "no such state section; try power, amp, governor, audio, net, sys, ir, controls or update");
	hifi_snapshot_t *s = calloc(1, sizeof(*s));
	if (!s) return error(out, cap, 500, "out of memory");
	ops->snapshot(s);
	hifi_api_write_state_part(s, part, out, cap);
	free(s);
	return 200;
}

static double num_or(const char *body, const char *key, double dflt) {
	double v;
	if (hj_num_bad(body, key)) return NAN; /* fails the tone's range checks: 400 */
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
	if (!hifi_input_parse(in, &v)) return error(out, cap, 400, "input must be stream, dj_direct or line");
	hifi_info_t i;
	memset(&i, 0, sizeof(i));
	ops->info(&i);
	if ((v == HIFI_INPUT_DJ_DIRECT && !i.caps.dj_direct) || (v == HIFI_INPUT_LINE && !i.caps.line))
		return error(out, cap, 409, "this board has no such input");
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
	bool on, has_on = hj_get_bool(body, "on", &on), has_db = hj_get_num(body, "level_db", &db) ||
	     hj_num_bad(body, "level_db");
	if (!has_on && !has_db) return error(out, cap, 400, "send level_db and/or on");
	hifi_info_t i;
	memset(&i, 0, sizeof(i));
	ops->info(&i);
	if (has_db && !(db >= -30 && db <= i.caps.sub_max_db)) {
		char m[64];
		snprintf(m, sizeof(m), "level_db must be -30 to %+.1f on this model", i.caps.sub_max_db);
		return error(out, cap, 400, m);
	}
	ops->set_sub(has_on ? on : -1, (float) db);
	return r_state(ops, NULL, out, cap);
}

static int r_loudness(const hifi_ops_t *ops, const char *body, char *out, size_t cap) {
	hifi_info_t i;
	memset(&i, 0, sizeof(i));
	ops->info(&i);
	if (!ops->set_loudness || !(i.caps.boost_max_db > 0)) return error(out, cap, 404, "this model has no Boost");
	double b = NAN, w = NAN;
	bool has_b = hj_get_num(body, "boost_db", &b) || hj_num_bad(body, "boost_db");
	bool has_w = hj_get_num(body, "heat_cap_w", &w) || hj_num_bad(body, "heat_cap_w");
	if (!has_b && !has_w) return error(out, cap, 400, "send boost_db and/or heat_cap_w");
	if (has_b && !(b >= 0 && b <= i.caps.boost_max_db)) return error(out, cap, 400, "boost_db is out of range");
	if (has_w && !(w >= i.caps.heat_min_w && w <= i.caps.heat_max_w))
		return error(out, cap, 400, "heat_cap_w is out of range");
	if (ops->set_loudness(has_b ? (float) b : NAN, has_w ? (float) w : NAN))
		return error(out, cap, 500, "the loudness is set but couldn't be saved");
	return r_state(ops, NULL, out, cap);
}

static int r_channel(const hifi_ops_t *ops, const char *body, char *out, size_t cap) {
	char v[16];
	hifi_chan_t c;
	if (!ops->set_channel) return error(out, cap, 404, "this model has no channel mode");
	if (!hj_get_str(body, "channel", v, sizeof(v)) || !hifi_chan_parse(v, &c))
		return error(out, cap, 400, "channel must be stereo, left, right or mono");
	if (ops->set_channel(c)) return error(out, cap, 500, "couldn't set the channel");
	return r_state(ops, NULL, out, cap);
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
	return r_state(ops, NULL, out, cap);
}

static int r_name(const hifi_ops_t *ops, const char *body, char *out, size_t cap) {
	char name[65] = "";
	int got = hj_get_str_fit(body, "name", name, sizeof(name));
	if (got == HJ_STR_CUT || strlen(name) > 32) return error(out, cap, 400, "name must be 32 characters or fewer");
	if (got != HJ_STR_OK || !name[0]) return error(out, cap, 400, "name is required");
	/* the name goes into squeezelite's command line (autoexec1, -n "<name>"):
	 * a quote or backslash there would end the word and add options */
	for (const char *c = name; *c; c++) {
		if ((unsigned char) *c < 0x20 || *c == 0x7f) return error(out, cap, 400, "name has a control character");
		if (*c == '"' || *c == '\\') return error(out, cap, 400, "name may not have \" or \\ in it");
	}
	if (ops->set_name(name)) return error(out, cap, 500, "couldn't save the name");
	return r_state(ops, NULL, out, cap);
}

static int r_reboot(const hifi_ops_t *ops, bool lan, const char *body, char *out, size_t cap, bool reset) {
	if (reset) {
		bool factory = false;
		if (!hj_get_bool(body, "factory", &factory) || !factory)
			return error(out, cap, 400, "send {\"factory\": true} to erase every setting");
		/* L6: an erase from the LAN would bring the setup AP back on the
		 * public password; from there it only arms, and someone with the
		 * receiver in hand finishes it by unplugging it (security.md) */
		if (lan) {
			if (!ops->factory_arm || ops->factory_arm())
				return error(out, cap, 403, "a factory reset needs the USB cable here");
			snprintf(out, cap, "{\"armed\":true,\"within_s\":%d,\"next\":\"unplug the receiver and plug it back in\"}",
				 (int) (HIFI_RESET_ARM_US / 1000000));
			return 200;
		}
		ops->factory_reset();
	} else {
		ops->reboot();
	}
	return r_state(ops, NULL, out, cap);
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
	return hj_get_str(body, key, t, sizeof(t)) || hj_get_bool(body, key, &b) || hj_get_num(body, key, &d) ||
	       hj_num_bad(body, key);
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
	return r_state(ops, NULL, out, cap);
}

static int r_ir(const hifi_ops_t *ops, const char *body, char *out, size_t cap, bool learn) {
	char name[16] = "";
	bool has = hj_get_str(body, "action", name, sizeof(name));
	int a = has ? hifi_ir_action_from(name) : -1;
	if ((learn || has) && a < 0)
		return error(out, cap, 400, "action must be volup, voldown, mute, toggle, next, prev, input or power");
	if (learn) ops->ir_learn(a);
	else ops->ir_forget(a);
	return r_state(ops, NULL, out, cap);
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

#define CRASH_LOG_MAX 3072
/* a remote bench caps a console line at 1000 characters, so a POST reads the
 * log a page at a time: {"from":N} gives the cleaned text from N on, as much
 * as fits CRASH_PAGE escaped, with "next" (where the following page starts)
 * and "len" (the whole log's) */
#define CRASH_PAGE 700
static size_t esc_len(char c) { return c == '\n' ? 6 : (c == '"' || c == '\\') ? 2 : 1; }

static int r_crash(const hifi_ops_t *ops, const char *body, char *out, size_t cap) {
	if (!ops->reset_reason || !ops->last_log) return error(out, cap, 404, "this build keeps no crash log");
	double from_d = -1;
	if (body && !hj_get_num(body, "from", &from_d)) return error(out, cap, 400, "from: a number, where the page starts");
	if (body && !(from_d >= 0)) return error(out, cap, 400, "from: a number, where the page starts");
	char *log = malloc(CRASH_LOG_MAX + 1);
	if (!log) return error(out, cap, 503, "out of memory");
	size_t n = ops->last_log(log, CRASH_LOG_MAX + 1), k = 0;
	/* keep it plain text (no colour codes) so the escaped reply fits */
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char) log[i];
		if (c == 0x1b) { /* ESC [ ... letter */
			while (i + 1 < n && !((log[i + 1] >= 'A' && log[i + 1] <= 'Z') || (log[i + 1] >= 'a' && log[i + 1] <= 'z'))) i++;
			i++;
			continue;
		}
		if (c == '\n' || (c >= 0x20 && c < 0x7f)) log[k++] = (char) c;
	}
	log[k] = '\0';
	hj_t j;
	hj_init(&j, out, cap);
	hj_obj(&j, NULL);
	hj_str(&j, "reset_reason", ops->reset_reason());
	if (body) {
		size_t from = from_d > k ? k : (size_t) from_d, end = from, esc = 0;
		while (end < k && esc + esc_len(log[end]) <= CRASH_PAGE) esc += esc_len(log[end++]);
		log[end] = '\0';
		hj_num(&j, "len", (double) k, 0);
		hj_num(&j, "from", (double) from, 0);
		hj_num(&j, "next", (double) end, 0);
		hj_str(&j, "log", log + from);
	} else {
		/* JSON-escaped it must fit the reply (hifi_api_reply_cap): drop the oldest */
		size_t esc = 0, start = 0;
		for (size_t i = 0; i < k; i++) esc += esc_len(log[i]);
		while (esc > 2 * CRASH_LOG_MAX && start < k) esc -= esc_len(log[start++]);
		hj_str(&j, "log", log + start);
	}
	hj_obj_end(&j);
	free(log);
	return 200;
}

/* ------------------------------------------------------------------ diagnostics */

static void diag_power(const hifi_ops_t *ops, hifi_diag_power_t *p) {
	hifi_snapshot_t *s = calloc(1, sizeof(*s));
	if (!s) return;
	ops->snapshot(s);
	*p = (hifi_diag_power_t) {
		.known = true,
		.state = hifi_power_state_name(s->pwr_state),
		.vbus_v = s->vbus_v, .vbus_i_a = s->vbus_i_a, .pvdd_v = s->pvdd_v,
		.proto = s->contract.known ? hifi_proto_name(s->contract.proto) : NULL,
		.contract_v = s->contract.known ? s->contract.v : NAN,
		.contract_a = s->contract.known ? s->contract.i_max_a : NAN,
		.pg = s->pg, .amp_on = s->amp_enabled, .amp_fault = s->amp_fault,
		.amp_faults = s->amp_faults, .amp_temp_c = s->amp_temp_c,
	};
	free(s);
}

/* diag, diag/tasks, diag/net, diag/audio, diag/ota (GET or POST) and
 * POST diag/echo; tasks takes {"from":N} or diag/tasks/N to page, echo
 * {"bytes":N} */
static int r_diag(const hifi_ops_t *ops, const char *what, bool post, bool lan, const char *body, char *out, size_t cap) {
	const hifi_diag_ops_t *d = ops->diag;
	if (!d) return error(out, cap, 404, "this build has no diagnostics");
	if (!*what) {
		if (!d->sys) return error(out, cap, 404, "this build has no diagnostics");
		hifi_diag_sys_t *s = calloc(1, sizeof(*s));
		if (!s) return error(out, cap, 503, "out of memory");
		d->sys(s);
		diag_power(ops, &s->power);
		hifi_diag_write_sys(s, out, cap);
		free(s);
		return 200;
	}
	if (!strcmp(what, "/tasks") || !strncmp(what, "/tasks/", 7)) {
		double from = 0;
		/* diag/tasks/N: the page from N with no body, for a bench that takes
		 * only bodiless lines from a lane that doesn't hold the board */
		if (what[6]) {
			const char *q = what + 7;
			if (!*q || strlen(q) > 3 || strspn(q, "0123456789") != strlen(q))
				return error(out, cap, 400, "diag/tasks/N: N a whole number, where the page starts");
			from = atoi(q);
		}
		if (post && hj_get_raw(body, "from", &(const char *) {0}, &(size_t) {0}) &&
		    (!hj_get_num(body, "from", &from) || from < 0 || from != floor(from)))
			return error(out, cap, 400, "from: a whole number, where the page starts");
		if (from > HIFI_DIAG_TASKS_MAX) return error(out, cap, 400, "from: past the last task");
		if (!d->tasks) return error(out, cap, 404, "this build keeps no task stats");
		hifi_diag_tasks_t *t = calloc(1, sizeof(*t));
		if (!t) return error(out, cap, 503, "out of memory");
		/* the first page takes a new sample, the rest page through it */
		int rc = d->tasks(t, from == 0);
		if (!rc) hifi_diag_write_tasks(t, (int) from, out, cap);
		free(t);
		if (rc > 0) {
			char m[64];
			snprintf(m, sizeof(m), "%d tasks, more than the %d the list holds", rc, HIFI_DIAG_TASKS_MAX);
			return error(out, cap, 507, m);
		}
		if (rc == -2) return error(out, cap, 503, "the CPU sample failed, try again");
		return rc ? error(out, cap, 404, "this build keeps no task stats") : 200;
	}
	if (!strcmp(what, "/net") && d->net) {
		hifi_diag_net_t n = {0};
		d->net(&n);
		hifi_diag_write_net(&n, out, cap);
		return 200;
	}
	if (!strcmp(what, "/audio") && d->audio) {
		hifi_diag_audio_t a = {0};
		d->audio(&a);
		hifi_diag_write_audio(&a, out, cap);
		return 200;
	}
	if (!strcmp(what, "/ota") && d->ota) {
		hifi_diag_ota_t o = {0};
		d->ota(&o);
		hifi_diag_write_ota(&o, out, cap);
		return 200;
	}
	if (!strcmp(what, "/echo")) {
		if (!post) return -1;
		/* it sends up to 1 MB over the Hive's link: a test for the bench, not the LAN */
		if (lan) return error(out, cap, 403, "the throughput test runs over USB or the relay, not from the LAN");
		if (!d->echo) return error(out, cap, 404, "this build has no throughput test");
		double bytes = HIFI_DIAG_ECHO_DEFAULT;
		if (hj_get_raw(body, "bytes", &(const char *) {0}, &(size_t) {0}) &&
		    (!hj_get_num(body, "bytes", &bytes) || !(bytes >= 1 && bytes <= HIFI_DIAG_ECHO_MAX) ||
		     bytes != floor(bytes)))
			return error(out, cap, 400, "bytes: 1 to 1048576");
		hifi_diag_echo_t e = {0};
		const char *err = "the test failed";
		int st = d->echo((uint32_t) bytes, &e, &err);
		if (st) return error(out, cap, st, err);
		hifi_diag_write_echo(&e, out, cap);
		return 200;
	}
	return error(out, cap, 404, "no such path");
}

static int r_bttest(const hifi_ops_t *ops, const char *body, char *out, size_t cap) {
	if (!ops->bt_test) return error(out, cap, 404, "this build has no Bluetooth test");
	char sink[33];
	if (!hj_get_str(body, "sink", sink, sizeof(sink)) || !sink[0])
		return error(out, cap, 400, "sink (the other board's Bluetooth name) is required");
	const char *err = NULL;
	if (ops->bt_test(sink, &err)) return error(out, cap, 409, err ? err : "couldn't arm the test");
	return r_state(ops, NULL, out, cap);
}

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

/* one step of an update over the API, under the session's lock (r_ota) */
static int r_ota_step(const hifi_ops_t *ops, bool lan, const char *what, const char *body, char *out, size_t cap,
		      bool *restart) {
	hifi_ota_t *o = ops->ota;
	/* the URL job or the app's upload is running one (L10) */
	if (o->held) return error(out, cap, 409, "an update is already running");
	/* the LAN can't touch an update the Hive started */
	if (lan && o->active && !o->need_sig)
		return error(out, cap, 403, "an update the Hive started is running");
	const char *err = "update failed";
	int st;
	hj_t j;
	if (!strcmp(what, "begin")) {
		double size;
		char sha[72] = "", version[32] = "";
		if (!hj_get_num(body, "size", &size) || !(size > 0)) return error(out, cap, 400, "size is required");
		/* range-checked before the cast: past size_t it is undefined (32 bits on the board) */
		if (!(size <= 4294967295.0)) return error(out, cap, 400, "image is larger than the app slot");
		hj_get_str(body, "sha256", sha, sizeof(sha));
		hj_get_str(body, "version", version, sizeof(version));
		if ((st = lan ? hifi_ota_begin_ex(o, (size_t) size, sha[0] ? sha : NULL, version, true, &err)
			      : hifi_ota_begin(o, (size_t) size, sha, version, &err)) != 200)
			return error(out, cap, st, err);
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
		/* range-checked before the cast, as size is */
		if (!hj_get_num(body, "off", &off) || !(off >= 0 && off <= 4294967295.0))
			return error(out, cap, 400, "off is required");
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
		*restart = reboot;
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

static int r_ota(const hifi_ops_t *ops, bool lan, const char *what, const char *body, char *out, size_t cap) {
	hifi_ota_t *o = ops->ota;
	if (!o) return error(out, cap, 404, "no OTA in this build");
	bool restart = false;
	hifi_ota_lock(o);
	int st = r_ota_step(ops, lan, what, body, out, cap, &restart);
	hifi_ota_unlock(o);
	if (restart) ops->reboot(); /* after the reply has gone */
	return st;
}

static int capture_frames(const char *body) {
	double f;
	if (body && hj_num_bad(body, "frames")) return -1;
	if (!body || !hj_get_num(body, "frames", &f)) return HIFI_CAPTURE_FRAMES_DEFAULT;
	/* out of range stays out of range, without a cast past int */
	if (!(f >= 0)) return -1;
	return f > HIFI_CAPTURE_FRAMES_MAX ? HIFI_CAPTURE_FRAMES_MAX + 1 : (int) f;
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
	char ssid[33] = "", pw[65] = "";
	/* a cut ssid or password would join another network, or fail as a wrong password */
	int got = hj_get_str_fit(body, "ssid", ssid, sizeof(ssid));
	if (got == HJ_STR_CUT) return error(out, cap, 400, "ssid must be 32 bytes or fewer");
	if (got != HJ_STR_OK || !ssid[0]) return error(out, cap, 400, "ssid is required");
	if (hj_get_str_fit(body, "password", pw, sizeof(pw)) == HJ_STR_CUT)
		return error(out, cap, 400, "password must be 64 characters or fewer");
	if (ops->set_wifi(ssid, pw)) return error(out, cap, 500, "couldn't start joining");
	return ok_reply(out, cap);
}

static int r_remote(const hifi_ops_t *ops, bool post, const char *body, char *out, size_t cap) {
	if (!ops->remote_status) return error(out, cap, 404, "no relay in this build");
	if (post) {
		char url[128] = "", user[48] = "", pw[96] = "", prefix[24] = "hifi", key[66] = "";
		if (!ops->remote_set) return error(out, cap, 404, "no relay in this build");
		/* a cut-off url, token or prefix would dial somewhere else, or fail without saying why */
		if (hj_get_str_fit(body, "url", url, sizeof(url)) == HJ_STR_CUT ||
		    hj_get_str_fit(body, "user", user, sizeof(user)) == HJ_STR_CUT ||
		    hj_get_str_fit(body, "password", pw, sizeof(pw)) == HJ_STR_CUT)
			return error(out, cap, 400, "url, user or password is too long");
		if (hj_get_str_fit(body, "prefix", prefix, sizeof(prefix)) == HJ_STR_CUT)
			return error(out, cap, 400, "prefix is too long");
		hj_get_str_fit(body, "key", key, sizeof(key)); /* a cut key reads "": hifi_ws_key_ok refuses it below */
		bool ws = !strncmp(url, "wss://", 6);
		if (url[0] && !ws && strncmp(url, "mqtts://", 8) && strncmp(url, "mqtt://", 7))
			return error(out, cap, 400, "url must be wss://host/path (the Hive's endpoint), mqtts://host:port "
						    "(or mqtt:// on a LAN), or empty to turn it off");
		char host[HIFI_WS_HOST_MAX], path[HIFI_WS_PATH_MAX];
		int port;
		if (ws && hifi_ws_url_parse(url, host, &port, path)) return error(out, cap, 400, "that wss:// url can't be dialled");
		if (ws && !hifi_ws_key_ok(key)) return error(out, cap, 400, "a wss:// link needs its key: 64 hex digits");
		if (!ws) key[0] = '\0';
		if (!prefix[0] || strpbrk(prefix, "+#/ ")) return error(out, cap, 400, "prefix must be one topic level");
		/* the service token goes into the dial's headers: nothing that could start another one */
		char hd[HIFI_WS_HEADERS_MAX];
		if (ws && hifi_ws_headers(hd, sizeof(hd), user, pw, "000000000000", key) < 0)
			return error(out, cap, 400, "user and password must be printable, with a password when there is a user");
		if (ops->remote_set(url, user, pw, prefix, key)) return error(out, cap, 500, "couldn't save the relay settings");
	}
	hifi_remote_status_t r = {0};
	ops->remote_status(&r);
	hj_t j;
	hj_init(&j, out, cap);
	hj_obj(&j, NULL);
	hj_bool(&j, "configured", r.configured);
	hj_bool(&j, "connected", r.connected);
	hj_str(&j, "url", r.url[0] ? r.url : NULL);
	hj_str(&j, "transport", !r.url[0] ? NULL : strncmp(r.url, "wss://", 6) ? "mqtt" : "ws");
	hj_str(&j, "user", r.user[0] ? r.user : NULL);
	hj_str(&j, "prefix", r.prefix);
	hj_str(&j, "dev", r.dev);
	hj_bool(&j, "has_key", r.has_key);
	hj_int(&j, "keys", r.keys);
	hj_int(&j, "accepted", r.accepted);
	hj_int(&j, "rejected", r.rejected);
	hj_obj_end(&j);
	return 200;
}

static int r_wifi_scan(const hifi_ops_t *ops, char *out, size_t cap) {
	if (!ops->wifi_scan) return error(out, cap, 404, "no Wi-Fi scan in this build");
	hifi_wifi_ap_t aps[HIFI_WIFI_SCAN_MAX];
	int n = ops->wifi_scan(aps, HIFI_WIFI_SCAN_MAX);
	if (n < 0) return error(out, cap, 409, "the radio is busy: scan again in a few seconds");
	hj_t j;
	hj_init(&j, out, cap);
	hj_obj(&j, NULL);
	hj_arr(&j, "networks");
	for (int i = 0; i < n && i < HIFI_WIFI_SCAN_MAX; i++) {
		if (!aps[i].ssid[0]) continue; /* hidden networks: join them by name */
		hj_obj(&j, NULL);
		hj_str(&j, "ssid", aps[i].ssid);
		hj_int(&j, "rssi", aps[i].rssi);
		hj_bool(&j, "secure", aps[i].secure);
		hj_obj_end(&j);
	}
	hj_arr_end(&j);
	hj_obj_end(&j);
	return j.overflow ? error(out, cap, 500, "reply buffer too small") : 200;
}

static int r_wifi_forget(const hifi_ops_t *ops, char *out, size_t cap) {
	if (!ops->wifi_forget) return error(out, cap, 404, "no Wi-Fi setup in this build");
	if (ops->wifi_forget()) return error(out, cap, 500, "couldn't forget the network");
	return ok_reply(out, cap);
}

static int r_zone(const hifi_ops_t *ops, const char *body, char *out, size_t cap) {
	if (!ops->set_zone) return error(out, cap, 404, "this model has one zone");
	char zone[8] = "";
	bool on;
	double v;
	if (!hj_get_str(body, "zone", zone, sizeof(zone)) || strcmp(zone, "b")) return error(out, cap, 400, "zone must be \"b\"");
	if (hj_get_num(body, "volume", &v) || hj_num_bad(body, "volume"))
		return error(out, cap, 400, "zone B has no level of its own: it plays at the main volume");
	if (!hj_get_bool(body, "on", &on)) return error(out, cap, 400, "on (true or false) is required");
	ops->set_zone(on);
	return ok_reply(out, cap);
}

/* led and backlight: {"mode": ...} */
static int r_mode(int (*set)(const char *), const char *none, const char *modes, const char *body,
		  char *out, size_t cap) {
	if (!set) return error(out, cap, 404, none);
	char mode[8] = "", msg[48];
	snprintf(msg, sizeof(msg), "mode must be %s", modes);
	if (!hj_get_str(body, "mode", mode, sizeof(mode)) || set(mode)) return error(out, cap, 400, msg);
	return ok_reply(out, cap);
}

static int r_update_check(const hifi_ops_t *ops, char *out, size_t cap) {
	if (!ops->update_check) return error(out, cap, 404, "no update server in this build");
	hifi_update_offer_t o;
	hifi_info_t i = {0};
	const char *err = "no update server";
	int st = ops->update_check(&o, &err);
	if (st) return error(out, cap, st, err);
	ops->info(&i);
	hj_t j;
	hj_init(&j, out, cap);
	hj_obj(&j, NULL);
	hj_str(&j, "current", i.fw);
	hj_str(&j, "latest", o.latest);
	hj_str(&j, "url", o.url);
	hj_str(&j, "sha256", o.sha256[0] ? o.sha256 : NULL);
	hj_str(&j, "notes", o.notes[0] ? o.notes : NULL);
	hj_obj_end(&j);
	return 200;
}

static int r_update(const hifi_ops_t *ops, bool lan, const char *body, char *out, size_t cap) {
	if (!ops->update_start) return error(out, cap, 404, "no updates from a URL in this build");
	char url[192] = "", sha[72] = "";
	if (!hj_get_str(body, "url", url, sizeof(url)) || (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)))
		return error(out, cap, 400, "url (http:// or https://) is required");
	hj_get_str(body, "sha256", sha, sizeof(sha));
	const char *err = "couldn't start the update";
	int st = ops->update_start(url, sha[0] ? sha : NULL, lan, &err);
	if (st) return error(out, cap, st, err);
	return ok_reply(out, cap);
}

size_t hifi_api_reply_cap(const char *path, const char *body) {
	if (path && strstr(path, PREFIX "capture") == path) {
		int f = capture_frames(body);
		if (f < 64) f = 64;
		if (f > HIFI_CAPTURE_FRAMES_MAX) f = HIFI_CAPTURE_FRAMES_MAX;
		return hifi_b64_len((size_t) f * 4) + 1024;
	}
	if (path && strstr(path, PREFIX "crash") == path) return 2 * CRASH_LOG_MAX + 256;
	return 4096;
}

int hifi_api_handle(const hifi_ops_t *ops, const char *method, const char *path,
		    const char *body, char *out, size_t cap) {
	return hifi_api_handle_from(ops, HIFI_SRC_TRUSTED, method, path, body, out, cap);
}

int hifi_api_handle_from(const hifi_ops_t *ops, hifi_src_t src, const char *method, const char *path,
			 const char *body, char *out, size_t cap) {
	bool lan = src != HIFI_SRC_TRUSTED;
	char route[32];
	size_t plen = strlen(PREFIX);
	if (strncmp(path, PREFIX, plen)) return error(out, cap, 404, "no such path");
	snprintf(route, sizeof(route), "%s", path + plen);
	route[strcspn(route, "?")] = '\0';
	bool get = !strcmp(method, "GET"), post = !strcmp(method, "POST");

	if (post && (!body || !*body)) body = "{}";
	if (post && !hj_valid_object(body)) return error(out, cap, 400, "body must be a JSON object");

	/* while new firmware is on trial, a reboot or a lost Wi-Fi rolls it back:
	 * the LAN may not reboot, change or forget the Wi-Fi, or erase settings,
	 * until it is kept (hifi_lan_trial_refused; esp/hifi_http.c applies the
	 * same list to squeezelite's pages) */
	if (lan && ops->ota_status && hifi_lan_trial_refused(path, method)) {
		hifi_ota_status_t o = {0};
		ops->ota_status(&o);
		if (o.state && !strcmp(o.state, "pending_verify"))
			return error(out, cap, 409, "new firmware is on trial; try again in a few minutes");
	}

	int status;
	if (!strcmp(route, "info")) status = get ? r_info(ops, out, cap) : -1;
	else if (!strcmp(route, "state")) status = get ? r_state(ops, NULL, out, cap) : -1;
	else if (!strncmp(route, "state/", 6)) status = get ? r_state(ops, route + 6, out, cap) : -1;
	else if (!strcmp(route, "tone")) status = post ? r_tone(ops, body, out, cap) : -1;
	else if (!strcmp(route, "amp")) status = post ? r_amp(ops, body, out, cap) : -1;
	else if (!strcmp(route, "input")) status = post ? r_input(ops, body, out, cap) : -1;
	else if (!strcmp(route, "sub")) status = post ? r_sub(ops, body, out, cap) : -1;
	else if (!strcmp(route, "loudness")) status = post ? r_loudness(ops, body, out, cap) : -1;
	else if (!strcmp(route, "channel")) status = post ? r_channel(ops, body, out, cap) : -1;
	else if (!strcmp(route, "eq")) status = post ? r_eq(ops, body, out, cap) : -1;
	else if (!strcmp(route, "name")) status = post ? r_name(ops, body, out, cap) : -1;
	else if (!strcmp(route, "reboot")) status = post ? r_reboot(ops, lan, body, out, cap, false) : -1;
	else if (!strcmp(route, "reset")) status = post ? r_reboot(ops, lan, body, out, cap, true) : -1;
	else if (!strcmp(route, "control")) status = post ? r_control(ops, body, out, cap) : -1;
	else if (!strcmp(route, "volume")) status = post ? r_volume(ops, body, out, cap) : -1;
	else if (!strcmp(route, "ir/learn")) status = post ? r_ir(ops, body, out, cap, true) : -1;
	else if (!strcmp(route, "ir/forget")) status = post ? r_ir(ops, body, out, cap, false) : -1;
	else if (!strcmp(route, "selftest")) status = post ? r_selftest(ops, out, cap) : -1;
	else if (!strcmp(route, "ota")) status = get ? r_ota_get(ops, out, cap) : -1;
	else if (!strcmp(route, "crash")) status = get ? r_crash(ops, NULL, out, cap) : post ? r_crash(ops, body ? body : "{}", out, cap) : -1;
	else if (!strcmp(route, "bttest")) status = post ? r_bttest(ops, body, out, cap) : -1;
	else if (!strncmp(route, "ota/", 4)) status = post ? r_ota(ops, lan, route + 4, body, out, cap) : -1;
	else if (!strcmp(route, "capture")) status = post ? r_capture(ops, body, out, cap) : -1;
	else if (!strcmp(route, "wifi")) status = post ? r_wifi(ops, body, out, cap) : -1;
	else if (!strcmp(route, "wifi/scan")) status = get ? r_wifi_scan(ops, out, cap) : -1;
	else if (!strcmp(route, "wifi/forget")) status = post ? r_wifi_forget(ops, out, cap) : -1;
	else if (!strcmp(route, "zone")) status = post ? r_zone(ops, body, out, cap) : -1;
	else if (!strcmp(route, "led"))
		status = post ? r_mode(ops->set_led, "the firmware doesn't drive this model's LED", "test or auto", body, out, cap) : -1;
	else if (!strcmp(route, "backlight"))
		status = post ? r_mode(ops->set_backlight, "this model has no backlight", "on, off or auto", body, out, cap) : -1;
	else if (!strcmp(route, "update/check")) status = get ? r_update_check(ops, out, cap) : -1;
	else if (!strcmp(route, "update")) status = post ? r_update(ops, lan, body, out, cap) : -1;
	else if (!strcmp(route, "remote"))
		status = lan && post ? error(out, cap, 403, "the relay is set over USB or by the Hive, not from the LAN")
				     : r_remote(ops, post, body, out, cap);
	else if (!strcmp(route, "lab"))
		status = !ops->lab ? error(out, cap, 404, "this build has no lab")
			 : lan && post ? error(out, cap, 403, "lab runs are started over USB or by the Hive, not from the LAN")
			 : ops->lab(post, body, out, cap);
	else if (!strncmp(route, "party", 5) && (!route[5] || route[5] == '/'))
		status = !ops->party ? error(out, cap, 404, "this build has no party mode")
		       : get || post ? ops->party(route[5] ? route + 6 : "", post, lan, body, out, cap) : -1;
	else if (!strncmp(route, "diag", 4) && (!route[4] || route[4] == '/'))
		status = get || post ? r_diag(ops, route + 4, post, lan, body, out, cap) : -1;
	else return error(out, cap, 404, "no such path");

	if (status < 0) return error(out, cap, 405, "wrong method for this path");
	return status;
}

/* ------------------------------------------------------------------ console */

static const char USAGE[] =
	"usage: hifi info|state [section]|crash|selftest | diag [tasks [from]|net|audio|ota|echo [bytes]] | tone sine <hz> <dBFS> [both|left|right] [s] | "
	"tone sweep <from> <to> <dBFS> [s] | tone noise <dBFS> [s] | tone silence [s] | tone stop | "
	"amp on|off|reset | input stream|dj_direct|line | sub <dB> | channel stereo|left|right|mono | control <action> | "
	"volume <0-100> | mute on|off | maxvol <30-100> | ir learn <action> | ir forget [action] | "
	"led test|auto | backlight on|off|auto | party [on|off|lead|stats] | i2c <addr> <reg> [val]";

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

	if (!strcmp(cmd, "crash") && argc == 2 && is_num(argv[1])) {
		*path = PREFIX "crash";
		snprintf(body, bc, "{\"from\":%s}", argv[1]);
		return true;
	}
	if (!strcmp(cmd, "state") && argc == 2) {
		static const char *const paths[] = {PREFIX "state/power", PREFIX "state/amp", PREFIX "state/governor",
						    PREFIX "state/audio", PREFIX "state/net", PREFIX "state/sys",
						    PREFIX "state/ir", PREFIX "state/controls", PREFIX "state/update"};
		*method = "GET";
		for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++)
			if (!strcmp(argv[1], paths[i] + strlen(PREFIX "state/"))) return *path = paths[i], true;
		return usage(out, cap);
	}
	if (!strcmp(cmd, "info") || !strcmp(cmd, "state") || !strcmp(cmd, "crash")) {
		*method = "GET";
		*path = !strcmp(cmd, "info") ? PREFIX "info" : !strcmp(cmd, "state") ? PREFIX "state" : PREFIX "crash";
		return argc == 1 || usage(out, cap);
	}
	if (!strcmp(cmd, "diag") && argc <= 3) {
		static const char *const parts[] = {"tasks", "net", "audio", "ota", "echo"};
		static const char *const paths[] = {PREFIX "diag/tasks", PREFIX "diag/net", PREFIX "diag/audio",
						    PREFIX "diag/ota", PREFIX "diag/echo"};
		*path = PREFIX "diag";
		if (argc == 1) {
			*method = "GET";
			return true;
		}
		int k = -1;
		for (int i = 0; i < 5; i++)
			if (!strcmp(argv[1], parts[i])) k = i;
		if (k < 0) return usage(out, cap);
		*path = paths[k];
		if (argc == 3 && (k != 0 && k != 4)) return usage(out, cap);
		if (argc == 3 && !is_num(argv[2])) return usage(out, cap);
		if (argc == 3) snprintf(body, bc, "{\"%s\":%s}", k == 0 ? "from" : "bytes", argv[2]);
		else if (k == 4) snprintf(body, bc, "{}");
		else *method = "GET";
		return true;
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
		hifi_input_t v;
		return hifi_input_parse(argv[1], &v) || usage(out, cap);
	}
	if (!strcmp(cmd, "sub") && argc == 2 && is_num(argv[1])) {
		*path = PREFIX "sub";
		snprintf(body, bc, "{\"level_db\":%s}", argv[1]);
		return true;
	}
	if (!strcmp(cmd, "channel") && argc == 2) {
		hifi_chan_t v;
		if (!hifi_chan_parse(argv[1], &v)) return usage(out, cap);
		*path = PREFIX "channel";
		snprintf(body, bc, "{\"channel\":\"%s\"}", argv[1]);
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
	if (!strcmp(cmd, "party") && argc <= 2) {
		*path = PREFIX "party";
		if (argc == 1 || !strcmp(argv[1], "stats")) {
			*method = "GET";
			if (argc == 2) *path = PREFIX "party/stats";
			return true;
		}
		if (strcmp(argv[1], "on") && strcmp(argv[1], "off") && strcmp(argv[1], "lead")) return usage(out, cap);
		snprintf(body, bc, "{\"op\":\"%s\"}", argv[1]);
		return true;
	}
	if ((!strcmp(cmd, "led") || !strcmp(cmd, "backlight")) && argc == 2) {
		bool led = !strcmp(cmd, "led");
		const char *m = argv[1];
		bool okm = led ? !strcmp(m, "test") || !strcmp(m, "auto")
			       : !strcmp(m, "on") || !strcmp(m, "off") || !strcmp(m, "auto");
		if (!okm) return usage(out, cap);
		*path = led ? PREFIX "led" : PREFIX "backlight";
		snprintf(body, bc, "{\"mode\":\"%s\"}", m);
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

/* ------------------------------------------------------------------ i2c */

/* a whole number in [lo, hi], decimal or 0x hex */
static bool parse_byte(const char *s, long lo, long hi, uint8_t *v) {
	char *end;
	if (!s || !*s) return false;
	long n = strtol(s, &end, 0);
	if (*end || n < lo || n > hi) return false;
	*v = (uint8_t) n;
	return true;
}

int hifi_i2c_cli(int argc, const char *const *argv, const char *busy, hifi_i2c_rd_t rd,
		 hifi_i2c_wr_t wr, char *out, size_t cap) {
	uint8_t addr, reg, val = 0;
	if (argc < 3 || argc > 4 || strcmp(argv[0], "i2c") || !parse_byte(argv[1], 0x08, 0x77, &addr) ||
	    !parse_byte(argv[2], 0, 255, &reg) || (argc == 4 && !parse_byte(argv[3], 0, 255, &val)))
		return error(out, cap, 400, "usage: i2c <addr 0x08-0x77> <reg 0-255> [val 0-255]");
	bool write = argc == 4;
	if (write && busy) {
		char msg[96];
		snprintf(msg, sizeof(msg), "no writes while %s: stop it and turn the amps off (hifi amp off)", busy);
		return error(out, cap, 409, msg);
	}
	if (write ? wr(addr, reg, val) : rd(addr, reg, &val)) {
		char msg[48];
		snprintf(msg, sizeof(msg), "0x%02x doesn't answer", addr);
		return error(out, cap, 502, msg);
	}
	char a[5], r[5], v[5];
	snprintf(a, sizeof(a), "0x%02x", addr);
	snprintf(r, sizeof(r), "0x%02x", reg);
	snprintf(v, sizeof(v), "0x%02x", val);
	hj_t j;
	hj_init(&j, out, cap);
	hj_obj(&j, NULL);
	hj_str(&j, "addr", a);
	hj_str(&j, "reg", r);
	hj_str(&j, "val", v);
	if (write) hj_bool(&j, "wrote", true);
	hj_obj_end(&j);
	return 200;
}
