/*
 * The receiver's board code on ESP-IDF: reads the power path, runs the power
 * sequencer and governor from core/, plays test signals into squeezelite's
 * output, and implements the test API's operations (hifi_ops_t).
 *
 * Squeezelite-esp32 calls in through hooks our patch adds (firmware/patches):
 * target_hifi.init at boot, hifi_dsp_process on every output block,
 * hifi_http_register when its web server starts, hifi_volume_hook on every
 * source's volume, hifi_display_* with what is playing, hifi_ir_hook and
 * hifi_button_hook on each remote code and button. They all live in this
 * file so the linker pulls it in with the target.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2c.h"
#include "driver/i2s.h"
#include "esp_attr.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_spi_flash.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "mdns.h"
#include "nvs_flash.h"

#include "audio_controls.h"
#include "network_manager.h"
#include "nvs_utilities.h"
#include "platform_config.h"

#include "hifi_api.h"
#include "hifi_boost.h"
#include "hifi_board.h"
#include "hifi_bsp.h"
#include "hifi_chan.h"
#include "hifi_gate.h"
#include "hifi_crash.h"
#include "hifi_esp.h"
#include "hifi_gov.h"
#include "hifi_http.h"
#include "hifi_ir.h"
#ifdef CONFIG_HIFI_LAB
#include "hifi_lab_esp.h"
#endif
#include "hifi_json.h"
#include "hifi_line.h"
#include "hifi_meta.h"
#include "hifi_tap.h"
#include "hifi_pd.h"
#include "hifi_power.h"
#include "hifi_quiet.h"
#include "hifi_tone.h"
#include "hifi_vol.h"
#include "hifi_ws.h"
#include "hifi_ws_proto.h"

static const char TAG[] = "hifi";

/* squeezelite-esp32, through our patch; weak so a build without them links */
extern void hifi_sink_start(uint32_t rate) __attribute__((weak));
extern uint32_t hifi_sink_write(const uint8_t *data, uint32_t len) __attribute__((weak));
extern void hifi_sink_stop(void) __attribute__((weak));
extern uint32_t hifi_sink_queued_ms(void) __attribute__((weak));
extern void hifi_line_start(uint32_t rate, bool unity) __attribute__((weak));
extern int hifi_sink_source(void) __attribute__((weak));
extern void equalizer_set_gain(int8_t *gain) __attribute__((weak));
extern int set_squeezelite_player_name(FILE *f, const char *name) __attribute__((weak));
extern int set_cspot_player_name(FILE *f, const char *name) __attribute__((weak));
extern bool bt_sink_pair_mode(void) __attribute__((weak));
extern void hifi_bt_sink_boot_off(void) __attribute__((weak));
extern bool hifi_output_gain(unsigned left, unsigned right) __attribute__((weak));

/* ------------------------------------------------------------------ state */

static SemaphoreHandle_t lock;       /* power, contract and settings below */
static hifi_power_t power;
static hifi_gov_t gov;
static hifi_contract_t contract;
static float vbus_v, pvdd_v = NAN, vbus_i_a = NAN;
static bool pg, fault, sink_ok;
static uint8_t pd_status;
static hifi_input_t input = HIFI_INPUT_STREAM;
static bool sub_on = true;
static float sub_db = 0;
/* Boost and its lookahead limiter (k2388); ~1 KB, in PSRAM to spare the
 * internal heap */
static EXT_RAM_ATTR hifi_boost_t boost;
static volatile bool amp_hot; /* the amp's over-temperature warning is held */
static hifi_chan_t channel = HIFI_CHAN_STEREO;
static volatile bool codec_dirty = true; /* re-apply input/sub to the codec */
/* the power task's routes: started counts each one begun, routed is the
 * started count of the last one that went through. A route begun after a
 * request reads that request's settings (selftest_loopback waits for one). */
static volatile uint32_t codec_started, codec_routed;
/* the amps stay in shutdown until then: across an input change (the analog
 * switch moves only with them off, firmware requirement C5) and for 2.5 s
 * after the codec first takes its routing, so its VMID has settled before
 * the amp hears it (L4); review-3 R3-SYSTEM-5 */
static volatile uint32_t amp_hold_until, route_at;
static bool codec_up;
/* the quiet stop (hifi_quiet_stop): quiet_req stops the routing (and a
 * step with it set acks in quiet_ack, so a write already begun has landed);
 * amp_off_req, set only once the ramp and the mute are done, holds the amps
 * in shutdown (a step with it set acks in amp_ack) */
static volatile bool quiet_req, quiet_ack, amp_off_req, amp_ack;
static bool in_recovery; /* recovery plays nothing: its amps stay off */
static TaskHandle_t power_task_h;
static uint32_t otw_ms, otw_clear_ms, otw_high_ms; /* the OTW step-down (C2) */
/* a trip taken as heat (Lite, cap above the default): the last one, the
 * gain trim held until, and when the amp was last off (a short trips at once) */
static uint32_t heat_trip_ms, trim_hold_until;
static volatile uint32_t amp_off_ms; /* the last power-task pass with the amp off */
static bool heat_trip_any;
static volatile uint32_t selftest_hold_until; /* keeps the amp awake for the self-test */
static volatile bool capture_busy;   /* the codec's ADC is ours until the capture ends */
/* the self-test's loopback check measures with the user's sound settings
 * out: no EQ or loudness (hifi_eq_bypass), stereo (hifi_chan_mix and the codec) */
static volatile bool loop_flat;

static hifi_vol_t vol;               /* under lock, like what follows */
static bool vol_dirty, vol_from_source;
static uint32_t vol_changed_ms;
static hifi_meta_t meta;
static hifi_ir_t ir;
static hifi_tap_t taps;           /* the play button's taps; under lock */
static TaskHandle_t tap_task;
static char last_ctl[24];            /* the last button, knob step or remote action */
static uint32_t last_ctl_ms;
static volatile uint32_t heap_boot; /* the boot heap (hifi_boot.h); 0: not taken yet */

static volatile bool audio_active;   /* set by the audio thread */

/* the line in through the ADC (boards whose caps say line: core) */
#define LINE_RATE 48000
#define LINE_BLOCK 128      /* frames a read: 2.7 ms */
#define LINE_QUEUE_MS 20    /* ahead of squeezelite's output at most; more is dropped */
static bool board_line;
static volatile bool line_unity;     /* the line sink plays at unity (DJ direct) */
static hifi_djg_t djg;               /* DJ direct's thermal guard */

/* the tone player */
static TaskHandle_t tone_task;
static SemaphoreHandle_t tone_lock;
static hifi_tone_t tone;
static volatile bool tone_active, tone_stop_req;
static hifi_tone_params_t tone_params;

#define LOCK() xSemaphoreTake(lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(lock)

static uint32_t now_ms(void) { return (uint32_t) (esp_timer_get_time() / 1000); }

/* ------------------------------------------------------------------ I2C */

/*
 * Until squeezelite's codec start (or hifi_i2c_ensure) installs the driver,
 * every transfer fails and the driver logs an error for it. power_task probes
 * the codec every 10 ms, which buried a boot log under ~700 such lines. While
 * the driver is missing, ask again only every 250 ms and with its log quiet.
 */
static bool i2c_missing = true;
static uint32_t i2c_probe_at;

static esp_err_t i2c_xfer(uint8_t addr, uint8_t reg, uint8_t *val, bool write) {
	bool probe = i2c_missing;
	if (probe) {
		uint32_t now = now_ms();
		if (i2c_probe_at && (int32_t) (now - i2c_probe_at) < 250) return ESP_ERR_INVALID_STATE;
		i2c_probe_at = now ? now : 1;
		esp_log_level_set("i2c", ESP_LOG_NONE);
	}
	i2c_cmd_handle_t cmd = i2c_cmd_link_create();
	i2c_master_start(cmd);
	i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
	i2c_master_write_byte(cmd, reg, true);
	if (write) {
		i2c_master_write_byte(cmd, *val, true);
	} else {
		i2c_master_start(cmd);
		i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_READ, true);
		i2c_master_read_byte(cmd, val, I2C_MASTER_NACK);
	}
	i2c_master_stop(cmd);
	esp_err_t err = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(50));
	i2c_cmd_link_delete(cmd);
	if (probe) esp_log_level_set("i2c", CONFIG_LOG_DEFAULT_LEVEL);
	/* ESP_ERR_INVALID_STATE: no driver (a missing part NACKs with ESP_FAIL) */
	i2c_missing = err == ESP_ERR_INVALID_STATE;
	return err;
}

esp_err_t hifi_i2c_read(uint8_t addr, uint8_t reg, uint8_t *val) { return i2c_xfer(addr, reg, val, false); }
esp_err_t hifi_i2c_write(uint8_t addr, uint8_t reg, uint8_t val) { return i2c_xfer(addr, reg, &val, true); }

/*
 * Squeezelite installs the I2C driver when it starts the codec (Lite) or its
 * services (core). If it hasn't after a few seconds (recovery firmware, no
 * codec config), install it here so the PD contract can still be read.
 */
void hifi_i2c_ensure(void) {
	static bool tried;
	uint8_t v;
	if (tried || now_ms() < 8000 || hifi_i2c_read(CH224_I2C_ADDR, CH224_REG_STATUS, &v) != ESP_ERR_INVALID_STATE) return;
	tried = true;
	i2c_config_t c = {
		.mode = I2C_MODE_MASTER, .sda_io_num = I2C_SDA, .scl_io_num = I2C_SCL,
		.sda_pullup_en = GPIO_PULLUP_ENABLE, .scl_pullup_en = GPIO_PULLUP_ENABLE,
		.master.clk_speed = 100000,
	};
	i2c_param_config(I2C_PORT, &c);
	i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
	i2c_probe_at = 0; /* the next transfer tries it at once */
	ESP_LOGW(TAG, "codec driver didn't start; installed I2C %d ourselves", I2C_PORT);
}

static void input_changed(hifi_input_t in, bool save);

/* ------------------------------------------------------------------ power task */

