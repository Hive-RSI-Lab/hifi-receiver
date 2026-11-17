/* The lab frame (hifi_lab) and the link measurements (hifi_link) */
#include <stdlib.h>

#include "check.h"
#include "hifi_api.h"
#include "hifi_json.h"
#include "hifi_lab.h"
#include "hifi_link.h"

/* ------------------------------------------------------------ a fake exp */

static int fake_started, fake_stopped, fake_fail, fake_stuck;
static uint32_t fake_run;
static char fake_role[16];

static int fake_start(uint32_t run, const char *body, char *err, size_t n) {
	fake_run = run;
	if (fake_fail) {
		snprintf(err, n, "peer is required");
		return 400;
	}
	if (!hj_get_str(body, "role", fake_role, sizeof(fake_role))) fake_role[0] = 0;
	fake_started++;
	return 0;
}
static bool fake_stop(void) {
	fake_stopped++;
	return !fake_stuck;
}
static void fake_result(void *hj) { hj_int(hj, "frames", 42); }

static const hifi_lab_exp_t FAKE = {"espnow", "ESP-NOW link test", fake_start, fake_stop, fake_result};
static const hifi_lab_exp_t OTHER = {"ble", NULL, fake_start, fake_stop, NULL};

static void lab_frame(void) {
	hifi_lab_t l;
	char out[1024];
	hifi_lab_init(&l);
	char v[64];
	CHECK(!hifi_lab_selftest(&l, v, sizeof(v))); /* a lab build with nothing in it */

	CHECK(hifi_lab_register(&l, &FAKE));
	CHECK(hifi_lab_register(&l, &OTHER));
	CHECK(!hifi_lab_register(&l, &FAKE)); /* taken */
	hifi_lab_exp_t bad = FAKE;
	bad.name = "Bad Name";
	CHECK(!hifi_lab_register(&l, &bad));
	bad.name = "all";
	CHECK(!hifi_lab_register(&l, &bad));
	bad.name = "x";
	bad.stop = NULL;
	CHECK(!hifi_lab_register(&l, &bad));
	CHECK(hifi_lab_selftest(&l, v, sizeof(v)));
	CHECK_STR(v, "2 experiments");

	/* GET lists */
	CHECK(hifi_lab_request(&l, false, NULL, 0, 100000, out, sizeof(out)) == 200);
	CHECK(hj_valid_object(out));
	CHECK_HAS(out, "\"name\":\"espnow\"");
	CHECK_HAS(out, "\"running\":\"\"");

	/* bad requests */
	CHECK(hifi_lab_request(&l, true, "nope", 0, 100000, out, sizeof(out)) == 400);
	CHECK(hifi_lab_request(&l, true, "{\"exp\":\"espnow\"}", 0, 100000, out, sizeof(out)) == 400);
	CHECK(hifi_lab_request(&l, true, "{\"op\":\"start\"}", 0, 100000, out, sizeof(out)) == 400);
	CHECK(hifi_lab_request(&l, true, "{\"op\":\"start\",\"exp\":\"nope\"}", 0, 100000, out, sizeof(out)) == 404);
	CHECK(hifi_lab_request(&l, true, "{\"op\":\"jump\",\"exp\":\"espnow\"}", 0, 100000, out, sizeof(out)) == 400);
	CHECK(hifi_lab_request(&l, true, "{\"op\":\"start\",\"exp\":\"espnow\",\"max_s\":0}", 0, 100000, out, sizeof(out)) == 400);
	CHECK(hifi_lab_request(&l, true, "{\"op\":\"start\",\"exp\":\"espnow\",\"max_s\":3601}", 0, 100000, out, sizeof(out)) == 400);
	CHECK(fake_started == 0);

	/* too little RAM: refused before the experiment is asked */
	CHECK(hifi_lab_request(&l, true, "{\"op\":\"start\",\"exp\":\"espnow\"}", 0, HIFI_LAB_HEAP_FLOOR - 1, out, sizeof(out)) == 503);
	CHECK(fake_started == 0);

	/* the experiment refuses: its status and message come through */
	fake_fail = 1;
	CHECK(hifi_lab_request(&l, true, "{\"op\":\"start\",\"exp\":\"espnow\"}", 0, 100000, out, sizeof(out)) == 400);
	CHECK_HAS(out, "peer is required");
	CHECK(!hifi_lab_running(&l)[0]);
	fake_fail = 0;

	/* start: the whole body reaches the experiment */
	CHECK(hifi_lab_request(&l, true, "{\"op\":\"start\",\"exp\":\"espnow\",\"max_s\":10,\"role\":\"echo\"}", 1000, 100000,
			       out, sizeof(out)) == 200);
	CHECK(fake_started == 1);
	CHECK_STR(fake_role, "echo");
	CHECK_STR(hifi_lab_running(&l), "espnow");
	CHECK_HAS(out, "\"running\":true");
	CHECK_HAS(out, "\"frames\":42");
	CHECK(hifi_lab_request(&l, true, "{\"op\":\"start\",\"exp\":\"espnow\"}", 2000, 100000, out, sizeof(out)) == 409);
	CHECK(hifi_lab_selftest(&l, v, sizeof(v)));
	CHECK_STR(v, "2 experiments, running espnow");

	/* the deadline stops it, not a moment before */
	CHECK(hifi_lab_tick(&l, 10999, 100000) == 0);
	CHECK(hifi_lab_tick(&l, 11000, 100000) == 1);
	CHECK(fake_stopped == 1);
	CHECK(!hifi_lab_running(&l)[0]);
	CHECK(hifi_lab_request(&l, true, "{\"op\":\"result\",\"exp\":\"espnow\"}", 20000, 100000, out, sizeof(out)) == 200);
	CHECK_HAS(out, "\"stopped\":\"deadline\"");
	CHECK_HAS(out, "\"elapsed_s\":10.0");
	CHECK_HAS(out, "\"runs\":1");

	/* stop when asked; a stop of a stopped one is harmless */
	CHECK(hifi_lab_request(&l, true, "{\"op\":\"start\",\"exp\":\"espnow\"}", 30000, 100000, out, sizeof(out)) == 200);
	CHECK(hifi_lab_request(&l, true, "{\"op\":\"stop\",\"exp\":\"espnow\"}", 31000, 100000, out, sizeof(out)) == 200);
	CHECK(fake_stopped == 2);
	CHECK_HAS(out, "\"stopped\":\"asked\"");
	CHECK(hifi_lab_request(&l, true, "{\"op\":\"stop\",\"exp\":\"espnow\"}", 31000, 100000, out, sizeof(out)) == 200);
	CHECK(fake_stopped == 2);

	/* default deadline; low RAM stops every run */
	CHECK(hifi_lab_request(&l, true, "{\"op\":\"start\",\"exp\":\"espnow\"}", 40000, 100000, out, sizeof(out)) == 200);
	CHECK(hifi_lab_request(&l, true, "{\"op\":\"start\",\"exp\":\"ble\"}", 40000, 100000, out, sizeof(out)) == 200);
	CHECK(hifi_lab_tick(&l, 41000, HIFI_LAB_HEAP_STOP - 1) == 2);
	CHECK(fake_stopped == 4);
	CHECK(hifi_lab_request(&l, true, "{\"op\":\"result\",\"exp\":\"ble\"}", 41000, 100000, out, sizeof(out)) == 200);
	CHECK_HAS(out, "\"stopped\":\"heap\"");

	/* the deadline across the millisecond clock wrapping */
	CHECK(hifi_lab_request(&l, true, "{\"op\":\"start\",\"exp\":\"espnow\",\"max_s\":2}", UINT32_MAX - 500, 100000, out,
			       sizeof(out)) == 200);
	CHECK(hifi_lab_tick(&l, 1000, 100000) == 0);
	CHECK(hifi_lab_tick(&l, 1500, 100000) == 1);

	/* stop all; an experiment that ends itself isn't stopped again */
	CHECK(hifi_lab_request(&l, true, "{\"op\":\"start\",\"exp\":\"espnow\"}", 50000, 100000, out, sizeof(out)) == 200);
	int before = fake_stopped;
	uint32_t run = fake_run;
	hifi_lab_finished(&l, "espnow", run - 1, 52000, true); /* a late finish of the run before: dropped */
	CHECK(!strcmp(hifi_lab_running(&l), "espnow"));
	hifi_lab_finished(&l, "espnow", run + 1, 52000, true); /* not started yet: dropped */
	CHECK(!strcmp(hifi_lab_running(&l), "espnow"));
	hifi_lab_finished(&l, "espnow", run, 52000, true);
	CHECK(fake_stopped == before);
	CHECK(!hifi_lab_running(&l)[0]);
	CHECK(hifi_lab_request(&l, true, "{\"op\":\"result\",\"exp\":\"espnow\"}", 53000, 100000, out, sizeof(out)) == 200);
	CHECK_HAS(out, "\"stopped\":\"done\"");
	CHECK(hifi_lab_request(&l, true, "{\"op\":\"start\",\"exp\":\"ble\"}", 60000, 100000, out, sizeof(out)) == 200);
	CHECK(hifi_lab_request(&l, true, "{\"op\":\"stop\",\"exp\":\"all\"}", 61000, 100000, out, sizeof(out)) == 200);
	CHECK(fake_stopped == before + 1);
	CHECK(hj_valid_object(out));
	hifi_lab_finished(&l, "espnow", run, 62000, false); /* after it is done: nothing changes */
	CHECK(hifi_lab_request(&l, true, "{\"op\":\"result\",\"exp\":\"espnow\"}", 62000, 100000, out, sizeof(out)) == 200);
	CHECK_HAS(out, "\"stopped\":\"done\"");

	/* the restart reasons the self-test names */
	{
		char v2[160];
		l.stuck_restart = true;
		CHECK(hifi_lab_selftest(&l, v2, sizeof(v2)));
		CHECK_HAS(v2, "tick stuck");
		/* the experiment it was in, within a self-test value's 64 bytes */
		char v3[64];
		hifi_lab_set_crashed(&l, "espnow");
		CHECK(hifi_lab_selftest(&l, v3, sizeof(v3)));
		CHECK_HAS(v3, "tick stuck 3 min during espnow");
		l.stuck_restart = false;
		l.offline_restart = true;
		CHECK(hifi_lab_selftest(&l, v3, sizeof(v3)));
		CHECK_HAS(v3, "off dial-in 15 min during espnow");
		l.offline_restart = false;
		hifi_lab_set_crashed(&l, "");
	}

	/* a crash during a run is named after the restart */
	hifi_lab_set_crashed(&l, "espnow");
	CHECK(hifi_lab_selftest(&l, v, sizeof(v)));
	CHECK_HAS(v, "just after espnow");
	CHECK(hifi_lab_request(&l, false, NULL, 0, 100000, out, sizeof(out)) == 200);
	CHECK_HAS(out, "\"crashed_in\":\"espnow\"");

	/* what the board keeps for a crash: the running one, else one stopped in the last minute */
	CHECK(!strcmp(hifi_lab_recent(&l, 61000), "ble")); /* ble stopped at 61000 by "all" */
	CHECK(!strcmp(hifi_lab_recent(&l, 61000 + HIFI_LAB_RECENT_MS - 1), "ble"));
	CHECK(!hifi_lab_recent(&l, 61000 + HIFI_LAB_RECENT_MS)[0]);
	CHECK(hifi_lab_request(&l, true, "{\"op\":\"start\",\"exp\":\"espnow\"}", 200000, 100000, out, sizeof(out)) == 200);
	CHECK(!strcmp(hifi_lab_recent(&l, 200500), "espnow"));
	CHECK(hifi_lab_request(&l, true, "{\"op\":\"stop\",\"exp\":\"espnow\"}", 201000, 100000, out, sizeof(out)) == 200);
	CHECK(!strcmp(hifi_lab_recent(&l, 230000), "espnow"));

	/* a stop that doesn't finish in time says so */
	CHECK(hifi_lab_request(&l, true, "{\"op\":\"start\",\"exp\":\"espnow\"}", 240000, 100000, out, sizeof(out)) == 200);
	fake_stuck = 1;
	CHECK(hifi_lab_request(&l, true, "{\"op\":\"stop\",\"exp\":\"espnow\"}", 241000, 100000, out, sizeof(out)) == 200);
	fake_stuck = 0;
	CHECK_HAS(out, "\"stopped\":\"stuck\"");

	/* "recently stopped" ends with its window, and the clock's wrap can't bring it back */
	{
		CHECK(strstr(hifi_lab_recent(&l, 242000), "espnow") != NULL);
		hifi_lab_tick(&l, 241000 + HIFI_LAB_RECENT_MS, 100000);
		CHECK(!hifi_lab_recent(&l, 241000 + HIFI_LAB_RECENT_MS)[0]);
		CHECK(!hifi_lab_recent(&l, 241000u + 0x80000000u)[0]);
		CHECK(!hifi_lab_recent(&l, 241000u + 10u)[0]); /* 2^32 ms later, the same low bits */
	}

	/* the lab's own restart is named in the self-test and the listing */
	hifi_lab_set_crashed(&l, "");
	l.offline_restart = true;
	CHECK(hifi_lab_selftest(&l, v, sizeof(v)));
	CHECK_HAS(v, "off dial-in 15 min");
	CHECK(hifi_lab_request(&l, false, NULL, 0, 100000, out, sizeof(out)) == 200);
	CHECK_HAS(out, "\"offline_restart\":true");
	CHECK(hj_valid_object(out));
	l.offline_restart = false;

	/* a reply buffer that's too small says so rather than sending half */
	char tiny[40];
	CHECK(hifi_lab_request(&l, false, NULL, 0, 100000, tiny, sizeof(tiny)) == 500);
}

