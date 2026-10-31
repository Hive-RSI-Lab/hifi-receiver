/* review-5 UF: the board's knob decode against the EC11E1834403.
 * Alps EC11 catalogue (LCSC C361165 copy) p.1 product table: EC11E1834403 =
 * vertical, 20 mm flat shaft, torque 7 +3/-4 mN.m, detents NONE, 18 pulses
 * per rev, push switch. So 72 quadrature edges a turn and no rest position.
 * Checks, with the cfg board_core.c uses (edges_per_detent = 4):
 *  1. one turn CW gives 18 steps (one per pulse), one turn CCW gives -18;
 *  2. contact chatter at any position adds nothing (a cycle with bounces = 1 step);
 *  3. a stop anywhere mid-cycle and a turn back gives back what it counted;
 *  4. DIRECTION: clockwise seen from the knob must count + (hifi_exp.h, hifi_vol.h:
 *     "+ clockwise" = volume up). Alps p.282 output waveform, CW: A changes a
 *     quarter cycle before B (A leads); OFF = open = 1 through the 10k pull-ups,
 *     so CW from rest 11 is 11 -> 01 -> 00 -> 10 -> 11 (A,B). The footprint
 *     SW-TH_EC11E1820402 at 180 deg puts pad A on Alps terminal A (fig. 5, seen
 *     from the insertion side), and A -> ENC_A -> core U17 P12 = bit 10 = enc_a.
 * Build/run: cc -I../../../../firmware/components/hifi/core enc_rev.c \
 *   ../../../../firmware/components/hifi/core/hifi_exp.c -o /tmp/enc && /tmp/enc
 * Exit 1 on any failure. */
#include <stdio.h>
#include "hifi_exp.h"
static hifi_exp_t e; static hifi_exp_event_t ev[4]; static uint32_t t;
static int a = 1, b = 1, steps;
static void feed(void) {
	uint16_t p = (uint16_t)((0xFFFF & ~(3u << 10)) | a << 10 | b << 11);
	int n = hifi_exp_input(&e, p, t += 3, ev, 4);
	for (int i = 0; i < n; i++) if (ev[i].kind == HIFI_EXP_KNOB) steps += ev[i].steps;
}
/* Gray sequence a,b: 11 -> 01 -> 00 -> 10 -> 11 is +1 here = Alps clockwise (A leads) */
static void edge(int dir) {
	static const int seq[4][2] = {{1, 1}, {0, 1}, {0, 0}, {1, 0}};
	int k = 0; for (; k < 4; k++) if (seq[k][0] == a && seq[k][1] == b) break;
	k = (k + (dir > 0 ? 1 : 3)) % 4; a = seq[k][0]; b = seq[k][1]; feed();
}
int main(void) {
	hifi_exp_cfg_t c = {.enc_a = 11, .enc_b = 10, /* board_core.c's wiring after R5-UF-1 */ .edges_per_detent = 4, .buttons = {-1, -1, -1, -1}};
	int fail = 0;
	hifi_exp_init(&e, &c); feed();
	steps = 0; for (int i = 0; i < 72; i++) edge(+1);
	int cw = steps;
	steps = 0; for (int i = 0; i < 72; i++) edge(-1);
	int ccw = steps;
	printf("one turn: %d steps one way, %d the other (expect +-18)\n", cw, ccw);
	fail |= !((cw == 18 && ccw == -18) || (cw == -18 && ccw == 18));
	printf("direction: clockwise (A leads) counts %+d per turn: %s\n", cw,
	       cw > 0 ? "volume up, as hifi_vol.h says" : "VOLUME DOWN: the knob runs backwards");
	fail |= cw <= 0;
	/* advance one full cycle an edge at a time, bouncing 20 times at each of the
	 * 4 positions: the bounces must add nothing, so the total is one step */
	steps = 0;
	for (int pos = 0; pos < 4; pos++) {
		for (int j = 0; j < 20; j++) { edge(+1); edge(-1); }
		edge(+1);
	}
	int chat = steps;
	printf("one cycle with 20 bounces at each of its 4 positions: %d steps (expect +-1)\n", chat);
	fail |= !(chat == 1 || chat == -1);
	steps = 0; for (int i = 0; i < 6; i++) edge(+1); for (int i = 0; i < 6; i++) edge(-1);
	printf("6 edges on, 6 back: net %d (expect 0)\n", steps);
	fail |= steps != 0;
	printf("%s\n", fail ? "FAIL" : "PASS");
	return fail;
}
