/* The relay's own websocket client on what a server (or anyone on the
 * path) sends back: frame headers, the upgrade response, and URLs. */
#include "fuzz_board.h"
#include "hifi_ws_proto.h"

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
	if (n >= 2) {
		size_t hl = hifi_ws_head_len(d);
		FUZZ_ASSERT(hl >= 2 && hl <= HIFI_WS_HEAD_MAX);
		if (hl <= n) {
			hifi_ws_head_t h;
			if (hifi_ws_head_parse(d, &h) == 0) {
				/* what it accepts has no reserved bits and, for a control frame,
				 * is whole and at most 125 bytes (hifi_ws.c drains other ops);
				 * the length is the one the header's own length field gives */
				FUZZ_ASSERT(!(d[0] & 0x70) && h.op == (d[0] & 15) && h.masked == !!(d[1] & 0x80));
				FUZZ_ASSERT(h.op < 8 || (h.fin && h.len <= 125));
				FUZZ_ASSERT((d[1] & 0x7f) < 126 ? h.len == (d[1] & 0x7fu) : (d[1] & 0x7f) == 127 || h.len < 65536);
			}
		}
	}
	char *s = fuzz_str(d, n);
	hifi_ws_upgrade_ok(s, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
	hifi_ws_key_ok(s);
	char host[HIFI_WS_HOST_MAX], path[HIFI_WS_PATH_MAX];
	int port;
	if (hifi_ws_url_parse(s, host, &port, path) == 0)
		FUZZ_ASSERT(strlen(host) < HIFI_WS_HOST_MAX && strlen(path) < HIFI_WS_PATH_MAX && port > 0 && port < 65536);
	free(s);
	return 0;
}
