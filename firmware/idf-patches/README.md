# ESP-IDF v4.4.8 security backports

ESP-IDF v4.4 is end of life (July 2024), so these fixes never shipped for it.
`tools/fetch.sh` copies each patched IDF component (`bt`, `lwip`, `mdns`) out
of `$IDF_PATH` into the build tree's `components/`, where it overrides the
IDF's own copy, and applies the patch there. The shared IDF install is never
touched. `fetch.sh` refuses an IDF that isn't 4.4.8.

| patch | upstream fix | CVE / advisory | how |
|---|---|---|---|
| `bt-bluedroid-cves.patch` | esp-idf c8cd8f4 (L2CAP config options length checks) | CVE-2026-81507, GHSA-v335-fxwc-rc44 | clean cherry-pick |
| | esp-idf 60f9362f (AVRCP `avrc_pars_vendor_cmd` OOB read) | CVE-2026-46532, GHSA-3pp8-42fh-3j3c | clean cherry-pick |
| | esp-idf 075ed218 (AVRCP/SDP/RFCOMM/SMP bounds) | CVE-2025-66409, GHSA-qhf9-vr2h-jh96 | cherry-pick; one hunk in `sdp_discovery.c` merged by hand (4.4 lacks the line above it); the `sdp_api.h` change of `array[4]` to `array[]` left out (gcc 8.4 refuses a flexible array in a union; same size on the ESP32) |
| | esp-idf 565fa98d (AVRCP vendor OOB) | CVE-2025-68474, GHSA-43gh-7r4f-qp57 | clean cherry-pick |
| | esp-idf 6ca6f422 (`bta_dm_sdp_result` OOB write) | CVE-2025-68473, GHSA-hmjj-rjvv-w8pq | clean cherry-pick |
| `lwip-dhcps-cve-2026-45160.patch` | esp-idf 8b4b5d53 (DHCP server `parse_options` OOB read) | CVE-2026-45160, GHSA-g764-gwc3-75m5 | hand port: 4.4's `parse_options` has no `dhcps` argument |
| `mdns-cve-2026-64860.patch` | esp-protocols 95c882c7 (mdns 1.11.2: name decompression depth cap, rdata bound) | CVE-2026-64860, GHSA-7fpr-vjw5-2c86 | hand port into 4.4's single `mdns.c` |

To redo one: copy the component from `$IDF_PATH/components` into a scratch git
repo, commit, apply the upstream commit (`https://github.com/espressif/esp-idf/commit/<sha>.patch`),
fix what doesn't apply, and `git diff -- components/<name>` back into the file.
