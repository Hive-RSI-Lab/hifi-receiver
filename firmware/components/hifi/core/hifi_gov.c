#include "hifi_gov.h"
#include "hifi_api.h" /* hifi_chan_t */

#include <math.h>
#include <string.h>

void hifi_gov_cfg_lite(hifi_gov_cfg_t *cfg) {
	*cfg = (hifi_gov_cfg_t) {
		/* 18.2 V peak: the ES8388's ~1.08 Vrms full scale at OUT1's -4.5 dB
		 * (code 27, ES8388_OUT1_FULL) x 19.95 (26 dB) x sqrt 2
		 * (docs/review-2/firmware-requirements.md L3: at 0 dB OUT1 would
		 * overdrive the amp by 4.1 dB at 20 V, so raising OUT1 gains nothing;
		 * Boost adds loudness instead). The higher figure of the two
		 * estimates (the other gave 17.98 V), so Boost's clip ceiling errs
		 * low. To be calibrated with a 1 kHz sine into a dummy load */
		.vfs_peak_v = 18.2f,
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
		/* the TPA3118 in the Lite box: 9 W a channel over 30 s into 4 ohm.
		 * Requirement L1's 13 W (review-2 THERMAL-3) puts Tj at 126-149 C in
		 * a warm box, at the 150 C latch; 9 W is the review's figure (review-3
		 * R3-LAYOUT-13, docs/review-3/sims/layout/thermal_calc.py) */
		.thermal_out_w = 18.0f,
		.thermal_tau_s = 30.0f,
		/* the app's heat cap goes to 15 W a channel (k2388: "heat trips or a
		 * fan are fine"). The thermal model puts Tj at the 150 C shutdown
		 * near 15 W in a warm box, so past 9 W the amp may cut out: Lite has
		 * no OTW pin, and hifi_main answers each trip above the default
		 * with 2 W less cap and 3 dB less gain. 22 W needs a fan, a heatsink
		 * and a measurement first (Opus review of k2388) */
		.thermal_max_w = 30.0f,
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
	/* no heat cap above the default: the TPA3220 has no heatsink, and its
	 * OTW pin backs it off; the Lite's 15 W top is not for this amp */
	cfg->thermal_max_w = 0;
	cfg->thermal_out2_w = 17.0f;
	cfg->thermal_tau_s = 60.0f;
}

void hifi_gov_init(hifi_gov_t *g, const hifi_gov_cfg_t *cfg) {
	memset(g, 0, sizeof(*g));
	g->cfg = *cfg;
	g->gain = g->target = g->trim = 1.0f;
	g->amps = 1;
	g->heat_def_w = cfg->thermal_out_w;
	g->heat_def2_w = cfg->thermal_out2_w;
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
	/* left or right: the mixers play that side on both speakers after this,
	 * so it draws twice its own power (mono sums at -6 dB: no more than stereo) */
	float pl = channel_w(g, msl), pr = channel_w(g, msr);
	float p_now = (g->chan == HIFI_CHAN_LEFT ? 2 * pl : g->chan == HIFI_CHAN_RIGHT ? 2 * pr : pl + pr) * g->amps;
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
	hifi_gov_why_t why = HIFI_GOV_FREE;
	if (g->p_out_pre > 1e-6f && avail < g->p_out_pre) {
		target = avail > 0 ? sqrtf(avail / g->p_out_pre) : 0.0f;
		if (target < c->min_gain) target = c->min_gain;
		why = HIFI_GOV_CHARGER;
	}
	/* the charger's current limit: the loudest recent block */
	float avail_pk = (g->contract_w * c->peak_frac - c->p_idle_w) * c->efficiency;
	if (c->peak_frac > 0 && g->p_out_peak > 1e-6f && avail_pk < g->p_out_peak) {
		float t3 = avail_pk > 0 ? sqrtf(avail_pk / g->p_out_peak) : 0.0f;
		if (t3 < c->min_gain) t3 = c->min_gain;
		if (t3 < target) {
			target = t3;
			why = HIFI_GOV_CHARGER_PEAK;
		}
	}
	/* the amps' heat: the slow average of what they would play */
	float cap = g->amps > 1 && c->thermal_out2_w > 0 ? c->thermal_out2_w : c->thermal_out_w * g->amps;
	if (c->thermal_out_w > 0 && g->p_out_slow > cap) {
		float t2 = sqrtf(cap / g->p_out_slow);
		if (t2 < c->min_gain) t2 = c->min_gain;
		if (t2 < target) {
			target = t2;
			why = HIFI_GOV_HEAT;
		}
	}
	if (g->trim < target) {
		target = g->trim < c->min_gain ? c->min_gain : g->trim;
		why = HIFI_GOV_AMP_HOT;
	}
	bool limiting = target < 0.999f;
	g->why = limiting ? why : HIFI_GOV_FREE;
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

float hifi_gov_heat_def_w(const hifi_gov_t *g) { return g->heat_def_w / 2; }

float hifi_gov_heat_max_w(const hifi_gov_t *g) {
	float m = g->cfg.thermal_max_w > g->heat_def_w ? g->cfg.thermal_max_w : g->heat_def_w;
	return m / 2;
}

float hifi_gov_heat_w(const hifi_gov_t *g) { return g->cfg.thermal_out_w / 2; }

float hifi_gov_set_heat_w(hifi_gov_t *g, float w) {
	if (g->heat_def_w <= 0) return 0; /* a board without a thermal limit */
	if (!(w > 0)) w = hifi_gov_heat_def_w(g);
	float mx = hifi_gov_heat_max_w(g);
	if (w < HIFI_GOV_HEAT_MIN_W) w = HIFI_GOV_HEAT_MIN_W;
	if (w > mx) w = mx;
	w = roundf(w * 2.0f) / 2.0f; /* 0.5 W steps */
	if (w > mx) w = mx;
	float k = 2 * w / g->heat_def_w;
	g->cfg.thermal_out_w = 2 * w;
	g->cfg.thermal_out2_w = g->heat_def2_w * k;
	return w;
}

float hifi_gov_heat_now_w(const hifi_gov_t *g) {
	int a = g->amps > 0 ? g->amps : 1;
	return g->p_out_slow * g->gain * g->gain / (2 * a);
}

const char *hifi_gov_why(const hifi_gov_t *g) {
	switch (g->why) {
	case HIFI_GOV_CHARGER: return "charger";
	case HIFI_GOV_CHARGER_PEAK: return "charger_peak";
	case HIFI_GOV_HEAT: return "heat";
	case HIFI_GOV_AMP_HOT: return "amp_hot";
	default: return "";
	}
}

void hifi_gov_set_pvdd(hifi_gov_t *g, float pvdd_v) { g->pvdd_v = pvdd_v > 0 ? pvdd_v : 0; }

float hifi_gov_clip_fs(const hifi_gov_t *g) {
	if (!(g->cfg.vfs_peak_v > 0) || !(g->vsupply > 0)) return 1.0f;
	float v = g->pvdd_v > 0 && g->pvdd_v < g->vsupply ? g->pvdd_v : g->vsupply;
	/* -0.5 dB: ~1.7 V lost in hot switches and coil at 4.75 A peak (review
	 * of k2388); replace with a measured swing */
	float f = 0.944f * g->cfg.swing * v / g->cfg.vfs_peak_v;
	return f < 1.0f ? f : 1.0f;
}
