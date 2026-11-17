/*
 * Lab experiment "ble": a BLE GATT link to another board, beside the Wi-Fi
 * station and Classic Bluetooth (the A2DP sink), on the BLE lab image
 * (board/lite-lab-ble: the controller in dual mode, Bluedroid's GATT server
 * and client, no Spotify).
 *
 *   api lab "{\"op\":\"start\",\"exp\":\"ble\",\"role\":\"echo\",\"peer\":\"3076f52d742c\",\"max_s\":120}"
 *   api lab "{\"op\":\"start\",\"exp\":\"ble\",\"role\":\"ping\",\"peer\":\"3076f52d73e0\",\"rate_hz\":20}"
 *   api lab "{\"op\":\"result\",\"exp\":\"ble\"}"
 *
 * Roles: echo is the GATT server (run it on the far board first): it
 * advertises while it has no link, answers PINGs with a notification and
 * counts DATA. ping and flood are the GATT client: they scan for the echo
 * board's advertisement, connect, ask for a 247-byte MTU, find the lab
 * characteristic, turn notifications on, then ping (rate_hz PINGs of len
 * bytes written without response, round trip from the notified PONG, clock
 * offset and drift) or flood (DATA written without response as fast as the
 * stack takes it, the echo end has the count and throughput).
 * Optional: count (frames, then the run ends itself), len (38 to the MTU's
 * payload, at most 244), interval_ms (the connection interval the client
 * asks for, 7.5-100; the default is the stack's), scan_s (client, 3-60,
 * default 20).
 *
 * Who may connect: the frames are the same hifi_link frames as ESP-NOW's.
 * The echo board advertises only during a run and only answers the board
 * named in peer: its BLE address is worked out from the peer's Wi-Fi MAC
 * with the same offset this board's own two addresses have (one ESP32
 * family, one rule). Anything else that connects is dropped at once and
 * counted (rejected). No pairing, no bonding, nothing kept.
 *
 * The stop (asked, deadline, low RAM, the link lost or the run's own end)
 * stops scanning and advertising and closes the link. The GATT apps, the
 * service and the callbacks are registered once and kept: Bluedroid's
 * unregister path is not needed for a lab, and a late callback never finds
 * freed memory.
 */
#include <stdlib.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gattc_api.h"
#include "esp_gatts_api.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "hifi_json.h"
#include "hifi_lab_esp.h"
#include "hifi_link.h"

static const char *TAG = "lab_ble";

typedef enum { ROLE_ECHO, ROLE_PING, ROLE_FLOOD } role_t;
static const char *const ROLE_NAMES[] = {"echo", "ping", "flood"};

/* how far the client got (the echo: advertise until its peer connects, ready
 * while linked); the result names where the last run ended */
typedef enum { PH_IDLE, PH_SCAN, PH_OPEN, PH_MTU, PH_FIND, PH_SUBSCRIBE, PH_READY, PH_ADVERTISE } phase_t;
static const char *const PHASE_NAMES[] = {"idle", "scan", "open", "mtu", "find", "subscribe", "ready", "advertise"};

#define LAB_MTU 247
#define PAYLOAD_MAX (LAB_MTU - 3) /* a write or notification's value */

typedef enum { EV_FRAME, EV_READY, EV_LOST, EV_FAIL } ev_t;

typedef struct {
	uint8_t ev;
	uint16_t len;
	int64_t t_us;
	uint8_t data[PAYLOAD_MAX];
} rx_item_t;

#define QLEN 32
#define STACK 4096
#define APP_SERVER 0x4c41
#define APP_CLIENT 0x4c42
#define MFR_TAG_LEN 10 /* company 0xffff (none, for tests), "hl", the board's Wi-Fi MAC */

/* the lab's service and characteristic (random 128-bit UUIDs, little-endian) */
static const uint8_t SVC_UUID[16] = {0x6b, 0x1a, 0x5e, 0x2f, 0x91, 0x3c, 0x4d, 0x8a,
				     0xb2, 0x07, 0x5f, 0xe1, 0x00, 0x4c, 0x61, 0x62};
static const uint8_t CHR_UUID[16] = {0x6b, 0x1a, 0x5e, 0x2f, 0x91, 0x3c, 0x4d, 0x8a,
				     0xb2, 0x07, 0x5f, 0xe1, 0x01, 0x4c, 0x61, 0x62};

/* kept for the life of the board: see the top */
static QueueHandle_t q;
static StaticQueue_t q_ctl;
static SemaphoreHandle_t reg_sem, space_sem, closed_sem;
static TaskHandle_t worker;
static StaticTask_t worker_tcb;
static hifi_link_tx_t *tx;
static hifi_link_rx_t *rx;
static rx_item_t *cb_it; /* the callbacks' item (they all run on Bluedroid's task) */
static bool cbs_registered;
static esp_gatt_if_t gatts_if = ESP_GATT_IF_NONE, gattc_if = ESP_GATT_IF_NONE;
static uint16_t h_svc, h_val, h_cccd; /* the server's handles */

