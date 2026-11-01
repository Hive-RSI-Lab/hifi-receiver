#include "hifi_line.h"

#include <math.h>
#include <string.h>

void hifi_line_to_s16(const int32_t *in, int16_t *out, size_t frames, bool invert) {
	for (size_t i = 0; i < 2 * frames; i++) {
		int32_t v = in[i] >> 16;
		if (invert) v = v == -32768 ? 32767 : -v;
		out[i] = (int16_t) v;
	}
}

void hifi_sub_mix(const void *in, int bytes_per_frame, int32_t *out, size_t frames, float gain) {
	float g = gain * 0.5f;
	for (size_t i = 0; i < frames; i++) {
		float m;
		if (bytes_per_frame == 8) {
			const int32_t *s = (const int32_t *) in + 2 * i;
			m = ((float) (s[0] >> 16) + (float) (s[1] >> 16)) * g;
		} else {
			const int16_t *s = (const int16_t *) in + 2 * i;
			m = ((float) s[0] + (float) s[1]) * g;
		}
		int32_t v = (int32_t) lrintf(m > 32767 ? 32767 : m < -32768 ? -32768 : m);
		out[2 * i] = out[2 * i + 1] = (int32_t) ((uint32_t) v << 16);
	}
}

float hifi_line_block_w(const hifi_gov_cfg_t *c, const int16_t *buf, size_t frames, int amps) {
	if (!frames) return 0;
	double sl = 0, sr = 0;
	for (size_t i = 0; i < frames; i++) {
		double l = buf[2 * i] / 32768.0, r = buf[2 * i + 1] / 32768.0;
		sl += l * l;
		sr += r * r;
	}
	float vpk2 = c->vfs_peak_v * c->vfs_peak_v;
	return (float) ((sl + sr) / frames) * vpk2 / c->r_load * (amps > 0 ? amps : 1);
}

void hifi_djg_cfg_default(hifi_djg_cfg_t *c, const hifi_gov_cfg_t *gc) {
	c->tau_s = 60.0f;
	c->otw_ms = 200;
	c->over_steps = 2;
	c->efficiency = gc->efficiency;
	c->idle_w = gc->p_idle_w;
}

void hifi_djg_init(hifi_djg_t *g, const hifi_djg_cfg_t *c) {
	memset(g, 0, sizeof(*g));
	g->cfg = *c;
}

void hifi_djg_reset(hifi_djg_t *g) {
	g->out_w = 0;
	g->meas_w = 0;
	g->otw_low = false;
	g->over_n = 0;
}

static float ema(float avg, float x, float dt, float tau) {
	return avg + (1.0f - expf(-dt / tau)) * (x - avg);
}

void hifi_djg_audio(hifi_djg_t *g, float out_w, float dt_s) {
	if (dt_s > 0) g->out_w = ema(g->out_w, out_w, dt_s, g->cfg.tau_s);
}

float hifi_djg_in_w(const hifi_djg_t *g) {
	float est = g->out_w / g->cfg.efficiency + g->cfg.idle_w;
	return g->meas_w > est ? g->meas_w : est;
}

float hifi_djg_limit_w(const hifi_gov_cfg_t *gc, int amps) {
	float cap = amps > 1 && gc->thermal_out2_w > 0 ? gc->thermal_out2_w : gc->thermal_out_w * (amps > 0 ? amps : 1);
	return cap / gc->efficiency + gc->p_idle_w;
}

const char *hifi_djg_step(hifi_djg_t *g, uint32_t now_ms, float dt_s, float in_w_meas, bool otw, float limit_w,
			  float contract_w) {
	if (!isnan(in_w_meas) && dt_s > 0) g->meas_w = ema(g->meas_w, in_w_meas, dt_s, g->cfg.tau_s);
	if (!isnan(in_w_meas) && contract_w > 0 && in_w_meas > contract_w) g->over_n++;
	else g->over_n = 0;
	if (otw && !g->otw_low) g->otw_since = now_ms;
	g->otw_low = otw;
	const char *why = NULL;
	if (g->over_n >= g->cfg.over_steps) why = "contract";
	else if (otw && now_ms - g->otw_since >= g->cfg.otw_ms) why = "otw";
	else if (limit_w > 0 && hifi_djg_in_w(g) > limit_w) why = "power";
	if (why) g->tripped = why;
	return why;
}