/* the API route: 404 outside a lab build, through to the lab in one */
static hifi_lab_t api_lab;
static int api_lab_op(bool post, const char *body, char *out, size_t cap) {
	return hifi_lab_request(&api_lab, post, body, 0, 100000, out, cap);
}

static void lab_route(void) {
	hifi_ops_t ops;
	memset(&ops, 0, sizeof(ops));
	char out[1024];
	CHECK(hifi_api_handle(&ops, "GET", "/api/v1/lab", NULL, out, sizeof(out)) == 404);
	hifi_lab_init(&api_lab);
	hifi_lab_register(&api_lab, &FAKE);
	ops.lab = api_lab_op;
	CHECK(hifi_api_handle(&ops, "GET", "/api/v1/lab", NULL, out, sizeof(out)) == 200);
	CHECK_HAS(out, "espnow");
	CHECK(hifi_api_handle(&ops, "POST", "/api/v1/lab", "{\"op\":\"result\",\"exp\":\"espnow\"}", out, sizeof(out)) == 200);
	/* the LAN may read the lab but not start or stop a run, as with the relay */
	CHECK(hifi_api_handle_from(&ops, HIFI_SRC_LAN, "GET", "/api/v1/lab", NULL, out, sizeof(out)) == 200);
	CHECK(hifi_api_handle_from(&ops, HIFI_SRC_LAN, "POST", "/api/v1/lab", "{\"op\":\"start\",\"exp\":\"espnow\"}", out, sizeof(out)) == 403);
	CHECK_HAS(out, "not from the LAN");
}

