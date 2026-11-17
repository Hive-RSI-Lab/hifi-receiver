#include "hifi_gate.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

/* the suffixes home routers hand out with DHCP names */
/* Only the names the internet can't own (RFC 6762 .local, RFC 8375
 * .home.arpa, ICANN's reserved .internal). Router suffixes such as .lan,
 * .home, .box or .fritz.box are, or could become, public top-level domains:
 * someone who owns hifi-lite-4.<that> could rebind it to the board. */
static const char *const home_suffixes[] = {"local", "home.arpa", "internal"};

/* copy the host part of a Host header (no port, no brackets) */
static bool host_part(const char *host, char *out, size_t cap, bool *v6) {
	*v6 = false;
	size_t n;
	if (host[0] == '[') {
		const char *e = strchr(host, ']');
		if (!e) return false;
		if (e[1] && e[1] != ':') return false;
		host++;
		n = (size_t) (e - host);
		*v6 = true;
	} else {
		const char *c = strchr(host, ':');
		n = c ? (size_t) (c - host) : strlen(host);
		if (c && strchr(c + 1, ':')) return false; /* bare IPv6 is not a Host */
	}
	if (n == 0 || n >= cap) return false;
	memcpy(out, host, n);
	out[n] = 0;
	/* a trailing dot names the same host */
	if (!*v6 && out[n - 1] == '.') out[n - 1] = 0;
	return out[0] != 0;
}

static bool is_ipv4(const char *s) {
	int parts = 0;
	while (*s) {
		int v = 0, d = 0;
		while (isdigit((unsigned char) *s)) {
			v = v * 10 + (*s++ - '0');
			if (++d > 3 || v > 255) return false;
		}
		if (!d) return false;
		parts++;
		if (*s == '.') {
			s++;
			if (!*s) return false;
		} else if (*s) {
			return false;
		}
	}
	return parts == 4;
}

static bool is_ipv6(const char *s) {
	bool colon = false;
	for (; *s; s++) {
		if (*s == ':') colon = true;
		else if (*s == '%') return colon; /* zone id */
		else if (!isxdigit((unsigned char) *s) && *s != '.') return false;
	}
	return colon;
}

bool hifi_gate_host_is_ip(const char *host) {
	char h[64];
	bool v6;
	if (!host || !host_part(host, h, sizeof(h), &v6)) return false;
	return v6 ? is_ipv6(h) : is_ipv4(h);
}

static bool name_ok(const char *h, const char *const *names, size_t n) {
	for (size_t i = 0; i < n; i++) {
		const char *nm = names[i];
		/* a hostname is one label: one with a dot (renamed from the LAN to,
		 * say, a domain someone owns) would let that domain rebind to us */
		if (!nm || !*nm || strchr(nm, '.')) continue;
		size_t l = strlen(nm);
		if (strncasecmp(h, nm, l)) continue;
		if (h[l] == 0) return true;
		if (h[l] != '.') continue;
		for (size_t k = 0; k < sizeof(home_suffixes) / sizeof(home_suffixes[0]); k++)
			if (!strcasecmp(h + l + 1, home_suffixes[k])) return true;
	}
	return false;
}

bool hifi_gate_allow(const char *host, const char *origin, const char *const *names, size_t n) {
	if (!host || !*host) {
		/* no Host: not a browser. A browser always sends Host, and an
		 * Origin without one is not a request a browser makes. */
		return !origin || !*origin;
	}
	char h[128];
	bool v6;
	if (!host_part(host, h, sizeof(h), &v6)) return false;
	bool ok = v6 ? is_ipv6(h) : (is_ipv4(h) || name_ok(h, names, n));
	if (!ok) return false;
	if (!origin || !*origin) return true;
	/* Origin "null" comes from sandboxed frames and file pages: refused */
	if (strncasecmp(origin, "http://", 7)) return false;
	return !strcasecmp(origin + 7, host);
}

/* ---- squeezelite's UI from the LAN ---- */

static bool has_prefix(const char *s, const char *p) {
	return strncmp(s, p, strlen(p)) == 0;
}

/* split a console line exactly as esp_console_split_argv (IDF 4.4) does:
 * only a space parts words; a word that starts with a double quote runs to
 * the next one; a backslash before a backslash, quote or space gives that
 * character, before anything else both are dropped. false: more than max
 * words (the console would cut the line short, not this check). */
