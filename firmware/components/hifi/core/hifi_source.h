/*
 * Who may use the output when a phone source starts: the newest of
 * Bluetooth, Spotify Connect and AirPlay takes it from the other two.
 *
 * squeezelite's decode_external.c asks hifi_source_decide() before each
 * sink event. A start (Bluetooth audio starting, a Spotify track starting,
 * an AirPlay session being set up) takes the output from another phone
 * source, playing or not: that source is paused and let go, behind the
 * quiet fade. A source never takes the output from LMS, a test signal, the
 * line in or a party while those play (they decide for themselves, as
 * before). Any other event of a source that doesn't own the output is
 * refused, so a source that lost the output can't stop, flush, pause or set
 * the volume of the one that has it; the end of its own session is
 * acknowledged without touching the output. A resume (play after a pause)
 * acts on the output only for its owner. Any other source's resume is
 * acknowledged without touching it (a phone's play status can come before
 * its audio starts, and that start takes the output), so a stray resume
 * can't leave the output running with no owner; while LMS, a test signal,
 * the line in or a party plays it is refused, as before.
 */
#ifndef HIFI_SOURCE_H
#define HIFI_SOURCE_H

#include <stdbool.h>

/* who owns the output: decode_external.c's DECODE_* values */
enum { HIFI_SRC_NONE = 0, HIFI_SRC_BT, HIFI_SRC_RAOP, HIFI_SRC_CSPOT, HIFI_SRC_TEST, HIFI_SRC_LINE, HIFI_SRC_PARTY };

typedef enum {
	HIFI_EV_START,   /* the source starts playing: may take the output */
	HIFI_EV_OWN,     /* acts on the output: only for its owner, or with none */
	HIFI_EV_SESSION, /* the source's session ends: the output only if it owns it */
	HIFI_EV_PLAY,    /* the source resumes: only its owner acts on it; its next start takes the output */
} hifi_ev_class_t;

typedef enum {
	HIFI_SRC_ALLOW,    /* go ahead */
	HIFI_SRC_TAKEOVER, /* let the owner go first (hifi_source_is_phone(owner)), then go ahead */
	HIFI_SRC_REFUSE,   /* tell the source no */
	HIFI_SRC_IGNORE,   /* say yes, but leave the output alone */
} hifi_src_verdict_t;

bool hifi_source_is_phone(int src);

/* owner: output.external; busy: the output plays (output.state above
 * OUTPUT_STOPPED); own: the asking source; ev: a hifi_ev_class_t. Returns a
 * hifi_src_verdict_t, as an int so squeezelite can declare it without this
 * header (it reaches it through a weak reference) */
int hifi_source_decide(int owner, bool busy, int own, int ev);

#endif
