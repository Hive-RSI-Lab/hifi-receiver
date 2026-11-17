/* the LAN gate (core/hifi_gate.c): DNS rebinding and cross-site requests */
#include "check.h"
#include "hifi_gate.h"

static const char *const names[] = {"hifi-lite-4", "squeezelite-a1b2c3"};
#define ALLOW(h, o) CHECK(hifi_gate_allow(h, o, names, 2))
#define DENY(h, o) CHECK(!hifi_gate_allow(h, o, names, 2))

static uint32_t fake_rnd(void) {
	static uint32_t x = 12345;
	x = x * 1103515245u + 12345u;
	return x >> 8;
}

int main(void) {
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
	CHECK(hifi_lan_command_ok("cfg-hw-dac -m 1"));
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
	CHECK(hifi_lan_config_ok("autoexec2", "cfg-hw-dac -m 1"));
	CHECK(hifi_autoexec_ok("squeezelite -o BT -n kitchen"));
	CHECK(!hifi_autoexec_ok("restart"));
	CHECK(!hifi_autoexec_ok("nvs_set hifi_relay_key str -v x"));
	CHECK(!hifi_autoexec_ok("squeezelite\nrecovery"));
	CHECK(!hifi_autoexec_ok(NULL));
	CHECK(!hifi_autoexec_ok(""));
	/* cfg-hw-preset stores any NVS key its JSON names: an explicit list */
	CHECK(!hifi_lan_command_ok("cfg-hw-preset --model_config {\"name\":\"x\",\"config\":[{\"hifi_relay_url\":\"ws://evil\"}]}"));
	CHECK(!hifi_autoexec_ok("cfg-hw-preset --model_config x"));
	CHECK(!hifi_lan_command_ok("cfg-hw-dacx"));
	CHECK(!hifi_lan_command_ok("cfg-anything"));
	CHECK(hifi_lan_command_ok("cfg-syst-name -n kitchen"));
	CHECK(hifi_lan_command_ok("cfg-syst-services --AirPlay"));
	CHECK(hifi_autoexec_ok("cfg-audio-general -v 50"));
	/* squeezelite options that make it exit (and reboot the board 60 s later,
	 * at every boot), or that this build doesn't know */
	CHECK(hifi_autoexec_ok("squeezelite -o I2S -b 500:2000 -d all=info -C 30 -W")); /* the default */
	CHECK(hifi_autoexec_ok("squeezelite -o \"BT -n 'Speaker'\" -b 500:2000 -R -Z 96000 -r \"44100-44100\""));
	CHECK(hifi_autoexec_ok("squeezelite -o I2S -u m -s 192.168.1.10 -n \"Living room\""));
	CHECK(hifi_autoexec_ok("squeezelite -o I2S -u -n x"));
	CHECK(hifi_autoexec_ok("squeezelite"));
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
	CHECK(!hifi_lan_config_ok("autoexec1", "squeezelite -o I2S -t"));
	/* the name quoted as the console reads it: a quote ends the word, \" is a quote */
	CHECK(!hifi_autoexec_ok("squeezelite -n \"a\" -t \"b\""));
	CHECK(hifi_autoexec_ok("squeezelite -n \"a\\\" -t \\\"b\""));
	CHECK(!hifi_autoexec_ok("squeezelite -n a\\ b -t"));
	CHECK(hifi_autoexec_ok("squeezelite -n a\\ b"));
	CHECK(!hifi_autoexec_ok("squeezelite \\-t"));        /* \- drops both: "t" is a stray word */
	/* during an OTA trial: what a LAN request may not do */
	CHECK(hifi_lan_trial_refused("/api/v1/wifi", "POST"));
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
	CHECK(hifi_lan_config_secret("hifi_relay_pass"));
	CHECK(hifi_lan_config_secret("hifi_relay_key"));
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
