#include <math.h>
#include <stdlib.h>

#include "hifi_capture.h"

void hifi_capture_stats(const int16_t *pcm, int frames, int ch, hifi_cap_stats_t *out) {
	double sum = 0, sq = 0;
	int peak = 0, clipped = 0;
	for (int i = 0; i < frames; i++) {
		int v = pcm[2 * i + ch];
		sum += v;
		sq += (double) v * v;
		if (abs(v) > peak) peak = abs(v);
		if (v >= 32767 || v <= -32768) clipped++;
	}
	double n = frames > 0 ? frames : 1, mean = sum / n;
	double var = sq / n - mean * mean; /* the AC part: the RMS a meter would read */
	out->dc_fs = (float) (mean / 32768.0);
	out->rms_fs = (float) (sqrt(var > 0 ? var : 0) / 32768.0);
	out->peak_fs = (float) (peak / 32768.0);
	out->clipped = clipped;
}

float hifi_capture_tone_rms(const int16_t *pcm, int frames, int ch, float freq_hz, int rate) {
	if (frames <= 0 || rate <= 0) return 0;
	/* the least-squares line through the samples comes off first: a DC
	 * offset still settling (the taps' coupling capacitors, the first capture
	 * after a boot) is a ramp from end to end, and its jump between the ends
	 * would otherwise leak into every bin, 20 dB over a quiet leg's floor */
	double n = frames, tm = (n - 1) / 2, sum = 0, st = 0;
	for (int i = 0; i < frames; i++) {
		sum += pcm[2 * i + ch];
		st += (i - tm) * pcm[2 * i + ch];
	}
	double mean = sum / n, slope = frames > 1 ? st / (n * (n * n - 1) / 12) : 0;
	double w = 2 * M_PI * freq_hz / rate, cw = 2 * cos(w), s1 = 0, s2 = 0;
	for (int i = 0; i < frames; i++) {
		double s0 = (pcm[2 * i + ch] - mean - slope * (i - tm)) / 32768.0 + cw * s1 - s2;
		s2 = s1;
		s1 = s0;
	}
	double p = s1 * s1 + s2 * s2 - cw * s1 * s2; /* |X|^2 at the tone's frequency */
	return (float) (sqrt(2 * (p > 0 ? p : 0)) / frames);
}
