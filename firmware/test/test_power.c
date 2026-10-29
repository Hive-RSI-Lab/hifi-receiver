#include "check.h"
#include "hifi_power.h"

static hifi_power_t p;
static hifi_power_in_t in;

/* steps the sequencer in 10 ms ticks for `ms` */
static const hifi_power_out_t *run(uint32_t ms) {
	const hifi_power_out_t *o = NULL;
	for (uint32_t t = 0; t < ms; t += 10) {
		in.now_ms += 10;
		o = hifi_power_step(&p, &in);
	}
	return o;
}

static void setup(uint32_t start) {
	hifi_power_cfg_t cfg;
	hifi_power_cfg_lite(&cfg);
	hifi_power_init(&p, &cfg, start);
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

int main(void) {
	laptop_port_never_powers_amp();
	normal_power_up();
	brownout_and_recovery();
	low_contract_keeps_amp_off();
	pg_drop_turns_off();
	faults_retry_then_latch();
	idle_off();
	timer_wraparound();
	CHECK_STR(hifi_power_state_name(HIFI_PWR_SOFT_START), "soft_start");
	return check_report("power");
}
