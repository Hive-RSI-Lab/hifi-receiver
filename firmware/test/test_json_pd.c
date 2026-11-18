#include "check.h"
#include "hifi_json.h"
#include "hifi_pd.h"

static void writer(void) {
	char buf[256];
	hj_t j;
	hj_init(&j, buf, sizeof(buf));
	hj_obj(&j, NULL);
	hj_str(&j, "s", "a\"b\\c\n");
	hj_num(&j, "n", 1.236, 2);
	hj_num(&j, "nan", NAN, 2);
	hj_num(&j, "negzero", -0.001, 1);
	hj_int(&j, "i", -7);
	hj_bool(&j, "b", true);
	hj_arr(&j, "a");
	hj_int(&j, NULL, 1);
	hj_obj(&j, NULL);
	hj_null(&j, "x");
	hj_obj_end(&j);
	hj_arr_end(&j);
	hj_str(&j, "null", NULL);
	hj_obj_end(&j);
	CHECK(!j.overflow);
	CHECK_STR(buf, "{\"s\":\"a\\\"b\\\\c\\u000a\",\"n\":1.24,\"nan\":null,\"negzero\":0.0,\"i\":-7,"
		       "\"b\":true,\"a\":[1,{\"x\":null}],\"null\":null}");
	CHECK(hj_valid_object(buf));

	/* overflow never writes past the buffer */
	char small[8];
	hj_init(&j, small, sizeof(small));
	hj_obj(&j, NULL);
	hj_str(&j, "long", "value");
	hj_obj_end(&j);
	CHECK(j.overflow);
	CHECK(strlen(small) < sizeof(small));
}

