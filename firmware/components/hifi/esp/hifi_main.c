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

#include "driver/adc.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/i2s.h"
#include "esp_adc_cal.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_spi_flash.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs_flash.h"

#include "audio_controls.h"
#include "network_manager.h"
#include "platform_config.h"

#include "hifi_api.h"
#include "hifi_board.h"
#include "hifi_esp.h"
#include "hifi_gov.h"
#include "hifi_http.h"
#include "hifi_ir.h"
#include "hifi_json.h"
#include "hifi_meta.h"
#include "hifi_pd.h"
#include "hifi_power.h"
#include "hifi_tone.h"
#include "hifi_vol.h"

static const char TAG[] = "hifi";

/* squeezelite-esp32, through our patch; weak so a build without them links */
extern void hifi_sink_start(uint32_t rate) __attribute__((weak));
extern uint32_t hifi_sink_write(const uint8_t *data, uint32_t len) __attribute__((weak));
extern void hifi_sink_stop(void) __attribute__((weak));
extern uint32_t hifi_sink_queued_ms(void) __attribute__((weak));
extern int hifi_sink_source(void) __attribute__((weak));
extern void equalizer_set_gain(int8_t *gain) __attribute__((weak));
extern int set_squeezelite_player_name(FILE *f, const char *name) __attribute__((weak));
extern int set_cspot_player_name(FILE *f, const char *name) __attribute__((weak));
extern bool hifi_output_gain(unsigned left, unsigned right) __attribute__((weak));

/* ------------------------------------------------------------------ state */

static SemaphoreHandle_t lock;       /* power, contract and settings below */
static hifi_power_t power;
static hifi_gov_t gov;
static hifi_contract_t contract;
static float vbus_v, pvdd_v;
static bool pg, fault, sink_ok;
static uint8_t pd_status;
static hifi_input_t input = HIFI_INPUT_STREAM;
static bool sub_on = true;
static float sub_db = 0;
static volatile bool codec_dirty = true; /* re-apply input/sub to the codec */
static volatile uint32_t selftest_hold_until; /* keeps the amp awake for the self-test */
static volatile bool capture_busy;   /* the codec's ADC is ours until the capture ends */

static hifi_vol_t vol;               /* under lock, like what follows */
static bool vol_dirty, vol_from_source;
static uint32_t vol_changed_ms;
static hifi_meta_t meta;
static hifi_ir_t ir;

static volatile bool audio_active;   /* set by the audio thread */
static esp_adc_cal_characteristics_t adc_chars;

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

static esp_err_t i2c_xfer(uint8_t addr, uint8_t reg, uint8_t *val, bool write) {
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
	return err;
}

static esp_err_t i2c_read(uint8_t addr, uint8_t reg, uint8_t *val) { return i2c_xfer(addr, reg, val, false); }
static esp_err_t i2c_write(uint8_t addr, uint8_t reg, uint8_t val) { return i2c_xfer(addr, reg, &val, true); }

/*
 * Squeezelite installs the I2C driver when it starts the codec. If it hasn't
 * after a few seconds (recovery firmware, no codec config), install it here
 * so the PD contract can still be read.
 */
static void i2c_ensure(void) {
	static bool tried;
	uint8_t v;
	if (tried || now_ms() < 8000 || i2c_read(CH224_I2C_ADDR, CH224_REG_STATUS, &v) != ESP_ERR_INVALID_STATE) return;
	tried = true;
	i2c_config_t c = {
		.mode = I2C_MODE_MASTER, .sda_io_num = I2C_SDA, .scl_io_num = I2C_SCL,
		.sda_pullup_en = GPIO_PULLUP_ENABLE, .scl_pullup_en = GPIO_PULLUP_ENABLE,
		.master.clk_speed = 100000,
	};
	i2c_param_config(I2C_PORT, &c);
	i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
	ESP_LOGW(TAG, "codec driver didn't start; installed I2C %d ourselves", I2C_PORT);
}

/* ------------------------------------------------------------------ codec */

static int sub_code(float db) {
	int code = (int) lrintf(30 + db / 1.5f);
	return code < 0 ? 0 : code > 33 ? 33 : code;
}

/* Input routing and sub level onto the ES8388. Squeezelite's controlset
 * (firmware/patches, Kconfig) sets the rest at codec start. */
