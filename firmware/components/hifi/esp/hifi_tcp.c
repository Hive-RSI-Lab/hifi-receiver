/*
 * The printer relay (docs/remote.md, "The TCP relay"): the tower reaches a
 * LAN printer through the board's bench link. The rules are in
 * core/hifi_tcp.c; this file holds the sockets.
 *
 * Only the tower opens a stream, with a signed /relay/open; a binary frame
 * for a stream that isn't open is dropped, and nothing the board reads
 * from a socket can open anything. The board dials one address only, the
 * one a signed /relay/enable gave it, while it shows the certificate
 * recorded then (its DER's SHA-256, checked on enable, again on an open
 * when the last check is over a minute old, and every 10 minutes while a
 * stream stays open). A wrong certificate turns the relay off until a new
 * enable. Ports: 8883, 990, and one stream at a time on 1024 and up, only
 * while an FTPS (990) stream is open, for its passive data. Each stream
 * belongs to the link it was opened on and ends with it.
 *
 * One task, its stack in PSRAM, owns the sockets: it serves the worker's
 * requests and reads the streams. The link's task writes the tower's bytes
 * into a stream under `smux`. NVS is written by the worker (internal
 * stack), never here.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/sha256.h"
#include "mbedtls/ssl.h"

#include "platform_config.h"

#include "hifi_tcp.h"
#include "hifi_tcp_esp.h"
#include "hifi_ws.h"

static const char TAG[] = "hifi_tcp";

#define STACK 8192          /* a TLS handshake, as the link's */
#define DIAL_MS 5000
#define SEND_MS 5000
#define CHECK_PORT 8883
#define FRAME_MS 1000      /* the most one frame from the tower may wait on a printer socket */
#define RECHECK_OPEN_S 600 /* the certificate is checked again this often while a stream is open */

typedef struct {
	int sid, fd, port;
	uint32_t gen; /* the link it was opened on (hifi_ws_gen) */
	int64_t last_us;
	bool dead; /* a write to it failed: the task closes it */
	uint32_t wrote, acked; /* bytes written to the printer socket, and the most the tower has been told */
} stream_t;

typedef struct {
	hifi_tcp_cmd_t cmd;
	int status;
	bool cleared, stored; /* the worker clears or stores NVS after */
	char body[200];
} req_t;

static stream_t streams[HIFI_TCP_STREAMS];
static SemaphoreHandle_t smux, req_done;
static QueueHandle_t reqs;
static TaskHandle_t task;
static uint8_t *rbuf;

/* the enabled printer: only the task changes it (the worker reads it for NVS) */
static uint32_t en_ip;
static uint8_t en_fp[32];
static int64_t checked_us; /* 0: not checked since it was set */
static char mismatch[65];   /* a mismatch the task found by itself: the next open reports it */

static int64_t now_us(void) { return esp_timer_get_time(); }

static void ip_text(uint32_t ip, char out[16]) {
	snprintf(out, 16, "%u.%u.%u.%u", (unsigned) (ip >> 24), (unsigned) (ip >> 16 & 255), (unsigned) (ip >> 8 & 255),
		 (unsigned) (ip & 255));
}

/* ------------------------------------------------------------ sockets */

/* a TCP connection to the enabled address only; -1 if it can't be made in DIAL_MS */
static int dial(uint32_t ip, int port) {
	if (!ip || !hifi_tcp_port_ok(port)) return -1;
	int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (fd < 0) return -1;
	struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t) port), .sin_addr.s_addr = htonl(ip)};
	int fl = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, fl | O_NONBLOCK);
	int r = connect(fd, (struct sockaddr *) &a, sizeof(a));
	if (r < 0 && errno == EINPROGRESS) {
		fd_set w;
		FD_ZERO(&w);
		FD_SET(fd, &w);
		struct timeval tv = {.tv_sec = DIAL_MS / 1000};
		int e = 0;
		socklen_t el = sizeof(e);
		r = select(fd + 1, NULL, &w, NULL, &tv) == 1 && !getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &el) && !e ? 0 : -1;
	}
	if (r < 0) {
		close(fd);
		return -1;
	}
	fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
	struct timeval st = {.tv_sec = SEND_MS / 1000};
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &st, sizeof(st));
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &st, sizeof(st));
	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	return fd;
}

