/*
 * Channel mode: which side of the program the speaker output plays, so one
 * Lite can drive one speaker of a stereo pair. The ES8388's two output
 * mixers can each take either DAC side (stream, line) or either analog input
 * (DJ direct), so the routing is four mixer registers and no DSP. The mixer
 * outputs feed LOUT1/ROUT1 (amp) and LOUT2/ROUT2 (sub out) alike, so on the
 * Lite the sub out follows the channel too.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "hifi_api.h"

/* ES8388 registers 0x27..0x2A (LD2LO/LI2LO, RD2LO/RI2LO, LD2RO/LI2RO, RD2RO/RI2RO),
 * the DAC's digital volume (0x1A, 0x1B; both the same) */
typedef struct {
	uint8_t mix[4];
	uint8_t dac_vol;
} hifi_chan_regs_t;

#define HIFI_CHAN_MIX_FIRST 0x27
#define HIFI_CHAN_DACVOL_L 0x1A
#define HIFI_CHAN_DACVOL_R 0x1B

/* direct: DJ direct, the analog inputs in the mix and the DAC out of it.
 * direct_lin_code: that path's input gain code (bits 5:3; 3 = -3 dB);
 * live: false leaves the source out of the mix (DJ direct muted). */
void hifi_chan_regs(hifi_chan_t c, bool direct, int direct_lin_code, bool live, hifi_chan_regs_t *r);
