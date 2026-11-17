/* The lab's LAN scan, the plain-C part (hifi_scan.h) */
#include "hifi_scan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hifi_json.h"

bool hifi_scan_target_ok(uint32_t self, uint32_t mask, uint32_t gw, uint32_t target, char *err, size_t n) {
	/* a /24 or narrower: a contiguous mask with at least 24 bits */
	if (!self || (mask >> 8) != 0xffffffu || (~mask & (~mask + 1)) != 0) {
		snprintf(err, n, "the board's own network isn't a /24 or narrower");
		return false;
	}
	if (target >= 0xe0000000u || self >= 0xe0000000u || (target >> 24) == 127 || (target >> 24) == 0) {
		snprintf(err, n, "target must be a unicast address");
		return false;
	}
	if ((target & mask) != (self & mask)) {
		snprintf(err, n, "target must be on the board's own subnet");
		return false;
	}
	if (target == self) {
		snprintf(err, n, "target is this board");
		return false;
	}
	if (target == gw) {
		snprintf(err, n, "target is the gateway");
		return false;
	}
	if ((target & ~mask) == 0 || (target & ~mask) == ~mask) {
		snprintf(err, n, "target is the network or broadcast address");
		return false;
	}
	return true;
}

bool hifi_scan_parse_ip(const char *s, uint32_t *ip) {
	uint32_t v = 0;
	for (int i = 0; i < 4; i++) {
		if (*s < '0' || *s > '9') return false;
		unsigned o = 0;
		int d = 0;
		while (*s >= '0' && *s <= '9') {
			o = o * 10 + (unsigned) (*s++ - '0');
			if (++d > 3 || o > 255) return false;
		}
		v = (v << 8) | o;
		if (i < 3 && *s++ != '.') return false;
	}
	if (*s) return false;
	*ip = v;
	return true;
}

void hifi_scan_ip_str(uint32_t ip, char *out, size_t n) {
	snprintf(out, n, "%u.%u.%u.%u", (unsigned) (ip >> 24), (unsigned) (ip >> 16 & 255), (unsigned) (ip >> 8 & 255),
		 (unsigned) (ip & 255));
}

static bool port_num(const char **s, uint32_t *v) {
	if (**s < '0' || **s > '9') return false;
	uint32_t x = 0;
	while (**s >= '0' && **s <= '9') {
		x = x * 10 + (uint32_t) (*(*s)++ - '0');
		if (x > 65535) return false;
	}
	if (!x) return false;
	*v = x;
	return true;
}

bool hifi_scan_ports(const char *spec, uint8_t bits[HIFI_SCAN_BITS], uint32_t *count) {
	memset(bits, 0, HIFI_SCAN_BITS);
	const char *s = spec;
	for (;;) {
		uint32_t a, b;
		if (!port_num(&s, &a)) return false;
		b = a;
		if (*s == '-') {
			s++;
			if (!port_num(&s, &b) || b < a) return false;
		}
		for (uint32_t p = a; p <= b; p++) hifi_scan_set(bits, (uint16_t) p);
		if (!*s) break;
		if (*s++ != ',') return false;
	}
	*count = hifi_scan_count(bits);
	return *count > 0;
}

uint16_t hifi_scan_next(const uint8_t *b, uint32_t from) {
	if (!from) from = 1;
	for (uint32_t p = from; p <= 65535; p++) {
		if (!(p & 7) && !b[p >> 3]) { /* skip an empty byte */
			p += 7;
			continue;
		}
		if (hifi_scan_bit(b, (uint16_t) p)) return (uint16_t) p;
	}
	return 0;
}

uint32_t hifi_scan_count(const uint8_t *b) {
	uint32_t c = 0;
	for (int i = 0; i < HIFI_SCAN_BITS; i++) c += (uint32_t) __builtin_popcount(b[i]);
	return c;
}

uint16_t hifi_scan_cksum(uint32_t sum, const uint8_t *p, size_t n) {
	for (size_t i = 0; i + 1 < n; i += 2) sum += (uint32_t) p[i] << 8 | p[i + 1];
	if (n & 1) sum += (uint32_t) p[n - 1] << 8;
	while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
	return (uint16_t) ~sum;
}

static void put16(uint8_t *p, uint16_t v) {
	p[0] = (uint8_t) (v >> 8);
	p[1] = (uint8_t) v;
}
static void put32(uint8_t *p, uint32_t v) {
	put16(p, (uint16_t) (v >> 16));
	put16(p + 2, (uint16_t) v);
}
static uint16_t get16(const uint8_t *p) { return (uint16_t) (p[0] << 8 | p[1]); }
static uint32_t get32(const uint8_t *p) { return (uint32_t) get16(p) << 16 | get16(p + 2); }