/* ------------------------------------------------------------ link frames */

/* a test key; the boards' keys come from the run's start op, never the code */
static const uint8_t KEY[HIFI_LINK_KEY] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
static hifi_link_key_t K;

static void frames(void) {
	uint8_t buf[HIFI_LINK_MAX];
	hifi_link_frame_t f = {HIFI_LINK_PING, 7, 12345, -5, 1LL << 40, INT64_MAX, 100}, g;
	CHECK(hifi_link_encode(&K, &f, buf, sizeof(buf)) == 100);
	CHECK(hifi_link_decode(&K, buf, 100, &g));
	CHECK(g.type == HIFI_LINK_PING && g.session == 7 && g.seq == 12345);
	CHECK(g.t1 == -5 && g.t2 == (1LL << 40) && g.t3 == INT64_MAX && g.len == 100);

	/* damage anywhere is caught: the header (times and session too) by the
	 * tag, the payload by its pattern, the tag by itself */
	for (int i = 0; i < 100; i++) {
		uint8_t c[HIFI_LINK_MAX];
		memcpy(c, buf, 100);
		c[i] ^= 0x10;
		hifi_link_frame_t h;
		CHECK(!hifi_link_decode(&K, c, 100, &h));
	}
	CHECK(!hifi_link_decode(&K, buf, HIFI_LINK_MIN - 1, &g));
	/* a frame cut short (or padded) is caught by its length */
	for (size_t n = HIFI_LINK_MIN; n < 100; n += 7) CHECK(!hifi_link_decode(&K, buf, n, &g));
	CHECK(!hifi_link_decode(&K, buf, 101, &g));
	CHECK(hifi_link_decode(&K, buf, 100, &g));
	CHECK(!hifi_link_decode(NULL, buf, 100, &g));
	CHECK(hifi_link_encode(NULL, &f, buf, sizeof(buf)) == 0);

	/* sizes */
	f.len = HIFI_LINK_MIN;
	CHECK(hifi_link_encode(&K, &f, buf, sizeof(buf)) == HIFI_LINK_MIN);
	CHECK(hifi_link_decode(&K, buf, HIFI_LINK_MIN, &g));
	f.len = HIFI_LINK_MIN - 1;
	CHECK(hifi_link_encode(&K, &f, buf, sizeof(buf)) == 0);
	f.len = HIFI_LINK_MAX + 1;
	CHECK(hifi_link_encode(&K, &f, buf, sizeof(buf)) == 0);
	f.len = 200;
	CHECK(hifi_link_encode(&K, &f, buf, 150) == 0);
}

/* the tag is HMAC-SHA256 (RFC 2104), cut to 8 bytes: one answer worked out
 * with Python's hmac module, and the key parser */
static void tag(void) {
	uint8_t hdr[HIFI_LINK_HDR], t[HIFI_LINK_TAG];
	static const uint8_t want[HIFI_LINK_TAG] = {0x1d, 0x52, 0x7f, 0x43, 0xb4, 0xea, 0x00, 0xa2};
	for (int i = 0; i < HIFI_LINK_HDR; i++) hdr[i] = (uint8_t) (100 + i);
	hifi_link_tag(&K, hdr, t);
	CHECK(!memcmp(t, want, sizeof(t)));
	hifi_link_tag(&K, hdr, t); /* the key's states are not used up */
	CHECK(!memcmp(t, want, sizeof(t)));

	uint8_t k[HIFI_LINK_KEY], z[HIFI_LINK_KEY] = {0};
	CHECK(hifi_link_parse_key("000102030405060708090a0B0c0D0e0F", k) && !memcmp(k, KEY, sizeof(k)));
	memset(k, 0, sizeof(k));
	CHECK(!hifi_link_parse_key("000102030405060708090a0b0c0d0e0", k));   /* 31 */
	CHECK(!hifi_link_parse_key("000102030405060708090a0b0c0d0e0f0", k)); /* 33 */
	CHECK(!hifi_link_parse_key("000102030405060708090a0b0c0d0e0g", k));
	CHECK(!hifi_link_parse_key("", k) && !hifi_link_parse_key(NULL, k));
	CHECK(!memcmp(k, z, sizeof(k))); /* untouched by a refusal */
}

