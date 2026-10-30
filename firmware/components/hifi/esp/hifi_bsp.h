/*
 * What each board gives the shared board code (hifi_main.c): its pins and
 * parts behind the power sequencer, the input routing, the self-test and
 * the API's info. One of board_lite.c (lite-main: ES8388 codec, GPIO
 * power path) or board_core.c (Plus and Ultra: Sabre DAC, I/O expander,
 * current sense, knob, sub DAC) is built, picked by the Kconfig board.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "hifi_api.h"
#include "hifi_gov.h"
#include "hifi_power.h"
#include "hifi_vol.h"

typedef struct {
	float vbus_v;   /* volts */
	bool pg;        /* the PD contract is good */
	bool fault;     /* an amp says fault */
	float vbus_i_a; /* NaN: the board has no current sense */
	/* the PD sink's registers, when this call read them (every 500 ms) */
	bool pd_read, sink_ok;
	uint8_t pd_status, pd_current;
} hifi_bsp_power_t;

/* At boot, before the tasks: pins, ADC, parts; the board's numbers. */
void hifi_bsp_init(hifi_power_cfg_t *pc, hifi_gov_cfg_t *gc);
/* After the shared tasks: the board's own (core: controls, the sub DAC). */
void hifi_bsp_start(void);

/* Every 10 ms from the power task. */
void hifi_bsp_power_in(uint32_t now_ms, hifi_bsp_power_t *p);
/* Drives PVDD and the amps in a safe order: amps into shutdown before PVDD
 * opens, out of it after PVDD is up. */
void hifi_bsp_power_out(const hifi_power_out_t *o);

/*
 * Puts the input (stream or DJ direct), the sub and, where the analog path
 * skips squeezelite, the volume onto the board's parts. Returns false
 * while they can't take it yet (the codec still starting): called again.
 */
bool hifi_bsp_route(hifi_input_t in, bool sub_on, float sub_db, const hifi_vol_t *vol, uint32_t now_ms);
/* The sub level the board sets for a request of db. */
float hifi_bsp_sub_db(float db);

void hifi_bsp_info(hifi_info_t *i);         /* caps and psram */
void hifi_bsp_snapshot(hifi_snapshot_t *s); /* sensors, zones, Ethernet */
void hifi_bsp_selftest(hifi_selftest_t *t); /* the board's own parts answer */
bool hifi_bsp_parts_ok(void);               /* the same, for the trial boot */

/* Zone B's amp (boards with HIFI_ZONES 2) */
void hifi_bsp_set_zone(bool on);

/* An output block (hifi_dsp_process), after the governor: the core board
 * sends its sub DAC the sum of it. */
void hifi_bsp_audio(const uint8_t *buf, uint32_t frames, int bytes_per_frame);
