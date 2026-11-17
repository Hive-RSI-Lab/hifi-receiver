/*
 * Lab experiment "bt": Bluetooth out of the radio's way for a while, to see
 * what it costs the Wi-Fi side (ESP-NOW above all) on a board that has it.
 *
 *   api lab "{\"op\":\"start\",\"exp\":\"bt\",\"mode\":\"off\",\"max_s\":120}"
 *   api lab "{\"op\":\"start\",\"exp\":\"bt\",\"mode\":\"quiet\",\"max_s\":120}"
 *
 * mode "off": Bluedroid and then the controller are disabled (as the sink's
 * own shutdown does, with its 400 ms between them), so the radio is Wi-Fi's
 * alone and modem sleep may be turned off (the espnow experiment's ps none).
 * The stop enables them again and makes the board connectable and
 * discoverable as it is when idle, but the A2DP sink isn't set up again:
 * restart the board before playing over Bluetooth. A phone that is connected
 * is dropped. Not on the BLE lab image: lab_ble's GATT apps can't survive a
 * Bluedroid restart.
 *
 * mode "quiet": Bluetooth stays up, but neither connectable nor discoverable
 * (no page or inquiry scans) and the coexistence prefers Wi-Fi: what a
 * product image could do while nothing is paired. The stop puts both back
 * (connectable, discoverable, balanced). It is for a board with no phone
 * on it: refused while Bluetooth plays. The sink keeps no "connected" state
 * the lab can read, so a phone connected but not playing at the stop leaves
 * the board discoverable until it drops (the sink hides it on a connect).
 *
 * Nothing is sent or received; the run lasts until stopped or its deadline.
 */
#include <string.h>

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_coexist.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "hifi_json.h"
#include "hifi_lab_esp.h"

static const char *TAG = "lab_bt";

/* Weak: the recovery image links this file too (same component library, only
 * app_recovery differs) but has no Bluetooth host, and a strong reference
 * here pulled all of Bluedroid (~220 KB of libbt) into it, past its slot.
 * The app has them from the A2DP sink; recovery sees NULL and the
 * experiment refuses to start. */
#pragma weak esp_bluedroid_get_status
#pragma weak esp_bluedroid_enable
#pragma weak esp_bluedroid_disable
#pragma weak esp_bt_gap_set_scan_mode

static esp_bluedroid_status_t bd_status(void) {
	return esp_bluedroid_get_status ? esp_bluedroid_get_status() : ESP_BLUEDROID_STATUS_UNINITIALIZED;
}

/* the mode the controller was set up in (bt_app_core.c, patch 0012):
 * enabling it in another one fails */
#if CONFIG_BTDM_CTRL_MODE_BTDM
#define CTRL_MODE ESP_BT_MODE_BTDM
#else
#define CTRL_MODE ESP_BT_MODE_CLASSIC_BT
#endif

extern int hifi_sink_source(void) __attribute__((weak)); /* 2: Bluetooth plays (decode_external.c) */
extern void hifi_bt_sink_acl_reset(void) __attribute__((weak)); /* the sink's ACL table (patch 0018) */

typedef enum { MODE_OFF, MODE_QUIET } mode_t_;

static mode_t_ mode;
static bool started, active;
static char err_start[48], err_stop[48];
static const char *ps_fix; /* the stop turned modem sleep on before enabling the controller */
/* how long going off, staying off and coming back took (ms): the cost of a
 * short controller-off window */
static int64_t t_off, t_stop;
static uint32_t off_ms, down_ms, on_ms;

static const char *status_str(void) {
	switch (esp_bt_controller_get_status()) {
	case ESP_BT_CONTROLLER_STATUS_IDLE: return "idle";
	case ESP_BT_CONTROLLER_STATUS_INITED: return "inited";
	case ESP_BT_CONTROLLER_STATUS_ENABLED: return "enabled";
	default: return "?";
	}
}

static const char *bd_str(void) {
	switch (bd_status()) {
	case ESP_BLUEDROID_STATUS_UNINITIALIZED: return "uninitialized";
	case ESP_BLUEDROID_STATUS_INITIALIZED: return "initialized";
	case ESP_BLUEDROID_STATUS_ENABLED: return "enabled";
	default: return "?";
	}
}

static void note(char *buf, size_t n, const char *what, esp_err_t e) {
	if (!buf[0]) snprintf(buf, n, "%s: %s", what, esp_err_to_name(e));
	ESP_LOGW(TAG, "%s: %s", what, esp_err_to_name(e));
}

/* lab_a2dp refuses its source while this runs */
bool hifi_lab_bt_busy(void) { return active; }