/* A stranger in radio range, with any MAC, can't move the counts: frames
 * under another key (a live session, a jump in seq, a PONG for our ping)
 * are counted bad and change nothing else, and an old-version frame with
 * no tag is bad too. */
static void forged(void) {
	hifi_link_key_t other;
	uint8_t ok_key[HIFI_LINK_KEY];
	memcpy(ok_key, KEY, sizeof(ok_key));
	ok_key[15] ^= 1;
	hifi_link_key_init(&other, ok_key);

	hifi_link_rx_t rx;
	static hifi_link_tx_t tx;
	hifi_link_rx_init(&rx);
	hifi_link_tx_init(&tx, 5);
	uint8_t buf[HIFI_LINK_MAX];
	hifi_link_frame_t pong;
	int64_t now = 1000000;
	for (uint32_t q = 0; q < 10; q++) {
		hifi_link_frame_t f = {HIFI_LINK_DATA, 5, q, 0, 0, 0, 250};
		CHECK(hifi_link_encode(&K, &f, buf, sizeof(buf)) == 250);
		CHECK(hifi_link_receive(&K, &rx, &tx, true, false, buf, 250, now += 1000, &pong) == HIFI_LINK_GOT_USED);
	}
	CHECK(rx.rx == 10 && hifi_link_rx_expected(&rx) == 10 && rx.bad == 0);

	hifi_link_rx_t before = rx;
	/* the live session, a seq far ahead: would show 10^6 lost */
	hifi_link_frame_t jump = {HIFI_LINK_DATA, 5, 1000000, 0, 0, 0, 250};
	CHECK(hifi_link_encode(&other, &jump, buf, sizeof(buf)) == 250);
	CHECK(hifi_link_receive(&K, &rx, &tx, true, false, buf, 250, now += 1000, &pong) == HIFI_LINK_GOT_BAD);
	/* the next seq, which would be counted as received */
	hifi_link_frame_t next = {HIFI_LINK_DATA, 5, 10, 0, 0, 0, 250};
	CHECK(hifi_link_encode(&other, &next, buf, sizeof(buf)) == 250);
	CHECK(hifi_link_receive(&K, &rx, &tx, true, false, buf, 250, now += 1000, &pong) == HIFI_LINK_GOT_BAD);
	/* a genuine frame with its tag cut off and the length mended */
	CHECK(hifi_link_encode(&K, &next, buf, sizeof(buf)) == 250);
	buf[4] = 250 - HIFI_LINK_TAG;
	CHECK(hifi_link_receive(&K, &rx, &tx, true, false, buf, 250 - HIFI_LINK_TAG, now += 1000, &pong) == HIFI_LINK_GOT_BAD);
	/* a version 2 frame (no tag) */
	CHECK(hifi_link_encode(&K, &next, buf, sizeof(buf)) == 250);
	buf[2] = 2;
	CHECK(hifi_link_receive(&K, &rx, &tx, true, false, buf, 250, now += 1000, &pong) == HIFI_LINK_GOT_BAD);
	/* a PING under the wrong key gets no answer */
	hifi_link_frame_t ping = {HIFI_LINK_PING, 5, 1, now, 0, 0, 64};
	CHECK(hifi_link_encode(&other, &ping, buf, sizeof(buf)) == 64);
	CHECK(hifi_link_receive(&K, &rx, &tx, true, false, buf, 64, now += 1000, &pong) == HIFI_LINK_GOT_BAD);
	CHECK(rx.bad == 5);
	rx.bad = before.bad;
	CHECK(!memcmp(&rx, &before, sizeof(rx))); /* nothing else moved */

	/* the pinger: a forged PONG adds no round trip */
	hifi_link_tx_sent(&tx, 64, true, now);
	hifi_link_frame_t fp = {HIFI_LINK_PONG, 5, 0, now, now + 10, now + 20, 64};
	CHECK(hifi_link_encode(&other, &fp, buf, sizeof(buf)) == 64);
	CHECK(hifi_link_receive(&K, &rx, &tx, false, true, buf, 64, now + 500, &pong) == HIFI_LINK_GOT_BAD);
	CHECK(tx.pongs == 0 && tx.bad == 0 && rx.bad == before.bad + 1);
	/* and the same PONG under the run's key is taken: the check above is the tag */
	CHECK(hifi_link_encode(&K, &fp, buf, sizeof(buf)) == 64);
	CHECK(hifi_link_receive(&K, &rx, &tx, false, true, buf, 64, now + 500, &pong) == HIFI_LINK_GOT_USED);
	CHECK(tx.pongs == 1);
}

/* The forger lab_forge.c runs on a third board: a frame it heard, the seq
 * jumped, re-tagged under its own key, is refused; the same under the run's
 * key (the control) is taken and shows the jump as lost frames. */