static volatile bool run, alive, connected, notify_on, congested;
static volatile bool opening; /* a connect asked for and not yet answered (OPEN_EVT) */
static volatile phase_t phase, phase_end; /* now; where the last run ended */
static role_t role;
static uint8_t peer[6], peer_bt[6], self[6], self_bt[6], remote[6];
static uint16_t conn_id, c_start, c_end, c_val, c_cccd; /* the client's handles */
static volatile uint16_t mtu = 23, mtu_link; /* now; the highest the run's links reached (for the result) */
static uint32_t rate_hz, len, count, interval_ms, scan_s;
static volatile uint16_t ci_units, ci_latency, ci_timeout; /* the link's interval (1.25 ms), latency, timeout (10 ms) */
static uint32_t run_no;
static char fail[48];
static uint32_t heap_start, heap_min;
static int64_t t_link_us, t_start_us;
static volatile uint32_t q_full, other, rejected, seen_adv, connects, disconnects, congest_events, send_fail_busy;
static volatile int last_disc_reason;

static uint32_t heap_now(void) { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }

static void mac_str(const uint8_t *m, char *s, size_t n) {
	snprintf(s, n, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

static uint64_t mac_u64(const uint8_t *m) {
	uint64_t v = 0;
	for (int i = 0; i < 6; i++) v = v << 8 | m[i];
	return v;
}

static void u64_mac(uint64_t v, uint8_t *m) {
	for (int i = 5; i >= 0; i--, v >>= 8) m[i] = (uint8_t) v;
}

/* on Bluedroid's task: one item, made once */
static void post(ev_t ev, const uint8_t *data, size_t n) {
	rx_item_t *it = cb_it;
	it->ev = ev;
	it->t_us = esp_timer_get_time();
	it->len = (uint16_t) hifi_link_take(it->data, sizeof(it->data), data, n);
	if (xQueueSend(q, it, 0) != pdTRUE) q_full++;
}

static void failed(const char *why) {
	if (!run) return; /* a late answer after stop */
	if (!fail[0]) strlcpy(fail, why, sizeof(fail));
	post(EV_FAIL, NULL, 0);
}

/* ------------------------------------------------------------- advertising */

static esp_ble_adv_params_t adv_params = {
	.adv_int_min = 0x40, /* 40 ms */
	.adv_int_max = 0x60, /* 60 ms */
	.adv_type = ADV_TYPE_IND,
	.own_addr_type = BLE_ADDR_TYPE_PUBLIC,
	.channel_map = ADV_CHNL_ALL,
	.adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static bool advertise(void) {
	/* flags and the lab's mark with this board's Wi-Fi MAC: 15 of the 31
	 * bytes an advertisement holds (the 128-bit service UUID would take 18
	 * more; the client finds the service by discovery instead) */
	uint8_t raw[3 + 2 + MFR_TAG_LEN], *p = raw;
	*p++ = 2, *p++ = ESP_BLE_AD_TYPE_FLAG, *p++ = ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT;
	*p++ = 1 + MFR_TAG_LEN, *p++ = ESP_BLE_AD_MANUFACTURER_SPECIFIC_TYPE;
	*p++ = 0xff, *p++ = 0xff, *p++ = 'h', *p++ = 'l';
	memcpy(p, self, 6), p += 6;
	_Static_assert(sizeof(raw) <= ESP_BLE_ADV_DATA_LEN_MAX, "an advertisement holds 31 bytes");
	/* advertising starts when the data is set (gap_cb) */
	return esp_ble_gap_config_adv_data_raw(raw, p - raw) == ESP_OK;
}

static bool adv_is_peer(uint8_t *adv, uint8_t n) {
	uint8_t l = 0;
	uint8_t *m = esp_ble_resolve_adv_data(adv, ESP_BLE_AD_MANUFACTURER_SPECIFIC_TYPE, &l);
	return m && l == MFR_TAG_LEN && m[0] == 0xff && m[1] == 0xff && m[2] == 'h' && m[3] == 'l' && !memcmp(m + 4, peer, 6);
}

static void gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *p) {
	switch (event) {
	case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
		if (run && role == ROLE_ECHO && !connected) esp_ble_gap_start_advertising(&adv_params);
		break;
	case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
		if (p->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) failed("advertising didn't start");
		/* a start queued just before teardown's stop: stop it again */
		else if (!run || role != ROLE_ECHO || connected) esp_ble_gap_stop_advertising();
		break;
	case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
		if (run && phase == PH_SCAN) esp_ble_gap_start_scanning(scan_s);
		break;
	case ESP_GAP_BLE_SCAN_RESULT_EVT:
		if (!run || phase != PH_SCAN) break;
		if (p->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
			seen_adv++;
			if (!adv_is_peer(p->scan_rst.ble_adv, p->scan_rst.adv_data_len)) break;
			phase = PH_OPEN;
			esp_ble_gap_stop_scanning();
			memcpy(remote, p->scan_rst.bda, 6);
			opening = true;
			if (esp_ble_gattc_open(gattc_if, p->scan_rst.bda, p->scan_rst.ble_addr_type, true) != ESP_OK) {
				opening = false;
				failed("connect refused");
			}
		} else if (p->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT)
			failed("the peer wasn't advertising");
		break;
	case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
		if (p->update_conn_params.status == ESP_BT_STATUS_SUCCESS && connected &&
		    !memcmp(p->update_conn_params.bda, remote, 6)) {
			ci_units = p->update_conn_params.conn_int;
			ci_latency = p->update_conn_params.latency;
			ci_timeout = p->update_conn_params.timeout;
		}
		break;
	default:
		break;
	}
}

/* ------------------------------------------------------------------ server */

enum { IDX_SVC, IDX_CHR, IDX_VAL, IDX_CCCD, IDX_N };
static const uint16_t UUID_PRI = ESP_GATT_UUID_PRI_SERVICE, UUID_DECL = ESP_GATT_UUID_CHAR_DECLARE,
		      UUID_CCCD = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
static const uint8_t CHR_PROP = ESP_GATT_CHAR_PROP_BIT_WRITE_NR | ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_NOTIFY;
static uint8_t cccd_val[2];
static const esp_gatts_attr_db_t DB[IDX_N] = {
	[IDX_SVC] = {{ESP_GATT_AUTO_RSP},
		     {ESP_UUID_LEN_16, (uint8_t *) &UUID_PRI, ESP_GATT_PERM_READ, 16, 16, (uint8_t *) SVC_UUID}},
	[IDX_CHR] = {{ESP_GATT_AUTO_RSP},
		     {ESP_UUID_LEN_16, (uint8_t *) &UUID_DECL, ESP_GATT_PERM_READ, 1, 1, (uint8_t *) &CHR_PROP}},
	[IDX_VAL] = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_128, (uint8_t *) CHR_UUID, ESP_GATT_PERM_WRITE, PAYLOAD_MAX, 0, NULL}},
	[IDX_CCCD] = {{ESP_GATT_AUTO_RSP},
		      {ESP_UUID_LEN_16, (uint8_t *) &UUID_CCCD, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, 2, 2, cccd_val}},
};

