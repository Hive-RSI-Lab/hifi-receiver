/*
 * The remote channel on the board (docs/remote.md): an MQTT client that
 * dials out to the relay, takes signed commands through core/hifi_remote.c,
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
#include "freertos/task.h"
#include "mqtt_client.h"
#include "sodium.h"

#include "platform_config.h"

#include "hifi_board.h"
#include "hifi_esp.h"
#include "hifi_json.h"
#include "hifi_remote.h"

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
static char url[96], user[48], pass[96], prefix[24] = "hifi";
static char topic_cmd[64], topic_reply[64], topic_status[64], topic_event[64];
static QueueHandle_t inbox;
static char *rx;      /* a message arriving in pieces */
static size_t rx_len;
static bool rollback_reported;
static volatile bool settings_changed;

hifi_ota_t hifi_ota_session;

/* ------------------------------------------------------------------ settings */

static void load_str(const char *key, const char *dflt, char *out, size_t n) {
	char *s = config_alloc_get_str(key, NULL, (char *) dflt); /* it only reads the default */
	snprintf(out, n, "%s", s ? s : dflt);
	free(s);
}

int hifi_remote_set(const char *u, const char *us, const char *pw, const char *pre) {
	int err = 0;
	err |= config_set_value(NVS_TYPE_STR, "hifi_relay_url", u) != ESP_OK;
	err |= config_set_value(NVS_TYPE_STR, "hifi_relay_user", us) != ESP_OK;
	err |= config_set_value(NVS_TYPE_STR, "hifi_relay_pass", pw) != ESP_OK;
	err |= config_set_value(NVS_TYPE_STR, "hifi_relay_prefix", pre) != ESP_OK;
	if (err) return -1;
	settings_changed = true; /* the relay task reconnects with them */
	return 0;
}

static void load_settings(void) {
	load_str("hifi_relay_url", HIFI_RELAY_URL, url, sizeof(url));
	load_str("hifi_relay_user", HIFI_RELAY_USER, user, sizeof(user));
	load_str("hifi_relay_pass", HIFI_RELAY_PASS, pass, sizeof(pass));
	load_str("hifi_relay_prefix", "hifi", prefix, sizeof(prefix));
	snprintf(topic_cmd, sizeof(topic_cmd), "%s/%s/cmd", prefix, rem.dev);
	snprintf(topic_reply, sizeof(topic_reply), "%s/%s/reply", prefix, rem.dev);
	snprintf(topic_status, sizeof(topic_status), "%s/%s/status", prefix, rem.dev);
	snprintf(topic_event, sizeof(topic_event), "%s/%s/event", prefix, rem.dev);
}

void hifi_remote_status(hifi_remote_status_t *r) {
	r->configured = url[0] != 0;
	r->connected = connected;
	snprintf(r->url, sizeof(r->url), "%s", url);
	snprintf(r->user, sizeof(r->user), "%s", user);
	snprintf(r->prefix, sizeof(r->prefix), "%s", prefix);
	snprintf(r->dev, sizeof(r->dev), "%s", rem.dev);
	r->keys = rem.nkeys;
	r->accepted = rem.accepted;
	r->rejected = rem.rejected;
}

/* ------------------------------------------------------------------ publishing */