static void power_task(void *arg) {
	TickType_t wake = xTaskGetTickCount();
	static hifi_bsp_power_t bp; /* keeps the PD sink's last reading between reads */
	for (;;) {
		vTaskDelayUntil(&wake, pdMS_TO_TICKS(10));
		uint32_t now = now_ms();
		hifi_bsp_power_in(now, &bp);

		/* the routing: re-applied once the board's parts take it, and on request */
		if (codec_dirty && !capture_busy && !quiet_req && (int32_t) (now - route_at) >= 0) {
			LOCK();
			hifi_input_t in = input;
			bool on = sub_on;
			float db = sub_db;
			hifi_vol_t v = vol;
			UNLOCK();
			/* cleared before the route, so a request made while it runs
			 * (it may have read the old settings) routes again */
			uint32_t gen = ++codec_started;
			codec_dirty = false;
			if (!hifi_bsp_route(in, on, db, &v, now)) codec_dirty = true;
			else {
				codec_routed = gen;
				if (!codec_up) {
					codec_up = true;
					amp_hold_until = now + 2500;
				}
			}
		}

		LOCK();
		vbus_v = bp.vbus_v;
		vbus_i_a = bp.vbus_i_a;
		pvdd_v = bp.pvdd_v;
		hifi_gov_set_pvdd(&gov, bp.pvdd_v);
		pg = bp.pg;
		fault = bp.fault;
		if (bp.pd_read) {
			sink_ok = bp.sink_ok;
			pd_status = bp.pd_status;
			hifi_contract_from(bp.sink_ok, bp.sink_ok ? bp.pd_status : 0, bp.pd_current, bp.vbus_v, &contract);
			/* until PG, assume no more than a USB 2.0 port */
			hifi_gov_set_supply(&gov, contract.v, pg ? contract.w : 2.5f);
		}
		/* an amp that warns it is hot: -3 dB every 2 s while it does, back up
		 * 1 dB every 10 s once it has been clear for 10 s (requirement C2).
		 * OTW_CLIP must be low for 200 ms: it also pulses ~500 ns on every
		 * clipped PWM cycle, which is not heat (R5-DAL-5) */
		if (!bp.otw) otw_high_ms = now;
		bool otw_held = bp.otw && now - otw_high_ms >= 200;
		amp_hot = otw_held;
		if (otw_held) {
			otw_clear_ms = now;
			if (now - otw_ms >= 2000) {
				otw_ms = now;
				gov.trim = fmaxf(gov.trim * 0.708f, 0.25f);
				ESP_LOGW(TAG, "amp too hot: gain ceiling %.1f dB", 20 * log10f(gov.trim));
			}
		} else if (gov.trim < 1.0f && now - otw_clear_ms >= 10000 && now - otw_ms >= 10000 &&
			   (int32_t) (trim_hold_until - now) <= 0) {
			otw_ms = now;
			gov.trim = fminf(gov.trim * 1.122f, 1.0f);
		}
		/* DJ direct is analog, out of the governor's reach: when the amps
		 * get too hot for it, the governed line in takes over (R3-SYSTEM-4) */
		const char *dj_trip = NULL;
		if (board_line && input == HIFI_INPUT_DJ_DIRECT && power.out.amp_on) {
			float meas = isnan(bp.vbus_i_a) ? NAN : bp.vbus_i_a * bp.vbus_v;
			dj_trip = hifi_djg_step(&djg, now, 0.01f, meas, bp.otw, hifi_djg_limit_w(&gov.cfg, gov.amps),
						pg ? contract.w : 0);
			if (dj_trip && !strcmp(dj_trip, "contract")) {
				ESP_LOGW(TAG, "DJ direct: %.1f W in, over the charger's %.1f W: the line in goes through "
					      "the governor now", meas, contract.w);
				input = HIFI_INPUT_LINE;
			} else if (dj_trip) {
				ESP_LOGW(TAG, "DJ direct: %s (%.1f W of %.1f W): the line in goes through the governor now",
					 !strcmp(dj_trip, "otw") ? "the amps warn they are hot" : "over the amps' thermal budget",
					 hifi_djg_in_w(&djg), hifi_djg_limit_w(&gov.cfg, gov.amps));
				input = HIFI_INPUT_LINE;
			}
		}
		hifi_power_in_t in = {
			.now_ms = now, .vbus_v = bp.vbus_v, .pvdd_v = bp.pvdd_v, .pg = bp.pg, .fault = bp.fault,
			.audio = audio_active || tone_active || input == HIFI_INPUT_DJ_DIRECT ||
				 (int32_t) (selftest_hold_until - now) > 0,
		};
		/* the volume goes to flash once it has settled for 3 s */
		bool save_vol = vol_dirty && now - vol_changed_ms > 3000;
		int save_v = vol.volume, save_max = vol.max_volume;
		if (save_vol) vol_dirty = false;
		hifi_pwr_state_t before = power.state;
		int faults_before = power.faults;
		hifi_power_out_t o = *hifi_power_step(&power, &in);
		/* Lite's TPA3118 has no warning pin: heat shows only as its 150 C
		 * shutdown on FAULTZ, which also flags a short or DC. A trip is
		 * taken as heat only when the heat cap is over the default, the
		 * amps have played for 5 s (a short trips at once), the sustained
		 * power is over the default, and no trip was forgiven in the last
		 * fault window. Then the cap drops 2 W (saved), the gain 3 dB (held
		 * 60 s), the amp rests 15 s, and the trip doesn't count towards the
		 * latch. Every other trip latches as before. */
		float cap_w = hifi_gov_heat_w(&gov), def_w = hifi_gov_heat_def_w(&gov);
		float heat_cap_saved = NAN;
		if (power.faults > faults_before && def_w > 0 && cap_w > def_w + 0.25f &&
		    now - amp_off_ms >= 5000 && hifi_gov_heat_now_w(&gov) > def_w &&
		    (!heat_trip_any || now - heat_trip_ms > power.cfg.fault_window_ms)) {
			float w = hifi_gov_set_heat_w(&gov, fmaxf(cap_w - 2.0f, def_w));
			hifi_power_forgive_fault(&power, now);
			gov.trim = fmaxf(gov.trim * 0.708f, 0.25f);
			otw_ms = otw_clear_ms = now;
			trim_hold_until = now + 60000;
			amp_hold_until = now + 15000;
			heat_trip_ms = now;
			heat_trip_any = true;
			heat_cap_saved = w;
			ESP_LOGW(TAG, "amp tripped with the heat cap at %.1f W: taken as heat, cap now %.1f W, gain "
				      "ceiling %.1f dB, amp off 15 s", cap_w, w, 20 * log10f(gov.trim));
		}
		if (power.state != before)
			ESP_LOGI(TAG, "power %s -> %s (VBUS %.2f V, PVDD %.2f V, PG %d)", hifi_power_state_name(before),
				 hifi_power_state_name(power.state), bp.vbus_v, bp.pvdd_v, bp.pg);
		UNLOCK();
		/* until it is chosen again; a charger too small for DJ direct is
		 * remembered, so a restart after its OCP comes back governed */
		if (dj_trip) input_changed(HIFI_INPUT_LINE, !strcmp(dj_trip, "contract"));
		bool quiet = quiet_req, amp_off = amp_off_req;
		if (!hifi_quiet_amp_allowed(codec_up, amp_hold_until, amp_off || in_recovery, now)) o.amp_on = false;
		if (!o.amp_on) amp_off_ms = now;
		hifi_bsp_power_out(&o);
		if (quiet) quiet_ack = true;
		if (amp_off) amp_ack = true;
		if (!isnan(heat_cap_saved)) {
			char hv[12];
			snprintf(hv, sizeof(hv), "%.1f", heat_cap_saved);
			config_set_value(NVS_TYPE_STR, "hifi_heat_w", hv);
		}
		if (save_vol) {
			char sv[8];
			snprintf(sv, sizeof(sv), "%d", save_v);
			config_set_value(NVS_TYPE_STR, "hifi_volume", sv);
			snprintf(sv, sizeof(sv), "%d", save_max);
			config_set_value(NVS_TYPE_STR, "hifi_max_vol", sv);
		}
	}
}

bool hifi_main_amp_latched(void) { return power.latched; }

bool hifi_main_audio_active(void) { return audio_active || tone_active; }

void hifi_main_gov_amps(int n) { gov.amps = n; }

/* ------------------------------------------------------------------ quiet stop */

/* every board ramps squeezelite's digital gain (the core board's only
 * ramp: its Sabre's volume is squeezelite's); the BSP ramps the codec too */
static void q_level(void *c, float f) {
	/* a test tone and DJ direct's line copy play at unity, so they ramp from there;
	 * a lock held too long skips this step rather than hold up the restart;
	 * a restart before hifi_init made the lock (factory_boot) has nothing to ramp */
	if (hifi_output_gain && lock && xSemaphoreTake(lock, pdMS_TO_TICKS(20)) == pdTRUE) {
		uint32_t base = tone_active || line_unity ? HIFI_VOL_UNITY : hifi_vol_gain(&vol);
		UNLOCK();
		uint32_t g = (uint32_t) (base * (f < 0 ? 0 : f > 1 ? 1 : f));
		hifi_output_gain(g, g);
	}
	hifi_bsp_quiet_level(f);
}
static void q_mute(void *c) { hifi_bsp_quiet_mute(true); }
static void q_amp_off(void *c) {
	amp_ack = false;
	amp_off_req = true; /* only now, after the ramp and the mute */
	hifi_bsp_amp_off_now();
	/* the power task's step with amp_off_req set puts every board's amps
	 * into shutdown; from the power task itself, the caller's own step does */
	if (power_task_h && xTaskGetCurrentTaskHandle() != power_task_h)
		for (int i = 0; i < 10 && !amp_ack; i++) vTaskDelay(pdMS_TO_TICKS(10));
	hifi_bsp_amp_off_now(); /* once more, in case the power task's step raced the first */
}
static void q_sleep(void *c, uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1); }
static const hifi_quiet_ops_t quiet_ops = {.level = q_level, .mute = q_mute, .amp_off = q_amp_off, .sleep_ms = q_sleep};

static volatile bool quiet_busy, quiet_done, quiet_final;
static portMUX_TYPE quiet_mux = portMUX_INITIALIZER_UNLOCKED;
static void apply_volume(void);

void hifi_quiet_stop(const char *why, bool restart) {
	if (xPortInIsrContext() || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
		if (restart) quiet_final = true;
		hifi_bsp_amp_off_now();
		return;
	}
	if (restart) { /* a restart's stop is never released */
		taskENTER_CRITICAL(&quiet_mux);
		quiet_final = true;
		taskEXIT_CRITICAL(&quiet_mux);
	}
	while (__atomic_exchange_n(&quiet_busy, true, __ATOMIC_SEQ_CST)) {
		/* another path is ramping: restart only once it has finished; if a
		 * release freed it meanwhile, run the whole stop here */
		int i = 0;
		while (i++ < 50 && !quiet_done && quiet_busy) vTaskDelay(pdMS_TO_TICKS(10));
		if (quiet_done) return;
		if (quiet_busy) {
			hifi_bsp_amp_off_now();
			return;
		}
	}
	ESP_LOGI(TAG, "quiet stop: %s", why ? why : "?");
	quiet_ack = false;
	quiet_req = true; /* the routing stops here: nothing raises the outputs again */
	/* a routing write the power task has begun lands before the ramp reads the levels */
	if (power_task_h && xTaskGetCurrentTaskHandle() != power_task_h)
		for (int i = 0; i < 5 && !quiet_ack; i++) vTaskDelay(pdMS_TO_TICKS(10));
	hifi_quiet_run(&quiet_ops);
	quiet_done = true;
}

void hifi_quiet_release(void) {
	/* checked and taken together, so a restart's stop that comes in now
	 * either finds the stop still done or runs its own afterwards */
	taskENTER_CRITICAL(&quiet_mux);
	bool ok = quiet_done && !quiet_final;
	if (ok) quiet_done = false;
	taskEXIT_CRITICAL(&quiet_mux);
	if (!ok) return;
	ESP_LOGI(TAG, "quiet stop released: the update didn't go ahead");
	amp_hold_until = now_ms() + 300; /* the routing goes back on with the amps off */
	hifi_bsp_quiet_release();
	hifi_bsp_quiet_mute(false);
	codec_dirty = true;
	quiet_req = false;
	amp_off_req = false;
	__atomic_store_n(&quiet_busy, false, __ATOMIC_SEQ_CST);
	/* squeezelite's gain back where it plays: unity for a test tone or DJ direct */
	if (hifi_output_gain && (tone_active || line_unity)) hifi_output_gain(HIFI_VOL_UNITY, HIFI_VOL_UNITY);
	else apply_volume();
}

