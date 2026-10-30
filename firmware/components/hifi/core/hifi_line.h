/*
 * The core board's line in, in plain C: the ADC's I2S words to samples, the
 * sub mix, and DJ direct's thermal guard.
 *
 * Core runs I2S0 with 32-bit slots (BCLK 64 fs: the ES7243E takes its clock
 * from BCLK and needs 64 fs or more, review-3 R3-AUDIO-3), so the ADC's
 * samples and the sub DAC's arrive and leave as 32-bit words.
 *
 * DJ direct is analog (the TS5A23157 puts the line in on the amps), so the
 * governor can't turn it down. The guard watches it instead (review-3
 * R3-SYSTEM-4): the 60 s average of the input power, the larger of the
 * INA186's reading and an estimate from the ADC's copy of the program,
 * against the amps' thermal budget; and the amps' OTW_CLIP. When either
 * says the amps are getting too hot, DJ direct hands over to the governed
 * line in, which turns the program down instead of letting the amps trip.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hifi_gov.h"

/* 32-bit I2S words (the sample in the top 16 bits) to 16-bit stereo; invert
 * flips the polarity (firmware requirement C5: the DJ-direct path reaches
 * the speakers inverted, so the subs follow it) */
void hifi_line_to_s16(const int32_t *in, int16_t *out, size_t frames, bool invert);

/* The subs' feed: (L + R) / 2 x gain to both channels of 32-bit I2S words.
 * in is bytes_per_frame 4 (s16) or 8 (s32) stereo */
void hifi_sub_mix(const void *in, int bytes_per_frame, int32_t *out, size_t frames, float gain);

/* The speaker power (W, both channels, `amps` stereo amps) a block of
 * s16 stereo asks for, scaled as the governor scales the DAC's samples:
 * the board matches the line in's level to the DAC's full scale */
float hifi_line_block_w(const hifi_gov_cfg_t *c, const int16_t *buf, size_t frames, int amps);

typedef struct {
	float tau_s;        /* the power average; 60 */
	uint32_t otw_ms;    /* OTW_CLIP low this long trips it; 200 */
	float efficiency, idle_w; /* the governor's: speaker power to input power */
} hifi_djg_cfg_t;

typedef struct {
	hifi_djg_cfg_t cfg;
	float out_w;        /* averaged speaker power from the ADC's copy */
	float meas_w;       /* averaged measured input power */
	uint32_t otw_since; /* when OTW went low */
	bool otw_low;
	const char *tripped; /* the last trip's reason, NULL none */
} hifi_djg_t;

void hifi_djg_cfg_default(hifi_djg_cfg_t *c, const hifi_gov_cfg_t *gc);
void hifi_djg_init(hifi_djg_t *g, const hifi_djg_cfg_t *c);
/* starts the averages again (DJ direct comes on); keeps the last reason */
void hifi_djg_reset(hifi_djg_t *g);
/* each ADC block in DJ direct: its speaker power over dt_s */
void hifi_djg_audio(hifi_djg_t *g, float out_w, float dt_s);
/* the input power the guard sees: the larger of the two averages */
float hifi_djg_in_w(const hifi_djg_t *g);
/* The amps' thermal budget as input power, for the governor's thermal cap
 * with `amps` stereo amps playing */
float hifi_djg_limit_w(const hifi_gov_cfg_t *gc, int amps);
/*
 * Every power step in DJ direct: in_w_meas the measured input power (NaN:
 * none), otw the amps' warning. Returns why DJ direct must hand over
 * ("power" or "otw", kept in tripped), or NULL.
 */
const char *hifi_djg_step(hifi_djg_t *g, uint32_t now_ms, float dt_s, float in_w_meas, bool otw, float limit_w);