static int start(uint32_t run, const char *body, char *err, size_t n) {
	(void)run;
	if (active) {
		snprintf(err, n, "already %s", mode == MODE_OFF ? "off" : "quiet");
		return 409;
	}
	if (hifi_lab_a2dp_busy && hifi_lab_a2dp_busy()) {
		snprintf(err, n, "the a2dp experiment's source runs (or its teardown); stop it first");
		return 409;
	}
	/* the last run's result stays as it was until this start succeeds */
	char m[8] = "";
	mode_t_ want;
	char why[sizeof(err_start)] = "";
	hj_get_str(body, "mode", m, sizeof(m));
	if (!strcmp(m, "off")) want = MODE_OFF;
	else if (!strcmp(m, "quiet")) want = MODE_QUIET;
	else {
		snprintf(err, n, "mode must be off or quiet");
		return 400;
	}
	if (!esp_bluedroid_enable || !esp_bluedroid_disable || !esp_bt_gap_set_scan_mode) {
		snprintf(err, n, "no Bluetooth host in this image (recovery)");
		return 503;
	}
	if (esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_ENABLED ||
	    bd_status() != ESP_BLUEDROID_STATUS_ENABLED) {
		snprintf(err, n, "Bluetooth isn't up (controller %s, Bluedroid %s)", status_str(), bd_str());
		return 409;
	}
	esp_err_t e;
	int64_t t0 = esp_timer_get_time();
	if (want == MODE_OFF) {
#if CONFIG_BT_BLE_ENABLED
		snprintf(err, n, "not on the BLE image: lab_ble's GATT apps don't survive a Bluedroid restart");
		return 409;
#else
		if ((e = esp_bluedroid_disable()) != ESP_OK) {
			snprintf(err, n, "esp_bluedroid_disable: %s", esp_err_to_name(e));
			return 503;
		}
		/* the disable dropped every ACL link without a disconnect event */
		if (hifi_bt_sink_acl_reset) hifi_bt_sink_acl_reset();
		vTaskDelay(pdMS_TO_TICKS(400)); /* the sink's shutdown waits too: the controller crashes otherwise */
		if ((e = esp_bt_controller_disable()) != ESP_OK) {
			note(why, sizeof(why), "esp_bt_controller_disable", e);
			/* Bluedroid is down already: the stop brings it back */
		}
#endif
	} else {
		if (hifi_sink_source && hifi_sink_source() == 2) {
			snprintf(err, n, "Bluetooth is playing: quiet is for a board with no phone on it");
			return 409;
		}
		if ((e = esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE)) != ESP_OK) {
			snprintf(err, n, "esp_bt_gap_set_scan_mode: %s", esp_err_to_name(e));
			return 503;
		}
		if ((e = esp_coex_preference_set(ESP_COEX_PREFER_WIFI)) != ESP_OK)
			note(why, sizeof(why), "esp_coex_preference_set", e);
	}
	mode = want;
	t_off = esp_timer_get_time();
	off_ms = (uint32_t) ((t_off - t0) / 1000);
	down_ms = on_ms = 0;
	strlcpy(err_start, why, sizeof(err_start));
	err_stop[0] = 0;
	ps_fix = NULL;
	started = active = true;
	ESP_LOGI(TAG, "Bluetooth %s (controller %s)", mode == MODE_OFF ? "off" : "quiet", status_str());
	return 0;
}

static bool stop(void) {
	if (!active) return true;
	esp_err_t e;
	t_stop = esp_timer_get_time();
	down_ms = (uint32_t) ((t_stop - t_off) / 1000);
	if (mode == MODE_OFF) {
		if (esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_ENABLED) {
			/* the coexistence aborts with the controller on and modem sleep
			 * off (lab_espnow.c): turn it on first */
			wifi_ps_type_t ps;
			bool ps_ok = true;
			if (esp_wifi_get_ps(&ps) != ESP_OK || ps == WIFI_PS_NONE) {
				if ((e = esp_wifi_set_ps(WIFI_PS_MIN_MODEM)) == ESP_OK)
					ps_fix = "min";
				else {
					ps_ok = false;
					note(err_stop, sizeof(err_stop), "esp_wifi_set_ps min (Bluetooth left off)", e);
				}
			}
			if (ps_ok && (e = esp_bt_controller_enable(CTRL_MODE)) != ESP_OK)
				note(err_stop, sizeof(err_stop), "esp_bt_controller_enable", e);
		}
		/* Bluedroid on a controller that is down would wait on HCI for ever,
		 * holding the lab's lock */
		if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED &&
		    bd_status() != ESP_BLUEDROID_STATUS_ENABLED && (e = esp_bluedroid_enable()) != ESP_OK)
			note(err_stop, sizeof(err_stop), "esp_bluedroid_enable", e);
	} else if ((e = esp_coex_preference_set(ESP_COEX_PREFER_BALANCE)) != ESP_OK)
		note(err_stop, sizeof(err_stop), "esp_coex_preference_set", e);
	if (bd_status() == ESP_BLUEDROID_STATUS_ENABLED && !(hifi_sink_source && hifi_sink_source() == 2) &&
	    (e = esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE)) != ESP_OK)
		note(err_stop, sizeof(err_stop), "esp_bt_gap_set_scan_mode", e);
	on_ms = (uint32_t) ((esp_timer_get_time() - t_stop) / 1000);
	active = false;
	ESP_LOGI(TAG, "Bluetooth back (controller %s, Bluedroid %s)", status_str(), bd_str());
	return true;
}

static void result(void *hj) {
	hj_t *j = hj;
	if (!started) return;
	hj_str(j, "mode", mode == MODE_OFF ? "off" : "quiet");
	hj_bool(j, "active", active);
	hj_str(j, "controller", status_str());
	hj_str(j, "bluedroid", bd_str());
	if (err_start[0]) hj_str(j, "start_error", err_start);
	if (err_stop[0]) hj_str(j, "stop_error", err_stop);
	if (ps_fix) hj_str(j, "ps_set_on_stop", ps_fix);
	hj_int(j, "off_ms", off_ms);
	if (!active) {
		hj_int(j, "down_ms", down_ms);
		hj_int(j, "on_ms", on_ms);
	}
}

const hifi_lab_exp_t lab_bt = {
	.name = "bt",
	.help = "Bluetooth out of the way: mode off (controller and Bluedroid disabled; restart for A2DP after) or quiet (no scans, coex prefers Wi-Fi)",
	.start = start,
	.stop = stop,
	.result = result,
};