/* A phone source hands the output to another (decode_external.c's
 * phone_takeover, patch 0034): squeezelite's gain fades to 0 before the old
 * source is paused and the buffer emptied, and comes back once the buffer is
 * empty, a moment before the new source claims the output: nothing plays in
 * between, so the switch has no pop. Blocks the caller's task for the 200 ms
 * of the ramp. Only the digital gain:
 * the amps and the codec stay on, the next source plays at once */
static volatile bool handover;

void hifi_handover_quiet(bool down) {
	if (!hifi_output_gain) return;
	if (!down) {
		handover = false;
		if (!quiet_req) apply_volume();
		return;
	}
	bool ramp = !handover && !quiet_busy && lock;
	handover = true;
	if (!ramp) return;
	LOCK();
	uint32_t base = tone_active || line_unity ? HIFI_VOL_UNITY : hifi_vol_gain(&vol);
	UNLOCK();
	for (int i = HIFI_QUIET_STEPS - 1; i >= 0; i--) {
		uint32_t g = (uint32_t) ((uint64_t) base * i / HIFI_QUIET_STEPS);
		hifi_output_gain(g, g);
		vTaskDelay(pdMS_TO_TICKS(HIFI_QUIET_RAMP_MS / HIFI_QUIET_STEPS) ?: 1);
	}
	ESP_LOGI(TAG, "source handover: faded out");
}

/* ------------------------------------------------------------------ volume */

/* Puts the volume where it plays: squeezelite's gain for streams, the codec
 * for DJ direct. Called with the lock not held (squeezelite takes its own). */
static void apply_volume(void) {
	LOCK();
	uint32_t g = hifi_vol_gain(&vol);
	bool direct = input == HIFI_INPUT_DJ_DIRECT;
	vol_dirty = true;
	vol_changed_ms = now_ms();
	UNLOCK();
	if (direct) codec_dirty = true;
	if (hifi_party_volume) hifi_party_volume(g);
	/* a stop holds squeezelite's gain at 0 until it is released, a handover until it ends */
	if (hifi_output_gain && !tone_active && !line_unity && !quiet_req && !handover) hifi_output_gain(g, g);
}

/* A source set its volume (set_volume in squeezelite). */
bool hifi_volume_hook(unsigned *left, unsigned *right) {
	LOCK();
	/* sources send one gain for both sides; keep the louder as the volume */
	uint32_t g = hifi_vol_source(&vol, *left > *right ? *left : *right);
	vol_from_source = true;
	vol_dirty = true;
	vol_changed_ms = now_ms();
	UNLOCK();
	*left = *right = quiet_req || handover ? 0 : g; /* a stop (or a handover) holds the stream silent */
	return !tone_active && !line_unity; /* a test signal (and DJ direct's line copy) plays at unity;
					      * the volume comes back after */
}

/* volume up/down here rather than at the source: nothing streams, the line
 * in (which plays at this volume), or DJ direct */
static bool volume_is_local(void) {
	LOCK();
	bool here = input == HIFI_INPUT_DJ_DIRECT || input == HIFI_INPUT_LINE;
	UNLOCK();
	int s = hifi_sink_source ? hifi_sink_source() : 0;
	return here || s == 0 || s == 6 || s == 7; /* 7: party plays here at this volume */
}

/* A new input: the amps go to shutdown, the routing moves 30 ms later and
 * they come back 200 ms after the change; the choice survives a reboot
 * (not the guard's move off DJ direct, so a reboot brings back what was
 * chosen) */
static void input_changed(hifi_input_t in, bool save) {
	uint32_t now = now_ms();
	amp_hold_until = now + 200;
	route_at = now + 30;
	codec_dirty = true;
	if (in == HIFI_INPUT_DJ_DIRECT) hifi_djg_reset(&djg);
	if (save) config_set_value(NVS_TYPE_STR, "hifi_input", in == HIFI_INPUT_DJ_DIRECT ? "dj" : hifi_input_name(in));
}

/* The Input button: streaming, then (where the board has it) the governed
 * line in, then DJ direct */
static void toggle_input(void) {
	LOCK();
	if (input == HIFI_INPUT_STREAM) input = board_line ? HIFI_INPUT_LINE : HIFI_INPUT_DJ_DIRECT;
	else if (input == HIFI_INPUT_LINE) input = HIFI_INPUT_DJ_DIRECT;
	else input = HIFI_INPUT_STREAM;
	hifi_input_t in = input;
	UNLOCK();
	input_changed(in, true);
	apply_volume();
}

/* A remote or button action that is the receiver's own; false: the source's. */
static bool local_action(int a, bool pressed) {
	if (a == HIFI_IR_MUTE || a == HIFI_IR_INPUT) {
		if (!pressed) return true;
		if (a == HIFI_IR_INPUT) {
			toggle_input();
		} else {
			LOCK();
			hifi_vol_set(&vol, -1, !vol.mute, -1);
			UNLOCK();
			apply_volume();
		}
		return true;
	}
	if ((a == HIFI_IR_VOLUP || a == HIFI_IR_VOLDOWN) && volume_is_local()) {
		if (pressed) {
			LOCK();
			hifi_vol_step(&vol, a == HIFI_IR_VOLUP ? 1 : -1);
			UNLOCK();
			apply_volume();
		}
		return true;
	}
	return false;
}

static const actrls_action_e IR_TO_ACTRLS[HIFI_IR_N] = {
	[HIFI_IR_VOLUP] = ACTRLS_VOLUP, [HIFI_IR_VOLDOWN] = ACTRLS_VOLDOWN, [HIFI_IR_MUTE] = ACTRLS_NONE,
	[HIFI_IR_TOGGLE] = ACTRLS_TOGGLE, [HIFI_IR_NEXT] = ACTRLS_NEXT, [HIFI_IR_PREV] = ACTRLS_PREV,
	[HIFI_IR_INPUT] = ACTRLS_NONE, [HIFI_IR_POWER] = ACTRLS_POWER,
};

/* An action from the remote or the API: ours, else to whatever is playing. */
static void do_action(int a) {
	if (local_action(a, true)) return;
	actrls_handler h = IR_TO_ACTRLS[a] != ACTRLS_NONE ? get_ctrl_handler(IR_TO_ACTRLS[a]) : NULL;
	if (h) h(true); /* like a button press (LMS's handlers act on the release too) */
}

/* the last control for state's controls.last; under lock */
static void control_seen(const char *what, const char *name) {
	snprintf(last_ctl, sizeof(last_ctl), "%s %s", what, name);
	last_ctl_ms = now_ms();
}

static void control_seen_locked(const char *what, const char *name) {
	LOCK();
	control_seen(what, name);
	UNLOCK();
}

void hifi_main_action(int a) {
	control_seen_locked("button", hifi_ir_action_name(a));
	do_action(a);
}

/* The core board's knob: the receiver's own volume when nothing streams or
 * in DJ direct, else the source's, a step per click, as the buttons do. */
void hifi_main_knob(int detents) {
	char n[8];
	snprintf(n, sizeof(n), "%+d", detents);
	control_seen_locked("knob", n);
	if (volume_is_local()) {
		LOCK();
		hifi_vol_turn(&vol, detents);
		UNLOCK();
		apply_volume();
		return;
	}
	actrls_handler h = get_ctrl_handler(detents > 0 ? ACTRLS_VOLUP : ACTRLS_VOLDOWN);
	for (int i = 0; h && i < abs(detents); i++) h(true);
}

/*
 * The play button's taps, acted on once they're counted (hifi_tap.h): 1
 * play/pause, 2 next, 3 previous. Its stack is in PSRAM like the buttons
 * task's, so what it calls must not touch flash either.
 */
static void tap_main(void *arg) {
	(void) arg;
	for (;;) {
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		for (;;) {
			LOCK();
			int n = hifi_tap_poll(&taps, now_ms());
			bool pending = hifi_tap_pending(&taps);
			UNLOCK();
			if (n) {
				int a = n == 1 ? HIFI_IR_TOGGLE : n == 2 ? HIFI_IR_NEXT : HIFI_IR_PREV;
				control_seen_locked("button", hifi_ir_action_name(a));
				actrls_handler h = get_ctrl_handler(IR_TO_ACTRLS[a]);
				if (h) h(true);
			}
			if (!pending) break;
			ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
		}
	}
}

/* A button press on the board (squeezelite's audio controls). */
bool hifi_button_hook(int action, bool pressed) {
	/* play: counted as headphone taps (a short press arrives on release) */
	if (action == ACTRLS_TOGGLE && tap_task) {
		if (pressed) {
			LOCK();
			hifi_tap_press(&taps, now_ms());
			UNLOCK();
			xTaskNotifyGive(tap_task);
		}
		return true;
	}
	/* play held: let a new phone pair over Bluetooth */
	if (action == ACTRLS_POWER && bt_sink_pair_mode) {
		static bool pairing; /* the release goes where the press went */
		if (pressed) pairing = bt_sink_pair_mode();
		if (pairing) {
			if (pressed) control_seen_locked("button", "pair");
			return true;
		}
	}
	for (int a = 0; pressed && a < HIFI_IR_N; a++)
		if (IR_TO_ACTRLS[a] == action && action != ACTRLS_NONE) control_seen_locked("button", hifi_ir_action_name(a));
	if (action == ACTRLS_VOLUP) return local_action(HIFI_IR_VOLUP, pressed);
	if (action == ACTRLS_VOLDOWN) return local_action(HIFI_IR_VOLDOWN, pressed);
	return false;
}

/* ------------------------------------------------------------------ remote */

static void ir_save(void) {
	char s[160];
	LOCK();
	hifi_ir_save(&ir, s, sizeof(s));
	UNLOCK();
	config_set_value(NVS_TYPE_STR, "hifi_ir", s);
}

/* Every IR frame squeezelite decodes; true when it was ours. */
bool hifi_ir_hook(uint16_t addr, uint16_t cmd) {
	bool changed;
	uint32_t code = ((uint32_t) addr << 16) | cmd;
	LOCK();
	int a = hifi_ir_code(&ir, code, now_ms(), &changed);
	if (a >= 0) control_seen("ir", hifi_ir_action_name(a));
	UNLOCK();
	if (changed) {
		ESP_LOGI(TAG, "remote: learned %08lx", (unsigned long) code);
		ir_save();
	}
	if (a == HIFI_IR_PASS) return false;
	if (a >= 0) do_action(a);
	return true;
}

/* ------------------------------------------------------------------ now playing */

void hifi_display_control(int cmd) {
	/* display.h: SHUTDOWN, ACTIVATE, SUSPEND, TIMER_PAUSE, TIMER_RUN */
	LOCK();
	switch (cmd) {
	case 1: hifi_meta_active(&meta); break;
	case 3: hifi_meta_pause(&meta, now_ms()); break;
	case 4: hifi_meta_play(&meta, now_ms()); break;
	default: hifi_meta_idle(&meta); break;
	}
	UNLOCK();
}

void hifi_display_metadata(const char *artist, const char *album, const char *title) {
	LOCK();
	hifi_meta_track(&meta, artist, album, title);
	UNLOCK();
}

void hifi_display_timer(int elapsed, int duration) {
	LOCK();
	hifi_meta_timer(&meta, elapsed, duration, now_ms());
	UNLOCK();
}

/* ------------------------------------------------------------------ tones */

