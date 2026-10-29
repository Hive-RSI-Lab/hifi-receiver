#include "check.h"
#include "hifi_ir.h"
#include "hifi_meta.h"
#include "hifi_vol.h"

static void volume_curve(void) {
	CHECK(hifi_vol_to_gain(100) == HIFI_VOL_UNITY);
	CHECK(hifi_vol_to_gain(0) == 0);
	CHECK(hifi_vol_to_gain(50) == 8192); /* 0.5^3: -18 dB */
	CHECK(hifi_vol_to_gain(150) == HIFI_VOL_UNITY);
	CHECK(hifi_vol_from_gain(HIFI_VOL_UNITY * 2) == 100);
	/* every whole volume reads back as itself */
	int bad = 0;
	for (int v = 0; v <= 100; v++) bad += hifi_vol_from_gain(hifi_vol_to_gain(v)) != v;
	CHECK(bad == 0);
	/* Bluetooth's AVRC volume 64/127 and AirPlay's 0.5: the same curve, so 50 */
	CHECK(hifi_vol_from_gain((uint32_t) (65536 * powf(64 / 128.0f, 3))) == 50);
}

static void volume_rules(void) {
	hifi_vol_t v;
	hifi_vol_init(&v, 120, 10); /* both held in range */
	CHECK(v.max_volume == HIFI_VOL_MAX_MIN && v.volume == HIFI_VOL_MAX_MIN && !v.mute);

	hifi_vol_init(&v, 40, 100);
	CHECK(hifi_vol_gain(&v) == hifi_vol_to_gain(40));

	/* a source's gain is kept as it sent it, below the maximum */
	CHECK(hifi_vol_source(&v, 30000) == 30000);
	CHECK(v.volume == hifi_vol_from_gain(30000));

	/* the maximum caps a source and the app */
	CHECK(hifi_vol_set(&v, -1, -1, 60));
	CHECK(v.volume == 60 && v.gain == hifi_vol_to_gain(60)); /* 30000 was above 60's gain */
	CHECK(hifi_vol_source(&v, HIFI_VOL_UNITY) == hifi_vol_to_gain(60));
	CHECK(v.volume == 60);
	CHECK(hifi_vol_set(&v, 90, -1, -1));
	CHECK(v.volume == 60);
	/* raising the maximum doesn't turn it up by itself */
	CHECK(hifi_vol_set(&v, -1, -1, 100));
	CHECK(v.volume == 60);
	CHECK(hifi_vol_set(&v, 80, -1, -1) && v.volume == 80);
	/* max and volume together: max first */
	CHECK(hifi_vol_set(&v, 70, -1, 70) && v.volume == 70 && v.max_volume == 70);

	/* out of range changes nothing */
	CHECK(!hifi_vol_set(&v, 101, 1, -1));
	CHECK(!hifi_vol_set(&v, -5, -1, -1));
	CHECK(!hifi_vol_set(&v, 10, -1, 29));
	CHECK(v.volume == 70 && v.max_volume == 70 && !v.mute);

	/* mute holds the gain at 0 and remembers the volume */
	CHECK(hifi_vol_set(&v, -1, 1, -1));
	CHECK(hifi_vol_gain(&v) == 0 && v.volume == 70);
	CHECK(hifi_vol_source(&v, 8192) == 0); /* a source's change while muted stays muted */
	CHECK(v.volume == 50);
	CHECK(hifi_vol_set(&v, -1, 0, -1) && hifi_vol_gain(&v) == 8192);

	/* steps: 5, held to 0..max, and they unmute */
	hifi_vol_set(&v, 68, 1, -1);
	hifi_vol_step(&v, +1);
	CHECK(v.volume == 70 && !v.mute);
	hifi_vol_step(&v, +1);
	CHECK(v.volume == 70);
	hifi_vol_set(&v, 3, -1, -1);
	hifi_vol_step(&v, -1);
	CHECK(v.volume == 0 && hifi_vol_gain(&v) == 0);
}