static void reader(void) {
	const char *body = " {\"type\": \"sine\", \"nested\": {\"freq_hz\": 5, \"a\": [1, {\"b\": 2}]},"
			   " \"freq_hz\": 440.5, \"neg\": -20, \"force\": true, \"off\": false, \"n\": null,"
			   " \"esc\": \"x\\\"y\"} ";
	char s[16];
	double d;
	bool b;
	CHECK(hj_valid_object(body));
	CHECK(hj_get_str(body, "type", s, sizeof(s)));
	CHECK_STR(s, "sine");
	CHECK(hj_get_num(body, "freq_hz", &d)); /* the top-level one, not the nested one */
	CHECK_NEAR(d, 440.5, 1e-9);
	CHECK(hj_get_num(body, "neg", &d));
	CHECK_NEAR(d, -20, 1e-9);
	CHECK(hj_get_bool(body, "force", &b) && b);
	CHECK(hj_get_bool(body, "off", &b) && !b);
	CHECK(!hj_get_num(body, "n", &d));
	CHECK(!hj_get_num(body, "type", &d));
	CHECK(!hj_get_str(body, "missing", s, sizeof(s)));
	CHECK(!hj_get_num(body, "a", &d)); /* nested keys aren't top-level */
	/* strtod reads these, JSON has none of them, and NaN slips every range
	 * check after (found by fuzzing): refused, *out untouched */
	d = 7;
	CHECK(!hj_get_num("{\"n\":-nan}", "n", &d) && d == 7);
	CHECK(!hj_get_num("{\"n\":-inf}", "n", &d) && d == 7);
	CHECK(!hj_get_num("{\"n\":-infinity}", "n", &d) && d == 7);
	CHECK(!hj_get_num("{\"n\":1e999}", "n", &d) && d == 7);
	CHECK(hj_get_num("{\"n\":1e300}", "n", &d) && d == 1e300);
	double arr[3];
	CHECK(hj_get_num_array("{\"a\":[1,-nan,2]}", "a", arr, 3) == -1);
	CHECK(hj_get_num_array("{\"a\":[1,2e999]}", "a", arr, 3) == -1);
	CHECK(hj_get_str(body, "esc", s, sizeof(s)));
	CHECK_STR(s, "x\"y");
	/* truncation keeps a terminated string */
	char tiny[3];
	CHECK(hj_get_str(body, "type", tiny, sizeof(tiny)));
	CHECK_STR(tiny, "si");

	/* every JSON escape decodes (a CR used to come out as "r") */
	char e[32];
	CHECK(hj_get_str("{\"s\":\"a\\rb\"}", "s", e, sizeof(e)) && !strcmp(e, "a\rb"));
	CHECK(hj_get_str("{\"s\":\"a\\nb\\tc\"}", "s", e, sizeof(e)) && !strcmp(e, "a\nb\tc"));
	CHECK(hj_get_str("{\"s\":\"a\\bb\"}", "s", e, sizeof(e)) && !strcmp(e, "a\bb"));
	CHECK(hj_get_str("{\"s\":\"a\\fb\"}", "s", e, sizeof(e)) && !strcmp(e, "a\fb"));
	CHECK(hj_get_str("{\"s\":\"a\\/b\\\\c\\\"d\"}", "s", e, sizeof(e)) && !strcmp(e, "a/b\\c\"d"));
	/* \uXXXX as UTF-8: one, two, three bytes, and a surrogate pair as four */
	CHECK(hj_get_str("{\"s\":\"\\u0041\\u000d\"}", "s", e, sizeof(e)) && !strcmp(e, "A\r"));
	CHECK(hj_get_str("{\"s\":\"caf\\u00E9\"}", "s", e, sizeof(e)) && !strcmp(e, "caf\xc3\xa9"));
	CHECK(hj_get_str("{\"s\":\"\\u20ac\"}", "s", e, sizeof(e)) && !strcmp(e, "\xe2\x82\xac"));
	CHECK(hj_get_str("{\"s\":\"\\ud83d\\ude00!\"}", "s", e, sizeof(e)) && !strcmp(e, "\xf0\x9f\x98\x80!"));
	/* not a C string or not JSON: refused, out left "" */
	strcpy(e, "x");
	CHECK(!hj_get_str("{\"s\":\"a\\u0000b\"}", "s", e, sizeof(e)) && !e[0]);
	CHECK(!hj_get_str("{\"s\":\"a\\qb\"}", "s", e, sizeof(e)) && !e[0]);
	CHECK(!hj_get_str("{\"s\":\"\\u12g4\"}", "s", e, sizeof(e)) && !e[0]);
	CHECK(!hj_get_str("{\"s\":\"\\u12\"}", "s", e, sizeof(e)) && !e[0]);
	CHECK(!hj_get_str("{\"s\":\"\\ude00\"}", "s", e, sizeof(e)) && !e[0]);
	CHECK(!hj_get_str("{\"s\":\"\\ud83dx\"}", "s", e, sizeof(e)) && !e[0]);
	CHECK(!hj_get_str("{\"s\":\"\\ud83d\\u0041\"}", "s", e, sizeof(e)) && !e[0]);
	/* a cut never splits a character, and nothing after it slips in */
	char four[4];
	CHECK(hj_get_str("{\"s\":\"a\\u20acb\"}", "s", four, sizeof(four)) && !strcmp(four, "a"));
	CHECK(hj_get_str("{\"s\":\"ab\\u00e9c\"}", "s", four, sizeof(four)) && !strcmp(four, "ab"));
	/* raw UTF-8, as JSON.stringify sends it, is cut the same way */
	CHECK(hj_get_str("{\"s\":\"ab\xc3\xa9\"}", "s", four, sizeof(four)) && !strcmp(four, "ab"));
	CHECK(hj_get_str("{\"s\":\"a\xe2\x82\xac" "b\"}", "s", four, sizeof(four)) && !strcmp(four, "a"));
	CHECK(hj_get_str("{\"s\":\"\xf0\x9f\x98\x80\"}", "s", four, sizeof(four)) && !strcmp(four, ""));
	char five[5];
	CHECK(hj_get_str("{\"s\":\"ab\xc3\xa9\"}", "s", five, sizeof(five)) && !strcmp(five, "ab\xc3\xa9"));
	/* a stray lead or continuation byte passes through as it came */
	CHECK(hj_get_str("{\"s\":\"a\xc3" "b\x80\"}", "s", e, sizeof(e)) && !strcmp(e, "a\xc3" "b\x80"));
	/* a bad string anywhere refuses the whole text: no field reads as missing */
	CHECK(!hj_valid_object("{\"a\":\"\\q\"}"));
	CHECK(!hj_valid_object("{\"a\":{\"b\":[\"\\u0000\"]}}"));
	CHECK(!hj_valid_object("{\"\\ud800\":1}"));
	CHECK(!hj_valid_object("{\"a\":\"\\u12\"}"));
	CHECK(hj_valid_object("{\"a\":\"\\ud83d\\ude00 \\r\\/\"}"));
	strcpy(e, "x");
	CHECK(!hj_get_str("{\"s\":\"ok\",\"t\":\"\\q\"}", "s", e, sizeof(e)) && !e[0]);

	/* hj_get_str_fit: whole or nothing. A value that doesn't fit says so and
	 * leaves "", never a prefix of itself; escapes count as what they decode to. */
	{
		char f[5];
		CHECK(hj_get_str_fit("{\"s\":\"abcd\"}", "s", f, sizeof(f)) == HJ_STR_OK && !strcmp(f, "abcd"));
		CHECK(hj_get_str_fit("{\"s\":\"abcde\"}", "s", f, sizeof(f)) == HJ_STR_CUT && !f[0]);
		/* six bytes on the wire, four once decoded: it fits */
		CHECK(hj_get_str_fit("{\"s\":\"a\\rb\\/\"}", "s", f, sizeof(f)) == HJ_STR_OK && !strcmp(f, "a\rb/"));
		/* each of these is short on the wire or in characters but too long decoded */
		CHECK(hj_get_str_fit("{\"s\":\"abc\\r\\n\"}", "s", f, sizeof(f)) == HJ_STR_CUT && !f[0]);
		CHECK(hj_get_str_fit("{\"s\":\"abc\\u00e9\"}", "s", f, sizeof(f)) == HJ_STR_CUT && !f[0]);
		CHECK(hj_get_str_fit("{\"s\":\"\\ud83d\\ude00\"}", "s", f, sizeof(f)) == HJ_STR_OK && !strcmp(f, "\xf0\x9f\x98\x80"));
		CHECK(hj_get_str_fit("{\"s\":\"a\\ud83d\\ude00\"}", "s", f, sizeof(f)) == HJ_STR_CUT && !f[0]);
		CHECK(hj_get_str_fit("{\"s\":\"ab\xc3\xa9\"}", "s", f, sizeof(f)) == HJ_STR_OK && !strcmp(f, "ab\xc3\xa9"));
		CHECK(hj_get_str_fit("{\"s\":\"abc\xc3\xa9\"}", "s", f, sizeof(f)) == HJ_STR_CUT && !f[0]);
		/* missing or not a string: the caller's default stays */
		strcpy(f, "dflt");
		CHECK(hj_get_str_fit("{\"t\":\"x\"}", "s", f, sizeof(f)) == HJ_STR_NONE && !strcmp(f, "dflt"));
		CHECK(hj_get_str_fit("{\"s\":12}", "s", f, sizeof(f)) == HJ_STR_NONE && !strcmp(f, "dflt"));
		/* a bad string anywhere: none, and "" as hj_get_str leaves it */
		CHECK(hj_get_str_fit("{\"s\":\"ok\",\"t\":\"\\q\"}", "s", f, sizeof(f)) == HJ_STR_NONE && !f[0]);
		CHECK(hj_get_str_fit("{\"s\":\"\"}", "s", f, sizeof(f)) == HJ_STR_OK && !f[0]);
		CHECK(hj_get_str_fit("{\"s\":\"\"}", "s", f, 0) == HJ_STR_CUT);
		/* hj_get_str still cuts, as its callers expect */
		CHECK(hj_get_str("{\"s\":\"abc\\r\\n\"}", "s", f, sizeof(f)) && !strcmp(f, "abc\r"));
	}

	CHECK(hj_valid_object("{}"));
	CHECK(!hj_valid_object("{"));
	CHECK(!hj_valid_object("{\"a\":}"));
	CHECK(!hj_valid_object("{\"a\":1,}"));
	CHECK(!hj_valid_object("[1]"));
	CHECK(!hj_valid_object("{} x"));
	CHECK(!hj_valid_object("{\"a\":\"unterminated}"));
	CHECK(!hj_valid_object(NULL));
}

