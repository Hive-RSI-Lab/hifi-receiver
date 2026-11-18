#include "check.h"
#include "hifi_quiet.h"

/* the quiet stop's steps, as the board would see them */
static char log_[256];
static int ms, levels;
static float last_frac = 2;
static bool mono = true; /* each level at or under the one before */
static int amp_at = -1, mute_at = -1, ramp_end = -1;

static void lv(void *c, float f) {
	(void)c;
	if (f > last_frac) mono = false;
	last_frac = f;
	levels++;
	if (f == 0) ramp_end = ms;
	strcat(log_, "L");
}
static void mu(void *c) {
	(void)c;
	mute_at = ms;
	strcat(log_, "M");
}
static void off(void *c) {
	(void)c;
	amp_at = ms;
	strcat(log_, "A");
}
static void sl(void *c, uint32_t n) {
	(void)c;
	ms += n;
}

static void the_order_is_ramp_mute_amp_settle(void) {
	hifi_quiet_ops_t q = {.level = lv, .mute = mu, .amp_off = off, .sleep_ms = sl};
	hifi_quiet_run(&q);
	CHECK_STR(log_, "LLLLLLLLLLMA");
	CHECK(mono && last_frac == 0 && levels == HIFI_QUIET_STEPS);
	/* the ramp takes 100 to 300 ms, the mute's own ramp ends before the amp goes off */
	CHECK(ms - HIFI_QUIET_MUTE_MS - HIFI_QUIET_SETTLE_MS >= 100);
	CHECK(ramp_end <= 300);
	CHECK(amp_at - mute_at >= 20);
	/* and the amp settles before the restart or the flash */
	CHECK(ms - amp_at >= 20);
}

static void a_board_with_only_an_amp(void) {
	log_[0] = 0;
	ms = 0;
	hifi_quiet_ops_t q = {.amp_off = off, .sleep_ms = sl};
	hifi_quiet_run(&q);
	CHECK_STR(log_, "A");
	CHECK(ms >= HIFI_QUIET_SETTLE_MS);
}

static void codes_ramp_to_the_bottom(void) {
	int prev = 27;
	for (int i = 0; i <= HIFI_QUIET_STEPS; i++) {
		int c = hifi_quiet_code(27, 1.0f - (float) i / HIFI_QUIET_STEPS);
		CHECK(c <= prev && c >= 0);
		prev = c;
	}
	CHECK(prev == 0);
	CHECK(hifi_quiet_code(27, 1) == 27);
	CHECK(hifi_quiet_code(0, 0.5f) == 0);
	CHECK(hifi_quiet_code(33, NAN) == 0);
}

/* the boot: the codec answers at 2.4 s (as the loop below has it), the board's route waits 2 s for
 * squeezelite's init and then takes; the amp waits 2.5 s more */
static void amp_stays_off_at_boot_until_audio_is_steady(void) {
	bool up = false;
	uint32_t hold = 0, first_on = 0;
	for (uint32_t now = 0; now < 8000; now += 10) {
		if (!up && now >= 2400) {
			up = true;
			hold = now + 2500;
		}
		bool on = hifi_quiet_amp_allowed(up, hold, false, now);
		if (on && !first_on) first_on = now;
		if (now < 4900) CHECK(!on);
	}
	CHECK(first_on == 4900);
	/* a hold from before the timer wraps */
	CHECK(!hifi_quiet_amp_allowed(true, 5, false, 0xFFFFFFF0u));
	CHECK(hifi_quiet_amp_allowed(true, 0xFFFFFFF0u, false, 5));
}

static void amp_stays_off_once_quiet(void) {
	CHECK(hifi_quiet_amp_allowed(true, 0, false, 100000));
	CHECK(!hifi_quiet_amp_allowed(true, 0, true, 100000));
	CHECK(!hifi_quiet_amp_allowed(false, 0, false, 100000));
}

int main(void) {
	the_order_is_ramp_mute_amp_settle();
	a_board_with_only_an_amp();
	codes_ramp_to_the_bottom();
	amp_stays_off_at_boot_until_audio_is_steady();
	amp_stays_off_once_quiet();
	return check_report("quiet");
}
