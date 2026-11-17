/* The lab's LAN scan, its plain-C part (hifi_scan) */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "hifi_json.h"
#include "hifi_scan.h"

#define IP(a, b, c, d) ((uint32_t) (a) << 24 | (uint32_t) (b) << 16 | (uint32_t) (c) << 8 | (uint32_t) (d))

static void targets(void) {
	char err[80];
	uint32_t self = IP(192, 168, 2, 219), m24 = 0xffffff00u, gw = IP(192, 168, 2, 1);
	CHECK(hifi_scan_target_ok(self, m24, gw, IP(192, 168, 2, 222), err, sizeof(err)));
	CHECK(!hifi_scan_target_ok(self, m24, gw, self, err, sizeof(err)));
	CHECK_HAS(err, "this board");
	CHECK(!hifi_scan_target_ok(self, m24, gw, gw, err, sizeof(err)));
	CHECK_HAS(err, "gateway");
	CHECK(!hifi_scan_target_ok(self, m24, gw, IP(192, 168, 2, 0), err, sizeof(err)));
	CHECK(!hifi_scan_target_ok(self, m24, gw, IP(192, 168, 2, 255), err, sizeof(err)));
	CHECK(!hifi_scan_target_ok(self, m24, gw, IP(192, 168, 3, 5), err, sizeof(err)));
	CHECK_HAS(err, "own subnet");
	CHECK(!hifi_scan_target_ok(self, m24, gw, IP(8, 8, 8, 8), err, sizeof(err)));
	CHECK(!hifi_scan_target_ok(self, m24, gw, IP(224, 0, 0, 251), err, sizeof(err)));
	/* a wider network than a /24, or a broken mask, is refused outright */
	CHECK(!hifi_scan_target_ok(self, 0xffff0000u, gw, IP(192, 168, 2, 222), err, sizeof(err)));
	CHECK_HAS(err, "/24");
	CHECK(!hifi_scan_target_ok(self, 0xffffff0fu, gw, IP(192, 168, 2, 222), err, sizeof(err)));
	CHECK(!hifi_scan_target_ok(self, 0, 0, IP(192, 168, 2, 222), err, sizeof(err)));
	CHECK(!hifi_scan_target_ok(0, m24, gw, IP(0, 0, 0, 7), err, sizeof(err)));
	/* a /28: its own network and broadcast */
	uint32_t m28 = 0xfffffff0u, s28 = IP(10, 0, 0, 18);
	CHECK(hifi_scan_target_ok(s28, m28, IP(10, 0, 0, 17), IP(10, 0, 0, 20), err, sizeof(err)));
	CHECK(!hifi_scan_target_ok(s28, m28, IP(10, 0, 0, 17), IP(10, 0, 0, 31), err, sizeof(err)));
	CHECK(!hifi_scan_target_ok(s28, m28, IP(10, 0, 0, 17), IP(10, 0, 0, 16), err, sizeof(err)));
	CHECK(!hifi_scan_target_ok(s28, m28, IP(10, 0, 0, 17), IP(10, 0, 0, 40), err, sizeof(err)));

	/* a /31 and a /32: no other host to scan (/31 has its pair refused as
	 * the "broadcast", which is the safe side) */
	CHECK(!hifi_scan_target_ok(IP(10, 0, 0, 18), 0xfffffffeu, 0, IP(10, 0, 0, 19), err, sizeof(err)));
	CHECK(!hifi_scan_target_ok(IP(10, 0, 0, 18), 0xffffffffu, 0, IP(10, 0, 0, 18), err, sizeof(err)));
	CHECK(!hifi_scan_target_ok(IP(10, 0, 0, 18), 0xffffffffu, 0, IP(10, 0, 0, 19), err, sizeof(err)));
	/* multicast and class E inside a (bogus) subnet of their own, loopback */
	CHECK(!hifi_scan_target_ok(IP(224, 0, 0, 5), m24, 0, IP(224, 0, 0, 251), err, sizeof(err)));
	CHECK_HAS(err, "unicast");
	CHECK(!hifi_scan_target_ok(IP(240, 0, 0, 5), m24, 0, IP(240, 0, 0, 6), err, sizeof(err)));
	CHECK(!hifi_scan_target_ok(IP(127, 0, 0, 5), m24, 0, IP(127, 0, 0, 6), err, sizeof(err)));
	CHECK(!hifi_scan_target_ok(self, m24, gw, 0, err, sizeof(err)));
	/* no gateway (0.0.0.0): the target is still checked as usual */
	CHECK(hifi_scan_target_ok(self, m24, 0, IP(192, 168, 2, 1), err, sizeof(err)));
	CHECK(!hifi_scan_target_ok(self, m24, 0, IP(192, 168, 2, 255), err, sizeof(err)));

	uint32_t ip;
	CHECK(hifi_scan_parse_ip("192.168.2.222", &ip) && ip == IP(192, 168, 2, 222));
	CHECK(hifi_scan_parse_ip("0.0.0.0", &ip) && ip == 0);
	CHECK(!hifi_scan_parse_ip("192.168.2.256", &ip));
	CHECK(!hifi_scan_parse_ip("192.168.2", &ip));
	CHECK(!hifi_scan_parse_ip("192.168.2.1.5", &ip));
	CHECK(!hifi_scan_parse_ip("192.168.02.0001", &ip));
	CHECK(!hifi_scan_parse_ip("a.b.c.d", &ip));
	CHECK(!hifi_scan_parse_ip("", &ip));
	char s[16];
	hifi_scan_ip_str(IP(192, 168, 2, 222), s, sizeof(s));
	CHECK_STR(s, "192.168.2.222");
}

