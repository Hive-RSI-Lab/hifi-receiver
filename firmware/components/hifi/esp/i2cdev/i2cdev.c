/* i2cdev: the driver over any bus (plain C, host-tested); see include/i2cdev.h */
#include "i2cdev.h"

#include <stdio.h>
#include <string.h>

#define DEFAULT_TIMEOUT_MS 50
#define HALF_CLOCK_US 5 /* 100 kHz while recovering, slow enough for any chip */
#define NACK_WAIT_US 5000 /* before a NACK's retry: an EEPROM-style write cycle is up to 5 ms */
#define STOP_TRIES 3      /* a chip still mid-read can take SDA again as SCL falls for the STOP */

const char *i2cdev_err_name(int e) {
	switch (e) {
	case I2CDEV_OK: return "ok";
	case I2CDEV_NACK: return "nack";
	case I2CDEV_TIMEOUT: return "timeout";
	case I2CDEV_STUCK: return "stuck";
	case I2CDEV_ARG: return "arg";
	case I2CDEV_BUS: return "bus";
	case I2CDEV_DIFF: return "diff";
	default: return "?";
	}
}

int i2cdev_init(i2cdev_dev_t *d, const i2cdev_bus_t *bus, uint8_t addr, uint32_t timeout_ms) {
	if (!d || !bus || !bus->xfer || addr < I2CDEV_ADDR_LO || addr > I2CDEV_ADDR_HI) return I2CDEV_ARG;
	memset(d, 0, sizeof(*d));
	d->bus = bus;
	d->addr = addr;
	d->timeout_ms = timeout_ms ? timeout_ms : DEFAULT_TIMEOUT_MS;
	return I2CDEV_OK;
}

static void wait(const i2cdev_bus_t *b, uint32_t us) {
	if (b->wait_us) b->wait_us(b->ctx, us);
}

static int reset(const i2cdev_bus_t *b) {
	if (!b->reset) return I2CDEV_OK;
	return b->reset(b->ctx) == I2CDEV_OK ? I2CDEV_OK : I2CDEV_BUS;
}

int i2cdev_recover(const i2cdev_bus_t *b) {
	if (!b) return I2CDEV_ARG;
	if (!b->line_set || !b->line_get) return reset(b);
	b->line_set(b->ctx, I2CDEV_SDA, 1);
	b->line_set(b->ctx, I2CDEV_SCL, 1);
	wait(b, HALF_CLOCK_US);
	int stuck = 0, clocks = 0;
	if (!b->line_get(b->ctx, I2CDEV_SCL)) stuck = 1; /* SCL held or shorted: clocking can't help */
	for (int t = 0; !stuck && t < STOP_TRIES; t++) {
		/* a chip stopped mid-read holds SDA low for its next 0 bit: clock it
		 * out, a bit at a time, until it lets go (9 clocks in all) */
		for (; clocks < I2CDEV_RECOVER_CLOCKS && !b->line_get(b->ctx, I2CDEV_SDA); clocks++) {
			b->line_set(b->ctx, I2CDEV_SCL, 0);
			wait(b, HALF_CLOCK_US);
			b->line_set(b->ctx, I2CDEV_SCL, 1);
			wait(b, HALF_CLOCK_US);
			if (!b->line_get(b->ctx, I2CDEV_SCL)) {
				stuck = 1;
				break;
			}
		}
		if (stuck || !b->line_get(b->ctx, I2CDEV_SDA)) break;
		/* a STOP (SDA rising while SCL is high) so every chip is idle */
		b->line_set(b->ctx, I2CDEV_SCL, 0);
		wait(b, HALF_CLOCK_US);
		b->line_set(b->ctx, I2CDEV_SDA, 0);
		wait(b, HALF_CLOCK_US);
		b->line_set(b->ctx, I2CDEV_SCL, 1);
		wait(b, HALF_CLOCK_US);
		b->line_set(b->ctx, I2CDEV_SDA, 1);
		wait(b, HALF_CLOCK_US);
		if (b->line_get(b->ctx, I2CDEV_SCL) && b->line_get(b->ctx, I2CDEV_SDA)) break;
		/* SDA low again: the chip took it as SCL fell; clock on and try again */
	}
	if (!b->line_get(b->ctx, I2CDEV_SCL) || !b->line_get(b->ctx, I2CDEV_SDA)) stuck = 1;
	int r = reset(b); /* the lines go back to the controller either way */
	if (stuck) return I2CDEV_STUCK;
	return r;
}

/* One transaction with the driver's policy: a NACK is tried again
 * nack_retries times; a TIMEOUT gets one recovery and one more try. */