static void gatts_cb(esp_gatts_cb_event_t event, esp_gatt_if_t gif, esp_ble_gatts_cb_param_t *p) {
	switch (event) {
	case ESP_GATTS_REG_EVT:
		if (p->reg.app_id != APP_SERVER) break;
		if (p->reg.status != ESP_GATT_OK) {
			xSemaphoreGive(reg_sem);
			break;
		}
		gatts_if = gif;
		if (esp_ble_gatts_create_attr_tab(DB, gif, IDX_N, 0) != ESP_OK) xSemaphoreGive(reg_sem);
		break;
	case ESP_GATTS_CREAT_ATTR_TAB_EVT:
		if (p->add_attr_tab.status == ESP_GATT_OK && p->add_attr_tab.num_handle == IDX_N) {
			h_svc = p->add_attr_tab.handles[IDX_SVC];
			h_val = p->add_attr_tab.handles[IDX_VAL];
			h_cccd = p->add_attr_tab.handles[IDX_CCCD];
			esp_ble_gatts_start_service(h_svc);
		}
		xSemaphoreGive(reg_sem);
		break;
	/* Bluedroid tells every registered app about every link: on the client
	 * board the server sees the client's own link come and go, so the
	 * server's link events are the echo's alone */
	case ESP_GATTS_CONNECT_EVT:
		if (gif != gatts_if || role != ROLE_ECHO) break;
		if (!run || connected || memcmp(p->connect.remote_bda, peer_bt, 6)) {
			rejected++; /* remote stays the peer's: the rejected one's goes nowhere */
			esp_ble_gap_disconnect(p->connect.remote_bda);
			break;
		}
		conn_id = p->connect.conn_id;
		memcpy(remote, p->connect.remote_bda, 6);
		ci_units = p->connect.conn_params.interval;
		ci_latency = p->connect.conn_params.latency;
		ci_timeout = p->connect.conn_params.timeout;
		connected = true;
		notify_on = false;
		phase = PH_READY;
		connects++;
		t_link_us = esp_timer_get_time();
		break;
	case ESP_GATTS_DISCONNECT_EVT:
		if (gif != gatts_if || role != ROLE_ECHO) break;
		if (!connected || p->disconnect.conn_id != conn_id) {
			/* a rejected one: its connect stopped the advertising, so start
			 * it again or the peer can't get in for the rest of the run */
			if (run && !connected && !advertise()) failed("advertising data refused");
			break;
		}
		connected = notify_on = false;
		disconnects++;
		last_disc_reason = p->disconnect.reason;
		mtu = 23;
		xSemaphoreGive(closed_sem);
		if (run) {
			phase = PH_ADVERTISE; /* the client may come back */
			if (!advertise()) failed("advertising data refused");
		}
		break;
	case ESP_GATTS_MTU_EVT:
		if (gif == gatts_if && role == ROLE_ECHO && connected && p->mtu.conn_id == conn_id) {
			mtu = p->mtu.mtu;
			if (mtu > mtu_link) mtu_link = mtu;
		}
		break;
	case ESP_GATTS_WRITE_EVT:
		if (role != ROLE_ECHO || !connected || p->write.conn_id != conn_id) break;
		if (p->write.is_prep) other++; /* a long write: the lab's frames fit the MTU */
		else if (p->write.handle == h_cccd && p->write.len == 2)
			notify_on = p->write.value[0] & 1;
		else if (p->write.handle == h_val && run)
			post(EV_FRAME, p->write.value, p->write.len);
		break;
	case ESP_GATTS_CONGEST_EVT:
		if (role != ROLE_ECHO) break;
		congested = p->congest.congested;
		if (congested) congest_events++;
		else xSemaphoreGive(space_sem);
		break;
	default:
		break;
	}
}

