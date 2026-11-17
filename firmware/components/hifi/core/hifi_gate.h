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
 * Refused: fwurl (an unsigned update from any URL), telnet*, every hifi_*
 * key (the relay, the update URL, the Bluetooth test's saved command line);
 * autoexecN only when its line may run at every boot (hifi_autoexec_ok). */
bool hifi_lan_config_ok(const char *key, const char *strval);

/* config.json GET: is key a secret to blank out (the Hive's credentials)? */
bool hifi_lan_config_secret(const char *key);

/* commands.json POST: may the LAN run this console line? Only the settings
 * commands (an explicit cfg-* list, without cfg-hw-preset), squeezelite with
 * options that keep it running, restart and read-only system info; never
 * nvs_*, api, hifi, update, recovery, restart_ota, join, i2c*, sleep. */
bool hifi_lan_command_ok(const char *line);

/* cfg-hw-* lines (L8), from the LAN and in autoexecN: only the command's own
 * options, by exact long name ("--data 26" or "--data=26"), and no pin
 * option may name a reserved pin. Always reserved: the flash (GPIO 6-11)
 * and the WROVER's PSRAM (16, 17); the board adds the pins its firmware
 * holds (HIFI_LAN_RESERVED_PINS in its header) before the web server starts. */
#define HIFI_GATE_PINS_ALWAYS (0x3FULL << 6 | 3ULL << 16)
void hifi_gate_reserve_pins(uint64_t mask);

/* A line that may be kept in autoexecN (run at every boot): squeezelite's
 * command line (no option that makes it exit) or a cfg-* setter, one line. Also checked where the firmware
 * copies a saved line back into autoexec1 (the Bluetooth test). */
bool hifi_autoexec_ok(const char *line);

/* While new firmware is on trial (not yet marked good), may a LAN request
 * with this method and path (query allowed) roll it back? true for every
 * method but GET/HEAD on: /api/v1/ wifi, wifi/forget, reset, reboot, bttest;
 * squeezelite's /connect.json, /reboot.json, /reboot_ota.json,
 * /commands.json, /config.json. The caller answers 409 then. */
bool hifi_lan_trial_refused(const char *path, const char *method);

/* ---- GETs a web page elsewhere could fire (L7) ----
 * A browser sends no Origin on a plain cross-site GET (an <img>, a link),
 * and sends Sec-Fetch-Site only to https or localhost, never to the board's
 * http:// address: the gate can't tell such a GET from the app's own. The
 * page can't read the answer, so what is left is the work a GET starts:
 * a Wi-Fi scan (/scan.json, /api/v1/wifi/scan: the radio leaves the
 * channel, audio can stutter) or a fetch of the update manifest
 * (/api/v1/update/check). Those run at most once per HIFI_LAN_GAP_US from the
 * LAN; true: refuse this one (the caller answers 503, try again).
 * *last_us holds the time of the last one let through (0: none yet). */
#define HIFI_LAN_GAP_US (5LL * 1000 * 1000)
bool hifi_lan_throttled(const char *path, const char *method, long long now_us, long long *last_us);

/* ---- the setup access point (security.md, firmware/patches/0010) ----
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

/* ---- a factory reset from the LAN (L6) ----
 * An erase wipes the saved Wi-Fi and the AP's own password, so the setup AP
 * comes back on the public one: from the LAN a reset only arms (hifi_reset_armed
 * in NVS), and it happens only if the board next starts from power-on, which
 * takes a hand on the plug. Any other start (a reboot, a crash, an update)
 * disarms it, and so does HIFI_RESET_ARM_US going by. USB and the relay
 * reset at once. true: erase now. */
#define HIFI_RESET_ARM_US (10LL * 60 * 1000 * 1000)
bool hifi_reset_confirmed(bool armed, bool power_on);

#endif
