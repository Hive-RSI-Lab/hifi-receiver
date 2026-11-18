/* Boost (hifi_boost) and the governor's heat cap (hifi_gov), ask k2388 */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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

	/* a quiet passage, then a sudden full-scale hit: the lookahead has
	 * the gain down before it arrives */
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
	/* 19.06 V of swing less 0.5 dB, 18.2 V full scale */
	CHECK_NEAR(hifi_gov_clip_fs(&g), 0.944 * 0.953 * 20 / cfg.vfs_peak_v, 1e-4);
	/* a sagging PVDD lowers it; one over the contract doesn't raise it */
	hifi_gov_set_pvdd(&g, 18);
	CHECK_NEAR(hifi_gov_clip_fs(&g), 0.944 * 0.953 * 18 / cfg.vfs_peak_v, 1e-4);
	hifi_gov_set_pvdd(&g, 21);
	CHECK_NEAR(hifi_gov_clip_fs(&g), 0.944 * 0.953 * 20 / cfg.vfs_peak_v, 1e-4);
	hifi_gov_set_pvdd(&g, NAN);
	hifi_gov_set_supply(&g, 28, 60);
	CHECK(hifi_gov_clip_fs(&g) == 1.0f);
	hifi_gov_set_supply(&g, 9, 27);
	CHECK_NEAR(hifi_gov_clip_fs(&g), 0.944 * 0.953 * 9 / cfg.vfs_peak_v, 1e-4);

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
	/* at 0 dB the delay stays in while there is audio (taking it out would
	 * skip 1 ms), and leaves in the first quiet block */
	CHECK(hifi_boost_active(&b) && b.primed);
	memset(b16, 0, sizeof(b16));
	hifi_boost_process(&b, b16, BLOCK, 4, RATE);
	CHECK(!hifi_boost_active(&b));
	/* and the boost changes over ~50 ms, not in one step */
	hifi_boost_set_db(&b, 9);
	sine16(-20, 1000);
	hifi_boost_process(&b, b16, BLOCK, 4, RATE);
	CHECK(b.gain > 1.0f && b.gain < 1.8f);
}

/* 0.5 s of quiet DC, then a step to full scale, in s32: the gain (output
 * over the boosted input LA frames back) falls smoothly over the lookahead,
 * never in one step, and the output stays under the ceiling */
static void lookahead_ramps_down(void) {
	hifi_boost_t b;
	hifi_boost_init(&b);
	hifi_boost_set_db(&b, 9);
	const float bg = powf(10, 9 / 20.0f);
	static float in[RATE];
	static float out[RATE];
	int n = 0;
	for (int k = 0; n + BLOCK <= RATE; k++) {
		for (int i = 0; i < BLOCK; i++) {
			int32_t v = n + i < RATE / 2 ? 21474836 : 2147483647; /* -40 dB, then 0 dB */
			b32[2 * i] = b32[2 * i + 1] = v;
			in[n + i] = v / 2147483648.0f * bg;
		}
		hifi_boost_process(&b, b32, BLOCK, 8, RATE);
		for (int i = 0; i < BLOCK; i++) out[n + i] = b32[2 * i] / 2147483648.0f;
		n += BLOCK;
	}
	double pk = 0, worst_step = 0, prev = 1;
	for (int i = 2 * RATE / 5; i < n; i++) { /* after the boost's 50 ms ramp */
		if (out[i] > pk) pk = out[i];
		double g = out[i] / in[i - HIFI_BOOST_LA];
		if (fabs(g - prev) > worst_step) worst_step = fabs(g - prev);
		prev = g;
	}
	CHECK(pk <= 1.0);
	double floor_g = 1.0 / bg;                     /* 0 dBFS boosted 9 dB */
	CHECK(worst_step <= (1 - floor_g) / HIFI_BOOST_LA * 1.05);
	CHECK(worst_step > (1 - floor_g) / HIFI_BOOST_LA * 0.5); /* it did ramp */
	CHECK_NEAR(out[n - 1], 1.0, 1e-4);              /* and sits at the ceiling */
}