static bool split_words(const char *line, char *buf, size_t cap, char **argv, int max, int *argc) {
	enum { SPACE, ARG, QUOTED } st = SPACE;
	bool esc = false;
	size_t o = 0;
	int n = 0;
	for (const char *c = line; *c; c++) {
		int out = -1;
		if (esc) {
			if (*c == '\\' || *c == '"' || *c == ' ') out = *c;
			esc = false;
		} else if (st == SPACE) {
			if (*c == ' ') continue;
			if (n == max) return false;
			argv[n++] = buf + o;
			if (*c == '"') st = QUOTED;
			else if (*c == '\\') st = ARG, esc = true;
			else st = ARG, out = *c;
		} else if ((st == QUOTED && *c == '"') || (st == ARG && *c == ' ')) {
			out = 0;
			st = SPACE;
		} else if (*c == '\\') {
			esc = true;
		} else {
			out = *c;
		}
		if (out >= 0) {
			if (o + 1 >= cap) return false;
			buf[o++] = (char) out;
		}
	}
	buf[o] = 0;
	*argc = n;
	return true;
}

/* squeezelite's command line as this build parses it (components/squeezelite
 * main.c): an unknown option, a word left over, -t (license) or -? (help)
 * makes it exit(0), and cmd_squeezelite.c then reboots the board 60 s later,
 * at every boot; exit(1) or exit(2) (a failed malloc) leaves a dead player
 * until the next reboot. Only the options this build knows and that keep it
 * running pass: the ones with a value, then -W, and -u/-R (resampling, with
 * an optional value). Bare "squeezelite" gets -? added by the console (help,
 * then exit), so at least one option is needed; without -o the output is
 * I2S. -N is refused (with -n it exits; there is no file system for it
 * anyway), and -d needs what squeezelite's strtok needs: a name, '=', a level.
 * -b sets the stream and output buffers in KB (next_param on ':'). The sizes
 * squeezelite would end with are checked: stream >= 64, output >= 256, and
 * together <= 2600 KB, counting the default 1450 KB output as the 1740 KB a
 * crossfade grows it to (stock 500:2000 is 2500 and leaves 1.2 MB of PSRAM
 * free, its largest block 1.15 MB, for codecs, cspot and AirPlay). With
 * -u/-R and no output size, squeezelite multiplies the 1450 KB default by
 * the first rate / 44100 (1 to 8; 8 with no -r). -r: squeezelite's range
 * form writes one past rates[] when the top rate is above 768000 (it zeroes
 * the MAC), so every rate must be 768000 or less; a list of 18 leaves no 0
 * to end it, so 17 at most; and a non-zero rate under 8000 is refused. */
static bool debug_ok(const char *v) {
	while (*v == '=') v++;
	size_t l = strcspn(v, "=");
	if (!l) return false;
	v += l;
	while (*v == '=') v++;
	return *v != 0;
}

#define SQ_STREAM_KB 480  /* STREAMBUF_SIZE on ESP32 */
#define SQ_OUTPUT_KB 1450 /* OUTPUTBUF_SIZE */
#define SQ_OUTPUT_XFADE_KB 1740 /* OUTPUTBUF_SIZE_CROSSFADE */
#define SQ_STREAM_MIN_KB 64
#define SQ_OUTPUT_MIN_KB 256
#define SQ_BUFFERS_MAX_KB 2600
#define SQ_RATE_MAX 768000
#define SQ_RATE_MIN 8000 /* the lowest of squeezelite's TEST_RATES */
#define SQ_RATES_MAX 17 /* MAX_SUPPORTED_SAMPLERATES 18 less the 0 that ends the list */

/* one -b value: "S", "S:O", ":O" or "S:", each part 1-4 digits; an empty
 * part leaves that size as it was (next_param returns NULL for "") */
static bool buffers_ok(const char *v, unsigned *stream_kb, unsigned *output_kb) {
	unsigned part[2] = {0, 0};
	int len[2] = {0, 0}, k = 0;
	for (; *v; v++) {
		if (*v == ':') {
			if (++k > 1) return false;
		} else if (*v >= '0' && *v <= '9') {
			if (++len[k] > 4) return false;
			part[k] = part[k] * 10 + (unsigned) (*v - '0');
		} else {
			return false;
		}
	}
	if (!len[0] && !len[1]) return false;
	if (len[0]) *stream_kb = part[0];
	if (len[1]) *output_kb = part[1];
	return true;
}

