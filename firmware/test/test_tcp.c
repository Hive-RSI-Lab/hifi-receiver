/* The printer relay's rules (core/hifi_tcp.c): the address, the ports and
 * the signed commands' fields. */
#include <string.h>

#include "check.h"
#include "hifi_tcp.h"

static void ips(void) {
	uint32_t ip = 0;
	CHECK(hifi_tcp_parse_ip("192.168.2.214", &ip) && ip == 0xc0a802d6u);
	CHECK(hifi_tcp_parse_ip("10.0.0.1", &ip) && ip == 0x0a000001u);
	const char *no[] = {"", "printer.local", "192.168.2", "192.168.2.214.1", "192.168.2.256", "192.168.02.214",
			    " 192.168.2.214", "192.168.2.214 ", "192.168.2.-1", "+1.2.3.4", "1..2.3", "0.0.0.0",
			    "255.255.255.255", "224.0.0.251", "239.1.1.1", "1234.1.1.1", "0x7f.0.0.1", NULL};
	for (int i = 0; no[i]; i++) {
		ip = 7;
		CHECK(!hifi_tcp_parse_ip(no[i], &ip));
		CHECK(ip == 7);
	}
	CHECK(!hifi_tcp_parse_ip(NULL, &ip));
}

static void ports(void) {
	CHECK(hifi_tcp_port_ok(8883));
	CHECK(hifi_tcp_port_ok(990));
	CHECK(hifi_tcp_port_ok(1024));
	CHECK(hifi_tcp_port_ok(65535));
	CHECK(!hifi_tcp_port_ok(80));
	CHECK(!hifi_tcp_port_ok(22));
	CHECK(!hifi_tcp_port_ok(1023));
	CHECK(!hifi_tcp_port_ok(0));
	CHECK(!hifi_tcp_port_ok(65536));
	CHECK(!hifi_tcp_port_ok(-8883));
}

static void hex(void) {
	uint8_t b[32];
	const char *h = "513ce6bf00112233445566778899aabbccddeeff00112233445566778899d61e";
	CHECK(hifi_tcp_parse_hex32(h, b) && b[0] == 0x51 && b[31] == 0x1e);
	char back[65];
	hifi_tcp_hex32(b, back);
	CHECK_STR(back, h);
	CHECK(hifi_tcp_parse_hex32("513CE6BF00112233445566778899AABBCCDDEEFF00112233445566778899D61E", b));
	CHECK(!hifi_tcp_parse_hex32("513ce6bf", b));
	CHECK(!hifi_tcp_parse_hex32("513ce6bf00112233445566778899aabbccddeeff00112233445566778899d61g", b));
	CHECK(!hifi_tcp_parse_hex32("513ce6bf00112233445566778899aabbccddeeff00112233445566778899d61e0", b));
}

static void cmds(void) {
	hifi_tcp_cmd_t c;
	char err[64];
	const char *fp = "\"513ce6bf00112233445566778899aabbccddeeff00112233445566778899d61e\"";
	char body[200];
	snprintf(body, sizeof(body), "{\"ip\":\"192.168.2.214\",\"sha256\":%s}", fp);
	CHECK(hifi_tcp_cmd_parse("POST", "/relay/enable", body, &c, err, sizeof(err)) == 0);
	CHECK(c.op == HIFI_TCP_ENABLE && c.ip == 0xc0a802d6u && c.sha256[0] == 0x51);
	snprintf(body, sizeof(body), "{\"ip\":\"printer.lan\",\"sha256\":%s}", fp);
	CHECK(hifi_tcp_cmd_parse("POST", "/relay/enable", body, &c, err, sizeof(err)) == 400);
	CHECK_HAS(err, "IPv4");
	CHECK(hifi_tcp_cmd_parse("POST", "/relay/enable", "{\"ip\":\"192.168.2.214\"}", &c, err, sizeof(err)) == 400);
	CHECK(hifi_tcp_cmd_parse("GET", "/relay/enable", body, &c, err, sizeof(err)) == 405);
	CHECK(hifi_tcp_cmd_parse("POST", "/relay/enable", NULL, &c, err, sizeof(err)) == 400);

	CHECK(hifi_tcp_cmd_parse("POST", "/relay/open", "{\"sid\":3,\"port\":8883}", &c, err, sizeof(err)) == 0);
	CHECK(c.op == HIFI_TCP_OPEN && c.sid == 3 && c.port == 8883);
	CHECK(hifi_tcp_cmd_parse("POST", "/relay/open", "{\"sid\":3,\"port\":50000}", &c, err, sizeof(err)) == 0);
	CHECK(hifi_tcp_cmd_parse("POST", "/relay/open", "{\"sid\":3,\"port\":80}", &c, err, sizeof(err)) == 400);
	CHECK(hifi_tcp_cmd_parse("POST", "/relay/open", "{\"sid\":3,\"port\":8883.5}", &c, err, sizeof(err)) == 400);
	CHECK(hifi_tcp_cmd_parse("POST", "/relay/open", "{\"sid\":0,\"port\":8883}", &c, err, sizeof(err)) == 400);
	CHECK(hifi_tcp_cmd_parse("POST", "/relay/open", "{\"sid\":256,\"port\":8883}", &c, err, sizeof(err)) == 400);
	CHECK(hifi_tcp_cmd_parse("POST", "/relay/open", "{\"port\":8883}", &c, err, sizeof(err)) == 400);
	/* no host field is read: the board only ever dials the enabled address */
	CHECK(hifi_tcp_cmd_parse("POST", "/relay/open", "{\"sid\":1,\"port\":8883,\"ip\":\"8.8.8.8\"}", &c, err, sizeof(err)) == 0);
	CHECK(c.ip == 0);

	CHECK(hifi_tcp_cmd_parse("POST", "/relay/close", "{\"sid\":0}", &c, err, sizeof(err)) == 0);
	CHECK(c.op == HIFI_TCP_CLOSE && c.sid == 0);
	CHECK(hifi_tcp_cmd_parse("POST", "/relay/close", "{\"sid\":-1}", &c, err, sizeof(err)) == 400);
	CHECK(hifi_tcp_cmd_parse("POST", "/relay/disable", NULL, &c, err, sizeof(err)) == 0 && c.op == HIFI_TCP_DISABLE);
	CHECK(hifi_tcp_cmd_parse("GET", "/relay/state", NULL, &c, err, sizeof(err)) == 0 && c.op == HIFI_TCP_STATE);
	CHECK(hifi_tcp_cmd_parse("POST", "/relay/state", NULL, &c, err, sizeof(err)) == 405);
	CHECK(hifi_tcp_cmd_parse("POST", "/relay/connect", "{}", &c, err, sizeof(err)) == 404);
	CHECK(hifi_tcp_cmd_parse("POST", "/relay/open", "{\"sid\":1,", &c, err, sizeof(err)) == 400);
}

static void frames(void) {
	uint8_t f[8];
	CHECK(hifi_tcp_frame(f, 7, (const uint8_t *) "abc", 3) == 4 && f[0] == 7 && !memcmp(f + 1, "abc", 3));
	CHECK(hifi_tcp_frame(f, 9, NULL, 0) == 1 && f[0] == 9);
	uint8_t k[6];
	CHECK(hifi_tcp_ack(k, 5, 0x01020304u) == 6 && k[0] == 0 && k[1] == 5 && k[2] == 1 && k[3] == 2 && k[4] == 3 && k[5] == 4);
}

int main(void) {
	ips();
	ports();
	hex();
	cmds();
	frames();
	return check_report("tcp");
}