static void tone_main(void *arg) {
	static int16_t buf[2 * 256];
	for (;;) {
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		hifi_sink_start(TONE_RATE);
		for (;;) {
			xSemaphoreTake(tone_lock, portMAX_DELAY);
			if (tone_stop_req) {
				hifi_tone_stop(&tone); /* 10 ms fade, then done */
				tone_stop_req = false;
			}
			size_t n = hifi_tone_render(&tone, buf, 256);
			if (!n) tone_active = false; /* under the lock: a new tone now notifies us again */
			xSemaphoreGive(tone_lock);
			if (!n) break;
			/* keep no more than 100 ms queued so stop and level changes are prompt */
			while (hifi_sink_queued_ms() > 100) vTaskDelay(pdMS_TO_TICKS(20));
			hifi_sink_write((const uint8_t *) buf, n * 4);
		}
		for (int i = 0; i < 50 && hifi_sink_queued_ms() > 0; i++) vTaskDelay(pdMS_TO_TICKS(10));
		/* squeezelite's stop puts its saved gain back past our gate: not during a stop */
		while (quiet_req) vTaskDelay(pdMS_TO_TICKS(20));
		hifi_sink_stop();
		apply_volume(); /* anything set while the tone played */
	}
}

/* ------------------------------------------------------------------ the line in */

/*
 * The line in through the ADC, for the governed line in and for DJ direct.
 *
 * Line in: the ADC's samples go into squeezelite's output as a source, as
 * the test tones do, so they get the volume, the EQ, the governor (and its
 * thermal limit) and the subs, as a stream does; behind squeezelite's DMA,
 * about 0.1 s late. A stream or a tone that starts takes the output; the
 * line in comes back when it stops.
 *
 * DJ direct: the mains are analog, so the same samples (inverted, C5) go
 * to the subs straight, a few ms late, and to the guard's estimate. They go
 * on into squeezelite's output too, at unity, which keeps I2S0 and the
 * ADC's clock running; the amps don't hear that copy.
 */
static void line_main(void *arg) {
	static int16_t buf[2 * LINE_BLOCK];
	bool owns = false, owned_dj = false, adc = false, direct = false;
	for (;;) {
		hifi_input_t in = input;
		bool dj = in == HIFI_INPUT_DJ_DIRECT;
		bool want = (in == HIFI_INPUT_LINE || dj) && codec_up;
		if ((want && dj) != direct) {
			direct = want && dj;
			hifi_bsp_sub_direct(direct);
		}
		int src = hifi_sink_source ? hifi_sink_source() : 0;
		if (owns && src != 6) owns = false; /* a tone or a stream took the output */
		if (!want && owns && !quiet_req) { /* a stop holds the gain where it is */
			hifi_sink_stop();
			owns = false;
		}
		if (want && !quiet_req && ((!owns && src == 0 && !tone_active) || (owns && owned_dj != dj))) {
			hifi_line_start(LINE_RATE, dj);
			owns = true;
			owned_dj = dj;
		}
		/* unity only while DJ direct's copy holds the output: whatever plays
		 * otherwise gets the volume */
		if ((owns && dj) != line_unity) {
			line_unity = owns && dj;
			apply_volume();
		}
		if (!want) {
			vTaskDelay(pdMS_TO_TICKS(50));
			continue;
		}
		/* the ADC takes its clock from I2S0's BCLK, which runs once
		 * something plays: the line in's own output, a stream or a tone */
		if (!adc && src != 0) adc = hifi_bsp_line_start();
		if (!adc) {
			vTaskDelay(pdMS_TO_TICKS(500));
			continue;
		}
		int n = hifi_bsp_line_read(buf, LINE_BLOCK, dj, 50);
		if (n <= 0) continue;
		if (dj) {
			hifi_bsp_sub_write(buf, n);
			hifi_djg_audio(&djg, hifi_line_block_w(&gov.cfg, buf, n, gov.amps), (float) n / LINE_RATE);
		}
		/* the ADC and the DAC share I2S0's clock, so the queue holds; after a
		 * hiccup it is cut back rather than playing later from then on */
		if (owns && hifi_sink_queued_ms() <= LINE_QUEUE_MS) hifi_sink_write((const uint8_t *) buf, n * 4);
	}
}

/* ------------------------------------------------------------------ ops */

/*
 * A task whose stack is in PSRAM, to leave internal RAM to Wi-Fi, Bluetooth
 * and DMA (the Lite had 17 KB of it free with everything up). Only for tasks
 * that never touch flash (NVS, OTA): IDF asserts on a flash operation from
 * a PSRAM stack. Its TCB stays internal; IDF counts stacks in bytes. Falls back to an internal stack.
 */
static TaskHandle_t task_psram(TaskFunction_t fn, const char *name, uint32_t bytes, UBaseType_t prio, BaseType_t core) {
	StackType_t *stack = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	StaticTask_t *tcb = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
	TaskHandle_t h = NULL;
	if (stack && tcb) h = xTaskCreateStaticPinnedToCore(fn, name, bytes, NULL, prio, stack, tcb, core);
	if (h) return h;
	free(stack);
	free(tcb);
	xTaskCreatePinnedToCore(fn, name, bytes, NULL, prio, &h, core);
	return h;
}

static void op_info(hifi_info_t *i) {
	uint8_t mac[6];
	i->model = HIFI_MODEL;
	i->board = HIFI_BOARD;
	i->fw = HIFI_FW_VERSION;
	i->upstream = HIFI_UPSTREAM;
	esp_read_mac(mac, ESP_MAC_WIFI_STA);
	snprintf(i->mac, sizeof(i->mac), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	char *s = config_alloc_get_str("host_name", NULL, HIFI_MODEL);
	snprintf(i->hostname, sizeof(i->hostname), "%s", s ? s : HIFI_MODEL);
	free(s);
	s = config_alloc_get_str("hifi_name", NULL, NULL);
	if (!s) s = config_alloc_get_str("airplay_name", NULL, HIFI_MODEL);
	snprintf(i->name, sizeof(i->name), "%s", s ? s : HIFI_MODEL);
	free(s);
	i->flash_mb = spi_flash_get_chip_size() >> 20;
	hifi_bsp_info(i);
#if HIFI_HAS_BOOST
	i->caps.boost_max_db = HIFI_BOOST_MAX_DB;
#endif
	if (hifi_gov_heat_max_w(&gov) > hifi_gov_heat_def_w(&gov)) {
		i->caps.heat_min_w = HIFI_GOV_HEAT_MIN_W;
		i->caps.heat_max_w = hifi_gov_heat_max_w(&gov);
		i->caps.heat_def_w = hifi_gov_heat_def_w(&gov);
	}
}

static const char *source_name(void) {
	static const char *names[] = {"idle", "lms", "bt", "airplay", "spotify", "test", "line", "party"};
	int s = hifi_sink_source ? hifi_sink_source() : 0;
	return s >= 0 && s <= 7 ? names[s] : "idle";
}

static void read_eq(float eq[HIFI_EQ_BANDS]) {
	char *s = config_alloc_get_str("equalizer", NULL, "");
	char *p = s;
	for (int i = 0; i < HIFI_EQ_BANDS; i++) {
		eq[i] = p && *p ? strtof(p, &p) : 0;
		while (p && (*p == ',' || *p == ' ')) p++;
	}
	free(s);
}

static void op_snapshot(hifi_snapshot_t *s) {
	s->uptime_s = now_ms() / 1000;
	LOCK();
	s->pwr_state = power.state;
	s->vbus_v = vbus_v;
	s->pg = pg;
	s->contract = contract;
	s->pvdd_on = power.out.pvdd_en;
	s->amp_enabled = power.out.amp_on;
	s->amp_fault = fault;
	s->amp_latched = power.latched;
	s->amp_faults = power.faults;
	s->input = input;
	s->dj_guard = board_line;
	s->dj_guard_w = hifi_djg_in_w(&djg);
	s->dj_guard_limit_w = hifi_djg_limit_w(&gov.cfg, gov.amps);
	s->dj_guard_tripped = djg.tripped;
	s->sub_on = sub_on;
	s->sub_db = hifi_bsp_sub_db(sub_db);
	s->channel = channel;
	s->vbus_i_a = vbus_i_a;
	s->pvdd_v = pvdd_v;
	s->volume = vol.volume;
	s->mute = vol.mute;
	s->max_volume = vol.max_volume;
	uint32_t t = now_ms();
	s->playing = meta;
	s->playing.elapsed_ms = hifi_meta_elapsed_ms(&meta, t);
	s->ir_learning = hifi_ir_learning(&ir, t);
	for (int a = 0; a < HIFI_IR_N; a++) s->ir_learned[a] = ir.have[a];
	s->ir_have_last = ir.have_last;
	s->ir_last_code = ir.last_code;
	if (last_ctl[0]) {
		snprintf(s->last_control, sizeof(s->last_control), "%s", last_ctl);
		s->last_control_ago_s = (t - last_ctl_ms) / 1000;
	}
	UNLOCK();
	s->gov_budget_w = gov.budget_w;
	s->gov_in_w = hifi_gov_in_w(&gov);
	s->gov_out_w = hifi_gov_out_w(&gov);
	s->gov_gain_db = hifi_gov_gain_db(&gov);
	s->gov_limiting = gov.limiting;
	s->gov_events = gov.events;
	s->loudness = true;
	s->gov_why = hifi_gov_why(&gov);
	s->heat_cap_w = hifi_gov_heat_w(&gov);
	s->heat_w = hifi_gov_heat_now_w(&gov);
	s->amp_hot = amp_hot;
	s->boost_db = boost.db;
	s->boost_limit_db = hifi_boost_reduction_db(&boost);
	s->boost_limiting = boost.limiting;
	s->boost_events = boost.events;
	s->level_dbfs = hifi_gov_level_dbfs(&gov);
	s->source = source_name();
	/* LMS sends its track text only as display graphics: the state, no text */
	if (s->playing.state == HIFI_PLAY_NONE && !strcmp(s->source, "lms")) s->playing.state = HIFI_PLAY_PLAY;
	s->tone_active = tone_active;
	s->tone = tone_params;
	read_eq(s->eq);

	char *h = config_alloc_get_str("host_name", NULL, HIFI_MODEL);
	snprintf(s->hostname, sizeof(s->hostname), "%s", h ? h : HIFI_MODEL);
	free(h);
	hifi_net_ip(s->ip, sizeof(s->ip));
	wifi_ap_record_t ap;
	if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
		snprintf(s->ssid, sizeof(s->ssid), "%s", (const char *) ap.ssid);
		s->rssi = ap.rssi;
	}
	wifi_mode_t mode = WIFI_MODE_NULL;
	esp_wifi_get_mode(&mode);
	s->net_mode = !s->ip[0] && (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) ? "ap" : "sta";
	s->heap_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
	s->heap_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
	s->heap_boot = heap_boot;
	s->reset_reason = hifi_crash_reset_reason();
	s->restart_cause = hifi_crash_restart_cause();
	s->psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
	hifi_net_update_status(&s->update);
	hifi_bsp_snapshot(s); /* its sensors, zones and Ethernet */
}

static int op_tone_start(const hifi_tone_params_t *p, bool force, const char **err) {
	if (!hifi_sink_start || !tone_task) {
		*err = "this build has no audio output";
		return 500;
	}
	if (quiet_req) { /* squeezelite's unity step would get past the stop's ramp */
		*err = "a restart or an update is under way";
		return 409;
	}
	LOCK();
	hifi_pwr_state_t st = power.state;
	bool latched = power.latched;
	UNLOCK();
	if (!force && (st != HIFI_PWR_ON || latched)) {
		static char why[64];
		snprintf(why, sizeof(why), "amp isn't ready (power %s%s)", hifi_power_state_name(st), latched ? ", fault latched" : "");
		*err = why;
		return 409;
	}
	xSemaphoreTake(tone_lock, portMAX_DELAY);
	hifi_tone_start(&tone, p, TONE_RATE);
	tone_params = *p;
	tone_stop_req = false;
	bool was = tone_active;
	tone_active = true;
	xSemaphoreGive(tone_lock);
	if (!was) xTaskNotifyGive(tone_task); /* a running tone just switches signal */
	return 0;
}