/* one -r value: "<rates>[:<delay>]", rates a comma list of up to 17
 * ("44100,48000") or "<max>", "<min>-<max>" (either side may be empty);
 * digits only, each rate 0 or 8000 to 768000, the delay up to 5 digits. An empty
 * rates part leaves the rates as they were. *top: squeezelite's rates[0],
 * the highest rate given. */
static bool rates_ok(const char *v, unsigned *top) {
	unsigned n = 0, hi = 0;
	int digits = 0, commas = 0, dashes = 0, any = 0;
	for (; *v && *v != ':'; v++) {
		if (*v >= '0' && *v <= '9') {
			if (++digits > 6) return false;
			n = n * 10 + (unsigned) (*v - '0');
			if (n > SQ_RATE_MAX) return false;
			any = 1;
			continue;
		}
		if (digits && n && n < SQ_RATE_MIN) return false;
		if (*v == ',') {
			if (!digits || dashes || ++commas >= SQ_RATES_MAX) return false;
		} else if (*v == '-') {
			if (commas || ++dashes > 1) return false;
		} else {
			return false;
		}
		if (n > hi) hi = n;
		n = 0, digits = 0;
	}
	if (commas && !digits) return false; /* "44100," */
	if (digits && n && n < SQ_RATE_MIN) return false;
	if (dashes && !any) return false;    /* "-" means 768000 to squeezelite */
	if (n > hi) hi = n;
	if (*v == ':') {
		int d = 0;
		for (v++; *v; v++)
			if (*v < '0' || *v > '9' || ++d > 5) return false;
	}
	if (any) *top = hi;
	return true;
}

static bool squeezelite_args_ok(char **argv, int argc) {
	if (argc < 2) return false;
	unsigned stream_kb = SQ_STREAM_KB, output_kb = 0, top_rate = 0;
	bool resample = false;
	for (int i = 1; i < argc;) {
		const char *a = argv[i];
		if (a[0] != '-' || !a[1] || a[2]) return false;
		if (strchr("oabcCdefmMnpPrsZ", a[1])) {
			if (i + 1 >= argc) return false;
			if (a[1] == 'd' && !debug_ok(argv[i + 1])) return false;
			if (a[1] == 'b' && !buffers_ok(argv[i + 1], &stream_kb, &output_kb)) return false;
			if (a[1] == 'r' && !rates_ok(argv[i + 1], &top_rate)) return false;
			i += 2;
		} else if (a[1] == 'W') {
			i += 1;
		} else if (a[1] == 'u' || a[1] == 'R') {
			resample = true;
			i += 1;
			if (i < argc && argv[i][0] != '-') i += 1;
		} else {
			return false;
		}
	}
	if (!output_kb) {
		unsigned scale = 1;
		if (resample) {
			scale = top_rate ? top_rate / 44100 : 8;
			if (scale > 8) scale = 8;
			if (scale < 1) scale = 1;
		}
		output_kb = SQ_OUTPUT_KB * scale;
	}
	unsigned held = output_kb == SQ_OUTPUT_KB ? SQ_OUTPUT_XFADE_KB : output_kb;
	return stream_kb >= SQ_STREAM_MIN_KB && output_kb >= SQ_OUTPUT_MIN_KB &&
	       stream_kb + held <= SQ_BUFFERS_MAX_KB;
}

/* the settings commands a LAN page may run, and an autoexecN line may hold.
 * Not cfg-hw-preset: it stores any NVS key its JSON names (hifi_relay_*,
 * fwurl, autoexec1), past every check on config.json. */
static const char *const cfg_ok[] = {
	"cfg-audio-bt_source", "cfg-audio-general", "cfg-audio-squeezelite", "cfg-hw-dac",
	"cfg-hw-display", "cfg-hw-i2c", "cfg-hw-ledvu", "cfg-hw-rotary",
	"cfg-hw-spdif", "cfg-hw-spi", "cfg-syst-cspot", "cfg-syst-name",
	"cfg-syst-services",
};

static bool word_in(const char *w, size_t n, const char *const *list, size_t count) {
	for (size_t i = 0; i < count; i++)
		if (strlen(list[i]) == n && strncmp(w, list[i], n) == 0) return true;
	return false;
}

/* boot: lines kept in autoexecN run on the console at every boot, so only
 * squeezelite's own command line and the cfg-* setters may live there
 * ("restart" in autoexec1 would reboot the board forever).
 * The console (platform_console.c) cuts a line at 599 bytes and keeps 27
 * words (max_cmdline_length 600, max_cmdline_args 28 with the NULL), both
 * silently: what it would run is not what was checked, so longer is refused. */
