#include "hifi_lab.h"

#include <stdio.h>
#include <string.h>

#include "hifi_json.h"

static void copy(char *dst, size_t n, const char *src) {
	if (!n) return;
	snprintf(dst, n, "%s", src ? src : "");
}

void hifi_lab_init(hifi_lab_t *l) { memset(l, 0, sizeof(*l)); }

static bool good_name(const char *s) {
	if (!s || !*s || strlen(s) > 15) return false;
	for (; *s; s++)
		if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_' || *s == '-')) return false;
	return true;
}

static hifi_lab_slot_t *find(hifi_lab_t *l, const char *name) {
	for (int i = 0; i < l->n; i++)
		if (!strcmp(l->s[i].exp->name, name)) return &l->s[i];
	return NULL;
}

bool hifi_lab_register(hifi_lab_t *l, const hifi_lab_exp_t *e) {
	if (!e || l->n >= HIFI_LAB_EXP_MAX || !good_name(e->name) || !e->start || !e->stop) return false;
	if (!strcmp(e->name, "all") || find(l, e->name)) return false;
	memset(&l->s[l->n], 0, sizeof(l->s[0]));
	l->s[l->n++].exp = e;
	return true;
}

static void stop_slot(hifi_lab_slot_t *s, uint32_t now_ms, const char *why) {
	if (!s->running) return;
	bool down = s->exp->stop();
	s->running = false;
	s->recent = true;
	s->stopped_ms = now_ms;
	copy(s->stop_why, sizeof(s->stop_why), down ? why : "stuck");
}

static void write_slot(hj_t *j, const hifi_lab_slot_t *s, uint32_t now_ms, bool with_result) {
	hj_obj(j, NULL);
	hj_str(j, "name", s->exp->name);
	/* help in the list only: a result has to fit one console line */
	if (s->exp->help && !with_result) hj_str(j, "help", s->exp->help);
	hj_bool(j, "running", s->running);
	hj_int(j, "runs", s->runs);
	if (s->runs) {
		uint32_t end = s->running ? now_ms : s->stopped_ms;
		hj_num(j, "elapsed_s", (end - s->started_ms) / 1000.0, 1);
		if (s->running)
			hj_num(j, "left_s", (int32_t) (s->deadline_ms - now_ms) / 1000.0, 1);
		else
			hj_str(j, "stopped", s->stop_why);
		if (with_result && s->exp->result) {
			hj_obj(j, "result");
			s->exp->result(j);
			hj_obj_end(j);
		}
	}
	hj_obj_end(j);
}

static int reply_err(char *out, size_t cap, int status, const char *msg) {
	hj_t j;
	hj_init(&j, out, cap);
	hj_obj(&j, NULL);
	hj_str(&j, "error", msg);
	hj_obj_end(&j);
	return status;
}

static int reply_list(hifi_lab_t *l, uint32_t now_ms, char *out, size_t cap) {
	hj_t j;
	hj_init(&j, out, cap);
	hj_obj(&j, NULL);
	hj_str(&j, "running", hifi_lab_running(l));
	if (l->crashed_in[0]) hj_str(&j, "crashed_in", l->crashed_in);
	if (l->offline_restart) hj_bool(&j, "offline_restart", true);
	if (l->stuck_restart) hj_bool(&j, "stuck_restart", true);
	hj_int(&j, "max_s", HIFI_LAB_MAX_S);
	hj_arr(&j, "experiments");
	for (int i = 0; i < l->n; i++) write_slot(&j, &l->s[i], now_ms, false);
	hj_arr_end(&j);
	hj_obj_end(&j);
	return j.overflow ? reply_err(out, cap, 500, "reply too long") : 200;
}

static int reply_slot(const hifi_lab_slot_t *s, uint32_t now_ms, char *out, size_t cap) {
	hj_t j;
	hj_init(&j, out, cap);
	write_slot(&j, s, now_ms, true);
	if (j.overflow) return reply_err(out, cap, 500, "reply too long");
	return 200;
}