static void op_tone_stop(void) {
	if (tone_active) tone_stop_req = true;
}

static void op_amp_enable(bool on) {
	LOCK();
	hifi_power_amp_enable(&power, on);
	UNLOCK();
}

static void op_amp_reset(void) {
	LOCK();
	hifi_power_amp_reset(&power);
	UNLOCK();
}

static int op_set_input(hifi_input_t in) {
	if (in == HIFI_INPUT_LINE && !board_line) return 1;
	LOCK();
	bool changed = input != in;
	input = in;
	UNLOCK();
	if (changed) input_changed(in, true);
	else codec_dirty = true;
	return 0;
}

/* Boost (dB) and the heat cap (W a channel); NaN leaves one. Both take
 * effect at once; nonzero when one couldn't be saved for the next start. */
static int op_set_loudness(float boost_db, float heat_w) {
	char v[16];
	int err = 0;
	if (!isnan(boost_db)) {
		hifi_boost_set_db(&boost, boost_db);
		snprintf(v, sizeof(v), "%.1f", boost.db);
		err |= config_set_value(NVS_TYPE_STR, "hifi_boost_db", v) != ESP_OK;
	}
	if (!isnan(heat_w)) {
		LOCK();
		float w = hifi_gov_set_heat_w(&gov, heat_w);
		UNLOCK();
		snprintf(v, sizeof(v), "%.1f", w);
		err |= config_set_value(NVS_TYPE_STR, "hifi_heat_w", v) != ESP_OK;
	}
	if (err) ESP_LOGW(TAG, "loudness: set, but not saved to NVS");
	return err;
}

static float op_set_sub(int on, float db) {
	char v[16];
	LOCK();
	if (on >= 0) sub_on = on;
	if (!isnan(db)) sub_db = db;
	float set = hifi_bsp_sub_db(sub_db);
	bool o = sub_on;
	UNLOCK();
	snprintf(v, sizeof(v), "%.1f", set);
	config_set_value(NVS_TYPE_STR, "hifi_sub_db", v);
	config_set_value(NVS_TYPE_STR, "hifi_sub_on", o ? "1" : "0");
	codec_dirty = true;
	return set;
}

#if HIFI_HAS_CHANNEL
static int op_set_channel(hifi_chan_t c) {
	LOCK();
	channel = c;
	gov.chan = c;
	if (!loop_flat) hifi_bsp_channel(c); /* else the loopback check does when it ends */
	UNLOCK();
	config_set_value(NVS_TYPE_STR, "hifi_channel", hifi_chan_name(c));
	codec_dirty = true;
	return 0;
}
#endif

static int op_set_eq(const float g[HIFI_EQ_BANDS]) {
	int8_t q[HIFI_EQ_BANDS];
	for (int i = 0; i < HIFI_EQ_BANDS; i++) q[i] = (int8_t) lrintf(g[i]);
	if (equalizer_set_gain) {
		equalizer_set_gain(q); /* applies it live and saves it */
		return 0;
	}
	char s[HIFI_EQ_BANDS * 4 + 1];
	int n = 0;
	for (int i = 0; i < HIFI_EQ_BANDS; i++) n += sprintf(s + n, "%d,", q[i]);
	s[n - 1] = '\0';
	return config_set_value(NVS_TYPE_STR, "equalizer", s) == ESP_OK ? 0 : -1;
}

static int discard_write(void *cookie, const char *buf, int n) {
	(void) cookie; (void) buf;
	return n;
}

/* The name people see in AirPlay, Bluetooth, Spotify and LMS. The hostname
 * stays the model's (hifi-lite) so hifi-lite.local keeps working. */
static int op_set_name(const char *name) {
	int err = 0;
	err |= config_set_value(NVS_TYPE_STR, "hifi_name", name) != ESP_OK;
	err |= config_set_value(NVS_TYPE_STR, "airplay_name", name) != ESP_OK;
	err |= config_set_value(NVS_TYPE_STR, "bt_name", name) != ESP_OK;
	err |= config_set_value(NVS_TYPE_STR, "a2dp_dev_name", name) != ESP_OK;
	/* upstream's setters fprintf their chatter to f unchecked, and IDF 4.4
	 * has no /dev/null: fopen gave NULL and the board crashed on a rename */
	FILE *f = funopen(NULL, NULL, discard_write, NULL, NULL);
	if (!f) return -1;
	if (set_squeezelite_player_name) err |= set_squeezelite_player_name(f, name) != 0;
	if (set_cspot_player_name) err |= set_cspot_player_name(f, name) != 0;
	fclose(f);
	return err ? -1 : 0; /* the services pick it up at the next restart */
}

static void restart_cb(void *arg) {
	if (arg) nvs_flash_erase();
	hifi_restart_note(arg ? "api_factory_reset" : "api_reboot");
	esp_restart();
}

static void restart_later(bool erase) {
	esp_timer_handle_t t;
	esp_timer_create_args_t a = {.callback = restart_cb, .arg = erase ? (void *) 1 : NULL, .name = "hifi_restart"};
	if (esp_timer_create(&a, &t) == ESP_OK) esp_timer_start_once(t, 700 * 1000); /* after the reply */
}

static void op_reboot(void) { restart_later(false); }
static void op_factory_reset(void) { restart_later(true); }

/* L6 (hifi_reset_confirmed): the LAN's reset arms, a power-on start finishes
 * it. The flag is a hifi_* key, which squeezelite's settings page can't set. */
static void factory_disarm(TimerHandle_t t) {
	config_set_value(NVS_TYPE_STR, "hifi_reset_armed", "0");
	ESP_LOGW(TAG, "factory reset disarmed: not unplugged in time");
}

/* A FreeRTOS timer, not esp_timer: its task (3328 bytes) is the one that
 * already runs platform_config's own commits. The flag is committed before
 * the reply, so an unplug right after "armed" still finds it. */
