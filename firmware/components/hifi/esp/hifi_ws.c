/*
 * The board's WebSocket to the Hive (hifi_ws.h). Built on tcp_transport
 * rather than esp_websocket_client: in IDF 4.4 that client can't take the
 * certificate bundle, and without a CA it doesn't check the server at all.
 * Here the SSL transport carries esp_crt_bundle_attach, so the Cloudflare
 * edge's certificate is checked like any https:// site's. The WebSocket on
 * top is our own (core/hifi_ws_proto.c), not transport_ws: that one reads
 * the upgrade answer in one gulp and drops whatever followed it in the same
 * TLS record, so a command sent the moment the board connected put every
 * later frame out of step.
 *
 * mbedTLS isn't safe from two tasks at once, so every read and write on the
 * socket holds `lock`; the task waits for data (poll) without it.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha1.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_transport.h"
#include "esp_transport_ssl.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "hifi_esp.h"
#include "hifi_ws.h"
#include "hifi_ws_proto.h"

static const char TAG[] = "hifi_ws";

#define PING_S 20       /* the endpoint drops a board it hasn't heard from in 90 s */
#define BACKOFF_MIN_S 5
#define BACKOFF_MAX_S 300
#define IO_MS 10000
#define STACK 6144     /* bytes: a TLS handshake with the certificate bundle */

static SemaphoreHandle_t lock;
static SemaphoreHandle_t done, go; /* go: the parked task starts a link */
static TaskHandle_t task;
static esp_transport_handle_t ssl;
static char url[128], *headers;
static size_t msg_max;
static hifi_ws_msg_cb_t on_msg;
static hifi_ws_up_cb_t on_up;
static volatile bool running, up, broken;
static int64_t last_tx;
static hifi_ws_stats_t stats; /* hifi_ws_stats(); 64-bit fields, so under stats_mux */
static portMUX_TYPE stats_mux = portMUX_INITIALIZER_UNLOCKED;

/* Why this link is ending: the first cause noted wins (a failed read after
 * the endpoint's close frame is still "closed"); reset under lock as the
 * link comes up. */
static hifi_ws_drop_t cause;
static int cause_code;
/* Whether this link rode the station (not Ethernet), and the station's
 * disconnect count when it came up: a count that moved means the Wi-Fi
 * went while the link was up, even if it is back by the drop. */
static bool on_sta;
static uint32_t sta_drops_at_up;

static int64_t now_s(void) { return esp_timer_get_time() / 1000000; }

static void note(hifi_ws_drop_t kind, int code) {
	if (cause != HIFI_WS_DROP_NONE) return;
	cause = kind;
	cause_code = code;
}

static void note_errno(int e) {
	if (e == ECONNABORTED) note(HIFI_WS_DROP_TCP_GAVE_UP, e); /* lwIP's ERR_ABRT: keepalive or resends ran out */
	else note(HIFI_WS_DROP_SOCKET, e);
}

/* A read or write that returned r <= 0, errno as it left it: 0 is the
 * transport's timeout; below 0, its error handle says what broke. */
static void note_io(int r, bool wr, int e) {
	if (r == 0) {
		note(wr ? HIFI_WS_DROP_WRITE_TIMEOUT : HIFI_WS_DROP_READ_TIMEOUT, IO_MS / 1000);
		return;
	}
	esp_tls_error_handle_t eh = esp_transport_get_error_handle(ssl);
	int sock = esp_transport_get_errno(ssl); /* a poll's error set */
	int tls = eh ? eh->esp_tls_error_code : 0; /* mbedTLS's, made positive */
	if (eh && eh->last_error == ESP_ERR_ESP_TLS_TCP_CLOSED_FIN) note(HIFI_WS_DROP_EOF, 0);
	else if (sock > 0) note_errno(sock);
	else if (!tls && (e == EAGAIN || e == EWOULDBLOCK)) /* SO_RCVTIMEO/SO_SNDTIMEO ran out mid-record */
		note(wr ? HIFI_WS_DROP_WRITE_TIMEOUT : HIFI_WS_DROP_READ_TIMEOUT, IO_MS / 1000);
	else if (tls == -MBEDTLS_ERR_SSL_CONN_EOF || tls == -MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) note(HIFI_WS_DROP_EOF, 0);
	else if (tls == -MBEDTLS_ERR_NET_CONN_RESET) note(HIFI_WS_DROP_SOCKET, ECONNRESET);
	else if ((tls == -MBEDTLS_ERR_NET_RECV_FAILED || tls == -MBEDTLS_ERR_NET_SEND_FAILED) && e > 0) note_errno(e);
	else if (tls) note(HIFI_WS_DROP_TLS, tls);
	else if (e > 0) note_errno(e);
	else note(HIFI_WS_DROP_UNKNOWN, 0);
}

