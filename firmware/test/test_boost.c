/* Boost (hifi_boost) and the governor's heat cap (hifi_gov), ask k2388 */
#include <stdint.h>
#include <stdlib.h>

#include "check.h"
#include "hifi_boost.h"
#include "hifi_gov.h"
#include "hifi_line.h"

#define RATE 48000
#define BLOCK 256

static int16_t b16[2 * BLOCK];
static int32_t b32[2 * BLOCK];
static double phase;

static void sine16(double dbfs, double hz) {
	double a = 32767.0 * pow(10, dbfs / 20);
	for (int i = 0; i < BLOCK; i++, phase += 2 * M_PI * hz / RATE) {
		int16_t v = (int16_t) lrint(a * sin(phase));
		b16[2 * i] = v;
		b16[2 * i + 1] = (int16_t) -v;
	}
}

static void sine32(double dbfs, double hz) {
	double a = 2147483647.0 * pow(10, dbfs / 20);
	for (int i = 0; i < BLOCK; i++, phase += 2 * M_PI * hz / RATE) {
		int32_t v = (int32_t) lrint(a * sin(phase));
		b32[2 * i] = v;
		b32[2 * i + 1] = v;
	}
}

/* plays `s` seconds of a sine through the boost; returns the last half
 * second's RMS (dB vs a full-scale sine) and the loudest sample (FS) */
static double run16(hifi_boost_t *b, double dbfs, double hz, double s, double *peak) {
	int blocks = (int) (s * RATE / BLOCK), from = blocks - RATE / 2 / BLOCK;
	double acc = 0, pk = 0;
	long n = 0;
	for (int k = 0; k < blocks; k++) {
		sine16(dbfs, hz);
		hifi_boost_process(b, b16, BLOCK, 4, RATE);
		for (int i = 0; i < 2 * BLOCK; i++) {
			double v = fabs(b16[i] / 32768.0);
			if (v > pk) pk = v;
			if (k >= from) {
				acc += v * v;
				n++;
			}
		}
	}
	if (peak) *peak = pk;
	return 10 * log10(2 * acc / n);
}

static void boost_off_is_untouched(void) {
	hifi_boost_t b;
	hifi_boost_init(&b);
	CHECK(!hifi_boost_active(&b));
	sine16(0, 1000);
	int16_t ref[2 * BLOCK];
	memcpy(ref, b16, sizeof(ref));
	hifi_boost_process(&b, b16, BLOCK, 4, RATE);
	CHECK(!memcmp(ref, b16, sizeof(ref)));
	sine32(0, 1000);
	int32_t r32[2 * BLOCK];
	memcpy(r32, b32, sizeof(r32));
	hifi_boost_process(&b, b32, BLOCK, 8, RATE);
	CHECK(!memcmp(r32, b32, sizeof(r32)));
	CHECK(b.events == 0 && hifi_boost_reduction_db(&b) == 0);
}

static void set_db_clamps(void) {
	hifi_boost_t b;
	hifi_boost_init(&b);
	CHECK(hifi_boost_set_db(&b, -3) == 0);
	CHECK(hifi_boost_set_db(&b, NAN) == 0);
	CHECK(hifi_boost_set_db(&b, 20) == HIFI_BOOST_MAX_DB);
	CHECK_NEAR(hifi_boost_set_db(&b, 4.26f), 4.3, 1e-5);
}

static void quiet_music_gets_louder(void) {
	hifi_boost_t b;
	hifi_boost_init(&b);
	hifi_boost_set_db(&b, 6);
	double pk, lv = run16(&b, -20, 1000, 1, &pk);
	CHECK_NEAR(lv, -14, 0.05);
	CHECK(!b.limiting && b.events == 0);
	CHECK(hifi_boost_reduction_db(&b) == 0);
}