static esp_err_t codec_apply(void) {
	LOCK();
	hifi_input_t in = input;
	bool on = sub_on;
	int code = sub_code(sub_db);
	/* the bypass skips squeezelite, so the volume goes on the codec's outputs */
	int out1 = hifi_vol_codec_code(&vol, ES8388_OUT1_FULL);
	UNLOCK();
	esp_err_t err = ESP_OK;
	if (in == HIFI_INPUT_DJ_DIRECT) {
		/* analog inputs on (ADCs stay off), mixers take LIN1/RIN1 at -3 dB, DAC out
		 * of the mix: the line in reaches the amp and sub outs with no latency.
		 * Muted: the line in leaves the mix too */
		uint8_t mix = ES8388_MIX_LIN_M3DB | (out1 < 0 ? 0 : 0x40); /* bit6: LIN into the mixer */
		err |= i2c_write(ES8388_ADDR, ES8388_ADCPOWER, 0x38);
		err |= i2c_write(ES8388_ADDR, ES8388_DACCONTROL17, mix);
		err |= i2c_write(ES8388_ADDR, ES8388_DACCONTROL20, mix);
		err |= i2c_write(ES8388_ADDR, ES8388_LOUT1VOL, out1 < 0 ? 0 : out1);
		err |= i2c_write(ES8388_ADDR, ES8388_ROUT1VOL, out1 < 0 ? 0 : out1);
	} else {
		/* squeezelite's own setting: DAC into the mixers, inputs powered down;
		 * the volume is squeezelite's digital gain (hifi_volume_hook) */
		err |= i2c_write(ES8388_ADDR, ES8388_DACCONTROL17, 0xB8);
		err |= i2c_write(ES8388_ADDR, ES8388_DACCONTROL20, 0xB8);
		err |= i2c_write(ES8388_ADDR, ES8388_ADCPOWER, 0xFC);
		err |= i2c_write(ES8388_ADDR, ES8388_LOUT1VOL, ES8388_OUT1_FULL);
		err |= i2c_write(ES8388_ADDR, ES8388_ROUT1VOL, ES8388_OUT1_FULL);
	}
	err |= i2c_write(ES8388_ADDR, ES8388_DACPOWER, on ? 0x3C : 0x30);
	err |= i2c_write(ES8388_ADDR, ES8388_LOUT2VOL, on ? code : 0);
	err |= i2c_write(ES8388_ADDR, ES8388_ROUT2VOL, on ? code : 0);
	return err;
}

/* ------------------------------------------------------------------ power task */

static float read_div(adc1_channel_t ch, float divider) {
	uint32_t sum = 0;
	for (int i = 0; i < 8; i++) sum += adc1_get_raw(ch);
	return esp_adc_cal_raw_to_voltage(sum / 8, &adc_chars) * divider / 1000.0f;
}

static float read_vbus(void) { return read_div(ADC_VBUS, VBUS_DIVIDER); }

/* PVDD after the switch: the sequencer confirms the switch closed from it */
static float read_pvdd(void) { return read_div(ADC_PVDD, PVDD_DIVIDER); }

