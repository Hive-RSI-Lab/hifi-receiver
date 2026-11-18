/*
 * The app's Wi-Fi and update calls (docs/test-api.md): a scan through
 * squeezelite's network manager, forgetting the network, and firmware
 * updates the person starts, from the update manifest's URL or from a file
 * the phone uploads. Both write the same OTA session as the relay's
 * update (hifi_relay.c), so a bad image rolls back the same way.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hifi_crash.h"

#include "network_manager.h"
#include "network_status.h"
#include "platform_config.h"

#include "hifi_board.h"
#include "hifi_esp.h"
#include "hifi_json.h"

static const char TAG[] = "hifi_net";

/* ------------------------------------------------------------------ Wi-Fi */

/* The network manager reads every scan's results itself, so ask it to scan
 * and read its list once the scan has had time (about 2.5 s for all
 * channels); this waits in the HTTP handler, as the app shows "Scanning…". */
int hifi_net_wifi_scan(hifi_wifi_ap_t *aps, int max) {
	network_async_scan();
	vTaskDelay(pdMS_TO_TICKS(4000));
	if (!network_status_lock_json_buffer(pdMS_TO_TICKS(1000))) return -1;
	char *s = network_status_alloc_get_ap_list_json();
	network_status_unlock_json_buffer();
	cJSON *list = s ? cJSON_Parse(s) : NULL;
	free(s);
	int n = 0;
	cJSON *ap;
	cJSON_ArrayForEach(ap, list) {
		cJSON *ssid = cJSON_GetObjectItem(ap, "ssid"), *rssi = cJSON_GetObjectItem(ap, "rssi"),
		      *auth = cJSON_GetObjectItem(ap, "auth");
		/* a known network out of range comes without a signal: not in range */
		if (n >= max || !cJSON_IsString(ssid) || !cJSON_IsNumber(rssi)) continue;
		snprintf(aps[n].ssid, sizeof(aps[n].ssid), "%s", ssid->valuestring);
		aps[n].rssi = rssi->valueint;
		aps[n].secure = cJSON_IsNumber(auth) && auth->valueint != 0; /* WIFI_AUTH_OPEN */
		n++;
	}
	cJSON_Delete(list);
	return n;
}

int hifi_net_wifi_forget(void) {
	network_async_delete(); /* drops the saved network and opens the receiver's own */
	return 0;
}

/* ------------------------------------------------------------------ updates */

static hifi_update_status_t upd;    /* what /state reports */
static hifi_update_offer_t offer;  /* the last check's, for its sha256 */
static char job_url[192], job_sha[65];
static bool job_need_sig; /* from the LAN: the image must be signed (hifi_ota.h) */

void hifi_net_update_status(hifi_update_status_t *out) { *out = upd; }

static void fail(const char *why) {
	ESP_LOGE(TAG, "update failed: %s", why);
	snprintf(upd.error, sizeof(upd.error), "%s", why);
	upd.state = HIFI_UPD_ERROR;
}

static void restart_soon(void *arg) {
	vTaskDelay(pdMS_TO_TICKS(1500)); /* the app sees "done" first */
	hifi_restart_note("update_done");
	esp_restart();
}

/* the manifest's URL: NVS hifi_update_url, or the build's HIFI_UPDATE_URL */
static char *manifest_url(void) {
#ifdef HIFI_UPDATE_URL
	return config_alloc_get_str("hifi_update_url", NULL, HIFI_UPDATE_URL);
#else
	return config_alloc_get_str("hifi_update_url", NULL, NULL);
#endif
}

static esp_http_client_handle_t open_url(const char *url, int *len) {
	esp_http_client_config_t c = {.url = url, .timeout_ms = 10000, .buffer_size = 2048};
	if (!strncmp(url, "https://", 8)) c.crt_bundle_attach = esp_crt_bundle_attach;
	esp_http_client_handle_t h = esp_http_client_init(&c);
	if (!h) return NULL;
	if (esp_http_client_open(h, 0) != ESP_OK) {
		esp_http_client_cleanup(h);
		return NULL;
	}
	*len = esp_http_client_fetch_headers(h);
	int st = esp_http_client_get_status_code(h);
	if (st != 200) {
		ESP_LOGW(TAG, "%s: HTTP %d", url, st);
		esp_http_client_cleanup(h);
		return NULL;
	}
	return h;
}

int hifi_net_update_check(hifi_update_offer_t *out, const char **err) {
	char *url = manifest_url();
	if (!url || !*url) {
		free(url);
		return *err = "no update server set (NVS hifi_update_url): use a file", 404;
	}
	int len = 0, st = 0;
	esp_http_client_handle_t h = open_url(url, &len);
	char *buf = h ? calloc(1, 4096) : NULL;
	if (!h || !buf) {
		st = 502, *err = "the update server didn't answer";
	} else {
		int got = 0, r;
		while (got < 4095 && (r = esp_http_client_read(h, buf + got, 4095 - got)) > 0) got += r;
		if (hifi_update_parse(buf, url, HIFI_MODEL, out, err)) st = 502;
		else offer = *out;
	}
	free(buf);
	if (h) esp_http_client_cleanup(h);
	free(url);
	return st;
}