static void forger(void) {
	hifi_link_key_t other;
	uint8_t ok_key[HIFI_LINK_KEY];
	memcpy(ok_key, KEY, sizeof(ok_key));
	ok_key[0] ^= 0x80;
	hifi_link_key_init(&other, ok_key);
	hifi_link_rx_t rx;
	static hifi_link_tx_t tx;
	hifi_link_rx_init(&rx);
	hifi_link_tx_init(&tx, 9);
	uint8_t heard[HIFI_LINK_MAX], out[HIFI_LINK_MAX];
	hifi_link_frame_t pong, g;
	int64_t now = 1000000;
	for (uint32_t q = 0; q < 10; q++) {
		hifi_link_frame_t f = {HIFI_LINK_DATA, 9, q, 0, 0, 0, 200};
		CHECK(hifi_link_encode(&K, &f, heard, sizeof(heard)) == 200);
		CHECK(hifi_link_receive(&K, &rx, &tx, true, false, heard, 200, now += 1000, &pong) == HIFI_LINK_GOT_USED);
	}
	/* heard: seq 9. Forged under the forger's own key */
	CHECK(hifi_link_forge(&other, heard, 200, 1000, out, sizeof(out)) == 200);
	CHECK(hifi_link_decode(&other, out, 200, &g) && g.seq == 1009 && g.session == 9 && g.len == 200);
	CHECK(!hifi_link_decode(&K, out, 200, &g));
	hifi_link_rx_t before = rx;
	CHECK(hifi_link_receive(&K, &rx, &tx, true, false, out, 200, now += 1000, &pong) == HIFI_LINK_GOT_BAD);
	CHECK(rx.bad == before.bad + 1);
	rx.bad = before.bad;
	CHECK(!memcmp(&rx, &before, sizeof(rx)));
	CHECK(hifi_link_rx_expected(&rx) == 10);
	/* the control: the run's key, and the jump is taken as 999 lost */
	CHECK(hifi_link_forge(&K, heard, 200, 1000, out, sizeof(out)) == 200);
	CHECK(hifi_link_receive(&K, &rx, &tx, true, false, out, 200, now += 1000, &pong) == HIFI_LINK_GOT_USED);
	CHECK(hifi_link_rx_expected(&rx) == 1010 && rx.rx == 11);
	/* not shaped like ours: refused, nothing written */
	CHECK(hifi_link_forge(&K, heard, 199, 1, out, sizeof(out)) == 0);
	CHECK(hifi_link_forge(&K, heard, 200, 1, out, 199) == 0);
	CHECK(hifi_link_forge(&K, heard, HIFI_LINK_MIN - 1, 1, out, sizeof(out)) == 0);
	CHECK(hifi_link_forge(NULL, heard, 200, 1, out, sizeof(out)) == 0);
	heard[2] = 2;
	CHECK(hifi_link_forge(&K, heard, 200, 1, out, sizeof(out)) == 0);
}

/* a stranger who sends again a genuine frame they heard (forge with jump 0):
 * what the tag can't stop, and what each end makes of it */
static void replay(void) {
	hifi_link_rx_t rx;
	static hifi_link_tx_t tx;
	hifi_link_rx_init(&rx);
	hifi_link_tx_init(&tx, 7);
	uint8_t heard[HIFI_LINK_MAX], out[HIFI_LINK_MAX];
	hifi_link_frame_t pong;
	int64_t now = 1000000;
	/* a PING, answered once; its replay (as heard, tag and all) gets nothing */
	hifi_link_frame_t p = {HIFI_LINK_PING, 7, 3, now, 0, 0, 60};
	CHECK(hifi_link_encode(&K, &p, heard, sizeof(heard)) == 60);
	CHECK(hifi_link_receive(&K, &rx, &tx, true, false, heard, 60, now += 1000, &pong) == HIFI_LINK_GOT_REPLY);
	CHECK(hifi_link_forge(&K, heard, 60, 0, out, sizeof(out)) == 60 && !memcmp(out, heard, 60));
	for (int i = 0; i < 3; i++)
		CHECK(hifi_link_receive(&K, &rx, &tx, true, false, out, 60, now += 1000, &pong) == HIFI_LINK_GOT_OTHER);
	CHECK(rx.pings == 1 && rx.ping_dup == 3 && rx.bad == 0);
	/* the next PING of the run is answered; a new session starts the memory again */
	p.seq = 4;
	CHECK(hifi_link_encode(&K, &p, heard, sizeof(heard)) == 60);
	CHECK(hifi_link_receive(&K, &rx, &tx, true, false, heard, 60, now += 1000, &pong) == HIFI_LINK_GOT_REPLY);
	p.session = 8;
	p.seq = 3;
	CHECK(hifi_link_encode(&K, &p, heard, sizeof(heard)) == 60);
	CHECK(hifi_link_receive(&K, &rx, &tx, true, false, heard, 60, now += 1000, &pong) == HIFI_LINK_GOT_REPLY);
	CHECK(rx.pings == 3 && rx.ping_dup == 3);
	/* DATA: the replay is a dup, the counts don't move */
	hifi_link_frame_t d = {HIFI_LINK_DATA, 7, 0, 0, 0, 0, 100};
	for (uint32_t q = 0; q < 5; q++) {
		d.seq = q;
		CHECK(hifi_link_encode(&K, &d, heard, sizeof(heard)) == 100);
		CHECK(hifi_link_receive(&K, &rx, &tx, true, false, heard, 100, now += 1000, &pong) == HIFI_LINK_GOT_USED);
	}
	CHECK(hifi_link_receive(&K, &rx, &tx, true, false, heard, 100, now += 1000, &pong) == HIFI_LINK_GOT_USED);
	CHECK(rx.rx == 5 && rx.dup == 1 && hifi_link_rx_expected(&rx) == 5);
	CHECK(rx.pings == 3 && rx.ping_dup == 3); /* the echo's side kept through the DATA session */
	/* a PONG: taken once, its replay a dup at the pinger */
	hifi_link_tx_sent(&tx, 60, true, now);
	hifi_link_frame_t q = {HIFI_LINK_PING, 7, 0, now, 0, 0, 60}, r;
	hifi_link_pong_for(&q, now + 500, now + 600, &r);
	CHECK(hifi_link_encode(&K, &r, heard, sizeof(heard)) == 60);
	CHECK(hifi_link_receive(&K, &rx, &tx, false, true, heard, 60, now + 1100, &pong) == HIFI_LINK_GOT_USED);
	CHECK(hifi_link_receive(&K, &rx, &tx, false, true, heard, 60, now + 2100, &pong) == HIFI_LINK_GOT_USED);
	CHECK(tx.pongs == 1 && tx.dup == 1 && tx.bad == 0);
}

/* a key not set, or wiped: refused everywhere, though its state is all zeros
 * that anyone could compute */