static bool netif_has_ip(const char *key) {
	esp_netif_t *n = esp_netif_get_handle_from_ifkey(key);
	esp_netif_ip_info_t ip;
	return n && esp_netif_get_ip_info(n, &ip) == ESP_OK && ip.ip.addr != 0;
}

static void drop(void) {
	xSemaphoreTake(lock, portMAX_DELAY);
	if (up) {
		bool wifi_down = false;
		if (on_sta) {
			wifi_ap_record_t ap;
			wifi_down = hifi_diag_wifi_disconnects() != sta_drops_at_up || esp_wifi_sta_get_ap_info(&ap) != ESP_OK;
		}
		if (!running) note(HIFI_WS_DROP_STOPPED, 0);
		/* losing the station takes its address, and lwIP aborts every socket on
		 * it: that ECONNABORTED is the Wi-Fi, not keepalive or resends (and a
		 * blip the station rode out can still have run TCP's clock out) */
		if (wifi_down && cause == HIFI_WS_DROP_TCP_GAVE_UP) cause = HIFI_WS_DROP_WIFI;
		note(wifi_down ? HIFI_WS_DROP_WIFI : HIFI_WS_DROP_UNKNOWN, 0);
		int64_t t = esp_timer_get_time();
		portENTER_CRITICAL(&stats_mux);
		stats.drops++;
		stats.last_drop = (uint8_t) cause;
		stats.last_drop_code = cause_code;
		stats.last_drop_wifi_down = wifi_down;
		stats.last_drop_us = t;
		stats.last_drop_lasted_s = stats.up_since_us ? (uint32_t) ((t - stats.up_since_us) / 1000000) : 0;
		stats.up_since_us = 0;
		portEXIT_CRITICAL(&stats_mux);
		char why[64];
		hifi_ws_drop_text(why, sizeof(why), cause, cause_code, wifi_down);
		ESP_LOGW(TAG, "the link dropped: %s", why);
	}
	up = false;
	if (ssl) {
		esp_transport_close(ssl);
		esp_transport_destroy(ssl);
	}
	ssl = NULL;
	xSemaphoreGive(lock);
}

/* n bytes or nothing: a short read mid-frame would put the stream out of step */
static bool read_exact(void *b, size_t n) {
	size_t got = 0;
	while (got < n) {
		errno = 0;
		int r = esp_transport_read(ssl, (char *) b + got, (int) (n - got), IO_MS);
		if (r <= 0) {
			note_io(r, false, errno);
			return false;
		}
		got += (size_t) r;
		portENTER_CRITICAL(&stats_mux);
		stats.rx_bytes += (uint64_t) r;
		portEXIT_CRITICAL(&stats_mux);
	}
	return true;
}

static bool write_all(const void *b, size_t n) {
	size_t put = 0;
	while (put < n) {
		errno = 0;
		int r = esp_transport_write(ssl, (const char *) b + put, (int) (n - put), IO_MS);
		if (r <= 0) {
			note_io(r, true, errno);
			return false;
		}
		put += (size_t) r;
		portENTER_CRITICAL(&stats_mux);
		stats.tx_bytes += (uint64_t) r;
		portEXIT_CRITICAL(&stats_mux);
	}
	return true;
}

/* One masked frame, header and payload in one write (one TLS record). Holds no lock. */
static bool write_frame(int op, const char *data, size_t len) {
	uint8_t mask[4];
	esp_fill_random(mask, sizeof(mask));
	uint8_t *f = malloc(HIFI_WS_HEAD_MAX + len);
	if (!f) {
		note(HIFI_WS_DROP_NO_MEMORY, 0);
		return false;
	}
	size_t h = hifi_ws_head(f, op, len, mask);
	for (size_t i = 0; i < len; i++) f[h + i] = (uint8_t) data[i] ^ mask[i & 3];
	bool ok = write_all(f, h + len);
	free(f);
	return ok;
}