static void power_task(void *arg) {
	TickType_t wake = xTaskGetTickCount();
	uint32_t last_pd = 0, codec_ok_at = 0;
	for (;;) {
		vTaskDelayUntil(&wake, pdMS_TO_TICKS(10));
		uint32_t now = now_ms();
		float v = read_vbus(), pv = read_pvdd();
		bool g = gpio_get_level(PIN_PD_PG) == 0;
		bool f = gpio_get_level(PIN_AMP_FAULTZ) == 0;

		/* the PD sink's registers every 500 ms */
		uint8_t st = pd_status, cur = 0;
		bool ok = sink_ok, refreshed = now - last_pd >= 500;
		if (refreshed) {
			last_pd = now;
			i2c_ensure();
			ok = i2c_read(CH224_I2C_ADDR, CH224_REG_STATUS, &st) == ESP_OK &&
			     i2c_read(CH224_I2C_ADDR, CH224_REG_CURRENT, &cur) == ESP_OK;
			if (!ok) cur = 0;
		}

		/* the codec: re-apply our routing once it answers, and on request */
		if (codec_dirty && !capture_busy) {
			uint8_t dummy;
			if (i2c_read(ES8388_ADDR, 0x00, &dummy) == ESP_OK) {
				if (!codec_ok_at) codec_ok_at = now; /* let squeezelite finish its init */
				if (now - codec_ok_at > 2000 && codec_apply() == ESP_OK) codec_dirty = false;
			}
		}

		LOCK();
		vbus_v = v;
		pvdd_v = pv;
		pg = g;
		fault = f;
		if (refreshed) {
			sink_ok = ok;
			pd_status = st;
			if (ok) hifi_contract_from(true, st, cur, v, &contract);
			else hifi_contract_from(false, 0, 0, v, &contract);
			/* until PG, assume no more than a USB 2.0 port */
			hifi_gov_set_supply(&gov, contract.v, pg ? contract.w : 2.5f);
		}
		hifi_power_in_t in = {
			.now_ms = now, .vbus_v = v, .pvdd_v = pv, .pg = g, .fault = f,
			.audio = audio_active || tone_active || input == HIFI_INPUT_DJ_DIRECT ||
				 (int32_t) (selftest_hold_until - now) > 0,
		};
		/* the volume goes to flash once it has settled for 3 s */
		bool save_vol = vol_dirty && now - vol_changed_ms > 3000;
		int save_v = vol.volume, save_max = vol.max_volume;
		if (save_vol) vol_dirty = false;
		hifi_pwr_state_t before = power.state;
		const hifi_power_out_t *o = hifi_power_step(&power, &in);
		/* the amp goes into shutdown before PVDD opens, and comes out after */
		if (!o->amp_on) gpio_set_level(PIN_AMP_SDZ, 0);
		gpio_set_level(PIN_PVDD_EN, o->pvdd_en);
		if (o->amp_on) gpio_set_level(PIN_AMP_SDZ, 1);
		if (power.state != before)
			ESP_LOGI(TAG, "power %s -> %s (VBUS %.2f V, PVDD %.2f V, PG %d)", hifi_power_state_name(before),
				 hifi_power_state_name(power.state), v, pv, g);
		UNLOCK();
		if (save_vol) {
			char sv[8];
			snprintf(sv, sizeof(sv), "%d", save_v);
			config_set_value(NVS_TYPE_STR, "hifi_volume", sv);
			snprintf(sv, sizeof(sv), "%d", save_max);
			config_set_value(NVS_TYPE_STR, "hifi_max_vol", sv);
		}
	}
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
	if (hifi_output_gain && !tone_active) hifi_output_gain(g, g);
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
	*left = *right = g;
	return !tone_active; /* a test signal plays at unity; the volume comes back after */
}

/* volume up/down here rather than at the source: nothing streams, or DJ direct */
static bool volume_is_local(void) {
	LOCK();
	bool direct = input == HIFI_INPUT_DJ_DIRECT;
	UNLOCK();
	int s = hifi_sink_source ? hifi_sink_source() : 0;
	return direct || s == 0;
}

static void toggle_input(void) {
	LOCK();
	input = input == HIFI_INPUT_DJ_DIRECT ? HIFI_INPUT_STREAM : HIFI_INPUT_DJ_DIRECT;
	UNLOCK();
	codec_dirty = true;
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

/* A button press on the board (squeezelite's audio controls). */
bool hifi_button_hook(int action, bool pressed) {
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
		hifi_sink_stop();
		apply_volume(); /* anything set while the tone played */
	}
}

/* ------------------------------------------------------------------ ops */

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
	i->psram_mb = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) ? 8 : 0; /* WROVER-E N8R8 */
	i->caps = (hifi_caps_t) {
		.dj_direct = true, .eq_min_db = -12, .eq_max_db = 12,
		.sub_count = 2, .sub_crossover = false, .sub_per_sub = false,
		.zones = 1, .eth = false, .pdo_v_max = 20,
	};
}

