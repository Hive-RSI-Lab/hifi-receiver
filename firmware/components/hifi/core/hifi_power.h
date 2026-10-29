/*
 * Power sequencer: decides when the amplifier's supply (PVDD) and the amp
 * itself (SDZ) may be on, from VBUS, the PD sink's power-good and the amp's
 * fault pin. Pure logic, stepped every few ms by the board code.
 *
 * Rules (docs/firmware.md, "Power"):
 *  - PVDD closes only when PG says the contract is good and VBUS has sat in
 *    [v_min, v_max] for settle_ms. On lite-main PG also gates PVDD in
 *    hardware, so a 5 V port can't power the amp even if this code is wrong.
 *  - The amp leaves shutdown soft_start_ms after PVDD closes, and only when
 *    the user hasn't disabled it, it isn't faulted, and there is audio to
 *    play (or idle_off_ms is 0).
 *  - VBUS out of range while on opens everything at once; it comes back
 *    after recover_ms in range.
 *  - A fault (FAULTZ low while enabled) holds the amp off for
 *    fault_retry_ms; fault_latch faults inside fault_window_ms latch it off
 *    until hifi_power_amp_reset().
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
	HIFI_PWR_WAIT_PD = 0,
	HIFI_PWR_SOFT_START,
	HIFI_PWR_ON,
	HIFI_PWR_UNDERVOLT,
	HIFI_PWR_OVERVOLT,
} hifi_pwr_state_t;

typedef struct {
	float v_min, v_max;        /* VBUS window for the amp, volts */
	uint32_t settle_ms;        /* VBUS good this long before PVDD closes */
	uint32_t soft_start_ms;    /* PVDD ramp before the amp may run */
	uint32_t recover_ms;       /* after under/over-voltage */
	uint32_t fault_blank_ms;   /* FAULTZ ignored this long after enabling */
	uint32_t fault_retry_ms;
	uint32_t fault_window_ms;
	int fault_latch;
	uint32_t idle_off_ms;      /* amp to shutdown after this much silence; 0 = never */
} hifi_power_cfg_t;

typedef struct {
	uint32_t now_ms;
	float vbus_v;
	bool pg;          /* PD contract good (CH224 PG low) */
	bool fault;       /* amp FAULTZ asserted (low) */
	bool audio;       /* something non-silent is playing, or DJ direct is on */
} hifi_power_in_t;

typedef struct {
	bool pvdd_en;
	bool amp_on; /* SDZ high */
} hifi_power_out_t;

typedef struct {
	hifi_power_cfg_t cfg;
	hifi_pwr_state_t state;
	uint32_t t_state;      /* when the state was entered */
	uint32_t t_good;       /* since when the supply has been good */
	bool good_prev;
	bool user_enable;
	bool latched;
	int faults;            /* since boot or the last reset */
	uint32_t fault_t[8];   /* ring of recent fault times */
	int fault_n;
	bool backoff;
	uint32_t t_backoff;
	bool amp_was_on;
	uint32_t t_amp_on;
	uint32_t t_audio;      /* last time audio was seen */
	hifi_power_out_t out;
} hifi_power_t;

/* lite-main: 9-25 V window around the 20 V contract */
void hifi_power_cfg_lite(hifi_power_cfg_t *cfg);
void hifi_power_init(hifi_power_t *p, const hifi_power_cfg_t *cfg, uint32_t now_ms);
const hifi_power_out_t *hifi_power_step(hifi_power_t *p, const hifi_power_in_t *in);
void hifi_power_amp_enable(hifi_power_t *p, bool enable);
void hifi_power_amp_reset(hifi_power_t *p);
const char *hifi_power_state_name(hifi_pwr_state_t s);
