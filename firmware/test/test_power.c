#include "check.h"
#include "hifi_power.h"

static hifi_power_t p;
static hifi_power_in_t in;

/* lite-main's PVDD switch as GPIO39 sees it: it closes 150 ms after PVDD_EN
 * (the gate RC) and PVDD reads 99 % of VBUS; stuck open; or a board with no
 * PVDD sense (NaN) */
static enum { SW_WORKS, SW_STUCK, SW_UNSENSED } sw;
static uint32_t t_en;
static float pvdd_frac; /* PVDD over VBUS once closed */

/* steps the sequencer in 10 ms ticks for `ms` */
static const hifi_power_out_t *run(uint32_t ms) {
	const hifi_power_out_t *o = NULL;
	for (uint32_t t = 0; t < ms; t += 10) {
		in.now_ms += 10;
		if (!p.out.pvdd_en) t_en = in.now_ms;
		bool closed = sw == SW_WORKS && in.now_ms - t_en >= 150;
		in.pvdd_v = sw == SW_UNSENSED ? NAN : closed ? pvdd_frac * in.vbus_v : 0.0f;
		o = hifi_power_step(&p, &in);
	}
	return o;
}

static void setup(uint32_t start) {
	hifi_power_cfg_t cfg;
	hifi_power_cfg_lite(&cfg);
	hifi_power_init(&p, &cfg, start);
	sw = SW_WORKS;
	pvdd_frac = 0.99f;
	in = (hifi_power_in_t) {.now_ms = start, .vbus_v = 5.0f, .pg = false, .fault = false, .audio = true};
}

static void laptop_port_never_powers_amp(void) {
	setup(0);
	const hifi_power_out_t *o = run(5000);
	CHECK(p.state == HIFI_PWR_WAIT_PD);
	CHECK(!o->pvdd_en && !o->amp_on);
	/* PG alone at 5 V isn't enough either */
	in.pg = true;
	o = run(5000);
	CHECK(p.state == HIFI_PWR_WAIT_PD && !o->pvdd_en);
}

static void normal_power_up(void) {
	setup(0);
	in.pg = true;
	in.vbus_v = 20.1f;
	const hifi_power_out_t *o = run(150);
	CHECK(p.state == HIFI_PWR_WAIT_PD && !o->pvdd_en); /* still settling */
	o = run(100);
	CHECK(p.state == HIFI_PWR_SOFT_START);
	CHECK(o->pvdd_en && !o->amp_on);                   /* PVDD before the amp */
	o = run(350);
	CHECK(p.state == HIFI_PWR_SOFT_START && !o->amp_on); /* PVDD still ramping */
	o = run(100);
	CHECK(p.state == HIFI_PWR_ON);
	CHECK(o->pvdd_en && o->amp_on);

	/* user disables, then enables */
	hifi_power_amp_enable(&p, false);
	o = run(10);
	CHECK(o->pvdd_en && !o->amp_on);
	hifi_power_amp_enable(&p, true);
	o = run(10);
	CHECK(o->amp_on);
}

static void brownout_and_recovery(void) {
	setup(0);
	in.pg = true;
	in.vbus_v = 20.0f;
	run(800);
	CHECK(p.state == HIFI_PWR_ON);
	in.vbus_v = 8.0f; /* charger sagging */
	const hifi_power_out_t *o = run(10);
	CHECK(p.state == HIFI_PWR_UNDERVOLT && !o->pvdd_en && !o->amp_on);
	in.vbus_v = 20.0f;
	o = run(900);
	CHECK(p.state == HIFI_PWR_UNDERVOLT && !o->pvdd_en); /* waits recover_ms */
	o = run(200);
	CHECK(p.state == HIFI_PWR_SOFT_START && o->pvdd_en && !o->amp_on);
	run(400);
	CHECK(p.state == HIFI_PWR_ON);

	in.vbus_v = 26.0f;
	o = run(10);
	CHECK(p.state == HIFI_PWR_OVERVOLT && !o->pvdd_en);

	/* unplugged to a 5 V port: back to waiting */
	in.vbus_v = 5.0f;
	in.pg = false;
	run(10);
	CHECK(p.state == HIFI_PWR_WAIT_PD);
}

