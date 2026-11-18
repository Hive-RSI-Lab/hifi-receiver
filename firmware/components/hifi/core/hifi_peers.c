#include "hifi_peers.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "hifi_gate.h"
#include "hifi_json.h"

/* control characters, which JSON escapes to six bytes each, as spaces */
static void clean(char *s) {
	for (; *s; s++)
		if ((unsigned char) *s < 0x20 || *s == 0x7f) *s = ' ';
}

static void put(char *dst, size_t cap, const char *src, size_t len) {
	if (len >= cap) len = cap - 1;
	memcpy(dst, src, len);
	dst[len] = 0;
	clean(dst);
}

static void drop(hifi_peers_t *t, int i) {
	t->p[i] = t->p[--t->n];
}

void hifi_peers_seen(hifi_peers_t *t, const hifi_peer_t *p, long long now_us) {
	if (!t || !p || !p->ip[0] || !p->host[0]) return;
	if (t->mask ? !hifi_gate_on_net(p->ip, t->net, t->mask) : !hifi_gate_private_v4(p->ip)) return;
	int at = -1;
	for (int i = 0; i < t->n; i++)
		if (!strcmp(t->p[i].ip, p->ip)) at = i;
	/* the same box at a new address: the old row goes */
	for (int i = t->n - 1; i >= 0; i--)
		if (i != at && !strcasecmp(t->p[i].host, p->host)) {
			drop(t, i);
			if (at == t->n) at = i; /* drop moved the last row into i */
		}
	if (at < 0 && t->n < HIFI_PEERS_MAX) at = t->n++;
	if (at < 0) {
		at = 0;
		for (int i = 1; i < t->n; i++)
			if (t->p[i].seen_us < t->p[at].seen_us) at = i;
	}
	hifi_peer_t *r = &t->p[at];
	*r = *p;
	r->ip[sizeof(r->ip) - 1] = r->host[sizeof(r->host) - 1] = r->name[sizeof(r->name) - 1] = 0;
	r->model[sizeof(r->model) - 1] = r->fw[sizeof(r->fw) - 1] = 0;
	clean(r->host);
	clean(r->name);
	clean(r->model);
	clean(r->fw);
	r->seen_us = now_us;
}

void hifi_peers_expire(hifi_peers_t *t, long long now_us) {
	if (!t) return;
	for (int i = t->n - 1; i >= 0; i--)
		if (now_us - t->p[i].seen_us > HIFI_PEERS_GONE_US) drop(t, i);
}

void hifi_peers_txt(hifi_peer_t *p, const char *key, const char *val, size_t len) {
	if (!p || !key || (!val && len)) return;
	if (!strcmp(key, "name")) put(p->name, sizeof(p->name), val, len);
	else if (!strcmp(key, "model")) put(p->model, sizeof(p->model), val, len);
	else if (!strcmp(key, "fw")) put(p->fw, sizeof(p->fw), val, len);
}

static void row(hj_t *j, const hifi_peer_t *p, bool self, long long now_us) {
	hj_obj(j, NULL);
	hj_str(j, "name", p->name[0] ? p->name : p->host);
	hj_str(j, "host", p->host);
	hj_str(j, "ip", p->ip);
	hj_str(j, "model", p->model[0] ? p->model : NULL);
	hj_str(j, "fw", p->fw[0] ? p->fw : NULL);
	hj_bool(j, "self", self);
	hj_int(j, "seen_s", self || now_us < p->seen_us ? 0 : (now_us - p->seen_us) / 1000000);
	hj_obj_end(j);
}

static bool is_self(const hifi_peer_t *p, const hifi_peer_t *self) {
	return self && ((self->ip[0] && !strcmp(p->ip, self->ip)) ||
			(self->host[0] && !strcasecmp(p->host, self->host)));
}

static const char *label(const hifi_peer_t *p) {
	return p->name[0] ? p->name : p->host;
}

int hifi_peers_json(const hifi_peers_t *t, const hifi_peer_t *self, bool searching, long long now_us,
		    char *out, size_t cap) {
	int idx[HIFI_PEERS_MAX], n = 0;
	for (int i = 0; t && i < t->n; i++) {
		if (is_self(&t->p[i], self)) continue;
		/* by name, then IP: the same order on every box */
		int k = n++;
		while (k > 0) {
			const hifi_peer_t *a = &t->p[idx[k - 1]], *b = &t->p[i];
			int c = strcasecmp(label(a), label(b));
			if (c < 0 || (c == 0 && strcmp(a->ip, b->ip) <= 0)) break;
			idx[k] = idx[k - 1];
			k--;
		}
		idx[k] = i;
	}
	hj_t j;
	hj_init(&j, out, cap);
	hj_obj(&j, NULL);
	hj_arr(&j, "peers");
	if (self) row(&j, self, true, now_us);
	for (int k = 0; k < n; k++) row(&j, &t->p[idx[k]], false, now_us);
	hj_arr_end(&j);
	hj_bool(&j, "searching", searching);
	hj_obj_end(&j);
	if (j.overflow) {
		snprintf(out, cap, "{\"error\":\"the speakers list is too long\"}");
		return 500;
	}
	return 200;
}
