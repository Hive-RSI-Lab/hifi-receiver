#include "check.h"
#include "hifi_source.h"

/* who may use the output when Bluetooth, Spotify Connect and AirPlay meet */

enum { NONE = HIFI_SRC_NONE, BT = HIFI_SRC_BT, RAOP = HIFI_SRC_RAOP, CSPOT = HIFI_SRC_CSPOT,
       TEST = HIFI_SRC_TEST, LINE = HIFI_SRC_LINE, PARTY = HIFI_SRC_PARTY };

static void a_phone_start_takes_the_output_from_another_phone(void) {
	int phones[] = {BT, RAOP, CSPOT};
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++) {
			if (i == j) continue;
			/* playing or paused, the newest phone wins */
			CHECK(hifi_source_decide(phones[i], true, phones[j], HIFI_EV_START) == HIFI_SRC_TAKEOVER);
			CHECK(hifi_source_decide(phones[i], false, phones[j], HIFI_EV_START) == HIFI_SRC_TAKEOVER);
		}
}

static void a_phone_start_with_nothing_playing_goes_ahead(void) {
	CHECK(hifi_source_decide(NONE, false, BT, HIFI_EV_START) == HIFI_SRC_ALLOW);
	CHECK(hifi_source_decide(NONE, false, CSPOT, HIFI_EV_START) == HIFI_SRC_ALLOW);
	/* its own start again (a new track, the stream restarting) */
	CHECK(hifi_source_decide(CSPOT, true, CSPOT, HIFI_EV_START) == HIFI_SRC_ALLOW);
	CHECK(hifi_source_decide(BT, false, BT, HIFI_EV_START) == HIFI_SRC_ALLOW);
}

static void lms_test_line_and_party_keep_their_rules(void) {
	/* LMS playing (no external owner, busy): refused, as before */
	CHECK(hifi_source_decide(NONE, true, BT, HIFI_EV_START) == HIFI_SRC_REFUSE);
	CHECK(hifi_source_decide(NONE, true, RAOP, HIFI_EV_START) == HIFI_SRC_REFUSE);
	/* a test signal or the line in playing: refused */
	CHECK(hifi_source_decide(TEST, true, CSPOT, HIFI_EV_START) == HIFI_SRC_REFUSE);
	CHECK(hifi_source_decide(LINE, true, BT, HIFI_EV_START) == HIFI_SRC_REFUSE);
	/* a party follower: decode_external.c stops the party for a cast first
	 * (spec 8.4), so the start then finds the output free; while the party
	 * still holds it, Bluetooth is refused (it is off during a party anyway) */
	CHECK(hifi_source_decide(PARTY, true, BT, HIFI_EV_START) == HIFI_SRC_REFUSE);
	CHECK(!hifi_source_is_phone(PARTY) && !hifi_source_is_phone(TEST) && !hifi_source_is_phone(LINE));
	CHECK(!hifi_source_is_phone(NONE));
}

static void a_source_that_lost_the_output_cannot_touch_it(void) {
	/* stop, flush, pause, play, volume from the phone that was let go */
	CHECK(hifi_source_decide(BT, true, CSPOT, HIFI_EV_OWN) == HIFI_SRC_REFUSE);
	CHECK(hifi_source_decide(BT, false, CSPOT, HIFI_EV_OWN) == HIFI_SRC_REFUSE);
	CHECK(hifi_source_decide(CSPOT, true, RAOP, HIFI_EV_OWN) == HIFI_SRC_REFUSE);
	CHECK(hifi_source_decide(NONE, true, BT, HIFI_EV_OWN) == HIFI_SRC_REFUSE); /* LMS plays */
	CHECK(hifi_source_decide(TEST, true, BT, HIFI_EV_OWN) == HIFI_SRC_REFUSE);
	/* with nothing on the output, a source may set its volume ahead of a start */
	CHECK(hifi_source_decide(NONE, false, BT, HIFI_EV_OWN) == HIFI_SRC_ALLOW);
	/* the owner does what it likes */
	CHECK(hifi_source_decide(RAOP, true, RAOP, HIFI_EV_OWN) == HIFI_SRC_ALLOW);
}

static void the_end_of_a_session_is_acknowledged(void) {
	/* not the owner: yes, but the output is left alone */
	CHECK(hifi_source_decide(BT, true, CSPOT, HIFI_EV_SESSION) == HIFI_SRC_IGNORE);
	CHECK(hifi_source_decide(NONE, true, RAOP, HIFI_EV_SESSION) == HIFI_SRC_IGNORE);
	CHECK(hifi_source_decide(NONE, false, RAOP, HIFI_EV_SESSION) == HIFI_SRC_IGNORE);
	/* the owner: its handler ends it */
	CHECK(hifi_source_decide(RAOP, true, RAOP, HIFI_EV_SESSION) == HIFI_SRC_ALLOW);
}

static void only_the_owner_resumes(void) {
	CHECK(hifi_source_decide(BT, false, BT, HIFI_EV_PLAY) == HIFI_SRC_ALLOW);
	CHECK(hifi_source_decide(CSPOT, true, CSPOT, HIFI_EV_PLAY) == HIFI_SRC_ALLOW);
	/* a phone's play status before its audio starts: acknowledged, the start takes the output */
	CHECK(hifi_source_decide(CSPOT, true, BT, HIFI_EV_PLAY) == HIFI_SRC_IGNORE);
	CHECK(hifi_source_decide(CSPOT, false, BT, HIFI_EV_PLAY) == HIFI_SRC_IGNORE);
	CHECK(hifi_source_decide(NONE, false, BT, HIFI_EV_PLAY) == HIFI_SRC_IGNORE);
	CHECK(hifi_source_decide(BT, true, RAOP, HIFI_EV_PLAY) == HIFI_SRC_IGNORE);
	/* LMS, a test signal, the line in or a party plays: refused, as before */
	CHECK(hifi_source_decide(NONE, true, BT, HIFI_EV_PLAY) == HIFI_SRC_REFUSE);
	CHECK(hifi_source_decide(TEST, true, CSPOT, HIFI_EV_PLAY) == HIFI_SRC_REFUSE);
	CHECK(hifi_source_decide(PARTY, true, BT, HIFI_EV_PLAY) == HIFI_SRC_REFUSE);
}

int main(void) {
	a_phone_start_takes_the_output_from_another_phone();
	a_phone_start_with_nothing_playing_goes_ahead();
	lms_test_line_and_party_keep_their_rules();
	a_source_that_lost_the_output_cannot_touch_it();
	the_end_of_a_session_is_acknowledged();
	only_the_owner_resumes();
	return check_report("source");
}