static void key_ready(void) {
	hifi_link_key_t z;
	memset(&z, 0, sizeof(z));
	uint8_t b[HIFI_LINK_MAX], out[HIFI_LINK_MAX];
	hifi_link_frame_t f = {HIFI_LINK_DATA, 1, 1, 0, 0, 0, 80}, g;
	CHECK(hifi_link_encode(&z, &f, b, sizeof(b)) == 0);
	CHECK(hifi_link_encode(&K, &f, b, sizeof(b)) == 80);
	CHECK(!hifi_link_decode(&z, b, 80, &g));
	CHECK(hifi_link_forge(&z, b, 80, 1, out, sizeof(out)) == 0);
	/* a frame tagged under the zero state by hand: still refused */
	z.ready = true;
	CHECK(hifi_link_encode(&z, &f, b, sizeof(b)) == 80);
	z.ready = false;
	CHECK(!hifi_link_decode(&z, b, 80, &g));
	hifi_link_key_t k;
	hifi_link_key_init(&k, KEY);
	CHECK(k.ready && hifi_link_encode(&k, &f, b, sizeof(b)) == 80);
	hifi_link_key_wipe(&k);
	CHECK(!k.ready && !hifi_link_decode(&k, b, 80, &g));
	uint8_t key[HIFI_LINK_KEY] = {1};
	CHECK(!hifi_link_parse_key("0123456789abcdef0123456789abcdeg", key) && key[0] == 1);
}

/* the forger's view of the air: an ESP-NOW action frame around a link frame */
static size_t espnow_air(uint8_t *a, const uint8_t mac[6], const uint8_t *body, size_t n) {
	static const uint8_t oui[3] = {0x18, 0xfe, 0x34};
	memset(a, 0, HIFI_LINK_ESPNOW_BODY);
	a[0] = 0xd0;
	memset(a + 4, 0xff, 6);
	memcpy(a + 10, mac, 6);
	memset(a + 16, 0xff, 6);
	a[24] = 127;
	memcpy(a + 25, oui, 3);
	a[32] = 221;
	a[33] = (uint8_t) (n + 5);
	memcpy(a + 34, oui, 3);
	a[37] = 4;
	a[38] = 1;
	memcpy(a + HIFI_LINK_ESPNOW_BODY, body, n);
	return HIFI_LINK_ESPNOW_BODY + n;
}

static void espnow_body(void) {
	const uint8_t mac[6] = {0x30, 0x76, 0xf5, 0x2d, 0x74, 0x2c}, them[6] = {0x30, 0x76, 0xf5, 0x2d, 0x73, 0xe0};
	uint8_t body[HIFI_LINK_MAX], a[HIFI_LINK_ESPNOW_MAX + 8];
	memset(body, 0xab, sizeof(body));
	size_t n = espnow_air(a, mac, body, 200);
	CHECK(hifi_link_espnow_body(a, n, mac) == 200);
	CHECK(hifi_link_espnow_body(a, n + 4, mac) == 200); /* trailing bytes past the element */
	CHECK(hifi_link_espnow_body(a, n - 1, mac) == 0);   /* the element runs past the frame */
	CHECK(hifi_link_espnow_body(a, n, them) == 0);      /* another sender */
	CHECK(hifi_link_espnow_body(NULL, n, mac) == 0);
	/* the shortest and longest link frames */
	n = espnow_air(a, mac, body, HIFI_LINK_MIN);
	CHECK(n == HIFI_LINK_ESPNOW_BODY + HIFI_LINK_MIN && hifi_link_espnow_body(a, n, mac) == HIFI_LINK_MIN);
	n = espnow_air(a, mac, body, HIFI_LINK_MIN - 1);
	CHECK(hifi_link_espnow_body(a, n, mac) == 0);
	n = espnow_air(a, mac, body, HIFI_LINK_MAX);
	CHECK(n == HIFI_LINK_ESPNOW_MAX && hifi_link_espnow_body(a, n, mac) == HIFI_LINK_MAX);
	CHECK(hifi_link_espnow_body(a, n + 1, mac) == 0); /* longer than any ESP-NOW frame */
	/* one wrong byte in each field that marks it */
	const size_t at[] = {0, 24, 25, 32, 34, 37};
	for (size_t i = 0; i < sizeof(at) / sizeof(at[0]); i++) {
		n = espnow_air(a, mac, body, 100);
		a[at[i]] ^= 1;
		CHECK(hifi_link_espnow_body(a, n, mac) == 0);
	}
	/* an element length under 5 (it counts OUI, type and version) */
	n = espnow_air(a, mac, body, 100);
	for (uint8_t l = 0; l < 5; l++) {
		a[33] = l;
		CHECK(hifi_link_espnow_body(a, n, mac) == 0);
	}
}

static void seen(void) {
	hifi_link_seen_t s;
	memset(&s, 0, sizeof(s));
	CHECK(hifi_link_seen(&s, 0) == 0);
	CHECK(hifi_link_seen(&s, 0) == 1);
	CHECK(hifi_link_seen(&s, 5) == 0);
	CHECK(hifi_link_seen(&s, 3) == 0); /* late, but new */
	CHECK(hifi_link_seen(&s, 3) == 1);
	CHECK(hifi_link_seen(&s, 5 + HIFI_LINK_WINDOW) == 0);
	CHECK(hifi_link_seen(&s, 5) == -1);    /* fell out of the window */
	CHECK(hifi_link_seen(&s, 6) == 0); /* just inside: its old bit was cleared as the window moved */
	CHECK(hifi_link_seen(&s, 7 + HIFI_LINK_WINDOW / 2) == 0); /* inside the window, not seen */
	/* a slot reused by the moving window doesn't look like a duplicate */
	memset(&s, 0, sizeof(s));
	for (uint32_t q = 0; q < 5000; q += 2) CHECK(hifi_link_seen(&s, q) == 0);
	CHECK(hifi_link_seen(&s, 4997) == 0);
	CHECK(hifi_link_seen(&s, 4998) == 1);
	/* a jump far ahead clears everything */
	CHECK(hifi_link_seen(&s, 4000000000u) == 0);
	CHECK(hifi_link_seen(&s, 4000000000u - 1) == 0);
	/* the last value would wrap top to 0 and reset "expected" (found by fuzz_espnow) */
	CHECK(hifi_link_seen(&s, UINT32_MAX) == -1);
	CHECK(s.top == 4000000001u);
}

