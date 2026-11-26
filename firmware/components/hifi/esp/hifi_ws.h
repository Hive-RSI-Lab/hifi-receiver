/*
 * The board's WebSocket to the Hive's bench endpoint (docs/remote.md, "The
 * bench endpoint over wss://"): one task that dials wss:// with the dial's
 * headers (core/hifi_ws_proto.c), keeps the link with a ping every 20 s,
 * hands each text message to the relay and redials with a backoff when the
 * link drops. TLS is checked against the certificate bundle.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A whole text message from the Hive, malloc'd and NUL-terminated: the
 * callee frees it. */
typedef void (*hifi_ws_msg_cb_t)(char *data, size_t len);
/* Each time the link comes up (the board sends its status then). */
typedef void (*hifi_ws_up_cb_t)(void);

/* A whole binary message (the printer relay's [sid][data], hifi_tcp.c), on
 * the link's task with no lock held; the buffer is freed after it returns. */
typedef void (*hifi_ws_bin_cb_t)(const uint8_t *data, size_t len);
#define HIFI_WS_BIN_MAX 8192 /* a bigger binary frame is read and dropped */

/* Starts the link; url and headers are copied. msg_max is the biggest
 * message taken (a bigger one is read and dropped). on_bin may be NULL:
 * binary frames are dropped then. False if it can't start. */
bool hifi_ws_start(const char *url, const char *headers, size_t msg_max, hifi_ws_msg_cb_t on_msg, hifi_ws_up_cb_t on_up,
		   hifi_ws_bin_cb_t on_bin);
/* Closes the link and waits for its task to end. */
void hifi_ws_stop(void);
/* True while the link is up. */
bool hifi_ws_up(void);
/* Sends one text message; false if the link is down or the send failed. */
bool hifi_ws_send(const char *text, size_t len);
/* Sends one binary message; the same. */
bool hifi_ws_send_bin(const uint8_t *data, size_t len);
/* The link's generation: it changes each time the link comes up or drops,
 * so a stream opened on one link is known not to belong to the next. */
uint32_t hifi_ws_gen(void);
/* hifi_ws_send_bin, only while the link is still generation g (0: any). */
bool hifi_ws_send_bin_on(uint32_t g, const uint8_t *data, size_t len);

/* The link's counters since boot, for `api diag/net`: bytes on the TLS
 * socket (frames and their headers; not the TLS overhead), the times an up
 * link dropped, and when it came up (esp_timer µs; 0: not up). The last
 * drop: why (a hifi_ws_drop_t, core/hifi_ws_proto.h; 0: none since boot)
 * and its code, whether the station had lost its access point then, when
 * (esp_timer µs) and how long that link had been up. */
typedef struct {
	uint64_t tx_bytes, rx_bytes;
	uint32_t drops;
	int64_t up_since_us, first_up_us;
	uint8_t last_drop;
	bool last_drop_wifi_down;
	int32_t last_drop_code;
	int64_t last_drop_us;
	uint32_t last_drop_lasted_s;
} hifi_ws_stats_t;
void hifi_ws_stats(hifi_ws_stats_t *out);