/* a boost that doesn't limit is the boost and a delay of exactly LA frames */
static void latency_is_the_lookahead(void) {
	hifi_boost_t b;
	hifi_boost_init(&b);
	hifi_boost_set_db(&b, 6);
	run16(&b, -30, 1000, 0.5, NULL); /* the boost ramp settles */
	static int16_t ref[2 * BLOCK];
	sine16(-30, 1000);
	memcpy(ref, b16, sizeof(ref));
	hifi_boost_process(&b, b16, BLOCK, 4, RATE);
	const double g = pow(10, 6 / 20.0);
	double worst = 0;
	for (int i = HIFI_BOOST_LA; i < BLOCK; i++) {
		double e = fabs(b16[2 * i] - ref[2 * (i - HIFI_BOOST_LA)] * g);
		if (e > worst) worst = e;
	}
	CHECK(worst <= 1.0);
}

/* noise bursts at random levels and odd block sizes, against the ceiling a
 * low supply sets: no frame ever passes it */
static void random_never_passes_ceiling(void) {
	hifi_boost_t b;
	hifi_boost_init(&b);
	hifi_boost_set_db(&b, 9);
	hifi_boost_set_ceiling(&b, 0.7f);
	srand(7);
	double pk = 0;
	float amp = 0.1f;
	for (int k = 0; k < 4000; k++) {
		int frames = 1 + rand() % BLOCK;
		if (rand() % 8 == 0) amp = (float) rand() / RAND_MAX;
		for (int i = 0; i < frames; i++) {
			float x = amp * (2.0f * rand() / RAND_MAX - 1);
			b32[2 * i] = (int32_t) (x * 2147483647.0f);
			b32[2 * i + 1] = (int32_t) (-x * 2147483647.0f);
		}
		hifi_boost_process(&b, b32, frames, 8, RATE);
		for (int i = 0; i < 2 * frames; i++) {
			double v = fabs(b32[i] / 2147483648.0);
			if (v > pk) pk = v;
		}
	}
	CHECK(pk <= 0.7 + 1e-6);
	CHECK(pk > 0.69);
}

/* a test tone or the line copy takes the delay out at once */
static void bypass_takes_it_out(void) {
	hifi_boost_t b;
	hifi_boost_init(&b);
	hifi_boost_set_db(&b, 6);
	run16(&b, -20, 1000, 0.2, NULL);
	hifi_boost_bypass(&b);
	CHECK(!b.primed);
	CHECK(hifi_boost_active(&b)); /* the boost is still set */
	hifi_boost_set_db(&b, 0);
	b.gain = 1.0f;
	hifi_boost_bypass(&b);
	CHECK(!hifi_boost_active(&b));
}

/* after a peak the limiter's request reaches unity exactly (not 0.9998), so
 * Boost back at 0 takes the path out and the audio is bit-exact again */
static void release_reaches_unity(void) {
	hifi_boost_t b;
	hifi_boost_init(&b);
	hifi_boost_set_db(&b, 9);
	run16(&b, -1, 100, 0.5, NULL); /* deep limiting */
	CHECK(b.req < 0.6f);
	run16(&b, -40, 1000, 3, NULL); /* quiet, well under the ceiling */
	CHECK(b.req == 1.0f && b.req_d == 0.0f);
	hifi_boost_set_db(&b, 0);
	run16(&b, -40, 1000, 1, NULL); /* the boost ramps out; still playing */
	sine16(-40, 1000);
	static int16_t ref[2 * BLOCK];
	memcpy(ref, b16, sizeof(ref));
	hifi_boost_process(&b, b16, BLOCK, 4, RATE);
	/* still delayed (the audio never went quiet), but at exactly unity */
	int bad = 0;
	for (int i = HIFI_BOOST_LA; i < BLOCK; i++) bad += b16[2 * i] != ref[2 * (i - HIFI_BOOST_LA)];
	CHECK(bad == 0);
}

/* turning Boost up mid-song doesn't drop 1 ms of audio: the live frames play
 * while the delay fills, then cross over */
static void coming_in_has_no_gap(void) {
	hifi_boost_t b;
	hifi_boost_init(&b);
	phase = 0;
	run16(&b, -6, 1000, 0.2, NULL);
	hifi_boost_set_db(&b, 3);
	/* every 1 kHz cycle (48 frames) through the change keeps its peak */
	for (int k = 0; k < 4; k++) {
		sine16(-6, 1000);
		hifi_boost_process(&b, b16, BLOCK, 4, RATE);
		for (int c = 0; c + 48 <= BLOCK; c += 48) {
			int pk = 0;
			for (int i = c; i < c + 48; i++) pk = abs(b16[2 * i]) > pk ? abs(b16[2 * i]) : pk;
			CHECK(pk > 0.45 * 32768); /* -6 dBFS is 0.5 */
		}
	}
}