static const char *source_name(void) {
	static const char *names[] = {"idle", "lms", "bt", "airplay", "spotify", "test"};
	int s = hifi_sink_source ? hifi_sink_source() : 0;
	return s >= 0 && s <= 5 ? names[s] : "idle";
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
	s->pvdd_v = pvdd_v;
	s->pg = pg;
	s->contract = contract;
	s->pvdd_on = power.out.pvdd_en;
	s->amp_enabled = power.out.amp_on;
	s->amp_fault = fault;
	s->amp_latched = power.latched;
	s->amp_faults = power.faults;
	s->input = input;
	s->sub_on = sub_on;
	s->sub_db = (sub_code(sub_db) - 30) * 1.5f;
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
	UNLOCK();
	s->vbus_i_a = s->amp_temp_c = NAN; /* no current or temperature sense on lite-main */
	s->gov_budget_w = gov.budget_w;
	s->gov_in_w = hifi_gov_in_w(&gov);
	s->gov_out_w = hifi_gov_out_w(&gov);
	s->gov_gain_db = hifi_gov_gain_db(&gov);
	s->gov_limiting = gov.limiting;
	s->gov_events = gov.events;
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
	esp_netif_ip_info_t ip = {0};
	esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
	if (sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr)
		snprintf(s->ip, sizeof(s->ip), IPSTR, IP2STR(&ip.ip));
	wifi_ap_record_t ap;
	if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
		snprintf(s->ssid, sizeof(s->ssid), "%s", (const char *) ap.ssid);
		s->rssi = ap.rssi;
	}
	wifi_mode_t mode = WIFI_MODE_NULL;
	esp_wifi_get_mode(&mode);
	s->net_mode = !s->ip[0] && (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) ? "ap" : "sta";
	s->heap_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
	s->psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
}