static void download(void *arg) {
	hifi_ota_t *o = &hifi_ota_session;
	const char *err = "update failed";
	int len = 0, st;
	uint8_t *buf = malloc(HIFI_OTA_CHUNK_MAX);
	esp_http_client_handle_t h = buf ? open_url(job_url, &len) : NULL;
	if (!h) {
		fail("couldn't download the image");
	} else if (len <= 0) {
		fail("the server didn't say the image's size");
	} else if (hifi_ota_begin_ex(o, len, job_sha[0] ? job_sha : NULL, NULL, job_need_sig, &err) != 200) {
		fail(err);
	} else {
		/* the app reads the next chunk while this one is written (hifi_dl.c);
		 * recovery, or the app short of memory for it, one after the other */
		if (hifi_dl_pump && (st = hifi_dl_pump(o, h, buf, &upd.progress, &err)) >= 0) {
			if (st) fail(err);
		} else while (o->active && o->written < o->size) {
			int want = o->size - o->written < HIFI_OTA_CHUNK_MAX ? o->size - o->written : HIFI_OTA_CHUNK_MAX;
			int got = 0, r;
			while (got < want && (r = esp_http_client_read(h, (char *) buf + got, want - got)) > 0) got += r;
			if (got < want) {
				hifi_ota_abort(o);
				fail("the download stopped part way");
				break;
			}
			if (hifi_ota_chunk(o, o->written, buf, got, &err) != 200) {
				fail(err);
				break;
			}
			upd.progress = (int) (o->written * 100 / o->size);
		}
		if (upd.state == HIFI_UPD_DOWNLOADING) {
			upd.state = HIFI_UPD_FLASHING;
			if (hifi_ota_finish(o, &err) != 200) {
				fail(err);
			} else {
				upd.state = HIFI_UPD_DONE;
				ESP_LOGI(TAG, "update from %s written to %s: restarting", job_url, o->slot);
				xTaskCreate(restart_soon, "hifi_restart", 2048, NULL, ESP_TASK_PRIO_MIN + 1, NULL);
			}
		}
	}
	if (h) esp_http_client_cleanup(h);
	free(buf);
	hifi_ota_release(o);
	vTaskDelete(NULL);
}

int hifi_net_update_start(const char *url, const char *sha, bool need_sig, const char **err) {
	/* held until download() ends: the API can't begin over it (L10) */
	if (hifi_ota_claim(&hifi_ota_session, err)) return 409;
	job_need_sig = need_sig;
	snprintf(job_url, sizeof(job_url), "%s", url);
	/* no hash given: the last check's, if it offered this url */
	snprintf(job_sha, sizeof(job_sha), "%s", sha ? sha : !strcmp(url, offer.url) ? offer.sha256 : "");
	upd = (hifi_update_status_t) {.state = HIFI_UPD_DOWNLOADING};
	if (xTaskCreate(download, "hifi_update", 6144, NULL, ESP_TASK_PRIO_MIN + 1, NULL) != pdPASS) {
		hifi_ota_release(&hifi_ota_session);
		upd.state = HIFI_UPD_IDLE;
		return *err = "out of memory", 500;
	}
	return 0;
}

/* POST /api/v1/update/upload with the image as the body (the app's "From a
 * file…"). It writes as it receives, then answers with the state. */
esp_err_t hifi_net_upload_handler(httpd_req_t *req) {
	hifi_ota_t *o = &hifi_ota_session;
	const char *err = "update failed";
	int st = 200;
	uint8_t *buf = NULL;
	bool held = false;
	if (hifi_ota_claim(o, &err)) {
		st = 409;
	} else if ((held = true, buf = malloc(HIFI_OTA_CHUNK_MAX)) == NULL) {
		st = 500, err = "out of memory";
	} else if ((st = hifi_ota_begin_ex(o, req->content_len, NULL, NULL, true, &err)) == 200) {
		upd = (hifi_update_status_t) {.state = HIFI_UPD_DOWNLOADING};
		while (o->active && o->written < o->size) {
			int want = o->size - o->written < HIFI_OTA_CHUNK_MAX ? o->size - o->written : HIFI_OTA_CHUNK_MAX;
			int got = 0, r;
			while (got < want) {
				r = httpd_req_recv(req, (char *) buf + got, want - got);
				if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
				if (r <= 0) break;
				got += r;
			}
			if (got < want) {
				hifi_ota_abort(o);
				st = 400, err = "the upload stopped part way";
				break;
			}
			if ((st = hifi_ota_chunk(o, o->written, buf, got, &err)) != 200) break;
			upd.progress = (int) (o->written * 100 / o->size);
		}
		if (st == 200) {
			upd.state = HIFI_UPD_FLASHING;
			st = hifi_ota_finish(o, &err);
		}
		if (st == 200) {
			upd.state = HIFI_UPD_DONE;
			xTaskCreate(restart_soon, "hifi_restart", 2048, NULL, ESP_TASK_PRIO_MIN + 1, NULL);
		} else {
			fail(err);
		}
	}
	if (held) hifi_ota_release(o);
	free(buf);

	char *out = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if (!out) out = malloc(4096);
	if (!out) return httpd_resp_send_500(req);
	if (st == 200) {
		hifi_snapshot_t *s = calloc(1, sizeof(*s));
		if (s) hifi_ops.snapshot(s);
		if (s) hifi_api_write_state(s, out, 4096);
		else snprintf(out, 4096, "{\"ok\":true}");
		free(s);
		httpd_resp_set_status(req, "200 OK");
	} else {
		hj_t j;
		hj_init(&j, out, 4096);
		hj_obj(&j, NULL);
		hj_str(&j, "error", err);
		hj_obj_end(&j);
		httpd_resp_set_status(req, st == 409 ? "409 Conflict" : st == 400 ? "400 Bad Request" : "500 Internal Server Error");
	}
	httpd_resp_set_type(req, "application/json");
	esp_err_t e = httpd_resp_send(req, out, strlen(out));
	free(out);
	return e;
}
