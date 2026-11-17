# ESP-IDF v4.4.8 security backports

ESP-IDF v4.4 is end of life (July 2024), so these fixes never shipped for it.
`tools/fetch.sh` copies each patched IDF component (`bt`, `lwip`, `mdns`, `mbedtls`,
`wpa_supplicant`, `esp_phy`, `esp_netif`) out
of `$IDF_PATH` into the build tree's `components/`, where it overrides the
IDF's own copy, and applies the patch there. The shared IDF install is never
touched. `fetch.sh` refuses an IDF that isn't 4.4.8. Patches apply in file
name order (the same under C and UTF-8 collation), so a later round's file for
a component sorts after the earlier one (`bt-r2-*` after `bt-bluedroid-*`).

| patch | upstream fix | CVE / advisory | how |
|---|---|---|---|
| `bt-bluedroid-cves.patch` | esp-idf c8cd8f4 (L2CAP config options length checks) | CVE-2026-81507, GHSA-v335-fxwc-rc44 | clean cherry-pick |
| | esp-idf 60f9362f (AVRCP `avrc_pars_vendor_cmd` OOB read) | CVE-2026-46532, GHSA-3pp8-42fh-3j3c | clean cherry-pick |
| | esp-idf 075ed218 (AVRCP/SDP/RFCOMM/SMP bounds) | CVE-2025-66409, GHSA-qhf9-vr2h-jh96 | cherry-pick; one hunk in `sdp_discovery.c` merged by hand (4.4 lacks the line above it); the `sdp_api.h` change of `array[4]` to `array[]` left out (gcc 8.4 refuses a flexible array in a union; same size on the ESP32) |
| | esp-idf 565fa98d (AVRCP vendor OOB) | CVE-2025-68474, GHSA-43gh-7r4f-qp57 | clean cherry-pick |
| | esp-idf 6ca6f422 (`bta_dm_sdp_result` OOB write) | CVE-2025-68473, GHSA-hmjj-rjvv-w8pq | clean cherry-pick |
| `lwip-dhcps-cve-2026-45160.patch` | esp-idf 8b4b5d53 (DHCP server `parse_options` OOB read) | CVE-2026-45160, GHSA-g764-gwc3-75m5 | hand port: 4.4's `parse_options` has no `dhcps` argument |
| `mdns-cve-2026-64860.patch` | esp-protocols 95c882c7 (mdns 1.11.2: name decompression depth cap, rdata bound) | CVE-2026-64860, GHSA-7fpr-vjw5-2c86 | hand port into 4.4's single `mdns.c` |

Round 2 (2026-11-17, `security.md` "Patches and CVEs"; the full list with
reasons is in `projects/hifi-lite-lab/cve-survey.md`). Each `bt-r2-*` file
bundles several small upstream commits:

