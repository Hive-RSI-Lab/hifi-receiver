/* the LAN gate (core/hifi_gate.c): DNS rebinding and cross-site requests */
#include "check.h"
#include "hifi_gate.h"
#include "../components/hifi/esp/hifi_board_lite.h" /* the Lite's HIFI_LAN_RESERVED_PINS */

static const char *const names[] = {"hifi-lite-4", "squeezelite-a1b2c3"};
#define ALLOW(h, o) CHECK(hifi_gate_allow(h, o, names, 2))
#define DENY(h, o) CHECK(!hifi_gate_allow(h, o, names, 2))

static uint32_t fake_rnd(void) {
	static uint32_t x = 12345;
	x = x * 1103515245u + 12345u;
	return x >> 8;
}

int main(void) {
	/* L6: an armed LAN reset erases only on a power-on start */
	CHECK(hifi_reset_confirmed(true, true));
	CHECK(!hifi_reset_confirmed(true, false));
	CHECK(!hifi_reset_confirmed(false, true));
	CHECK(!hifi_reset_confirmed(false, false));
	/* IP literals, any port, no Origin */
	ALLOW("192.168.1.50", NULL);
	ALLOW("192.168.4.1:80", NULL);
	ALLOW("10.0.0.7", "");
	ALLOW("[fe80::1]", NULL);
	ALLOW("[fe80::1%25en0]:80", NULL);
	ALLOW("[2001:db8::7]:8080", NULL);
	/* the board's own names */
	ALLOW("hifi-lite-4", NULL);
	ALLOW("HIFI-Lite-4.local", NULL);
	ALLOW("hifi-lite-4.local.", NULL);
	ALLOW("hifi-lite-4.home.arpa", NULL);
	ALLOW("hifi-lite-4.internal:80", NULL);
	/* router suffixes are, or could be, public domains: refused */
	DENY("hifi-lite-4.lan:80", NULL);
	DENY("squeezelite-a1b2c3.fritz.box", NULL);
	DENY("hifi-lite-4.home", NULL);
	DENY("hifi-lite-4.box", NULL);
	DENY("hifi-lite-4.router", NULL);
	DENY("hifi-lite-4.localdomain", NULL);
	/* no Host: not a browser */
	ALLOW(NULL, NULL);
	ALLOW("", NULL);
	DENY(NULL, "http://evil.example");

	/* DNS rebinding: someone else's name pointed at the board */
	DENY("evil.example", NULL);
	DENY("evil.example:80", NULL);
	DENY("hifi-lite-4.evil.example", NULL);
	DENY("hifi-lite-4x.local", NULL);
	DENY("hifi-lite.local", NULL);
	DENY("localhost", NULL);
	/* a hostname renamed to a domain (from the LAN) never counts as ours */
	{
		static const char *const dotted[] = {"evil.example"};
		CHECK(!hifi_gate_allow("evil.example", NULL, dotted, 1));
		CHECK(!hifi_gate_allow("evil.example.local", NULL, dotted, 1));
		CHECK(hifi_gate_allow("192.168.1.50", NULL, dotted, 1));
	}
	DENY("1.2.3.4.evil.example", NULL);
	DENY("192.168.1.256", NULL);
	DENY("192.168.1", NULL);
	ALLOW("192.168.1.50.", NULL);
	DENY("fe80::1", NULL);
	DENY("[fe80::1]x", NULL);
	DENY("[zz::1]", NULL);
	DENY("[fe80::1", NULL);
	DENY(":80", NULL);
	DENY("a:1:2", NULL);
	/* a port is 1 to 5 digits (L12) */
	ALLOW("192.168.1.50:65535", NULL);
	ALLOW("[fe80::1]:8", NULL);
	DENY("192.168.1.50:abc", NULL);
	DENY("hifi-lite-4:80x", NULL);
	DENY("192.168.1.50:", NULL);
	DENY("[fe80::1]:", NULL);
	DENY("192.168.1.50:123456", NULL);
	DENY("192.168.1.50: 80", NULL);
	DENY("[fe80::1]:8:0", NULL);
	DENY("192.168.1.50:-1", NULL);
	{
		char longhost[300];
		memset(longhost, 'a', sizeof(longhost) - 1);
		longhost[sizeof(longhost) - 1] = 0;
		DENY(longhost, NULL);
	}

	/* cross-site: Origin must be this board, as the browser sees it */
	ALLOW("192.168.1.50", "http://192.168.1.50");
	ALLOW("192.168.1.50:8080", "http://192.168.1.50:8080");
	ALLOW("hifi-lite-4.local", "http://HIFI-LITE-4.local");
	DENY("192.168.1.50", "http://evil.example");
	DENY("192.168.1.50", "https://192.168.1.50");
	DENY("192.168.1.50", "http://192.168.1.50:81");
	DENY("192.168.1.50", "http://192.168.1.5");
	DENY("192.168.1.50", "null");
	DENY("hifi-lite-4.local", "http://192.168.1.50");

	/* host_is_ip */
	CHECK(hifi_gate_host_is_ip("192.168.4.1"));
	CHECK(hifi_gate_host_is_ip("[::1]:80"));
	CHECK(!hifi_gate_host_is_ip("hifi-lite-4.local"));
	CHECK(!hifi_gate_host_is_ip(NULL));
	/* no names configured: only IPs */
	CHECK(hifi_gate_allow("192.168.1.50", NULL, NULL, 0));
	CHECK(!hifi_gate_allow("hifi-lite-4", NULL, NULL, 0));

	/* squeezelite's UI from the LAN */
	CHECK(hifi_lan_command_ok("cfg-hw-dac --model_name ES8388"));
	CHECK(hifi_lan_command_ok("  squeezelite -o I2S -n kitchen"));
	CHECK(hifi_lan_command_ok("restart"));
	CHECK(hifi_lan_command_ok("version"));
	CHECK(!hifi_lan_command_ok("cfg-"));
	CHECK(!hifi_lan_command_ok("restart_ota"));
	CHECK(!hifi_lan_command_ok("nvs_set hifi_relay_key -t str -v x"));
	CHECK(!hifi_lan_command_ok("nvs_list"));
	CHECK(!hifi_lan_command_ok("api remote on"));
	CHECK(!hifi_lan_command_ok("hifi relay"));
	CHECK(!hifi_lan_command_ok("update http://evil/x.bin"));
	CHECK(!hifi_lan_command_ok("recovery"));
	CHECK(!hifi_lan_command_ok("join evil pass"));
	CHECK(!hifi_lan_command_ok("i2cset -c 0x4c -r 1 0"));
	CHECK(!hifi_lan_command_ok("restartx"));
	CHECK(!hifi_lan_command_ok(""));
	CHECK(!hifi_lan_command_ok(NULL));
	/* a second command hidden after a line break or other control character */
	CHECK(!hifi_lan_command_ok("version\nrecovery"));
	CHECK(!hifi_lan_command_ok("version\rrecovery"));
	CHECK(!hifi_lan_command_ok("cfg-x\nnvs_set hifi_relay_key -t str -v x"));
	CHECK(!hifi_lan_command_ok("restart\x7f"));
	CHECK(!hifi_lan_command_ok("squeezelite -n a\x1b[2Jb"));
	CHECK(!hifi_lan_command_ok("version\n"));
	/* the console parts words at spaces only: "squeezelite\t-n" is one word */
	CHECK(!hifi_lan_command_ok("squeezelite\t-n kitchen"));
	CHECK(!hifi_lan_config_ok("autoexec1", "version\nrecovery"));
	CHECK(hifi_lan_config_ok("autoexec1", "squeezelite -n kitchen"));

	CHECK(hifi_lan_config_ok("host_name", "kitchen"));
	CHECK(hifi_lan_config_ok("ap_ssid", "x"));
	CHECK(hifi_lan_config_ok("autoexec", "1"));
	CHECK(hifi_lan_config_ok("autoexec1", "squeezelite -o I2S -n kitchen"));
	CHECK(hifi_lan_config_ok("autoexec2", ""));
	CHECK(!hifi_lan_config_ok("autoexec2", "nvs_set hifi_relay_url -t str -v http://evil"));
	CHECK(!hifi_lan_config_ok("autoexec3", NULL));
	CHECK(!hifi_lan_config_ok("fwurl", "http://evil/x.bin"));
	CHECK(!hifi_lan_config_ok("telnet_enable", "Y"));
	CHECK(!hifi_lan_config_ok("hifi_relay_url", "http://evil"));
	CHECK(!hifi_lan_config_ok("hifi_relay_key", "x"));
	CHECK(!hifi_lan_config_ok("hifi_update_url", "http://evil"));
	CHECK(!hifi_lan_config_ok("", "x"));
	/* every hifi_* key is the firmware's own: the Bluetooth test's saved line
	 * goes back into autoexec1 at boot, the rollback notes feed the relay */
	CHECK(!hifi_lan_config_ok("hifi_bt_save", "nvs_set hifi_relay_key str -v x"));
	CHECK(!hifi_lan_config_ok("hifi_bt_save", "squeezelite -n kitchen"));
	CHECK(!hifi_lan_config_ok("hifi_bttest", "used"));
	CHECK(!hifi_lan_config_ok("hifi_rb_why", "x"));
	CHECK(!hifi_lan_config_ok("hifi_name", "kitchen"));
	/* autoexecN runs at every boot: squeezelite or cfg-* only */
	CHECK(!hifi_lan_config_ok("autoexec1", "restart"));
	CHECK(!hifi_lan_config_ok("autoexec1", "  restart"));
	CHECK(!hifi_lan_config_ok("autoexec2", "version"));
	CHECK(!hifi_lan_config_ok("autoexec2", "tasks"));
	CHECK(hifi_lan_config_ok("autoexec2", "cfg-hw-dac --model_name ES8388"));
	CHECK(hifi_autoexec_ok("squeezelite -o BT -n kitchen"));
	CHECK(!hifi_autoexec_ok("restart"));
	CHECK(!hifi_autoexec_ok("nvs_set hifi_relay_key str -v x"));
	CHECK(!hifi_autoexec_ok("squeezelite\nrecovery"));
	CHECK(!hifi_autoexec_ok(NULL));
	CHECK(!hifi_autoexec_ok(""));
	/* L15 (patches/0012): cfg-audio-bt_source -n rewrites autoexec1 with the BT
	 * name stripped (strip_bt_name); the patch saves only what this passes */
	CHECK(hifi_autoexec_ok("squeezelite -o \"BT\" -b 500:2000 -R -Z 96000 -r \"44100-44100\""));
	CHECK(hifi_autoexec_ok("squeezelite -o BT -b 500:2000"));
	CHECK(!hifi_autoexec_ok("squeezelite -o BT -b 500:2000 -t")); /* -t prints the licence and exits */
	CHECK(!hifi_autoexec_ok("squeezelite -o BT\nrestart"));
	/* cfg-hw-preset stores any NVS key its JSON names: an explicit list */
	CHECK(!hifi_lan_command_ok("cfg-hw-preset --model_config {\"name\":\"x\",\"config\":[{\"hifi_relay_url\":\"ws://evil\"}]}"));
	CHECK(!hifi_autoexec_ok("cfg-hw-preset --model_config x"));
	CHECK(!hifi_lan_command_ok("cfg-hw-dacx"));
	CHECK(!hifi_lan_command_ok("cfg-anything"));
	CHECK(hifi_lan_command_ok("cfg-syst-name -n kitchen"));
	CHECK(hifi_lan_command_ok("cfg-syst-services --AirPlay"));

	/* L8: cfg-hw-* takes its own options by exact long name, and no pin
	 * option names a reserved pin. Always: flash 6-11, PSRAM 16-17 */
	CHECK(hifi_lan_command_ok("cfg-hw-dac --model_name ES8388 --clock 27 --wordselect 25 --data 26 --dac_sda 33 --dac_scl 32 --dac_i2c 16"));
	CHECK(hifi_lan_command_ok("cfg-hw-dac --model_name \"ES8388\" --clock=27 --mute_gpio -1 --mute_level"));
	CHECK(hifi_lan_command_ok("cfg-hw-dac --clear"));
	CHECK(hifi_lan_command_ok("cfg-hw-dac"));
	CHECK(!hifi_lan_command_ok("cfg-hw-dac --data 7"));	   /* flash */
	CHECK(!hifi_lan_command_ok("cfg-hw-dac --data=11"));	   /* flash */
	CHECK(!hifi_lan_command_ok("cfg-hw-dac --mute_gpio 16")); /* PSRAM */
	CHECK(!hifi_lan_command_ok("cfg-hw-spdif --clock 1 --wordselect 2 --data 17"));
	CHECK(hifi_lan_command_ok("cfg-hw-dac --dac_i2c 16")); /* an address, not a pin */
	/* pins only in plain decimal: argtable reads 0x07, 0o7 and 7KB as numbers too */
	CHECK(!hifi_lan_command_ok("cfg-hw-dac --data 0x07"));
	CHECK(!hifi_lan_command_ok("cfg-hw-dac --data 0o7"));
	CHECK(!hifi_lan_command_ok("cfg-hw-dac --data 7KB"));
	CHECK(!hifi_lan_command_ok("cfg-hw-dac --data +7"));
	CHECK(!hifi_lan_command_ok("cfg-hw-dac --data -7"));
	CHECK(!hifi_lan_command_ok("cfg-hw-dac --data 64"));
	CHECK(!hifi_lan_command_ok("cfg-hw-dac --data 40")); /* no GPIO 40-63 on the ESP32 */
	CHECK(hifi_lan_command_ok("cfg-hw-dac --data 39"));
	CHECK(!hifi_lan_command_ok("cfg-hw-dac --data 007"));
	CHECK(!hifi_lan_command_ok("cfg-hw-dac --data \"\""));
	CHECK(!hifi_lan_command_ok("cfg-hw-dac --data"));	   /* no value */
	CHECK(!hifi_lan_command_ok("cfg-hw-dac --data="));
	/* no short options (cfg-hw-display gives -b to cs and back), no
	 * abbreviations (getopt_long takes --mute as --mute_gpio), no positionals */
	CHECK(!hifi_lan_command_ok("cfg-hw-spi -d 7"));
	CHECK(!hifi_lan_command_ok("cfg-hw-display -b 7"));
	CHECK(!hifi_lan_command_ok("cfg-hw-dac --mute 7"));
	CHECK(!hifi_lan_command_ok("cfg-hw-dac --mute 20"));
	CHECK(!hifi_lan_command_ok("cfg-hw-dac --data 26 x"));
	CHECK(!hifi_lan_command_ok("cfg-hw-dac -- --data 7"));
	CHECK(!hifi_lan_command_ok("cfg-hw-dac --clear=1"));
	CHECK(!hifi_lan_command_ok("cfg-hw-dac --A 5"));	   /* rotary's, not dac's */
	/* a value option takes the next word whatever it is, as getopt does */
	CHECK(hifi_lan_command_ok("cfg-hw-display --type SPI --depth -1 --cs 5 --back 2 --reset 15 --width 128 --height 64"));
	CHECK(hifi_lan_command_ok("cfg-hw-ledvu --type WS2812 --length 8 --gpio 22"));
	CHECK(hifi_lan_command_ok("cfg-hw-rotary --A 5 --B 13 --SW 15 --knobonly --timer 350"));
	CHECK(hifi_lan_command_ok("cfg-hw-i2c --port 0 --speed 400000 --sda 33 --scl 32"));
	CHECK(hifi_lan_command_ok("cfg-hw-spi --clk 14 --data 13 --miso 12 --dc 15 --host 1"));
	CHECK(!hifi_lan_command_ok("cfg-hw-ledvu --type WS2812 --length 8 --gpio 9"));
	CHECK(!hifi_autoexec_ok("cfg-hw-dac --data 8")); /* autoexecN too */
	CHECK(hifi_lan_config_ok("autoexec2", "cfg-hw-ledvu --type WS2812 --length 8 --gpio 22"));
	CHECK(!hifi_lan_config_ok("autoexec2", "cfg-hw-ledvu --type WS2812 --length 8 --gpio 10"));
	/* the Lite's own: the power path, the amp, the sense inputs */
	CHECK(hifi_lan_command_ok("cfg-hw-ledvu --type WS2812 --length 8 --gpio 18"));
	hifi_gate_reserve_pins(HIFI_LAN_RESERVED_PINS);
	CHECK(!hifi_lan_command_ok("cfg-hw-ledvu --type WS2812 --length 8 --gpio 18")); /* PVDD_EN */
	CHECK(!hifi_lan_command_ok("cfg-hw-dac --mute_gpio 21"));		     /* AMP_SDZ */
	CHECK(!hifi_lan_command_ok("cfg-hw-rotary --A 4 --B 5"));		     /* PD PG */
	CHECK(!hifi_lan_command_ok("cfg-hw-rotary --A 5 --B 13 --SW 23"));	     /* FAULTZ */
	CHECK(!hifi_lan_command_ok("cfg-hw-i2c --sda 34 --scl 32"));		     /* VBUS sense */
	CHECK(!hifi_lan_command_ok("cfg-hw-spi --clk 39"));			     /* PVDD sense */
	CHECK(!hifi_lan_command_ok("cfg-hw-dac --data 9"));			     /* still flash */
	/* the codec's own pins stay: the UI's DAC form sends them back */
	CHECK(hifi_lan_command_ok("cfg-hw-dac --model_name ES8388 --clock 27 --wordselect 25 --data 26 --dac_sda 33 --dac_scl 32"));
	/* the other cfg-* setters aren't hardware: unchanged */
	CHECK(hifi_lan_command_ok("cfg-syst-name -n 18"));
	CHECK(hifi_autoexec_ok("cfg-audio-general -v 50"));
	/* squeezelite options that make it exit (and reboot the board 60 s later,
	 * at every boot), or that this build doesn't know */
	CHECK(hifi_autoexec_ok("squeezelite -o I2S -b 500:2000 -d all=info -C 30 -W")); /* the default */
	CHECK(hifi_autoexec_ok("squeezelite -o \"BT -n 'Speaker'\" -b 500:2000 -R -Z 96000 -r \"44100-44100\""));
	CHECK(hifi_autoexec_ok("squeezelite -o I2S -b 500:2000 -u m -s 192.168.1.10 -n \"Living room\""));
	CHECK(hifi_autoexec_ok("squeezelite -o I2S -b :2000 -u -n x"));
	/* -u/-R with no output size: 1450 KB times up to 8, malloc fails, exit */
	CHECK(!hifi_autoexec_ok("squeezelite -o I2S -u m -s 192.168.1.10 -n \"Living room\""));
	CHECK(!hifi_autoexec_ok("squeezelite -o I2S -u -n x"));
	CHECK(!hifi_autoexec_ok("squeezelite -o I2S -b 500 -R"));
	CHECK(hifi_autoexec_ok("squeezelite -o I2S -b 500:2000 -b 500: -R -b 600")); /* output 2000 kept */
	/* -b: sizes squeezelite can allocate (a failed malloc exits) */
	CHECK(hifi_autoexec_ok("squeezelite -o I2S -b 500"));           /* output 1450 */
	CHECK(hifi_autoexec_ok("squeezelite -o I2S -b 500:"));
	CHECK(hifi_autoexec_ok("squeezelite -o I2S -b :2100"));         /* stream 480 */
	CHECK(hifi_autoexec_ok("squeezelite -o I2S -b 600:2000"));
	CHECK(!hifi_autoexec_ok("squeezelite -o I2S -b :2500"));        /* 2980 KB */
	CHECK(!hifi_autoexec_ok("squeezelite -o I2S -b 1000:2000"));    /* 3000 KB */
	CHECK(hifi_autoexec_ok("squeezelite -o I2S -b 860"));           /* 860 + 1740 (crossfade) */
	CHECK(!hifi_autoexec_ok("squeezelite -o I2S -b 861"));
	CHECK(!hifi_autoexec_ok("squeezelite -o I2S -b 1100:1450"));    /* 1450 grows to 1740 */
	CHECK(hifi_autoexec_ok("squeezelite -o I2S -b 1100:1451"));
	CHECK(hifi_autoexec_ok("squeezelite -o I2S -b 0500:2000"));
	CHECK(hifi_autoexec_ok("squeezelite -o I2S -b 64:256"));
	CHECK(!hifi_autoexec_ok("squeezelite -o I2S -b 100000:2000"));
	CHECK(!hifi_autoexec_ok("squeezelite -o I2S -b 500:999999"));
	CHECK(!hifi_autoexec_ok("squeezelite -o I2S -b 601:2000"));     /* 2601 KB */
	CHECK(!hifi_autoexec_ok("squeezelite -o I2S -b :2121"));        /* 480 + 2121 */
	/* -u/-R with no -b output: 1450 KB times the top rate / 44100 (1 to 8) */
	CHECK(hifi_autoexec_ok("squeezelite -o I2S -R -r 44100"));
	CHECK(hifi_autoexec_ok("squeezelite -o I2S -R -r 48000,44100:500"));
	CHECK(!hifi_autoexec_ok("squeezelite -o I2S -R -r 96000"));     /* 2900 KB */
	CHECK(!hifi_autoexec_ok("squeezelite -o I2S -R -r 44100 -r 96000"));
	CHECK(hifi_autoexec_ok("squeezelite -o I2S -R -r 44100 -r :100"));
	CHECK(!hifi_autoexec_ok("squeezelite -o I2S -R -r 0"));         /* 0: x8 */
	/* -r: squeezelite writes past rates[] above 768000 */
	CHECK(hifi_autoexec_ok("squeezelite -o I2S -r 768000"));
	CHECK(hifi_autoexec_ok("squeezelite -o I2S -r 8000-96000"));
	CHECK(hifi_autoexec_ok("squeezelite -o I2S -r -96000"));
	CHECK(hifi_autoexec_ok("squeezelite -o I2S -r 44100-:12345"));
	CHECK(!hifi_autoexec_ok("squeezelite -o I2S -r 800000"));
	CHECK(!hifi_autoexec_ok("squeezelite -o I2S -r 0-999999"));
	CHECK(!hifi_autoexec_ok("squeezelite -o I2S -r 4294967296"));
	CHECK(!hifi_autoexec_ok("squeezelite -o I2S -r -"));
	CHECK(!hifi_autoexec_ok("squeezelite -o I2S -r 1-2-3"));
	CHECK(!hifi_autoexec_ok("squeezelite -o I2S -r 44100,48000-96000"));
	CHECK(!hifi_autoexec_ok("squeezelite -o I2S -r 44100,"));
	CHECK(!hifi_autoexec_ok("squeezelite -o I2S -r ,44100"));
	CHECK(!hifi_autoexec_ok("squeezelite -o I2S -r 44100,,48000"));
	CHECK(!hifi_autoexec_ok("squeezelite -o I2S -r 44100:1:2"));
	CHECK(!hifi_autoexec_ok("squeezelite -o I2S -r 44100:123456"));
	CHECK(!hifi_autoexec_ok("squeezelite -o I2S -r 44.1k"));
	CHECK(hifi_autoexec_ok("squeezelite -o I2S -r 8001,8002,8003,8004,8005,8006,8007,8008,8009,8010,8011,8012,8013,8014,8015,8016,8017"));
	CHECK(!hifi_autoexec_ok("squeezelite -o I2S -r 8001,8002,8003,8004,8005,8006,8007,8008,8009,8010,8011,8012,8013,8014,8015,8016,8017,8018")); /* no 0 left to end the list */
	CHECK(!hifi_autoexec_ok("squeezelite -o I2S -r 1"));            /* under 8000 */
	CHECK(!hifi_autoexec_ok("squeezelite -o I2S -r 7999-44100"));
	CHECK(!hifi_autoexec_ok("squeezelite -o I2S -r 44100,100"));
	CHECK(hifi_autoexec_ok("squeezelite -o I2S -r 8000-44100"));
	CHECK(hifi_autoexec_ok("squeezelite -o I2S -r 0-44100"));
	CHECK(!hifi_autoexec_ok("squeezelite -o I2S -b 2000"));         /* 2000 + 1450 */
	CHECK(!hifi_autoexec_ok("squeezelite -o I2S -b -1:2000"));
	CHECK(!hifi_autoexec_ok("squeezelite -o I2S -b 500:2000:9"));
	CHECK(!hifi_autoexec_ok("squeezelite -o I2S -b :"));
	CHECK(!hifi_autoexec_ok("squeezelite -o I2S -b 0:2000"));
	CHECK(hifi_autoexec_ok("squeezelite -o I2S -b 500:0"));        /* 0: the default 1450, as squeezelite does */
	CHECK(!hifi_autoexec_ok("squeezelite -o I2S -b 500:0 -R"));
	CHECK(!hifi_autoexec_ok("squeezelite -o I2S -b 63:2000"));
	CHECK(!hifi_autoexec_ok("squeezelite -o I2S -b 500:255"));
	CHECK(!hifi_autoexec_ok("squeezelite -o I2S -b 5e2:2000"));
	CHECK(!hifi_autoexec_ok("squeezelite -o I2S -b \" 500:2000\""));
	CHECK(!hifi_autoexec_ok("squeezelite -o I2S -b 500:2000 -b 2000:2000")); /* the last wins */
	CHECK(hifi_autoexec_ok("squeezelite -o I2S -b 2000:500 -b 500:"));        /* 500 + 500 */
	/* a tab: the console doesn't split on it, so the line is refused */
	CHECK(!hifi_autoexec_ok("squeezelite\t-t -o I2S"));
	CHECK(!hifi_autoexec_ok("squeezelite -o I2S\t-n x"));
	CHECK(!hifi_lan_command_ok("version\t"));
	/* the name as the console sees it, quotes and all */
	CHECK(hifi_autoexec_ok("\"squeezelite\" -o I2S"));
	CHECK(!hifi_autoexec_ok("\"squeezelite -t\" -o I2S"));
	CHECK(!hifi_autoexec_ok("squeezelite"));           /* the console adds -?: help, exit */
	CHECK(!hifi_autoexec_ok("squeezelite   "));
	CHECK(!hifi_lan_command_ok("squeezelite"));
	CHECK(hifi_autoexec_ok("squeezelite -n kitchen"));  /* no -o: I2S */
	CHECK(!hifi_autoexec_ok("squeezelite -N /names"));   /* with -n it exits */
	CHECK(!hifi_autoexec_ok("squeezelite -n a -N b"));
	CHECK(!hifi_autoexec_ok("squeezelite -o I2S -d all"));        /* no '=': exits */
	CHECK(!hifi_autoexec_ok("squeezelite -o I2S -d all="));
	CHECK(!hifi_autoexec_ok("squeezelite -o I2S -d =info"));
	CHECK(!hifi_autoexec_ok("squeezelite -o I2S -d ==="));
	CHECK(hifi_autoexec_ok("squeezelite -o I2S -d slimproto=debug"));
	CHECK(hifi_autoexec_ok("squeezelite -o I2S -d all==info")); /* strtok takes it */
	CHECK(!hifi_autoexec_ok("squeezelite -o I2S -t"));
	CHECK(!hifi_autoexec_ok("squeezelite -?"));
	CHECK(!hifi_autoexec_ok("squeezelite -h"));
	CHECK(!hifi_autoexec_ok("squeezelite -l"));
	CHECK(!hifi_autoexec_ok("squeezelite -L"));
	CHECK(!hifi_autoexec_ok("squeezelite --help"));
	CHECK(!hifi_autoexec_ok("squeezelite -o I2S stray"));
	CHECK(!hifi_autoexec_ok("squeezelite -o"));          /* a value missing */
	CHECK(!hifi_autoexec_ok("squeezelite -ob I2S"));     /* strstr would take it; refused */
	CHECK(!hifi_lan_command_ok("squeezelite -t"));
	{
		/* the console keeps 27 words and 599 bytes and drops the rest unseen */
		char l[1024];
		strcpy(l, "squeezelite");
		for (int i = 0; i < 13; i++) strcat(l, " -n x");        /* 27 words */
		CHECK(hifi_autoexec_ok(l));
		strcat(l, " -W");                                         /* 28 */
		CHECK(!hifi_autoexec_ok(l));
		strcpy(l, "squeezelite -n ");
		size_t k = strlen(l);
		memset(l + k, 'a', 599 - k);
		l[599] = 0;                                                /* 599 bytes */
		CHECK(hifi_autoexec_ok(l));
		CHECK(hifi_lan_command_ok(l));
		l[599] = 'a';
		l[600] = 0;                                                /* 600 */
		CHECK(!hifi_autoexec_ok(l));
		CHECK(!hifi_lan_command_ok(l));
		CHECK(!hifi_lan_config_ok("autoexec1", l));
		strcpy(l, "cfg-syst-name");
		for (int i = 0; i < 27; i++) strcat(l, " x");          /* 28 words */
		CHECK(!hifi_lan_command_ok(l));
	}
	CHECK(!hifi_lan_config_ok("autoexec1", "squeezelite -o I2S -t"));
	/* the name quoted as the console reads it: a quote ends the word, \" is a quote */
	CHECK(!hifi_autoexec_ok("squeezelite -n \"a\" -t \"b\""));
	CHECK(hifi_autoexec_ok("squeezelite -n \"a\\\" -t \\\"b\""));
	CHECK(!hifi_autoexec_ok("squeezelite -n a\\ b -t"));
	CHECK(hifi_autoexec_ok("squeezelite -n a\\ b"));
	CHECK(!hifi_autoexec_ok("squeezelite \\-t"));        /* \- drops both: "t" is a stray word */
	/* during an OTA trial: what a LAN request may not do */
	CHECK(hifi_lan_trial_refused("/api/v1/wifi", "POST"));
	/* the server routes on the parsed path, not on these spellings */
	CHECK(hifi_lan_trial_refused("http://192.168.1.5/reboot.json", "POST"));
	CHECK(hifi_lan_trial_refused("/reboot.json#x", "POST"));
	CHECK(hifi_lan_trial_refused("/connect.json?a#b", "DELETE"));
	CHECK(hifi_lan_trial_refused("", "POST"));
	CHECK(!hifi_lan_trial_refused("http://192.168.1.5/reboot.json", "GET"));
	CHECK(hifi_lan_trial_refused("/api/v1/wifi/forget", "POST"));
	CHECK(hifi_lan_trial_refused("/api/v1/reset", "POST"));
	CHECK(hifi_lan_trial_refused("/api/v1/reboot", "POST"));
	CHECK(hifi_lan_trial_refused("/api/v1/bttest?x=1", "POST"));
	CHECK(hifi_lan_trial_refused("/connect.json", "POST"));
	CHECK(hifi_lan_trial_refused("/connect.json", "DELETE"));
	CHECK(hifi_lan_trial_refused("/reboot.json", "POST"));
	CHECK(hifi_lan_trial_refused("/reboot_ota.json", "POST"));
	CHECK(hifi_lan_trial_refused("/commands.json", "POST"));
	CHECK(hifi_lan_trial_refused("/config.json", "POST"));
	CHECK(!hifi_lan_trial_refused("/config.json", "GET"));
	CHECK(!hifi_lan_trial_refused("/connect.json", "HEAD"));
	CHECK(!hifi_lan_trial_refused("/api/v1/wifi/scan", "GET"));
	CHECK(!hifi_lan_trial_refused("/api/v1/volume", "POST"));
	CHECK(!hifi_lan_trial_refused("/api/v1/reboots", "POST"));
	CHECK(!hifi_lan_trial_refused("/config.jsonx", "POST"));
	CHECK(!hifi_lan_trial_refused(NULL, "POST"));
	CHECK(!hifi_lan_trial_refused("/reboot.json", NULL));
	/* another update during the trial would replace the rollback copy (L16) */
	CHECK(hifi_lan_trial_refused("/api/v1/update/upload", "POST"));
	CHECK(hifi_lan_trial_refused("/api/v1/update", "POST"));
	CHECK(hifi_lan_trial_refused("/api/v1/ota/begin", "POST"));
	CHECK(hifi_lan_trial_refused("/api/v1/ota/finish?x", "POST"));
	CHECK(!hifi_lan_trial_refused("/api/v1/update/check", "GET"));
	CHECK(!hifi_lan_trial_refused("/api/v1/ota", "GET"));
	/* GETs that start work: once per 5 s from the LAN (L7) */
	{
		long long last = 0, s = 1000000;
		CHECK(!hifi_lan_throttled("/scan.json", "GET", 10 * s, &last));
		CHECK(hifi_lan_throttled("/scan.json", "GET", 12 * s, &last));
		CHECK(hifi_lan_throttled("/api/v1/wifi/scan?x=1", "GET", 13 * s, &last)); /* one budget for all */
		CHECK(hifi_lan_throttled("/api/v1/update/check", "HEAD", 14 * s, &last));
		CHECK(!hifi_lan_throttled("/api/v1/wifi/scan", "GET", 15 * s, &last));
		CHECK(last == 15 * s);
		CHECK(!hifi_lan_throttled("/api/v1/state", "GET", 15 * s, &last));      /* not costly */
		CHECK(!hifi_lan_throttled("/scan.json", "POST", 15 * s, &last));        /* not a GET */
		CHECK(!hifi_lan_throttled("/scan.jsonx", "GET", 15 * s, &last));
		CHECK(last == 15 * s);
		CHECK(!hifi_lan_throttled("/api/v1/update/check", "GET", 3 * s, &last)); /* the clock went back */
		last = 0;
		CHECK(!hifi_lan_throttled("/scan.json", "GET", 0, &last));
		CHECK(last == 1);
		CHECK(!hifi_lan_throttled(NULL, "GET", 0, &last));
		CHECK(!hifi_lan_throttled("/scan.json", "GET", 0, NULL));
	}
	CHECK(hifi_lan_config_secret("hifi_relay_pass"));
	CHECK(hifi_lan_config_secret("hifi_relay_key"));
	CHECK(hifi_lan_config_secret("hifi_party_key"));
	CHECK(!hifi_lan_config_ok("hifi_party_key", "00"));
	CHECK(!hifi_lan_command_ok("nvs_set hifi_party_key -t str -v 00"));
	CHECK(!hifi_lan_config_secret("hifi_relay_url"));
	CHECK(!hifi_lan_config_secret("ap_pwd"));

	/* the setup AP */
	CHECK(!strcmp(hifi_ap_password_pick("squeezelite", "squeezelite", false), "squeezelite"));
	CHECK(hifi_ap_password_pick("squeezelite", "squeezelite", true) == NULL);
	CHECK(hifi_ap_password_pick("", "squeezelite", true) == NULL);
	CHECK(hifi_ap_password_pick(NULL, "squeezelite", true) == NULL);
	CHECK(!strcmp(hifi_ap_password_pick(NULL, "squeezelite", false), "squeezelite"));
	CHECK(!strcmp(hifi_ap_password_pick("my-own-pw", "squeezelite", true), "my-own-pw"));
	{
		char pw[13], pw2[13];
		hifi_ap_password_make(pw, sizeof(pw), fake_rnd);
		CHECK(strlen(pw) == 12);
		CHECK(strspn(pw, "abcdefghijkmnpqrstuvwxyz23456789") == 12);
		hifi_ap_password_make(pw2, sizeof(pw2), fake_rnd);
		CHECK(strcmp(pw, pw2) != 0);
	}
	CHECK(!hifi_ap_due(0, 0));
	CHECK(!hifi_ap_due(HIFI_AP_AFTER_US - 1, 0));
	CHECK(hifi_ap_due(HIFI_AP_AFTER_US, 0));
	CHECK(!hifi_ap_due(HIFI_AP_AFTER_US + 5, 10));
	return check_report("gate");
}
