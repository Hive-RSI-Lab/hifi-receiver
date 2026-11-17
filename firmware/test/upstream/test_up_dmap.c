/* Regression: AirPlay's DMAP metadata parser (patch 0014). A sender on the
 * LAN posts DMAP to the speaker; the parser recursed once per nested dict
 * with no limit, so a deep enough message ran off the task stack. Builds
 * upstream's dmap_parser.c, as patched, and feeds it hostile input. */
#include <stdint.h>
#include <string.h>

#include "../check.h"
#include "dmap_parser.c"

static int dicts, strs;
static void on_dict(void *ctx, const char *code, const char *name) { (void) ctx; (void) code; (void) name; dicts++; }
static void on_str(void *ctx, const char *code, const char *name, const char *buf, size_t len)
{ (void) ctx; (void) code; (void) name; (void) buf; (void) len; strs++; }

static size_t put(unsigned char *b, const char *code, uint32_t len)
{
	memcpy(b, code, 4);
	b[4] = len >> 24; b[5] = len >> 16; b[6] = len >> 8; b[7] = len;
	return 8;
}

/* DEPTH mlcl dicts, one inside the next, around a 4-byte minm string */
static size_t nest(unsigned char *b, int depth)
{
	size_t n = 0;
	for (int i = 0; i < depth; i++) n += put(b + n, "mlcl", (uint32_t) (8 * (depth - i) + 4));
	n += put(b + n, "minm", 4);
	memcpy(b + n, "song", 4);
	return n + 4;
}

int main(void)
{
	static unsigned char b[8 * 1000 + 16];
	dmap_settings s = { .on_dict_start = on_dict, .on_string = on_str };

	size_t n = nest(b, 5);
	dicts = strs = 0;
	CHECK(dmap_parse(&s, (const char *) b, n) == 0);
	CHECK(dicts == 5 && strs == 1);

	n = nest(b, 16);
	CHECK(dmap_parse(&s, (const char *) b, n) == 0);

	/* past the cap it stops instead of recursing on */
	n = nest(b, 1000);
	dicts = 0;
	CHECK(dmap_parse(&s, (const char *) b, n) == -1);
	CHECK(dicts <= 17);

	/* a field longer than the message (the old p + field_len could wrap) */
	n = put(b, "minm", 0xfffffff8u);
	CHECK(dmap_parse(&s, (const char *) b, n + 4) == -1);

	return check_report("up_dmap");
}
