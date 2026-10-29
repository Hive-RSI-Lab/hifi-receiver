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