/* ------------------------------------------------------------------ client */

static void gattc_cb(esp_gattc_cb_event_t event, esp_gatt_if_t gif, esp_ble_gattc_cb_param_t *p) {
	if (event == ESP_GATTC_REG_EVT) {
		if (p->reg.app_id == APP_CLIENT) {
			if (p->reg.status == ESP_GATT_OK) gattc_if = gif;
			xSemaphoreGive(reg_sem);
		}
		return;
	}
	/* the client's events are the client roles' alone (see the server's) */
	if (gif != gattc_if || role == ROLE_ECHO) return;
	switch (event) {
	case ESP_GATTC_CONNECT_EVT:
		/* the link's first parameters (an update later comes as a GAP event) */
		if (run && role != ROLE_ECHO && phase == PH_OPEN && !memcmp(p->connect.remote_bda, remote, 6)) {
			ci_units = p->connect.conn_params.interval;
			ci_latency = p->connect.conn_params.latency;
			ci_timeout = p->connect.conn_params.timeout;
		}
		break;
	case ESP_GATTC_OPEN_EVT:
		opening = false;
		xSemaphoreGive(closed_sem); /* teardown may be waiting for this answer */
		if (!run || phase != PH_OPEN) {
			/* a connect that outlived its run: take the link so it is closed and
			 * start waits for it to go */
			if (p->open.status == ESP_GATT_OK) {
				conn_id = p->open.conn_id;
				connected = true;
				esp_ble_gattc_close(gif, conn_id);
			}
			break;
		}
		if (p->open.status != ESP_GATT_OK) {
			failed("connect failed");
			break;
		}
		conn_id = p->open.conn_id;
		connected = true;
		connects++;
		t_link_us = esp_timer_get_time();
		phase = PH_MTU;
		if (interval_ms) {
			esp_ble_conn_update_params_t cp = {0};
			memcpy(cp.bda, p->open.remote_bda, 6);
			cp.min_int = cp.max_int = (uint16_t) (interval_ms * 4 / 5); /* 1.25 ms units */
			cp.latency = 0;
			cp.timeout = 400; /* 4 s */
			esp_ble_gap_update_conn_params(&cp);
		}
		if (esp_ble_gattc_send_mtu_req(gif, conn_id) != ESP_OK) failed("MTU request refused");
		break;
	case ESP_GATTC_CFG_MTU_EVT:
		if (!run || phase != PH_MTU) break;
		if (p->cfg_mtu.status == ESP_GATT_OK) mtu = p->cfg_mtu.mtu;
		if (mtu > mtu_link) mtu_link = mtu;
		phase = PH_FIND; /* a refused MTU leaves 23: the run goes on with short frames, or says len is too big */
		c_start = c_end = 0;
		esp_bt_uuid_t u = {.len = ESP_UUID_LEN_128};
		memcpy(u.uuid.uuid128, SVC_UUID, 16);
		if (esp_ble_gattc_search_service(gif, conn_id, &u) != ESP_OK) failed("service search refused");
		break;
	case ESP_GATTC_SEARCH_RES_EVT:
		if (run && phase == PH_FIND && p->search_res.srvc_id.uuid.len == ESP_UUID_LEN_128 &&
		    !memcmp(p->search_res.srvc_id.uuid.uuid.uuid128, SVC_UUID, 16)) {
			c_start = p->search_res.start_handle;
			c_end = p->search_res.end_handle;
		}
		break;
	case ESP_GATTC_SEARCH_CMPL_EVT: {
		if (!run || phase != PH_FIND) break;
		if (!c_start) {
			failed("no lab service on the peer");
			break;
		}
		esp_gattc_char_elem_t ch;
		uint16_t n = 1;
		esp_bt_uuid_t u = {.len = ESP_UUID_LEN_128};
		memcpy(u.uuid.uuid128, CHR_UUID, 16);
		if (esp_ble_gattc_get_char_by_uuid(gif, conn_id, c_start, c_end, u, &ch, &n) != ESP_GATT_OK || !n) {
			failed("no lab characteristic");
			break;
		}
		c_val = ch.char_handle;
		phase = PH_SUBSCRIBE;
		if (esp_ble_gattc_register_for_notify(gif, remote, c_val) != ESP_OK) failed("notify refused");
		break;
	}
	case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
		if (!run || phase != PH_SUBSCRIBE) break;
		esp_gattc_descr_elem_t d;
		uint16_t n = 1;
		esp_bt_uuid_t u = {.len = ESP_UUID_LEN_16, .uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG};
		if (p->reg_for_notify.status != ESP_GATT_OK ||
		    esp_ble_gattc_get_descr_by_char_handle(gif, conn_id, c_val, u, &d, &n) != ESP_GATT_OK || !n) {
			failed("no notify descriptor");
			break;
		}
		c_cccd = d.handle;
		uint8_t on[2] = {1, 0};
		if (esp_ble_gattc_write_char_descr(gif, conn_id, c_cccd, 2, on, ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE) !=
		    ESP_OK)
			failed("notify write refused");
		break;
	}
	case ESP_GATTC_WRITE_DESCR_EVT:
		if (!run || phase != PH_SUBSCRIBE) break;
		if (p->write.status != ESP_GATT_OK) {
			failed("notify write failed");
			break;
		}
		phase = PH_READY;
		post(EV_READY, NULL, 0);
		break;
	case ESP_GATTC_NOTIFY_EVT:
		if (connected && p->notify.conn_id == conn_id && p->notify.handle == c_val && run)
			post(EV_FRAME, p->notify.value, p->notify.value_len);
		break;
	case ESP_GATTC_CONGEST_EVT:
		congested = p->congest.congested;
		if (congested) congest_events++;
		else xSemaphoreGive(space_sem);
		break;
	case ESP_GATTC_DISCONNECT_EVT:
		if (!connected || p->disconnect.conn_id != conn_id) break;
		connected = false;
		disconnects++;
		last_disc_reason = p->disconnect.reason;
		mtu = 23;
		xSemaphoreGive(closed_sem);
		if (run) {
			if (!fail[0]) snprintf(fail, sizeof(fail), "link lost (reason 0x%02x)", p->disconnect.reason);
			post(EV_LOST, NULL, 0);
		}
		break;
	default:
		break;
	}
}

