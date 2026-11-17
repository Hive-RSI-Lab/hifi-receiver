/*
 * The LAN gate: which browser requests the board's web server answers.
 *
 * Nothing on port 80 asks for a password (security.md), so the gate stops
 * the two ways a web page on the internet could reach it through a browser
 * on the home network:
 *   - DNS rebinding: the page's own name is pointed at the board, so the
 *     request carries the page's name in Host. The gate answers only an IP
 *     address or the board's own name (alone, or with .local, .home.arpa
 *     or .internal: suffixes nobody on the internet can register).
 *   - Cross-site requests (CSRF): a page elsewhere posts to the board's IP.
 *     The browser sends that page's Origin; the gate needs Origin, when
 *     present, to be this same board (http://<Host>).
 * A request with no Host (HTTP/1.0 tools) passes: browsers always send it.
 */
#ifndef HIFI_GATE_H
#define HIFI_GATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* names: the board's host names (e.g. "hifi-lite-4"), n of them.
 * true when the request may be served. */
bool hifi_gate_allow(const char *host, const char *origin, const char *const *names, size_t n);

/* the Host header names an IPv4 or IPv6 address (port allowed) */
bool hifi_gate_host_is_ip(const char *host);

/* ---- what squeezelite's own UI (port 80) may change from the LAN ----
 * squeezelite's config.json and commands.json reach NVS and the console, which
 * also hold the Hive's dial-in (hifi_relay_*), the update URL and telnet.
 * A LAN page may still set up Wi-Fi, audio, hardware and the player. */

/* config.json POST: may the LAN set key to strval (strval NULL: not a string)?
 * Refused: fwurl (an unsigned update from any URL), telnet*, hifi_relay*,
 * hifi_update*, hifi_remote*; autoexecN only when its line is one the LAN
 * may run (hifi_lan_command_ok), since it runs on the console at boot. */
bool hifi_lan_config_ok(const char *key, const char *strval);

/* config.json GET: is key a secret to blank out (the Hive's credentials)? */
bool hifi_lan_config_secret(const char *key);

/* commands.json POST: may the LAN run this console line? Only the settings
 * commands (cfg-*), squeezelite, restart and read-only system info; never
 * nvs_*, api, hifi, update, recovery, restart_ota, join, i2c*, sleep. */
bool hifi_lan_command_ok(const char *line);

/* ---- the setup access point (security.md, firmware/patches/0009) ----
 * First setup keeps the public password (nothing on the board to protect
 * yet). Once a Wi-Fi network is saved, an AP still on the public password
 * gets a random one of its own, kept in NVS as ap_pwd: the serial log and
 * squeezelite's settings page (config.json, from the LAN) show it. */

/* the AP password to start with: configured unless it is the public default
 * and Wi-Fi is saved, in which case NULL (make one with hifi_ap_password_make) */
const char *hifi_ap_password_pick(const char *configured, const char *public_default, bool wifi_saved);

/* n-1 characters (n <= 64) from an alphabet with no look-alikes, then '\0' */
void hifi_ap_password_make(char *out, size_t n, uint32_t (*rnd)(void));

/* with Wi-Fi saved, the AP comes up only after the board has been without
 * it this long (not on every router reboot) */
#define HIFI_AP_AFTER_US (10LL * 60 * 1000 * 1000)
bool hifi_ap_due(long long now_us, long long lost_at_us);

#endif
