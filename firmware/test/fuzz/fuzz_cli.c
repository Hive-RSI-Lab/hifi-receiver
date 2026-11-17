/* The serial console, as cmd_hifi in hifi_main.c runs `hifi <args>`: the
 * input split on spaces (byte 0 picks the board), then hifi_cli_request
 * into a 256-byte body and, if it parses, the API into a 4096-byte reply;
 * `hifi i2c ...` goes to hifi_i2c_cli against a bus that acks every
 * second address. */
#include "fuzz_board.h"
#include "hifi_json.h"

static int i2c_rd(uint8_t a, uint8_t r, uint8_t *v) {
	*v = (uint8_t) (a ^ r);
	return a & 1 ? -1 : 0;
}
static int i2c_wr(uint8_t a, uint8_t r, uint8_t v) { return (void) r, (void) v, a & 1 ? -1 : 0; }

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
	if (n < 1) return 0;
	const hifi_ops_t *ops = fuzz_ops(d[0] & 1);
	const char *busy = d[0] & 2 ? "audio plays" : NULL;
	char *s = fuzz_str(d + 1, n - 1);
	const char *argv[32];
	int argc = 0;
	for (char *t = strtok(s, " "); t && argc < 32; t = strtok(NULL, " ")) argv[argc++] = t;
	enum { CAP = 4096 };
	char *out = malloc(CAP), *body = malloc(256);
	out[0] = '\0';
	if (argc >= 1 && !strcmp(argv[0], "i2c")) {
		hifi_i2c_cli(argc, argv, busy, i2c_rd, i2c_wr, out, CAP);
		FUZZ_ASSERT(hj_valid_object(out));
	} else {
		const char *method, *path;
		if (hifi_cli_request(argc, argv, &method, &path, body, 256, out, CAP)) {
			FUZZ_ASSERT(method && path && strlen(body) < 256);
			hifi_api_handle(ops, method, path, body, out, CAP);
			FUZZ_ASSERT(hj_valid_object(out));
		} else {
			FUZZ_ASSERT(out[0]); /* a console that refuses says how to use it */
		}
	}
	FUZZ_ASSERT(strlen(out) < CAP);
	free(out);
	free(body);
	free(s);
	return 0;
}