static void publish(const char *topic, const char *data, bool retain) {
	if (client && connected) esp_mqtt_client_publish(client, topic, data, 0, 1, retain);
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
	publish(topic_status, buf, true);
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
	publish(topic_event, buf, false);
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

static void worker(void *arg) {
	msg_t m;
	for (;;) {
		if (xQueueReceive(inbox, &m, portMAX_DELAY) != pdTRUE) continue;
		size_t cap = hifi_remote_reply_cap(m.data, m.len);
		char *out = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
		if (!out) out = malloc(cap);
		if (out) {
			int st = hifi_remote_handle(&rem, ops, m.data, m.len, clock_s(), out, cap);
			publish(topic_reply, out, false);
			if (st != 200) ESP_LOGW(TAG, "command answered %d", st);
			free(out);
		}
		free(m.data);
	}
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
	esp_mqtt_event_handle_t e = data;
	switch ((esp_mqtt_event_id_t) id) {
	case MQTT_EVENT_CONNECTED:
		connected = true;
		ESP_LOGI(TAG, "relay connected: %s as %s", url, rem.dev);
		esp_mqtt_client_subscribe(client, topic_cmd, 1);
		publish_status();
		break;
	case MQTT_EVENT_DISCONNECTED:
		connected = false;
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
			msg_t m = {.data = rx, .len = rx_len};
			if (xQueueSend(inbox, &m, 0) != pdTRUE) free(rx);
			rx = NULL;
		}
		break;
	default:
		break;
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

static int f_open(void *ctx, size_t size, const char **slot, const char **err) {
	ota_part = esp_ota_get_next_update_partition(NULL);
	if (!ota_part) return *err = "no app slot to write", -1;
	if (esp_ota_begin(ota_part, size, &ota_handle) != ESP_OK) return *err = "couldn't open the app slot", -1;
	*slot = ota_part->label;
	return 0;
}

static int f_write(void *ctx, const uint8_t *d, size_t n) { return esp_ota_write(ota_handle, d, n) == ESP_OK ? 0 : -1; }

static int f_commit(void *ctx, const char **err) {
	if (esp_ota_end(ota_handle) != ESP_OK) return *err = "image didn't validate", -1;
	if (esp_ota_set_boot_partition(ota_part) != ESP_OK) return *err = "couldn't set the boot slot", -1;
	return 0;
}

static void f_abort(void *ctx) { esp_ota_abort(ota_handle); }

static hifi_ota_io_t ota_io = {.open = f_open, .write = f_write, .commit = f_commit, .abort = f_abort};

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

/* ------------------------------------------------------------------ the relay task */

static void relay_task(void *arg) {
	bool sntp = false, on_trial = false;
	uint32_t trial_start = 0, last_status = 0;
	esp_ota_img_states_t st;
	if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
		on_trial = true;
		ESP_LOGW(TAG, "firmware %s is on trial: it has %d s to prove itself", HIFI_FW_VERSION, HIFI_TRIAL_LIMIT_S);
	}
	char pending[8];
	load_str("hifi_rb_report", "0", pending, sizeof(pending));
	rollback_reported = strcmp(pending, "1") != 0;

	for (;;) {
		vTaskDelay(pdMS_TO_TICKS(2000));
		uint32_t now = esp_timer_get_time() / 1000000;
		bool up = hifi_net_ip(NULL, 0);

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
			load_settings();
		}
		if (up && url[0] && !client) client_start();

		if (connected && !rollback_reported) {
			hifi_ota_status_t o = {0};
			hifi_ota_status(&o);
			publish_event("rolled_back", "from", o.last_from, "reason", o.last_reason);
			config_set_value(NVS_TYPE_STR, "hifi_rb_report", "0");
			rollback_reported = true;
		}
		if (connected && now - last_status >= 60) {
			publish_status();
			last_status = now;
		}

		if (on_trial) {
			if (!trial_start) trial_start = now ? now : 1;
			hifi_trial_t t = {0};
			hifi_main_trial_inputs(&t);
			t.relay_configured = url[0] != 0;
			t.relay_ok = connected;
			char why[96];
			hifi_trial_verdict_t v = hifi_trial_judge(&t, now - trial_start, why, sizeof(why));
			if (v == HIFI_TRIAL_GOOD) {
				esp_ota_mark_app_valid_cancel_rollback();
				on_trial = false;
				ESP_LOGI(TAG, "firmware %s kept: %s", HIFI_FW_VERSION, why);
				publish_event("ota_valid", "why", why, NULL, NULL);
				publish_status();
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

void hifi_remote_start(const hifi_ops_t *o) {
	ops = o;
	uint8_t mac[6];
	esp_read_mac(mac, ESP_MAC_WIFI_STA);
	hifi_remote_dev_id(mac, rem.dev);
	if (sodium_init() < 0) ESP_LOGE(TAG, "libsodium didn't start");
	rem.verify = verify;
	if (!hifi_remote_set_keys(&rem, HIFI_HIVE_PUBKEY)) ESP_LOGW(TAG, "no Hive key in this build: relay commands are refused");

	load_settings();

	const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
	ota_io.slot_size = next ? next->size : 0;
	hifi_ota_init(&hifi_ota_session, &ota_io);

	inbox = xQueueCreate(4, sizeof(msg_t));
	xTaskCreate(worker, "hifi_cmd", 6144, NULL, ESP_TASK_PRIO_MIN + 2, NULL);
	xTaskCreate(relay_task, "hifi_relay", 4096, NULL, ESP_TASK_PRIO_MIN + 1, NULL);
}
