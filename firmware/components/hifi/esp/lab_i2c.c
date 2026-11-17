/*
 * Lab experiment "i2c": reads the board's I2C chips through hive-fw's
 * i2c-device driver (esp/i2cdev, from templates/component/i2c-device) and
 * checks them against their datasheets. It only reads: nothing is written
 * to any chip, and above all never the CH224's 0x0A (the PD voltage).
 *
 *   api lab "{\"op\":\"start\",\"exp\":\"i2c\",\"max_s\":10}"
 *   api lab "{\"op\":\"result\",\"exp\":\"i2c\"}"
 *
 * The reads run in the start and the run ends itself at once. The bus is
 * the one squeezelite's codec driver installed (port 0 on a Lite), used
 * with install false: the legacy driver serialises each transaction on its
 * own lock, so a read lands between the codec driver's, and the driver
 * here never takes the lines (no recovery of its own on a bus it doesn't
 * own).
 *
 * CH224 (0x22), datasheet V2.0 section 5 (core/hifi_pd.h): 0x09 status,
 * bits 0-4 the handshake done (BC, QC2, QC3, PD, EPR), 5 EPR offered, 6 AVS
 * offered; 0x50 the contract's current in 50 mA units (PD). Checked: the PD
 * bit (the Lite runs only on PD, as its self-test's pd_contract says), one
 * protocol at most among bits 0-3, and a PD current of 50 mA to 5 A. Bit 7
 * isn't described there: a Lite 5 on PD read 0x88, so it is reported
 * (bit7) and not checked.
 * ES8388 (0x10, the Lite's codec): it has no ID register; it must ack and
 * its control registers 0x00-0x02 are read back as they are.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"

#include "hifi_board.h"
#include "hifi_json.h"
#include "hifi_lab_esp.h"
#include "hifi_pd.h"
#include "i2cdev.h"
#include "i2cdev_esp.h"

static const char *TAG = "lab_i2c";

#define CODEC_REGS 3

typedef struct {
	bool done;
	int probe, status_e, current_e, check_e;
	uint8_t status, current;
	char why[96];
	i2cdev_counts_t n;
} pd_t;

typedef struct {
	bool done;
	int probe, regs_e;
	uint8_t regs[CODEC_REGS];
	i2cdev_counts_t n;
} codec_t;

/* in PSRAM: only this task touches them, and reads land on the stack first (the Lite is short of internal RAM) */
static EXT_RAM_ATTR pd_t pd;
static EXT_RAM_ATTR codec_t codec;
static uint32_t runs;
static int bus_e = I2CDEV_OK;

static const i2cdev_expect_t ch224_expect[] = {
	{CH224_REG_STATUS, 0x08, 0x08, "CH224 PD handshake (status bit 3)"},
};

static void read_ch224(const i2cdev_bus_t *bus) {
	i2cdev_dev_t d;
	memset(&pd, 0, sizeof(pd));
	int e = i2cdev_init(&d, bus, CH224_I2C_ADDR, 50);
	if (e != I2CDEV_OK) {
		pd.probe = pd.status_e = pd.current_e = pd.check_e = e; /* a failed init fails the run */
		pd.done = true;
		return;
	}
	d.nack_retries = 1; /* a chip busy for a moment */
	pd.probe = i2cdev_probe(&d);
	pd.status_e = i2cdev_read_u8(&d, CH224_REG_STATUS, &pd.status);
	pd.current_e = i2cdev_read_u8(&d, CH224_REG_CURRENT, &pd.current);
	pd.check_e = i2cdev_check(&d, ch224_expect, sizeof(ch224_expect) / sizeof(ch224_expect[0]), pd.why,
				  sizeof(pd.why));
	if (pd.check_e == I2CDEV_OK && pd.current_e == I2CDEV_OK) {
		uint8_t p = pd.status & 0x0f; /* BC QC2 QC3 PD: one protocol at a time */
		if (p & (p - 1)) {
			pd.check_e = I2CDEV_DIFF;
			snprintf(pd.why, sizeof(pd.why), "CH224 status 0x%02x has more than one protocol bit", pd.status);
		} else if (pd.current < 1 || pd.current > 100) {
			pd.check_e = I2CDEV_DIFF;
			snprintf(pd.why, sizeof(pd.why), "CH224 PD current 0x%02x is outside 50 mA to 5 A", pd.current);
		}
	}
	pd.n = d.n;
	pd.done = true;
}

