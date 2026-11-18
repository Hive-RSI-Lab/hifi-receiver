/*
 * The remote channel on the board (docs/remote.md): an MQTT client that
 * dials out to the relay, or (a wss:// URL) a WebSocket to the Hive's bench
 * endpoint (esp/hifi_ws.c), takes signed commands through core/hifi_remote.c,
 * and publishes replies, status and events. Also the OTA session's flash
 * writes (esp_ota_*) and the trial boot that keeps or rolls back a new image.
 */
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "sodium.h"

#include "platform_config.h"

#include "hifi_board.h"
#include "hifi_crash.h"
#include "hifi_esp.h"
#include "hifi_json.h"
#include "hifi_remote.h"
#include "hifi_ws.h"
#include "hifi_ws_proto.h"

static const char TAG[] = "hifi_remote";

/* The Hive's public signing key(s), hex, comma-separated; set at build time
 * (firmware/tools/build.sh passes HIFI_HIVE_PUBKEY). Without one the board
 * still connects and reports, but takes no command from the relay. */
#ifndef HIFI_HIVE_PUBKEY
#define HIFI_HIVE_PUBKEY ""
#endif
/* A relay compiled in, used until provisioning sets one in NVS */
#ifndef HIFI_RELAY_URL
#define HIFI_RELAY_URL ""
#endif
#ifndef HIFI_RELAY_USER
#define HIFI_RELAY_USER ""
#endif
#ifndef HIFI_RELAY_PASS
#define HIFI_RELAY_PASS ""
#endif

#define MSG_MAX (16 * 1024) /* an OTA chunk command is about 5.6 KB */

static const hifi_ops_t *ops;
static hifi_remote_t rem;
static esp_mqtt_client_handle_t client;
static volatile bool connected;
static char url[128], user[48], pass[96], prefix[24] = "hifi";
static char key[66]; /* the board's own key for a wss:// endpoint; never shown */
static bool ws_on, ws_bad; /* the WebSocket is running; its settings won't dial */
static char topic_cmd[64], topic_reply[64], topic_status[64], topic_event[64];
static QueueHandle_t inbox;
/* The inbox holds up to INBOX_N messages and INBOX_BYTES of them at once. An
 * app's poll sends 2-3 calls together, so two apps (or an app and an agent)
 * need more than 4 slots; the byte cap keeps the heap the queued messages
 * hold under the old worst case (4 x MSG_MAX). Past either, a message is
 * dropped, as before. */
#define INBOX_N 12
#define INBOX_BYTES (24 * 1024)
static size_t inbox_bytes;
static portMUX_TYPE inbox_mux = portMUX_INITIALIZER_UNLOCKED;
static char *rx;      /* a message arriving in pieces */
static size_t rx_len;
static bool rollback_reported;
static volatile bool settings_changed;
static volatile bool announce; /* the link just came up: publish the status now */
static volatile uint32_t mqtt_drops; /* for `api diag/net` */
static int64_t mqtt_up_us, mqtt_first_us; /* 64-bit: under mqtt_mux */
static portMUX_TYPE mqtt_mux = portMUX_INITIALIZER_UNLOCKED;

hifi_ota_t hifi_ota_session;

/* ------------------------------------------------------------------ settings */

static void load_str(const char *key, const char *dflt, char *out, size_t n) {
	char *s = config_alloc_get_str(key, NULL, (char *) dflt); /* it only reads the default */
	snprintf(out, n, "%s", s ? s : dflt);
	free(s);
}

int hifi_remote_set(const char *u, const char *us, const char *pw, const char *pre, const char *k) {
	int err = 0;
	err |= config_set_value(NVS_TYPE_STR, "hifi_relay_url", u) != ESP_OK;
	err |= config_set_value(NVS_TYPE_STR, "hifi_relay_user", us) != ESP_OK;
	err |= config_set_value(NVS_TYPE_STR, "hifi_relay_pass", pw) != ESP_OK;
	err |= config_set_value(NVS_TYPE_STR, "hifi_relay_prefix", pre) != ESP_OK;
	err |= config_set_value(NVS_TYPE_STR, "hifi_relay_key", k ? k : "") != ESP_OK;
	if (err) return -1;
	settings_changed = true; /* the relay task reconnects with them */
	return 0;
}

