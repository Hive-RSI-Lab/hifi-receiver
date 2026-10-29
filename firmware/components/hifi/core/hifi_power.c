#include "hifi_power.h"

#include <string.h>

#define ELAPSED(now, t) ((uint32_t) ((now) - (t)))

void hifi_power_cfg_lite(hifi_power_cfg_t *cfg) {
	*cfg = (hifi_power_cfg_t) {
		/* TPA3118: 4.5-26 V; the 20 V contract may sit at 19-21 V. 9 V is the
		 * floor product.md set; 25 V leaves a volt under the amp's limit. */
		.v_min = 9.0f,
		.v_max = 25.0f,
		.settle_ms = 200,
		/* lite-main's Miller soft start ramps PVDD in about 6 ms */
		.soft_start_ms = 100,
		.recover_ms = 1000,
		.fault_blank_ms = 50,
		.fault_retry_ms = 1000,
		.fault_window_ms = 60000,
		.fault_latch = 3,
		.idle_off_ms = 10 * 60 * 1000,
	};
}

void hifi_power_init(hifi_power_t *p, const hifi_power_cfg_t *cfg, uint32_t now_ms) {
	memset(p, 0, sizeof(*p));
	p->cfg = *cfg;
	p->state = HIFI_PWR_WAIT_PD;
	p->t_state = now_ms;
	p->user_enable = true;
	p->t_audio = now_ms;
}

const char *hifi_power_state_name(hifi_pwr_state_t s) {
	switch (s) {
	case HIFI_PWR_WAIT_PD: return "wait_pd";
	case HIFI_PWR_SOFT_START: return "soft_start";
	case HIFI_PWR_ON: return "on";
	case HIFI_PWR_UNDERVOLT: return "undervolt";
	case HIFI_PWR_OVERVOLT: return "overvolt";
	}
	return "?";
}

static void enter(hifi_power_t *p, hifi_pwr_state_t s, uint32_t now) {
	p->state = s;
	p->t_state = now;
}

void hifi_power_amp_enable(hifi_power_t *p, bool enable) { p->user_enable = enable; }

void hifi_power_amp_reset(hifi_power_t *p) {
	p->latched = false;
	p->backoff = false;
	p->faults = 0;
	p->fault_n = 0;
}

static void record_fault(hifi_power_t *p, uint32_t now) {
	p->faults++;
	p->fault_t[p->fault_n % 8] = now;
	p->fault_n++;
	int recent = 0;
	for (int i = 0; i < 8 && i < p->fault_n; i++) {
		if (ELAPSED(now, p->fault_t[i]) <= p->cfg.fault_window_ms) recent++;
	}
	if (recent >= p->cfg.fault_latch) p->latched = true;
	p->backoff = true;
	p->t_backoff = now;
}

const hifi_power_out_t *hifi_power_step(hifi_power_t *p, const hifi_power_in_t *in) {
	const hifi_power_cfg_t *c = &p->cfg;
	uint32_t now = in->now_ms;
	bool in_range = in->vbus_v >= c->v_min && in->vbus_v <= c->v_max;
	bool good = in->pg && in_range;

	if (good && !p->good_prev) p->t_good = now;
	p->good_prev = good;
	bool settled = good && ELAPSED(now, p->t_good) >= c->settle_ms;

	if (in->audio) p->t_audio = now;

	switch (p->state) {
	case HIFI_PWR_WAIT_PD:
		if (settled) enter(p, HIFI_PWR_SOFT_START, now);
		break;
	case HIFI_PWR_SOFT_START:
	case HIFI_PWR_ON:
		if (!in->pg) enter(p, HIFI_PWR_WAIT_PD, now);
		else if (in->vbus_v < c->v_min) enter(p, HIFI_PWR_UNDERVOLT, now);
		else if (in->vbus_v > c->v_max) enter(p, HIFI_PWR_OVERVOLT, now);
		else if (p->state == HIFI_PWR_SOFT_START && ELAPSED(now, p->t_state) >= c->soft_start_ms)
			enter(p, HIFI_PWR_ON, now);
		break;
	case HIFI_PWR_UNDERVOLT:
	case HIFI_PWR_OVERVOLT:
		if (!in->pg && in->vbus_v < c->v_min) enter(p, HIFI_PWR_WAIT_PD, now);
		else if (good && ELAPSED(now, p->t_good) >= c->recover_ms) enter(p, HIFI_PWR_SOFT_START, now);
		break;
	}

	/* faults count only while the amp is out of shutdown and past blanking */
	if (p->amp_was_on && in->fault && ELAPSED(now, p->t_amp_on) >= c->fault_blank_ms) record_fault(p, now);
	if (p->backoff && !p->latched && ELAPSED(now, p->t_backoff) >= c->fault_retry_ms) p->backoff = false;

	bool idle = c->idle_off_ms && ELAPSED(now, p->t_audio) >= c->idle_off_ms;
	bool powered = p->state == HIFI_PWR_SOFT_START || p->state == HIFI_PWR_ON;
	bool amp = p->state == HIFI_PWR_ON && p->user_enable && !p->latched && !p->backoff && !idle;

	if (amp && !p->amp_was_on) p->t_amp_on = now;
	p->amp_was_on = amp;
	p->out.pvdd_en = powered;
	p->out.amp_on = amp;
	return &p->out;
}
