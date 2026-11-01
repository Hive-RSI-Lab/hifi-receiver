/* The core board's (Plus/Ultra) logic: the expander's knob and buttons, the
 * PD request, its power and governor numbers, and the update manifest. */
#include "check.h"
#include "hifi_exp.h"
#include "hifi_gov.h"
#include "hifi_pd.h"
#include "hifi_power.h"
#include "hifi_update.h"
#include "hifi_vol.h"

/* the core board's expander: knob on P12/P13, knob push P14, buttons P15/P16 */
/* as board_core.c wires it: Alps A (P12, bit 10) is the decoder's enc_b (R5-UF-1) */
static const hifi_exp_cfg_t CFG = {.enc_a = 11, .enc_b = 10, .edges_per_detent = 4, .buttons = {12, 13, 14, -1}};
#define IDLE 0xFFFF /* everything pulled up */

static uint16_t knob(int ab) { return (uint16_t) ((IDLE & ~(3u << 10)) | (ab >> 1 & 1) << CFG.enc_a | (ab & 1) << CFG.enc_b); }

/* knob(ab) takes the decoder's frame: ab = (enc_a, enc_b) = (Alps B, Alps A).
 * One detent clockwise from rest, A leading (Alps EC11E): 11 -> 10 -> 00 -> 01 -> 11 here,
 * i.e. 11 -> 01 -> 00 -> 10 -> 11 in (A, B) */
static int turn(hifi_exp_t *e, int dir, uint32_t *t, hifi_exp_event_t *ev) {
	static const int cw[4] = {2, 0, 1, 3}, ccw[4] = {1, 0, 2, 3};
	int n = 0;
	for (int i = 0; i < 4; i++) n += hifi_exp_input(e, knob(dir > 0 ? cw[i] : ccw[i]), *t += 2, ev + n, 4 - n);
	return n;
}

static void knob_detents(void) {
	hifi_exp_t e;
	hifi_exp_event_t ev[4];
	uint32_t t = 1000;
	hifi_exp_init(&e, &CFG);
	CHECK(hifi_exp_input(&e, IDLE, t, ev, 4) == 0);

	int sum = 0;
	for (int k = 0; k < 3; k++) {
		int n = turn(&e, +1, &t, ev);
		CHECK(n == 1);
		CHECK(n == 1 && ev[0].kind == HIFI_EXP_KNOB);
		if (n == 1) sum += ev[0].steps;
	}
	CHECK(sum == 3);
	int n = turn(&e, -1, &t, ev);
	CHECK(n == 1 && ev[0].steps == -1);

	/* contact bounce at rest (11 -> 10 -> 11) is no detent */
	CHECK(hifi_exp_input(&e, knob(2), t += 1, ev, 4) == 0);
	CHECK(hifi_exp_input(&e, knob(3), t += 1, ev, 4) == 0);

	/* half a turn and back: nothing */
	CHECK(hifi_exp_input(&e, knob(2), t += 2, ev, 4) == 0);
	CHECK(hifi_exp_input(&e, knob(0), t += 2, ev, 4) == 0);
	CHECK(hifi_exp_input(&e, knob(2), t += 2, ev, 4) == 0);
	CHECK(hifi_exp_input(&e, knob(3), t += 2, ev, 4) == 0);

	/* a fast turn read late: one edge missed (10 -> 01 skips 00), still a detent */
	CHECK(hifi_exp_input(&e, knob(2), t += 2, ev, 4) == 0);
	CHECK(hifi_exp_input(&e, knob(1), t += 2, ev, 4) == 0);
	n = hifi_exp_input(&e, knob(3), t += 2, ev, 4);
	CHECK(n == 1 && ev[0].steps == 1);

	/* a knob with half a cycle a click rests at 00 too */
	hifi_exp_cfg_t half = CFG;
	half.edges_per_detent = 2;
	hifi_exp_init(&e, &half);
	hifi_exp_input(&e, knob(3), t, ev, 4);
	CHECK(hifi_exp_input(&e, knob(2), t += 2, ev, 4) == 0);
	n = hifi_exp_input(&e, knob(0), t += 2, ev, 4);
	CHECK(n == 1 && ev[0].steps == 1);
	CHECK(hifi_exp_input(&e, knob(1), t += 2, ev, 4) == 0);
	n = hifi_exp_input(&e, knob(3), t += 2, ev, 4);
	CHECK(n == 1 && ev[0].steps == 1);
}

