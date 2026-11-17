/*
 * Boost: digital gain past unity, for a speaker that 100% doesn't make loud
 * enough (ask k2388). At 0 dB it does nothing and costs nothing.
 *
 * Above 0 dB the samples are multiplied by the boost and then go through a
 * peak limiter, so nothing clips: a stereo-linked envelope with an instant
 * attack (it rises to the sample's own peak, so the output can't pass the
 * ceiling), held 25 ms (a 20 Hz half-cycle, so the gain doesn't follow a
 * bass waveform) and then a ~0.15 s exponential release. The ceiling is digital full
 * scale, or lower when the supply can't swing the amp that far (the amp's
 * clip point in FS units, from the governor), so the amp doesn't clip either.
 *
 * It runs before the power governor (hifi_gov), which then sees the louder
 * signal and keeps the charger and the amps' heat budget as before. The state
 * is ~40 bytes and the work a few float operations a sample.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HIFI_BOOST_MAX_DB 9.0f

typedef struct {
	float db;        /* the set boost, 0..HIFI_BOOST_MAX_DB */
	float gain;      /* linear boost applied now (ramps to the set one) */
	float env;       /* the limiter's peak envelope (FS, after the boost) */
	float ceiling;   /* linear peak ceiling, <= 1 */
	float release_s;
	uint32_t hold;   /* samples left before the envelope may fall */
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
/* true when it would touch the samples (a boost set, or one ramping out) */
bool hifi_boost_active(const hifi_boost_t *b);
/* Processes interleaved stereo in place. bytes_per_frame 4 (s16) or 8 (s32). */
void hifi_boost_process(hifi_boost_t *b, void *buf, size_t frames, int bytes_per_frame, unsigned rate);
/* the limiter's gain reduction for display, in dB (<= 0) */
float hifi_boost_reduction_db(const hifi_boost_t *b);
