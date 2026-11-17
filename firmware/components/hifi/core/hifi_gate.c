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

bool hifi_lan_command_ok(const char *line) {
	if (!line) return false;
	/* one line, one command: a newline or other control character could
	 * carry a second command past the check on the first word */
	for (const char *c = line; *c; c++)
		if (((unsigned char) *c < 0x20 && *c != '\t') || *c == 0x7f) return false;
	while (*line == ' ' || *line == '\t') line++;
	size_t n = strcspn(line, " \t");
	if (!n) return false;
	if (n > 4 && has_prefix(line, "cfg-")) return true;
	static const char *const ok[] = {"squeezelite", "restart", "version", "free", "heap", "tasks"};
	for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++)
		if (strlen(ok[i]) == n && strncmp(line, ok[i], n) == 0) return true;
	return false;
}

bool hifi_lan_config_ok(const char *key, const char *strval) {
	if (!key || !*key) return false;
	if (!strcmp(key, "fwurl")) return false;
	static const char *const no[] = {"telnet", "hifi_relay", "hifi_update", "hifi_remote"};
	for (size_t i = 0; i < sizeof(no) / sizeof(no[0]); i++)
		if (has_prefix(key, no[i])) return false;
	if (has_prefix(key, "autoexec") && key[8]) /* autoexec1.. hold console lines; "autoexec" is the on/off flag */
		return strval && (!*strval || hifi_lan_command_ok(strval));
	return true;
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