/* ------------------------------------------------------------------ worker */

static bool send_frame(hifi_link_frame_t *f) {
	uint8_t buf[HIFI_LINK_MAX];
	int64_t t = esp_timer_get_time();
	if (f->type == HIFI_LINK_PING) f->t1 = t;
	else if (f->type == HIFI_LINK_PONG) f->t3 = t;
	size_t n = hifi_link_encode(f, buf, sizeof(buf));
	if (!n || !connected) return false;
	esp_err_t e;
	if (role == ROLE_ECHO) {
		if (!notify_on) return false;
		e = esp_ble_gatts_send_indicate(gatts_if, conn_id, h_val, n, buf, false);
	} else
		e = esp_ble_gattc_write_char(gattc_if, conn_id, c_val, n, buf, ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
	hifi_link_tx_sent(tx, n, e == ESP_OK, t);
	return e == ESP_OK;
}

static void handle(const rx_item_t *it) {
	hifi_link_frame_t pong;
	switch (hifi_link_receive(rx, tx, role == ROLE_ECHO, role != ROLE_ECHO, it->data, it->len, it->t_us, &pong)) {
	case HIFI_LINK_GOT_REPLY:
		send_frame(&pong);
		break;
	case HIFI_LINK_GOT_OTHER:
		other++;
		break;
	default:
		break;
	}
}

/* room in the stack's queue for one more write: not congested and a buffer
 * free on the link. Waits a little; false: none yet */
static bool room(void) {
	if (!congested && esp_ble_get_cur_sendable_packets_num(conn_id) > 0) return true;
	send_fail_busy++;
	/* at least a tick (10 ms at 100 Hz): a spin here starves the idle task */
	xSemaphoreTake(space_sem, congested ? pdMS_TO_TICKS(20) + 1 : 1);
	return false;
}

static void teardown(void) {
	run = false;
	if (role == ROLE_ECHO) {
		esp_ble_gap_stop_advertising();
		if (connected) esp_ble_gap_disconnect(remote);
	} else {
		if (phase == PH_SCAN) esp_ble_gap_stop_scanning();
		if (connected) {
			esp_ble_gattc_close(gattc_if, conn_id);
			esp_ble_gap_disconnect(remote); /* the link too, if another app holds it */
		}
		/* a connect still pending can't be cancelled on IDF 4.4 (no
		 * esp_ble_gattc_cancel_open, and a GAP disconnect needs a link): it
		 * stays pending until CONFIG_BT_BLE_ESTAB_LINK_CONN_TOUT (30 s), start
		 * refuses meanwhile, and a late OPEN is closed (gattc_cb) */
	}
	/* the link down and any connect answered before the next start (2 s;
	 * start refuses while either is left) */
	int64_t until = esp_timer_get_time() + 2000000;
	while ((connected || opening) && esp_timer_get_time() < until) xSemaphoreTake(closed_sem, pdMS_TO_TICKS(100));
	phase_end = phase;
	phase = PH_IDLE;
}

/* the run; true when it ended by itself (the lab frame is told) */
static bool one_run(bool *ok_out) {
	bool ok = true;
	rx_item_t it;
	uint32_t seq = 0;
	int64_t period = 1000000 / (rate_hz ? rate_hz : 1);
	int64_t next = 0, done_at = 0;
	bool ready = role == ROLE_ECHO;
	hifi_link_frame_t f = {0};
	f.session = tx->session;
	while (run) {
		int64_t now = esp_timer_get_time();
		uint32_t h = heap_now();
		if (h < heap_min) heap_min = h;
		if (!ready && now - t_start_us > (int64_t) (scan_s + 15) * 1000000) {
			if (!fail[0]) snprintf(fail, sizeof(fail), "no link in %u s (%s)", (unsigned) scan_s + 15, PHASE_NAMES[phase]);
			ok = false;
			break;
		}
		if (ready && role != ROLE_ECHO && connected && len > (uint32_t) mtu - 3) {
			snprintf(fail, sizeof(fail), "len %u over the link's %u", (unsigned) len, (unsigned) mtu - 3);
			ok = false;
			break;
		}
		if (ready && role == ROLE_FLOOD) {
			/* the frames from the echo end (none expected) still get read */
			while (xQueueReceive(q, &it, 0) == pdTRUE) {
				if (it.ev == EV_FRAME) handle(&it);
				else if (it.ev == EV_LOST || it.ev == EV_FAIL) {
					ok = false;
					break;
				}
			}
			if (!ok || !run) break;
			if (count && seq >= count) {
				if (!done_at) done_at = now + 500000;
				if (now >= done_at) break;
				vTaskDelay(pdMS_TO_TICKS(20));
				continue;
			}
			if (!room()) continue;
			f.type = HIFI_LINK_DATA;
			f.seq = seq;
			f.len = (uint16_t) len;
			if (send_frame(&f)) seq++;
			else vTaskDelay(1); /* refused: never spin */
			continue;
		}
		TickType_t wait = pdMS_TO_TICKS(100);
		if (ready && role == ROLE_PING && !done_at) {
			if (now >= next) {
				if (count && seq >= count)
					done_at = now + 1000000; /* a second for the last pongs */
				else {
					f.type = HIFI_LINK_PING;
					f.seq = seq++;
					f.t2 = f.t3 = 0;
					f.len = (uint16_t) len;
					if (!congested) send_frame(&f);
					else hifi_link_tx_sent(tx, len, false, now); /* skipped, the link backed up: a send_fail, not a loss */
					next += period;
					if (next < now) next = now + period; /* fell behind: don't burst */
				}
				continue;
			}
			wait = (next - now) / 1000 / portTICK_PERIOD_MS;
			if (wait < 1) wait = 1; /* never spin: idle on this core feeds the watchdog */
		}
		if (done_at && now >= done_at) break;
		if (xQueueReceive(q, &it, wait) != pdTRUE) continue;
		if (it.ev == EV_FRAME) handle(&it);
		else if (it.ev == EV_READY && !ready) {
			ready = true;
			next = esp_timer_get_time();
		} else if (it.ev == EV_FAIL || (it.ev == EV_LOST && role != ROLE_ECHO)) {
			ok = false;
			break;
		}
	}
	bool ended_itself = run;
	teardown();
	ESP_LOGI(TAG, "%s stopped%s%s: sent %u, pongs %u, rx %u", ROLE_NAMES[role], fail[0] ? ": " : "", fail, (unsigned) tx->sent,
		 (unsigned) tx->pongs, (unsigned) rx->rx);
	*ok_out = ok;
	return ended_itself;
}

static void task_main(void *arg) {
	for (;;) {
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		uint32_t my_run = run_no;
		bool ok, ended = one_run(&ok);
		alive = false; /* first: the lab frame may start the next run at once */
		if (ended) hifi_lab_esp_finished("ble", my_run, ok);
	}
}

/* ------------------------------------------------------------------- setup */

static uint32_t num(const char *body, const char *key, uint32_t dflt) {
	double v;
	if (!hj_get_num(body, key, &v) || !(v >= 0)) return dflt;
	return v >= 4294967295.0 ? UINT32_MAX : (uint32_t) v;
}

/* once: the queue, the worker, the callbacks and both GATT apps */
static int setup(char *err, size_t n) {
	if (worker && gatts_if != ESP_GATT_IF_NONE && h_val && gattc_if != ESP_GATT_IF_NONE) return 0;
	if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED) {
		snprintf(err, n, "Bluetooth isn't up (turn the Bluetooth sink on)");
		return 503;
	}
	if (!(esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED)) {
		snprintf(err, n, "the Bluetooth controller isn't enabled");
		return 503;
	}
	if (!worker) {
		uint8_t *qmem = heap_caps_malloc(QLEN * sizeof(rx_item_t), MALLOC_CAP_SPIRAM);
		StackType_t *stack = heap_caps_malloc(STACK, MALLOC_CAP_SPIRAM);
		if (!reg_sem) reg_sem = xSemaphoreCreateBinary();
		if (!space_sem) space_sem = xSemaphoreCreateBinary();
		if (!closed_sem) closed_sem = xSemaphoreCreateBinary();
		if (!tx) tx = heap_caps_calloc(1, sizeof(*tx), MALLOC_CAP_SPIRAM);
		if (!rx) rx = heap_caps_calloc(1, sizeof(*rx), MALLOC_CAP_SPIRAM);
		if (!cb_it) cb_it = heap_caps_calloc(1, sizeof(*cb_it), MALLOC_CAP_SPIRAM);
		if (!qmem || !stack || !reg_sem || !space_sem || !closed_sem || !tx || !rx || !cb_it) {
			free(qmem);
			free(stack);
			snprintf(err, n, "out of memory");
			return 503;
		}
		if (!q) q = xQueueCreateStatic(QLEN, sizeof(rx_item_t), qmem, &q_ctl);
		else free(qmem);
		worker = xTaskCreateStaticPinnedToCore(task_main, "lab_ble", STACK, NULL, ESP_TASK_PRIO_MIN + 5, stack, &worker_tcb, 0);
		if (!worker) {
			free(stack);
			snprintf(err, n, "no worker task");
			return 503;
		}
	}
	if (!cbs_registered) {
		esp_err_t e;
		if ((e = esp_ble_gap_register_callback(gap_cb)) != ESP_OK || (e = esp_ble_gatts_register_callback(gatts_cb)) != ESP_OK ||
		    (e = esp_ble_gattc_register_callback(gattc_cb)) != ESP_OK) {
			snprintf(err, n, "BLE callbacks: %s", esp_err_to_name(e));
			return 503;
		}
		esp_ble_gatt_set_local_mtu(LAB_MTU);
		cbs_registered = true;
	}
	if (gatts_if == ESP_GATT_IF_NONE || !h_val) {
		xSemaphoreTake(reg_sem, 0);
		/* registered but the table failed last time: make the table again */
		if (gatts_if == ESP_GATT_IF_NONE) esp_ble_gatts_app_register(APP_SERVER);
		else esp_ble_gatts_create_attr_tab(DB, gatts_if, IDX_N, 0);
		if (xSemaphoreTake(reg_sem, pdMS_TO_TICKS(3000)) != pdTRUE || gatts_if == ESP_GATT_IF_NONE || !h_val) {
			snprintf(err, n, "the GATT server didn't register");
			return 503;
		}
	}
	if (gattc_if == ESP_GATT_IF_NONE) {
		xSemaphoreTake(reg_sem, 0);
		esp_ble_gattc_app_register(APP_CLIENT);
		if (xSemaphoreTake(reg_sem, pdMS_TO_TICKS(3000)) != pdTRUE || gattc_if == ESP_GATT_IF_NONE) {
			snprintf(err, n, "the GATT client didn't register");
			return 503;
		}
	}
	return 0;
}