static void receiver(void) {
	hifi_link_rx_t r;
	hifi_link_rx_init(&r);
	hifi_link_frame_t f = {HIFI_LINK_DATA, 1, 0, 0, 0, 0, 250};
	/* 0..99 with 10 and 20 lost, 30 duplicated, 41 before 40 */
	int64_t now = 1000000;
	for (uint32_t q = 0; q < 100; q++) {
		if (q == 10 || q == 20) continue;
		if (q == 40) {
			f.seq = 41;
			hifi_link_rx_data(&r, &f, now += 1000);
			f.seq = 40;
			hifi_link_rx_data(&r, &f, now += 1000);
			q = 41;
			continue;
		}
		f.seq = q;
		hifi_link_rx_data(&r, &f, now += 1000);
		if (q == 30) hifi_link_rx_data(&r, &f, now += 1000);
	}
	hifi_link_rx_bad(&r);
	CHECK(r.rx == 98);
	CHECK(hifi_link_rx_expected(&r) == 100);
	CHECK(r.dup == 1 && r.reorder == 1 && r.bad == 1);
	CHECK(r.bytes == 98 * 250);
	CHECK_NEAR(hifi_link_rx_kbps(&r), 98 * 250 * 8 / 98.0, 0.1); /* first to last frame: 98 ms */

	char out[512];
	hj_t j;
	hj_init(&j, out, sizeof(out));
	hj_obj(&j, NULL);
	hifi_link_rx_json(&r, &j);
	hj_obj_end(&j);
	CHECK(!j.overflow && hj_valid_object(out));
	CHECK_HAS(out, "\"lost\":2");
	CHECK_HAS(out, "\"loss_pct\":2.000");

	/* a frame of another session while this one is live is a stray (a late
	 * frame of the last run must not wipe this run's counts) */
	f.session = 2;
	f.seq = 0;
	hifi_link_rx_data(&r, &f, now + 1000);
	CHECK(r.rx == 98 && r.session == 1 && r.stray == 1);
	/* once this one has been silent a second, a new session starts afresh,
	 * keeping the damaged and stray counts */
	hifi_link_rx_data(&r, &f, now + HIFI_LINK_SESSION_IDLE_US);
	CHECK(r.rx == 1 && r.session == 2 && r.dup == 0 && r.bad == 1 && r.stray == 1);
	f.session = 1; /* and the old one is now the stray */
	hifi_link_rx_data(&r, &f, now + HIFI_LINK_SESSION_IDLE_US + 10);
	CHECK(r.rx == 1 && r.session == 2 && r.stray == 2);
}

static void sender(void) {
	hifi_link_tx_t *t = calloc(1, sizeof(*t));
	hifi_link_tx_init(t, 9);
	CHECK(hifi_link_tx_rtt_pct(t, 50) < 0);
	double ppm;
	CHECK(!hifi_link_tx_drift_ppm(t, &ppm));

	/* The echo's clock reads ours + 5 s, running 20 ppm fast. 10 pings a
	 * second for 20 s; the round trip is 2 ms, every tenth 10 ms (and
	 * lopsided, so its offset is wrong: the best-of-second filter must
	 * drop it). The echo turns a ping round in 100 us. Ping 50 is lost. */
	hifi_link_rx_t echo;
	hifi_link_rx_init(&echo);
	int64_t a = 1000000;
	for (uint32_t q = 0; q < 200; q++, a += 100000) {
		hifi_link_frame_t ping = {HIFI_LINK_PING, 9, q, a, 0, 0, 64}, pong, got;
		uint8_t buf[HIFI_LINK_MAX];
		hifi_link_tx_sent(t, ping.len, true, a);
		if (q == 50) {
			hifi_link_tx_acked(t, false);
			continue;
		}
		hifi_link_tx_acked(t, true);
		int64_t out_us = (q % 10 == 9) ? 9000 : 950, back_us = (q % 10 == 9) ? 900 : 950;
		int64_t t2 = (int64_t) ((a + out_us) * (1 + 20e-6)) + 5000000;
		hifi_link_pong_for(&ping, t2, t2 + 100, &pong);
		CHECK(hifi_link_encode(&K, &pong, buf, sizeof(buf)) == 64 && hifi_link_decode(&K, buf, 64, &got));
		int64_t t4 = a + out_us + 100 + back_us;
		hifi_link_tx_pong(t, &got, t4);
		if (q == 7) hifi_link_tx_pong(t, &got, t4 + 5); /* duplicated */
	}
	hifi_link_frame_t stray = {HIFI_LINK_PONG, 8, 1, 0, 0, 0, 64};
	hifi_link_tx_pong(t, &stray, a);
	CHECK(t->sent == 200 && t->pongs == 199 && t->dup == 1 && t->stray == 1);
	CHECK(t->acked == 199 && t->nacked == 1);
	CHECK(t->rtt_min_us == 1900);
	CHECK_NEAR(hifi_link_tx_rtt_pct(t, 50), 1900, 100);
	CHECK(hifi_link_tx_rtt_pct(t, 99) > 9000);
	CHECK(t->rtt_max_us >= 9800 && t->rtt_max_us <= 9900);
	CHECK(t->windows >= 18);
	CHECK(hifi_link_tx_drift_ppm(t, &ppm));
	CHECK_NEAR(ppm, 20.0, 0.5);
	/* the last second is still open: the fit counts it */
	CHECK(t->win_any);
	{
		/* 2.5 s of pings: two windows closed, the third still open, and
		 * the fit has three points */
		hifi_link_tx_t *u = calloc(1, sizeof(*u));
		hifi_link_tx_init(u, 3);
		for (int q = 0; q < 25; q++) {
			int64_t t1 = 1000000 + q * 100000LL, t2 = (int64_t) (t1 * (1 + 50e-6)) + 1000;
			hifi_link_frame_t p = {HIFI_LINK_PONG, 3, (uint32_t) q, t1, t2, t2 + 10, 64};
			hifi_link_tx_sent(u, 64, true, t1);
			hifi_link_tx_pong(u, &p, t1 + 1010);
		}
		double d;
		CHECK(u->windows == 2 && u->win_any);
		CHECK(hifi_link_tx_drift_ppm(u, &d));
		CHECK_NEAR(d, 50.0, 1.0);
		free(u);
	}
	/* offset: 5 s plus the drift so far */
	CHECK_NEAR((double) t->offset_us, 5000000 + 20e-6 * 19.5e6, 30);

	char out[1024];
	hj_t j;
	hj_init(&j, out, sizeof(out));
	hj_obj(&j, NULL);
	hifi_link_tx_json(t, &j);
	hj_obj_end(&j);
	CHECK(!j.overflow && hj_valid_object(out));
	CHECK_HAS(out, "\"lost\":1");
	CHECK_HAS(out, "\"drift_ppm\":");
	CHECK_HAS(out, "\"p95\":");

	/* a pong from before the sender's own clock, or a negative delay */
	hifi_link_frame_t odd = {HIFI_LINK_PONG, 9, 500, a + 10, a + 5, a, 64};
	hifi_link_tx_pong(t, &odd, a);
	CHECK(t->bad == 1);

	/* a slow pong lands in the last bin, not past the end */
	hifi_link_frame_t slow = {HIFI_LINK_PONG, 9, 501, a, a, a, 64};
	hifi_link_tx_pong(t, &slow, a + 5000000);
	CHECK(t->rtt_bins[HIFI_LINK_RTT_BINS - 1] == 1);
	CHECK(hifi_link_tx_rtt_pct(t, 100) == 5000000);

	/* hostile times off the radio: no overflow (UBSan), counted as bad */
	uint32_t bad0 = t->bad, pongs0 = t->pongs, dup0 = t->dup;
	hifi_link_frame_t h1 = {HIFI_LINK_PONG, 9, 502, INT64_MIN, 0, 0, 64};
	hifi_link_tx_pong(t, &h1, a);
	hifi_link_frame_t h2 = {HIFI_LINK_PONG, 9, 503, a, INT64_MIN, INT64_MAX, 64};
	hifi_link_tx_pong(t, &h2, a + 10);
	hifi_link_frame_t h3 = {HIFI_LINK_PONG, 9, 504, a, INT64_MAX, INT64_MIN, 64};
	hifi_link_tx_pong(t, &h3, a + 10);
	hifi_link_frame_t h4 = {HIFI_LINK_PONG, 9, 505, a, INT64_MAX - 5, INT64_MAX, 64};
	hifi_link_tx_pong(t, &h4, a + 10);
	CHECK(t->bad == bad0 + 4 && t->pongs == pongs0);
	/* in range, but round trips the run can't have had: rtt_sum_us would
	 * overflow after two (found by fuzz_espnow); late, over the cap */
	int64_t far = ((int64_t) 1 << 62) - 1;
	for (uint32_t q = 506; q < 509; q++) {
		hifi_link_frame_t h = {HIFI_LINK_PONG, 9, q, 0, 5, 5, 64};
		hifi_link_tx_pong(t, &h, far);
	}
	hifi_link_frame_t late = {HIFI_LINK_PONG, 9, 509, a, a, a, 64};
	hifi_link_tx_pong(t, &late, a + HIFI_LINK_RTT_MAX_US + 1);
	CHECK(t->bad == bad0 + 8 && t->pongs == pongs0);
	/* a forged PONG doesn't use up the seq of the real one */
	hifi_link_frame_t real = {HIFI_LINK_PONG, 9, 509, a, a + 10, a + 20, 64};
	hifi_link_tx_pong(t, &real, a + 1000);
	CHECK(t->pongs == pongs0 + 1 && t->dup == dup0);
	free(t);
}

