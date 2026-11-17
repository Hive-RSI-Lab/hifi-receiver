/*
 * The lab's LAN scan (esp/lab_lanscan.c): one lab board probes another
 * board's ports from inside the home network, which the tower can't reach.
 * This is the plain-C part, host-tested: which targets a scan may touch,
 * port lists, the TCP SYN it sends and the replies it reads.
 *
 * Addresses are IPv4 in host order (192.168.2.219 is 0xc0a802db).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HIFI_SCAN_BITS 8192 /* a bitmap of all 65536 ports */
#define HIFI_SCAN_SYN_LEN 24 /* TCP header with an MSS option */

/* A scan may only touch another host on the board's own subnet, a /24 or
 * narrower: never itself, the gateway, the network or broadcast address, a
 * multicast or a zero address. false with err filled. */
bool hifi_scan_target_ok(uint32_t self, uint32_t mask, uint32_t gw, uint32_t target, char *err, size_t n);

bool hifi_scan_parse_ip(const char *s, uint32_t *ip);
void hifi_scan_ip_str(uint32_t ip, char *out, size_t n); /* n >= 16 */

/* "1-1024,5353,49152-49160" into bits (cleared first); ports 1-65535.
 * false on a bad list or an empty one. *count: the ports set. */
bool hifi_scan_ports(const char *spec, uint8_t bits[HIFI_SCAN_BITS], uint32_t *count);

static inline bool hifi_scan_bit(const uint8_t *b, uint16_t p) { return b[p >> 3] & (1u << (p & 7)); }
static inline void hifi_scan_set(uint8_t *b, uint16_t p) { b[p >> 3] |= (uint8_t) (1u << (p & 7)); }
/* the next set port at or after `from` (1-65535), 0 none */
uint16_t hifi_scan_next(const uint8_t *b, uint32_t from);
uint32_t hifi_scan_count(const uint8_t *b);

/* The Internet checksum of p, added to sum (for a pseudo header) and folded */
uint16_t hifi_scan_cksum(uint32_t sum, const uint8_t *p, size_t n);

/* A SYN from src:sport to dst:dport with sequence seq, checksum filled:
 * HIFI_SCAN_SYN_LEN bytes into out (a raw IPPROTO_TCP socket adds the IP
 * header). */
size_t hifi_scan_syn(uint8_t *out, uint32_t src, uint32_t dst, uint16_t sport, uint16_t dport, uint32_t seq);

enum { HIFI_SCAN_NONE = 0, HIFI_SCAN_OPEN, HIFI_SCAN_CLOSED };
/* A packet from a raw TCP socket (IPv4 header first): HIFI_SCAN_OPEN for a
 * SYN-ACK from `from` to our sport acking seq+1, HIFI_SCAN_CLOSED for a RST
 * acking it, NONE for anything else. *port: the target's port. */
int hifi_scan_tcp_reply(const uint8_t *p, size_t n, uint32_t from, uint16_t sport, uint32_t seq, uint16_t *port);

/* A packet from a raw ICMP socket: true when it is a port unreachable from
 * `target` for a UDP datagram we sent it from sport; *port: its port */
bool hifi_scan_icmp_unreach(const uint8_t *p, size_t n, uint32_t target, uint16_t sport, uint16_t *port);

/* A packet from a raw UDP socket: the UDP datagram's ports and its
 * destination (unicast to us, or a multicast group), when it came from
 * `from`; *len: its payload's length */
bool hifi_scan_udp_seen(const uint8_t *p, size_t n, uint32_t from, uint16_t *sport, uint32_t *dst, uint16_t *dport,
			uint16_t *len);

/* What the target was heard sending: one entry per (its port, where to) */
typedef struct {
	uint16_t sport, dport;
	uint32_t dst;
	uint32_t n;
} hifi_scan_heard_t;
#define HIFI_SCAN_HEARD_MAX 8
/* counts into tab (n entries in use, at most HIFI_SCAN_HEARD_MAX); false
 * when the table is full and this one is new (counted in *lost by the caller) */
bool hifi_scan_heard_add(hifi_scan_heard_t *tab, int *n, uint16_t sport, uint32_t dst, uint16_t dport);

/* "5353>224.0.0.251:5353 x3" */
void hifi_scan_heard_str(const hifi_scan_heard_t *h, char *out, size_t n);

/* An array of up to cap set ports (key) and, when there are more, key_more
 * with the count of the rest; hj is an open hj_t object */
void hifi_scan_ports_json(void *hj, const char *key, const uint8_t *bits, int cap);
