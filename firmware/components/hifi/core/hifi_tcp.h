/*
 * The printer relay's rules (docs/remote.md, "The TCP relay"), the plain-C
 * part: which address and ports a stream may open, and the signed
 * commands' fields. The sockets and the certificate check are the board's
 * (esp/hifi_tcp.c). Only the tower opens streams: every one starts with a
 * signed /relay/open, and a frame from the board can't start anything.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HIFI_TCP_STREAMS 4     /* MQTT, FTPS control and data, one spare */
#define HIFI_TCP_CHUNK 4096    /* the most a binary frame carries, past its sid byte */
#define HIFI_TCP_RECHECK_S 60  /* an open re-checks the certificate older than this */
#define HIFI_TCP_IDLE_S 120    /* a stream with no traffic this long is closed */

/* A dotted IPv4 address, four decimal parts 0-255 with no leading zeros,
 * signs or spaces, into host order. No names: a DNS answer can't move it.
 * 0.0.0.0, 255.255.255.255 and the multicast range are refused too. */
bool hifi_tcp_parse_ip(const char *s, uint32_t *out);
/* The ports a stream may open: 8883 (MQTT over TLS), 990 (implicit FTPS)
 * and 1024-65535 (an FTPS passive data port). */
bool hifi_tcp_port_ok(long port);
/* 64 hex digits into 32 bytes (either case); false on anything else. */
bool hifi_tcp_parse_hex32(const char *s, uint8_t out[32]);
void hifi_tcp_hex32(const uint8_t in[32], char out[65]);

typedef enum {
	HIFI_TCP_ENABLE = 1,
	HIFI_TCP_DISABLE,
	HIFI_TCP_OPEN,
	HIFI_TCP_CLOSE,
	HIFI_TCP_STATE,
} hifi_tcp_op_t;

typedef struct {
	hifi_tcp_op_t op;
	uint32_t ip;        /* enable */
	uint8_t sha256[32]; /* enable: the printer certificate's (DER) SHA-256 */
	int sid;            /* open, close: 1-255; close 0 is every stream */
	int port;           /* open */
} hifi_tcp_cmd_t;

/* A /relay/ command (the remote channel only: the LAN's API never sees
 * one) into cmd. 0, or an HTTP status with err set: 404 for an unknown
 * path, 405 for the wrong method, 400 for a bad field. */
int hifi_tcp_cmd_parse(const char *method, const char *path, const char *body, hifi_tcp_cmd_t *cmd, char *err, size_t n);

/* A board-to-tower frame: [sid][data]; no data is the stream's end. */
/* the board's acknowledgement: [0][sid][the bytes written to that stream's printer socket so far, 4 bytes
 * big-endian, wrapping]. sid 0 is never a stream, so an endpoint that doesn't know it drops it. The tower
 * keeps at most HIFI_TCP_WINDOW bytes in flight past it, so nothing piles up in the link's buffers ahead of
 * a signed command. */
#define HIFI_TCP_WINDOW 65536
size_t hifi_tcp_ack(uint8_t out[6], int sid, uint32_t total);
size_t hifi_tcp_frame(uint8_t *out, int sid, const uint8_t *data, size_t len);