size_t hifi_scan_syn(uint8_t *out, uint32_t src, uint32_t dst, uint16_t sport, uint16_t dport, uint32_t seq) {
	memset(out, 0, HIFI_SCAN_SYN_LEN);
	put16(out, sport);
	put16(out + 2, dport);
	put32(out + 4, seq);
	out[12] = (HIFI_SCAN_SYN_LEN / 4) << 4; /* data offset */
	out[13] = 0x02;                          /* SYN */
	put16(out + 14, 5840);                   /* window */
	out[20] = 2;                             /* MSS option */
	out[21] = 4;
	put16(out + 22, 1436);
	uint32_t sum = (src >> 16) + (src & 0xffff) + (dst >> 16) + (dst & 0xffff) + 6 + HIFI_SCAN_SYN_LEN;
	put16(out + 16, hifi_scan_cksum(sum, out, HIFI_SCAN_SYN_LEN));
	return HIFI_SCAN_SYN_LEN;
}

/* the IPv4 header's length when p starts with a whole one of protocol proto
 * from `from` (0: any source); 0 when it doesn't */
static size_t ip_hdr(const uint8_t *p, size_t n, uint8_t proto, uint32_t from) {
	if (n < 20 || (p[0] >> 4) != 4) return 0;
	size_t hl = (size_t) (p[0] & 15) * 4;
	if (hl < 20 || hl > n || p[9] != proto) return 0;
	if ((get16(p + 6) & 0x1fff) != 0) return 0; /* a later fragment */
	if (from && get32(p + 12) != from) return 0;
	return hl;
}

int hifi_scan_tcp_reply(const uint8_t *p, size_t n, uint32_t from, uint16_t sport, uint32_t seq, uint16_t *port) {
	size_t hl = ip_hdr(p, n, 6, from);
	if (!hl || n - hl < 20) return HIFI_SCAN_NONE;
	const uint8_t *t = p + hl;
	if (get16(t + 2) != sport) return HIFI_SCAN_NONE;
	uint8_t fl = t[13];
	bool ack_ok = (fl & 0x10) && get32(t + 8) == seq + 1;
	*port = get16(t);
	if (!*port || !ack_ok) return HIFI_SCAN_NONE;
	if (fl & 0x04) return HIFI_SCAN_CLOSED;
	if ((fl & 0x12) == 0x12) return HIFI_SCAN_OPEN;
	return HIFI_SCAN_NONE;
}

bool hifi_scan_icmp_unreach(const uint8_t *p, size_t n, uint32_t target, uint16_t sport, uint16_t *port) {
	size_t hl = ip_hdr(p, n, 1, target);
	if (!hl || n - hl < 8) return false;
	const uint8_t *c = p + hl;
	if (c[0] != 3 || c[1] != 3) return false; /* destination unreachable, port */
	const uint8_t *in = c + 8;                /* the datagram that caused it */
	size_t m = n - hl - 8;
	size_t ihl = ip_hdr(in, m, 17, 0);
	if (!ihl || m - ihl < 8 || get32(in + 16) != target) return false;
	const uint8_t *u = in + ihl;
	if (get16(u) != sport) return false;
	*port = get16(u + 2);
	return *port != 0;
}

bool hifi_scan_udp_seen(const uint8_t *p, size_t n, uint32_t from, uint16_t *sport, uint32_t *dst, uint16_t *dport,
			uint16_t *len) {
	size_t hl = ip_hdr(p, n, 17, from);
	if (!hl || n - hl < 8) return false;
	const uint8_t *u = p + hl;
	*sport = get16(u);
	*dport = get16(u + 2);
	*dst = get32(p + 16);
	uint16_t ul = get16(u + 4);
	*len = ul >= 8 ? (uint16_t) (ul - 8) : 0;
	return true;
}

bool hifi_scan_heard_add(hifi_scan_heard_t *tab, int *n, uint16_t sport, uint32_t dst, uint16_t dport) {
	for (int i = 0; i < *n; i++)
		if (tab[i].sport == sport && tab[i].dst == dst && tab[i].dport == dport) {
			tab[i].n++;
			return true;
		}
	if (*n >= HIFI_SCAN_HEARD_MAX) return false;
	tab[*n] = (hifi_scan_heard_t) {.sport = sport, .dst = dst, .dport = dport, .n = 1};
	(*n)++;
	return true;
}

void hifi_scan_heard_str(const hifi_scan_heard_t *h, char *out, size_t n) {
	char ip[16];
	hifi_scan_ip_str(h->dst, ip, sizeof(ip));
	snprintf(out, n, "%u>%s:%u x%u", (unsigned) h->sport, ip, (unsigned) h->dport, (unsigned) h->n);
}

void hifi_scan_ports_json(void *hj, const char *key, const uint8_t *bits, int cap) {
	hj_t *j = hj;
	hj_arr(j, key);
	int shown = 0;
	uint32_t more = 0;
	for (uint16_t p = hifi_scan_next(bits, 1); p; p = p == 65535 ? 0 : hifi_scan_next(bits, (uint32_t) p + 1)) {
		if (shown < cap) {
			hj_int(j, NULL, p);
			shown++;
		} else
			more++;
	}
	hj_arr_end(j);
	if (more) {
		char k[32];
		snprintf(k, sizeof(k), "%s_more", key);
		hj_int(j, k, more);
	}
}