static int op_tone_start(const hifi_tone_params_t *p, bool force, const char **err) {
	if (!hifi_sink_start || !tone_task) {
		*err = "this build has no audio output";
		return 500;
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
	LOCK();
	input = in;
	UNLOCK();
	codec_dirty = true;
	return 0;
}

static float op_set_sub(int on, float db) {
	char v[16];
	LOCK();
	if (on >= 0) sub_on = on;
	if (!isnan(db)) sub_db = db;
	float set = (sub_code(sub_db) - 30) * 1.5f;
	bool o = sub_on;
	UNLOCK();
	snprintf(v, sizeof(v), "%.1f", set);
	config_set_value(NVS_TYPE_STR, "hifi_sub_db", v);
	config_set_value(NVS_TYPE_STR, "hifi_sub_on", o ? "1" : "0");
	codec_dirty = true;
	return set;
}

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

/* The name people see in AirPlay, Bluetooth, Spotify and LMS. The hostname
 * stays hifi-lite so hifi-lite.local keeps working. */
static int op_set_name(const char *name) {
	int err = 0;
	err |= config_set_value(NVS_TYPE_STR, "hifi_name", name) != ESP_OK;
	err |= config_set_value(NVS_TYPE_STR, "airplay_name", name) != ESP_OK;
	err |= config_set_value(NVS_TYPE_STR, "bt_name", name) != ESP_OK;
	err |= config_set_value(NVS_TYPE_STR, "a2dp_dev_name", name) != ESP_OK;
	FILE *f = fopen("/dev/null", "w");
	if (set_squeezelite_player_name) err |= set_squeezelite_player_name(f, name) != 0;
	if (set_cspot_player_name) err |= set_cspot_player_name(f, name) != 0;
	if (f) fclose(f);
	return err ? -1 : 0; /* the services pick it up at the next restart */
}

static void restart_cb(void *arg) {
	if (arg) nvs_flash_erase();
	esp_restart();
}

static void restart_later(bool erase) {
	esp_timer_handle_t t;
	esp_timer_create_args_t a = {.callback = restart_cb, .arg = erase ? (void *) 1 : NULL, .name = "hifi_restart"};
	if (esp_timer_create(&a, &t) == ESP_OK) esp_timer_start_once(t, 700 * 1000); /* after the reply */
}

static void op_reboot(void) { restart_later(false); }
static void op_factory_reset(void) { restart_later(true); }

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

static void op_selftest(hifi_selftest_t *t) {
	uint8_t v;
	bool pd = i2c_read(CH224_I2C_ADDR, CH224_REG_STATUS, &v) == ESP_OK;
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
	bool vm = fabsf(s->vbus_v - s->contract.v) <= 0.08f * s->contract.v;
	hifi_selftest_add(t, "vbus_matches", vm, "%.2f V", s->vbus_v);

	bool codec = i2c_read(ES8388_ADDR, 0x00, &v) == ESP_OK;
	hifi_selftest_add(t, "codec_i2c", codec, codec ? "0x%02x acks" : "0x%02x doesn't answer", ES8388_ADDR);

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
	bool pu = s->pvdd_on && s->pvdd_v >= power.cfg.pvdd_min_frac * s->vbus_v;
	hifi_selftest_add(t, "pvdd_sensed", pu, "PVDD %.2f V of VBUS %.2f V%s", s->pvdd_v, s->vbus_v,
			  s->pvdd_on ? "" : " (switch not commanded on)");
	if (s->amp_enabled)
		hifi_selftest_add(t, "amp_no_fault", !s->amp_fault, s->amp_fault ? "FAULTZ low" : "FAULTZ high after 200 ms enabled");
	else
		hifi_selftest_add(t, "amp_no_fault", false, s->amp_latched ? "fault latched" : "amp not enabled (power %s)",
				  hifi_power_state_name(s->pwr_state));

	size_t ps = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
	hifi_selftest_add(t, "psram", ps >= 2u << 20, "%u KB mapped", (unsigned) (ps >> 10));
	hifi_selftest_add(t, "heap", s->heap_free > 20000, "%u B free", (unsigned) s->heap_free);
	hifi_selftest_add(t, "network", s->ip[0] != 0, s->ip[0] ? "%s" : "not connected", s->ip);
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

static bool has_taps(void) {
	if (HIFI_HAS_TAPS) return true;
	char *s = config_alloc_get_str("hifi_taps", NULL, "0");
	bool on = s && !strcmp(s, "1");
	free(s);
	return on;
}

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
	if (capture_busy) return *err = "a capture is running", 409;
	capture_busy = true;
	esp_err_t e = ESP_OK;
	e |= i2c_write(ES8388_ADDR, ES8388_ADCCONTROL1, 0x00);
	e |= i2c_write(ES8388_ADDR, ES8388_ADCCONTROL2, src == HIFI_CAP_LOOPBACK ? 0x50 : 0x00);
	e |= i2c_write(ES8388_ADDR, ES8388_ADCCONTROL4, 0x0C);
	e |= i2c_write(ES8388_ADDR, ES8388_ADCCONTROL5, 0x02);
	e |= i2c_write(ES8388_ADDR, ES8388_ADCCONTROL8, 0x00);
	e |= i2c_write(ES8388_ADDR, ES8388_ADCCONTROL9, 0x00);
	e |= i2c_write(ES8388_ADDR, ES8388_ADCCONTROL10, 0x00);
	e |= i2c_write(ES8388_ADDR, ES8388_ADCPOWER, ES8388_ADCPOWER_CAPTURE);
	int st = 0;
	if (e != ESP_OK) {
		*err = "codec didn't take the ADC settings";
		st = 500;
		goto out;
	}
	/* the coupling capacitors settle, and what the DMA held from before goes */
	vTaskDelay(pdMS_TO_TICKS(200));
	size_t got = 0, want = (size_t) frames * 4;
	for (int i = 0; i < 4; i++) i2s_read(CONFIG_I2S_NUM, pcm, want < 4096 ? want : 4096, &got, pdMS_TO_TICKS(50));
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
	capture_busy = false;
	codec_dirty = true; /* back to the routing for the input in use */
	return st;
}

static int op_set_wifi(const char *ssid, const char *password) {
	network_async_connect(ssid, password); /* the network manager saves it once joined */
	return 0;
}

static bool wifi_has_ip(void) {
	esp_netif_ip_info_t ip = {0};
	esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
	return sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr;
}

void hifi_main_trial_inputs(hifi_trial_t *t) {
	uint8_t v;
	t->component_ok = lock != NULL;
	t->i2c_ok = i2c_read(CH224_I2C_ADDR, CH224_REG_STATUS, &v) == ESP_OK && i2c_read(ES8388_ADDR, 0x00, &v) == ESP_OK;
	t->heap_ok = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) > 20000;
	wifi_config_t wc = {0};
	t->wifi_configured = esp_wifi_get_config(WIFI_IF_STA, &wc) == ESP_OK && wc.sta.ssid[0];
	t->wifi_ok = wifi_has_ip();
}

