/* Bluetooth pairing mode, the − + button combo, and the chimes */
#include "check.h"
#include "hifi_pair.h"

#include <math.h>
#include <stdlib.h>

static void pairing(void) {
	hifi_pair_t p;
	hifi_pair_init(&p);
	CHECK(!p.on && hifi_pair_left_s(&p, 0) == 0 && !hifi_pair_expired(&p, 1000));

	hifi_pair_start(&p, 1000, HIFI_PAIR_S);
	CHECK(p.on && hifi_pair_left_s(&p, 1000) == HIFI_PAIR_S);
	CHECK(hifi_pair_left_s(&p, 1001) == HIFI_PAIR_S); /* rounded up */
	CHECK(hifi_pair_left_s(&p, 1000 + 119500) == 1);
	CHECK(!hifi_pair_expired(&p, 1000 + HIFI_PAIR_S * 1000 - 1));
	CHECK(hifi_pair_expired(&p, 1000 + HIFI_PAIR_S * 1000));
	CHECK(!p.on && !hifi_pair_expired(&p, 1000 + HIFI_PAIR_S * 1000 + 5)); /* once */
	CHECK(HIFI_PAIR_S >= 60 && HIFI_PAIR_S <= 300);

	/* again while on: the clock starts over */
	hifi_pair_start(&p, 0, 10);
	hifi_pair_start(&p, 5000, 10);
	CHECK(!hifi_pair_expired(&p, 10000) && hifi_pair_left_s(&p, 10000) == 5);

	/* cancelled, or a phone connected */
	CHECK(hifi_pair_stop(&p) && !p.on && hifi_pair_left_s(&p, 10000) == 0);
	CHECK(!hifi_pair_stop(&p));
	CHECK(!hifi_pair_expired(&p, 99999));

	/* across the millisecond clock's wrap */
	hifi_pair_start(&p, UINT32_MAX - 500, 2);
	CHECK(hifi_pair_left_s(&p, 100) == 2);
	CHECK(!hifi_pair_expired(&p, 1000));
	CHECK(hifi_pair_expired(&p, 1500));
}

/* runs the combo from t to t + ms in 20 ms polls; returns how many times it fired */
static int hold(hifi_combo_t *c, bool a, bool b, uint32_t *t, uint32_t ms) {
	int fired = 0;
	for (uint32_t k = 0; k < ms; k += 20, *t += 20) fired += hifi_combo_poll(c, a, b, *t);
	return fired;
}

static void combo(void) {
	hifi_combo_t c;
	uint32_t t = 1000;

	/* pins that read pressed from boot (nothing configured them) never fire it */
	hifi_combo_init(&c);
	CHECK(hold(&c, true, true, &t, 10000) == 0 && !hifi_combo_owns(&c));

	/* armed once both are seen up; both held 2 s fires once */
	CHECK(hold(&c, false, false, &t, 100) == 0);
	CHECK(hold(&c, true, false, &t, 300) == 0 && !hifi_combo_owns(&c)); /* one alone: its own events */
	CHECK(hold(&c, true, true, &t, HIFI_COMBO_HOLD_MS - 20) == 0);
	CHECK(hifi_combo_owns(&c)); /* the volume buttons' events are the combo's */
	CHECK(hold(&c, true, true, &t, 40) == 1);
	CHECK(hold(&c, true, true, &t, 5000) == 0); /* held on: no second time */
	/* one let go first, then the other: still the combo's until both are up and a moment after */
	CHECK(hold(&c, false, true, &t, 200) == 0 && hifi_combo_owns(&c));
	CHECK(hold(&c, false, false, &t, HIFI_COMBO_TAIL_MS - 20) == 0 && hifi_combo_owns(&c));
	CHECK(hold(&c, false, false, &t, 40) == 0 && !hifi_combo_owns(&c));

	/* a short hold of both: no pairing, and its events are swallowed rather than step the volume */
	CHECK(hold(&c, true, true, &t, 500) == 0 && hifi_combo_owns(&c));
	CHECK(hold(&c, false, false, &t, 400) == 0 && !hifi_combo_owns(&c));

	/* one let go and pressed again: the 2 s start over */
	CHECK(hold(&c, true, true, &t, 1500) == 0);
	CHECK(hold(&c, true, false, &t, 100) == 0);
	CHECK(hold(&c, true, true, &t, 1500) == 0);
	CHECK(hold(&c, true, true, &t, 600) == 1);
	CHECK(hold(&c, false, false, &t, 400) == 0);

	/* and again later: it fires every hold */
	CHECK(hold(&c, true, true, &t, HIFI_COMBO_HOLD_MS + 40) == 1);
	CHECK(hold(&c, false, false, &t, 400) == 0);

	/* across the clock's wrap */
	t = UINT32_MAX - 1000;
	CHECK(hold(&c, true, true, &t, HIFI_COMBO_HOLD_MS + 40) == 1);
}

