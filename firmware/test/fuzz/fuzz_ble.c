/* The BLE link (lab_ble.c) as its GATT callbacks run it. At the echo, the
 * server: any client that connects may write the link characteristic's
 * value (up to 512 bytes as the stack hands it over, whatever the attribute's
 * maximum; a prepared write is dropped) or its notify switch, and each value write is copied by
 * hifi_link_take into the queue item and handled by hifi_link_receive. At a
 * pinger or flooder, the client: the echo's notifications take the same
 * path. Byte 0 picks the role and the clock's start; then events, each
 * after a gap of up to a second and a bit. The write routing below is
 * gatts_cb's ESP_GATTS_WRITE_EVT case, without the ESP types. lab_ble.c's
 * connected and run gates only drop events before hifi_link, so they aren't
 * modelled; event bit 4 is spare. */
#include "fuzz_link.h"

enum { H_VAL = 42, H_CCCD = 43 };

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
	if (n < 1) return 0;
	fz_t z = {d, n, 1};
	int role = (d[0] & 3) % 3; /* echo, ping, flood */
	bool echo = role == 0, pongs = role != 0;
	int64_t now = d[0] & 4 ? 0 : (int64_t) 86400 * 1000000 * 30;
	uint16_t conn_id = 0;

	static hifi_link_rx_t rx;
	static hifi_link_tx_t tx;
	hifi_link_rx_init(&rx);
	hifi_link_tx_init(&tx, FZ_SESSION);
	fz_keys();
	static hifi_link_rx_t rx0;
	static hifi_link_tx_t tx0;
	fz_counts_t c = {0};
	struct {
		uint16_t len;
		uint8_t data[HIFI_LINK_BLE_PAYLOAD];
	} it; /* rx_item_t */
	uint8_t air[512] = {0}; /* a damaged length may run past the frame */
	for (int events = 0; fz_more(&z) && events < 256; events++) {
		now += (int64_t) (fz_u8(&z) % 40) * 30000;
		uint8_t e = fz_u8(&z);
		uint16_t handle = e & 3 ? H_VAL : e & 4 ? H_CCCD : (uint16_t) fz_u8(&z);
		uint16_t from = e & 8 ? 1 : conn_id; /* another connection's write */
		if (pongs && e & 32) hifi_link_tx_sent(&tx, HIFI_LINK_HDR, true, now);
		bool forged;
		size_t len = fz_frame(&z, air, sizeof(air), now, rx.session, &forged);

		bool is_prep = e & 64; /* a long (prepared) write: dropped, counted as other */
		if (echo) { /* ESP_GATTS_WRITE_EVT */
			if (from != conn_id || is_prep || handle != H_VAL) continue; /* the notify switch, or not ours */
		} else if (from != conn_id || handle != H_VAL) /* ESP_GATTC_NOTIFY_EVT */
			continue;
		/* post(EV_FRAME, value, len) */
		size_t took = hifi_link_take(it.data, sizeof(it.data), air, len);
		FUZZ_ASSERT(took == (len && len <= HIFI_LINK_BLE_PAYLOAD ? len : 0));
		it.len = (uint16_t) took;
		FUZZ_ASSERT(!took || !memcmp(it.data, air, took));
		/* the worker's handle() */
		fz_canonical(it.data, it.len);
		hifi_link_frame_t pong;
		memcpy(&rx0, &rx, sizeof(rx));
		memcpy(&tx0, &tx, sizeof(tx));
		hifi_link_got_t g = hifi_link_receive(&fz_key, &rx, &tx, echo, pongs, it.data, it.len, now, &pong);
		if (forged) fz_refused(g, &rx0, &tx0, &rx, &tx);
		fz_got(g, it.data, it.len, echo, pongs, &pong, now, &c);
		fz_check(&rx, &tx, &c);
	}
	fz_json(&rx, &tx);
	return 0;
}