static void load_settings(void) {
	load_str("hifi_relay_url", HIFI_RELAY_URL, url, sizeof(url));
	load_str("hifi_relay_user", HIFI_RELAY_USER, user, sizeof(user));
	load_str("hifi_relay_pass", HIFI_RELAY_PASS, pass, sizeof(pass));
	load_str("hifi_relay_prefix", "hifi", prefix, sizeof(prefix));
	load_str("hifi_relay_key", "", key, sizeof(key));
	snprintf(topic_cmd, sizeof(topic_cmd), "%s/%s/cmd", prefix, rem.dev);
	snprintf(topic_reply, sizeof(topic_reply), "%s/%s/reply", prefix, rem.dev);
	snprintf(topic_status, sizeof(topic_status), "%s/%s/status", prefix, rem.dev);
	snprintf(topic_event, sizeof(topic_event), "%s/%s/event", prefix, rem.dev);
}

/* only whether the dial-in is set and up: no copies, safe from any task */
void hifi_remote_up(bool *configured, bool *up) {
	*configured = url[0] != 0;
	*up = connected;
}

void hifi_remote_status(hifi_remote_status_t *r) {
	r->configured = url[0] != 0;
	r->connected = connected;
	snprintf(r->url, sizeof(r->url), "%s", url);
	snprintf(r->user, sizeof(r->user), "%s", user);
	snprintf(r->prefix, sizeof(r->prefix), "%s", prefix);
	snprintf(r->dev, sizeof(r->dev), "%s", rem.dev);
	r->has_key = key[0] != 0;
	r->keys = rem.nkeys;
	r->accepted = rem.accepted;
	r->rejected = rem.rejected;
}

/* ------------------------------------------------------------------ publishing */

/* t tags the message on the WebSocket, where one socket carries all three */
static void publish(const char *topic, const char *t, const char *data, bool retain) {
	if (client && connected) {
		esp_mqtt_client_publish(client, topic, data, 0, 1, retain);
		return;
	}
	if (!ws_on || !hifi_ws_up()) return;
	size_t n = strlen(data), cap = n + 32;
	char *b = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if (!b) b = malloc(cap);
	if (!b) return;
	memcpy(b, data, n + 1);
	n = hifi_ws_tag(b, n, cap, t);
	if (n) hifi_ws_send(b, n);
	free(b);
}

static void publish_status(void) {
	hifi_info_t i = {0};
	ops->info(&i);
	hifi_ota_status_t o = {0};
	hifi_ota_status(&o);
	char ip[16] = "";
	int rssi = 0;
	hifi_net_ip(ip, sizeof(ip));
	wifi_ap_record_t ap;
	if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;

	char buf[384];
	hj_t j;
	hj_init(&j, buf, sizeof(buf));
	hj_obj(&j, NULL);
	hj_bool(&j, "online", true);
	hj_str(&j, "model", i.model);
	hj_str(&j, "board", i.board);
	hj_str(&j, "fw", i.fw);
	hj_str(&j, "ip", ip[0] ? ip : NULL);
	if (rssi) hj_int(&j, "rssi", rssi);
	else hj_null(&j, "rssi");
	hj_int(&j, "uptime_s", esp_timer_get_time() / 1000000);
	hj_obj(&j, "ota");
	hj_str(&j, "slot", o.slot);
	hj_str(&j, "state", o.state);
	hj_obj_end(&j);
	hj_obj_end(&j);
	publish(topic_status, "status", buf, true);
}

