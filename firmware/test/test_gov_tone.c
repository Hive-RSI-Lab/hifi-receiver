#include <stdlib.h>

#include "check.h"
#include "hifi_gov.h"
#include "hifi_tone.h"

#define RATE 48000
#define BLOCK 512

static int16_t buf[2 * BLOCK];

/* renders `seconds` of a tone through the governor; returns the RMS
 * (vs a full-scale sine, dB) of the last second after the governor */
static double play(hifi_gov_t *g, const hifi_tone_params_t *p, float seconds) {
	hifi_tone_t t;
	hifi_tone_params_t q = *p;
	q.duration_s = seconds;
	hifi_tone_start(&t, &q, RATE);
	double acc = 0;
	size_t n_acc = 0, total = 0, last = (size_t) ((seconds - 1) * RATE);
	size_t n;
	while ((n = hifi_tone_render(&t, buf, BLOCK)) > 0) {
		hifi_gov_process(g, buf, n, 4, RATE);
		for (size_t i = 0; i < n; i++, total++) {
			if (total >= last) {
				double v = buf[2 * i] / 32768.0;
				acc += v * v;
				n_acc++;
			}
		}
	}
	return 10 * log10(2 * acc / n_acc);
}

static void sine_level_and_dither(void) {
	hifi_tone_t t;
	hifi_tone_params_t p = {.type = HIFI_TONE_SINE, .freq_hz = 1000, .level_dbfs = -20, .duration_s = 1};
	hifi_tone_start(&t, &p, RATE);
	int16_t *all = malloc(sizeof(int16_t) * 2 * RATE);
	size_t got = 0, n;
	while ((n = hifi_tone_render(&t, all + 2 * got, 1000)) > 0) got += n;
	CHECK(got == RATE);
	CHECK(hifi_tone_done(&t));
	CHECK(all[0] == 0 || abs(all[0]) <= 1); /* fade-in starts at silence */
	double acc = 0;
	int16_t peak = 0;
	for (size_t i = RATE / 10; i < RATE * 9 / 10; i++) {
		double v = all[2 * i] / 32768.0;
		acc += v * v;
		if (abs(all[2 * i]) > peak) peak = (int16_t) abs(all[2 * i]);
		CHECK(abs(all[2 * i] - all[2 * i + 1]) <= 2); /* both channels, independent dither */
	}
	double rms_db = 10 * log10(2 * acc / (RATE * 8 / 10));
	CHECK_NEAR(rms_db, -20, 0.05);
	CHECK_NEAR(peak, 3277, 3);
	free(all);

	/* left only */
	p.channel = HIFI_CH_LEFT;
	hifi_tone_start(&t, &p, RATE);
	hifi_tone_render(&t, buf, BLOCK);
	int right_nonzero = 0;
	for (int i = 0; i < BLOCK; i++) right_nonzero |= buf[2 * i + 1];
	CHECK(!right_nonzero && buf[2 * (BLOCK - 1)] != 0);
}

static void sweep_and_noise(void) {
	hifi_tone_t t;
	hifi_tone_params_t p = {.type = HIFI_TONE_SWEEP, .start_hz = 20, .end_hz = 20000, .level_dbfs = -10, .duration_s = 2};
	hifi_tone_start(&t, &p, RATE);
	int16_t *all = malloc(sizeof(int16_t) * 2 * 2 * RATE);
	size_t got = 0, n;
	while ((n = hifi_tone_render(&t, all + 2 * got, 4096)) > 0) got += n;
	CHECK(got == 2 * RATE);
	int zc_start = 0, zc_end = 0;
	for (size_t i = 2 * RATE * 9 / 10; i < 2 * RATE * 95 / 100 - 1; i++)
		zc_end += (all[2 * i] >= 0) != (all[2 * i + 2] >= 0);
	for (size_t i = RATE / 100; i < RATE / 10; i++)
		zc_start += (all[2 * i] >= 0) != (all[2 * i + 2] >= 0);
	/* 1.8-1.9 s into the sweep, f rises 10.0-14.2 kHz: 1197 cycles, 2394 crossings */
	CHECK(zc_end > 2300 && zc_end < 2500);
	/* in the first 0.1 s, f is 20-28 Hz: a handful */
	CHECK(zc_start < 10);
	free(all);

	/* pink noise lands on its RMS */
	hifi_tone_params_t q = {.type = HIFI_TONE_NOISE, .level_dbfs = -20, .duration_s = 4};
	hifi_gov_t g;
	hifi_gov_cfg_t cfg;
	hifi_gov_cfg_lite(&cfg);
	hifi_gov_init(&g, &cfg);
	hifi_gov_set_supply(&g, 20, 1000); /* no limiting */
	CHECK_NEAR(play(&g, &q, 4), -20, 0.5);

	/* silence is digital zero */
	hifi_tone_params_t s = {.type = HIFI_TONE_SILENCE, .duration_s = 0.1f};
	hifi_tone_start(&t, &s, RATE);
	n = hifi_tone_render(&t, buf, BLOCK);
	int nz = 0;
	for (size_t i = 0; i < 2 * n; i++) nz |= buf[i];
	CHECK(!nz);
}