static void chimes(void) {
	/* the level: the volume's gain scales it, the cap holds it, mute silences it */
	CHECK(hifi_chime_amp(0) == 0);
	float cap = powf(10, HIFI_CHIME_CAP_DBFS / 20), base = powf(10, HIFI_CHIME_DBFS / 20);
	CHECK(HIFI_CHIME_CAP_DBFS <= -12 && HIFI_CHIME_DBFS >= HIFI_CHIME_CAP_DBFS);
	CHECK_NEAR(hifi_chime_amp(65536) * 1.0f, cap, 1e-5); /* volume 100: at the cap */
	uint32_t g50 = 65536 / 8; /* volume 50: (0.5)^3 */
	CHECK_NEAR(hifi_chime_amp(g50) * g50 / 65536.0f, base / 8, 1e-5); /* follows the volume */
	for (uint32_t g = 1; g <= 4 * 65536; g = g * 3 + 1) {
		float out = hifi_chime_amp(g) * g / 65536.0f; /* what reaches the DAC */
		CHECK(out <= cap * 1.0001f);
		CHECK(hifi_chime_amp(g) <= 1.0f);
	}

	hifi_chime_t c;
	CHECK(!hifi_chime_start(&c, HIFI_CHIME_NONE, 0.5f, 48000));
	CHECK(!hifi_chime_start(&c, HIFI_CHIME_PAIR, 0, 48000)); /* muted: nothing */
	CHECK_STR(hifi_chime_name(HIFI_CHIME_PAIR), "pair");
	CHECK_STR(hifi_chime_name(HIFI_CHIME_CONNECT), "connect");
	CHECK_STR(hifi_chime_name(HIFI_CHIME_DISCONNECT), "disconnect");

	static int16_t buf[2 * 48000];
	for (int k = HIFI_CHIME_PAIR; k <= HIFI_CHIME_DISCONNECT; k++) {
		uint32_t ms = hifi_chime_ms(k);
		CHECK(ms >= 200 && ms <= 600); /* short */
		float amp = 0.25f;
		CHECK(hifi_chime_start(&c, k, amp, 48000));
		size_t n = 0, got;
		while ((got = hifi_chime_render(&c, buf + 2 * n, 128)) > 0) n += got;
		CHECK(n == (size_t) ms * 48);
		CHECK(hifi_chime_render(&c, buf, 128) == 0); /* done stays done */
		int peak = 0, step = 0;
		for (size_t i = 0; i < n; i++) {
			CHECK(buf[2 * i] == buf[2 * i + 1]);
			if (abs(buf[2 * i]) > peak) peak = abs(buf[2 * i]);
			if (i && abs(buf[2 * i] - buf[2 * i - 2]) > step) step = abs(buf[2 * i] - buf[2 * i - 2]);
		}
		CHECK(peak <= amp * 32767 + 1 && peak > amp * 32767 * 0.7f);
		/* no clicks: it starts and ends near 0, and no sample jumps more than
		 * the fastest note's slope at full level allows */
		CHECK(abs(buf[0]) < 50 && abs(buf[2 * (n - 1)]) < 50);
		CHECK(step <= amp * 32767 * 2 * M_PI * 1568 / 48000 + 2);
	}

	/* other rates render the same length */
	CHECK(hifi_chime_start(&c, HIFI_CHIME_CONNECT, 0.1f, 44100));
	size_t n = 0, got;
	while ((got = hifi_chime_render(&c, buf, 300)) > 0) n += got;
	CHECK(n == (size_t) hifi_chime_ms(HIFI_CHIME_CONNECT) * 44100 / 1000);
}

int main(void) {
	pairing();
	combo();
	chimes();
	return check_report("pair");
}