static int rng(void *p, unsigned char *b, size_t n) {
	esp_fill_random(b, n);
	return 0;
}

/* The certificate ip:8883 shows, its DER's SHA-256 into out. The chain
 * isn't checked: the printer's own certificate is pinned instead. 0, or -1
 * when there was no handshake (the printer is off or away). */
static int cert_of(uint32_t ip, uint8_t out[32]) {
	int fd = dial(ip, CHECK_PORT);
	if (fd < 0) return -1;
	mbedtls_ssl_context ssl;
	mbedtls_ssl_config conf;
	mbedtls_ssl_init(&ssl);
	mbedtls_ssl_config_init(&conf);
	mbedtls_net_context net = {.fd = fd};
	int r = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
	if (!r) {
		mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
		mbedtls_ssl_conf_rng(&conf, rng, NULL);
		r = mbedtls_ssl_setup(&ssl, &conf);
	}
	if (!r) {
		mbedtls_ssl_set_bio(&ssl, &net, mbedtls_net_send, mbedtls_net_recv, NULL);
		int64_t end = now_us() + 10 * 1000000LL;
		while ((r = mbedtls_ssl_handshake(&ssl)) == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
			if (now_us() > end) break;
	}
	const mbedtls_x509_crt *c = r ? NULL : mbedtls_ssl_get_peer_cert(&ssl);
	int ok = c && c->raw.p && !mbedtls_sha256_ret(c->raw.p, c->raw.len, out, 0) ? 0 : -1;
	if (r) ESP_LOGW(TAG, "no TLS handshake with the printer: -0x%04x", (unsigned) -r);
	if (!r) mbedtls_ssl_close_notify(&ssl);
	mbedtls_ssl_free(&ssl);
	mbedtls_ssl_config_free(&conf);
	close(fd);
	return ok;
}

static stream_t *find(int sid) {
	for (int i = 0; i < HIFI_TCP_STREAMS; i++)
		if (streams[i].fd >= 0 && streams[i].sid == sid) return &streams[i];
	return NULL;
}

static bool is_data(int port) { return port != 8883 && port != 990; }

static int count_port(bool data, int port) {
	int n = 0;
	for (int i = 0; i < HIFI_TCP_STREAMS; i++)
		n += streams[i].fd >= 0 && (data ? is_data(streams[i].port) : streams[i].port == port);
	return n;
}

static int open_count(void) {
	int n = 0;
	for (int i = 0; i < HIFI_TCP_STREAMS; i++) n += streams[i].fd >= 0;
	return n;
}

/* closes one stream; eof: tell the tower it ended */
static void close_stream(stream_t *s, bool eof) {
	xSemaphoreTake(smux, portMAX_DELAY);
	int fd = s->fd, sid = s->sid;
	uint32_t g = s->gen;
	s->fd = -1;
	s->dead = false;
	xSemaphoreGive(smux);
	if (fd < 0) return;
	close(fd);
	if (eof) {
		uint8_t f[1];
		hifi_tcp_frame(f, sid, NULL, 0);
		hifi_ws_send_bin_on(g, f, 1);
	}
	ESP_LOGI(TAG, "stream %d closed", sid);
}

static void close_all(void) {
	for (int i = 0; i < HIFI_TCP_STREAMS; i++)
		if (streams[i].fd >= 0) close_stream(&streams[i], false);
}

static void disable(void) {
	close_all();
	en_ip = 0;
	memset(en_fp, 0, sizeof(en_fp));
	checked_us = 0;
}

/* ------------------------------------------------------------ requests */

static void reply_err(req_t *q, int st, const char *why) {
	q->status = st;
	snprintf(q->body, sizeof(q->body), "{\"error\":\"%s\"}", why);
}

/* the certificate check, on enable and on a stale open: 0 ok, else the reply is set */
static int check(req_t *q, const uint8_t want[32]) {
	uint8_t got[32];
	if (cert_of(en_ip, got)) {
		reply_err(q, 502, "the printer didn't answer on 8883");
		return -1;
	}
	if (memcmp(got, want, 32)) {
		char hex[65];
		hifi_tcp_hex32(got, hex);
		ESP_LOGE(TAG, "the printer's certificate changed (%s): the relay is off", hex);
		disable();
		q->cleared = true;
		q->status = 409;
		snprintf(q->body, sizeof(q->body), "{\"error\":\"certificate mismatch: the relay is off until a new enable\",\"seen\":\"%s\"}",
			 hex);
		return -1;
	}
	checked_us = now_us();
	return 0;
}

static void serve(req_t *q) {
	hifi_tcp_cmd_t *c = &q->cmd;
	q->status = 200;
	snprintf(q->body, sizeof(q->body), "{\"ok\":true}");
	switch (c->op) {
	case HIFI_TCP_ENABLE:
		disable(); /* a new enable starts clean, and a failed one leaves it off */
		mismatch[0] = '\0';
		q->cleared = true;
		en_ip = c->ip;
		if (check(q, c->sha256)) {
			disable();
			return;
		}
		memcpy(en_fp, c->sha256, 32);
		q->cleared = false;
		q->stored = true;
		break;
	case HIFI_TCP_DISABLE:
		disable();
		mismatch[0] = '\0';
		q->cleared = true;
		break;
	case HIFI_TCP_OPEN: {
		if (mismatch[0]) { /* found by the task's own check: the worker clears NVS now */
			q->cleared = true;
			q->status = 409;
			snprintf(q->body, sizeof(q->body), "{\"error\":\"certificate mismatch: the relay is off until a new enable\",\"seen\":\"%s\"}",
				 mismatch);
			return;
		}
		if (!en_ip) return reply_err(q, 409, "the relay is off: no signed enable");
		if (!hifi_ws_up()) return reply_err(q, 503, "the link is down");
		if (find(c->sid)) return reply_err(q, 409, "that sid is open");
		/* FTPS passive data: one data stream at a time, only beside an FTPS control stream */
		if (is_data(c->port) && !count_port(false, 990)) return reply_err(q, 403, "a data port only beside an FTPS stream");
		if (is_data(c->port) && count_port(true, 0)) return reply_err(q, 409, "a data stream is already open");
		stream_t *s = NULL;
		for (int i = 0; i < HIFI_TCP_STREAMS && !s; i++)
			if (streams[i].fd < 0) s = &streams[i];
		if (!s) return reply_err(q, 503, "every stream is in use");
		if (!checked_us || now_us() - checked_us > HIFI_TCP_RECHECK_S * 1000000LL) {
			uint8_t want[32];
			memcpy(want, en_fp, 32);
			if (check(q, want)) return;
		}
		int fd = dial(en_ip, c->port);
		if (fd < 0) return reply_err(q, 502, "the printer didn't take the connection");
		xSemaphoreTake(smux, portMAX_DELAY);
		s->sid = c->sid;
		s->port = c->port;
		s->gen = hifi_ws_gen();
		s->last_us = now_us();
		s->dead = false;
		s->wrote = s->acked = 0;
		s->fd = fd;
		xSemaphoreGive(smux);
		snprintf(q->body, sizeof(q->body), "{\"ok\":true,\"ack\":true}"); /* this board acknowledges (hifi_tcp_ack) */
		ESP_LOGI(TAG, "stream %d open to port %d", c->sid, c->port);
		break;
	}
	case HIFI_TCP_CLOSE:
		if (!c->sid) close_all();
		else if (find(c->sid)) close_stream(find(c->sid), false);
		break;
	case HIFI_TCP_STATE: {
		char ip[16] = "", fp[65] = "";
		if (en_ip) {
			ip_text(en_ip, ip);
			hifi_tcp_hex32(en_fp, fp);
		}
		int n = snprintf(q->body, sizeof(q->body), "{\"enabled\":%s,\"ip\":\"%s\",\"sha256\":\"%s\",\"streams\":%d}",
				 en_ip ? "true" : "false", ip, fp, open_count());
		(void) n;
		break;
	}
	}
}

/* ------------------------------------------------------------ the task */

/* tell the tower what each stream has written to the printer, so it sends more */
static void acks(void) {
	for (int i = 0; i < HIFI_TCP_STREAMS; i++) {
		stream_t *s = &streams[i];
		xSemaphoreTake(smux, portMAX_DELAY);
		bool due = s->fd >= 0 && s->wrote != s->acked;
		uint32_t total = s->wrote, g = s->gen;
		int sid = s->sid;
		if (due) s->acked = total;
		xSemaphoreGive(smux);
		if (!due) continue;
		uint8_t f[6];
		hifi_tcp_ack(f, sid, total);
		hifi_ws_send_bin_on(g, f, sizeof(f)); /* a lost link ends the stream in the task's own check */
	}
}

static void pump(void) {
	fd_set r;
	FD_ZERO(&r);
	int maxfd = -1;
	for (int i = 0; i < HIFI_TCP_STREAMS; i++) {
		stream_t *s = &streams[i];
		if (s->fd < 0) continue;
		/* FTPS control is silent while its data stream moves a file: that isn't idle */
		bool busy = s->port == 990 && count_port(true, 0);
		bool idle = !busy && now_us() - s->last_us > HIFI_TCP_IDLE_S * 1000000LL;
		bool orphan = is_data(s->port) && !count_port(false, 990); /* its control stream ended */
		if (s->dead || idle || orphan) {
			close_stream(s, true);
			continue;
		}
		FD_SET(s->fd, &r);
		if (s->fd > maxfd) maxfd = s->fd;
	}
	if (maxfd < 0) return;
	struct timeval tv = {.tv_usec = 50 * 1000};
	if (select(maxfd + 1, &r, NULL, NULL, &tv) <= 0) return;
	for (int i = 0; i < HIFI_TCP_STREAMS; i++) {
		stream_t *s = &streams[i];
		if (s->fd < 0 || !FD_ISSET(s->fd, &r)) continue;
		int n = recv(s->fd, rbuf + 1, HIFI_TCP_CHUNK, MSG_DONTWAIT);
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
		if (n <= 0) {
			close_stream(s, true); /* the printer closed it, or it broke */
			continue;
		}
		s->last_us = now_us();
		rbuf[0] = (uint8_t) s->sid;
		if (!hifi_ws_send_bin_on(s->gen, rbuf, (size_t) n + 1)) close_stream(s, false); /* its link is gone */
	}
}

static void tcp_task(void *arg) {
	for (;;) {
		req_t *q;
		bool any = open_count() > 0;
		if (xQueueReceive(reqs, &q, any ? 0 : pdMS_TO_TICKS(200)) == pdTRUE) {
			serve(q);
			xSemaphoreGive(req_done);
		}
		/* the link dropped, even if a new one is already up: every stream ends with
		 * the link it was opened on (the tower's side does too) */
		uint32_t g = hifi_ws_gen();
		bool stale = false;
		for (int i = 0; i < HIFI_TCP_STREAMS; i++) stale |= streams[i].fd >= 0 && (streams[i].gen != g || !hifi_ws_up());
		if (stale) {
			ESP_LOGW(TAG, "the link dropped: closing its streams");
			for (int i = 0; i < HIFI_TCP_STREAMS; i++)
				if (streams[i].fd >= 0 && (streams[i].gen != g || !hifi_ws_up())) close_stream(&streams[i], false);
		}
		/* a long session is checked again, between file transfers */
		if (en_ip && open_count() && !count_port(true, 0) && now_us() - checked_us > RECHECK_OPEN_S * 1000000LL) {
			uint8_t got[32];
			if (!cert_of(en_ip, got) && memcmp(got, en_fp, 32)) {
				hifi_tcp_hex32(got, mismatch);
				ESP_LOGE(TAG, "the printer's certificate changed (%s): the relay is off", mismatch);
				disable();
			} else checked_us = now_us(); /* no answer: the next open checks it */
		}
		pump();
		acks();
	}
}

/* ------------------------------------------------------------ the worker's side */

static void store(bool on, uint32_t ip, const uint8_t fp[32]) {
	char ipt[16] = "", hex[65] = "";
	if (on) {
		ip_text(ip, ipt);
		hifi_tcp_hex32(fp, hex);
	}
	config_set_value(NVS_TYPE_STR, "hifi_tcp_ip", ipt);
	config_set_value(NVS_TYPE_STR, "hifi_tcp_fp", hex);
}

int hifi_tcp_relay(const char *method, const char *path, const char *body, char *out, size_t cap) {
	static req_t q; /* the worker is one task: one request at a time */
	char err[80];
	int st = hifi_tcp_cmd_parse(method, path, body, &q.cmd, err, sizeof(err));
	if (st) {
		snprintf(out, cap, "{\"error\":\"%s\"}", err);
		return st;
	}
	if (!task) {
		snprintf(out, cap, "{\"error\":\"the relay didn't start (no memory)\"}");
		return 503;
	}
	q.cleared = q.stored = false;
	req_t *p = &q;
	xQueueSend(reqs, &p, portMAX_DELAY);
	xSemaphoreTake(req_done, portMAX_DELAY); /* at most a dial and a handshake */
	if (q.cleared) store(false, 0, NULL);
	if (q.stored) store(true, q.cmd.ip, q.cmd.sha256);
	snprintf(out, cap, "%s", q.body);
	return q.status;
}

void hifi_tcp_on_bin(const uint8_t *d, size_t len) {
	if (len < 2 || !smux) return; /* the tower ends a stream with /relay/close, not an empty frame */
	xSemaphoreTake(smux, portMAX_DELAY);
	stream_t *s = find(d[0]);
	if (s && !s->dead && s->gen == hifi_ws_gen()) {
		/* never blocks the link for long: a printer socket that takes less than
		 * a frame in FRAME_MS is dropped, so pings and signed commands go on */
		size_t put = 0;
		int64_t end = now_us() + FRAME_MS * 1000LL;
		while (put < len - 1) {
			int n = send(s->fd, d + 1 + put, len - 1 - put, MSG_DONTWAIT);
			if (n > 0) {
				put += (size_t) n;
				continue;
			}
			int64_t left = end - now_us();
			if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && left > 0) {
				fd_set w;
				FD_ZERO(&w);
				FD_SET(s->fd, &w);
				struct timeval tv = {.tv_sec = (time_t) (left / 1000000), .tv_usec = (suseconds_t) (left % 1000000)};
				select(s->fd + 1, NULL, &w, NULL, &tv);
				continue;
			}
			s->dead = true; /* the printer stalled, or the socket broke */
			break;
		}
		s->wrote += (uint32_t) put;
		s->last_us = now_us();
	}
	xSemaphoreGive(smux); /* an unknown sid is dropped: a frame never opens a stream */
}