static void stop_fades_out(void) {
	hifi_tone_t t;
	hifi_tone_params_t p = {.type = HIFI_TONE_SINE, .freq_hz = 1000, .level_dbfs = -6, .duration_s = 60};
	hifi_tone_start(&t, &p, RATE);
	hifi_tone_render(&t, buf, BLOCK);
	hifi_tone_stop(&t);
	size_t n = hifi_tone_render(&t, buf, BLOCK);
	CHECK(n == RATE / 100);  /* exactly the 10 ms fade */
	CHECK(abs(buf[2 * (n - 1)]) <= 20);
	CHECK(hifi_tone_done(&t));
	CHECK(hifi_tone_render(&t, buf, BLOCK) == 0);
}

static void limits(void) {
	hifi_tone_params_t p = {.type = HIFI_TONE_SINE, .freq_hz = 1000, .level_dbfs = -20, .duration_s = 5};
	CHECK(hifi_tone_check(&p, false) == NULL);
	p.level_dbfs = -3;
	CHECK(hifi_tone_check(&p, false) != NULL);
	CHECK(hifi_tone_check(&p, true) == NULL);
	p.level_dbfs = 1;
	CHECK(hifi_tone_check(&p, true) != NULL);
	p.level_dbfs = -20;
	p.freq_hz = 5;
	CHECK(hifi_tone_check(&p, false) != NULL);
	p.freq_hz = 1000;
	p.duration_s = 0;
	CHECK(hifi_tone_check(&p, false) != NULL);
	p.duration_s = NAN;
	CHECK(hifi_tone_check(&p, false) != NULL);
	hifi_tone_params_t s = {.type = HIFI_TONE_SWEEP, .start_hz = 100, .end_hz = 100, .level_dbfs = -20, .duration_s = 1};
	CHECK(hifi_tone_check(&s, false) != NULL);
	hifi_tone_params_t si = {.type = HIFI_TONE_SILENCE, .duration_s = 1, .level_dbfs = 99};
	CHECK(hifi_tone_check(&si, false) == NULL); /* level doesn't apply */
	CHECK(hifi_tone_type_from("sweep") == HIFI_TONE_SWEEP);
	CHECK(hifi_tone_type_from("bogus") == HIFI_TONE_NONE);
}

static void governor(void) {
	hifi_gov_cfg_t cfg;
	hifi_gov_cfg_lite(&cfg);
	hifi_gov_t g;
	hifi_gov_init(&g, &cfg);
	hifi_tone_params_t p = {.type = HIFI_TONE_SINE, .freq_hz = 1000, .level_dbfs = 0, .duration_s = 1};

	/* the lite chain: 0 dBFS sine = 17.98 V peak = 40.4 W per channel into 4 ohm */
	hifi_gov_set_supply(&g, 20, 1000);
	play(&g, &p, 6);
	CHECK_NEAR(hifi_gov_out_w(&g), 2 * 40.4, 1.0);
	CHECK(!g.limiting && g.gain == 1.0f);

	/* on a 65 W charger both channels at 0 dBFS would draw ~94 W:
	 * the governor holds the input at 90% of 65 W */
	hifi_gov_init(&g, &cfg);
	hifi_gov_set_supply(&g, 20, 65);
	double out_db = play(&g, &p, 6);
	CHECK(g.limiting && g.events == 1);
	CHECK_NEAR(hifi_gov_in_w(&g), 0.9 * 65, 0.6);
	/* (58.5 - 2.5) * 0.88 / 80.8 = 0.61 in power = -2.1 dB */
	CHECK_NEAR(hifi_gov_gain_db(&g), -2.14, 0.15);
	CHECK_NEAR(out_db, -2.14, 0.2);
	CHECK_NEAR(hifi_gov_level_dbfs(&g), -2.14, 0.2);

	/* quiet music never touches it, and the gain recovers to exactly 1 */
	p.level_dbfs = -20;
	play(&g, &p, 15);
	CHECK(!g.limiting);
	CHECK(g.gain == 1.0f);
	CHECK_NEAR(hifi_gov_level_dbfs(&g), -20, 0.1);

	/* clipping caps the estimate: at 9 V a 0 dBFS sine can't make 40 W */
	hifi_gov_init(&g, &cfg);
	hifi_gov_set_supply(&g, 9, 1000);
	p.level_dbfs = 0;
	play(&g, &p, 6);
	double clip_w = 2 * (0.953 * 9) * (0.953 * 9) / 8;
	CHECK_NEAR(hifi_gov_out_w(&g), clip_w, 0.3);

	/* before a contract is known (5 V, 2.5 W): almost everything is limited,
	 * but never below min_gain */
	hifi_gov_init(&g, &cfg);
	play(&g, &p, 6);
	CHECK_NEAR(g.gain, cfg.min_gain, 0.01);

	/* s32 frames take the same path */
	hifi_gov_init(&g, &cfg);
	hifi_gov_set_supply(&g, 20, 65);
	static int32_t b32[2 * BLOCK];
	for (int k = 0; k < 6 * RATE / BLOCK; k++) {
		for (int i = 0; i < BLOCK; i++) {
			int32_t v = (int32_t) (2147483647.0 * sin(2 * M_PI * 1000 * (k * BLOCK + i) / RATE));
			b32[2 * i] = b32[2 * i + 1] = v;
		}
		hifi_gov_process(&g, b32, BLOCK, 8, RATE);
	}
	CHECK_NEAR(hifi_gov_gain_db(&g), -2.14, 0.15);
}

int main(void) {
	sine_level_and_dither();
	sweep_and_noise();
	stop_fades_out();
	limits();
	governor();
	return check_report("gov+tone");
}
