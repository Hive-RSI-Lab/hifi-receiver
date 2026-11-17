/*
 * Boost: digital gain past unity, for a speaker that 100% doesn't make loud
 * enough (ask k2388). At 0 dB it does nothing and costs nothing.
 *
 * Above 0 dB the samples are multiplied by the boost and then go through a
 * lookahead peak limiter, so nothing clips and nothing is cut flat. Each
 * sample asks for the gain that keeps it under the ceiling; that request is
 * held 25 ms (a 20 Hz half-cycle, so the gain doesn't follow a bass waveform)
 * and then released over ~0.15 s. The audio is delayed HIFI_BOOST_LA frames
 * (1 ms at 48 kHz); the gain is the minimum request over that window and
 * the one before it, averaged over the window. The gain is then never more
 * than any sample asks for (the average of values that are each at or below
 * it), and it ramps down over the 1 ms before a peak instead of stepping at
 * it. The ceiling is digital full scale, or lower when the supply can't
 * swing the amp that far (the amp's clip point in FS units, from the
 * governor), so the amp doesn't clip either.
 *
 * The delay is in the path only while the limiter is: it comes in when Boost
 * is turned up (the live audio plays while it fills, then a 4 ms crossfade
 * to the delayed audio, so nothing is skipped or clicks) and leaves only
 * after Boost is back at 0 and the delayed audio is silent, so turning Boost
 * down never skips audio either.
 *
 * It runs before the power governor (hifi_gov), which then sees the louder
 * signal and keeps the charger and the amps' heat budget as before. The state
 * is ~1 KB (hifi_main keeps it in PSRAM) and the work ~30 float operations a
 * frame.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HIFI_BOOST_MAX_DB 9.0f
#define HIFI_BOOST_LA 48 /* lookahead, frames: 1.0 ms at 48 kHz, 0.5 ms at 96 kHz */

typedef struct {
	float db;        /* the set boost, 0..HIFI_BOOST_MAX_DB */
	float gain;      /* linear boost applied now (ramps to the set one) */
	float ceiling;   /* linear peak ceiling, <= 1 */
	float release_s;
	/* the limiter */
	float req;       /* the held and released gain request (linear, <= 1) */
	float req_d;     /* 1 - req, kept apart so the release reaches 1 exactly */
	uint32_t hold;   /* frames left before the request may rise */
	uint32_t t;      /* frames seen since it came in (wraps) */
	uint32_t pos;    /* the ring position of the oldest frame */
	float delay[HIFI_BOOST_LA][2]; /* the delayed audio (FS, boosted) */
	float box[HIFI_BOOST_LA];      /* the last LA window minimums */
	float box_sum;
	/* the window minimum: a monotonic queue of (request, frame) */
	float mq_v[HIFI_BOOST_LA + 1];
	uint32_t mq_t[HIFI_BOOST_LA + 1];
	uint32_t mq_head, mq_n;
	uint32_t quiet;  /* frames in a row below -80 dBFS (before the delay) */
	uint32_t fill;   /* frames since the delay came in (to LA + its crossfade) */
	bool primed;     /* the delay is in the path */
	/* what it shows */
	float gr_min;    /* the deepest limiter gain in the last block (linear) */
	float gr_show;   /* the same, held and decaying for the app (linear) */
	bool limiting;
	uint32_t events;
} hifi_boost_t;

void hifi_boost_init(hifi_boost_t *b);
/* clamps to 0..HIFI_BOOST_MAX_DB (NaN is 0) and returns what it set */
float hifi_boost_set_db(hifi_boost_t *b, float db);
/* 0 < ceiling <= 1: the peak the output may reach, in FS */
void hifi_boost_set_ceiling(hifi_boost_t *b, float ceiling);
/* true when it would touch the samples (a boost set, one ramping out, or
 * delayed audio still to play) */
bool hifi_boost_active(const hifi_boost_t *b);
/* takes the delay out of the path now (for a source that plays at unity: a
 * test tone, DJ direct's line copy); keeps the set boost */
void hifi_boost_bypass(hifi_boost_t *b);
/* Processes interleaved stereo in place. bytes_per_frame 4 (s16) or 8 (s32). */
void hifi_boost_process(hifi_boost_t *b, void *buf, size_t frames, int bytes_per_frame, unsigned rate);
/* the limiter's gain reduction for display, in dB (<= 0) */
float hifi_boost_reduction_db(const hifi_boost_t *b);
