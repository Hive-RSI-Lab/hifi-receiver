/*
 * The test API and the web app on squeezelite-esp32's HTTP server (port 80).
 * squeezelite's own UI moves to /squeezelite/ (firmware/patches), so '/'
 * opens the web app.
 */
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"

#include "hifi_esp.h"
#include "hifi_gate.h"
#include "hifi_http.h"
#include "webapp_files.h"

static const char TAG[] = "hifi_http";
static const hifi_ops_t *ops;

#define BODY_MAX 8192 /* an OTA chunk is about 5.6 KB */

static const char *status_line(int st) {
	switch (st) {
	case 200: return "200 OK";
	case 400: return "400 Bad Request";
	case 403: return "403 Forbidden";
	case 404: return "404 Not Found";
	case 405: return "405 Method Not Allowed";
	case 409: return "409 Conflict";
	case 503: return "503 Service Unavailable";
	case 507: return "507 Insufficient Storage";
	default: return "500 Internal Server Error";
	}
}

/* ---- the LAN gate (core/hifi_gate.h) ---- */

static bool gate_ok(httpd_req_t *req) {
	char host[128] = "", origin[128] = "";
	size_t hl = httpd_req_get_hdr_value_len(req, "Host"), ol = httpd_req_get_hdr_value_len(req, "Origin");
	/* a header too long for the buffer is not one the gate can vouch for */
	if (hl >= sizeof(host) || ol >= sizeof(origin)) return false;
	if (hl) httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
	if (ol) httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin));
	const char *names[4];
	size_t n = 0;
	for (esp_netif_t *nif = esp_netif_next(NULL); nif && n < 4; nif = esp_netif_next(nif)) {
		const char *h = NULL;
		if (esp_netif_get_hostname(nif, &h) == ESP_OK && h && *h) names[n++] = h;
	}
	if (hifi_gate_allow(host, origin, names, n)) return true;
	ESP_LOGW(TAG, "refused %s: Host '%s' Origin '%s'", req->uri, host, origin);
	return false;
}

static esp_err_t refuse(httpd_req_t *req) {
	static const char msg[] = "{\"error\":\"open the board by its IP address or name\"}";
	httpd_resp_set_status(req, status_line(403));
	httpd_resp_set_type(req, "application/json");
	return httpd_resp_send(req, msg, sizeof(msg) - 1);
}

typedef struct {
	esp_err_t (*handler)(httpd_req_t *r);
	void *ctx;
} gated_t;

/* new firmware on trial rolls back on a reboot or a lost Wi-Fi: squeezelite's
 * own pages may not reboot, change the Wi-Fi or the settings until it is kept */
static bool on_trial(httpd_req_t *req) {
	if (!hifi_lan_trial_refused(req->uri, http_method_str(req->method))) return false;
	esp_ota_img_states_t st;
	return esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK &&
	       st == ESP_OTA_IMG_PENDING_VERIFY;
}

static esp_err_t gate_trampoline(httpd_req_t *req) {
	const gated_t *g = req->user_ctx;
	if (!gate_ok(req)) return refuse(req);
	if (on_trial(req)) {
		static const char msg[] = "{\"error\":\"new firmware is on trial; try again in a few minutes\"}";
		httpd_resp_set_status(req, status_line(409));
		httpd_resp_set_type(req, "application/json");
		return httpd_resp_send(req, msg, sizeof(msg) - 1);
	}
	/* one server task: the last time needs no lock */
	static long long last_costly;
	if (hifi_lan_throttled(req->uri, http_method_str(req->method), esp_timer_get_time(), &last_costly)) {
		static const char msg[] = "{\"error\":\"busy: try again in a few seconds\"}";
		httpd_resp_set_status(req, status_line(503));
		httpd_resp_set_type(req, "application/json");
		httpd_resp_set_hdr(req, "Retry-After", "5");
		return httpd_resp_send(req, msg, sizeof(msg) - 1);
	}
	req->user_ctx = g->ctx; /* the server sets it again on every request */
	return g->handler(req);
}

esp_err_t hifi_lan_register(httpd_handle_t server, const httpd_uri_t *u) {
	gated_t *g = malloc(sizeof(*g));
	if (!g) return ESP_ERR_NO_MEM;
	g->handler = u->handler;
	g->ctx = u->user_ctx;
	httpd_uri_t c = *u;
	c.handler = gate_trampoline;
	c.user_ctx = g;
	esp_err_t e = httpd_register_uri_handler(server, &c);
	if (e != ESP_OK) free(g);
	return e;
}

