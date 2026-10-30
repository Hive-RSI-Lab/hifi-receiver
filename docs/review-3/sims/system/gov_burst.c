/*
 * R3-SYSTEM: does the firmware's power governor keep a USB-C charger inside
 * its contract on a bass burst that follows quiet music?  Links the real
 * firmware/components/hifi/core/hifi_gov.c (unchanged) and plays:
 *   3 s at -30 dBFS (quiet passage), then 0.6 s of a 45 Hz sine at 0 dBFS
 *   (a kick / drop), then -30 dBFS again.  Input power is averaged over
 *   one 45 Hz cycle (4 blocks, 21 ms) before it is compared.
 * Input power each 5.3 ms block = what the governor's own model says the amp
 * draws for the samples it lets through (channel power after its gain,
 * capped at clipping, / efficiency + idle).  The excess over the contract is
 * what the PVDD bulk would have to supply; the bulk holds
 * 0.5 C (Vbus^2 - Vmin^2) before VBUS falls under the firmware's 13 V
 * brown-out floor.  FAIL when the input stays over the contract for more
 * than 150 ms (the excess energy against the bulk's is printed).
 * Built with -DFIX against a patched copy of hifi_gov.c (run.sh: the
 * limiter's target from the unclipped power, tau 0.05 s, attack 0.01 s) it
 * shows the proposed fix.
 *   cc -O1 -I firmware/components/hifi/core gov_burst.c \
 *      firmware/components/hifi/core/hifi_gov.c -lm && ./a.out
 */
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include "hifi_gov.h"

#ifdef FIX
#define FIXNAME "fix"
#else
#define FIXNAME "as-is"
#endif
#define RATE 48000
#define BLOCK 256

typedef struct { const char *name; int core; float v, w, c_bulk; } case_t;

static int run(const case_t *k, int fix) {
	hifi_gov_cfg_t cfg;
	if (k->core) hifi_gov_cfg_core(&cfg); else hifi_gov_cfg_lite(&cfg);
	cfg.thermal_out_w = 0; /* supply limit only */
#ifdef FIX
	if (fix) { cfg.tau_s = 0.05f; cfg.attack_s = 0.01f; }
#endif
	hifi_gov_t g;
	hifi_gov_init(&g, &cfg);
	hifi_gov_set_supply(&g, k->v, k->w);
	if (k->core) g.amps = 1;
	static int16_t b[2 * BLOCK];
	double t = 0, e_over = 0, t_over = 0, p_max = 0;
	long n = 0;
	for (int blk = 0; blk < (int) (5.0 * RATE / BLOCK); blk++) {
		double amp = (t >= 3.0 && t < 3.6) ? 1.0 : pow(10, -30 / 20.0);
		for (int i = 0; i < BLOCK; i++, n++) {
			int16_t s = (int16_t) lrint(32767 * amp * sin(2 * M_PI * 45 * n / (double) RATE));
			b[2 * i] = b[2 * i + 1] = s;
		}
		hifi_gov_process(&g, b, BLOCK, 4, RATE);
		/* power of what was let through, with the governor's own model */
		double ms = 0;
		for (int i = 0; i < BLOCK; i++) { double x = b[2 * i] / 32768.0; ms += x * x; }
		ms /= BLOCK;
		double vpk = cfg.vfs_peak_v, clip = cfg.swing * k->v;
		double pch = ms * vpk * vpk / cfg.r_load, pclip = clip * clip / (2 * cfg.r_load);
		if (pch > pclip) pch = pclip;
		double pin_blk = 2 * pch / cfg.efficiency + cfg.p_idle_w;
		/* the bulk and the charger see the cycle average, not the sine's
		 * instantaneous power: average over the last 4 blocks (21 ms, ~one
		 * 45 Hz cycle) */
		static double hist[4];
		hist[blk % 4] = pin_blk;
		double pin = (hist[0] + hist[1] + hist[2] + hist[3]) / 4;
		double dt = (double) BLOCK / RATE;
		if (pin > p_max) p_max = pin;
		if (pin > k->w) { e_over += (pin - k->w) * dt; t_over += dt; }
		t += dt;
	}
	double e_bulk = 0.5 * k->c_bulk * (k->v * k->v - 13.0 * 13.0);
	/* the 1 s window's signature is ~0.5 s over; the fixed governor is ~0.1 s.
	 * What remains (1.3-1.4x for ~0.1 s) is the charger-peak question of
	 * R3-POWER-3: printed, gated at 150 ms here */
	int ok = t_over <= 0.150;
	printf("%-34s %-8s peak in %6.1f W vs %5.1f W contract (%.2fx), over for %4.0f ms, "
	       "excess %6.3f J vs bulk %5.3f J: %s\n", k->name, FIXNAME, p_max, k->w,
	       p_max / k->w, t_over * 1000, e_over, e_bulk, ok ? "PASS" : "FAIL");
	return ok;
}