static int op_factory_arm(void) {
	static TimerHandle_t t; /* the HTTP task and the relay's hifi_cmd task both arm */
	TimerHandle_t mine = NULL, none = NULL;
	if (!__atomic_load_n(&t, __ATOMIC_ACQUIRE)) {
		mine = xTimerCreate("hifi_disarm", pdMS_TO_TICKS(HIFI_RESET_ARM_US / 1000), pdFALSE, NULL, factory_disarm);
		if (!mine) return -1;
		if (!__atomic_compare_exchange_n(&t, &none, mine, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
			xTimerDelete(mine, 0); /* the other task won; use its timer */
	}
	/* the disarm window runs before the flag is set, so the flag never
	 * outlives a timer that failed to start */
	if (xTimerReset(t, pdMS_TO_TICKS(1000)) != pdPASS) return -1; /* a second arm starts the window again */
	bool ok = config_set_value(NVS_TYPE_STR, "hifi_reset_armed", "1") == ESP_OK;
	if (ok) {
		config_commit_to_nvs();
		/* read it back from flash: the commit clears its change flag even
		 * when a store fails, so only this shows an unplug would finish it */
		char *v = get_nvs_value_alloc(NVS_TYPE_STR, "hifi_reset_armed");
		ok = v && !strcmp(v, "1");
		free(v);
	}
	if (!ok) { /* refused: leave nothing armed behind the 403 */
		xTimerStop(t, pdMS_TO_TICKS(1000));
		config_set_value(NVS_TYPE_STR, "hifi_reset_armed", "0");
		config_commit_to_nvs();
		return -1;
	}
	ESP_LOGW(TAG, "factory reset armed from the LAN: unplug the receiver and plug it back in to finish");
	return 0;
}

/* at boot: an armed reset erases on a power-on start, any other disarms */
static void factory_boot(void) {
	char *f = config_alloc_get_str("hifi_reset_armed", NULL, "0");
	bool armed = f && !strcmp(f, "1");
	free(f);
	if (!armed) return;
	if (hifi_reset_confirmed(armed, esp_reset_reason() == ESP_RST_POWERON)) {
		ESP_LOGW(TAG, "factory reset: unplugged while armed, erasing every setting");
		nvs_flash_erase();
		hifi_restart_note("factory_reset");
		esp_restart();
	}
	config_set_value(NVS_TYPE_STR, "hifi_reset_armed", "0");
	ESP_LOGW(TAG, "factory reset disarmed: this start wasn't from power-on");
}

/* The Bluetooth test (ops.bt_test): this boot's squeezelite command line
 * (autoexec1) is kept in hifi_bt_save, the next boot runs it with "-o BT"
 * (a Bluetooth source playing to sink, a2dp_sink_name), and hifi_bttest walks
 * armed -> used -> off, so the boot after the test, crash or not, puts the
 * saved line back (bt_test_boot). */
static int op_bt_test(const char *sink, const char **err) {
	char *flag = config_alloc_get_str("hifi_bttest", NULL, "off");
	bool busy = flag && strcmp(flag, "off");
	free(flag);
	if (busy) {
		*err = "a Bluetooth test is already set up; reboot to end it";
		return -1;
	}
	char *ax = config_alloc_get_str("autoexec1", NULL, NULL);
	if (!ax || !strstr(ax, "squeezelite")) {
		free(ax);
		*err = "no squeezelite command line to change";
		return -1;
	}
	size_t n = strlen(ax) + 16;
	char *line = malloc(n);
	if (!line) {
		free(ax);
		*err = "out of memory";
		return -1;
	}
	/* swap "-o <device>" for "-o BT", or add it */
	const char *o = strstr(ax, " -o ");
	if (o) {
		const char *rest = o + 4;
		while (*rest == ' ') rest++;
		while (*rest && *rest != ' ') rest++;
		snprintf(line, n, "%.*s -o BT%s", (int) (o - ax), ax, rest);
	} else {
		snprintf(line, n, "%s -o BT", ax);
	}
	bool bad = config_set_value(NVS_TYPE_STR, "hifi_bt_save", ax) != ESP_OK;
	bad = bad || config_set_value(NVS_TYPE_STR, "a2dp_sink_name", sink) != ESP_OK;
	bad = bad || config_set_value(NVS_TYPE_STR, "hifi_bttest", "armed") != ESP_OK;
	bad = bad || config_set_value(NVS_TYPE_STR, "autoexec1", line) != ESP_OK;
	ESP_LOGW(TAG, "Bluetooth test: next boot runs '%s'", line);
	free(ax);
	free(line);
	if (bad) {
		config_set_value(NVS_TYPE_STR, "hifi_bttest", "used"); /* the next boot puts it back */
		*err = "couldn't save the settings";
		return -1;
	}
	restart_later(false);
	return 0;
}

/* at boot, before squeezelite reads autoexec1 */
static void bt_test_boot(void) {
	char *flag = config_alloc_get_str("hifi_bttest", NULL, "off");
	if (flag && !strcmp(flag, "armed")) {
		config_set_value(NVS_TYPE_STR, "hifi_bttest", "used");
		ESP_LOGW(TAG, "Bluetooth test boot: this boot plays to another board over Bluetooth");
	} else if (flag && !strcmp(flag, "used")) {
		char *ax = config_alloc_get_str("hifi_bt_save", NULL, NULL);
		if (ax && *ax && hifi_autoexec_ok(ax)) config_set_value(NVS_TYPE_STR, "autoexec1", ax);
		else if (ax && *ax) ESP_LOGE(TAG, "the saved command line isn't squeezelite's: not put back");
		free(ax);
		config_set_value(NVS_TYPE_STR, "hifi_bttest", "off");
		ESP_LOGW(TAG, "Bluetooth test over: the usual command line is back");
	}
	free(flag);
}

static int op_control(const char *a) {
	if (!strcmp(a, "mute") || !strcmp(a, "input") || !strcmp(a, "volup") || !strcmp(a, "voldown")) {
		do_action(hifi_ir_action_from(a)); /* the receiver's own when nothing streams */
		return 0;
	}
	static const struct { const char *name; actrls_action_e action; } map[] = {
		{"volup", ACTRLS_VOLUP}, {"voldown", ACTRLS_VOLDOWN}, {"toggle", ACTRLS_TOGGLE},
		{"play", ACTRLS_PLAY}, {"pause", ACTRLS_PAUSE}, {"stop", ACTRLS_STOP},
		{"next", ACTRLS_NEXT}, {"prev", ACTRLS_PREV}, {"power", ACTRLS_POWER},
	};
	for (int i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
		if (strcmp(a, map[i].name)) continue;
		actrls_handler h = get_ctrl_handler(map[i].action);
		if (h) h(true); /* like a button press, to whatever is playing */
		return 0;
	}
	return -1;
}

#if HIFI_HAS_CAPTURE
static void selftest_loopback(hifi_selftest_t *t, bool amp_on);
#endif

static void op_selftest(hifi_selftest_t *t) {
	uint8_t v;
	bool pd = hifi_i2c_read(CH224_I2C_ADDR, CH224_REG_STATUS, &v) == ESP_OK;
	hifi_selftest_add(t, "pd_sink_i2c", pd, pd ? "0x%02x acks" : "0x%02x doesn't answer", CH224_I2C_ADDR);

	hifi_snapshot_t *s = calloc(1, sizeof(*s));
	if (!s) return;
	op_snapshot(s);
	bool amp_ok_supply = s->pg && s->contract.v >= 9;
	if (s->contract.known)
		hifi_selftest_add(t, "pd_contract", amp_ok_supply, "%s %.1f V %.2f A%s", hifi_proto_name(s->contract.proto),
				  s->contract.v, isnan(s->contract.i_max_a) ? 0 : s->contract.i_max_a,
				  amp_ok_supply ? "" : ": too low for the amp");
	else
		hifi_selftest_add(t, "pd_contract", false, "unknown (PD sink silent)");
	hifi_bsp_selftest(t); /* the board's own parts */

	/* the amp: wake it for a moment and watch FAULTZ */
	selftest_hold_until = now_ms() + 1500;
	for (int i = 0; i < 100 && amp_ok_supply; i++) {
		LOCK();
		bool on = power.state == HIFI_PWR_ON;
		UNLOCK();
		if (on) break;
		vTaskDelay(pdMS_TO_TICKS(10));
	}
	vTaskDelay(pdMS_TO_TICKS(200));
	op_snapshot(s);
	hifi_selftest_add(t, "pvdd_on", s->pvdd_on && s->pwr_state == HIFI_PWR_ON, "%s", hifi_power_state_name(s->pwr_state));
	if (!isnan(s->pvdd_v)) { /* boards that sense PVDD after the switch (Lite) */
		bool pu = s->pvdd_on && s->pvdd_v >= power.cfg.pvdd_min_frac * s->vbus_v;
		hifi_selftest_add(t, "pvdd_sensed", pu, "PVDD %.2f V of VBUS %.2f V%s", s->pvdd_v, s->vbus_v,
				  s->pvdd_on ? "" : " (switch not commanded on)");
	}
	if (s->amp_enabled)
		hifi_selftest_add(t, "amp_no_fault", !s->amp_fault, s->amp_fault ? "FAULTZ low" : "FAULTZ high after 200 ms enabled");
	else
		hifi_selftest_add(t, "amp_no_fault", false, s->amp_latched ? "fault latched" : "amp not enabled (power %s)",
				  hifi_power_state_name(s->pwr_state));
#if HIFI_HAS_CAPTURE
	selftest_loopback(t, s->amp_enabled && !s->amp_fault);
#endif

	size_t ps = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
	hifi_selftest_add(t, "psram", ps >= 2u << 20, "%u KB mapped", (unsigned) (ps >> 10));
	char heap[64];
	bool heap_ok = hifi_heap_line(s->heap_free, s->heap_boot, heap, sizeof(heap));
	hifi_selftest_add(t, "heap", heap_ok, "%s", heap);
	char why[64];
	hifi_selftest_add(t, "network", s->ip[0] != 0, "%s", s->ip[0] ? s->ip : "not connected");
	/* the remote link's last drop, if any, as a line of its own (it passes:
	 * a drop isn't a fault; the whole of it is in `api diag/net`) */
	hifi_ws_stats_t ws;
	hifi_ws_stats(&ws);
	if (ws.last_drop) {
		hifi_ws_drop_line(why, sizeof(why), (uint32_t) ((esp_timer_get_time() - ws.last_drop_us) / 1000000),
				  (hifi_ws_drop_t) ws.last_drop, ws.last_drop_code, ws.last_drop_wifi_down);
		hifi_selftest_add(t, "link_drop", true, "%s", why);
	}
	int strays = hifi_listen_now(why, sizeof(why));
	hifi_selftest_add(t, "listeners", strays == 0, "%s", why);
	if (hifi_party_selftest) hifi_party_selftest(t);
#ifdef CONFIG_HIFI_LAB
	hifi_lab_esp_selftest(t);
#endif
	free(s);
}

static int op_set_volume(int v, int mute, int max) {
	LOCK();
	bool ok = hifi_vol_set(&vol, v, mute, max);
	UNLOCK();
	if (ok) apply_volume();
	return ok ? 0 : -1;
}

static void op_ir_learn(int a) {
	LOCK();
	hifi_ir_learn(&ir, a, now_ms());
	UNLOCK();
}

static void op_ir_forget(int a) {
	LOCK();
	hifi_ir_forget(&ir, a);
	UNLOCK();
	ir_save();
}

/* ------------------------------------------------------------------ remote bring-up */

#if HIFI_HAS_CAPTURE
static bool has_taps(void) {
	char *s = config_alloc_get_str("hifi_taps", NULL, HIFI_HAS_TAPS ? "1" : "0");
	bool on = s && !strcmp(s, "1");
	free(s);
	return on;
}

#define CAP_LEADIN_MS 100 /* fresh input op_capture drops ahead of what it keeps */

/*
 * Records the codec's ADC: the loopback taps (LIN2/RIN2) or the RCA line in
 * (LIN1/RIN1). The ADC shares I2S with the DAC (squeezelite opens it full
 * duplex, firmware/patches), so a capture needs the output running: a tone
 * or a stream. The analog DJ-direct path doesn't pass the ADC's input
 * switch, so it keeps playing.
 */
static int op_capture(hifi_cap_src_t src, int16_t *pcm, int frames, hifi_cap_info_t *info, const char **err) {
	if (src == HIFI_CAP_LOOPBACK && !has_taps()) return *err = "this board revision has no loopback taps", 409;
	if (!tone_active && !audio_active && input != HIFI_INPUT_DJ_DIRECT)
		return *err = "output idle: play a tone or a stream first", 409;
	if (!codec_up) return *err = "codec not started yet", 409; /* squeezelite's init still writes CHIPPOWER */
	if (capture_busy) return *err = "a capture is running", 409;
	capture_busy = true;
	/* squeezelite's init leaves the ADC powered down in CHIPPOWER (0xAA), so
	 * without this the ADC sends zeros; the DAC's bits are kept as they are */
	uint8_t chip = ES8388_CHIPPOWER_ADC_DOWN;
	/* Vref and DLL first, with the ADC's digital part and state machine held
	 * in reset; then its inputs and modulators (ADCPOWER), on a settled
	 * reference; then the digital part let go. The ADC used to come up with
	 * ADCPOWER (still at its reset value on the first capture after a boot)
	 * ahead of everything in CHIPPOWER, released in one write, and sometimes
	 * started scrambled: on Lite 3 the first capture after a boot read
	 * broadband noise on both taps for its whole 100 ms (the 1 kHz bin at
	 * -5 dB over 100 ms and about +5 dB in each 10 ms block, the quiet leg as
	 * loud as the driven one), and the next capture, after the ADC went down
	 * and up again, was clean (2026-11-18) */
	esp_err_t e = hifi_i2c_read(ES8388_ADDR, ES8388_CHIPPOWER, &chip);
	e |= hifi_i2c_write(ES8388_ADDR, ES8388_CHIPPOWER, (chip & 0x55) | ES8388_CHIPPOWER_ADC_RESET);
	vTaskDelay(pdMS_TO_TICKS(50));
	e |= hifi_i2c_write(ES8388_ADDR, ES8388_ADCCONTROL1, 0x00);
	e |= hifi_i2c_write(ES8388_ADDR, ES8388_ADCCONTROL2, src == HIFI_CAP_LOOPBACK ? 0x50 : 0x00);
	e |= hifi_i2c_write(ES8388_ADDR, ES8388_ADCCONTROL4, 0x0C);
	e |= hifi_i2c_write(ES8388_ADDR, ES8388_ADCCONTROL5, 0x02);
	e |= hifi_i2c_write(ES8388_ADDR, ES8388_ADCCONTROL8, 0x00);
	e |= hifi_i2c_write(ES8388_ADDR, ES8388_ADCCONTROL9, 0x00);
	e |= hifi_i2c_write(ES8388_ADDR, ES8388_ADCCONTROL10, 0x00);
	e |= hifi_i2c_write(ES8388_ADDR, ES8388_ADCPOWER, ES8388_ADCPOWER_CAPTURE);
	vTaskDelay(pdMS_TO_TICKS(20));
	e |= hifi_i2c_write(ES8388_ADDR, ES8388_CHIPPOWER, chip & 0x55);
	int st = 0;
	if (e != ESP_OK) {
		*err = "codec didn't take the ADC settings";
		st = 500;
		goto out;
	}
	/* the coupling capacitors settle; then everything the DMA ring holds goes.
	 * Nothing reads the I2S input between captures, so the ring sits full
	 * (squeezelite's 12 buffers of 512 frames, about 128 ms) and the reads
	 * below would start on the oldest of it: reading with no wait until the
	 * ring is empty leaves only buffers the DMA fills after the settle.
	 * Then the first CAP_LEADIN_MS of fresh input goes too: emptying the
	 * ring alone was not enough. On Lite 3, 1 boot in 10 still had the first
	 * capture since power-on start with 20 ms of noise on both taps, as loud
	 * on the quiet tap as on the driven one. It was live input from 190 ms to
	 * 210 ms after the ADC's release, and every later capture was clean
	 * (2026-11-18). The lead-in is five times that. */
	vTaskDelay(pdMS_TO_TICKS(200));
	size_t got = 0, want = (size_t) frames * 4;
	for (size_t drained = 0; drained < 256 * 1024; drained += got) { /* over 1 s of input: a bound, never reached */
		if (i2s_read(CONFIG_I2S_NUM, pcm, want < 4096 ? want : 4096, &got, 0) != ESP_OK || !got) break;
	}
	const size_t lead_bytes = CAP_LEADIN_MS * TONE_RATE / 1000 * 4;
	for (size_t lead = 0; lead < lead_bytes; lead += got) {
		size_t n = lead_bytes - lead;
		if (n > want) n = want;
		if (n > 4096) n = 4096;
		if (i2s_read(CONFIG_I2S_NUM, pcm, n, &got, pdMS_TO_TICKS(300)) != ESP_OK || !got) break;
	}
	size_t have = 0;
	while (have < want) {
		if (i2s_read(CONFIG_I2S_NUM, (uint8_t *) pcm + have, want - have, &got, pdMS_TO_TICKS(300)) != ESP_OK || !got) break;
		have += got;
	}
	if (have < want) {
		*err = "the I2S input stopped: is the output running?";
		st = 409;
		goto out;
	}
	info->rate = TONE_RATE;
	if (src == HIFI_CAP_LOOPBACK) {
		info->ch[0] = "spk_l+";
		info->ch[1] = "spk_r+";
		info->v_per_fs[0] = info->v_per_fs[1] = ADC_FS_PEAK_V * TAP_DIVIDER;
	} else {
		info->ch[0] = "line_l";
		info->ch[1] = "line_r";
		info->v_per_fs[0] = info->v_per_fs[1] = ADC_FS_PEAK_V * LINE_DIVIDER;
	}
out:
	/* the ADC down again, keeping the DAC's bits as they are now */
	if (hifi_i2c_read(ES8388_ADDR, ES8388_CHIPPOWER, &chip) != ESP_OK ||
	    hifi_i2c_write(ES8388_ADDR, ES8388_CHIPPOWER, (chip & 0x55) | ES8388_CHIPPOWER_ADC_DOWN) != ESP_OK)
		ESP_LOGW(TAG, "capture: couldn't power the ADC down again");
	capture_busy = false;
	codec_dirty = true; /* back to the routing for the input in use */
	return st;
}

/*
 * The self-test's speaker path, end to end: a quiet 1 kHz tone on the left
 * side, then the right, read back on the loopback taps and judged as
 * stereo (hifi_loop_judge). -50 dBFS is a faint beep on a speaker; on
 * Lite 2 the quiet legs read 40 to 58 dB under it in the 1 kHz bin (once
 * 28 dB on the first run after a boot, before the detrend). 4800 frames hold
 * whole cycles of 1 kHz, and hifi_capture_tone_rms takes off the ADC's
 * offset and its drift while it settles after a boot. It measures the
 * board, not the user's sound: the tone plays at unity with Boost out
 * (hifi_dsp_process), the EQ and loudness out (hifi_eq_bypass) and stereo
 * whatever the channel mode (hifi_chan_mix, the codec), so it is judged
 * as stereo, and the user's mode comes back when it ends. LOOP_TAP_GAIN is
 * the taps' RMS per RMS of the tone, measured on Lite 2 (2026-11-17) with
 * the EQ flat: one leg of the bridge, 1.2 dB under the nominal chain.
 * The check waits until the amp has been out of shutdown for
 * LOOP_AMP_SETTLE_MS: on Lite 3 the first self-test after a boot, with the
 * amp woken from cold about 0.5 s before the left side's capture, failed 2
 * runs in 4 (L -11.2/+0.2, L +4.7/-4.6 dB: the taps aren't settled yet),
 * and 4 runs in 4 passed when a tone had kept the amp on for 4 s first.
 */
#define LOOP_TONE_DBFS -50.0f
#define LOOP_AMP_SETTLE_MS 2000
#define LOOP_FRAMES 4800
#define LOOP_TAP_GAIN 0.386f
static const char *loop_busy(void) {
	const char *src = source_name();
	return audio_active ? "a stream plays" : tone_active ? "a test tone plays"
	       : strcmp(src, "idle") && strcmp(src, "test") ? "a source is connected"
	       : input == HIFI_INPUT_DJ_DIRECT ? "DJ direct plays" : NULL;
}

static bool loop_running; /* under LOCK: one loopback check at a time */

/* squeezelite's equalizer_process asks on every block (firmware/patches) */
bool hifi_eq_bypass(void) { return loop_flat; }

/* the codec back to the user's channel mode, and the EQ back in */
static void loop_unflat(void) {
	LOCK();
	loop_flat = false;
	hifi_bsp_channel(channel);
	UNLOCK();
	codec_dirty = true;
}

static void selftest_loopback(hifi_selftest_t *t, bool amp_on) {
	if (!has_taps()) return;
	const char *busy = loop_busy();
	LOCK();
	if (!busy && loop_running) busy = "another self-test runs it";
	if (!busy) loop_running = true;
	UNLOCK();
	if (busy) {
		hifi_selftest_add(t, "loopback", true, "skipped: %s", busy); /* not a fault, and not ours to stop */
		return;
	}
	int16_t *pcm = amp_on ? heap_caps_malloc(LOOP_FRAMES * 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
	if (!pcm) {
		hifi_selftest_add(t, "loopback", false, amp_on ? "no memory for the capture" : "amp not on");
		LOCK();
		loop_running = false;
		UNLOCK();
		return;
	}
	/* the amp out of shutdown long enough for its output to settle; the
	 * hold keeps it awake meanwhile */
	selftest_hold_until = now_ms() + LOOP_AMP_SETTLE_MS + 1500;
	for (int i = 0; i < LOOP_AMP_SETTLE_MS / 10 && now_ms() - amp_off_ms < LOOP_AMP_SETTLE_MS; i++)
		vTaskDelay(pdMS_TO_TICKS(10));
	float got[2][2] = {{0}};
	const char *err = now_ms() - amp_off_ms < LOOP_AMP_SETTLE_MS ? "the amp didn't stay on for the 2 s it settles in" : NULL;
	LOCK();
	loop_flat = true;
	hifi_bsp_channel(HIFI_CHAN_STEREO);
	UNLOCK();
	uint32_t asked = codec_started;
	codec_dirty = true;
	/* the power task routes it: wait for a route begun after the request */
	for (int i = 0; i < 100 && (int32_t) (codec_routed - asked) <= 0; i++) vTaskDelay(pdMS_TO_TICKS(10));
	if ((int32_t) (codec_routed - asked) <= 0) err = "the codec didn't take the test's stereo routing";
	for (int side = 0; side < 2 && !err; side++) {
		hifi_tone_params_t p = {.type = HIFI_TONE_SINE, .freq_hz = 1000, .level_dbfs = LOOP_TONE_DBFS,
					.duration_s = 2, .channel = side ? HIFI_CH_RIGHT : HIFI_CH_LEFT};
		if (op_tone_start(&p, false, &err)) break;
		vTaskDelay(pdMS_TO_TICKS(100)); /* the output starts and the 10 ms fade-in ends */
		hifi_cap_info_t info;
		if (op_capture(HIFI_CAP_LOOPBACK, pcm, LOOP_FRAMES, &info, &err)) break;
		for (int leg = 0; leg < 2; leg++)
			got[side][leg] = hifi_capture_tone_rms(pcm, LOOP_FRAMES, leg, p.freq_hz, TONE_RATE);
	}
	op_tone_stop();
	free(pcm);
	/* the tone's fade and the 100 ms queued behind it go out under the
	 * bypass: the EQ comes back in on silence */
	for (int i = 0; i < 50 && tone_active; i++) vTaskDelay(pdMS_TO_TICKS(10));
	vTaskDelay(pdMS_TO_TICKS(150));
	loop_unflat();
	LOCK();
	loop_running = false;
	UNLOCK();
	const char *src = source_name(); /* not audio_active: our own tone counts in it */
	if (strcmp(src, "idle") && strcmp(src, "test")) /* a source took the output mid-check */
		hifi_selftest_add(t, "loopback", true, "skipped: a source is connected");
	else if (err)
		hifi_selftest_add(t, "loopback", false, "%s", err);
	else {
		char v[64];
		bool ok = hifi_loop_judge(HIFI_CHAN_STEREO, got, LOOP_TAP_GAIN * powf(10, LOOP_TONE_DBFS / 20) / sqrtf(2),
					  HIFI_LOOP_TOL_DB, v, sizeof(v));
		hifi_selftest_add(t, "loopback", ok, "%s", v);
	}
}
#endif

static int op_set_wifi(const char *ssid, const char *password) {
	network_async_connect(ssid, password); /* the network manager saves it once joined */
	return 0;
}

static bool netif_ip(esp_netif_t *n, esp_netif_ip_info_t *ip) {
	const char *key = n ? esp_netif_get_ifkey(n) : NULL;
	return n && !(key && !strcmp(key, "WIFI_AP_DEF")) && esp_netif_is_netif_up(n) &&
	       esp_netif_get_ip_info(n, ip) == ESP_OK && ip->ip.addr;
}

bool hifi_net_ip(char *buf, size_t n) {
	/* the Wi-Fi station first, then Ethernet, then any other that is up: on
	 * Ethernet (Plus/Ultra, QEMU's open_eth) WIFI_STA_DEF has no address.
	 * (ESP-IDF v4.4 has no esp_netif_get_default_netif) */
	esp_netif_ip_info_t ip = {0};
	bool ok = netif_ip(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip) ||
		  netif_ip(esp_netif_get_handle_from_ifkey("ETH_DEF"), &ip);
	for (esp_netif_t *i = esp_netif_next(NULL); !ok && i; i = esp_netif_next(i)) ok = netif_ip(i, &ip);
	if (buf && n) {
		if (ok) snprintf(buf, n, IPSTR, IP2STR(&ip.ip));
		else buf[0] = 0;
	}
	return ok;
}

void hifi_main_note_boot_heap(void) {
	if (!heap_boot) {
		heap_boot = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
		ESP_LOGI(TAG, "boot heap %u B free", (unsigned) heap_boot);
	}
}

void hifi_main_trial_inputs(hifi_trial_t *t) {
	t->component_ok = lock != NULL;
	t->i2c_ok = hifi_bsp_parts_ok();
	t->heap_ok = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) > HIFI_HEAP_MIN;
	wifi_config_t wc = {0};
	t->wifi_configured = esp_wifi_get_config(WIFI_IF_STA, &wc) == ESP_OK && wc.sta.ssid[0];
	t->wifi_ok = hifi_net_ip(NULL, 0);
#ifdef CONFIG_HIFI_LAB
	t->lab_on = true;
	t->lab_ok = hifi_lab_esp_ok();
#endif
}

const hifi_ops_t hifi_ops = {
	.info = op_info, .snapshot = op_snapshot, .tone_start = op_tone_start, .tone_stop = op_tone_stop,
	.amp_enable = op_amp_enable, .amp_reset = op_amp_reset, .set_input = op_set_input,
	.set_sub = op_set_sub, .set_loudness = op_set_loudness,
#if HIFI_HAS_CHANNEL
	.set_channel = op_set_channel,
#endif
	.set_eq = op_set_eq, .set_name = op_set_name, .reboot = op_reboot,
	.factory_reset = op_factory_reset, .factory_arm = op_factory_arm, .control = op_control, .selftest = op_selftest,
	.set_volume = op_set_volume, .ir_learn = op_ir_learn, .ir_forget = op_ir_forget,
#if HIFI_HAS_CAPTURE
	.capture = op_capture,
#endif
	.set_wifi = op_set_wifi, .remote_status = hifi_remote_status,
	.remote_set = hifi_remote_set, .ota = &hifi_ota_session, .ota_status = hifi_ota_status,
	.diag = &hifi_diag_ops,
	.wifi_scan = hifi_net_wifi_scan, .wifi_forget = hifi_net_wifi_forget,
#if HIFI_ZONES > 1
	.set_zone = hifi_bsp_set_zone,
#endif
	.update_check = hifi_net_update_check, .update_start = hifi_net_update_start,
#ifdef CONFIG_HIFI_LAB
	.lab = hifi_lab_esp_op,
#endif
	.party = hifi_party_op,
#ifdef HIFI_HAS_LED_TEST
	.set_led = hifi_bsp_set_led,
#endif
#ifdef HIFI_HAS_BACKLIGHT
	.set_backlight = hifi_bsp_set_backlight,
#endif
	.reset_reason = hifi_crash_reset_reason,
	.last_log = hifi_crash_last_log,
	.bt_test = op_bt_test,
};

/* ------------------------------------------------------------------ console, mDNS */

static int cmd_hifi(int argc, char **argv) {
	const char *method, *path;
	char body[256];
	size_t cap = 4096;
	char *out = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if (!out) out = malloc(cap);
	if (!out) return 1;
	int rc = 0;
	if (argc >= 2 && !strcmp(argv[1], "i2c")) {
		/* `hifi i2c`: the console's only, never the LAN API's */
		const char *busy = NULL;
		if (tone_active) busy = "a test tone plays";
		else if (strcmp(source_name(), "idle")) busy = "a source is connected";
		else if (audio_active) busy = "audio plays";
		else if (power.out.amp_on) busy = "the amps are on";
		rc = hifi_i2c_cli(argc - 1, (const char *const *) argv + 1, busy, hifi_i2c_read, hifi_i2c_write, out, cap) != 200;
	} else if (!hifi_cli_request(argc - 1, (const char *const *) argv + 1, &method, &path, body, sizeof(body), out, cap)) {
		rc = 1;
	} else {
		int st = hifi_api_handle(&hifi_ops, method, path, body, out, cap);
		rc = st != 200;
	}
	printf("%s\n", out);
	free(out);
	return rc;
}

/*
 * `api <op> ["<json>"]`: any test API call over the console, for the bench
 * and provisioning (docs/remote.md). With a body it is a POST, without one a
 * GET. The reply is one line, `API {json}`, with "status" added on an error.
 */
static int cmd_api(int argc, char **argv) {
	if (argc < 2 || argc > 3) {
		printf("API {\"error\":\"usage: api <op> [json]\",\"status\":400}\n");
		return 1;
	}
	char path[64];
	snprintf(path, sizeof(path), "/api/v1/%s", argv[1]);
	const char *body = argc == 3 ? argv[2] : NULL;
	size_t cap = hifi_api_reply_cap(path, body);
	char *out = heap_caps_malloc(cap + 24, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if (!out) out = malloc(cap + 24);
	if (!out) return 1;
	int st = hifi_api_handle(&hifi_ops, body ? "POST" : "GET", path, body, out, cap);
	size_t n = strlen(out);
	if (st != 200 && n && out[n - 1] == '}') snprintf(out + n - 1, 24, ",\"status\":%d}", st);
	printf("API %s\n", out);
	free(out);
	return st != 200;
}

/* the console and mDNS come up after us; keep trying until they take it */
static void services_task(void *arg) {
	bool console = false, mdns = false, volume = !hifi_output_gain;
	while (!console || !mdns || !volume) {
		vTaskDelay(pdMS_TO_TICKS(1000));
		if (!volume) {
			/* the saved volume, once squeezelite's output is up, unless a
			 * source has set its own already */
			LOCK();
			bool src = vol_from_source;
			uint32_t g = hifi_vol_gain(&vol);
			UNLOCK();
			volume = src || quiet_req || (!tone_active && hifi_output_gain(g, g));
		}
		if (!console) {
			const esp_console_cmd_t c = {
				.command = "hifi",
				.help = "Receiver test API: hifi info|state|selftest|tone ...|amp ...|input ...|sub ...|control ...",
				.func = cmd_hifi,
			};
			const esp_console_cmd_t a = {
				.command = "api",
				.help = "Test API call: api <op> [\"json\"], replies API {json} (docs/remote.md)",
				.func = cmd_api,
			};
			console = esp_console_cmd_register(&c) == ESP_OK && esp_console_cmd_register(&a) == ESP_OK;
		}
		if (!mdns) {
			if (hifi_net_ip(NULL, 0)) {
				mdns_txt_item_t txt[] = {
					{"model", HIFI_MODEL}, {"board", HIFI_BOARD}, {"fw", HIFI_FW_VERSION}, {"api", "1"},
				};
				mdns = mdns_service_add(NULL, "_hifi", "_tcp", 80, txt, sizeof(txt) / sizeof(txt[0])) == ESP_OK;
				if (mdns) ESP_LOGI(TAG, "advertising _hifi._tcp");
			}
		}
	}
	vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ hooks */

void hifi_dsp_process(uint8_t *buf, uint32_t frames, int bytes_per_frame, uint32_t rate) {
	/* Boost first, under its limiter at the amp's clip point; a test tone
	 * and DJ direct's line copy play at unity */
	if (tone_active || line_unity) {
		hifi_boost_bypass(&boost); /* drops at most 1 ms of delayed audio */
	} else if (hifi_boost_active(&boost)) {
		hifi_boost_set_ceiling(&boost, hifi_gov_clip_fs(&gov));
		hifi_boost_process(&boost, buf, frames, bytes_per_frame, rate);
	}
	hifi_gov_process(&gov, buf, frames, bytes_per_frame, rate);
	audio_active = gov.ms_avg > 1e-8f; /* above about -77 dBFS */
	/* the channel mode, after the governor (it models it, and judges each
	 * side as played): the codec can't cross sides itself. The loopback
	 * check measures both sides apart, so it plays stereo */
	hifi_chan_mix(loop_flat ? HIFI_CHAN_STEREO : channel, buf, frames, bytes_per_frame);
	hifi_bsp_audio(buf, frames, bytes_per_frame);
}

void hifi_http_register(httpd_handle_t server) {
	hifi_gate_reserve_pins(HIFI_LAN_RESERVED_PINS); /* whichever starts first (L8) */
	hifi_http_start(server, &hifi_ops);
}

/* Every failed allocation goes to the console (and so to the crash log): a
 * Bluetooth connect panicked in btu_start_timer when its timer couldn't be
 * made, and this tells running out of internal RAM from other causes. */
static void alloc_failed(size_t size, uint32_t caps, const char *fn) {
	static int n;
	if (n++ >= 20) return;
	ESP_LOGE(TAG, "alloc of %u B failed in %s (caps 0x%x); internal %u B free, largest %u B, lowest %u B",
		 (unsigned) size, fn ? fn : "?", (unsigned) caps, (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
		 (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
		 (unsigned) heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

static bool hifi_init(void) {
	hifi_gate_reserve_pins(HIFI_LAN_RESERVED_PINS); /* first: bt_test_boot checks a line (L8) */
	heap_caps_register_failed_alloc_callback(alloc_failed);
	hifi_diag_init();
	factory_boot();
	bt_test_boot();
	lock = xSemaphoreCreateMutex();
	tone_lock = xSemaphoreCreateMutex();

	hifi_power_cfg_t pc;
	hifi_gov_cfg_t gc;
	hifi_bsp_init(&pc, &gc); /* the board's pins and parts, and its numbers */
	hifi_power_init(&power, &pc, now_ms());
	hifi_gov_init(&gov, &gc);

	char *s = config_alloc_get_str("hifi_sub_db", NULL, "0");
	sub_db = s ? strtof(s, NULL) : 0;
	free(s);
	hifi_boost_init(&boost);
	s = config_alloc_get_str("hifi_boost_db", NULL, "0");
	hifi_boost_set_db(&boost, s && HIFI_HAS_BOOST ? strtof(s, NULL) : 0);
	free(s);
	s = config_alloc_get_str("hifi_heat_w", NULL, NULL);
	if (s) hifi_gov_set_heat_w(&gov, strtof(s, NULL));
	free(s);
	s = config_alloc_get_str("hifi_sub_on", NULL, "1");
	sub_on = !s || strcmp(s, "0");
	free(s);
#if HIFI_HAS_CHANNEL
	s = config_alloc_get_str("hifi_channel", NULL, "stereo");
	if (!s || !hifi_chan_parse(s, &channel)) channel = HIFI_CHAN_STEREO;
	free(s);
	gov.chan = channel;
	hifi_bsp_channel(channel);
#endif
	char *sv = config_alloc_get_str("hifi_volume", NULL, "30");
	s = config_alloc_get_str("hifi_max_vol", NULL, "100");
	hifi_vol_init(&vol, sv ? atoi(sv) : 30, s ? atoi(s) : 100);
	free(sv);
	free(s);
	hifi_info_t bi;
	memset(&bi, 0, sizeof(bi));
	hifi_bsp_info(&bi);
	board_line = bi.caps.line && hifi_line_start;
	hifi_djg_cfg_t dc;
	hifi_djg_cfg_default(&dc, &gc);
	hifi_djg_init(&djg, &dc);
	s = config_alloc_get_str("hifi_input", NULL, "stream");
	if (s && !strcmp(s, "dj")) input = HIFI_INPUT_DJ_DIRECT;
	else if (s && !strcmp(s, "line") && board_line) input = HIFI_INPUT_LINE;
	else input = HIFI_INPUT_STREAM;
	free(s);
	hifi_meta_init(&meta);
	hifi_ir_init(&ir);
	hifi_tap_init(&taps);
	s = config_alloc_get_str("hifi_ir", NULL, "");
	hifi_ir_load(&ir, s);
	free(s);

	const esp_partition_t *run = esp_ota_get_running_partition();
	in_recovery = run && run->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY;
	xTaskCreatePinnedToCore(power_task, "hifi_power", 3072, NULL, ESP_TASK_PRIO_MIN + 3, &power_task_h, 0);
	if (hifi_sink_start) tone_task = task_psram(tone_main, "hifi_tone", 4096, ESP_TASK_PRIO_MIN + 2, 1);
	if (board_line) task_psram(line_main, "hifi_line", 4096, ESP_TASK_PRIO_MIN + 4, 1);
	xTaskCreate(services_task, "hifi_svc", 3072, NULL, ESP_TASK_PRIO_MIN + 1, NULL);
	tap_task = task_psram(tap_main, "hifi_tap", 4096, ESP_TASK_PRIO_MIN + 2, tskNO_AFFINITY);
#ifdef CONFIG_HIFI_LAB
	hifi_lab_esp_init();
#endif
	/* party on at boot: Bluetooth stays off (squeezelite registers it after
	 * this), and the group starts once Wi-Fi is up */
	if (hifi_party_init) { /* the app only */
		if (hifi_party_saved_on() && hifi_bt_sink_boot_off) hifi_bt_sink_boot_off();
		hifi_party_init();
		hifi_party_volume(hifi_vol_gain(&vol));
	}
	hifi_remote_start(&hifi_ops);
	hifi_bsp_start();
	ESP_LOGI(TAG, "%s on %s, firmware %s", HIFI_MODEL, HIFI_BOARD, HIFI_FW_VERSION);
	return true;
}

/* targets.h's struct, declared here so this component needn't depend on targets */
struct target_s {
	char *model;
	bool (*init)(void);
};
const struct target_s target_hifi = {.model = HIFI_MODEL, .init = hifi_init};