/* a 9 or 12 V contract can't turn lite-main's PVDD switch on fully: amp off */
static void low_contract_keeps_amp_off(void) {
	setup(0);
	in.pg = true;
	in.vbus_v = 12.0f;
	const hifi_power_out_t *o = run(5000);
	CHECK(!o->pvdd_en && !o->amp_on);
	in.vbus_v = 15.0f;
	o = run(800);
	CHECK(p.state == HIFI_PWR_ON && o->amp_on);

	/* a 15 V contract sagging under load stays on above the 13 V floor */
	in.vbus_v = 13.8f;
	o = run(5000);
	CHECK(p.state == HIFI_PWR_ON && o->amp_on);
	in.vbus_v = 12.9f;
	o = run(10);
	CHECK(p.state == HIFI_PWR_UNDERVOLT && !o->amp_on);
	/* and it comes back only once VBUS is over the 14 V start floor again */
	in.vbus_v = 13.8f;
	o = run(3000);
	CHECK(p.state == HIFI_PWR_UNDERVOLT && !o->pvdd_en);
	in.vbus_v = 15.0f;
	o = run(1100);
	CHECK(p.state == HIFI_PWR_SOFT_START && o->pvdd_en);
}

static void pg_drop_turns_off(void) {
	setup(0);
	in.pg = true;
	in.vbus_v = 20.0f;
	run(800);
	in.pg = false;
	const hifi_power_out_t *o = run(10);
	CHECK(p.state == HIFI_PWR_WAIT_PD && !o->pvdd_en && !o->amp_on);
}

static void faults_retry_then_latch(void) {
	setup(0);
	in.pg = true;
	in.vbus_v = 20.0f;
	run(800);
	CHECK(p.out.amp_on);

	in.fault = true;
	const hifi_power_out_t *o = run(10);
	/* inside the 50 ms blanking after enable? no: it's been on 200 ms */
	CHECK(!o->amp_on && p.faults == 1 && !p.latched);
	in.fault = false;
	o = run(900);
	CHECK(!o->amp_on);           /* still backing off */
	o = run(200);
	CHECK(o->amp_on);            /* retried */

	/* a fault inside the blanking window right after enable is ignored */
	setup(100000);
	in.pg = true;
	in.vbus_v = 20.0f;
	run(800);
	hifi_power_amp_enable(&p, false);
	run(10);
	hifi_power_amp_enable(&p, true);
	in.fault = true;
	o = run(20);
	CHECK(o->amp_on && p.faults == 0);
	o = run(50);
	CHECK(!o->amp_on && p.faults == 1);

	/* three inside a minute latch */
	in.fault = false;
	run(1100);
	in.fault = true;
	run(100);
	in.fault = false;
	run(1100);
	in.fault = true;
	o = run(100);
	CHECK(p.faults == 3 && p.latched && !o->amp_on);
	in.fault = false;
	o = run(5000);
	CHECK(!o->amp_on && o->pvdd_en); /* latched: PVDD stays, amp stays off */
	hifi_power_amp_reset(&p);
	o = run(10);
	CHECK(o->amp_on && p.faults == 0);
}

static void idle_off(void) {
	setup(0);
	in.pg = true;
	in.vbus_v = 20.0f;
	run(800);
	CHECK(p.out.amp_on);
	in.audio = false;
	const hifi_power_out_t *o = run(p.cfg.idle_off_ms - 1000);
	CHECK(o->amp_on);
	o = run(2000);
	CHECK(!o->amp_on && o->pvdd_en);
	in.audio = true;
	o = run(10);
	CHECK(o->amp_on);
}

static void timer_wraparound(void) {
	setup(0xFFFFFF00u); /* the ms counter wraps during power-up */
	in.pg = true;
	in.vbus_v = 20.0f;
	run(800);
	CHECK(p.state == HIFI_PWR_ON && p.out.amp_on);
}

/* the switch never closes (Q1 open, R17 missing): the amp never runs, it
 * retries every second, and three failures inside a minute latch it off */