int hifi_lab_request(hifi_lab_t *l, bool post, const char *body, uint32_t now_ms, uint32_t heap_free,
		     char *out, size_t cap) {
	if (!post) return reply_list(l, now_ms, out, cap);
	if (!body || !hj_valid_object(body)) return reply_err(out, cap, 400, "the body must be a JSON object");
	char op[12] = "", name[17] = "";
	if (!hj_get_str(body, "op", op, sizeof(op)) || !op[0])
		return reply_err(out, cap, 400, "op is required: start, stop or result");
	if (!hj_get_str(body, "exp", name, sizeof(name)) || !name[0])
		return reply_err(out, cap, 400, "exp (an experiment's name, or all to stop) is required");

	if (!strcmp(op, "stop") && !strcmp(name, "all")) {
		for (int i = 0; i < l->n; i++) stop_slot(&l->s[i], now_ms, "asked");
		return reply_list(l, now_ms, out, cap);
	}
	hifi_lab_slot_t *s = find(l, name);
	if (!s) return reply_err(out, cap, 404, "no such experiment in this build");

	if (!strcmp(op, "stop")) {
		stop_slot(s, now_ms, "asked");
		return reply_slot(s, now_ms, out, cap);
	}
	if (!strcmp(op, "result")) return reply_slot(s, now_ms, out, cap);
	if (strcmp(op, "start")) return reply_err(out, cap, 400, "op must be start, stop or result");

	if (s->running) return reply_err(out, cap, 409, "already running; stop it first");
	if (heap_free < HIFI_LAB_HEAP_FLOOR) return reply_err(out, cap, 503, "too little free RAM to start");
	double max_s = HIFI_LAB_DEFAULT_S;
	if (hj_get_num(body, "max_s", &max_s) && !(max_s >= 1 && max_s <= HIFI_LAB_MAX_S))
		return reply_err(out, cap, 400, "max_s must be 1 to 3600");
	char err[96] = "";
	int st = s->exp->start(s->runs + 1, body, err, sizeof(err));
	if (st) return reply_err(out, cap, st >= 400 && st < 600 ? st : 500, err[0] ? err : "the experiment didn't start");
	s->running = true;
	s->runs++;
	s->started_ms = now_ms;
	s->deadline_ms = now_ms + (uint32_t) (max_s * 1000.0);
	s->stopped_ms = 0;
	s->recent = false;
	s->stop_why[0] = 0;
	return reply_slot(s, now_ms, out, cap);
}

int hifi_lab_tick(hifi_lab_t *l, uint32_t now_ms, uint32_t heap_free) {
	int stopped = 0;
	for (int i = 0; i < l->n; i++) {
		hifi_lab_slot_t *s = &l->s[i];
		if (!s->running) {
			/* past the window: so the clock's wrap (49.7 days) can't bring it back */
			if (s->recent && now_ms - s->stopped_ms >= HIFI_LAB_RECENT_MS) s->recent = false;
			continue;
		}
		if (heap_free < HIFI_LAB_HEAP_STOP) {
			stop_slot(s, now_ms, "heap");
			stopped++;
		} else if ((int32_t) (now_ms - s->deadline_ms) >= 0) {
			stop_slot(s, now_ms, "deadline");
			stopped++;
		}
	}
	return stopped;
}

void hifi_lab_finished(hifi_lab_t *l, const char *name, uint32_t run, uint32_t now_ms, bool ok) {
	hifi_lab_slot_t *s = find(l, name);
	if (!s || !s->running || s->runs != run) return;
	s->recent = true;
	/* the experiment has already ended itself: don't call its stop */
	s->running = false;
	s->stopped_ms = now_ms;
	copy(s->stop_why, sizeof(s->stop_why), ok ? "done" : "failed");
}

const char *hifi_lab_running(const hifi_lab_t *l) {
	for (int i = 0; i < l->n; i++)
		if (l->s[i].running) return l->s[i].exp->name;
	return "";
}

void hifi_lab_set_crashed(hifi_lab_t *l, const char *name) { copy(l->crashed_in, sizeof(l->crashed_in), name); }

const char *hifi_lab_recent(const hifi_lab_t *l, uint32_t now_ms) {
	const char *run = hifi_lab_running(l);
	if (run[0]) return run;
	const hifi_lab_slot_t *best = NULL;
	for (int i = 0; i < l->n; i++) {
		const hifi_lab_slot_t *s = &l->s[i];
		if (!s->runs || !s->recent || now_ms - s->stopped_ms >= HIFI_LAB_RECENT_MS) continue;
		if (!best || now_ms - s->stopped_ms < now_ms - best->stopped_ms) best = s;
	}
	return best ? best->exp->name : "";
}

bool hifi_lab_link_tick(hifi_lab_link_t *w, uint32_t now_ms, bool configured, bool connected) {
	if (!configured) {
		w->seen = w->down = false;
		return false;
	}
	if (connected) {
		w->seen = true;
		w->down = false;
		return false;
	}
	if (!w->seen) return false;
	if (!w->down) {
		w->down = true;
		w->down_since = now_ms;
		return false;
	}
	return now_ms - w->down_since >= HIFI_LAB_OFFLINE_MS;
}

bool hifi_lab_selftest(const hifi_lab_t *l, char *value, size_t n) {
	/* value is a self-test check's (64 bytes): the lab's own restarts say
	 * which experiment held the board then, in as few words as fit */
	const char *run = hifi_lab_running(l), *in = l->crashed_in[0] ? " during " : "";
	if (l->stuck_restart)
		snprintf(value, n, "%d experiments%s%s; lab restart, tick stuck %u min%s%s", l->n,
			 run[0] ? ", running " : "", run, (unsigned) (HIFI_LAB_STUCK_MS / 60000u), in, l->crashed_in);
	else if (l->offline_restart)
		snprintf(value, n, "%d experiments%s%s; lab restart, off dial-in %u min%s%s", l->n,
			 run[0] ? ", running " : "", run, (unsigned) (HIFI_LAB_OFFLINE_MS / 60000u), in, l->crashed_in);
	else if (l->crashed_in[0])
		snprintf(value, n, "%d experiments%s%s; last restart during or just after %s", l->n,
			 run[0] ? ", running " : "", run, l->crashed_in);
	else
		snprintf(value, n, "%d experiments%s%s", l->n, run[0] ? ", running " : "", run);
	return l->n > 0;
}
