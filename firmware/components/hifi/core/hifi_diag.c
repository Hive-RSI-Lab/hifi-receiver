#include "hifi_diag.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hifi_json.h"
#include "hifi_ws_proto.h"

/* ------------------------------------------------------------------ CPU load */

static int busiest_first(const void *pa, const void *pb) {
	const hifi_diag_task_t *a = pa, *b = pb;
	float x = isnan(a->cpu_pct) ? -1 : a->cpu_pct, y = isnan(b->cpu_pct) ? -1 : b->cpu_pct;
	if (x != y) return x < y ? 1 : -1;
	return strcmp(a->name, b->name);
}

void hifi_diag_cpu(const hifi_diag_task_t *a, int na, hifi_diag_tasks_t *b, uint32_t elapsed) {
	b->idle_pct[0] = b->idle_pct[1] = NAN;
	for (int i = 0; i < b->n; i++) {
		hifi_diag_task_t *t = &b->t[i];
		if (!elapsed) {
			t->cpu_pct = NAN;
			continue;
		}
		uint32_t before = 0;
		for (int k = 0; k < na; k++)
			if (a[k].num == t->num) {
				before = a[k].runtime;
				break;
			}
		/* unsigned: right across the counter's wrap */
		double pct = (double) (uint32_t) (t->runtime - before) * 100.0 / elapsed;
		t->cpu_pct = pct > 100 ? 100 : (float) pct;
		if (!strncmp(t->name, "IDLE", 4) && t->core >= 0 && t->core < 2) b->idle_pct[t->core] = t->cpu_pct;
	}
	qsort(b->t, (size_t) b->n, sizeof(b->t[0]), busiest_first);
}

/* ------------------------------------------------------------------ reset history */

void hifi_diag_hist_push(char *hist, size_t cap, const char *reason) {
	if (!cap) return;
	char next[HIFI_DIAG_HIST_CAP * 2];
	snprintf(next, sizeof(next), "%s%s%s", reason && *reason ? reason : "unknown", hist[0] ? "," : "", hist);
	/* keep whole reasons: at most HIFI_DIAG_RESETS, and what fits cap */
	size_t keep = 0;
	int n = 0;
	for (size_t i = 0;; i++) {
		if (next[i] == ',' || next[i] == '\0') {
			if (i >= cap) break;
			keep = i;
			if (++n == HIFI_DIAG_RESETS || next[i] == '\0') break;
		}
	}
	next[keep] = '\0';
	memcpy(hist, next, keep + 1);
}

void hifi_restart_cause_fmt(char *out, size_t cap, const char *note, const char *task, uint32_t pc,
			    uint32_t up_s) {
	if (!cap) return;
	snprintf(out, cap, "%s by %s at 0x%08x after %u s", note && note[0] ? note : "esp_restart",
		 task && task[0] ? task : "?", (unsigned) pc, (unsigned) up_s);
}

bool hifi_diag_is_power_cut(const char *reason, uint32_t boots_before) {
	if (!reason || !boots_before) return false;
	return !strcmp(reason, "power_on") || !strcmp(reason, "brownout");
}

/* ------------------------------------------------------------------ underruns */

int hifi_diag_underrun_step(uint8_t *state, bool running, uint32_t frames, bool live, bool flushed) {
	if (!running || flushed) {
		*state = 0;
		return 0;
	}
	if (frames) {
		*state = HIFI_UNDERRUN_ARMED;
		return 0;
	}
	if (!(*state & HIFI_UNDERRUN_ARMED) || (*state & HIFI_UNDERRUN_DRY) || !live) return 0;
	*state |= HIFI_UNDERRUN_DRY;
	return 1;
}

/* ------------------------------------------------------------------ writers */

static void heap(hj_t *j, const char *key, const hifi_diag_heap_t *h) {
	if (!h->total) {
		hj_null(j, key);
		return;
	}
	hj_obj(j, key);
	hj_int(j, "free", h->free);
	hj_int(j, "min", h->min);
	hj_int(j, "largest", h->largest);
	hj_int(j, "total", h->total);
	hj_obj_end(j);
}

static void ms_or_null(hj_t *j, const char *key, uint32_t ms) {
	if (ms) hj_int(j, key, ms);
	else hj_null(j, key);
}