static void ports(void) {
	uint8_t *b = calloc(1, HIFI_SCAN_BITS);
	uint32_t n;
	CHECK(hifi_scan_ports("1-65535", b, &n) && n == 65535);
	CHECK(!hifi_scan_bit(b, 0) && hifi_scan_bit(b, 65535));
	CHECK(hifi_scan_ports("80,5000,5353,49152-49154", b, &n) && n == 6);
	CHECK(hifi_scan_next(b, 1) == 80 && hifi_scan_next(b, 81) == 5000 && hifi_scan_next(b, 5354) == 49152);
	CHECK(hifi_scan_next(b, 49155) == 0 && hifi_scan_next(b, 0) == 80);
	CHECK(hifi_scan_ports("7,7,7", b, &n) && n == 1);
	CHECK(!hifi_scan_ports("", b, &n));
	CHECK(!hifi_scan_ports("0", b, &n));
	CHECK(!hifi_scan_ports("65536", b, &n));
	CHECK(!hifi_scan_ports("10-5", b, &n));
	CHECK(!hifi_scan_ports("1,", b, &n));
	CHECK(!hifi_scan_ports("1;2", b, &n));
	CHECK(!hifi_scan_ports("99999999999", b, &n));
	/* the top of the range: next() never wraps past 65535 */
	CHECK(hifi_scan_ports("65528-65535", b, &n) && n == 8);
	CHECK(hifi_scan_next(b, 1) == 65528 && hifi_scan_next(b, 65534) == 65534 && hifi_scan_next(b, 65535) == 65535);
	CHECK(hifi_scan_next(b, 65536) == 0);
	CHECK(hifi_scan_ports("65535", b, &n) && n == 1 && hifi_scan_next(b, 1) == 65535);
	CHECK(hifi_scan_ports("65527,65529", b, &n) && hifi_scan_next(b, 65528) == 65529);
	uint32_t walked = 0;
	hifi_scan_ports("1-65535", b, &n);
	for (uint16_t p = hifi_scan_next(b, 1); p; p = p == 65535 ? 0 : hifi_scan_next(b, (uint32_t) p + 1)) walked++;
	CHECK(walked == 65535);

	char out[300];
	hj_t j;
	hj_init(&j, out, sizeof(out));
	hj_obj(&j, NULL);
	hifi_scan_ports("1-5,65535", b, &n);
	hifi_scan_ports_json(&j, "open", b, 3);
	hifi_scan_ports_json(&j, "few", b, 10);
	hj_obj_end(&j);
	CHECK_STR(out, "{\"open\":[1,2,3],\"open_more\":3,\"few\":[1,2,3,4,5,65535]}");
	/* a buffer too small: cut off and flagged, never written past */
	char small[24 + 8];
	memset(small, 0x5a, sizeof(small));
	hj_init(&j, small, 24);
	hj_obj(&j, NULL);
	hifi_scan_ports("1-65535", b, &n);
	hifi_scan_ports_json(&j, "silent", b, 24);
	hj_obj_end(&j);
	CHECK(j.overflow);
	int intact = 1;
	for (int i = 24; i < (int) sizeof(small); i++) intact &= small[i] == 0x5a;
	CHECK(intact);
	free(b);
}