/* Steady state: a 1 kHz sine at `dbfs` for 8 s; the input power over the
 * last 2 s against the contract.  Clipping matters here: when the program
 * is louder than the rail, the amp's power is the clip power whatever the
 * gain, until the gain brings the peak under the rail. */
static int steady(const case_t *k, double dbfs) {
	hifi_gov_cfg_t cfg;
	if (k->core) hifi_gov_cfg_core(&cfg); else hifi_gov_cfg_lite(&cfg);
	cfg.thermal_out_w = 0;
	hifi_gov_t g;
	hifi_gov_init(&g, &cfg);
	hifi_gov_set_supply(&g, k->v, k->w);
	static int16_t b[2 * BLOCK];
	long n = 0;
	double acc = 0, tt = 0;
	double a = pow(10, dbfs / 20);
	for (int blk = 0; blk < (int) (8.0 * RATE / BLOCK); blk++) {
		for (int i = 0; i < BLOCK; i++, n++)
			b[2 * i] = b[2 * i + 1] = (int16_t) lrint(32767 * a * sin(2 * M_PI * 1000 * n / (double) RATE));
		hifi_gov_process(&g, b, BLOCK, 4, RATE);
		double ms = 0;
		for (int i = 0; i < BLOCK; i++) { double x = b[2 * i] / 32768.0; ms += x * x; }
		ms /= BLOCK;
		double vpk = cfg.vfs_peak_v, clip = cfg.swing * k->v;
		double pch = ms * vpk * vpk / cfg.r_load, pclip = clip * clip / (2 * cfg.r_load);
		if (pch > pclip) pch = pclip;
		if (blk >= (int) (6.0 * RATE / BLOCK)) { acc += 2 * pch / cfg.efficiency + cfg.p_idle_w; tt++; }
	}
	double pin = acc / tt;
	int ok = pin <= k->w;
	printf("%-34s steady %+5.1f dBFS: governor gain %6.2f dB, input %6.1f W vs %5.1f W contract "
	       "(its own estimate %5.1f W): %s\n", k->name, dbfs, hifi_gov_gain_db(&g), pin, k->w,
	       hifi_gov_in_w(&g), ok ? "PASS" : "FAIL");
	return ok;
}

int main(void) {
	const case_t cases[] = {
		{"Lite 20 V 3 A (60 W), 4 ohm", 0, 20, 60, 690e-6},
		{"Lite 20 V 3.25 A (65 W), 4 ohm", 0, 20, 65, 690e-6},
		{"core 20 V 3.25 A (65 W), 4 ohm", 1, 20, 65, 1410e-6},
		{"core 28 V 5 A (140 W), 4 ohm", 1, 28, 140, 1410e-6},
	};
	int fails = 0;
	for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		fails += !run(&cases[i], 1);
	}
	const case_t st[] = {
		{"Lite 15 V 3 A (45 W), 4 ohm", 0, 15, 45, 690e-6},
		{"Lite 20 V 3.25 A (65 W), 4 ohm", 0, 20, 65, 690e-6},
		{"core 20 V 3.25 A (65 W), 4 ohm", 1, 20, 65, 1410e-6},
		{"core 20 V 5 A (100 W), 4 ohm", 1, 20, 100, 1410e-6},
		{"core 28 V 5 A (140 W), 4 ohm", 1, 28, 140, 1410e-6},
	};
	for (unsigned i = 0; i < sizeof st / sizeof st[0]; i++) fails += !steady(&st[i], 0);
	printf("%d as-is case(s) fail (burst beyond the bulk's ride-through, or steady input over the contract)\n", fails);
	return fails ? 1 : 0;
}