static int start(uint32_t run_n, const char *body, char *err, size_t n) {
	if (alive) {
		snprintf(err, n, "the last run is still stopping");
		return 409;
	}
	if (connected || opening) {
		snprintf(err, n, "the last run's link is still up");
		return 409;
	}
	char s[24];
	if (!hj_get_str(body, "role", s, sizeof(s))) s[0] = 0;
	if (!strcmp(s, "echo")) role = ROLE_ECHO;
	else if (!strcmp(s, "ping")) role = ROLE_PING;
	else if (!strcmp(s, "flood")) role = ROLE_FLOOD;
	else {
		snprintf(err, n, "role must be echo, ping or flood");
		return 400;
	}
	if (!hj_get_str(body, "peer", s, sizeof(s)) || !hifi_link_parse_mac(s, peer)) {
		snprintf(err, n, "peer (the other board's MAC or device id) is required");
		return 400;
	}
	rate_hz = num(body, "rate_hz", 20);
	len = num(body, "len", role == ROLE_FLOOD ? PAYLOAD_MAX : 64);
	count = num(body, "count", 0);
	scan_s = num(body, "scan_s", 20);
	double iv = 0;
	interval_ms = hj_get_num(body, "interval_ms", &iv) && iv >= 7.5 && iv <= 100 ? (uint32_t) (iv * 1000) / 1000 : 0;
	if (hj_get_num(body, "interval_ms", &iv) && !interval_ms) {
		snprintf(err, n, "interval_ms 7.5-100");
		return 400;
	}
	if (iv == 7.5) interval_ms = 8; /* 6 units of 1.25 ms: the shortest */
	if (rate_hz < 1 || rate_hz > 100 || len < HIFI_LINK_HDR || len > PAYLOAD_MAX || scan_s < 3 || scan_s > 60) {
		snprintf(err, n, "rate_hz 1-100, len %d-%d, scan_s 3-60", HIFI_LINK_HDR, PAYLOAD_MAX);
		return 400;
	}
	int st = setup(err, n);
	if (st) return st;

	esp_read_mac(self, ESP_MAC_WIFI_STA);
	memcpy(self_bt, esp_bt_dev_get_address(), 6);
	/* the peer's BLE address: its Wi-Fi MAC plus the offset ours have. IDF
	 * adds the offset to the last byte alone, with no carry (esp_read_mac in
	 * 4.4 and 5.5), so do the same when our two addresses differ only there */
	if (!memcmp(self_bt, self, 5)) {
		memcpy(peer_bt, peer, 6);
		peer_bt[5] += (uint8_t) (self_bt[5] - self[5]);
	} else
		u64_mac((mac_u64(peer) + mac_u64(self_bt) - mac_u64(self)) & 0xffffffffffffULL, peer_bt);

	heap_start = heap_min = heap_now();
	xQueueReset(q);
	xSemaphoreTake(space_sem, 0);
	xSemaphoreTake(closed_sem, 0);
	hifi_link_tx_init(tx, esp_random());
	hifi_link_rx_init(rx);
	q_full = other = rejected = seen_adv = connects = disconnects = congest_events = send_fail_busy = 0;
	last_disc_reason = 0;
	ci_units = ci_latency = ci_timeout = 0;
	mtu_link = 0;
	phase_end = PH_IDLE;
	fail[0] = 0;
	congested = notify_on = false;
	memset(remote, 0, 6);
	t_link_us = 0;
	t_start_us = esp_timer_get_time();
	run_no = run_n;
	run = alive = true;

	if (role == ROLE_ECHO) {
		phase = PH_ADVERTISE;
		if (!advertise()) {
			run = alive = false;
			phase = PH_IDLE;
			snprintf(err, n, "advertising refused");
			return 503;
		}
	} else {
		static esp_ble_scan_params_t sp = {
			.scan_type = BLE_SCAN_TYPE_PASSIVE,
			.own_addr_type = BLE_ADDR_TYPE_PUBLIC,
			.scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
			.scan_interval = 0x50, /* 50 ms */
			.scan_window = 0x30,   /* 30 ms of it: Classic and Wi-Fi share the radio */
			.scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE,
		};
		phase = PH_SCAN;
		if (esp_ble_gap_set_scan_params(&sp) != ESP_OK) {
			run = alive = false;
			phase = PH_IDLE;
			snprintf(err, n, "scan refused");
			return 503;
		}
	}
	xTaskNotifyGive(worker);
	char m[18];
	mac_str(peer_bt, m, sizeof(m));
	ESP_LOGI(TAG, "%s with %s", ROLE_NAMES[role], m);
	return 0;
}

