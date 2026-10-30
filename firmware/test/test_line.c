/* The core board's line in: I2S words, the sub mix, DJ direct's thermal guard */
#include <stdint.h>

#include "check.h"
#include "hifi_gov.h"
#include "hifi_line.h"

static void words(void) {
	int32_t in[6] = {0x12340000, (int32_t) 0xFFFF8000, (int32_t) 0x80000000, 0x7FFF0000, 0x0000FFFF, -65536};
	int16_t out[6];
	hifi_line_to_s16(in, out, 3, false);
	CHECK(out[0] == 0x1234 && out[1] == -1 && out[2] == -32768 && out[3] == 32767 && out[4] == 0 && out[5] == -1);
	hifi_line_to_s16(in, out, 3, true);
	/* C5: the subs follow the inverted analog path; -32768 doesn't wrap */
	CHECK(out[0] == -0x1234 && out[1] == 1 && out[2] == 32767 && out[3] == -32767 && out[5] == 1);
}

static void sub_mix(void) {
	int16_t s16[4] = {1000, 3000, 32767, 32767};
	int32_t out[4];
	hifi_sub_mix(s16, 4, out, 2, 1.0f);
	CHECK(out[0] == 2000 << 16 && out[1] == 2000 << 16);
	CHECK(out[2] == 32767 << 16 && out[3] == out[2]);
	hifi_sub_mix(s16, 4, out, 2, 0.5f);
	CHECK(out[0] == 1000 << 16);
	int32_t s32[4] = {-20000 * 65536, -30000 * 65536, 0, 0};
	hifi_sub_mix(s32, 8, out, 2, 2.0f); /* a boost clips, it doesn't wrap */
	CHECK(out[0] == (int32_t) ((uint32_t) -32768 << 16) && out[2] == 0);
	hifi_sub_mix(s32, 8, out, 2, 0.0f); /* the sub off */
	CHECK(out[0] == 0 && out[1] == 0);
}

static void block_power(void) {
	hifi_gov_cfg_t c;
	hifi_gov_cfg_core(&c);
	int16_t sine[2 * 480];
	for (int i = 0; i < 480; i++) sine[2 * i] = sine[2 * i + 1] = (int16_t) lrint(32767 * sin(2 * M_PI * i / 48.0));
	/* a full-scale sine on both channels: vfs^2 / (2 R) each */
	double want = 2 * c.vfs_peak_v * c.vfs_peak_v / (2 * c.r_load);
	CHECK_NEAR(hifi_line_block_w(&c, sine, 480, 1), want, want * 0.01);
	CHECK_NEAR(hifi_line_block_w(&c, sine, 480, 2), 2 * want, want * 0.02);
	CHECK(hifi_line_block_w(&c, sine, 0, 1) == 0);
}

/* runs DJ direct for `s` seconds at `out_w` of speaker power; the seconds it tripped, or -1 */
static double run(hifi_djg_t *g, float out_w, float meas_w, double s, float limit) {
	uint32_t t = 0;
	for (int i = 0; i < s * 100; i++, t += 10) {
		hifi_djg_audio(g, out_w, 0.01f);
		if (hifi_djg_step(g, t, 0.01f, meas_w, false, limit)) return t / 1000.0;
	}
	return -1;
}

static void guard(void) {
	hifi_gov_cfg_t ultra, plus;
	hifi_gov_cfg_core(&ultra);
	plus = ultra;
	plus.thermal_out_w = 19.0f; /* board_core.c: Plus splits the pair across both chips */
	hifi_djg_cfg_t dc;
	hifi_djg_cfg_default(&dc, &ultra);
	hifi_djg_t g;

	/* review-3 R3-SYSTEM-4: Ultra one zone ~14 W in, Plus ~26 W */
	CHECK_NEAR(hifi_djg_limit_w(&ultra, 1), 14.2, 0.1);
	CHECK_NEAR(hifi_djg_limit_w(&ultra, 2), 23.3, 0.1);
	CHECK_NEAR(hifi_djg_limit_w(&plus, 1), 25.6, 0.1);

	/* the FLX4 at 2.1 Vrms, DJ program 8 dB under its peak: ~8 W/ch on Ultra,
	 * 1.8x the budget. It trips inside the minute, before the chip gets to OTE */
	hifi_djg_init(&g, &dc);
	double t = run(&g, 16.0f, NAN, 300, hifi_djg_limit_w(&ultra, 1));
	CHECK(t > 30 && t < 60);
	CHECK_STR(g.tripped, "power");
	/* the same on Plus is 0.7x its budget: it plays on */
	hifi_djg_init(&g, &dc);
	CHECK(run(&g, 13.4f, NAN, 600, hifi_djg_limit_w(&plus, 1)) < 0);
	CHECK(g.tripped == NULL);
	CHECK_NEAR(hifi_djg_in_w(&g), 13.4 / 0.88 + 4, 0.1);

	/* the INA186 sees more than the estimate (a hungrier load than 4 ohm):
	 * the larger wins */
	hifi_djg_init(&g, &dc);
	CHECK(run(&g, 2.0f, 30.0f, 300, 25.6f) > 0);
	/* quiet: an idle box never trips */
	hifi_djg_init(&g, &dc);
	CHECK(run(&g, 0.0f, 4.5f, 600, 14.2f) < 0);

	/* OTW: a 150 ms blip passes, 200 ms held trips */
	hifi_djg_init(&g, &dc);
	uint32_t now = 1000;
	for (int i = 0; i < 15; i++, now += 10) CHECK(!hifi_djg_step(&g, now, 0.01f, NAN, true, 14.2f));
	CHECK(!hifi_djg_step(&g, now, 0.01f, NAN, false, 14.2f));
	now += 10;
	const char *why = NULL;
	int n = 0;
	for (; n < 30 && !why; n++, now += 10) why = hifi_djg_step(&g, now, 0.01f, NAN, true, 14.2f);
	CHECK_STR(why, "otw");
	CHECK(n == 21); /* the step at 200 ms */
	/* a reset starts the averages, and keeps what tripped it last */
	hifi_djg_reset(&g);
	CHECK(g.out_w == 0 && g.meas_w == 0);
	CHECK_STR(g.tripped, "otw");
}

int main(void) {
	words();
	sub_mix();
	block_power();
	guard();
	return check_report("line");
}
