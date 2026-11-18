/*
 * The quiet stop: what every planned restart and update does before it
 * flashes or restarts, so no pop, click or burst reaches a speaker.
 *
 *   1. ramp the outputs down over HIFI_QUIET_RAMP_MS, in steps;
 *   2. mute the DAC (the codec's own soft mute) and let its ramp finish;
 *   3. put the amp into shutdown (SDZ low, or the core board's resets);
 *   4. let the amp settle; only then flash or restart.
 *
 * hifi_esp.h's hifi_quiet_stop() runs it once on the board; the link wraps
 * esp_restart (hifi_crash.c) so every restart path calls it first, and the
 * OTA writer calls it before it opens the app slot. test_quiet_paths.py
 * checks both. At boot the amp waits for hifi_quiet_amp_allowed().
 */
#ifndef HIFI_QUIET_H
#define HIFI_QUIET_H

#include <stdbool.h>
#include <stdint.h>

#define HIFI_QUIET_RAMP_MS 200
#define HIFI_QUIET_STEPS 10
#define HIFI_QUIET_MUTE_MS 25   /* the ES8388's soft mute ramp is ~17 ms at 44.1 kHz */
#define HIFI_QUIET_SETTLE_MS 30 /* the TPA3118 is in shutdown well within this */

typedef struct {
	void *ctx;
	/* the outputs at frac of where they were (1: as they were, 0: their
	 * lowest); NULL on a board with no output level to ramp */
	void (*level)(void *ctx, float frac);
	void (*mute)(void *ctx); /* the DAC's soft mute; NULL: none */
	void (*amp_off)(void *ctx);
	void (*sleep_ms)(void *ctx, uint32_t ms);
} hifi_quiet_ops_t;

/* runs steps 1 to 4 in order */
void hifi_quiet_run(const hifi_quiet_ops_t *q);

/* a level register's code at frac of its starting code (0 at frac 0) */
int hifi_quiet_code(int start, float frac);

/* May the amp come out of shutdown? Not until the codec is up (its routing
 * applied, so its clocks and registers are steady), not while the hold
 * after that runs, and never once a quiet stop has begun. */
bool hifi_quiet_amp_allowed(bool codec_up, uint32_t hold_until, bool quiet, uint32_t now_ms);

#endif
