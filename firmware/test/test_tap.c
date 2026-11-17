/* The play button's taps: 1 play/pause, 2 next, 3 previous */
#include "check.h"
#include "hifi_tap.h"

static void taps(void) {
	hifi_tap_t t;
	hifi_tap_init(&t);
	CHECK(hifi_tap_poll(&t, 1000) == 0 && !hifi_tap_pending(&t));

	/* one tap: nothing until the gap has passed, then 1, once */
	hifi_tap_press(&t, 1000);
	CHECK(hifi_tap_pending(&t));
	CHECK(hifi_tap_poll(&t, 1000 + HIFI_TAP_GAP_MS - 1) == 0);
	CHECK(hifi_tap_poll(&t, 1000 + HIFI_TAP_GAP_MS) == 1);
	CHECK(hifi_tap_poll(&t, 5000) == 0 && !hifi_tap_pending(&t));

	/* two taps inside the gap: 2, timed from the second */
	hifi_tap_press(&t, 2000);
	hifi_tap_press(&t, 2250);
	CHECK(hifi_tap_poll(&t, 2000 + HIFI_TAP_GAP_MS) == 0);
	CHECK(hifi_tap_poll(&t, 2250 + HIFI_TAP_GAP_MS) == 2);

	/* the third tap acts at once */
	hifi_tap_press(&t, 3000);
	hifi_tap_press(&t, 3200);
	hifi_tap_press(&t, 3400);
	CHECK(hifi_tap_poll(&t, 3400) == 3);
	CHECK(!hifi_tap_pending(&t));

	/* taps a gap apart are separate */
	hifi_tap_press(&t, 4000);
	CHECK(hifi_tap_poll(&t, 4000 + HIFI_TAP_GAP_MS) == 1);
	hifi_tap_press(&t, 4000 + HIFI_TAP_GAP_MS + 10);
	CHECK(hifi_tap_poll(&t, 4000 + 2 * HIFI_TAP_GAP_MS + 10) == 1);

	/* the millisecond clock wrapping doesn't stall a count */
	hifi_tap_press(&t, UINT32_MAX - 100);
	CHECK(hifi_tap_poll(&t, UINT32_MAX - 100 + 50) == 0);
	CHECK(hifi_tap_poll(&t, (uint32_t) (UINT32_MAX - 100 + HIFI_TAP_GAP_MS)) == 1);
}

int main(void) {
	taps();
	return check_report("tap");
}