static void publish_event(const char *event, const char *k1, const char *v1, const char *k2, const char *v2) {
	char buf[256];
	hj_t j;
	hj_init(&j, buf, sizeof(buf));
	hj_obj(&j, NULL);
	hj_str(&j, "event", event);
	hj_str(&j, "fw", HIFI_FW_VERSION);
	if (k1) hj_str(&j, k1, v1);
	if (k2) hj_str(&j, k2, v2);
	hj_obj_end(&j);
	publish(topic_event, "event", buf, false);
}

/* ------------------------------------------------------------------ commands */

static bool verify(const uint8_t pub[32], const uint8_t *msg, size_t n, const uint8_t sig[64]) {
	return crypto_sign_verify_detached(sig, msg, n, pub) == 0;
}

static int64_t clock_s(void) {
	time_t t = time(NULL);
	return t > 1700000000 ? (int64_t) t : 0; /* 0 until SNTP has set it */
}

typedef struct {
	char *data;
	size_t len;
} msg_t;

static void inbox_take(size_t len) { /* a queued message's bytes back */
	portENTER_CRITICAL(&inbox_mux);
	inbox_bytes -= len;
	portEXIT_CRITICAL(&inbox_mux);
}

/* queue a whole message, or free it if the inbox is full */
static void inbox_put(char *data, size_t len) {
	bool room;
	portENTER_CRITICAL(&inbox_mux);
	room = inbox_bytes + len <= INBOX_BYTES;
	if (room) inbox_bytes += len;
	portEXIT_CRITICAL(&inbox_mux);
	msg_t m = {.data = data, .len = len};
	if (!room || xQueueSend(inbox, &m, 0) != pdTRUE) {
		if (room) inbox_take(len);
		ESP_LOGW(TAG, "inbox full: a %u B message dropped", (unsigned) len);
		free(data);
	}
}

static void worker(void *arg) {
	msg_t m;
	for (;;) {
		if (xQueueReceive(inbox, &m, portMAX_DELAY) != pdTRUE) continue;
		size_t cap = hifi_remote_reply_cap(m.data, m.len);
		char *out = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
		if (!out) out = malloc(cap);
		if (out) {
			int st = hifi_remote_handle(&rem, ops, m.data, m.len, clock_s(), out, cap);
			publish(topic_reply, "reply", out, false);
			if (st != 200) ESP_LOGW(TAG, "command answered %d", st);
			free(out);
		}
		inbox_take(m.len);
		free(m.data);
	}
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
	esp_mqtt_event_handle_t e = data;
	switch ((esp_mqtt_event_id_t) id) {
	case MQTT_EVENT_CONNECTED:
		connected = true;
		portENTER_CRITICAL(&mqtt_mux);
		mqtt_up_us = esp_timer_get_time();
		if (!mqtt_first_us) mqtt_first_us = mqtt_up_us;
		portEXIT_CRITICAL(&mqtt_mux);
		ESP_LOGI(TAG, "relay connected: %s as %s", url, rem.dev);
		esp_mqtt_client_subscribe(client, topic_cmd, 1);
		publish_status();
		break;
	case MQTT_EVENT_DISCONNECTED:
		if (connected) mqtt_drops++;
		connected = false;
		portENTER_CRITICAL(&mqtt_mux);
		mqtt_up_us = 0;
		portEXIT_CRITICAL(&mqtt_mux);
		break;
	case MQTT_EVENT_DATA:
		/* a message bigger than the client's buffer arrives in pieces */
		if (e->current_data_offset == 0) {
			free(rx);
			rx = NULL;
			rx_len = 0;
			if (e->total_data_len > MSG_MAX) {
				ESP_LOGW(TAG, "dropping a %d-byte command", e->total_data_len);
				break;
			}
			rx = malloc(e->total_data_len + 1);
		}
		if (!rx) break;
		memcpy(rx + e->current_data_offset, e->data, e->data_len);
		rx_len = e->current_data_offset + e->data_len;
		if (rx_len == (size_t) e->total_data_len) {
			rx[rx_len] = '\0';
			inbox_put(rx, rx_len);
			rx = NULL;
		}
		break;
	default:
		break;
	}
}

