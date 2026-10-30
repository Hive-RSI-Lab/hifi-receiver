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

static int fixed_pdos(const uint8_t *p, int words, float *v, float *i, int max) {
	int n = 0;
	for (int k = 0; k < words; k++) {
		uint32_t pdo = p[4 * k] | p[4 * k + 1] << 8 | (uint32_t) p[4 * k + 2] << 16 | (uint32_t) p[4 * k + 3] << 24;
		if (k == 0 && ((pdo >> 30) != 0 || ((pdo >> 10) & 0x3ff) != 100)) return 0; /* not 5 V first */
		if (pdo == 0) break;
		if ((pdo >> 30) != 0) continue; /* PPS, AVS, battery, variable */
		if (n < max) {
			v[n] = ((pdo >> 10) & 0x3ff) * 0.05f;
			i[n] = (pdo & 0x3ff) * 0.01f;
			n++;
		}
	}
	return n;
}

int hifi_pd_srccap_fixed(const uint8_t *raw, int len, float *v, float *i, int max) {
	if (len >= 6) {
		uint16_t hdr = raw[0] | raw[1] << 8;
		int words = (hdr >> 12) & 7;
		if ((hdr & 0x1f) == 1 && words > 0 && 2 + 4 * words <= len) { /* Source_Capabilities */
			int n = fixed_pdos(raw + 2, words, v, i, max);
			if (n) return n;
		}
	}
	return fixed_pdos(raw, len / 4 < 7 ? len / 4 : 7, v, i, max);
}

float hifi_ch224_request(uint8_t status, const float *fixed_v, int n, uint8_t *code) {
	if (!(status & 0x18)) return 0; /* no PD contract yet */
	if (status & (CH224_ST_EPR_OFFERED | 0x10)) {
		*code = 5;
		return 28.0f;
	}
	if (n <= 0) {
		*code = 4;
		return 20.0f;
	}
	static const float levels[] = {20, 15, 12, 9};
	for (unsigned k = 0; k < sizeof(levels) / sizeof(levels[0]); k++) {
		for (int j = 0; j < n; j++) {
			if (fabsf(fixed_v[j] - levels[k]) < 0.3f) {
				*code = (uint8_t) (4 - k);
				return levels[k];
			}
		}
	}
	*code = 0;
	return 5.0f;
}

float hifi_ch224_vbus(uint8_t status, float requested) {
	if (!(status & 0x18)) return 5.0f;
	if (status & 0x10) return 28.0f;
	return requested > 0 && requested < 28.0f ? requested : 20.0f;
}

bool hifi_ch224_settled(uint8_t status, uint32_t ms_since_pd, uint32_t epr_wait_ms) {
	if (!(status & 0x18)) return false;
	if ((status & CH224_ST_EPR_OFFERED) && !(status & 0x10)) return ms_since_pd >= epr_wait_ms;
	return true;
}
