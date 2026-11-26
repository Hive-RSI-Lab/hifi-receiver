#include <stdio.h>
#include <string.h>

#include "hifi_json.h"
#include "hifi_tcp.h"

bool hifi_tcp_parse_ip(const char *s, uint32_t *out) {
	uint32_t ip = 0;
	if (!s) return false;
	for (int part = 0; part < 4; part++) {
		if (part && *s++ != '.') return false;
		if (*s < '0' || *s > '9') return false;
		if (*s == '0' && s[1] >= '0' && s[1] <= '9') return false; /* no leading zero: 010 isn't 8 or 10 */
		unsigned v = 0;
		int digits = 0;
		while (*s >= '0' && *s <= '9') {
			v = v * 10 + (unsigned) (*s++ - '0');
			if (++digits > 3 || v > 255) return false;
		}
		ip = ip << 8 | v;
	}
	if (*s) return false;
	if (ip == 0 || ip == 0xffffffffu || (ip >> 28) == 0xe) return false;
	*out = ip;
	return true;
}

bool hifi_tcp_port_ok(long port) { return port == 8883 || port == 990 || (port >= 1024 && port <= 65535); }

static int hexval(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

bool hifi_tcp_parse_hex32(const char *s, uint8_t out[32]) {
	if (!s || strlen(s) != 64) return false;
	for (int i = 0; i < 32; i++) {
		int hi = hexval(s[2 * i]), lo = hexval(s[2 * i + 1]);
		if (hi < 0 || lo < 0) return false;
		out[i] = (uint8_t) (hi << 4 | lo);
	}
	return true;
}

void hifi_tcp_hex32(const uint8_t in[32], char out[65]) {
	for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", in[i]);
}

static int bad(char *err, size_t n, int st, const char *why) {
	snprintf(err, n, "%s", why);
	return st;
}

/* an integer field in lo..hi; false if missing, fractional or out of range */
static bool get_int(const char *body, const char *key, long lo, long hi, long *out) {
	double d;
	if (!body || !hj_get_num(body, key, &d) || d < (double) lo || d > (double) hi || d != (double) (long) d) return false;
	*out = (long) d;
	return true;
}

int hifi_tcp_cmd_parse(const char *method, const char *path, const char *body, hifi_tcp_cmd_t *cmd, char *err, size_t n) {
	memset(cmd, 0, sizeof(*cmd));
	err[0] = '\0';
	bool post = method && !strcmp(method, "POST"), get = method && !strcmp(method, "GET");
	if (!path) return bad(err, n, 404, "no such relay command");
	if (body && !hj_valid_object(body)) return bad(err, n, 400, "the body isn't a JSON object");
	if (!strcmp(path, "/relay/state")) {
		if (!get) return bad(err, n, 405, "GET only");
		cmd->op = HIFI_TCP_STATE;
		return 0;
	}
	if (strcmp(path, "/relay/enable") && strcmp(path, "/relay/disable") && strcmp(path, "/relay/open") &&
	    strcmp(path, "/relay/close"))
		return bad(err, n, 404, "no such relay command");
	if (!post) return bad(err, n, 405, "POST only");
	if (!strcmp(path, "/relay/disable")) {
		cmd->op = HIFI_TCP_DISABLE;
		return 0;
	}
	if (!body) return bad(err, n, 400, "a body is needed");
	if (!strcmp(path, "/relay/enable")) {
		char ip[20] = "", fp[72] = "";
		if (hj_get_str_fit(body, "ip", ip, sizeof(ip)) != HJ_STR_OK || !hifi_tcp_parse_ip(ip, &cmd->ip))
			return bad(err, n, 400, "ip must be a dotted IPv4 address");
		if (hj_get_str_fit(body, "sha256", fp, sizeof(fp)) != HJ_STR_OK || !hifi_tcp_parse_hex32(fp, cmd->sha256))
			return bad(err, n, 400, "sha256 must be 64 hex digits");
		cmd->op = HIFI_TCP_ENABLE;
		return 0;
	}
	long sid, port;
	if (!strcmp(path, "/relay/open")) {
		if (!get_int(body, "sid", 1, 255, &sid)) return bad(err, n, 400, "sid must be 1-255");
		if (!get_int(body, "port", 1, 65535, &port) || !hifi_tcp_port_ok(port))
			return bad(err, n, 400, "port must be 8883, 990 or 1024-65535");
		cmd->op = HIFI_TCP_OPEN;
		cmd->sid = (int) sid;
		cmd->port = (int) port;
		return 0;
	}
	if (!get_int(body, "sid", 0, 255, &sid)) return bad(err, n, 400, "sid must be 0 (all) or 1-255");
	cmd->op = HIFI_TCP_CLOSE;
	cmd->sid = (int) sid;
	return 0;
}

size_t hifi_tcp_ack(uint8_t out[6], int sid, uint32_t total) {
	out[0] = 0;
	out[1] = (uint8_t) sid;
	for (int i = 0; i < 4; i++) out[2 + i] = (uint8_t) (total >> (24 - 8 * i));
	return 6;
}

size_t hifi_tcp_frame(uint8_t *out, int sid, const uint8_t *data, size_t len) {
	out[0] = (uint8_t) sid;
	if (len) memcpy(out + 1, data, len);
	return len + 1;
}
