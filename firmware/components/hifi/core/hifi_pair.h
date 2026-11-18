/*
 * Bluetooth pairing mode and the box's chimes.
 *
 * The Bluetooth sink is hidden (connectable, not discoverable) unless it is
 * in pairing mode: phones already paired reconnect as before, a new one
 * can only find the box while pairing mode is on. Holding − and + together
 * for HIFI_COMBO_HOLD_MS (or the app's "Pair a new phone") turns it on for
 * HIFI_PAIR_S; it ends on timeout, when a phone connects, or when cancelled.
 *
 * Three short chimes say what happened: pairing mode on, a phone connected,
 * a phone gone. They are a few sine notes generated on the fly (no stored
 * samples), played at the box's volume and never louder than
 * HIFI_CHIME_CAP_DBFS.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ pairing mode */

#define HIFI_PAIR_S 120

typedef struct {
	bool on;
	uint32_t until_ms;
} hifi_pair_t;

void hifi_pair_init(hifi_pair_t *p);
/* Turns pairing mode on for `secs` from now (again, if it was on already). */
void hifi_pair_start(hifi_pair_t *p, uint32_t now_ms, int secs);
/* Turns it off; true when it was on. */
bool hifi_pair_stop(hifi_pair_t *p);
/* True once, when the time has run out (it is off after). */
bool hifi_pair_expired(hifi_pair_t *p, uint32_t now_ms);
/* Seconds left, rounded up; 0 when off. */
int hifi_pair_left_s(const hifi_pair_t *p, uint32_t now_ms);

/* ------------------------------------------------------------------ the button combo */

#define HIFI_COMBO_HOLD_MS 2000
/* after both are up, the volume buttons' late events still belong to the combo */
#define HIFI_COMBO_TAIL_MS 300

typedef struct {
	bool armed;     /* both buttons have been seen up: a pin that reads low
			 * because nothing configured it never fires the combo */
	bool both;      /* both held now */
	bool fired;     /* this hold has fired already */
	bool active;    /* a combo is under way: the volume buttons' events are its */
	bool tail;      /* both up again, since up_ms */
	uint32_t since_ms, up_ms;
} hifi_combo_t;

void hifi_combo_init(hifi_combo_t *c);
/* Called every few tens of ms with the two buttons' states (true = pressed).
 * True once per hold, when both have been held HIFI_COMBO_HOLD_MS. */
bool hifi_combo_poll(hifi_combo_t *c, bool down_a, bool down_b, uint32_t now_ms);
/* Whether a volume button's event now is part of the combo (and is to be
 * swallowed rather than step the volume or skip a track). */
bool hifi_combo_owns(const hifi_combo_t *c);

/* ------------------------------------------------------------------ chimes */

typedef enum {
	HIFI_CHIME_NONE = 0,
	HIFI_CHIME_PAIR,       /* pairing mode on: three notes up */
	HIFI_CHIME_CONNECT,    /* a phone connected: two notes up */
	HIFI_CHIME_DISCONNECT, /* a phone gone: two notes down */
} hifi_chime_kind_t;

/* a chime's level at volume 100 (the volume's gain scales it down from there) */
#define HIFI_CHIME_DBFS (-14.0f)
/* the loudest a chime plays after the volume (a chime at full volume is this) */
#define HIFI_CHIME_CAP_DBFS (-20.0f)
#define HIFI_CHIME_NOTES_MAX 3

typedef struct {
	hifi_chime_kind_t kind;
	unsigned rate;
	float amp;          /* the notes' peak, before the output's gain */
	uint8_t note, notes;
	uint32_t n, len;    /* frames into this note, and its length */
	/* the note's sine as a turning phasor: four multiplies a sample, no
	 * sin() (a double sin a sample is too slow on the ESP32) */
	float re, im, cr, ci;
} hifi_chime_t;

const char *hifi_chime_name(hifi_chime_kind_t k);

/*
 * The peak (0..1) to render a chime at so that, after the output's `gain`
 * (16.16, 65536 = 0 dB; the volume's gain), it plays at HIFI_CHIME_DBFS
 * scaled by the volume and never above HIFI_CHIME_CAP_DBFS. 0 when the gain
 * is 0 (muted): no chime.
 */
float hifi_chime_amp(uint32_t gain);

/* Starts `kind` at `amp`; false (and nothing to play) for HIFI_CHIME_NONE or amp 0. */
bool hifi_chime_start(hifi_chime_t *c, hifi_chime_kind_t kind, float amp, unsigned rate);
/* Renders up to `frames` of interleaved stereo s16; returns how many (0 once done). */
size_t hifi_chime_render(hifi_chime_t *c, int16_t *out, size_t frames);
/* The whole chime's length in ms. */
uint32_t hifi_chime_ms(hifi_chime_kind_t kind);