static void pd(void) {
	hifi_contract_t c;
	CHECK(hifi_ch224_protocol(0x00) == HIFI_PROTO_NONE);
	CHECK(hifi_ch224_protocol(0x01) == HIFI_PROTO_BC);
	CHECK(hifi_ch224_protocol(0x03) == HIFI_PROTO_QC2);
	CHECK(hifi_ch224_protocol(0x05) == HIFI_PROTO_QC3);
	CHECK(hifi_ch224_protocol(0x08) == HIFI_PROTO_PD);
	CHECK(hifi_ch224_protocol(0x30) == HIFI_PROTO_PD); /* EPR handshake */

	CHECK_NEAR(hifi_pd_nominal_v(20.4f), 20, 1e-6);
	CHECK_NEAR(hifi_pd_nominal_v(19.0f), 20, 1e-6);
	CHECK_NEAR(hifi_pd_nominal_v(5.1f), 5, 1e-6);
	CHECK_NEAR(hifi_pd_nominal_v(28.9f), 28, 1e-6);
	CHECK_NEAR(hifi_pd_nominal_v(17.3f), 17.3, 1e-4);

	/* 20 V, 3.25 A PD: a 65 W laptop charger */
	hifi_contract_from(true, 0x08, 65, 20.1f, &c);
	CHECK(c.known && c.proto == HIFI_PROTO_PD && !c.epr);
	CHECK_NEAR(c.v, 20, 1e-6);
	CHECK_NEAR(c.i_max_a, 3.25, 1e-5);
	CHECK_NEAR(c.w, 65, 1e-4);

	/* 28 V EPR, 5 A: 140 W */
	hifi_contract_from(true, 0x38, 100, 28.2f, &c);
	CHECK(c.epr);
	CHECK_NEAR(c.w, 140, 1e-3);

	/* QC: current unknown, 18 W assumed */
	hifi_contract_from(true, 0x04, 0, 12.0f, &c);
	CHECK(c.proto == HIFI_PROTO_QC3 && isnan(c.i_max_a));
	CHECK_NEAR(c.w, 18, 1e-6);

	/* the sink didn't answer: unknown, USB 2.0 power */
	hifi_contract_from(false, 0xff, 0xff, 5.0f, &c);
	CHECK(!c.known && c.proto == HIFI_PROTO_NONE && !c.epr);
	CHECK_NEAR(c.w, 7.5, 1e-6);
	CHECK_STR(hifi_proto_name(HIFI_PROTO_PD), "pd");
	CHECK_STR(hifi_proto_name(HIFI_PROTO_BC), "bc");
	CHECK_STR(hifi_proto_name(HIFI_PROTO_QC2), "qc2");
	CHECK_STR(hifi_proto_name(HIFI_PROTO_QC3), "qc3");
	CHECK_STR(hifi_proto_name(HIFI_PROTO_NONE), "none");
}

int main(void) {
	writer();
	reader();
	pd();
	return check_report("json+pd");
}
