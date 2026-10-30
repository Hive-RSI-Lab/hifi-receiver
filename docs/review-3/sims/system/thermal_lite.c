/*
 * R3-SYSTEM-2: does Lite's governor hold firmware duty L1 (THERMAL-3:
 * 30 s RMS output <= 9 W per channel into 4 ohm, or the TPA3118 reaches its
 * latched 150 C shutdown in the box)?  Links the real hifi_gov.c with Lite's
 * config on a 20 V / 5 A (100 W) charger, so the supply limit doesn't act,
 * and plays a 1 kHz sine at -3 dBFS (loud, compressed music's RMS) for 180 s.
 * FAIL when the last 30 s mean exceeds 9 W per channel (+2 %); the highest
 * 30 s window mean over the run is printed too (the governor's 30 s
 * exponential average lets the first half-minute through).
 *   cc -I firmware/components/hifi/core thermal_lite.c .../hifi_gov.c -lm
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include "hifi_gov.h"

int main(void) {
	hifi_gov_cfg_t cfg;
	hifi_gov_cfg_lite(&cfg);
	hifi_gov_t g;
	hifi_gov_init(&g, &cfg);
	hifi_gov_set_supply(&g, 20, 100);
	static int16_t b[512];
	long n = 0;
	double acc = 0, win[1125] = {0}, wsum = 0, wmax = 0; /* 1125 blocks = 6 s; 5 of them = 30 s */
	int cnt = 0;
	const int blocks = 180 * 48000 / 256, w30 = 30 * 48000 / 256;
	static double hist[180 * 48000 / 256];
	for (int k = 0; k < blocks; k++) {
		for (int i = 0; i < 256; i++, n++)
			b[2 * i] = b[2 * i + 1] = (int16_t) lrint(32767 * 0.7079 * sin(2 * M_PI * 1000 * n / 48000.0));
		hifi_gov_process(&g, b, 256, 4, 48000);
		double p = hifi_gov_out_w(&g) / 2;
		hist[k] = p;
		wsum += p;
		if (k >= w30) wsum -= hist[k - w30];
		if (k >= w30 - 1 && wsum / w30 > wmax) wmax = wsum / w30;
		if (k >= blocks - w30) { acc += p; cnt++; }
	}
	double w = acc / cnt;
	(void) win;
	int ok = w <= 9.0 * 1.02;
	printf("Lite, -3 dBFS 180 s, 4 ohm: thermal_out_w %.1f W, gain %.2f dB, last 30 s %.1f W per channel, "
	       "highest 30 s window %.1f W, vs L1 13 W: %s\n", cfg.thermal_out_w, hifi_gov_gain_db(&g), w, wmax,
	       ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