static void loud_music_never_clips(void) {
	hifi_boost_t b;
	hifi_boost_init(&b);
	hifi_boost_set_db(&b, 9);
	double pk, lv = run16(&b, 0, 1000, 1, &pk);
	CHECK(pk <= 1.0);
	/* a sine limited flat to the ceiling: about full scale, with the
	 * envelope's ripple on the 1 kHz peaks */
	CHECK(lv > -1.0 && lv <= 0.05);
	CHECK(b.limiting && b.events == 1);
	CHECK_NEAR(hifi_boost_reduction_db(&b), -9, 0.3);

	/* a bass sine: the 0.15 s release holds across the 25 ms cycle */
	hifi_boost_init(&b);
	hifi_boost_set_db(&b, 9);
	lv = run16(&b, -3, 40, 2, &pk);
	CHECK(pk <= 1.0);
	CHECK(lv > -1.5 && lv <= 0.05);

	/* a quiet passage, then a sudden full-scale hit: the instant attack
	 * keeps the first sample under the ceiling */
	hifi_boost_init(&b);
	hifi_boost_set_db(&b, 9);
	run16(&b, -40, 1000, 1, NULL);
	run16(&b, 0, 3000, 0.6, &pk);
	CHECK(pk <= 1.0);
}

static void ceiling_follows_the_supply(void) {
	hifi_gov_cfg_t cfg;
	hifi_gov_cfg_lite(&cfg);
	hifi_gov_t g;
	hifi_gov_init(&g, &cfg);
	hifi_gov_set_supply(&g, 20, 60);
	CHECK(hifi_gov_clip_fs(&g) == 1.0f); /* 19.06 V of swing, 17.98 V full scale */
	hifi_gov_set_supply(&g, 9, 27);
	CHECK_NEAR(hifi_gov_clip_fs(&g), 0.953 * 9 / cfg.vfs_peak_v, 1e-4);

	hifi_boost_t b;
	hifi_boost_init(&b);
	hifi_boost_set_db(&b, 6);
	hifi_boost_set_ceiling(&b, hifi_gov_clip_fs(&g));
	double pk;
	run16(&b, -3, 1000, 1, &pk);
	CHECK(pk <= hifi_gov_clip_fs(&g) + 1.0 / 32768);
	CHECK(pk > hifi_gov_clip_fs(&g) - 0.01);
}

static void s32_full_scale(void) {
	hifi_boost_t b;
	hifi_boost_init(&b);
	hifi_boost_set_db(&b, 9);
	double pk = 0;
	for (int k = 0; k < RATE / BLOCK; k++) {
		sine32(0, 997);
		/* the extremes too: nothing may overflow the cast (UBSan) */
		b32[0] = INT32_MAX;
		b32[1] = INT32_MIN;
		hifi_boost_process(&b, b32, BLOCK, 8, RATE);
		for (int i = 0; i < 2 * BLOCK; i++) {
			double v = fabs(b32[i] / 2147483648.0);
			if (v > pk) pk = v;
		}
	}
	CHECK(pk <= 1.0);
	CHECK(b.limiting);
}

static void ramps_back_to_off(void) {
	hifi_boost_t b;
	hifi_boost_init(&b);
	hifi_boost_set_db(&b, 6);
	run16(&b, -20, 1000, 0.5, NULL);
	hifi_boost_set_db(&b, 0);
	CHECK(hifi_boost_active(&b)); /* still ramping */
	double lv = run16(&b, -20, 1000, 1, NULL);
	CHECK_NEAR(lv, -20, 0.05);
	CHECK(!hifi_boost_active(&b));
	/* and the boost changes over ~50 ms, not in one step */
	hifi_boost_set_db(&b, 9);
	sine16(-20, 1000);
	hifi_boost_process(&b, b16, BLOCK, 4, RATE);
	CHECK(b.gain > 1.0f && b.gain < 1.8f);
}