static int xfer(i2cdev_dev_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn) {
	const i2cdev_bus_t *b = d->bus;
	unsigned nacks = 0;
	int recovered = 0;
	for (;;) {
		int e = b->xfer(b->ctx, d->addr, w, wn, r, rn, d->timeout_ms);
		switch (e) {
		case I2CDEV_OK:
			d->n.ok++;
			return I2CDEV_OK;
		case I2CDEV_NACK:
			d->n.nack++;
			if (nacks++ < d->nack_retries) {
				wait(b, NACK_WAIT_US);
				continue;
			}
			return I2CDEV_NACK;
		case I2CDEV_TIMEOUT:
			d->n.timeout++;
			if (recovered) return I2CDEV_TIMEOUT;
			recovered = 1;
			if (!b->reset && !b->line_set) continue; /* a bus another driver owns: it resets itself; one more try */
			e = i2cdev_recover(b);
			if (e == I2CDEV_STUCK) {
				d->n.stuck++;
				return I2CDEV_STUCK;
			}
			if (e != I2CDEV_OK) return I2CDEV_BUS;
			d->n.recovered++;
			continue;
		default:
			return I2CDEV_BUS;
		}
	}
}

int i2cdev_probe(i2cdev_dev_t *d) {
	if (!d || !d->bus) return I2CDEV_ARG;
	return xfer(d, NULL, 0, NULL, 0);
}

int i2cdev_read(i2cdev_dev_t *d, uint8_t reg, uint8_t *buf, size_t n) {
	if (!d || !d->bus || !buf || n == 0 || n > I2CDEV_MAX) return I2CDEV_ARG;
	uint8_t tmp[I2CDEV_MAX];
	int e = xfer(d, &reg, 1, tmp, n);
	if (e == I2CDEV_OK) memcpy(buf, tmp, n);
	return e;
}

int i2cdev_read_u8(i2cdev_dev_t *d, uint8_t reg, uint8_t *v) { return i2cdev_read(d, reg, v, 1); }

int i2cdev_write(i2cdev_dev_t *d, uint8_t reg, const uint8_t *buf, size_t n) {
	if (!d || !d->bus || !buf || n == 0 || n > I2CDEV_MAX) return I2CDEV_ARG;
	uint8_t tmp[1 + I2CDEV_MAX];
	tmp[0] = reg;
	memcpy(tmp + 1, buf, n);
	return xfer(d, tmp, 1 + n, NULL, 0);
}

int i2cdev_write_u8(i2cdev_dev_t *d, uint8_t reg, uint8_t v) { return i2cdev_write(d, reg, &v, 1); }

int i2cdev_update(i2cdev_dev_t *d, uint8_t reg, uint8_t mask, uint8_t val) {
	uint8_t v;
	int e = i2cdev_read_u8(d, reg, &v);
	if (e != I2CDEV_OK) return e;
	uint8_t nv = (uint8_t) ((v & ~mask) | (val & mask));
	return nv == v ? I2CDEV_OK : i2cdev_write_u8(d, reg, nv);
}

int i2cdev_check(i2cdev_dev_t *d, const i2cdev_expect_t *e, size_t n, char *why, size_t cap) {
	if (why && cap) why[0] = 0;
	if (!d || (!e && n)) return I2CDEV_ARG;
	for (size_t i = 0; i < n; i++) {
		uint8_t v;
		const char *what = e[i].what ? e[i].what : "register";
		int r = i2cdev_read_u8(d, e[i].reg, &v);
		if (r != I2CDEV_OK) {
			if (why && cap) snprintf(why, cap, "%s (0x%02x): %s", what, e[i].reg, i2cdev_err_name(r));
			return r;
		}
		if ((v & e[i].mask) != (e[i].want & e[i].mask)) {
			if (why && cap)
				snprintf(why, cap, "%s (0x%02x) read 0x%02x, & 0x%02x should be 0x%02x", what, e[i].reg, v,
					 e[i].mask, e[i].want & e[i].mask);
			return I2CDEV_DIFF;
		}
	}
	return I2CDEV_OK;
}

size_t i2cdev_counts_json(const i2cdev_dev_t *d, char *out, size_t cap) {
	if (!out || !cap) return 0;
	out[0] = 0;
	if (!d) return 0;
	int n = snprintf(out, cap,
			 "{\"addr\":\"0x%02x\",\"ok\":%lu,\"nack\":%lu,\"timeout\":%lu,\"recovered\":%lu,\"stuck\":%lu}",
			 d->addr, (unsigned long) d->n.ok, (unsigned long) d->n.nack, (unsigned long) d->n.timeout,
			 (unsigned long) d->n.recovered, (unsigned long) d->n.stuck);
	if (n < 0 || (size_t) n >= cap) {
		out[0] = 0;
		return 0;
	}
	return (size_t) n;
}