/* the supply sagging mid-stream lowers the ceiling: the frames already in the
 * delay ease under the new one instead of being cut flat */
static void ceiling_drop_eases(void) {
	hifi_boost_t b;
	hifi_boost_init(&b);
	hifi_boost_set_db(&b, 6);
	run16(&b, -8, 200, 0.5, NULL);
	hifi_boost_set_ceiling(&b, 0.6f);
	sine16(-8, 200);
	hifi_boost_process(&b, b16, BLOCK, 4, RATE);
	/* a cut-flat run would sit at the ceiling for many frames in a row */
	int run = 0, longest = 0;
	for (int i = 0; i < BLOCK; i++) {
		run = abs(b16[2 * i]) >= (int) (0.6 * 32768) - 1 ? run + 1 : 0;
		longest = run > longest ? run : longest;
	}
	CHECK(longest <= 2);
	double pk = 0;
	for (int i = 0; i < 2 * BLOCK; i++) pk = fmax(pk, fabs(b16[i] / 32768.0));
	CHECK(pk <= 0.6 + 1.0 / 32768);
}

/* a test tone bypasses the boost at +9 dB: back in, it ramps up from unity
 * again, so the first loud cycles are limited, not cut flat */
static void after_bypass_ramps_again(void) {
	hifi_boost_t b;
	hifi_boost_init(&b);
	hifi_boost_set_db(&b, 9);
	phase = 0;
	run16(&b, -3, 200, 0.5, NULL);
	hifi_boost_bypass(&b);
	CHECK(b.gain == 1.0f);
	int run = 0, longest = 0;
	for (int k = 0; k < 4; k++) {
		sine16(-3, 200);
		hifi_boost_process(&b, b16, BLOCK, 4, RATE);
		for (int i = 0; i < BLOCK; i++) {
			run = abs(b16[2 * i]) >= 32766 ? run + 1 : 0;
			longest = run > longest ? run : longest;
		}
	}
	CHECK(longest <= 2);
}

/* PVDD read noise (1% each 10 ms) on music well under the ceiling never
 * turns into gain reduction */
static void ceiling_noise_ignored(void) {
	hifi_boost_t b;
	hifi_boost_init(&b);
	hifi_boost_set_db(&b, 6);
	hifi_boost_set_ceiling(&b, 0.9f);
	phase = 0;
	unsigned x = 1;
	double gr = 1;
	for (int k = 0; k < RATE / BLOCK * 2; k++) {
		x = x * 1103515245u + 12345u;
		hifi_boost_set_ceiling(&b, 0.9f * (1 + 0.01f * ((int) (x >> 16 & 0xff) - 128) / 128));
		sine16(-12, 200); /* +6 dB: 0.5, far under 0.9 */
		hifi_boost_process(&b, b16, BLOCK, 4, RATE);
		if (k > RATE / BLOCK / 2) gr = fmin(gr, b.gr_min);
	}
	CHECK(gr > 0.999);
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

	/* a long 0 dBFS sine (41.4 W a channel) on a big charger: the default
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
	CHECK_NEAR(hifi_gov_gain_db(&g), 10 * log10(9 / 41.4), 0.2);

	/* raised to 15 W, the same sine plays 2.2 dB louder */
	hifi_gov_set_heat_w(&g, 15);
	for (int k = 0; k < 120 * RATE / BLOCK; k++) {
		sine16(0, 1000);
		hifi_gov_process(&g, b16, BLOCK, 4, RATE);
	}
	CHECK_STR(hifi_gov_why(&g), "heat");
	CHECK_NEAR(hifi_gov_heat_now_w(&g), 15, 0.5);
	CHECK_NEAR(hifi_gov_gain_db(&g), 10 * log10(15 / 41.4), 0.2);

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
	lookahead_ramps_down();
	latency_is_the_lookahead();
	random_never_passes_ceiling();
	bypass_takes_it_out();
	release_reaches_unity();
	coming_in_has_no_gap();
	ceiling_drop_eases();
	after_bypass_ramps_again();
	ceiling_noise_ignored();
	heat_cap();
	boost_then_governor();
	return check_report("boost");
}