static void heat_cap(void) {
	hifi_gov_cfg_t cfg;
	hifi_gov_cfg_lite(&cfg);
	hifi_gov_t g;
	hifi_gov_init(&g, &cfg);
	CHECK(hifi_gov_heat_def_w(&g) == 9.0f);
	CHECK(hifi_gov_heat_w(&g) == 9.0f);
	CHECK(hifi_gov_heat_max_w(&g) == 15.0f);
	CHECK(hifi_gov_set_heat_w(&g, 1) == HIFI_GOV_HEAT_MIN_W);
	CHECK(hifi_gov_set_heat_w(&g, 40) == 15.0f);
	CHECK(hifi_gov_set_heat_w(&g, NAN) == 9.0f);
	CHECK(hifi_gov_set_heat_w(&g, 12.3f) == 12.5f);
	CHECK(g.cfg.thermal_out_w == 25.0f);
	/* the DJ guard's limit moves with it: 25 W / 0.88 + 2.5 */
	CHECK_NEAR(hifi_djg_limit_w(&g.cfg, 1), 25 / 0.88 + 2.5, 1e-3);

	/* a long 0 dBFS sine (40.4 W a channel) on a big charger: the default
	 * cap holds it at 9 W sustained, and says why */
	hifi_gov_init(&g, &cfg);
	hifi_gov_set_supply(&g, 20, 1000);
	phase = 0;
	for (int k = 0; k < 120 * RATE / BLOCK; k++) {
		sine16(0, 1000);
		hifi_gov_process(&g, b16, BLOCK, 4, RATE);
	}
	CHECK(g.limiting);
	CHECK_STR(hifi_gov_why(&g), "heat");
	CHECK_NEAR(hifi_gov_heat_now_w(&g), 9, 0.3);
	CHECK_NEAR(hifi_gov_gain_db(&g), 10 * log10(9 / 40.4), 0.2);

	/* raised to 15 W, the same sine plays 2.2 dB louder */
	hifi_gov_set_heat_w(&g, 15);
	for (int k = 0; k < 120 * RATE / BLOCK; k++) {
		sine16(0, 1000);
		hifi_gov_process(&g, b16, BLOCK, 4, RATE);
	}
	CHECK_STR(hifi_gov_why(&g), "heat");
	CHECK_NEAR(hifi_gov_heat_now_w(&g), 15, 0.5);
	CHECK_NEAR(hifi_gov_gain_db(&g), 10 * log10(15 / 40.4), 0.2);

	/* the amp's warning (trim) wins over a raised cap */
	g.trim = 0.25f;
	for (int k = 0; k < 2 * RATE / BLOCK; k++) {
		sine16(0, 1000);
		hifi_gov_process(&g, b16, BLOCK, 4, RATE);
	}
	CHECK_STR(hifi_gov_why(&g), "amp_hot");

	/* a 65 W charger limits before the heat does: the reason says so */
	hifi_gov_init(&g, &cfg);
	hifi_gov_set_supply(&g, 20, 65);
	for (int k = 0; k < 2 * RATE / BLOCK; k++) {
		sine16(0, 1000);
		hifi_gov_process(&g, b16, BLOCK, 4, RATE);
	}
	CHECK(g.limiting);
	CHECK(!strncmp(hifi_gov_why(&g), "charger", 7));

	/* quiet: free, no reason */
	for (int k = 0; k < 20 * RATE / BLOCK; k++) {
		sine16(-30, 1000);
		hifi_gov_process(&g, b16, BLOCK, 4, RATE);
	}
	CHECK(!g.limiting);
	CHECK_STR(hifi_gov_why(&g), "");

	/* a board without a heat limit ignores the setter */
	cfg.thermal_out_w = 0;
	hifi_gov_init(&g, &cfg);
	CHECK(hifi_gov_set_heat_w(&g, 15) == 0);
	CHECK(g.cfg.thermal_out_w == 0);
}

/* boost then the governor, as hifi_dsp_process runs them: the charger's
 * budget still holds with 9 dB of boost on a loud program */
static void boost_then_governor(void) {
	hifi_gov_cfg_t cfg;
	hifi_gov_cfg_lite(&cfg);
	hifi_gov_t g;
	hifi_gov_init(&g, &cfg);
	hifi_gov_set_supply(&g, 20, 60);
	hifi_gov_set_heat_w(&g, 15);
	hifi_boost_t b;
	hifi_boost_init(&b);
	hifi_boost_set_db(&b, 9);
	hifi_boost_set_ceiling(&b, hifi_gov_clip_fs(&g));
	double worst = 0;
	for (int k = 0; k < 10 * RATE / BLOCK; k++) {
		sine16(-6, 60);
		hifi_boost_process(&b, b16, BLOCK, 4, RATE);
		hifi_gov_process(&g, b16, BLOCK, 4, RATE);
		if (k > RATE / BLOCK && hifi_gov_in_w(&g) > worst) worst = hifi_gov_in_w(&g);
	}
	CHECK(worst <= 0.9 * 60 * 1.02);
	CHECK(g.limiting);
}

int main(void) {
	boost_off_is_untouched();
	set_db_clamps();
	quiet_music_gets_louder();
	loud_music_never_clips();
	ceiling_follows_the_supply();
	s32_full_scale();
	ramps_back_to_off();
	heat_cap();
	boost_then_governor();
	return check_report("boost");
}