static void on_ws_msg(char *data, size_t len) {
	inbox_put(data, len);
}

static void on_ws_up(void) {
	connected = true;
	ESP_LOGI(TAG, "bench endpoint connected: %s as %s", url, rem.dev);
	announce = true; /* the relay task publishes: the status reads flash, and this runs on the link's PSRAM stack */
}

static void ws_start(void) {
	char h[HIFI_WS_HEADERS_MAX];
	if (hifi_ws_headers(h, sizeof(h), user, pass, rem.dev, key) < 0) {
		ESP_LOGE(TAG, "%s needs the board's key from the Hive (and a whole service token, if any)", url);
		ws_bad = true;
		return;
	}
	ws_on = hifi_ws_start(url, h, MSG_MAX, on_ws_msg, on_ws_up);
	memset(h, 0, sizeof(h));
	if (!ws_on) {
		ESP_LOGE(TAG, "couldn't start the link to %s", url);
		ws_bad = true;
	}
}

static void client_start(void) {
	char lwt[] = "{\"online\":false}";
	char client_id[32];
	snprintf(client_id, sizeof(client_id), "hifi-%s", rem.dev);
	esp_mqtt_client_config_t c = {
		.uri = url,
		.username = user[0] ? user : NULL,
		.password = pass[0] ? pass : NULL,
		.client_id = client_id,
		.lwt_topic = topic_status,
		.lwt_msg = lwt,
		.lwt_qos = 1,
		.lwt_retain = 1,
		.keepalive = 60,
		.buffer_size = 8192,
		.out_buffer_size = 4096,
		.task_stack = 6144,
	};
	if (!strncmp(url, "mqtts://", 8)) c.crt_bundle_attach = esp_crt_bundle_attach;
	client = esp_mqtt_client_init(&c);
	if (!client) return;
	esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, on_event, NULL);
	esp_mqtt_client_start(client);
}

/* ------------------------------------------------------------------ OTA writes */

static esp_ota_handle_t ota_handle;
static const esp_partition_t *ota_part;
/* the update's timing, for `api diag/ota`: from open to commit, and the part
 * of it in esp_ota_begin/write/end (the slot's erase and flash writes) */
static int64_t ota_t0, ota_flash_us;
static uint32_t ota_bytes;

static int f_open(void *ctx, size_t size, const char **slot, const char **err) {
	ota_t0 = esp_timer_get_time();
	ota_flash_us = 0;
	ota_bytes = 0;
	/* the music stops gracefully before the flash is touched; an update
	 * that doesn't go ahead gives it back (f_abort) */
	hifi_quiet_stop("update", false);
	ota_part = esp_ota_get_next_update_partition(NULL);
	if (!ota_part) {
		hifi_quiet_release();
		return *err = "no app slot to write", -1;
	}
	if (esp_ota_begin(ota_part, size, &ota_handle) != ESP_OK) {
		hifi_quiet_release();
		return *err = "couldn't open the app slot", -1;
	}
	ota_flash_us += esp_timer_get_time() - ota_t0;
	*slot = ota_part->label;
	return 0;
}

static int f_write(void *ctx, const uint8_t *d, size_t n) {
	int64_t t = esp_timer_get_time();
	esp_err_t e = esp_ota_write(ota_handle, d, n);
	ota_flash_us += esp_timer_get_time() - t;
	if (e != ESP_OK) return -1;
	ota_bytes += n;
	return 0;
}

static int f_commit(void *ctx, const char **err) {
	int64_t t = esp_timer_get_time();
	if (esp_ota_end(ota_handle) != ESP_OK) return *err = "image didn't validate", -1;
	ota_flash_us += esp_timer_get_time() - t;
	if (esp_ota_set_boot_partition(ota_part) != ESP_OK) return *err = "couldn't set the boot slot", -1;
	esp_app_desc_t d;
	const char *v = esp_ota_get_partition_description(ota_part, &d) == ESP_OK ? d.version : "";
	hifi_diag_ota_done(ota_bytes, (uint32_t) ((esp_timer_get_time() - ota_t0) / 1000), (uint32_t) (ota_flash_us / 1000), v);
	return 0;
}