static void pvdd_switch_stuck_open(void) {
	setup(0);
	sw = SW_STUCK;
	in.pg = true;
	in.vbus_v = 20.0f;
	const hifi_power_out_t *o = run(700);
	CHECK(p.state == HIFI_PWR_SOFT_START && o->pvdd_en && !o->amp_on); /* waiting past soft start */
	o = run(520); /* 1 s after PVDD_EN */
	CHECK(p.state == HIFI_PWR_NO_PVDD && !o->pvdd_en && !o->amp_on && p.pvdd_fails == 1);
	o = run(900);
	CHECK(p.state == HIFI_PWR_NO_PVDD && !o->pvdd_en); /* waits recover_ms */
	o = run(200);
	CHECK(p.state == HIFI_PWR_SOFT_START && o->pvdd_en);
	o = run(5000);
	CHECK(p.state == HIFI_PWR_NO_PVDD && p.pvdd_fails == 3 && !o->pvdd_en && !o->amp_on);
	o = run(10000);
	CHECK(p.state == HIFI_PWR_NO_PVDD && !o->pvdd_en); /* latched */
	CHECK_STR(hifi_power_state_name(p.state), "no_pvdd");

	/* fixed and reset: the next try runs the amp */
	sw = SW_WORKS;
	hifi_power_amp_reset(&p);
	o = run(1500);
	CHECK(p.state == HIFI_PWR_ON && o->amp_on && p.pvdd_fails == 0);
}

/* a slow switch is waited for: ON only once PVDD reads up */
static void pvdd_slow_close(void) {
	setup(0);
	sw = SW_STUCK;
	in.pg = true;
	in.vbus_v = 20.0f;
	run(800); /* 600 ms into soft start, PVDD still down */
	CHECK(p.state == HIFI_PWR_SOFT_START && !p.out.amp_on);
	sw = SW_WORKS;
	const hifi_power_out_t *o = run(20);
	CHECK(p.state == HIFI_PWR_ON && o->amp_on);
}

/* PVDD opening while on (PG gating it in hardware, Q1 failing): a glitch
 * shorter than pvdd_drop_ms is ignored, a real drop opens everything */
static void pvdd_drops_while_on(void) {
	setup(0);
	in.pg = true;
	in.vbus_v = 20.0f;
	run(800);
	CHECK(p.state == HIFI_PWR_ON && p.out.amp_on);
	sw = SW_STUCK;
	const hifi_power_out_t *o = run(30);
	CHECK(p.state == HIFI_PWR_ON && o->amp_on);
	sw = SW_WORKS;
	o = run(10);
	sw = SW_STUCK;
	o = run(40);
	CHECK(p.state == HIFI_PWR_ON && o->amp_on); /* the dip restarted the clock */
	o = run(20);
	CHECK(p.state == HIFI_PWR_NO_PVDD && !o->pvdd_en && !o->amp_on);
	/* a sagging PVDD (Q1 half on, 3 V across it) counts as down too */
	setup(0);
	in.pg = true;
	in.vbus_v = 20.0f;
	run(800);
	pvdd_frac = 0.84f;
	o = run(40);
	CHECK(p.state == HIFI_PWR_ON);
	o = run(20);
	CHECK(p.state == HIFI_PWR_NO_PVDD && !o->amp_on);
}

/* Plus/Ultra's core has no PVDD sense: the sequencer runs on time alone */
static void pvdd_unsensed(void) {
	setup(0);
	sw = SW_UNSENSED;
	in.pg = true;
	in.vbus_v = 20.0f;
	const hifi_power_out_t *o = run(650);
	CHECK(p.state == HIFI_PWR_ON && o->amp_on);
	o = run(5000);
	CHECK(p.state == HIFI_PWR_ON && o->amp_on);
}

int main(void) {
	laptop_port_never_powers_amp();
	normal_power_up();
	brownout_and_recovery();
	low_contract_keeps_amp_off();
	pg_drop_turns_off();
	faults_retry_then_latch();
	idle_off();
	timer_wraparound();
	pvdd_switch_stuck_open();
	pvdd_slow_close();
	pvdd_drops_while_on();
	pvdd_unsensed();
	CHECK_STR(hifi_power_state_name(HIFI_PWR_SOFT_START), "soft_start");
	return check_report("power");
}
