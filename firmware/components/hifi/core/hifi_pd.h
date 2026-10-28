/*
 * The USB-C power contract, from WCH's CH224 PD sink and the VBUS reading.
 *
 * Registers from the CH224 datasheet V2.0, section 5 (I2C address 0x22):
 *   0x09 status (read): bit0 BC, bit1 QC2, bit2 QC3, bit3 PD, bit4 EPR
 *        handshake done; bit5 the source offers EPR; bit6 it offers AVS.
 *   0x0A voltage request (write-only), 0x50 max current in this contract in
 *   50 mA units (PD only), 0x60-0x8F the source's capabilities (CH224Q only;
 *   the CH224A on lite-main has no such area).
 * The firmware never writes 0x0A: the board's Rset picks the voltage.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define CH224_I2C_ADDR 0x22
#define CH224_REG_STATUS 0x09
#define CH224_REG_CURRENT 0x50

typedef enum {
	HIFI_PROTO_NONE = 0,
	HIFI_PROTO_BC,
	HIFI_PROTO_QC2,
	HIFI_PROTO_QC3,
	HIFI_PROTO_PD,
} hifi_proto_t;

typedef struct {
	hifi_proto_t proto;
	bool epr;      /* in an EPR (28 V+) contract */
	float v;       /* nominal volts: VBUS snapped to a fixed PD level */
	float i_max_a; /* NaN when the sink can't say (anything but PD) */
	float w;       /* v * i_max_a, or the protocol's safe assumption */
	bool known;    /* false until the sink has answered on I2C */
} hifi_contract_t;

const char *hifi_proto_name(hifi_proto_t p);

/* The highest protocol whose handshake bit is set. */
hifi_proto_t hifi_ch224_protocol(uint8_t status);

/* VBUS snapped to 5/9/12/15/20/28 V when within 8% of one, else as read. */
float hifi_pd_nominal_v(float vbus_v);

/*
 * Builds the contract. `sink_ok` false means the CH224 didn't answer: the
 * contract is then unknown, and its power is what a USB 2.0 port guarantees.
 * When the source doesn't say its current (QC, BC), the power assumed is the
 * protocol's floor: 18 W for QC, 7.5 W for BC/none.
 */
void hifi_contract_from(bool sink_ok, uint8_t status, uint8_t current_reg,
			float vbus_v, hifi_contract_t *out);