static void f_abort(void *ctx) {
	esp_ota_abort(ota_handle);
	hifi_quiet_release();
}

/* the web server, the relay and the URL job share the session (L10) */
static SemaphoreHandle_t ota_mux;
static void f_lock(void *ctx) { xSemaphoreTake(ota_mux, portMAX_DELAY); }
static void f_unlock(void *ctx) { xSemaphoreGive(ota_mux); }
static hifi_ota_io_t ota_io = {.open = f_open, .write = f_write, .commit = f_commit, .abort = f_abort,
			       .lock = f_lock, .unlock = f_unlock};

void hifi_ota_status(hifi_ota_status_t *o) {
	const esp_partition_t *run = esp_ota_get_running_partition();
	esp_ota_img_states_t st;
	o->slot = run->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY ? "recovery" : run->label;
	o->state = "unknown";
	if (esp_ota_get_state_partition(run, &st) == ESP_OK) {
		if (st == ESP_OTA_IMG_PENDING_VERIFY) o->state = "pending_verify";
		else if (st == ESP_OTA_IMG_VALID) o->state = "valid";
	} else if (run->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY) {
		o->state = "valid";
	}
	load_str("hifi_rb_from", "", o->last_from, sizeof(o->last_from));
	load_str("hifi_rb_why", "", o->last_reason, sizeof(o->last_reason));
}

void hifi_remote_link(hifi_diag_net_t *n, uint32_t *first_up_ms) {
	int64_t now = esp_timer_get_time(), up, first;
	if (ws_on) {
		hifi_ws_stats_t w;
		hifi_ws_stats(&w);
		n->tx_bytes = w.tx_bytes;
		n->rx_bytes = w.rx_bytes;
		n->link_drops = w.drops;
		up = w.up_since_us;
		first = w.first_up_us;
		n->link_last_drop = w.last_drop;
		n->link_last_code = w.last_drop_code;
		n->link_last_wifi_down = w.last_drop_wifi_down;
		n->link_last_ago_s = w.last_drop ? (uint32_t) ((now - w.last_drop_us) / 1000000) : 0;
		n->link_last_lasted_s = w.last_drop_lasted_s;
	} else {
		/* MQTT: no byte counts */
		n->link_drops = mqtt_drops;
		portENTER_CRITICAL(&mqtt_mux);
		up = mqtt_up_us;
		first = mqtt_first_us;
		portEXIT_CRITICAL(&mqtt_mux);
	}
	n->link_up = connected && up;
	n->link_up_s = n->link_up ? (uint32_t) ((now - up) / 1000000) : 0;
	*first_up_ms = (uint32_t) (first / 1000);
}

/* ------------------------------------------------------------------ the relay task */

/* L11: recovery's "boot the app" reads this (firmware/patches/0013): the slot
 * that last ran well, since otadata is wiped once recovery is booted */
static void note_good_slot(void) {
	const esp_partition_t *run = esp_ota_get_running_partition();
	if (run->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY) return;
	char saved[16];
	load_str("hifi_good_slot", "", saved, sizeof(saved));
	if (strcmp(saved, run->label)) config_set_value(NVS_TYPE_STR, "hifi_good_slot", run->label);
}

static hifi_restart_hold_t restart_hold; /* hifi_boot.h; under restart_mux */
static portMUX_TYPE restart_mux = portMUX_INITIALIZER_UNLOCKED;

/* hifi_esp.h: called from upstream's IP-change reboot (patch 0030). It reads
 * only RAM: restart_hold.on_trial is set at boot and cleared by the relay task
 * after the image is marked good, in the same critical section that takes
 * a held restart, so a restart is either held and then run by the relay task,
 * or run here with otadata already rewritten. */
