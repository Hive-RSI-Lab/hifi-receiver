/* the Speakers list (core/hifi_peers.c): the table and GET /api/v1/peers */
#include "check.h"
#include "hifi_peers.h"

#define S(x) ((long long) (x) * 1000000)

static hifi_peer_t peer(const char *ip, const char *host, const char *name) {
	hifi_peer_t p = {0};
	snprintf(p.ip, sizeof(p.ip), "%s", ip);
	snprintf(p.host, sizeof(p.host), "%s", host);
	snprintf(p.name, sizeof(p.name), "%s", name);
	return p;
}

int main(void) {
	hifi_peers_t t = {0};
	char out[6144];
	hifi_peer_t self = peer("192.168.1.10", "hifi-lite", "Kitchen");
	snprintf(self.model, sizeof(self.model), "lite");

	/* alone: self, and the search flag */
	CHECK(hifi_peers_json(&t, &self, true, S(1), out, sizeof(out)) == 200);
	CHECK(strstr(out, "\"peers\":[{\"name\":\"Kitchen\",\"host\":\"hifi-lite\",\"ip\":\"192.168.1.10\"") != NULL);
	CHECK(strstr(out, "\"self\":true") != NULL);
	CHECK(strstr(out, "\"searching\":true") != NULL);

	/* rows without an IP or host are ignored */
	hifi_peer_t bad = peer("", "x", "X");
	hifi_peers_seen(&t, &bad, S(1));
	bad = peer("192.168.1.9", "", "X");
	hifi_peers_seen(&t, &bad, S(1));
	CHECK(t.n == 0);

	/* TXT items: name, model, fw; values not 0-terminated */
	hifi_peer_t b = peer("192.168.1.12", "hifi-lite-2", "");
	hifi_peers_txt(&b, "name", "Living roomXXX", 11);
	hifi_peers_txt(&b, "model", "lite", 4);
	hifi_peers_txt(&b, "fw", "0.9.1", 5);
	hifi_peers_txt(&b, "board", "lite-v1", 7);
	hifi_peers_txt(&b, "api", NULL, 0);
	CHECK_STR(b.name, "Living room");
	CHECK_STR(b.model, "lite");
	CHECK_STR(b.fw, "0.9.1");
	char long_name[200];
	memset(long_name, 'a', sizeof(long_name));
	hifi_peer_t c = peer("192.168.1.13", "hifi-plus", "");
	hifi_peers_txt(&c, "name", long_name, sizeof(long_name));
	CHECK(strlen(c.name) == sizeof(c.name) - 1);
	hifi_peers_txt(&c, "name", "Bedroom", 7);

	hifi_peers_seen(&t, &b, S(2));
	hifi_peers_seen(&t, &c, S(3));
	/* its own answer comes back too: left out of the list */
	hifi_peer_t me = self;
	hifi_peers_seen(&t, &me, S(3));
	CHECK(t.n == 3);
	CHECK(hifi_peers_json(&t, &self, false, S(5), out, sizeof(out)) == 200);
	/* self first, then by name: Bedroom before Living room */
	const char *k = strstr(out, "Kitchen"), *bed = strstr(out, "Bedroom"), *liv = strstr(out, "Living room");
	CHECK(k && bed && liv && k < bed && bed < liv);
	/* only the first row is self */
	CHECK(strstr(out, "\"self\":true") < bed && !strstr(bed, "\"self\":true"));
	CHECK(strstr(bed, "\"seen_s\":2") != NULL);
	CHECK(strstr(out, "\"searching\":false") != NULL);
	/* no TXT name: the host stands in; no model: null */
	hifi_peer_t d = peer("192.168.1.14", "hifi-ultra", "");
	hifi_peers_seen(&t, &d, S(5));
	hifi_peers_json(&t, &self, false, S(5), out, sizeof(out));
	CHECK(strstr(out, "{\"name\":\"hifi-ultra\",\"host\":\"hifi-ultra\",\"ip\":\"192.168.1.14\",\"model\":null") != NULL);

	/* the same IP again: updated in place */
	hifi_peers_txt(&b, "name", "Lounge", 6);
	hifi_peers_seen(&t, &b, S(10));
	CHECK(t.n == 4);
	hifi_peers_json(&t, &self, false, S(10), out, sizeof(out));
	CHECK(strstr(out, "Lounge") && !strstr(out, "Living room"));
	/* the same box at a new address (DHCP): the old row goes */
	hifi_peer_t moved = c;
	snprintf(moved.ip, sizeof(moved.ip), "192.168.1.99");
	hifi_peers_seen(&t, &moved, S(11));
	CHECK(t.n == 4);
	hifi_peers_json(&t, &self, false, S(11), out, sizeof(out));
	CHECK(strstr(out, "192.168.1.99") && !strstr(out, "192.168.1.13"));
	/* host names compare without case */
	hifi_peer_t moved2 = moved;
	snprintf(moved2.ip, sizeof(moved2.ip), "192.168.1.98");
	snprintf(moved2.host, sizeof(moved2.host), "HIFI-PLUS");
	hifi_peers_seen(&t, &moved2, S(12));
	CHECK(t.n == 4);

	/* not heard from in 90 s: gone */
	hifi_peers_expire(&t, S(2) + HIFI_PEERS_GONE_US);
	CHECK(t.n == 4);
	hifi_peers_expire(&t, S(5) + HIFI_PEERS_GONE_US + 1);
	CHECK(t.n == 2); /* hifi-ultra (5 s) and self's own row (3 s) went */
	hifi_peers_json(&t, &self, false, S(100), out, sizeof(out));
	CHECK(!strstr(out, "hifi-ultra"));

	/* a full table gives up the row heard from longest ago */
	hifi_peers_t f = {0};
	for (int i = 0; i < HIFI_PEERS_MAX; i++) {
		char ip[16], host[64];
		snprintf(ip, sizeof(ip), "10.0.0.%u", (unsigned char) (i + 1));
		snprintf(host, sizeof(host), "box-%u", (unsigned char) i);
		hifi_peer_t p = peer(ip, host, host);
		hifi_peers_seen(&f, &p, S(100 + i));
	}
	CHECK(f.n == HIFI_PEERS_MAX);
	hifi_peer_t extra = peer("10.0.0.200", "box-new", "New");
	hifi_peers_seen(&f, &extra, S(200));
	CHECK(f.n == HIFI_PEERS_MAX);
	hifi_peers_json(&f, &self, false, S(200), out, sizeof(out));
	CHECK(strstr(out, "box-new") && !strstr(out, "\"box-0\"") && strstr(out, "\"box-1\""));

	/* the longest reply fits (HIFI_PEERS_REPLY): self and 12 rows, every
	 * field full, each byte one that escapes; control characters arrive as
	 * spaces, so none escapes to six bytes */
	hifi_peers_t big = {0};
	for (int i = 0; i < HIFI_PEERS_MAX; i++) {
		hifi_peer_t p = {0};
		snprintf(p.ip, sizeof(p.ip), "192.168.100.%u", (unsigned char) (100 + i));
		memset(p.host, '"', sizeof(p.host) - 1);
		p.host[0] = 'a' + i;
		memset(p.name, '\\', sizeof(p.name) - 1);
		memset(p.model, '"', sizeof(p.model) - 1);
		memset(p.fw, '"', sizeof(p.fw) - 1);
		p.name[3] = 0x1f;
		hifi_peers_seen(&big, &p, S(1));
	}
	CHECK(big.n == HIFI_PEERS_MAX);
	CHECK(big.p[0].name[3] == ' ');
	hifi_peer_t bigself = big.p[0];
	snprintf(bigself.ip, sizeof(bigself.ip), "192.168.100.250");
	bigself.host[0] = 'z';
	memset(bigself.fw, '"', sizeof(bigself.fw) - 1);
	char reply[HIFI_PEERS_REPLY];
	CHECK(hifi_peers_json(&big, &bigself, true, S(1), reply, sizeof(reply)) == 200);
	size_t len = strlen(reply);
	CHECK(len > 4000 && len < sizeof(reply) - 1);
	CHECK(!strstr(reply, "\\u00"));
	CHECK(len > 5 && !strcmp(reply + len - 5, "true}"));
	/* too small a buffer: an error, never a cut-off list with a 200 */
	CHECK(hifi_peers_json(&big, &bigself, true, S(1), reply, 1000) == 500);
	CHECK(strstr(reply, "\"error\"") == reply + 1);

	/* a row for a public address or another subnet is not a speaker the
	 * app may open: a LAN device can answer for _hifi._tcp with any IP */
	hifi_peers_t lan = {.net = 0xC0A8010Au, .mask = 0xFFFFFF00u}; /* 192.168.1.10/24 */
	hifi_peer_t far = peer("8.8.8.8", "hifi-lite-9", "Phish");
	hifi_peers_seen(&lan, &far, S(1));
	far = peer("192.168.2.7", "hifi-lite-9", "Phish");
	hifi_peers_seen(&lan, &far, S(1));
	far = peer("10.0.0.7", "hifi-lite-9", "Phish");
	hifi_peers_seen(&lan, &far, S(1));
	far = peer("192.168.1.300", "hifi-lite-9", "Phish");
	hifi_peers_seen(&lan, &far, S(1));
	CHECK(lan.n == 0);
	hifi_peer_t near = peer("192.168.1.77", "hifi-lite-3", "Hall");
	hifi_peers_seen(&lan, &near, S(1));
	CHECK(lan.n == 1);
	/* no subnet known yet: private addresses only */
	hifi_peers_t nonet = {0};
	far = peer("8.8.8.8", "hifi-lite-9", "Phish");
	hifi_peers_seen(&nonet, &far, S(1));
	CHECK(nonet.n == 0);

	/* the box that moved was the last row: drop() fills the hole with it */
	hifi_peers_t mv = {0};
	hifi_peer_t ma = peer("192.168.1.31", "hifi-lite-5", "A"), mb = peer("192.168.1.32", "hifi-lite-6", "B");
	hifi_peers_seen(&mv, &ma, S(1));
	hifi_peers_seen(&mv, &mb, S(1));
	hifi_peer_t mc = peer("192.168.1.32", "hifi-lite-5", "A moved");
	hifi_peers_seen(&mv, &mc, S(2));
	CHECK(mv.n == 1);
	CHECK_STR(mv.p[0].ip, "192.168.1.32");
	CHECK_STR(mv.p[0].name, "A moved");

	/* TXT values: control characters kept as spaces */
	hifi_peer_t cc = peer("192.168.1.40", "hifi-lite-7", "");
	hifi_peers_txt(&cc, "name", "Den\n\x01x", 6);
	CHECK_STR(cc.name, "Den  x");

	/* two boxes set to hifi-lite: mDNS renamed this one hifi-lite-3, and
	 * the one that kept hifi-lite is listed, not taken for self (Lite 2
	 * and 3, 2026-11-18) */
	hifi_peers_t two = {0};
	hifi_peer_t me2 = peer("192.168.2.220", "hifi-lite", "Lite 2");
	hifi_peer_t heard = peer("192.168.2.220", "hifi-lite-3", "Lite 2");
	hifi_peer_t kept = peer("192.168.2.218", "hifi-lite", "Lite 5");
	hifi_peers_seen(&two, &heard, S(1));
	hifi_peers_seen(&two, &kept, S(1));
	CHECK(hifi_peers_json(&two, &me2, false, S(2), out, sizeof(out)) == 200);
	CHECK(strstr(out, "{\"name\":\"Lite 2\",\"host\":\"hifi-lite-3\",\"ip\":\"192.168.2.220\"") != NULL);
	CHECK(strstr(out, "{\"name\":\"Lite 5\",\"host\":\"hifi-lite\",\"ip\":\"192.168.2.218\"") != NULL);
	CHECK(strstr(out, "\"self\":true") < strstr(out, "Lite 5"));
	/* before it has an address, its host name is all it has */
	hifi_peer_t noip = peer("", "hifi-lite", "Lite 2");
	hifi_peers_json(&two, &noip, false, S(2), out, sizeof(out));
	CHECK(strstr(out, "Lite 5") == NULL);

	/* no table, no self */
	CHECK(hifi_peers_json(NULL, NULL, false, 0, out, sizeof(out)) == 200);
	CHECK_STR(out, "{\"peers\":[],\"searching\":false}");
	return check_report("peers");
}
