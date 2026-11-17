/* The LAN test API as the HTTP server calls it: byte 0 picks the method
 * and whether the board has every optional op (bit 2) and an OTA of 1024
 * bytes already open (bit 3, so chunk and finish are reached); then the
 * path, a newline,
 * and the body (no newline: no body). The reply buffer is exactly
 * hifi_api_reply_cap's size, as hifi_http.c allocates it, so ASan sees a
 * write past it; every reply must be one JSON object. */
#include "fuzz_board.h"
#include "hifi_json.h"

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
	if (n < 1) return 0;
	static const char *methods[] = {"GET", "POST", "PUT", "DELETE"};
	const char *method = methods[d[0] & 3];
	const hifi_ops_t *ops = fuzz_ops(d[0] & 4);
	const char *err;
	if (d[0] & 8 && ops->ota) hifi_ota_begin(ops->ota, 1024, FUZZ_SHA, "fuzz", &err);
	char *s = fuzz_str(d + 1, n - 1);
	char *nl = strchr(s, '\n'), *body = NULL;
	if (nl) *nl = '\0', body = nl + 1;
	size_t cap = hifi_api_reply_cap(s, body);
	char *out = malloc(cap);
	int st = hifi_api_handle(ops, method, s, body, out, cap);
	FUZZ_ASSERT(st >= 100 && st < 600);
	FUZZ_ASSERT(strlen(out) < cap);
	FUZZ_ASSERT(hj_valid_object(out));
	free(out);
	free(s);
	return 0;
}
