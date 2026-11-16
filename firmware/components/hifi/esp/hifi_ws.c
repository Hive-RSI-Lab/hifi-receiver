/*
 * The board's WebSocket to the Hive (hifi_ws.h). Built on tcp_transport
 * rather than esp_websocket_client: in IDF 4.4 that client can't take the
 * certificate bundle, and without a CA it doesn't check the server at all.
 * Here the SSL transport carries esp_crt_bundle_attach, so the Cloudflare
 * edge's certificate is checked like any https:// site's.
 *
 * mbedTLS isn't safe from two tasks at once, so every read and write on the
 * socket holds `lock`; the task waits for data (poll) without it.
 */
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_transport.h"
#include "esp_transport_ssl.h"
#include "esp_transport_ws.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "hifi_ws.h"
#include "hifi_ws_proto.h"

static const char TAG[] = "hifi_ws";

#define PING_S 20       /* the endpoint drops a board it hasn't heard from in 90 s */
#define BACKOFF_MIN_S 5
#define BACKOFF_MAX_S 300
#define IO_MS 10000

static SemaphoreHandle_t lock;
static SemaphoreHandle_t done;
static esp_transport_handle_t ssl, ws;
static char url[128], *headers;
static size_t msg_max;
static hifi_ws_msg_cb_t on_msg;
static hifi_ws_up_cb_t on_up;
static volatile bool running, up, broken;
static int64_t last_tx;

static int64_t now_s(void) { return esp_timer_get_time() / 1000000; }

static void drop(void) {
	xSemaphoreTake(lock, portMAX_DELAY);
	up = false;
	if (ws) {
		esp_transport_close(ws);
		esp_transport_destroy(ws); /* frees only its own state, not the SSL under it */
	}
	if (ssl) esp_transport_destroy(ssl);
	ws = ssl = NULL;
	xSemaphoreGive(lock);
}

static bool dial(void) {
	char host[HIFI_WS_HOST_MAX], path[HIFI_WS_PATH_MAX];
	int port;
	if (hifi_ws_url_parse(url, host, &port, path)) return false;
	ssl = esp_transport_ssl_init();
	if (!ssl) return false;
	esp_transport_ssl_crt_bundle_attach(ssl, esp_crt_bundle_attach);
	esp_transport_keep_alive_t ka = {.keep_alive_enable = true, .keep_alive_idle = 30, .keep_alive_interval = 10, .keep_alive_count = 3};
	esp_transport_ssl_set_keep_alive(ssl, &ka);
	ws = esp_transport_ws_init(ssl);
	if (!ws) return false;
	esp_transport_ws_set_path(ws, path);
	if (headers[0] && esp_transport_ws_set_headers(ws, headers) != ESP_OK) return false;
	/* a refused key (403), a revoked one or no Access token all end here:
	 * IDF 4.4 doesn't say the HTTP status, so each is just a failed dial */
	if (esp_transport_connect(ws, host, port, IO_MS) < 0) return false;
	return true;
}

/* One text frame in, whole; anything else (binary, too big) is read and dropped. */
static void read_frame(void) {
	char first[256];
	xSemaphoreTake(lock, portMAX_DELAY);
	int r = esp_transport_read(ws, first, sizeof(first), IO_MS);
	if (r < 0) {
		broken = true;
		xSemaphoreGive(lock);
		return;
	}
	ws_transport_opcodes_t op = esp_transport_ws_get_read_opcode(ws);
	int total = esp_transport_ws_get_read_payload_len(ws);
	if (r == 0 || op == WS_TRANSPORT_OPCODES_NONE) {
		/* a ping or pong handled inside the transport, or the Hive's close
		 * (answered there too); a closed socket shows up as a failed read next */
		xSemaphoreGive(lock);
		return;
	}
	bool take = (op & 0x0f) == WS_TRANSPORT_OPCODES_TEXT && total > 0 && (size_t) total <= msg_max;
	char *m = take ? malloc((size_t) total + 1) : NULL;
	if (m) memcpy(m, first, (size_t) r);
	int got = r;
	while (got < total) {
		int n = m ? esp_transport_read(ws, m + got, total - got, IO_MS) : esp_transport_read(ws, first, sizeof(first) < (size_t) (total - got) ? (int) sizeof(first) : total - got, IO_MS);
		if (n <= 0) {
			broken = true;
			break;
		}
		got += n;
	}
	xSemaphoreGive(lock);
	if (!m) {
		if (!broken) ESP_LOGW(TAG, "dropped a %d-byte frame (opcode %d)", total, op & 0x0f);
		return;
	}
	if (broken) {
		free(m);
		return;
	}
	m[total] = '\0';
	on_msg(m, (size_t) total);
}

static void ws_task(void *arg) {
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
		up = true;
		if (on_up) on_up();
		int idle = 0; /* readable but nothing read: a socket the far end closed */
		while (running && !broken) {
			if (now_s() - last_tx >= PING_S) {
				xSemaphoreTake(lock, portMAX_DELAY);
				if (esp_transport_write(ws, NULL, 0, IO_MS) < 0) broken = true; /* zero bytes: a ping */
				last_tx = now_s();
				xSemaphoreGive(lock);
			}
			int p = esp_transport_poll_read(ws, 500);
			if (p < 0) break;
			if (p == 0) {
				idle = 0;
				continue;
			}
			bool was = broken;
			read_frame();
			idle = !was && !broken ? idle + 1 : 0;
			if (esp_transport_ws_get_read_opcode(ws) != WS_TRANSPORT_OPCODES_NONE) idle = 0; /* a frame came */
			if (idle >= 3) break;
		}
		drop();
		if (running) {
			ESP_LOGW(TAG, "link dropped; again in %d s", BACKOFF_MIN_S);
			for (int i = 0; i < BACKOFF_MIN_S * 2 && running; i++) vTaskDelay(pdMS_TO_TICKS(500));
		}
	}
	xSemaphoreGive(done);
	vTaskDelete(NULL);
}

bool hifi_ws_start(const char *u, const char *h, size_t max, hifi_ws_msg_cb_t m, hifi_ws_up_cb_t cb) {
	if (running || !m || strlen(u) >= sizeof(url)) return false;
	if (!lock) lock = xSemaphoreCreateMutex();
	if (!done) done = xSemaphoreCreateBinary();
	if (!lock || !done) return false;
	free(headers);
	headers = strdup(h ? h : "");
	if (!headers) return false;
	strcpy(url, u);
	msg_max = max;
	on_msg = m;
	on_up = cb;
	running = true;
	if (xTaskCreate(ws_task, "hifi_ws", 6144, NULL, tskIDLE_PRIORITY + 2, NULL) != pdPASS) {
		running = false;
		return false;
	}
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

bool hifi_ws_send(const char *text, size_t len) {
	if (!up || !len) return false;
	char *b = malloc(len); /* the transport masks it in place */
	if (!b) return false;
	memcpy(b, text, len);
	xSemaphoreTake(lock, portMAX_DELAY);
	bool ok = up && ws && esp_transport_ws_send_raw(ws, WS_TRANSPORT_OPCODES_TEXT | WS_TRANSPORT_OPCODES_FIN, b, (int) len, IO_MS) == (int) len;
	if (ok) last_tx = now_s();
	else broken = up;
	xSemaphoreGive(lock);
	free(b);
	return ok;
}