#define CONSOLE_LINE_MAX 600
#define CONSOLE_WORDS_MAX 27
static bool command_ok(const char *line, bool boot) {
	if (!line || strlen(line) >= CONSOLE_LINE_MAX) return false;
	/* one line, one command: a newline or other control character could
	 * carry a second command past the check on the first word */
	for (const char *c = line; *c; c++)
		if ((unsigned char) *c < 0x20 || *c == 0x7f) return false;
	char buf[CONSOLE_LINE_MAX];
	char *argv[CONSOLE_WORDS_MAX];
	int argc = 0;
	if (!split_words(line, buf, sizeof(buf), argv, CONSOLE_WORDS_MAX, &argc)) return false;
	/* the command name as the console sees it (it splits on spaces only;
	 * a tab is refused above) */
	if (!argc) return false;
	line = argv[0];
	size_t n = strlen(line);
	if (word_in(line, n, cfg_ok, sizeof(cfg_ok) / sizeof(cfg_ok[0]))) return true;
	if (n == 11 && !strncmp(line, "squeezelite", 11)) return squeezelite_args_ok(argv, argc);
	static const char *const ok[] = {"restart", "version", "free", "heap", "tasks"};
	return !boot && word_in(line, n, ok, sizeof(ok) / sizeof(ok[0]));
}

bool hifi_lan_command_ok(const char *line) {
	return command_ok(line, false);
}

bool hifi_autoexec_ok(const char *line) {
	return command_ok(line, true);
}

bool hifi_lan_config_ok(const char *key, const char *strval) {
	if (!key || !*key) return false;
	if (!strcmp(key, "fwurl")) return false;
	/* hifi_* keys are this firmware's own (relay, update URL, the saved
	 * command line of the Bluetooth test, rollback notes): its own API sets
	 * the ones a user may change, squeezelite's settings page never does */
	static const char *const no[] = {"telnet", "hifi_"};
	for (size_t i = 0; i < sizeof(no) / sizeof(no[0]); i++)
		if (has_prefix(key, no[i])) return false;
	if (has_prefix(key, "autoexec") && key[8]) /* autoexec1.. hold console lines; "autoexec" is the on/off flag */
		return strval && (!*strval || hifi_autoexec_ok(strval));
	return true;
}

/* during an OTA trial, what may roll the new firmware back: a reboot (the
 * bootloader then goes back to the old image), a Wi-Fi change or forget (no
 * Wi-Fi at the verdict), an erase, or squeezelite's settings and console,
 * which reach all three */
bool hifi_lan_trial_refused(const char *path, const char *method) {
	if (!path || !method || !strcmp(method, "GET") || !strcmp(method, "HEAD")) return false;
	/* the server routes on the parsed path: an absolute-form target
	 * (http://ip/reboot.json) or a '#' would slip past the names below */
	if (path[0] != '/' || strchr(path, '#')) return true;
	size_t n = strcspn(path, "?");
	static const char *const paths[] = {
		"/api/v1/wifi", "/api/v1/wifi/forget", "/api/v1/reset", "/api/v1/reboot", "/api/v1/bttest",
		"/connect.json", "/reboot.json", "/reboot_ota.json", "/commands.json", "/config.json",
	};
	return word_in(path, n, paths, sizeof(paths) / sizeof(paths[0]));
}

bool hifi_lan_config_secret(const char *key) {
	return key && (!strcmp(key, "hifi_relay_pass") || !strcmp(key, "hifi_relay_key"));
}

/* ---- the setup access point ---- */

const char *hifi_ap_password_pick(const char *configured, const char *public_default, bool wifi_saved) {
	if (!configured || !*configured || (public_default && !strcmp(configured, public_default)))
		return wifi_saved ? NULL : (configured && *configured ? configured : public_default);
	return configured;
}

void hifi_ap_password_make(char *out, size_t n, uint32_t (*rnd)(void)) {
	static const char abc[] = "abcdefghijkmnpqrstuvwxyz23456789"; /* 32: no l, o, 0, 1 */
	if (!out || !n) return;
	if (n > 64) n = 64;
	for (size_t i = 0; i + 1 < n; i++) out[i] = abc[rnd() & 31];
	out[n - 1] = 0;
}

bool hifi_ap_due(long long now_us, long long lost_at_us) {
	return now_us - lost_at_us >= HIFI_AP_AFTER_US;
}
