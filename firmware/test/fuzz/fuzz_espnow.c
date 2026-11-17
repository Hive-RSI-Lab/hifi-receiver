/* The ESP-NOW link (lab_espnow.c) as its receive side runs it: every frame
 * any nearby board sends to this one's MAC (recv_cb takes 1..250 bytes from
 * the configured peer, and a peer's MAC is easy to forge) goes through
 * hifi_link_receive at an echo, a pinger or a flooder. Byte 0 picks the role
 * and the clock's start; then frames, each after a gap of up to a second
 * and a bit, so sessions go idle and the clock's one-second windows close.
 * The whole input also goes, as a frame off the air, through the forger's
 * parser (hifi_link_espnow_body), which must never point past it. */
#include "fuzz_link.h"

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
	if (n < 1) return 0;
	if (n >= 16) {
		size_t b = hifi_link_espnow_body(d, n, d + 10);
		if (b && (b < HIFI_LINK_MIN || b > HIFI_LINK_MAX || HIFI_LINK_ESPNOW_BODY + b > n)) __builtin_trap();
	}
	fz_t z = {d, n, 1};
	int role = (d[0] & 3) % 3; /* echo, ping, flood */
	bool echo = role == 0, pongs = role == 1;
	int64_t now = d[0] & 4 ? 0 : (int64_t) 86400 * 1000000 * 30; /* just booted, or up a month */

	static hifi_link_rx_t rx;
	static hifi_link_tx_t tx;
	hifi_link_rx_init(&rx);
	hifi_link_tx_init(&tx, FZ_SESSION);
	fz_keys();
	static hifi_link_rx_t rx0;
	static hifi_link_tx_t tx0;
	fz_counts_t c = {0};
	uint8_t item[HIFI_LINK_MAX]; /* rx_item_t's data */
	uint8_t air[512] = {0}; /* a damaged length may run past the frame */
	for (int frames = 0; fz_more(&z) && frames < 256; frames++) {
		now += (int64_t) (fz_u8(&z) % 40) * 30000;
		uint8_t fl = fz_u8(&z); /* read in every role, so one seed fits them all */
		if (pongs && fl & 1) hifi_link_tx_sent(&tx, HIFI_LINK_HDR, true, now); /* a ping of ours went out */
		bool forged;
		size_t len = fz_frame(&z, air, sizeof(air), now, rx.session, &forged);
		/* recv_cb */
		if (len == 0 || len > HIFI_LINK_MAX) continue;
		memcpy(item, air, len);
		fz_canonical(item, len);
		hifi_link_frame_t pong;
		memcpy(&rx0, &rx, sizeof(rx));
		memcpy(&tx0, &tx, sizeof(tx));
		hifi_link_got_t g = hifi_link_receive(&fz_key, &rx, &tx, echo, pongs, item, len, now, &pong);
		if (forged) fz_refused(g, &rx0, &tx0, &rx, &tx);
		fz_got(g, item, len, echo, pongs, &pong, now, &c);
		fz_check(&rx, &tx, &c);
	}
	fz_json(&rx, &tx);
	return 0;
}