static esp_err_t api_handler(httpd_req_t *req) {
	int st;
	if (req->content_len > BODY_MAX) {
		char err[48];
		snprintf(err, sizeof(err), "{\"error\":\"body over %d bytes\"}", BODY_MAX);
		httpd_resp_set_status(req, status_line(400));
		httpd_resp_set_type(req, "application/json");
		return httpd_resp_send(req, err, strlen(err));
	}
	char *body = calloc(1, req->content_len + 1);
	size_t got = 0;
	while (body && got < req->content_len) {
		int r = httpd_req_recv(req, body + got, req->content_len - got);
		if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
		if (r <= 0) break;
		got += r;
	}
	size_t cap = hifi_api_reply_cap(req->uri, body);
	char *out = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if (!out) out = malloc(cap);
	if (!out) {
		free(body);
		return httpd_resp_send_500(req);
	}
	if (!body || got < req->content_len) {
		/* never act on half a request */
		snprintf(out, cap, "{\"error\":\"body cut off after %u of %u bytes\"}",
			 (unsigned) got, (unsigned) req->content_len);
		st = 400;
	} else {
		const char *method = req->method == HTTP_POST ? "POST" : "GET";
		st = hifi_api_handle_from(ops, HIFI_SRC_LAN, method, req->uri, body, out, cap);
	}
	httpd_resp_set_status(req, status_line(st));
	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
	esp_err_t err = httpd_resp_send(req, out, strlen(out));
	free(body);
	free(out);
	return err;
}

/* A file's tag: FNV-1a over its gzipped bytes and its length, worked out on
 * each request (well under a millisecond for the largest file, from flash)
 * so it needs no heap. A reload that sends it back gets a bodiless 304; the
 * browser still asks each time (no-cache), so an update is seen at once. */
static void file_tag(const webapp_file_t *f, char out[24]) {
	uint32_t h = 2166136261u;
	for (size_t i = 0; i < f->len; i++) h = (h ^ f->data[i]) * 16777619u;
	snprintf(out, 24, "\"%08lx-%x\"", (unsigned long) h, (unsigned) f->len);
}

static esp_err_t file_handler(httpd_req_t *req) {
	const webapp_file_t *f = req->user_ctx;
	char tag[24], seen[24];
	file_tag(f, tag);
	httpd_resp_set_hdr(req, "ETag", tag);
	httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
	httpd_resp_set_type(req, f->content_type);
	if (httpd_req_get_hdr_value_str(req, "If-None-Match", seen, sizeof(seen)) == ESP_OK && !strcmp(seen, tag)) {
		httpd_resp_set_status(req, "304 Not Modified");
		return httpd_resp_send(req, NULL, 0);
	}
	httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
	return httpd_resp_send(req, (const char *) f->data, f->len);
}

void hifi_http_start(httpd_handle_t server, const hifi_ops_t *o) {
	ops = o;
	/* the app's firmware upload is a raw body, too big for api_handler: first */
	httpd_uri_t up = {.uri = "/api/v1/update/upload", .method = HTTP_POST, .handler = hifi_net_upload_handler};
	hifi_lan_register(server, &up);
	httpd_uri_t get = {.uri = "/api/v1/*", .method = HTTP_GET, .handler = api_handler};
	httpd_uri_t post = {.uri = "/api/v1/*", .method = HTTP_POST, .handler = api_handler};
	hifi_lan_register(server, &get);
	hifi_lan_register(server, &post);

	for (size_t i = 0; i < webapp_files_count; i++) {
		const webapp_file_t *f = &webapp_files[i];
		httpd_uri_t u = {.uri = f->path, .method = HTTP_GET, .handler = file_handler, .user_ctx = (void *) f};
		if (hifi_lan_register(server, &u) != ESP_OK) ESP_LOGW(TAG, "no room for %s", f->path);
		if (!strcmp(f->path, "/index.html")) {
			u.uri = "/";
			hifi_lan_register(server, &u);
		}
	}
	ESP_LOGI(TAG, "test API at /api/v1/, web app at / (%u files)", (unsigned) webapp_files_count);
}
