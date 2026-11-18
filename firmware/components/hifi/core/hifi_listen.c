#include "hifi_listen.h"

#include <stdio.h>
#include <string.h>

#define EPHEMERAL_LO 49152 /* lwIP's UDP_LOCAL_PORT_RANGE_START (0xc000) */

bool hifi_listen_allowed(const hifi_sock_t *s, bool ap_on) {
	if (s->loopback) return true;
	if (s->tcp) return s->port == 80 || s->port == 5000;
	if (s->port == 5353 || s->port == 68) return true;
	if (s->port == 4951) return true; /* party mode (HIFI_PARTY_PORT), while it is on */
	if (s->port == 67 || s->port == 53) return ap_on;
	return s->port >= EPHEMERAL_LO; /* a client's reply port */
}

static bool seen(const hifi_sock_t *prev, int nprev, const hifi_sock_t *s) {
	for (int i = 0; i < nprev; i++)
		if (prev[i].tcp == s->tcp && prev[i].port == s->port && prev[i].loopback == s->loopback) return true;
	return false;
}

int hifi_listen_check(const hifi_sock_t *prev, int nprev, const hifi_sock_t *s, int n, bool ap_on, char *why,
		      size_t nwhy) {
	if (n < 0 || nprev < 0) {
		snprintf(why, nwhy, "socket list unreadable");
		return 1;
	}
	int bad = 0;
	char list[96] = "";
	size_t len = 0;
	for (int i = 0; i < n; i++) {
		if (hifi_listen_allowed(&s[i], ap_on)) continue;
		/* a UDP pcb that came and went is a client query (DNS); a TCP listener is never that */
		if (!s[i].tcp && prev && !seen(prev, nprev, &s[i])) continue;
		bad++;
		int w = snprintf(list + len, sizeof(list) - len, "%s%s/%u", len ? " " : "", s[i].tcp ? "tcp" : "udp",
				 (unsigned) s[i].port);
		if (w > 0 && len + w < sizeof(list)) len += w;
		else list[len] = 0; /* didn't fit: keep the ones that did */
	}
	if (bad) snprintf(why, nwhy, "%d open: %s", bad, list);
	else snprintf(why, nwhy, "%d sockets, all allowed", n);
	return bad;
}

void hifi_listen_report_init(hifi_listen_report_t *r) {
	memset(r, 0, sizeof(*r));
	r->strays = -1;
}

bool hifi_listen_report(hifi_listen_report_t *r, int strays, const char *why) {
	bool fresh = strays > 0 && (strays != r->strays || strcmp(why, r->open));
	if (fresh) r->events++;
	r->strays = strays;
	snprintf(r->open, sizeof(r->open), "%s", strays > 0 ? why : "");
	return fresh;
}
