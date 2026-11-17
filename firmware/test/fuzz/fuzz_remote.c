/* The relay's signed envelope as hifi_relay.c's worker runs it: a reply
 * buffer of exactly hifi_remote_reply_cap, a board with every op. The
 * verifier passes a signature that starts with 0x5a (any other fails), so
 * the fuzzer gets past the signature into the envelope's checks and the API;
 * byte 0 picks the board's clock and whether it has a key at all. Also the
 * base64 the signature comes in, round trip. */
#include "fuzz_board.h"
#include "hifi_b64.h"
#include "hifi_json.h"
#include "hifi_remote.h"

static bool verify(const uint8_t pub[32], const uint8_t *msg, size_t n, const uint8_t sig[64]) {
	(void) pub, (void) msg, (void) n;
	return sig[0] == 0x5a;
}

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
	if (n < 1) return 0;
	hifi_remote_t rem = {.verify = verify};
	snprintf(rem.dev, sizeof(rem.dev), "240ac4123456");
	if (!(d[0] & 1)) hifi_remote_set_keys(&rem, "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
	rem.last_id = d[0] & 2 ? 1761700000000ull : 0;
	int64_t now = d[0] & 4 ? 1761700000 : 0;
	const hifi_ops_t *ops = fuzz_ops(true);

	/* not NUL-terminated: the relay hands over a length */
	char *msg = malloc(n - 1 ? n - 1 : 1);
	memcpy(msg, d + 1, n - 1);
	size_t cap = hifi_remote_reply_cap(msg, n - 1);
	char *out = malloc(cap);
	int st = hifi_remote_handle(&rem, ops, msg, n - 1, now, out, cap);
	FUZZ_ASSERT(st >= 100 && st < 600);
	FUZZ_ASSERT(strlen(out) < cap);
	FUZZ_ASSERT(hj_valid_object(out));
	FUZZ_ASSERT(rem.accepted + rem.rejected == 1);
	free(out);

	uint8_t raw[96];
	long got = hifi_b64_decode(msg, n - 1, raw, sizeof(raw));
	if (got > 0) {
		char enc[160];
		size_t el = hifi_b64_encode(raw, (size_t) got, enc, sizeof(enc));
		uint8_t back[96];
		FUZZ_ASSERT(el && hifi_b64_decode(enc, el, back, sizeof(back)) == got && !memcmp(raw, back, (size_t) got));
	}
	free(msg);
	return 0;
}