static void buttons(void) {
	hifi_exp_t e;
	hifi_exp_event_t ev[4];
	uint32_t t = 0;
	hifi_exp_init(&e, &CFG);
	hifi_exp_input(&e, IDLE, t, ev, 4);
	uint16_t b1 = IDLE & ~(1u << 13); /* BTN_1 (P15) down */

	/* a 10 ms glitch is no press */
	CHECK(hifi_exp_input(&e, b1, t += 10, ev, 4) == 0);
	CHECK(hifi_exp_input(&e, IDLE, t += 10, ev, 4) == 0);
	CHECK(hifi_exp_input(&e, IDLE, t += 100, ev, 4) == 0);

	/* a short press: the event comes on release */
	CHECK(hifi_exp_input(&e, b1, t += 10, ev, 4) == 0);
	CHECK(hifi_exp_input(&e, b1, t += 40, ev, 4) == 0);
	CHECK(hifi_exp_input(&e, IDLE, t += 200, ev, 4) == 0);
	int n = hifi_exp_input(&e, IDLE, t += 40, ev, 4);
	CHECK(n == 1 && ev[0].kind == HIFI_EXP_PRESS && ev[0].button == 1);

	/* a long press fires once while held, and nothing on release */
	hifi_exp_input(&e, b1, t += 10, ev, 4);
	CHECK(hifi_exp_input(&e, b1, t += 40, ev, 4) == 0);
	CHECK(hifi_exp_input(&e, b1, t += 900, ev, 4) == 0);
	n = hifi_exp_input(&e, b1, t += 100, ev, 4);
	CHECK(n == 1 && ev[0].kind == HIFI_EXP_LONG && ev[0].button == 1);
	CHECK(hifi_exp_input(&e, b1, t += 2000, ev, 4) == 0);
	hifi_exp_input(&e, IDLE, t += 10, ev, 4);
	CHECK(hifi_exp_input(&e, IDLE, t += 40, ev, 4) == 0);

	/* a button held through boot gives nothing until pressed again */
	hifi_exp_init(&e, &CFG);
	hifi_exp_input(&e, b1, t, ev, 4);
	CHECK(hifi_exp_input(&e, b1, t += 3000, ev, 4) == 0);
	hifi_exp_input(&e, IDLE, t += 10, ev, 4);
	CHECK(hifi_exp_input(&e, IDLE, t += 40, ev, 4) == 0);

	CHECK(hifi_exp_bit(0x00, 3, true) == 0x08);
	CHECK(hifi_exp_bit(0xFF, 0, false) == 0xFE);
}

static void knob_volume(void) {
	hifi_vol_t v;
	hifi_vol_init(&v, 30, 80);
	hifi_vol_set(&v, -1, 1, -1);
	hifi_vol_turn(&v, 3);
	CHECK(v.volume == 36 && !v.mute);
	hifi_vol_turn(&v, -40);
	CHECK(v.volume == 0);
	hifi_vol_turn(&v, 60);
	CHECK(v.volume == 80); /* held to the maximum */
}

