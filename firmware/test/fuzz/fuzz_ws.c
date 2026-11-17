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
			hifi_ws_head_parse(d, &h);
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