static void read_codec(const i2cdev_bus_t *bus) {
	memset(&codec, 0, sizeof(codec));
#ifdef ES8388_ADDR
	i2cdev_dev_t d;
	int e = i2cdev_init(&d, bus, ES8388_ADDR, 50);
	if (e != I2CDEV_OK) {
		codec.probe = codec.regs_e = e;
		codec.done = true;
		return;
	}
	d.nack_retries = 1;
	codec.probe = i2cdev_probe(&d);
	/* one register at a time: the ES8388 doesn't promise auto-increment */
	codec.regs_e = I2CDEV_OK;
	for (int i = 0; i < CODEC_REGS && codec.regs_e == I2CDEV_OK; i++)
		codec.regs_e = i2cdev_read_u8(&d, (uint8_t) i, &codec.regs[i]);
	codec.n = d.n;
	codec.done = true;
#else
	(void) bus;
#endif
}

static int start(uint32_t run, const char *body, char *err, size_t n) {
	(void) body;
	i2cdev_bus_t bus;
	i2cdev_esp_cfg_t cfg = {.port = I2C_PORT, .sda = I2C_SDA, .scl = I2C_SCL, .install = false};
	bus_e = i2cdev_esp_bus(&bus, &cfg);
	if (bus_e != I2CDEV_OK) {
		snprintf(err, n, "i2c port %d: %s", I2C_PORT, i2cdev_err_name(bus_e));
		return 503;
	}
	runs++;
	read_ch224(&bus);
	read_codec(&bus);
	bool ok = pd.done && pd.check_e == I2CDEV_OK && pd.current_e == I2CDEV_OK &&
		  (!codec.done || (codec.probe == I2CDEV_OK && codec.regs_e == I2CDEV_OK));
	ESP_LOGI(TAG, "CH224 status 0x%02x (%s), codec %s", pd.status, i2cdev_err_name(pd.check_e),
		 codec.done ? i2cdev_err_name(codec.regs_e) : "none");
	hifi_lab_esp_finished("i2c", run, ok);
	return 0;
}

static bool stop(void) { return true; /* the start did it all */ }

static void hex(hj_t *j, const char *key, uint8_t v) {
	char s[8];
	snprintf(s, sizeof(s), "0x%02x", v);
	hj_str(j, key, s);
}

static void counts(hj_t *j, const i2cdev_counts_t *n) {
	hj_obj(j, "counts");
	hj_int(j, "ok", n->ok);
	hj_int(j, "nack", n->nack);
	hj_int(j, "timeout", n->timeout);
	hj_int(j, "recovered", n->recovered);
	hj_int(j, "stuck", n->stuck);
	hj_obj_end(j);
}

static void result(void *hj) {
	hj_t *j = hj;
	hj_int(j, "runs", runs);
	if (bus_e != I2CDEV_OK) hj_str(j, "bus", i2cdev_err_name(bus_e));
	if (!pd.done) return;
	hj_obj(j, "ch224");
	hj_str(j, "probe", i2cdev_err_name(pd.probe));
	hj_str(j, "check", i2cdev_err_name(pd.check_e));
	if (pd.why[0]) hj_str(j, "why", pd.why);
	if (pd.status_e == I2CDEV_OK) {
		hex(j, "status", pd.status);
		hj_bool(j, "handshake", (pd.status & 0x1f) != 0);
		hj_bool(j, "pd", (pd.status & 0x08) != 0);
		hj_bool(j, "bit7", (pd.status & 0x80) != 0); /* not in the datasheet's table */
	} else
		hj_str(j, "status_err", i2cdev_err_name(pd.status_e));
	if (pd.current_e == I2CDEV_OK) {
		hex(j, "current", pd.current);
		hj_int(j, "current_ma", pd.current * 50);
	} else
		hj_str(j, "current_err", i2cdev_err_name(pd.current_e));
	counts(j, &pd.n);
	hj_obj_end(j);
	if (!codec.done) return;
	hj_obj(j, "codec");
	hj_str(j, "probe", i2cdev_err_name(codec.probe));
	hj_str(j, "regs_read", i2cdev_err_name(codec.regs_e));
	if (codec.regs_e == I2CDEV_OK) {
		hj_arr(j, "regs");
		for (int i = 0; i < CODEC_REGS; i++) {
			char s[8];
			snprintf(s, sizeof(s), "0x%02x", codec.regs[i]);
			hj_str(j, NULL, s);
		}
		hj_arr_end(j);
	}
	counts(j, &codec.n);
	hj_obj_end(j);
}

const hifi_lab_exp_t lab_i2c = {
	.name = "i2c",
	.help = "read the board's I2C chips (CH224 status and current, the codec's control registers); writes nothing",
	.start = start,
	.stop = stop,
	.result = result,
};