const hifi_ops_t hifi_ops = {
	.info = op_info, .snapshot = op_snapshot, .tone_start = op_tone_start, .tone_stop = op_tone_stop,
	.amp_enable = op_amp_enable, .amp_reset = op_amp_reset, .set_input = op_set_input,
	.set_sub = op_set_sub, .set_eq = op_set_eq, .set_name = op_set_name, .reboot = op_reboot,
	.factory_reset = op_factory_reset, .control = op_control, .selftest = op_selftest,
	.set_volume = op_set_volume, .ir_learn = op_ir_learn, .ir_forget = op_ir_forget,
	.capture = op_capture, .set_wifi = op_set_wifi, .remote_status = hifi_remote_status,
	.remote_set = hifi_remote_set, .ota = &hifi_ota_session, .ota_status = hifi_ota_status,
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
	if (!hifi_cli_request(argc - 1, (const char *const *) argv + 1, &method, &path, body, sizeof(body), out, cap)) {
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
			volume = src || (!tone_active && hifi_output_gain(g, g));
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
			esp_netif_ip_info_t ip = {0};
			esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
			if (sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr) {
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
	hifi_gov_process(&gov, buf, frames, bytes_per_frame, rate);
	audio_active = gov.ms_avg > 1e-8f; /* above about -77 dBFS */
}

void hifi_http_register(httpd_handle_t server) { hifi_http_start(server, &hifi_ops); }

static bool hifi_init(void) {
	lock = xSemaphoreCreateMutex();
	tone_lock = xSemaphoreCreateMutex();

	gpio_config_t out = {.pin_bit_mask = BIT64(PIN_PVDD_EN) | BIT64(PIN_AMP_SDZ), .mode = GPIO_MODE_OUTPUT};
	gpio_set_level(PIN_PVDD_EN, 0);
	gpio_set_level(PIN_AMP_SDZ, 0);
	gpio_config(&out);
	gpio_config_t in = {.pin_bit_mask = BIT64(PIN_PD_PG) | BIT64(PIN_AMP_FAULTZ), .mode = GPIO_MODE_INPUT,
			    .pull_up_en = GPIO_PULLUP_ENABLE};
	gpio_config(&in);

	adc1_config_width(ADC_WIDTH_BIT_12);
	adc1_config_channel_atten(ADC_VBUS, ADC_ATTEN_DB_12);
	adc1_config_channel_atten(ADC_PVDD, ADC_ATTEN_DB_12);
	esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_12, ADC_WIDTH_BIT_12, 1100, &adc_chars);

	hifi_power_cfg_t pc;
	hifi_power_cfg_lite(&pc);
	hifi_power_init(&power, &pc, now_ms());
	hifi_gov_cfg_t gc;
	hifi_gov_cfg_lite(&gc);
	hifi_gov_init(&gov, &gc);

	char *s = config_alloc_get_str("hifi_sub_db", NULL, "0");
	sub_db = s ? strtof(s, NULL) : 0;
	free(s);
	s = config_alloc_get_str("hifi_sub_on", NULL, "1");
	sub_on = !s || strcmp(s, "0");
	free(s);
	char *sv = config_alloc_get_str("hifi_volume", NULL, "30");
	s = config_alloc_get_str("hifi_max_vol", NULL, "100");
	hifi_vol_init(&vol, sv ? atoi(sv) : 30, s ? atoi(s) : 100);
	free(sv);
	free(s);
	hifi_meta_init(&meta);
	hifi_ir_init(&ir);
	s = config_alloc_get_str("hifi_ir", NULL, "");
	hifi_ir_load(&ir, s);
	free(s);

	xTaskCreatePinnedToCore(power_task, "hifi_power", 3072, NULL, ESP_TASK_PRIO_MIN + 3, NULL, 0);
	if (hifi_sink_start) xTaskCreatePinnedToCore(tone_main, "hifi_tone", 4096, NULL, ESP_TASK_PRIO_MIN + 2, &tone_task, 1);
	xTaskCreate(services_task, "hifi_svc", 3072, NULL, ESP_TASK_PRIO_MIN + 1, NULL);
	hifi_remote_start(&hifi_ops);
	ESP_LOGI(TAG, "%s on %s, firmware %s", HIFI_MODEL, HIFI_BOARD, HIFI_FW_VERSION);
	return true;
}

/* targets.h's struct, declared here so this component needn't depend on targets */
struct target_s {
	char *model;
	bool (*init)(void);
};
const struct target_s target_hifi = {.model = HIFI_MODEL, .init = hifi_init};