void hifi_diag_write_sys(const hifi_diag_sys_t *s, char *out, size_t cap) {
	hj_t j;
	hj_init(&j, out, cap < HIFI_DIAG_PAGE + 1 ? cap : HIFI_DIAG_PAGE + 1);
	hj_obj(&j, NULL);
	hj_int(&j, "uptime_s", s->uptime_s);
	hj_obj(&j, "boot_ms");
	ms_or_null(&j, "app", s->boot_app_ms);
	ms_or_null(&j, "ip", s->boot_ip_ms);
	ms_or_null(&j, "link", s->boot_link_ms);
	hj_obj_end(&j);
	heap(&j, "heap", &s->internal);
	heap(&j, "psram", &s->psram);
	hj_str(&j, "reset_reason", s->reset_reason);
	hj_str(&j, "resets", s->reset_hist);
	hj_int(&j, "boots", s->boots);
	hj_int(&j, "power_cuts", s->power_cuts);
	hj_obj(&j, "nvs");
	for (int i = 0; i < 2; i++) {
		const hifi_diag_nvs_t *v = &s->nvs[i];
		if (!v->name) continue;
		hj_obj(&j, v->name);
		hj_int(&j, "used", v->used);
		hj_int(&j, "free", v->free);
		hj_int(&j, "total", v->total);
		hj_int(&j, "namespaces", v->namespaces);
		hj_num(&j, "used_pct", v->total ? 100.0 * v->used / v->total : NAN, 1);
		hj_obj_end(&j);
	}
	hj_obj_end(&j);
	const hifi_diag_power_t *p = &s->power;
	if (p->known) {
		hj_obj(&j, "power");
		hj_str(&j, "state", p->state);
		hj_num(&j, "vbus_v", p->vbus_v, 2);
		hj_num(&j, "vbus_i_a", p->vbus_i_a, 2);
		hj_num(&j, "pvdd_v", p->pvdd_v, 2);
		hj_bool(&j, "pg", p->pg);
		hj_str(&j, "proto", p->proto);
		hj_num(&j, "contract_v", p->contract_v, 1);
		hj_num(&j, "contract_a", p->contract_a, 2);
		hj_bool(&j, "amp_on", p->amp_on);
		hj_bool(&j, "amp_fault", p->amp_fault);
		hj_int(&j, "amp_faults", p->amp_faults);
		hj_num(&j, "amp_temp_c", p->amp_temp_c, 1);
		hj_obj_end(&j);
	} else {
		hj_null(&j, "power");
	}
	/* last, and only when it still fits the page with the closing brace: a
	 * reply as wide as sys can get leaves it out rather than cut the reply
	 * (GET /api/v1/state/sys always has it) */
	if (s->restart_cause) {
		hj_t keep = j;
		hj_str(&j, "restart_cause", s->restart_cause);
		if (j.overflow || j.len + 1 >= j.cap) {
			j = keep;
			j.buf[j.len] = '\0';
		}
	}
	hj_obj_end(&j);
}

void hifi_diag_write_tasks(const hifi_diag_tasks_t *t, int from, char *out, size_t cap) {
	size_t lim = cap < HIFI_DIAG_PAGE + 1 ? cap : HIFI_DIAG_PAGE + 1;
	if (from < 0) from = 0;
	if (from > t->n) from = t->n;
	hj_t j;
	hj_init(&j, out, lim);
	hj_obj(&j, NULL);
	hj_int(&j, "n", t->n);
	hj_int(&j, "from", from);
	hj_int(&j, "sample_ms", t->sample_ms);
	hj_arr(&j, "idle_pct");
	hj_num(&j, NULL, t->idle_pct[0], 1);
	hj_num(&j, NULL, t->idle_pct[1], 1);
	hj_arr_end(&j);
	hj_arr(&j, "cols");
	hj_str(&j, NULL, "name");
	hj_str(&j, NULL, "stack_free");
	hj_str(&j, NULL, "core");
	hj_str(&j, NULL, "prio");
	hj_str(&j, NULL, "cpu_pct");
	hj_str(&j, NULL, "stack");
	hj_str(&j, NULL, "ram");
	hj_arr_end(&j);
	hj_arr(&j, "rows");
	int i = from;
	/* room left for `],"next":NNN}` */
	const size_t tail = 20;
	for (; i < t->n; i++) {
		const hifi_diag_task_t *r = &t->t[i];
		hj_t keep = j;
		hj_arr(&j, NULL);
		hj_str(&j, NULL, r->name);
		hj_int(&j, NULL, r->stack_free);
		if (r->core >= 0) hj_int(&j, NULL, r->core);
		else hj_null(&j, NULL);
		hj_int(&j, NULL, r->prio);
		hj_num(&j, NULL, r->cpu_pct, 1);
		if (r->stack_size) hj_int(&j, NULL, r->stack_size);
		else hj_null(&j, NULL);
		if (r->stack_ram == 'i') hj_str(&j, NULL, "int");
		else if (r->stack_ram == 'p') hj_str(&j, NULL, "psram");
		else hj_null(&j, NULL);
		hj_arr_end(&j);
		if (j.overflow || j.len + tail >= lim) {
			j = keep;
			out[j.len] = '\0';
			break;
		}
	}
	hj_arr_end(&j);
	hj_int(&j, "next", i);
	hj_obj_end(&j);
}