static bool upgrade(const char *host, int port, const char *path) {
	uint8_t raw[16];
	unsigned char key[32], acc[32], sha[20];
	size_t kn = 0, an = 0;
	esp_fill_random(raw, sizeof(raw));
	if (mbedtls_base64_encode(key, sizeof(key), &kn, raw, sizeof(raw))) return false;
	key[kn] = '\0';
	char *req = malloc(512 + strlen(headers));
	if (!req) return false;
	int n = sprintf(req, "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
			"Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nUser-Agent: hifi-board\r\n%s\r\n",
			path, host, (char *) key, headers);
	bool ok = write_all(req, (size_t) n);
	free(req);
	if (!ok) return false;
	/* the answer a byte at a time, so nothing after its blank line is read here */
	char *resp = malloc(2048);
	if (!resp) return false;
	size_t got = 0;
	while (got < 2047 && !(got >= 4 && !memcmp(resp + got - 4, "\r\n\r\n", 4))) {
		if (!read_exact(resp + got, 1)) break;
		got++;
	}
	resp[got] = '\0';
	char magic[64];
	snprintf(magic, sizeof(magic), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", (char *) key);
	ok = got >= 4 && !memcmp(resp + got - 4, "\r\n\r\n", 4) && !mbedtls_sha1_ret((const unsigned char *) magic, strlen(magic), sha) &&
	     !mbedtls_base64_encode(acc, sizeof(acc), &an, sha, sizeof(sha));
	if (ok) {
		acc[an] = '\0';
		ok = hifi_ws_upgrade_ok(resp, (char *) acc);
	}
	if (!ok) {
		char *e = strstr(resp, "\r\n");
		if (e) *e = '\0';
		ESP_LOGW(TAG, "the endpoint refused the link: %.60s", resp[0] ? resp : "(no answer)");
	}
	free(resp);
	(void) port;
	return ok;
}

static bool dial(void) {
	char host[HIFI_WS_HOST_MAX], path[HIFI_WS_PATH_MAX];
	int port;
	if (hifi_ws_url_parse(url, host, &port, path)) return false;
	xSemaphoreTake(lock, portMAX_DELAY);
	ssl = esp_transport_ssl_init();
	xSemaphoreGive(lock);
	if (!ssl) return false;
	esp_transport_ssl_crt_bundle_attach(ssl, esp_crt_bundle_attach);
	esp_transport_keep_alive_t ka = {.keep_alive_enable = true, .keep_alive_idle = 30, .keep_alive_interval = 10, .keep_alive_count = 3};
	esp_transport_ssl_set_keep_alive(ssl, &ka);
	if (esp_transport_connect(ssl, host, port, IO_MS) < 0) return false;
	return upgrade(host, port, path);
}

/* One frame in, whole. A text message goes to on_msg; a ping is answered; a
 * close or a broken read ends the link; anything else is read and dropped. */
static void read_frame(void) {
	uint8_t hb[HIFI_WS_HEAD_MAX];
	hifi_ws_head_t h;
	char *m = NULL;
	bool take = false;
	xSemaphoreTake(lock, portMAX_DELAY);
	if (!read_exact(hb, 2) || !read_exact(hb + 2, hifi_ws_head_len(hb) - 2)) {
		broken = true;
		goto out;
	}
	if (hifi_ws_head_parse(hb, &h)) {
		note(HIFI_WS_DROP_BAD_FRAME, 0);
		broken = true;
		goto out;
	}
	take = h.op == 1 && h.fin && h.len > 0 && h.len <= msg_max;
	bool ctl = h.op >= 8;
	if (take || ctl) {
		m = malloc((size_t) h.len + 1);
		if (!m) note(HIFI_WS_DROP_NO_MEMORY, 0);
		if (!m || !read_exact(m, (size_t) h.len)) {
			broken = true;
			goto out;
		}
		for (size_t i = 0; i < h.len; i++) m[i] ^= (char) h.mask[i & 3];
		m[h.len] = '\0';
	} else {
		char sink[128];
		for (uint64_t left = h.len; left;) {
			size_t n = left < sizeof(sink) ? (size_t) left : sizeof(sink);
			if (!read_exact(sink, n)) {
				broken = true;
				goto out;
			}
			left -= n;
		}
		ESP_LOGW(TAG, "dropped a %u-byte frame (opcode %d%s)", (unsigned) h.len, h.op, h.fin ? "" : ", split");
		goto out;
	}
	if (h.op == 9) {
		if (!write_frame(10, m, (size_t) h.len)) broken = true;
		last_tx = now_s();
	} else if (h.op == 8) {
		unsigned code = h.len >= 2 ? (unsigned) ((uint8_t) m[0] << 8 | (uint8_t) m[1]) : 1005;
		ESP_LOGW(TAG, "the endpoint closed the link (%u)", code);
		note(HIFI_WS_DROP_CLOSED, (int) code);
		write_frame(8, m, h.len >= 2 ? 2 : 0);
		broken = true;
	}
out:
	xSemaphoreGive(lock);
	if (m && take && !broken) {
		on_msg(m, (size_t) h.len);
		return;
	}
	free(m);
}

/* One task for the board's life, parked on `go` while the link is off: a
 * task that exited and was made again would need a fresh 6 KB stack each
 * time, and on a Lite with Bluetooth on, internal RAM can have no 6 KB block
 * left, so the link never started and said nothing. Its stack is in PSRAM;
 * nothing on it touches flash (messages go to the relay's worker, which
 * has an internal stack). */
static void ws_task(void *arg) {
	for (;;) {
		xSemaphoreTake(go, portMAX_DELAY);
		int backoff = BACKOFF_MIN_S;
		while (running) {
			broken = false;
			if (!dial()) {
				drop();
				ESP_LOGW(TAG, "couldn't reach %s; again in %d s", url, backoff);
				for (int i = 0; i < backoff * 2 && running; i++) vTaskDelay(pdMS_TO_TICKS(500));
				backoff = backoff * 2 > BACKOFF_MAX_S ? BACKOFF_MAX_S : backoff * 2;
				continue;
			}
			backoff = BACKOFF_MIN_S;
			ESP_LOGI(TAG, "up: %s", url);
			last_tx = now_s();
			portENTER_CRITICAL(&stats_mux);
			stats.up_since_us = esp_timer_get_time();
			if (!stats.first_up_us) stats.first_up_us = stats.up_since_us;
			portEXIT_CRITICAL(&stats_mux);
			/* no Ethernet address: the station carries the link (a board
			 * with both prefers Ethernet's route) */
			bool sta = !netif_has_ip("ETH_DEF") && netif_has_ip("WIFI_STA_DEF");
			uint32_t sta_drops = hifi_diag_wifi_disconnects();
			xSemaphoreTake(lock, portMAX_DELAY);
			esp_tls_error_handle_t eh = esp_transport_get_error_handle(ssl);
			if (eh) memset(eh, 0, sizeof(*eh)); /* the dial's own errors aren't this link's */
			cause = HIFI_WS_DROP_NONE;
			cause_code = 0;
			on_sta = sta;
			sta_drops_at_up = sta_drops;
			up = true;
			xSemaphoreGive(lock);
			if (on_up) on_up();
			while (running && !broken) {
				if (now_s() - last_tx >= PING_S) {
					xSemaphoreTake(lock, portMAX_DELAY);
					if (!write_frame(9, NULL, 0)) broken = true;
					last_tx = now_s();
					xSemaphoreGive(lock);
				}
				errno = 0;
				int p = esp_transport_poll_read(ssl, 500);
				if (p < 0) {
					int e = errno;
					xSemaphoreTake(lock, portMAX_DELAY); /* hifi_ws_send may be noting too */
					note_io(p, false, e);
					xSemaphoreGive(lock);
					break;
				}
				if (p > 0) read_frame(); /* a socket the far end closed reads as broken */
			}
			drop();
			if (running) {
				ESP_LOGW(TAG, "again in %d s", BACKOFF_MIN_S);
				for (int i = 0; i < BACKOFF_MIN_S * 2 && running; i++) vTaskDelay(pdMS_TO_TICKS(500));
			}
		}
		xSemaphoreGive(done);
	}
}
static bool task_make(void) {
	StackType_t *stack = heap_caps_malloc(STACK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	StaticTask_t *tcb = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
	if (stack && tcb) task = xTaskCreateStatic(ws_task, "hifi_ws", STACK, NULL, tskIDLE_PRIORITY + 2, stack, tcb);
	if (task) return true;
	free(stack);
	free(tcb);
	if (xTaskCreate(ws_task, "hifi_ws", STACK, NULL, tskIDLE_PRIORITY + 2, &task) == pdPASS) return true;
	task = NULL;
	ESP_LOGE(TAG, "no memory for the link's task (internal heap %u B free, largest block %u B)",
		 (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
	return false;
}

bool hifi_ws_start(const char *u, const char *h, size_t max, hifi_ws_msg_cb_t m, hifi_ws_up_cb_t cb) {
	if (running || !m || strlen(u) >= sizeof(url)) return false;
	if (!lock) lock = xSemaphoreCreateMutex();
	if (!done) done = xSemaphoreCreateBinary();
	if (!go) go = xSemaphoreCreateBinary();
	if (!lock || !done || !go) return false;
	if (!task && !task_make()) return false;
	free(headers);
	headers = strdup(h ? h : "");
	if (!headers) return false;
	strcpy(url, u);
	msg_max = max;
	on_msg = m;
	on_up = cb;
	running = true;
	xSemaphoreGive(go);
	return true;
}

void hifi_ws_stop(void) {
	if (!running) return;
	running = false;
	xSemaphoreTake(done, portMAX_DELAY); /* at most a dial's or a read's timeout */
	free(headers);
	headers = NULL;
}

bool hifi_ws_up(void) { return up; }

void hifi_ws_stats(hifi_ws_stats_t *out) {
	portENTER_CRITICAL(&stats_mux);
	*out = stats;
	portEXIT_CRITICAL(&stats_mux);
}

bool hifi_ws_send(const char *text, size_t len) {
	if (!up || !len) return false;
	xSemaphoreTake(lock, portMAX_DELAY);
	bool ok = up && ssl && write_frame(1, text, len);
	if (ok) last_tx = now_s();
	else broken = up;
	xSemaphoreGive(lock);
	return ok;
}
