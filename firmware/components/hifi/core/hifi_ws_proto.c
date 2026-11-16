#include "hifi_ws_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int hifi_ws_url_parse(const char *url, char host[HIFI_WS_HOST_MAX], int *port, char path[HIFI_WS_PATH_MAX]) {
	if (!url || strncmp(url, "wss://", 6)) return -1;
	const char *h = url + 6, *p = h;
	while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '.' || *p == '-') p++;
	size_t hn = (size_t) (p - h);
	if (!hn || hn >= HIFI_WS_HOST_MAX || h[0] == '.' || h[0] == '-') return -1;
	memcpy(host, h, hn);
	host[hn] = '\0';
	*port = 443;
	if (*p == ':') {
		long n = 0;
		const char *d = ++p;
		while (*p >= '0' && *p <= '9' && p - d < 6) n = n * 10 + (*p++ - '0');
		if (p == d || n < 1 || n > 65535) return -1;
		*port = (int) n;
	}
	if (!*p) {
		strcpy(path, "/");
		return 0;
	}
	if (*p != '/' || strlen(p) >= HIFI_WS_PATH_MAX) return -1;
	for (const char *c = p; *c; c++)
		if ((unsigned char) *c <= ' ' || *c == 0x7f) return -1;
	strcpy(path, p);
	return 0;
}

bool hifi_ws_key_ok(const char *key) {
	if (!key || strlen(key) != 64) return false;
	for (const char *c = key; *c; c++)
		if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f') || (*c >= 'A' && *c <= 'F'))) return false;
	return true;
}

/* a header value: printable ASCII, 1..max characters */
static bool value_ok(const char *v, size_t max) {
	size_t n = v ? strlen(v) : 0;
	if (!n || n > max) return false;
	for (; *v; v++)
		if ((unsigned char) *v < 0x20 || (unsigned char) *v >= 0x7f) return false;
	return true;
}

int hifi_ws_headers(char *out, size_t cap, const char *id, const char *secret, const char *dev, const char *key) {
	if (!out || !cap) return -1;
	out[0] = '\0';
	bool token = id && id[0];
	if (token && !(value_ok(id, 80) && value_ok(secret, 96))) return -1;
	if (!value_ok(dev, 12) || !hifi_ws_key_ok(key)) return -1;
	int n = token ? snprintf(out, cap, "CF-Access-Client-Id: %s\r\nCF-Access-Client-Secret: %s\r\n", id, secret) : 0;
	if (n < 0 || (size_t) n >= cap) return out[0] = '\0', -1;
	int m = snprintf(out + n, cap - n, "X-Bench-Dev: %s\r\nX-Bench-Key: %s\r\n", dev, key);
	if (m < 0 || (size_t) (n + m) >= cap) return out[0] = '\0', -1;
	return n + m;
}

size_t hifi_ws_tag(char *buf, size_t len, size_t cap, const char *t) {
	if (!buf || len < 2 || buf[0] != '{' || !t || !value_ok(t, 16) || strpbrk(t, "\"\\")) return 0;
	char head[32];
	const char *rest = buf + 1;
	while (*rest == ' ') rest++;
	int hn = snprintf(head, sizeof(head), "\"t\":\"%s\"%s", t, *rest == '}' ? "" : ",");
	if (hn < 0 || len + (size_t) hn + 1 > cap) return 0;
	memmove(buf + 1 + hn, buf + 1, len); /* the rest and its NUL */
	memcpy(buf + 1, head, (size_t) hn);
	return len + (size_t) hn;
}