void hifi_diag_write_net(const hifi_diag_net_t *n, char *out, size_t cap) {
	hj_t j;
	hj_init(&j, out, cap < HIFI_DIAG_PAGE + 1 ? cap : HIFI_DIAG_PAGE + 1);
	hj_obj(&j, NULL);
	hj_obj(&j, "wifi");
	if (n->rssi) hj_int(&j, "rssi", n->rssi);
	else hj_null(&j, "rssi");
	if (n->channel) hj_int(&j, "channel", n->channel);
	else hj_null(&j, "channel");
	hj_str(&j, "bssid", n->bssid[0] ? n->bssid : NULL);
	hj_str(&j, "phy", n->phy);
	hj_int(&j, "connects", n->connects);
	hj_int(&j, "disconnects", n->disconnects);
	if (n->last_reason) hj_int(&j, "last_reason", n->last_reason);
	else hj_null(&j, "last_reason");
	hj_int(&j, "up_s", n->up_s);
	hj_obj_end(&j);
	hj_obj(&j, "link");
	hj_bool(&j, "up", n->link_up);
	hj_int(&j, "drops", n->link_drops);
	hj_int(&j, "up_s", n->link_up_s);
	hj_int(&j, "tx_bytes", (long long) n->tx_bytes);
	hj_int(&j, "rx_bytes", (long long) n->rx_bytes);
	if (n->link_last_drop) {
		char why[64];
		hifi_ws_drop_text(why, sizeof(why), (hifi_ws_drop_t) n->link_last_drop, n->link_last_code, n->link_last_wifi_down);
		hj_obj(&j, "last_drop");
		hj_str(&j, "kind", hifi_ws_drop_name((hifi_ws_drop_t) n->link_last_drop));
		hj_int(&j, "code", n->link_last_code);
		hj_bool(&j, "wifi_down", n->link_last_wifi_down);
		hj_str(&j, "why", why);
		hj_int(&j, "ago_s", n->link_last_ago_s);
		hj_int(&j, "lasted_s", n->link_last_lasted_s);
		hj_obj_end(&j);
	} else {
		hj_null(&j, "last_drop");
	}
	hj_obj_end(&j);
	hj_obj(&j, "listeners");
	if (n->listen_strays >= 0) hj_int(&j, "strays", n->listen_strays);
	else hj_null(&j, "strays");
	hj_int(&j, "events", n->listen_events);
	hj_str(&j, "open", n->listen_open[0] ? n->listen_open : NULL);
	hj_obj_end(&j);
	hj_obj_end(&j);
}

void hifi_diag_write_audio(const hifi_diag_audio_t *a, char *out, size_t cap) {
	hj_t j;
	hj_init(&j, out, cap < HIFI_DIAG_PAGE + 1 ? cap : HIFI_DIAG_PAGE + 1);
	hj_obj(&j, NULL);
	if (a->known) {
		hj_int(&j, "underruns", a->underruns);
		hj_int(&j, "rate", a->rate);
		hj_int(&j, "buffer_ms", a->buffer_ms);
		hj_int(&j, "dma_ms", a->dma_ms);
		hj_int(&j, "latency_ms", (long long) a->buffer_ms + a->dma_ms);
		hj_int(&j, "played_s", a->played_s);
	} else {
		hj_null(&j, "underruns");
	}
	hj_obj_end(&j);
}

void hifi_diag_write_ota(const hifi_diag_ota_t *o, char *out, size_t cap) {
	hj_t j;
	hj_init(&j, out, cap < HIFI_DIAG_PAGE + 1 ? cap : HIFI_DIAG_PAGE + 1);
	hj_obj(&j, NULL);
	if (o->known) {
		hj_str(&j, "version", o->version);
		hj_int(&j, "bytes", o->bytes);
		hj_int(&j, "total_ms", o->total_ms);
		hj_int(&j, "write_ms", o->write_ms);
		hj_num(&j, "kbps", o->total_ms ? o->bytes * 8.0 / o->total_ms : NAN, 1);
		hj_num(&j, "write_kbps", o->write_ms ? o->bytes * 8.0 / o->write_ms : NAN, 1);
		hj_int(&j, "boot", o->boot);
	} else {
		hj_null(&j, "version");
	}
	hj_obj_end(&j);
}

void hifi_diag_write_echo(const hifi_diag_echo_t *e, char *out, size_t cap) {
	hj_t j;
	hj_init(&j, out, cap < HIFI_DIAG_PAGE + 1 ? cap : HIFI_DIAG_PAGE + 1);
	hj_obj(&j, NULL);
	hj_int(&j, "bytes", e->bytes);
	hj_int(&j, "ms", e->ms);
	hj_num(&j, "kbps", e->ms ? e->bytes * 8.0 / e->ms : NAN, 1);
	hj_obj_end(&j);
}