/* a SYN and the replies, checked against hand-made packets */
static void put16(uint8_t *p, uint16_t v) {
	p[0] = (uint8_t) (v >> 8);
	p[1] = (uint8_t) v;
}
static void put32(uint8_t *p, uint32_t v) {
	put16(p, (uint16_t) (v >> 16));
	put16(p + 2, (uint16_t) v);
}
static size_t iphdr(uint8_t *p, uint8_t proto, uint32_t src, uint32_t dst, size_t payload) {
	memset(p, 0, 20);
	p[0] = 0x45;
	put16(p + 2, (uint16_t) (20 + payload));
	p[8] = 64;
	p[9] = proto;
	put32(p + 12, src);
	put32(p + 16, dst);
	put16(p + 10, hifi_scan_cksum(0, p, 20));
	return 20;
}

static void packets(void) {
	uint32_t a = IP(192, 168, 2, 219), t = IP(192, 168, 2, 222);
	uint8_t syn[HIFI_SCAN_SYN_LEN];
	CHECK(hifi_scan_syn(syn, a, t, 41000, 80, 0x12345678u) == HIFI_SCAN_SYN_LEN);
	CHECK(syn[0] == 41000 >> 8 && syn[1] == (41000 & 255) && syn[2] == 0 && syn[3] == 80);
	CHECK(syn[13] == 0x02 && syn[12] == 0x60);
	/* the checksum over the pseudo header and segment comes out 0 */
	uint8_t ps[12 + HIFI_SCAN_SYN_LEN];
	put32(ps, a);
	put32(ps + 4, t);
	ps[8] = 0;
	ps[9] = 6;
	put16(ps + 10, HIFI_SCAN_SYN_LEN);
	memcpy(ps + 12, syn, sizeof(syn));
	CHECK(hifi_scan_cksum(0, ps, sizeof(ps)) == 0);
	/* RFC 1071's example: 0001 f203 f4f5 f6f7 sums to ddf2 */
	uint8_t r[] = {0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7};
	CHECK(hifi_scan_cksum(0, r, sizeof(r)) == (uint16_t) ~0xddf2);
	uint8_t odd[] = {0xab};
	CHECK(hifi_scan_cksum(0, odd, 1) == (uint16_t) ~0xab00);

	/* replies: a SYN-ACK, a RST, and ones that aren't ours */
	uint8_t pk[80];
	uint16_t port = 0;
	size_t hl = iphdr(pk, 6, t, a, 20);
	uint8_t *tc = pk + hl;
	memset(tc, 0, 20);
	put16(tc, 80);
	put16(tc + 2, 41000);
	put32(tc + 8, 0x12345679u);
	tc[12] = 0x50;
	tc[13] = 0x12;
	CHECK(hifi_scan_tcp_reply(pk, 40, t, 41000, 0x12345678u, &port) == HIFI_SCAN_OPEN && port == 80);
	tc[13] = 0x14;
	CHECK(hifi_scan_tcp_reply(pk, 40, t, 41000, 0x12345678u, &port) == HIFI_SCAN_CLOSED);
	CHECK(hifi_scan_tcp_reply(pk, 40, t, 41001, 0x12345678u, &port) == HIFI_SCAN_NONE); /* another port of ours */
	CHECK(hifi_scan_tcp_reply(pk, 40, t, 41000, 0x12345677u, &port) == HIFI_SCAN_NONE); /* wrong ack */
	CHECK(hifi_scan_tcp_reply(pk, 40, IP(192, 168, 2, 9), 41000, 0x12345678u, &port) == HIFI_SCAN_NONE);
	CHECK(hifi_scan_tcp_reply(pk, 39, t, 41000, 0x12345678u, &port) == HIFI_SCAN_NONE); /* short */
	put16(tc, 0); /* from port 0: never ours to count */
	tc[13] = 0x12;
	CHECK(hifi_scan_tcp_reply(pk, 40, t, 41000, 0x12345678u, &port) == HIFI_SCAN_NONE);
	tc[13] = 0x14;
	CHECK(hifi_scan_tcp_reply(pk, 40, t, 41000, 0x12345678u, &port) == HIFI_SCAN_NONE);
	put16(tc, 80);
	tc[13] = 0x10; /* a bare ACK */
	CHECK(hifi_scan_tcp_reply(pk, 40, t, 41000, 0x12345678u, &port) == HIFI_SCAN_NONE);
	tc[13] = 0x02; /* a SYN without the ACK */
	CHECK(hifi_scan_tcp_reply(pk, 40, t, 41000, 0x12345678u, &port) == HIFI_SCAN_NONE);
	pk[0] = 0x4f; /* an IP header longer than the packet */
	CHECK(hifi_scan_tcp_reply(pk, 40, t, 41000, 0x12345678u, &port) == HIFI_SCAN_NONE);
	pk[0] = 0x44; /* shorter than 20 */
	CHECK(hifi_scan_tcp_reply(pk, 40, t, 41000, 0x12345678u, &port) == HIFI_SCAN_NONE);
	CHECK(hifi_scan_tcp_reply(pk, 0, t, 41000, 0x12345678u, &port) == HIFI_SCAN_NONE);

	/* ICMP port unreachable quoting our datagram to port 5000 */
	hl = iphdr(pk, 1, t, a, 8 + 28);
	uint8_t *ic = pk + hl;
	memset(ic, 0, 8);
	ic[0] = 3;
	ic[1] = 3;
	size_t ihl = iphdr(ic + 8, 17, a, t, 8);
	uint8_t *u = ic + 8 + ihl;
	put16(u, 41000);
	put16(u + 2, 5000);
	put16(u + 4, 8);
	size_t n = hl + 8 + ihl + 8;
	CHECK(hifi_scan_icmp_unreach(pk, n, t, 41000, &port) && port == 5000);
	CHECK(!hifi_scan_icmp_unreach(pk, n, t, 41001, &port));
	CHECK(!hifi_scan_icmp_unreach(pk, n, IP(192, 168, 2, 9), 41000, &port));
	CHECK(!hifi_scan_icmp_unreach(pk, n - 1, t, 41000, &port)); /* its UDP header cut */
	ic[1] = 1;                                                    /* host unreachable */
	CHECK(!hifi_scan_icmp_unreach(pk, n, t, 41000, &port));
	ic[1] = 3;
	put32(ic + 8 + 16, IP(192, 168, 2, 5)); /* quoting a datagram to someone else */
	CHECK(!hifi_scan_icmp_unreach(pk, n, t, 41000, &port));

	/* a datagram the target sent: its mDNS to the group */
	hl = iphdr(pk, 17, t, IP(224, 0, 0, 251), 8 + 30);
	u = pk + hl;
	put16(u, 5353);
	put16(u + 2, 5353);
	put16(u + 4, 38);
	uint16_t sp, dp, len;
	uint32_t dst;
	CHECK(hifi_scan_udp_seen(pk, hl + 8, t, &sp, &dst, &dp, &len) && sp == 5353 && dp == 5353 &&
	      dst == IP(224, 0, 0, 251) && len == 30);
	CHECK(!hifi_scan_udp_seen(pk, hl + 7, t, &sp, &dst, &dp, &len));
	CHECK(!hifi_scan_udp_seen(pk, hl + 8, a, &sp, &dst, &dp, &len));
	put16(pk + 6, 0x0010); /* a later fragment: no UDP header to read */
	CHECK(!hifi_scan_udp_seen(pk, hl + 8, t, &sp, &dst, &dp, &len));
}

static void heard(void) {
	hifi_scan_heard_t tab[HIFI_SCAN_HEARD_MAX];
	int n = 0;
	CHECK(hifi_scan_heard_add(tab, &n, 5353, IP(224, 0, 0, 251), 5353));
	CHECK(hifi_scan_heard_add(tab, &n, 5353, IP(224, 0, 0, 251), 5353));
	CHECK(n == 1 && tab[0].n == 2);
	for (int i = 0; i < HIFI_SCAN_HEARD_MAX - 1; i++) CHECK(hifi_scan_heard_add(tab, &n, 1000 + i, 1, 2));
	CHECK(n == HIFI_SCAN_HEARD_MAX);
	CHECK(!hifi_scan_heard_add(tab, &n, 9, 1, 2));
	CHECK(hifi_scan_heard_add(tab, &n, 1000, 1, 2)); /* a known one still counts */
	char s[48];
	hifi_scan_heard_str(&tab[0], s, sizeof(s));
	CHECK_STR(s, "5353>224.0.0.251:5353 x2");
}

int main(void) {
	targets();
	ports();
	packets();
	heard();
	return check_report("scan");
}