static void macs(void) {
	uint8_t m[6];
	CHECK(hifi_link_parse_mac("30:76:f5:2d:74:3c", m) && m[0] == 0x30 && m[5] == 0x3c);
	CHECK(hifi_link_parse_mac("3076F52D742C", m) && m[2] == 0xf5 && m[5] == 0x2c);
	CHECK(hifi_link_parse_mac("30-76-f5-2d-74-3c", m));
	CHECK(!hifi_link_parse_mac("30:76:f5:2d:74", m));
	CHECK(!hifi_link_parse_mac("30:76:f5:2d:74:3g", m));
	CHECK(!hifi_link_parse_mac("30:76:f5:2d:74x3c", m));
	CHECK(!hifi_link_parse_mac("", m) && !hifi_link_parse_mac(NULL, m));
}

/* the dial-in watch: a restart only after a long time off it, and only once it was on it */
static void link_watch(void) {
	hifi_lab_link_t w = {0};
	CHECK(!hifi_lab_link_tick(&w, 1000, true, false));                          /* never on it: setup AP, no network */
	CHECK(!hifi_lab_link_tick(&w, 1000 + 2 * HIFI_LAB_OFFLINE_MS, true, false));
	CHECK(!hifi_lab_link_tick(&w, 5000, true, true));
	CHECK(!hifi_lab_link_tick(&w, 6000, true, false));                          /* dropped at 6 s */
	CHECK(!hifi_lab_link_tick(&w, 6000 + HIFI_LAB_OFFLINE_MS - 1, true, false));
	CHECK(hifi_lab_link_tick(&w, 6000 + HIFI_LAB_OFFLINE_MS, true, false));
	/* back on it: the clock starts again from the next drop */
	CHECK(!hifi_lab_link_tick(&w, 7000 + HIFI_LAB_OFFLINE_MS, true, true));
	CHECK(!hifi_lab_link_tick(&w, 8000 + HIFI_LAB_OFFLINE_MS, true, false));
	CHECK(!hifi_lab_link_tick(&w, 8000 + 2 * HIFI_LAB_OFFLINE_MS - 1, true, false));
	CHECK(hifi_lab_link_tick(&w, 8000 + 2 * HIFI_LAB_OFFLINE_MS, true, false));
	/* across the millisecond counter's wrap (49 days) */
	hifi_lab_link_t z = {0};
	CHECK(!hifi_lab_link_tick(&z, 0xffffff00u, true, true));
	CHECK(!hifi_lab_link_tick(&z, 0xffffff10u, true, false));
	CHECK(!hifi_lab_link_tick(&z, 0x100u, true, false));
	CHECK(hifi_lab_link_tick(&z, 0xffffff10u + HIFI_LAB_OFFLINE_MS, true, false));
	/* the dial-in turned off (not configured) resets the watch: set again, it starts afresh */
	hifi_lab_link_t c = {0};
	CHECK(!hifi_lab_link_tick(&c, 1000, true, true));
	CHECK(!hifi_lab_link_tick(&c, 2000, true, false));
	CHECK(!hifi_lab_link_tick(&c, 3000, false, false));
	CHECK(!hifi_lab_link_tick(&c, 2000 + HIFI_LAB_OFFLINE_MS, true, false)); /* never on it since */
	CHECK(!hifi_lab_link_tick(&c, 2000 + 3 * HIFI_LAB_OFFLINE_MS, true, false));
}

int main(void) {
	hifi_link_key_init(&K, KEY);
	macs();
	link_watch();
	lab_frame();
	lab_route();
	frames();
	tag();
	forged();
	forger();
	replay();
	key_ready();
	espnow_body();
	seen();
	receiver();
	sender();
	return check_report("lab");
}
