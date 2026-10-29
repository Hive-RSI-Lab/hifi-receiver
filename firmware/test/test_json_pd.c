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
	CHECK(hj_get_str(body, "esc", s, sizeof(s)));
	CHECK_STR(s, "x\"y");
	/* truncation keeps a terminated string */
	char tiny[3];
	CHECK(hj_get_str(body, "type", tiny, sizeof(tiny)));
	CHECK_STR(tiny, "si");

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
}

int main(void) {
	writer();
	reader();
	pd();
	return check_report("json+pd");
}
