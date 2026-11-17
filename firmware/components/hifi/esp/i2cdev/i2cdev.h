/*
 * i2cdev: one chip on an I2C bus, with the bus behind an interface.
 *
 * Plain C, no IDF: the host test runs it against i2cdev_mock.c (a bus with
 * chips, NACKs, timeouts and a held SDA line), and i2cdev_esp.c is the bus
 * on a board. Registers are 8-bit addresses, auto-incrementing, the way
 * most codecs, PD controllers, sensors and port expanders work: a write is
 * START addr+W reg data... STOP, a read is START addr+W reg, a repeated
 * START, addr+R data... STOP.
 *
 * Not thread-safe: callers serialise use of one bus (on a shared bus, the
 * bus layer's lock or the caller's own). Recovery takes the lines from the
 * controller for about 100 us, so nothing else may use the bus meanwhile.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define I2CDEV_MAX 32		/* the most data bytes one read or write moves after the register */
#define I2CDEV_ADDR_LO 0x08	/* 7-bit addresses outside 0x08-0x77 are reserved by the I2C spec */
#define I2CDEV_ADDR_HI 0x77
#define I2CDEV_RECOVER_CLOCKS 9 /* a chip mid-byte lets SDA go within 9 clocks (8 bits and the ack) */

typedef enum {
	I2CDEV_OK = 0,
	I2CDEV_NACK,	 /* nobody answered at the address, or the chip refused a byte */
	I2CDEV_TIMEOUT, /* the transaction didn't end in time: a held line, a stuck controller */
	I2CDEV_STUCK,	 /* a line stayed low after recovery: SDA held by a chip, or SCL held or shorted */
	I2CDEV_ARG,	 /* a bad call: no buffer, a length of 0 or over I2CDEV_MAX, an address outside 0x08-0x77 */
	I2CDEV_BUS,	 /* the bus layer failed some other way (not set up, out of memory) */
	I2CDEV_DIFF,	 /* i2cdev_check: a register read fine but isn't what the datasheet says */
} i2cdev_err_t;

enum { I2CDEV_SCL = 0, I2CDEV_SDA = 1 };

/*
 * What the driver needs from a bus.
 *
 * xfer: one transaction. START, addr+W, w[0..wn), then when rn > 0 a
 *   repeated START, addr+R, r[0..rn), then STOP. wn 0 with rn 0 is an
 *   address-only probe. Returns I2CDEV_OK, NACK, TIMEOUT or BUS.
 * line_set, line_get, wait_us: the lines as open-drain GPIO, for recovery
 *   (high = let go, low = pull down). NULL when the bus can't hand its lines
 *   over: recovery is then only reset.
 * reset: give the lines back to the controller and reset it. NULL if none.
 */
typedef struct {
	int (*xfer)(void *ctx, uint8_t addr, const uint8_t *w, size_t wn, uint8_t *r, size_t rn,
		    uint32_t timeout_ms);
	void (*line_set)(void *ctx, int line, int high);
	int (*line_get)(void *ctx, int line);
	void (*wait_us)(void *ctx, uint32_t us);
	int (*reset)(void *ctx);
	void *ctx;
} i2cdev_bus_t;

typedef struct {
	uint32_t ok, nack, timeout, recovered, stuck;
} i2cdev_counts_t;

typedef struct {
	const i2cdev_bus_t *bus;
	uint8_t addr;
	uint32_t timeout_ms;
	uint8_t nack_retries; /* a NACK is tried again this many times (a chip busy with its own write); 0 */
	i2cdev_counts_t n;
} i2cdev_dev_t;

/* One register the datasheet fixes (a chip ID, a reserved bit pattern):
 * (value & mask) must be want. `what` names it in the reason. */
typedef struct {
	uint8_t reg, mask, want;
	const char *what;
} i2cdev_expect_t;

/* Sets d up for the chip at addr: I2CDEV_ARG for a NULL, a bus without
 * xfer or an address outside 0x08-0x77. timeout_ms 0 means 50 ms. Sends
 * nothing. */
int i2cdev_init(i2cdev_dev_t *d, const i2cdev_bus_t *bus, uint8_t addr, uint32_t timeout_ms);

/* An address-only transaction: OK when the chip acks, NACK when nobody does.
 * Writes nothing to the chip. */
int i2cdev_probe(i2cdev_dev_t *d);

/* n bytes from reg on (1..I2CDEV_MAX); buf is left as it was unless OK. */
int i2cdev_read(i2cdev_dev_t *d, uint8_t reg, uint8_t *buf, size_t n);
int i2cdev_read_u8(i2cdev_dev_t *d, uint8_t reg, uint8_t *v);

/* n bytes to reg on (1..I2CDEV_MAX). */
int i2cdev_write(i2cdev_dev_t *d, uint8_t reg, const uint8_t *buf, size_t n);
int i2cdev_write_u8(i2cdev_dev_t *d, uint8_t reg, uint8_t v);

/* Reads reg, sets the bits in mask to val's, and writes it back only when
 * that changes it. */
int i2cdev_update(i2cdev_dev_t *d, uint8_t reg, uint8_t mask, uint8_t val);

/* Reads each register in e and compares it: OK, I2CDEV_DIFF for the first
 * that differs (why says which, what it read and what it should be), or the
 * read's error. Only reads: the identity check a HIL case can run on a
 * product board without changing it. why may be NULL. */
int i2cdev_check(i2cdev_dev_t *d, const i2cdev_expect_t *e, size_t n, char *why, size_t cap);

/*
 * Frees a held bus: lets both lines go, clocks SCL (up to 9 times) until
 * the chip holding SDA lets go, then sends a STOP and calls reset. OK when
 * both lines are high after it, STUCK when one stays low. Reads and writes
 * call it themselves after a TIMEOUT and try once more.
 */
int i2cdev_recover(const i2cdev_bus_t *bus);

/* "ok", "nack", "timeout", ...; "?" for anything else */
const char *i2cdev_err_name(int e);

/* d's counts as JSON: {"addr":"0x22","ok":3,"nack":0,...}; the length, or 0
 * when cap is too small (out is then ""). */
size_t i2cdev_counts_json(const i2cdev_dev_t *d, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
