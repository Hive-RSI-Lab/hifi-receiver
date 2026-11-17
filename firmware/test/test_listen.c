/* "Cannot open the home network" (core/hifi_listen.c): the board's listening
 * sockets against the allow list, with planted listeners that must fail */
#include "check.h"
#include "hifi_listen.h"

#define T(p) {.tcp = true, .port = (p)}
#define U(p) {.tcp = false, .port = (p)}
#define LO_T(p) {.tcp = true, .port = (p), .loopback = true}
#define LO_U(p) {.tcp = false, .port = (p), .loopback = true}
#define N(a) ((int) (sizeof(a) / sizeof(a[0])))

/* what a playing Lite shows: web UI, AirPlay with its RTP ports, mDNS, the
 * DHCP client, httpd's control socket, squeezelite's wake pair, a DNS query
 * on a random port */
static const hifi_sock_t board[] = {
	T(80), T(5000), LO_T(53211), U(5353), U(68), LO_U(32768),
	U(49153), U(49154), U(49155), U(6411),
};

int main(void) {
	char why[128];

	/* the allow list, one socket at a time */
	CHECK(hifi_listen_allowed(&(hifi_sock_t) T(80), false));
	CHECK(hifi_listen_allowed(&(hifi_sock_t) T(5000), false));
	CHECK(!hifi_listen_allowed(&(hifi_sock_t) T(23), false));     /* telnet */
	CHECK(!hifi_listen_allowed(&(hifi_sock_t) T(8080), false));
	CHECK(!hifi_listen_allowed(&(hifi_sock_t) T(3483), false));   /* slimproto server side */
	CHECK(!hifi_listen_allowed(&(hifi_sock_t) T(50000), false));  /* no ephemeral pass for TCP */
	CHECK(!hifi_listen_allowed(&(hifi_sock_t) T(443), true));
	CHECK(hifi_listen_allowed(&(hifi_sock_t) LO_T(23), false));   /* loopback only */
	CHECK(hifi_listen_allowed(&(hifi_sock_t) U(5353), false));
	CHECK(hifi_listen_allowed(&(hifi_sock_t) U(68), false));
	CHECK(!hifi_listen_allowed(&(hifi_sock_t) U(67), false));     /* DHCP server off the AP */
	CHECK(!hifi_listen_allowed(&(hifi_sock_t) U(53), false));     /* an open resolver */
	CHECK(hifi_listen_allowed(&(hifi_sock_t) U(67), true));
	CHECK(hifi_listen_allowed(&(hifi_sock_t) U(53), true));
	CHECK(!hifi_listen_allowed(&(hifi_sock_t) U(1900), false));   /* SSDP / UPnP */
	CHECK(!hifi_listen_allowed(&(hifi_sock_t) U(1900), true));
	CHECK(!hifi_listen_allowed(&(hifi_sock_t) U(5351), false));   /* NAT-PMP */
	CHECK(!hifi_listen_allowed(&(hifi_sock_t) U(161), false));    /* SNMP */
	CHECK(!hifi_listen_allowed(&(hifi_sock_t) U(49151), false));  /* just below the range */
	CHECK(hifi_listen_allowed(&(hifi_sock_t) U(49152), false));
	CHECK(hifi_listen_allowed(&(hifi_sock_t) U(65535), false));

	/* the board as it is: one scan alone flags the in-flight DNS query (udp/6411) */
	CHECK(hifi_listen_check(NULL, 0, board, N(board), false, why, sizeof(why)) == 1);
	CHECK_STR(why, "1 open: udp/6411");
	/* two scans: the query is gone from the second, so all clear */
	static const hifi_sock_t later[] = {T(80), T(5000), LO_T(53211), U(5353), U(68), LO_U(32768), U(49153)};
	CHECK(hifi_listen_check(board, N(board), later, N(later), false, why, sizeof(why)) == 0);
	CHECK_STR(why, "7 sockets, all allowed");
	/* a new DNS query in the second scan only: still clear */
	static const hifi_sock_t later2[] = {T(80), U(5353), U(2222)};
	CHECK(hifi_listen_check(later, N(later), later2, N(later2), false, why, sizeof(why)) == 0);

	/* planted listeners that stay: telnet and UPnP, in both scans */
	static const hifi_sock_t planted[] = {T(80), T(23), U(5353), U(1900), T(5000)};
	CHECK(hifi_listen_check(planted, N(planted), planted, N(planted), false, why, sizeof(why)) == 2);
	CHECK_STR(why, "2 open: tcp/23 udp/1900");
	/* the same port on loopback in the first scan isn't the same socket */
	static const hifi_sock_t lo1900[] = {LO_U(1900)}, lan1900[] = {U(1900)};
	CHECK(hifi_listen_check(lo1900, 1, lan1900, 1, false, why, sizeof(why)) == 0);
	CHECK(hifi_listen_check(lan1900, 1, lan1900, 1, false, why, sizeof(why)) == 1);
	/* a TCP listener counts from one scan: a quick open/close can't slip past */
	static const hifi_sock_t lan23[] = {T(23)};
	CHECK(hifi_listen_check(NULL, 0, lan23, 1, false, why, sizeof(why)) == 1);
	CHECK(hifi_listen_check(later, N(later), lan23, 1, false, why, sizeof(why)) == 1);
	CHECK_STR(why, "1 open: tcp/23");
	/* tcp/53 is not udp/53 */
	static const hifi_sock_t dns_t[] = {T(53)};
	CHECK(hifi_listen_check(dns_t, 1, dns_t, 1, true, why, sizeof(why)) == 1);

	/* the setup AP: its DHCP and DNS servers pass while it is up, fail after */
	static const hifi_sock_t ap[] = {T(80), U(67), U(53), U(5353)};
	CHECK(hifi_listen_check(ap, N(ap), ap, N(ap), true, why, sizeof(why)) == 0);
	CHECK(hifi_listen_check(ap, N(ap), ap, N(ap), false, why, sizeof(why)) == 2);
	CHECK_STR(why, "2 open: udp/67 udp/53");

	/* an unreadable list fails, closed */
	CHECK(hifi_listen_check(NULL, 0, NULL, -1, false, why, sizeof(why)) == 1);
	CHECK_STR(why, "socket list unreadable");
	CHECK(hifi_listen_check(NULL, -1, board, N(board), false, why, sizeof(why)) == 1);

	/* many strays: the count is right and the list stays inside its buffer */
	hifi_sock_t many[HIFI_LISTEN_MAX];
	for (int i = 0; i < HIFI_LISTEN_MAX; i++) many[i] = (hifi_sock_t) T(10000 + i);
	CHECK(hifi_listen_check(many, HIFI_LISTEN_MAX, many, HIFI_LISTEN_MAX, false, why, sizeof(why)) ==
	      HIFI_LISTEN_MAX);
	CHECK_HAS(why, "48 open: tcp/10000 tcp/10001");
	char tiny[8];
	CHECK(hifi_listen_check(planted, N(planted), planted, N(planted), false, tiny, sizeof(tiny)) == 2);
	CHECK(strlen(tiny) == 7);

	return check_report("listen");
}