static void pd_request(void) {
	uint8_t code = 0xFF;
	/* before the PD handshake: nothing asked, VBUS taken as 5 V */
	CHECK_NEAR(hifi_ch224_request(0x00, NULL, 0, &code), 0, 0);
	CHECK_NEAR(hifi_ch224_request(0x02 | CH224_ST_EPR_OFFERED, NULL, 0, &code), 0, 0); /* QC2 */
	CHECK(code == 0xFF);
	CHECK_NEAR(hifi_ch224_vbus(0x02, 0), 5, 0);
	/* PD, EPR offered: ask for 28 V; in EPR it is 28 V */
	CHECK_NEAR(hifi_ch224_request(0x08 | CH224_ST_EPR_OFFERED, NULL, 0, &code), 28, 0);
	CHECK(code == 5);
	CHECK_NEAR(hifi_ch224_vbus(0x08 | CH224_ST_EPR_OFFERED, 28), 20, 0);
	CHECK_NEAR(hifi_ch224_vbus(0x10 | CH224_ST_EPR_OFFERED, 28), 28, 0);
	CHECK_NEAR(hifi_ch224_request(0x10, NULL, 0, &code), 28, 0);
	/* PD without EPR and no capability list: 20 V */
	CHECK_NEAR(hifi_ch224_request(0x08, NULL, 0, &code), 20, 0);
	CHECK(code == 4);
	CHECK_NEAR(hifi_ch224_vbus(0x08, 0), 20, 0);

	/* a 30 W phone charger: 5/9/15 V fixed plus a PPS APDO, as a PD
	 * message (header: Source_Capabilities, 4 objects) */
	uint32_t pdos[4] = {
		(100u << 10) | 300,             /* 5 V 3 A */
		(180u << 10) | 300,             /* 9 V 3 A */
		(300u << 10) | 200,             /* 15 V 2 A */
		(3u << 30) | (110u << 17) | 60, /* PPS 3.3-11 V */
	};
	uint8_t raw[CH224_SRCCAP_LEN] = {0};
	uint16_t hdr = 1 | 4 << 12;
	raw[0] = hdr & 0xff;
	raw[1] = hdr >> 8;
	for (int k = 0; k < 4; k++)
		for (int b = 0; b < 4; b++) raw[2 + 4 * k + b] = pdos[k] >> (8 * b) & 0xff;
	float v[7], i[7];
	int n = hifi_pd_srccap_fixed(raw, sizeof raw, v, i, 7);
	CHECK(n == 3);
	CHECK_NEAR(v[2], 15, 0.01);
	CHECK_NEAR(i[2], 2, 0.01);
	CHECK_NEAR(hifi_ch224_request(0x08, v, n, &code), 15, 0);
	CHECK(code == 3);
	CHECK_NEAR(hifi_ch224_vbus(0x08, 15), 15, 0);
	/* the same PDOs without the header */
	uint8_t bare[CH224_SRCCAP_LEN] = {0};
	for (int k = 0; k < 4; k++)
		for (int b = 0; b < 4; b++) bare[4 * k + b] = pdos[k] >> (8 * b) & 0xff;
	CHECK(hifi_pd_srccap_fixed(bare, sizeof bare, v, i, 7) == 3);
	/* nonsense (all zero, or no 5 V first): unknown */
	uint8_t zero[CH224_SRCCAP_LEN] = {0};
	CHECK(hifi_pd_srccap_fixed(zero, sizeof zero, v, i, 7) == 0);
	/* a 5/9/12 V-only charger: 12 V, under core's 14 V start floor */
	float v12[3] = {5, 9, 12};
	CHECK_NEAR(hifi_ch224_request(0x08, v12, 3, &code), 12, 0);
	CHECK(code == 2);

	/* EPR offered but not yet entered: not settled, for up to the wait */
	CHECK(!hifi_ch224_settled(0x08 | CH224_ST_EPR_OFFERED, 100, 3000));
	CHECK(hifi_ch224_settled(0x08 | CH224_ST_EPR_OFFERED, 3000, 3000));
	CHECK(hifi_ch224_settled(0x10 | CH224_ST_EPR_OFFERED, 0, 3000));
	CHECK(hifi_ch224_settled(0x08, 0, 3000));
	CHECK(!hifi_ch224_settled(0x02, 9999, 3000));
}

