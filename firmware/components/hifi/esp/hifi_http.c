/*
 * The test API and the web app on squeezelite-esp32's HTTP server (port 80).
 * squeezelite's own UI moves to /squeezelite/ (firmware/patches), so '/'
 * opens the web app.
 */
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "hifi_esp.h"
#include "hifi_http.h"
#include "webapp_files.h"

static const char TAG[] = "hifi_http";
static const hifi_ops_t *ops;

#define BODY_MAX 8192 /* an OTA chunk is about 5.6 KB */

static const char *status_line(int st) {
	switch (st) {
	case 200: return "200 OK";
	case 400: return "400 Bad Request";
	case 404: return "404 Not Found";
	case 405: return "405 Method Not Allowed";
	case 409: return "409 Conflict";
	default: return "500 Internal Server Error";
	}
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
		st = hifi_api_handle(ops, method, req->uri, body, out, cap);
	}
	httpd_resp_set_status(req, status_line(st));
	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	esp_err_t err = httpd_resp_send(req, out, strlen(out));
	free(body);
	free(out);
	return err;
}

static esp_err_t file_handler(httpd_req_t *req) {
	const webapp_file_t *f = req->user_ctx;
	httpd_resp_set_type(req, f->content_type);
	httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
	httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
	return httpd_resp_send(req, (const char *) f->data, f->len);
}

void hifi_http_start(httpd_handle_t server, const hifi_ops_t *o) {
	ops = o;
	/* the app's firmware upload is a raw body, too big for api_handler: first */
	httpd_uri_t up = {.uri = "/api/v1/update/upload", .method = HTTP_POST, .handler = hifi_net_upload_handler};
	httpd_register_uri_handler(server, &up);
	httpd_uri_t get = {.uri = "/api/v1/*", .method = HTTP_GET, .handler = api_handler};
	httpd_uri_t post = {.uri = "/api/v1/*", .method = HTTP_POST, .handler = api_handler};
	httpd_register_uri_handler(server, &get);
	httpd_register_uri_handler(server, &post);

	for (size_t i = 0; i < webapp_files_count; i++) {
		const webapp_file_t *f = &webapp_files[i];
		httpd_uri_t u = {.uri = f->path, .method = HTTP_GET, .handler = file_handler, .user_ctx = (void *) f};
		if (httpd_register_uri_handler(server, &u) != ESP_OK) ESP_LOGW(TAG, "no room for %s", f->path);
		if (!strcmp(f->path, "/index.html")) {
			u.uri = "/";
			httpd_register_uri_handler(server, &u);
		}
	}
	ESP_LOGI(TAG, "test API at /api/v1/, web app at / (%u files)", (unsigned) webapp_files_count);
}
