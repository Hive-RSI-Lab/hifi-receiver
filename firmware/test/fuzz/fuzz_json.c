/* hifi_json's readers on any text (byte 0 picks the key, the rest is the
 * document), the writer on any string, and the update manifest parser.
 * Properties: any string the writer quotes (control characters and UTF-8
 * included) reads back the same, and what the writer makes is one valid
 * object. */
#include "fuzz_board.h"
#include "hifi_json.h"
#include "hifi_update.h"

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
	if (n < 1) return 0;
	static const char *keys[] = {"id", "body", "volume", "url", "a", "", "sha256", "latest"};
	const char *key = keys[d[0] & 7];
	char *s = fuzz_str(d + 1, n - 1);
	size_t sl = strlen(s);

	char str[16];
	double num, arr[8];
	bool b;
	const char *v;
	size_t vl;
	if (hj_get_str(s, key, str, sizeof(str))) FUZZ_ASSERT(strlen(str) < sizeof(str));
	hj_get_num(s, key, &num);
	hj_get_bool(s, key, &b);
	hj_valid_object(s);
	FUZZ_ASSERT(hj_get_num_array(s, key, arr, 8) <= 8);
	if (hj_get_raw(s, key, &v, &vl)) FUZZ_ASSERT(v >= s && v + vl <= s + sl);
	if (hj_get_str_span(s, key, &v, &vl)) FUZZ_ASSERT(v >= s && v + vl <= s + sl);

	/* the writer, into a buffer that may be too small */
	size_t cap = (d[0] >> 3) * 16 + 2;
	char *w = malloc(cap);
	hj_t j;
	hj_init(&j, w, cap);
	hj_obj(&j, NULL);
	hj_str(&j, key[0] ? key : "k", s);
	hj_obj(&j, "o");
	hj_num(&j, "n", (double) n * 0.37, d[0] % 5);
	hj_obj_end(&j);
	hj_obj_end(&j);
	FUZZ_ASSERT(strlen(w) < cap);
	if (!j.overflow) {
		FUZZ_ASSERT(hj_valid_object(w));
		char back[256];
		if (sl < sizeof(back)) {
			FUZZ_ASSERT(hj_get_str(w, key[0] ? key : "k", back, sizeof(back)));
			FUZZ_ASSERT(!strcmp(back, s));
		}
	}
	free(w);

	hifi_update_offer_t o;
	const char *err;
	if (hifi_update_parse(s, d[0] & 1 ? "http://h:1/fw/m.json" : NULL, "hifi-lite", &o, &err) == 0)
		FUZZ_ASSERT(strlen(o.url) < sizeof(o.url) && strlen(o.latest) < sizeof(o.latest));
	free(s);
	return 0;
}