bool hifi_restart_after_trial(const char *why) {
	portENTER_CRITICAL(&restart_mux);
	bool held = hifi_restart_hold(&restart_hold, why);
	portEXIT_CRITICAL(&restart_mux);
	if (held) ESP_LOGW(TAG, "%s: restart held until firmware %s is kept", why ? why : "restart", HIFI_FW_VERSION);
	return held;
}

static void relay_task(void *arg) {
	bool sntp = false, on_trial = false, boot_heap_taken = false;
	uint32_t trial_start = 0, last_status = 0;
	if (restart_hold.on_trial) { /* set by hifi_remote_start */
		on_trial = true;
		ESP_LOGW(TAG, "firmware %s is on trial: it has %d s to prove itself", HIFI_FW_VERSION, HIFI_TRIAL_LIMIT_S);
	} else {
		note_good_slot();
	}
	char pending[8];
	load_str("hifi_rb_report", "0", pending, sizeof(pending));
	rollback_reported = strcmp(pending, "1") != 0;

	for (;;) {
		vTaskDelay(pdMS_TO_TICKS(2000));
		hifi_diag_tick();
		if (hifi_party_poll) hifi_party_poll();
		uint32_t now = esp_timer_get_time() / 1000000;
		bool up = hifi_net_ip(NULL, 0);
		hifi_listen_tick(up);

		if (up && !sntp) {
			sntp_setoperatingmode(SNTP_OPMODE_POLL);
			sntp_setservername(0, "pool.ntp.org");
			sntp_init();
			sntp = true;
		}
		if (settings_changed) {
			settings_changed = false;
			if (client) {
				esp_mqtt_client_stop(client);
				esp_mqtt_client_destroy(client);
				client = NULL;
				connected = false;
			}
			if (ws_on) {
				hifi_ws_stop();
				ws_on = false;
				connected = false;
			}
			ws_bad = false;
			load_settings();
		}
		if (ws_on) connected = hifi_ws_up();
		if (up && url[0] && !client && !ws_on && !ws_bad) {
			if (!strncmp(url, "wss://", 6)) ws_start();
			else client_start();
		}

		if (connected && !rollback_reported) {
			hifi_ota_status_t o = {0};
			hifi_ota_status(&o);
			publish_event("rolled_back", "from", o.last_from, "reason", o.last_reason);
			config_set_value(NVS_TYPE_STR, "hifi_rb_report", "0");
			rollback_reported = true;
		}
		if (connected && (announce || now - last_status >= 60)) {
			announce = false;
			publish_status();
			last_status = now;
		}

		if (!boot_heap_taken && hifi_heap_boot_due(now, up, url[0] != 0, connected)) {
			hifi_main_note_boot_heap();
			boot_heap_taken = true;
		}

		if (on_trial) {
			if (!trial_start) trial_start = now ? now : 1;
			hifi_trial_t t = {0};
			hifi_main_trial_inputs(&t);
			t.relay_configured = url[0] != 0;
			t.relay_ok = connected;
			hifi_listen_trial(&t);
			char why[96];
			hifi_trial_verdict_t v = hifi_trial_judge(&t, now - trial_start, why, sizeof(why));
			esp_err_t valid = ESP_FAIL;
			if (v == HIFI_TRIAL_GOOD && (valid = esp_ota_mark_app_valid_cancel_rollback()) != ESP_OK) {
				/* not kept yet: a held restart now would roll it back. Try
				 * again on the next pass */
				ESP_LOGE(TAG, "firmware %s passed its trial but couldn't be marked good: %s", HIFI_FW_VERSION,
					 esp_err_to_name(valid));
			} else if (v == HIFI_TRIAL_GOOD) {
				note_good_slot();
				on_trial = false;
				ESP_LOGI(TAG, "firmware %s kept: %s", HIFI_FW_VERSION, why);
				publish_event("ota_valid", "why", why, NULL, NULL);
				publish_status();
				const char *held = NULL;
				portENTER_CRITICAL(&restart_mux);
				bool go = hifi_restart_release(&restart_hold, &held);
				portEXIT_CRITICAL(&restart_mux);
				if (go) {
					/* AirPlay stayed bound to the old address until now */
					ESP_LOGW(TAG, "restarting for the held %s", held);
					publish_event("restart", "why", held, NULL, NULL);
					vTaskDelay(pdMS_TO_TICKS(500));
					/* the good slot, and any setting changed meanwhile, as
					 * upstream's own restart does */
					if (!wait_for_commit()) ESP_LOGW(TAG, "settings not committed before the restart");
					hifi_restart_note(held);
					esp_restart();
				}
			} else if (v == HIFI_TRIAL_BAD) {
				ESP_LOGE(TAG, "firmware %s failed its trial: %s; rolling back", HIFI_FW_VERSION, why);
				config_set_value(NVS_TYPE_STR, "hifi_rb_from", HIFI_FW_VERSION);
				config_set_value(NVS_TYPE_STR, "hifi_rb_why", why);
				config_set_value(NVS_TYPE_STR, "hifi_rb_report", "1");
				publish_event("ota_failed", "why", why, NULL, NULL);
				vTaskDelay(pdMS_TO_TICKS(500));
				esp_ota_mark_app_invalid_rollback_and_reboot();
			}
		}
	}
}

