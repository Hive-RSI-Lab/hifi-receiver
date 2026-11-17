/*
 * The Wi-Fi link's plain parts (docs/remote.md, "The bench endpoint over
 * wss://"): the board dials the Hive's endpoint through Cloudflare over a
 * WebSocket instead of an MQTT broker. Its URL, the headers of its dial and
 * the "t" tag on each message it sends are built here, tested on a PC; the
 * socket itself is esp/hifi_ws.c.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HIFI_WS_HOST_MAX 64
#define HIFI_WS_PATH_MAX 64
#define HIFI_WS_HEADERS_MAX 320

/* Splits wss://host[:port][/path]. Only wss:// (TLS) is taken; the host is
 * letters, digits, dots and dashes; the port defaults to 443; the path to
 * "/" and has no spaces or control characters. Returns 0, or -1 for a URL
 * the board won't dial. */
int hifi_ws_url_parse(const char *url, char host[HIFI_WS_HOST_MAX], int *port, char path[HIFI_WS_PATH_MAX]);

/* True for the board's own key from the Hive: exactly 64 hex digits. */
bool hifi_ws_key_ok(const char *key);

/*
 * The dial's extra headers, each "Name: value\r\n": the Cloudflare service
 * token (CF-Access-Client-Id/-Secret, left out when id is empty), then
 * X-Bench-Dev and X-Bench-Key. Returns the length written into out, or -1
 * when a value is empty where it is needed, too long, or holds a character
 * that could start another header (CR, LF or any other control character).
 */
int hifi_ws_headers(char *out, size_t cap, const char *id, const char *secret, const char *dev, const char *key);

/*
 * Tags a JSON object the board sends ("{...}", len bytes, NUL-terminated in
 * a buffer of cap bytes) with "t":"<t>" as its first member, in place, so
 * the endpoint can tell a status, an event and a reply apart on one socket.
 * Returns the new length, or 0 if it isn't an object or doesn't fit.
 */
size_t hifi_ws_tag(char *buf, size_t len, size_t cap, const char *t);

/*
 * The WebSocket framing itself (RFC 6455), done here rather than by IDF 4.4's
 * transport_ws: that one reads the server's 101 answer in one gulp and loses
 * whatever came in the same TLS record after it, so a command the Hive sends
 * the moment the board connects cut the stream and every frame after it was
 * misread.
 */
#define HIFI_WS_HEAD_MAX 14

typedef struct {
	int op;            /* 1 text, 2 binary, 8 close, 9 ping, 10 pong */
	bool fin, masked;
	uint8_t mask[4];
	uint64_t len;      /* the payload's */
} hifi_ws_head_t;

/* A client frame's header: FIN, op, masked with mask. Returns its length. */
size_t hifi_ws_head(uint8_t out[HIFI_WS_HEAD_MAX], int op, size_t len, const uint8_t mask[4]);
/* The whole header's length from its first two bytes (2 to 14). */
size_t hifi_ws_head_len(const uint8_t b[2]);
/* Reads a whole header (hifi_ws_head_len bytes). -1 if a reserved bit is
 * set or a control frame is longer than 125 bytes or split. */
int hifi_ws_head_parse(const uint8_t *b, hifi_ws_head_t *h);
/* The server's answer to the upgrade, NUL-terminated: true only for a 101
 * whose Sec-WebSocket-Accept (header name in any case) is accept. */
bool hifi_ws_upgrade_ok(const char *resp, const char *accept);

/*
 * Why an up link ended, kept by esp/hifi_ws.c for `api diag/net` and the
 * self-test (the bench saw drops it couldn't explain, 2026-11-17). The first
 * cause seen on a link wins: a failed read that follows the endpoint's close
 * frame is still "closed".
 */
typedef enum {
	HIFI_WS_DROP_NONE,          /* no drop since boot */
	HIFI_WS_DROP_UNKNOWN,       /* the link ended with no cause noted */
	HIFI_WS_DROP_WIFI,          /* the station lost its access point (the stack's abort of the socket, or no socket cause) */
	HIFI_WS_DROP_CLOSED,        /* the endpoint's close frame; code: its status code */
	HIFI_WS_DROP_EOF,           /* the far end closed the socket (FIN or TLS close_notify) */
	HIFI_WS_DROP_TCP_GAVE_UP,   /* ECONNABORTED with Wi-Fi up: keepalive or resends ran out */
	HIFI_WS_DROP_SOCKET,        /* another socket error; code: its errno */
	HIFI_WS_DROP_TLS,           /* a TLS error; code: mbedTLS's, positive */
	HIFI_WS_DROP_READ_TIMEOUT,  /* half a frame, then nothing for code seconds */
	HIFI_WS_DROP_WRITE_TIMEOUT, /* a send stalled code seconds: the socket's buffer stayed full */
	HIFI_WS_DROP_BAD_FRAME,     /* a frame header the board won't take */
	HIFI_WS_DROP_NO_MEMORY,
	HIFI_WS_DROP_STOPPED,       /* hifi_ws_stop */
} hifi_ws_drop_t;

/* Its short name for JSON ("socket", "tcp_gave_up", ...); NULL for NONE. */
const char *hifi_ws_drop_name(hifi_ws_drop_t kind);
/* One short line for a person: "socket errno 104 (reset)", "endpoint closed
 * it (1001)", with ", Wi-Fi down" when the station had no access point at
 * the drop. Returns the length written (snprintf's, cut to cap). */
int hifi_ws_drop_text(char *out, size_t cap, hifi_ws_drop_t kind, int code, bool wifi_down);
/* The self-test's link_drop line: "120s ago: <hifi_ws_drop_text>". Any
 * kind with a real code (an errno, a close status up to 4999, a timeout's
 * seconds) and up to 9999999 s fits a self-test value (63 characters). */
int hifi_ws_drop_line(char *out, size_t cap, uint32_t ago_s, hifi_ws_drop_t kind, int code, bool wifi_down);
