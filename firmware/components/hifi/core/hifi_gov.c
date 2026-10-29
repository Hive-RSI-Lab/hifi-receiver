#include "hifi_gov.h"

#include <math.h>
#include <string.h>

void hifi_gov_cfg_lite(hifi_gov_cfg_t *cfg) {
	*cfg = (hifi_gov_cfg_t) {
		/* 0.9 Vrms x -3 dB x 19.95 (26 dB) x sqrt 2 = 17.98 V peak; to be
		 * calibrated at bring-up with a 1 kHz sine into a dummy load */
		.vfs_peak_v = 0.9f * 0.7079f * 19.95f * 1.4142f,
		/* measured on hwde's made TPA3118 board: 95.3% of the supply */
		.swing = 0.953f,
		/* the Viennas' 4 ohm: the worst case for the charger */
		.r_load = 4.0f,
		.efficiency = 0.88f,
		.p_idle_w = 2.5f,
		.budget_frac = 0.9f,
		.tau_s = 1.0f,
		.attack_s = 0.05f,
		.release_s = 2.0f,
		.min_gain = 0.05f, /* -26 dB */
	};
}

void hifi_gov_cfg_core(hifi_gov_cfg_t *cfg) {
	hifi_gov_cfg_lite(cfg);
	/* the ES9018K2M's OPA1678 stage gives about 1.15 Vrms at full scale
	 * (boards/core/architecture/blocks.md B7), the TPA3221 24 dB: 1.15 x
	 * 15.85 x sqrt 2 = 25.8 V peak, near the 28 V rail. Not yet measured */
	cfg->vfs_peak_v = 1.15f * 15.85f * 1.4142f;
	cfg->swing = 0.95f;
	cfg->efficiency = 0.88f;
	/* the ESP32, PHY, DAC, ADC and an amp idling at 28 V: an estimate for
	 * bring-up to replace (the INA186 reads it directly) */
	cfg->p_idle_w = 4.0f;
}

void hifi_gov_init(hifi_gov_t *g, const hifi_gov_cfg_t *cfg) {
	memset(g, 0, sizeof(*g));
	g->cfg = *cfg;
	g->gain = g->target = 1.0f;
	g->amps = 1;
	/* until a contract is known: a USB 2.0 port */
	hifi_gov_set_supply(g, 5.0f, 2.5f);
}

void hifi_gov_set_supply(hifi_gov_t *g, float vsupply, float contract_w) {
	g->vsupply = vsupply;
	g->budget_w = contract_w * g->cfg.budget_frac;
}

/* speaker power of one channel with mean square `ms` (full-scale peak = 1) */
static float channel_w(const hifi_gov_t *g, float ms) {
	float vpk = g->cfg.vfs_peak_v;
	float p = ms * vpk * vpk / g->cfg.r_load;
	float clip_pk = g->cfg.swing * g->vsupply;
	float p_clip = clip_pk * clip_pk / (2.0f * g->cfg.r_load);
	return p < p_clip ? p : p_clip;
}

void hifi_gov_process(hifi_gov_t *g, void *buf, size_t frames, int bytes_per_frame, unsigned rate) {
	if (!frames || !rate) return;
	const hifi_gov_cfg_t *c = &g->cfg;
	double sl = 0, sr = 0;

	if (bytes_per_frame == 8) {
		const int32_t *s = buf;
		for (size_t i = 0; i < frames; i++) {
			double l = s[2 * i] / 2147483648.0, r = s[2 * i + 1] / 2147483648.0;
			sl += l * l;
			sr += r * r;
		}
	} else {
		const int16_t *s = buf;
		for (size_t i = 0; i < frames; i++) {
			double l = s[2 * i] / 32768.0, r = s[2 * i + 1] / 32768.0;
			sl += l * l;
			sr += r * r;
		}
	}
	float msl = (float) (sl / frames), msr = (float) (sr / frames);
	float dt = (float) frames / rate;
	float a = 1.0f - expf(-dt / c->tau_s);
	float p_now = (channel_w(g, msl) + channel_w(g, msr)) * g->amps;
	g->p_out_pre += a * (p_now - g->p_out_pre);
	g->ms_avg += a * ((msl + msr) / 2 - g->ms_avg);

	/* gain^2 * p_out / eff + idle = budget */
	float avail = (g->budget_w - c->p_idle_w) * c->efficiency;
	float target = 1.0f;
	if (g->p_out_pre > 1e-6f && avail < g->p_out_pre) {
		target = avail > 0 ? sqrtf(avail / g->p_out_pre) : 0.0f;
		if (target < c->min_gain) target = c->min_gain;
	}
	bool limiting = target < 0.999f;
	if (limiting && !g->limiting) g->events++;
	g->limiting = limiting;
	g->target = target;

	float tc = target < g->gain ? c->attack_s : c->release_s;
	float g0 = g->gain;
	float g1 = g0 + (target - g0) * (1.0f - expf(-dt / tc));
	if (fabsf(g1 - 1.0f) < 1e-3f && target >= 1.0f) g1 = 1.0f; /* 0.01 dB */
	g->gain = g1;
	if (g0 == 1.0f && g1 == 1.0f) return;

	/* ramp linearly across the block so there's no step */
	float step = (g1 - g0) / frames;
	if (bytes_per_frame == 8) {
		int32_t *s = buf;
		for (size_t i = 0; i < frames; i++) {
			float k = g0 + step * (i + 1);
			s[2 * i] = (int32_t) (s[2 * i] * (double) k);
			s[2 * i + 1] = (int32_t) (s[2 * i + 1] * (double) k);
		}
	} else {
		int16_t *s = buf;
		for (size_t i = 0; i < frames; i++) {
			float k = g0 + step * (i + 1);
			s[2 * i] = (int16_t) lrintf(s[2 * i] * k);
			s[2 * i + 1] = (int16_t) lrintf(s[2 * i + 1] * k);
		}
	}
}

float hifi_gov_gain_db(const hifi_gov_t *g) { return 20.0f * log10f(g->gain); }

float hifi_gov_out_w(const hifi_gov_t *g) { return g->p_out_pre * g->gain * g->gain; }

float hifi_gov_in_w(const hifi_gov_t *g) {
	return hifi_gov_out_w(g) / g->cfg.efficiency + g->cfg.p_idle_w;
}

float hifi_gov_level_dbfs(const hifi_gov_t *g) {
	float ms = g->ms_avg * g->gain * g->gain;
	if (ms < 1e-12f) return -120.0f;
	return 10.0f * log10f(2.0f * ms);
}