void hifi_tcp_start(void) {
	for (int i = 0; i < HIFI_TCP_STREAMS; i++) streams[i].fd = -1;
	char *ip = config_alloc_get_str("hifi_tcp_ip", NULL, "");
	char *fp = config_alloc_get_str("hifi_tcp_fp", NULL, "");
	uint32_t a;
	uint8_t f[32];
	if (ip && fp && hifi_tcp_parse_ip(ip, &a) && hifi_tcp_parse_hex32(fp, f)) {
		en_ip = a;
		memcpy(en_fp, f, 32);
		ESP_LOGI(TAG, "the relay is on for %s (its certificate is checked at the first open)", ip);
	}
	free(ip);
	free(fp);
	smux = xSemaphoreCreateMutex();
	req_done = xSemaphoreCreateBinary();
	reqs = xQueueCreate(1, sizeof(req_t *));
	rbuf = heap_caps_malloc(HIFI_TCP_CHUNK + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if (!smux || !req_done || !reqs || !rbuf) return;
	StackType_t *stack = heap_caps_malloc(STACK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	StaticTask_t *tcb = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
	if (stack && tcb) task = xTaskCreateStatic(tcp_task, "hifi_tcp", STACK, NULL, tskIDLE_PRIORITY + 2, stack, tcb);
	if (!task) {
		free(stack);
		free(tcb);
		ESP_LOGE(TAG, "no memory for the relay's task");
	}
}