static bool stop(void) {
	run = false;
	for (int i = 0; i < 400 && alive; i++) vTaskDelay(pdMS_TO_TICKS(10));
	if (alive) ESP_LOGE(TAG, "the task didn't stop in 4 s");
	return !alive;
}

static void result(void *hj) {
	hj_t *j = hj;
	if (!tx) return;
	char m[18];
	hj_str(j, "role", ROLE_NAMES[role]);
	mac_str(peer, m, sizeof(m));
	hj_str(j, "peer", m);
	mac_str(peer_bt, m, sizeof(m));
	hj_str(j, "peer_ble", m);
	mac_str(self_bt, m, sizeof(m));
	hj_str(j, "self_ble", m);
	if (remote[0] | remote[1] | remote[2] | remote[3] | remote[4] | remote[5]) {
		mac_str(remote, m, sizeof(m));
		hj_str(j, "remote", m);
	}
	hj_str(j, "phase", PHASE_NAMES[alive ? phase : phase_end]);
	if (fail[0]) hj_str(j, "fail", fail);
	hj_bool(j, "connected", connected);
	if (t_link_us) hj_int(j, "link_ms", (int64_t) ((t_link_us - t_start_us) / 1000));
	hj_int(j, "mtu", mtu_link ? mtu_link : mtu); /* the link's, not the post-disconnect reset */
	hj_obj(j, "conn"); /* the link's parameters as last set: interval in 1.25 ms units, timeout in 10 ms */
	hj_int(j, "interval_ms_x100", ci_units * 125);
	hj_int(j, "latency", ci_latency);
	hj_int(j, "timeout_ms", ci_timeout * 10);
	if (interval_ms) hj_int(j, "asked_ms", interval_ms);
	hj_obj_end(j);
	hj_int(j, "len", len);
	if (role == ROLE_PING) hj_int(j, "rate_hz", rate_hz);
	if (role != ROLE_ECHO) hj_int(j, "adverts_seen", seen_adv);
	hj_int(j, "connects", connects);
	hj_int(j, "disconnects", disconnects);
	if (disconnects) hj_int(j, "last_disconnect_reason", last_disc_reason);
	hj_int(j, "rejected", rejected);
	hj_int(j, "congested", congest_events);
	if (role == ROLE_FLOOD) hj_int(j, "waits_for_room", send_fail_busy);
	hj_int(j, "queue_full", q_full);
	hj_int(j, "other", other);
	hj_obj(j, "heap"); /* internal RAM: at the start, the least during the run */
	hj_int(j, "start", heap_start);
	hj_int(j, "min", heap_min);
	hj_obj_end(j);
	hj_obj(j, "tx");
	hifi_link_tx_json(tx, j);
	hj_obj_end(j);
	if (role == ROLE_ECHO) {
		hj_obj(j, "rx");
		hifi_link_rx_json(rx, j);
		hj_obj_end(j);
	}
}

const hifi_lab_exp_t lab_ble = {
	.name = "ble",
	.help = "BLE GATT link beside Wi-Fi and A2DP: role echo|ping|flood, peer, rate_hz, len, count, interval_ms, scan_s",
	.start = start,
	.stop = stop,
	.result = result,
};
