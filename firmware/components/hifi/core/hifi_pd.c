#include "hifi_pd.h"

#include <math.h>

const char *hifi_proto_name(hifi_proto_t p) {
	switch (p) {
	case HIFI_PROTO_BC: return "bc";
	case HIFI_PROTO_QC2: return "qc2";
	case HIFI_PROTO_QC3: return "qc3";
	case HIFI_PROTO_PD: return "pd";
	default: return "none";
	}
}

hifi_proto_t hifi_ch224_protocol(uint8_t status) {
	if (status & 0x18) return HIFI_PROTO_PD; /* PD or EPR handshake */
	if (status & 0x04) return HIFI_PROTO_QC3;
	if (status & 0x02) return HIFI_PROTO_QC2;
	if (status & 0x01) return HIFI_PROTO_BC;
	return HIFI_PROTO_NONE;
}

float hifi_pd_nominal_v(float vbus_v) {
	static const float levels[] = {5, 9, 12, 15, 20, 28};
	for (unsigned i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
		if (fabsf(vbus_v - levels[i]) <= 0.08f * levels[i]) return levels[i];
	}
	return roundf(vbus_v * 10) / 10;
}

void hifi_contract_from(bool sink_ok, uint8_t status, uint8_t current_reg,
			float vbus_v, hifi_contract_t *out) {
	out->known = sink_ok;
	out->proto = sink_ok ? hifi_ch224_protocol(status) : HIFI_PROTO_NONE;
	out->epr = sink_ok && (status & 0x10);
	out->v = hifi_pd_nominal_v(vbus_v);
	out->i_max_a = NAN;
	if (out->proto == HIFI_PROTO_PD && current_reg) out->i_max_a = current_reg * 0.05f;

	if (!isnan(out->i_max_a)) out->w = out->v * out->i_max_a;
	else if (out->proto == HIFI_PROTO_QC2 || out->proto == HIFI_PROTO_QC3) out->w = 18.0f;
	else out->w = 7.5f;
}
