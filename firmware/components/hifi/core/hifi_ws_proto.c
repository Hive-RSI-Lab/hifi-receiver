#include "hifi_ws_proto.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
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

size_t hifi_ws_head(uint8_t out[HIFI_WS_HEAD_MAX], int op, size_t len, const uint8_t mask[4]) {
	size_t n = 0;
	out[n++] = (uint8_t) (0x80 | (op & 0x0f));
	if (len < 126) {
		out[n++] = (uint8_t) (0x80 | len);
	} else if (len < 65536) {
		out[n++] = 0x80 | 126;
		out[n++] = (uint8_t) (len >> 8);
		out[n++] = (uint8_t) len;
	} else {
		out[n++] = 0x80 | 127;
		for (int i = 7; i >= 0; i--) out[n++] = (uint8_t) ((uint64_t) len >> (8 * i));
	}
	memcpy(out + n, mask, 4);
	return n + 4;
}

size_t hifi_ws_head_len(const uint8_t b[2]) {
	size_t n = 2, l = b[1] & 0x7f;
	if (l == 126) n += 2;
	else if (l == 127) n += 8;
	return n + ((b[1] & 0x80) ? 4 : 0);
}

int hifi_ws_head_parse(const uint8_t *b, hifi_ws_head_t *h) {
	if (b[0] & 0x70) return -1;
	h->fin = b[0] & 0x80;
	h->op = b[0] & 0x0f;
	h->masked = b[1] & 0x80;
	uint64_t l = b[1] & 0x7f;
	size_t n = 2;
	if (l == 126) {
		l = (uint64_t) b[2] << 8 | b[3];
		n = 4;
	} else if (l == 127) {
		l = 0;
		for (int i = 0; i < 8; i++) l = l << 8 | b[2 + i];
		n = 10;
	}
	h->len = l;
	if (h->masked) memcpy(h->mask, b + n, 4);
	else memset(h->mask, 0, 4);
	if (h->op >= 8 && (l > 125 || !h->fin)) return -1;
	return 0;
}

bool hifi_ws_upgrade_ok(const char *resp, const char *accept) {
	if (!resp || !accept || !accept[0]) return false;
	if (strncmp(resp, "HTTP/1.1 101", 12) || (resp[12] != ' ' && resp[12] != '\r')) return false;
	static const char name[] = "Sec-WebSocket-Accept:";
	for (const char *l = strstr(resp, "\r\n"); l; l = strstr(l + 2, "\r\n")) {
		const char *v = l + 2;
		if (strncasecmp(v, name, sizeof(name) - 1)) continue;
		v += sizeof(name) - 1;
		while (*v == ' ' || *v == '\t') v++;
		size_t a = strlen(accept);
		if (strncmp(v, accept, a)) return false;
		v += a;
		while (*v == ' ' || *v == '\t') v++;
		return v[0] == '\r' && v[1] == '\n';
	}
	return false;
}

const char *hifi_ws_drop_name(hifi_ws_drop_t kind) {
	static const char *const names[] = {NULL,          "unknown",       "wifi_down",     "closed",
					    "eof",         "tcp_gave_up",   "socket",        "tls",
					    "read_timeout", "write_timeout", "bad_frame",    "no_memory",
					    "stopped"};
	return (unsigned) kind < sizeof(names) / sizeof(names[0]) ? names[kind] : "unknown";
}

static const char *errno_word(int e) {
	switch (e) {
	case ECONNRESET: return "reset";
	case ECONNABORTED: return "aborted";
	case ETIMEDOUT: return "timed out";
	case EHOSTUNREACH: return "no route";
	case ENETUNREACH: return "net unreachable";
	case ENOTCONN: return "not connected";
	case EPIPE: return "broken pipe";
	case ENOMEM: return "no memory";
	default: return NULL;
	}
}

int hifi_ws_drop_text(char *out, size_t cap, hifi_ws_drop_t kind, int code, bool wifi_down) {
	char why[48];
	const char *w;
	switch (kind) {
	case HIFI_WS_DROP_NONE: snprintf(why, sizeof(why), "no drop"); break;
	case HIFI_WS_DROP_WIFI: snprintf(why, sizeof(why), "Wi-Fi down"); break;
	case HIFI_WS_DROP_CLOSED: snprintf(why, sizeof(why), "endpoint closed it (%d)", code); break;
	case HIFI_WS_DROP_EOF: snprintf(why, sizeof(why), "far end closed the socket"); break;
	case HIFI_WS_DROP_TCP_GAVE_UP: snprintf(why, sizeof(why), "TCP gave up: keepalive or resends"); break;
	case HIFI_WS_DROP_SOCKET:
		w = errno_word(code);
		if (w) snprintf(why, sizeof(why), "socket errno %d (%s)", code, w);
		else snprintf(why, sizeof(why), "socket errno %d", code);
		break;
	case HIFI_WS_DROP_TLS: snprintf(why, sizeof(why), "TLS error -0x%04X", (unsigned) code & 0xffffu); break;
	case HIFI_WS_DROP_READ_TIMEOUT: snprintf(why, sizeof(why), "read stalled %d s mid-frame", code); break;
	case HIFI_WS_DROP_WRITE_TIMEOUT: snprintf(why, sizeof(why), "send stalled %d s", code); break;
	case HIFI_WS_DROP_BAD_FRAME: snprintf(why, sizeof(why), "bad frame"); break;
	case HIFI_WS_DROP_NO_MEMORY: snprintf(why, sizeof(why), "no memory"); break;
	case HIFI_WS_DROP_STOPPED: snprintf(why, sizeof(why), "stopped"); break;
	default: snprintf(why, sizeof(why), "unknown"); break;
	}
	return snprintf(out, cap, "%s%s", why, wifi_down && kind != HIFI_WS_DROP_WIFI ? ", Wi-Fi down" : "");
}

int hifi_ws_drop_line(char *out, size_t cap, uint32_t ago_s, hifi_ws_drop_t kind, int code, bool wifi_down) {
	char why[64];
	hifi_ws_drop_text(why, sizeof(why), kind, code, wifi_down);
	return snprintf(out, cap, "%us ago: %s", (unsigned) ago_s, why);
}