| patch | upstream fixes | what |
|---|---|---|
| `bt-r2-1-hci-l2cap.patch` | 0126907574 + 96f092b66a, 99aee96317, 15e0c748e5, b70e8ae903 | HCI rx length checks, L2CAP command OOB, NULLs in the state machine and FCR |
| `bt-r2-2-sdp.patch` | 11eea1b4de, 158519cf8c, be25062b0d, 46baa2dac1, 5a2b29fe82 | SDP client and server lengths |
| `bt-r2-3-avdtp-a2dp-sbc.patch` | 4cb6ccc6f6, f15fe75f24, 4df287c536 (AVDT), fa22e34f56, 31d51adf4f, 5872a8cccb, e7e0be698a | AVDT reassembly, RTCP and media offsets, setconfig reject, SBC bitpool check (was debug-only) |
| `bt-r2-4-avctp-avrcp.patch` | 1ba8aaaff8, 458150888d, 39c167b539, b7d0ef96a7, 4df287c536 (AVCT), 173747750d (hand), own fix | AVRCP parsers, AVCT callbacks; own: the `btc_avrc.c` VLA sized by the phone (up to about 3 KB on a 3 KB task) moved to the heap |
| `bt-r2-5-gap-btm-smp.patch` | 1cd73de8d9, 1f4e41d2ac, cf5a568dc0 + 65833e571d, 33af3a0aa6, 70f82a5607 (hand), 1ef83cc8db | EIR bounds, SMP over BR/EDR length, `btm_sec` use-after-free and NULL |
| `lwip-r2-tcp-ooseq-fin.patch` | esp-lwip fe4fb18c11 | empty FIN into the out-of-order queue |
| `lwip-r2-ip6frag-sizeof.patch` | esp-lwip 91ad363baf | `sizeof` of a pointer in IPv6 reassembly |
| `lwip-r2-nd6-lladdr.patch` | esp-lwip 59107adec8 | ND6 link-layer address length |
| `lwip-r2-dhcp-parse-pbufchain.patch` | lwip 73b45e2642 | DHCP option split over 3+ pbufs |
| `lwip-r2-tcp-optbyte.patch` | lwip 3d896ba0a3 | TCP option index bound |
| `lwip-r2-nd6-ptb-minmtu.patch` | lwip d08f4773ed | Packet Too Big minimum MTU |
| `lwip-r2-dhcp-mtu-min.patch` | esp-idf 279b117202 | DHCP MTU at least 68 |
| `lwip-r2-dhcps-handle-len.patch` | esp-idf 9b8a8e5b2d + 95cc19674d | DHCP server message length |
| `lwip-r2-dhcps-parse-msg-nullcheck.patch` | esp-idf 26c44b765f | DHCP server NULL on OOM |
| `mbedtls-cve-2026-49300-basicconstraints.patch` | Mbed TLS 2026-07 advisory fix, ported to 2.28 | CVE-2026-49300, forged CA bit in a chain |
| `mbedtls-crt-bundle-drop-emsign.patch` | esp-idf 52ec7af8d2, f67793f49e | drops three emSign roots Mozilla removed. The Comodo AAA removal (614fb7a6df) is left out: a server that sends AAA as the top of its chain would fail, and AAA expires 2028-12-31 anyway |
| `mbedtls-crt-bundle-accept-cross-signed-roots.patch` | esp-idf 79707d416f | the bundle callback accepts a cross-signed root at any depth; bounded issuer compare |
| `wifi-sae-token-len.patch` | esp-idf cad36beb4c | WPA3 SAE anti-clogging token: freed but not NULLed, length wrap |
| `wifi-sae-h2e-null-token.patch` | esp-idf 9de31f008c | SAE H2E write through a NULL token |
| `wifi-memcmp-const.patch` | esp-idf 77eead46cf | `os_memcmp_const` was plain `memcmp` |
| `wifi-hardening.patch` | esp-idf 342ae2d231, c9472256be | SAE and WPA IE checks on OOM / malformed IEs |
| `bt-r2-6-review.patch` | own, from the round-2 review | `smp_act.c` NULL `btm_find_dev`; AVDT multiplexing copy uses the adjusted length; AVRCP company-ID capability length is 3 bytes each |
| `wifi-r2-review.patch` | esp-idf cad36beb4c, c9472256be hunks the first port left out | SoftAP passphrase with no NUL at 64 characters; RRM and extended-capability IE lengths |
| `wifi-lib-printf.patch` | esp-idf 5ca9e054c9 | stack bounds in esp_phy's log shim |
| `netif-hostname-null.patch`, `netif-null-args.patch` | esp-idf 352f4997fa, 9e52922932 | `esp_netif` NULL checks |

Keep the bundle patches together: the cross-signed callback is what lets chains through after a root is dropped.

Held, with reasons in `security.md`: lwip 6a3a944207 (SYN_RCVD sequence
check, one day old upstream), esp-idf 9669403338 (PTK with no PMK, reachability
in the closed library unverified), nvs 79b93d1db7 (changes how existing flash
is read; needs physical access), httpd 370cbcaff9 (the firmware's own
`components/esp_http_server` would be replaced by an override).

To redo one: copy the component from `$IDF_PATH/components` into a scratch git
repo, commit, apply the upstream commit (`https://github.com/espressif/esp-idf/commit/<sha>.patch`),
fix what doesn't apply, and `git diff -- components/<name>` back into the file.