static void core_power_gov(void) {
	hifi_power_cfg_t pc;
	hifi_power_cfg_core(&pc);
	hifi_power_t p;
	hifi_power_init(&p, &pc, 0);
	uint32_t t = 0;
	hifi_power_in_t in = {.vbus_v = 28, .pg = true, .audio = true};
	/* 500 ms settle, then 500 ms for 1410 uF of PVDD (R3-POWER-2/5) */
	for (; t < 990; t += 10) {
		in.now_ms = t;
		hifi_power_step(&p, &in);
	}
	CHECK(p.state == HIFI_PWR_SOFT_START && p.out.pvdd_en && !p.out.amp_on);
	for (; t < 1500; t += 10) {
		in.now_ms = t;
		hifi_power_step(&p, &in);
	}
	CHECK(p.state == HIFI_PWR_ON && p.out.pvdd_en && p.out.amp_on);
	/* a 9 V contract never starts the amps */
	hifi_power_init(&p, &pc, 0);
	in.vbus_v = 9;
	for (t = 0; t < 2000; t += 10) {
		in.now_ms = t;
		hifi_power_step(&p, &in);
	}
	CHECK(p.state == HIFI_PWR_WAIT_PD && !p.out.pvdd_en);

	/* 140 W EPR: a full-scale sine on both channels into 4 ohm wants
	 * 2 x 25.8^2 / 8 = 166 W out, ~193 W in: held to 126 W in */
	hifi_gov_cfg_t gc;
	hifi_gov_cfg_core(&gc);
	float thermal = gc.thermal_out_w;
	gc.thermal_out_w = 0; /* the supply budget alone first */
	hifi_gov_t g;
	hifi_gov_init(&g, &gc);
	hifi_gov_set_supply(&g, 28, 140);
	static int16_t buf[2 * 480];
	for (int i = 0; i < 480; i++) buf[2 * i] = buf[2 * i + 1] = (int16_t) (32767 * sin(2 * M_PI * 1000 * i / 48000.0));
	static int16_t blk[2 * 480];
	for (int k = 0; k < 600; k++) {
		memcpy(blk, buf, sizeof(buf));
		hifi_gov_process(&g, blk, 480, 4, 48000);
	}
	CHECK(g.limiting);
	CHECK_NEAR(hifi_gov_in_w(&g), 126, 3);
	float one = hifi_gov_gain_db(&g);
	/* zone B on as well: twice the amps, a quarter of the power each, -3 dB more */
	g.amps = 2;
	for (int k = 0; k < 600; k++) {
		memcpy(blk, buf, sizeof(buf));
		hifi_gov_process(&g, blk, 480, 4, 48000);
	}
	CHECK_NEAR(hifi_gov_in_w(&g), 126, 3);
	CHECK_NEAR(hifi_gov_gain_db(&g) - one, -3.0, 0.3);

	/* the TPA3220's heat: a -6 dBFS sine (2 x 20.8 W into 4 ohm) is well
	 * inside the 140 W supply, so it plays untouched for seconds, but the
	 * minute's average comes down to the amp's 9 W */
	gc.thermal_out_w = thermal;
	hifi_gov_init(&g, &gc);
	hifi_gov_set_supply(&g, 28, 140);
	for (int i = 0; i < 2 * 480; i++) blk[i] = buf[i] / 2;
	static int16_t half[2 * 480];
	memcpy(half, blk, sizeof(half));
	for (int k = 0; k < 500; k++) {
		memcpy(blk, half, sizeof(half));
		hifi_gov_process(&g, blk, 480, 4, 48000);
	}
	CHECK(!g.limiting);
	CHECK_NEAR(hifi_gov_out_w(&g), 41.5, 2);
	for (int k = 0; k < 30000; k++) {
		memcpy(blk, half, sizeof(half));
		hifi_gov_process(&g, blk, 480, 4, 48000);
	}
	CHECK(g.limiting);
	CHECK_NEAR(hifi_gov_out_w(&g), 9, 1);
}

static void manifest(void) {
	hifi_update_offer_t o;
	const char *err = NULL;
	const char *sha = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
	char m[1024];
	snprintf(m, sizeof(m),
		 "{\"latest\":\"0.2.0\",\"notes\":\"Zone B\",\"images\":{"
		 "\"hifi-lite\":{\"url\":\"https://x.example/lite.bin\",\"sha256\":\"%s\"},"
		 "\"hifi-plus\":{\"url\":\"plus-0.2.0.bin\",\"sha256\":\"%s\"}}}",
		 sha, sha);
	CHECK(hifi_update_parse(m, "http://hive.local:8080/fw/manifest.json", "hifi-plus", &o, &err) == 0);
	CHECK_STR(o.latest, "0.2.0");
	CHECK_STR(o.url, "http://hive.local:8080/fw/plus-0.2.0.bin");
	CHECK_STR(o.sha256, sha);
	CHECK_STR(o.notes, "Zone B");
	CHECK(hifi_update_parse(m, NULL, "hifi-lite", &o, &err) == 0);
	CHECK_STR(o.url, "https://x.example/lite.bin");
	CHECK(hifi_update_parse(m, "http://h/m.json", "hifi-ultra", &o, &err) == -1);
	CHECK_STR(err, "the update manifest has no image for this model");

	/* one image at the top level; a root-relative url; no sha256 */
	CHECK(hifi_update_parse("{\"latest\":\"0.3\",\"url\":\"/img/a.bin\"}", "http://h:1/fw/m.json", "any", &o, &err) == 0);
	CHECK_STR(o.url, "http://h:1/img/a.bin");
	CHECK_STR(o.sha256, "");
	CHECK(hifi_update_parse("{\"latest\":\"0.3\",\"url\":\"a.bin\"}", "http://h", "any", &o, &err) == 0);
	CHECK_STR(o.url, "http://h/a.bin");

	CHECK(hifi_update_parse("{\"url\":\"a.bin\"}", NULL, "any", &o, &err) == -1);
	CHECK(hifi_update_parse("[1]", NULL, "any", &o, &err) == -1);
	CHECK(hifi_update_parse("{\"latest\":\"1\",\"url\":\"a\",\"sha256\":\"xyz\"}", NULL, "any", &o, &err) == -1);
	CHECK_STR(hifi_update_state_name(HIFI_UPD_FLASHING), "flashing");
}

int main(void) {
	knob_detents();
	buttons();
	knob_volume();
	pd_request();
	core_power_gov();
	manifest();
	return check_report("core");
}