/* this build's number (firmware/tools/build.sh: seconds since 1970 at the
 * build), also kept as text in the image, where firmware/tools/sign_fw.py
 * reads it into the signature; 0: none, and the LAN can't update it (L9) */
#ifndef HIFI_FW_BUILD
#define HIFI_FW_BUILD 0
#endif
#define HIFI_XSTR_(x) #x
#define HIFI_XSTR(x) HIFI_XSTR_(x)
__attribute__((used)) const char hifi_fw_build_tag[] = HIFI_BUILD_TAG_PREFIX HIFI_XSTR(HIFI_FW_BUILD) ";";

void hifi_remote_start(const hifi_ops_t *o) {
	ops = o;
	uint8_t mac[6];
	esp_read_mac(mac, ESP_MAC_WIFI_STA);
	hifi_remote_dev_id(mac, rem.dev);
	if (sodium_init() < 0) ESP_LOGE(TAG, "libsodium didn't start");
	rem.verify = verify;
	if (!hifi_remote_set_keys(&rem, HIFI_HIVE_PUBKEY)) ESP_LOGW(TAG, "no Hive key in this build: relay commands are refused");

	load_settings();

	/* read once, before anything can mark the image good (hifi_boot.h) */
	esp_ota_img_states_t st;
	restart_hold.on_trial = esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK &&
				st == ESP_OTA_IMG_PENDING_VERIFY;

	const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
	ota_io.slot_size = next ? next->size : 0;
	ota_mux = xSemaphoreCreateMutex();
	if (!ota_mux) ota_io.lock = ota_io.unlock = NULL; /* out of memory at boot: unlocked, as before */
	hifi_ota_init(&hifi_ota_session, &ota_io);
	/* the same keys sign the images the LAN may install (security.md) */
	hifi_ota_set_keys(&hifi_ota_session, (const uint8_t (*)[32]) rem.keys, rem.nkeys, verify);
	/* and the LAN may not put back an older build than this one */
	const esp_app_desc_t *app = esp_ota_get_app_description();
	unsigned long long build = hifi_ota_build_of(hifi_fw_build_tag);
	if (!build) ESP_LOGW(TAG, "no build number in this build: updates from the LAN are refused");
	hifi_ota_set_running(&hifi_ota_session, app->project_name, build);

	inbox = xQueueCreate(INBOX_N, sizeof(msg_t));
	xTaskCreate(worker, "hifi_cmd", 6144, NULL, ESP_TASK_PRIO_MIN + 2, NULL);
	/* 5 KB: it parses JSON, and 4 KB left only 708 B unused on a bench run */
	xTaskCreate(relay_task, "hifi_relay", 5120, NULL, ESP_TASK_PRIO_MIN + 1, NULL);
}
