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
		/* a charger holds its contract for milliseconds, not a second, and
		 * the PVDD bulk rides through only ~0.1-0.4 J: a 50 ms window and
		 * 10 ms attack keep a 0 dBFS bass drop after a quiet passage
		 * within ~1.4x for ~0.1 s (docs/review-3/sims/system/run_gov.sh) */
		.tau_s = 0.05f,
		.attack_s = 0.01f,
		.release_s = 2.0f,
		.min_gain = 0.05f, /* -26 dB */
		/* a steady 0 dBFS 45 Hz tone's loudest half-cycle draws ~1.7x its
		 * average: an average budget alone lets a 3 A charger see 5 A
		 * (review-3 R3-POWER-3, docs/review-3/sims/power/charger_peak.py) */
		.peak_frac = 1.0f,
		.peak_hold_s = 0.5f,
		/* the TPA3118 in the Lite box: 13 W a channel over 30 s into 4 ohm
		 * (review-2 THERMAL-3, firmware requirement L1) */
		.thermal_out_w = 26.0f,
		.thermal_tau_s = 30.0f,
	};
}

void hifi_gov_cfg_core(hifi_gov_cfg_t *cfg) {
	hifi_gov_cfg_lite(cfg);
	/* the ES9018K2M's OPA1678 stage gives about 1.15 Vrms at full scale
	 * (boards/core/architecture/blocks.md B7), the TPA3220 24 dB: 1.15 x
	 * 15.85 x sqrt 2 = 25.8 V peak, near the 28 V rail. Not yet measured */
	cfg->vfs_peak_v = 1.15f * 15.85f * 1.4142f;
	cfg->swing = 0.95f;
	cfg->efficiency = 0.88f;
	/* the ESP32, PHY, DAC, ADC and an amp idling at 28 V: an estimate for
	 * bring-up to replace (the INA186 reads it directly) */
	cfg->p_idle_w = 4.0f;
	/* the TPA3220 cools into the board (no heatsink, ~26 C/W). In the
	 * vented Plus/Ultra box (enclosure/core_box.py vent_plan, ~1500 mm2 a
	 * stage) at 30 C room, hwde check_thermal holds Tj at 110 C with ~2.0 W
	 * per chip: Ultra's two channels per chip then sustain 4.4 W each into
	 * 4 ohm, so one zone 9 W and both zones 17 W (docs/review-2/
	 * heatsink-options.md). Plus splits its pair across both chips and
	 * raises this in board_core.c. Peaks pass; the minute doesn't */
	cfg->thermal_out_w = 9.0f;
	cfg->thermal_out2_w = 17.0f;
	cfg->thermal_tau_s = 60.0f;
}

void hifi_gov_init(hifi_gov_t *g, const hifi_gov_cfg_t *cfg) {
	memset(g, 0, sizeof(*g));
	g->cfg = *cfg;
	g->gain = g->target = g->trim = 1.0f;
	g->amps = 1;
	/* until a contract is known: a USB 2.0 port */
	hifi_gov_set_supply(g, 5.0f, 2.5f);
}

void hifi_gov_set_supply(hifi_gov_t *g, float vsupply, float contract_w) {
	g->vsupply = vsupply;
	g->budget_w = contract_w * g->cfg.budget_frac;
	g->contract_w = contract_w;
}

/* speaker power of one channel with mean square `ms` (full-scale peak = 1),
 * as the program asks for it: not capped at the rail's clip power, since a
 * gain worked out from the capped figure leaves the peaks clipping and the
 * amp drawing up to twice the sine's clip power (review-3 R3-SYSTEM-1) */
static float channel_w(const hifi_gov_t *g, float ms) {
	float vpk = g->cfg.vfs_peak_v;
	return ms * vpk * vpk / g->cfg.r_load;
}

/* what one channel can deliver at the rail: a sine at the clip */
static float clip_w(const hifi_gov_t *g) {
	float clip_pk = g->cfg.swing * g->vsupply;
	return clip_pk * clip_pk / (2.0f * g->cfg.r_load);
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
	if (c->thermal_out_w > 0 && c->thermal_tau_s > 0)
		g->p_out_slow += (1.0f - expf(-dt / c->thermal_tau_s)) * (p_now - g->p_out_slow);
	g->ms_avg += a * ((msl + msr) / 2 - g->ms_avg);
	if (c->peak_hold_s > 0) {
		float held = g->p_out_peak * expf(-dt / c->peak_hold_s);
		g->p_out_peak = p_now > held ? p_now : held;
	}

	/* gain^2 * p_out / eff + idle = budget */
	float avail = (g->budget_w - c->p_idle_w) * c->efficiency;
	float target = 1.0f;
	if (g->p_out_pre > 1e-6f && avail < g->p_out_pre) {
		target = avail > 0 ? sqrtf(avail / g->p_out_pre) : 0.0f;
		if (target < c->min_gain) target = c->min_gain;
	}
	/* the charger's current limit: the loudest recent block */
	float avail_pk = (g->contract_w * c->peak_frac - c->p_idle_w) * c->efficiency;
	if (c->peak_frac > 0 && g->p_out_peak > 1e-6f && avail_pk < g->p_out_peak) {
		float t3 = avail_pk > 0 ? sqrtf(avail_pk / g->p_out_peak) : 0.0f;
		if (t3 < c->min_gain) t3 = c->min_gain;
		if (t3 < target) target = t3;
	}
	/* the amps' heat: the slow average of what they would play */
	float cap = g->amps > 1 && c->thermal_out2_w > 0 ? c->thermal_out2_w : c->thermal_out_w * g->amps;
	if (c->thermal_out_w > 0 && g->p_out_slow > cap) {
		float t2 = sqrtf(cap / g->p_out_slow);
		if (t2 < c->min_gain) t2 = c->min_gain;
		if (t2 < target) target = t2;
	}
	if (g->trim < target) target = g->trim < c->min_gain ? c->min_gain : g->trim;
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

float hifi_gov_out_w(const hifi_gov_t *g) {
	float p = g->p_out_pre * g->gain * g->gain, cap = 2.0f * clip_w(g) * g->amps;
	return p < cap ? p : cap;
}

float hifi_gov_in_w(const hifi_gov_t *g) {
	return hifi_gov_out_w(g) / g->cfg.efficiency + g->cfg.p_idle_w;
}

float hifi_gov_level_dbfs(const hifi_gov_t *g) {
	float ms = g->ms_avg * g->gain * g->gain;
	if (ms < 1e-12f) return -120.0f;
	return 10.0f * log10f(2.0f * ms);
}