static void volume_codec(void) {
	hifi_vol_t v;
	hifi_vol_init(&v, 100, 100);
	CHECK(hifi_vol_codec_code(&v, 28) == 28);
	hifi_vol_set(&v, 50, -1, -1); /* -18 dB = 12 steps */
	CHECK(hifi_vol_codec_code(&v, 28) == 16);
	hifi_vol_set(&v, 5, -1, -1); /* -78 dB: as low as the codec goes */
	CHECK(hifi_vol_codec_code(&v, 28) == 0);
	hifi_vol_set(&v, 0, -1, -1);
	CHECK(hifi_vol_codec_code(&v, 28) == -1);
	hifi_vol_set(&v, 80, 1, -1);
	CHECK(hifi_vol_codec_code(&v, 28) == -1);
}

static void ir(void) {
	hifi_ir_t ir;
	bool ch;
	char s[256];
	hifi_ir_init(&ir);
	CHECK(hifi_ir_action_from("mute") == HIFI_IR_MUTE);
	CHECK(hifi_ir_action_from("explode") == -1);
	CHECK_STR(hifi_ir_action_name(HIFI_IR_POWER), "power");
	CHECK(hifi_ir_action_name(HIFI_IR_N) == NULL);

	/* nothing learned: every code goes on to squeezelite */
	CHECK(hifi_ir_code(&ir, 0x00ff40bf, 1000, &ch) == HIFI_IR_PASS && !ch);

	/* learn mute: the next press binds, its repeats don't fire it */
	hifi_ir_learn(&ir, HIFI_IR_MUTE, 2000);
	CHECK(hifi_ir_learning(&ir, 2000) == HIFI_IR_MUTE);
	CHECK(hifi_ir_code(&ir, 0x00ff40bf, 3000, &ch) == HIFI_IR_EATEN && ch);
	CHECK(hifi_ir_learning(&ir, 3000) == -1);
	CHECK(ir.have[HIFI_IR_MUTE] && ir.code[HIFI_IR_MUTE] == 0x00ff40bf);
	CHECK(hifi_ir_code(&ir, 0x00ff40bf, 3108, &ch) == HIFI_IR_EATEN && !ch);

	/* a new press is the action; a held one fires once */
	CHECK(hifi_ir_code(&ir, 0x00ff40bf, 5000, &ch) == HIFI_IR_MUTE);
	CHECK(hifi_ir_code(&ir, 0x00ff40bf, 5108, &ch) == HIFI_IR_EATEN);
	CHECK(hifi_ir_code(&ir, 0x00ff40bf, 5216, &ch) == HIFI_IR_EATEN);
	CHECK(hifi_ir_code(&ir, 0x00ff40bf, 6000, &ch) == HIFI_IR_MUTE);

	/* a held volume button steps every 200 ms or so */
	hifi_ir_learn(&ir, HIFI_IR_VOLUP, 7000);
	hifi_ir_code(&ir, 0x00ff00ff, 7100, &ch);
	CHECK(hifi_ir_code(&ir, 0x00ff00ff, 9000, &ch) == HIFI_IR_VOLUP);
	int fired = 0;
	for (uint32_t t = 9108; t < 10100; t += 108) fired += hifi_ir_code(&ir, 0x00ff00ff, t, &ch) == HIFI_IR_VOLUP;
	CHECK(fired >= 4 && fired <= 5);

	/* one button, one action: learning a used code moves it */
	hifi_ir_learn(&ir, HIFI_IR_TOGGLE, 11000);
	hifi_ir_code(&ir, 0x00ff40bf, 11500, &ch);
	CHECK(ir.have[HIFI_IR_TOGGLE] && !ir.have[HIFI_IR_MUTE]);
	CHECK(hifi_ir_code(&ir, 0x00ff40bf, 13000, &ch) == HIFI_IR_TOGGLE);

	/* learning gives up after 30 s */
	hifi_ir_learn(&ir, HIFI_IR_POWER, 20000);
	CHECK(hifi_ir_learning(&ir, 20000 + HIFI_IR_LEARN_MS + 1) == -1);
	CHECK(hifi_ir_code(&ir, 0x12345678, 60000, &ch) == HIFI_IR_PASS && !ch);
	hifi_ir_learn(&ir, 99, 0); /* not an action: ignored */
	CHECK(hifi_ir_learning(&ir, 60000) == -1);

	/* saved and loaded */
	hifi_ir_save(&ir, s, sizeof(s));
	CHECK_STR(s, "volup=00ff00ff,toggle=00ff40bf");
	hifi_ir_t b;
	hifi_ir_init(&b);
	hifi_ir_load(&b, s);
	CHECK(b.have[HIFI_IR_VOLUP] && b.code[HIFI_IR_VOLUP] == 0x00ff00ff);
	CHECK(b.have[HIFI_IR_TOGGLE] && b.code[HIFI_IR_TOGGLE] == 0x00ff40bf);
	CHECK(!b.have[HIFI_IR_MUTE]);
	/* junk in NVS is skipped, not trusted */
	hifi_ir_load(&b, "mute=zz,explode=00000001,next=0000a05f,=1,prev,power=1x");
	CHECK(!b.have[HIFI_IR_MUTE] && b.have[HIFI_IR_NEXT] && b.code[HIFI_IR_NEXT] == 0xa05f && !b.have[HIFI_IR_PREV]);
	CHECK(!b.have[HIFI_IR_POWER] && !b.have[HIFI_IR_VOLUP]);
	hifi_ir_load(&b, NULL);
	CHECK(!b.have[HIFI_IR_NEXT]);
	/* a buffer too small keeps whole entries only */
	hifi_ir_save(&ir, s, 20);
	CHECK_STR(s, "volup=00ff00ff");

	hifi_ir_forget(&ir, HIFI_IR_VOLUP);
	CHECK(!ir.have[HIFI_IR_VOLUP] && ir.have[HIFI_IR_TOGGLE]);
	hifi_ir_learn(&ir, HIFI_IR_NEXT, 0);
	hifi_ir_forget(&ir, -1);
	CHECK(!ir.have[HIFI_IR_TOGGLE] && ir.learning == -1);
	hifi_ir_save(&ir, s, sizeof(s));
	CHECK_STR(s, "");
}

