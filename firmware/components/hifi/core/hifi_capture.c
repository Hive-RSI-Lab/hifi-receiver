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

const char *hifi_capch_state_name(hifi_capch_state_t s) {
	static const char *n[] = {"idle", "armed", "recording", "done", "failed"};
	return (unsigned) s < sizeof(n) / sizeof(n[0]) ? n[s] : "idle";
}

int hifi_capture_find_notes(const int16_t *pcm, int frames, int rate, const float *hz, int nhz,
			    float min_fs, int min_blocks, hifi_cap_note_t *out, int max, float floor_fs[2]) {
	int bf = rate * HIFI_CAP_BLOCK_MS / 1000, nb = bf > 0 ? frames / bf : 0, found = 0;
	if (floor_fs) floor_fs[0] = floor_fs[1] = 0;
	if (nb <= 0 || nhz <= 0 || max <= 0) return 0;
	if (min_blocks < 1) min_blocks = 1;
	int run_f = -1, run_at = 0, run_len = 0, gap = 0;
	float run_lvl[2] = {0, 0}, best_floor[2] = {-1, -1};
	for (int b = 0; b <= nb; b++) {
		int f = -1;
		float lvl[2] = {0, 0};
		if (b < nb) {
			const int16_t *blk = pcm + 2 * (size_t) b * bf;
			for (int c = 0; c < 2; c++) {
				hifi_cap_stats_t st;
				hifi_capture_stats(blk, bf, c, &st);
				if (best_floor[c] < 0 || st.rms_fs < best_floor[c]) best_floor[c] = st.rms_fs;
			}
			float top = min_fs;
			for (int k = 0; k < nhz; k++) {
				float l = hifi_capture_tone_rms(blk, bf, 0, hz[k], rate);
				float r = hifi_capture_tone_rms(blk, bf, 1, hz[k], rate);
				float m = l > r ? l : r;
				if (m > top) {
					top = m;
					f = k;
					lvl[0] = l;
					lvl[1] = r;
				}
			}
		}
		if (f == run_f && f >= 0) {
			run_len += gap + 1; /* a one-block dip inside a note stays part of it */
			gap = 0;
			for (int c = 0; c < 2; c++)
				if (lvl[c] > run_lvl[c]) run_lvl[c] = lvl[c];
			continue;
		}
		if (f < 0 && run_f >= 0 && b < nb && !gap) {
			gap = 1;
			continue;
		}
		gap = 0;
		if (run_f >= 0 && run_len >= min_blocks && found < max) {
			out[found++] = (hifi_cap_note_t){.hz = hz[run_f],
							 .on_ms = run_at * HIFI_CAP_BLOCK_MS,
							 .len_ms = run_len * HIFI_CAP_BLOCK_MS,
							 .rms_fs = {run_lvl[0], run_lvl[1]}};
		}
		run_f = f;
		run_at = b;
		run_len = 1;
		run_lvl[0] = lvl[0];
		run_lvl[1] = lvl[1];
	}
	if (floor_fs)
		for (int c = 0; c < 2; c++) floor_fs[c] = best_floor[c] > 0 ? best_floor[c] : 0;
	return found;
}
