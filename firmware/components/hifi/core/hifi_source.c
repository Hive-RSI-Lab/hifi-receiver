#include "hifi_source.h"

bool hifi_source_is_phone(int src) {
	return src == HIFI_SRC_BT || src == HIFI_SRC_RAOP || src == HIFI_SRC_CSPOT;
}

int hifi_source_decide(int owner, bool busy, int own, int ev) {
	if (owner == own) return HIFI_SRC_ALLOW;
	switch (ev) {
	case HIFI_EV_START:
		if (hifi_source_is_phone(owner)) return HIFI_SRC_TAKEOVER;
		return busy ? HIFI_SRC_REFUSE : HIFI_SRC_ALLOW;
	case HIFI_EV_OWN:
		return owner == HIFI_SRC_NONE && !busy ? HIFI_SRC_ALLOW : HIFI_SRC_REFUSE;
	case HIFI_EV_PLAY:
		return busy && !hifi_source_is_phone(owner) ? HIFI_SRC_REFUSE : HIFI_SRC_IGNORE;
	case HIFI_EV_SESSION:
	default:
		return HIFI_SRC_IGNORE;
	}
}