static void meta(void) {
	hifi_meta_t m;
	char s[8];
	hifi_meta_init(&m);
	CHECK(hifi_play_name(m.state) == NULL);
	CHECK(hifi_meta_elapsed_ms(&m, 0) == -1);

	hifi_meta_active(&m);
	CHECK_STR(hifi_play_name(m.state), "stop");
	hifi_meta_track(&m, "Massive Attack", NULL, "Teardrop");
	CHECK_STR(m.artist, "Massive Attack");
	CHECK_STR(m.album, "");
	hifi_meta_timer(&m, 10000, 330000, 1000);
	hifi_meta_play(&m, 1000);
	CHECK_STR(hifi_play_name(m.state), "play");
	CHECK(hifi_meta_elapsed_ms(&m, 3500) == 12500);
	hifi_meta_play(&m, 3500); /* again: doesn't restart the clock */
	CHECK(hifi_meta_elapsed_ms(&m, 4000) == 13000);
	hifi_meta_pause(&m, 4000);
	CHECK(hifi_meta_elapsed_ms(&m, 60000) == 13000);
	hifi_meta_play(&m, 60000);
	CHECK(hifi_meta_elapsed_ms(&m, 61000) == 14000);
	CHECK(hifi_meta_elapsed_ms(&m, 999000) == 330000); /* held at the end */
	hifi_meta_timer(&m, -1, 0, 70000); /* leaves both */
	CHECK(m.duration_ms == 330000);
	hifi_meta_idle(&m);
	CHECK(hifi_play_name(m.state) == NULL && !m.title[0]);
	/* a track before any play still shows */
	hifi_meta_track(&m, NULL, NULL, "x");
	CHECK_STR(hifi_play_name(m.state), "stop");

	/* UTF-8 is cut at a whole character */
	hifi_utf8_copy(s, sizeof(s), "abcd\xc3\xa9\xc3\xa9"); /* abcdéé: 8 bytes */
	CHECK_STR(s, "abcd\xc3\xa9");
	hifi_utf8_copy(s, sizeof(s), "abcde\xe2\x82\xac"); /* abcde€ */
	CHECK_STR(s, "abcde");
	hifi_utf8_copy(s, sizeof(s), "short");
	CHECK_STR(s, "short");
	hifi_utf8_copy(s, sizeof(s), NULL);
	CHECK_STR(s, "");
	char big[200];
	memset(big, 'a', sizeof(big) - 1);
	big[199] = '\0';
	hifi_meta_track(&m, big, big, big);
	CHECK(strlen(m.title) == sizeof(m.title) - 1 && strlen(m.album) == sizeof(m.album) - 1);
}

int main(void) {
	volume_curve();
	volume_rules();
	volume_codec();
	ir();
	meta();
	return check_report("vol+ir+meta");
}
