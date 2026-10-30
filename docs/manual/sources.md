# Where the manual's facts come from

The manual ([`user-manual.md`](user-manual.md), [`quick-start.md`](quick-start.md))
has to stay in step with the boards, the firmware, the web app and the
enclosure. This page lists each fact the manual depends on, where it comes
from, and its status. When something here changes, change the manual too.

`grep -rn "TBD" docs/manual/` lists every open item in the text.

Checked against the as-built files on 2026-10-28 (task 261028-191739):
the firmware on its branch `hive/261028-165619-70c34f28b3` at 0878316, the
web app on `hive/261028-165625-c782eda91e` at 80f6763, and the Lite board and
enclosure on main at 18ceb29.

**Status:**

- **source:** taken from a committed design file.
- **proposed:** a behaviour the manual describes that no work has fixed yet.
  The firmware or app may adopt it as specified, or change it and correct
  the manual.
- **TBD:** not known yet. It is marked in the text.

## From `docs/product.md` (source)

| Fact | Manual section |
|---|---|
| Tier names, chargers (20 V / 28 V EPR, 140 W, 240 W cable) | QS 2, §3, §5 |
| Power per channel: 43 / 77 W at 4 Ω; 39 W at 20 V fallback; 8 Ω figures | §3, §5, §13 |
| Zone B shares 140 W; no parallel speakers (2 Ω) | QS 1, §2, §3 |
| 5 V port boots with the amp held off | QS 2, §5, §14 |
| Power governor: 90% of the PDO, averaged over 1 s | §5 |
| DAC/amp/ADC part numbers, 24/96 target | §13 |
| Sub outs: Lite full range; Plus/Ultra bass-managed (crossover, level, delay, phase), optional mains high-pass | §4, §9 |
| Lite line in is the codec's analog bypass; Plus/Ultra DJ direct switch; 5–15 ms digital path | §4, §9 |
| Ultra optical in (TOSLINK) | §4 |
| Wi-Fi 2.4 GHz only; Ethernet on Plus/Ultra | §6 |
| Streaming: Spotify (cspot, Premium), AirPlay 1, BT A2DP SBC, Lyrion | §1, §7 |
| Installable PWA; LMS apps also work (product.md's `hifi.local` became `hifi-<model>.local`, see the web app) | §8 |
| Plus knob + source + preset; Ultra screen + knob + 3 buttons (Lite's buttons now come from the firmware) | §8 |
| IR learn mode; Plus/Ultra ship a 21-key NEC remote | §8 |
| CH340C serial on USB-C; `_hifi._tcp` mDNS; test API (PD, VBUS I, faults, tones, self-test) | §14 |
| Enclosure PETG/ASA, not PLA (heatsink 60–80 °C) | §2, §13 |
| Not certified: cspot, AirPlay 1, BT SIG, FCC/CE; the module's modular approval | §15 |
| Lite power is calculated from the 10.9 W/6 Ω/12 V board | §13 |

## From the boards and the enclosure

| Fact | Source | Status |
|---|---|---|
| Lite rear, left to right: speaker sockets low (R− R+ L− L+), USB-C low, SUB OUT, LINE IN; upper RCA = left (white) | `enclosure/README.md` (Panels), `boards/lite-main/README.md` | source |
| Lite speaker outputs: four board-mounted 4 mm banana sockets (Amass 24.243, J4-J7) at 11 mm pitch (single plugs only), nothing wired by hand | `enclosure/README.md`, `boards/lite-main/README.md` | source |
| Lite top: vol−, vol+, ▶ left to right along the front edge; status light pipe to their left; IR window front, bottom right | `enclosure/README.md` | source |
| Lite status LED is one red LED (D2) | `boards/lite-main/README.md` | source |
| Lite case 106 × 96 × 46.8 mm, PETG/ASA; no moulded labels yet (sticker) | `enclosure/README.md` | source |
| Ultra front: screen, knob (EC11, no detents), 3 buttons, IR, RGB status LED | `boards/ultra-front/README.md` | source |
| Plus control: knob, 2 buttons, IR, status LED; SUB1/SUB2; screw speaker terminals | `boards/core/README.md` | source |
| Plus and Ultra rear panels, size, terminals | their enclosures | TBD |
| Ventilation clearance: 5 cm above and behind | the manual | proposed |

**Mismatch left for the board and enclosure:** both READMEs call SW3 "play/source
(hold = BT pairing)". The firmware has no pairing mode and maps hold to standby;
the manual follows the firmware.

## From the firmware (261028-165619-70c34f28b3)

Its `docs/firmware.md`, `docs/test-api.md`, `firmware/board/lite-main/sdkconfig.defaults`
and `firmware/patches/0001-hifi-receiver-hooks.patch`, and what its worker told the lead at 16:35.

| Fact | Manual | Status |
|---|---|---|
| Hostname `hifi-lite` (`hifi-lite.local`); a second box is `hifi-lite-2.local` | §6, §8 | source |
| Lite buttons: − press = volume down, hold 1 s = previous; + press = up, hold = next; ▶ press = play/pause, hold = standby; no double press | QS 5, §8 | source |
| No Bluetooth pairing mode: discoverable whenever no phone is connected | QS 4, §7 | source |
| Factory reset from the app only (`POST /api/v1/reset {factory:true}`); it erases all of NVS, so Bluetooth phones and remote keys too | §10 | source |
| Status light blinks during Wi-Fi setup; no blue | §11 | source; the steady pattern is TBD at bring-up |
| Setup network: SSID `hifi-lite` with squeezelite-esp32's MAC suffix (`register_default_with_mac`), password `squeezelite`, portal at 192.168.4.1 | QS 3, §6, §10 | source (sdkconfig); the exact suffix format is to confirm at bring-up |
| Default name in Spotify, AirPlay and Bluetooth: `hifi-lite` | QS 4, §7 | source (sdkconfig); Bluetooth's may carry the same MAC suffix, to confirm at bring-up |
| Amp off after 10 min of digital silence, back on the first sound | §5 | source |
| Under/over-voltage turns the speakers off, back after 1 s in range | §5, §12 | source |
| The power limiter can't see DJ direct (analog) | §4 | source |
| 16-bit up to 96 kHz (`DEPTH=16`) on Lite | §7, §13 | source; Plus/Ultra TBD |
| Lite sub out: LOUT2/ROUT2, the same stereo signal as the speakers, full range | §4, §13 | source |
| `/squeezelite/` for squeezelite-esp32's own settings | §8 | source |
| Updates: squeezelite-esp32's OTA (URL) and recovery (file); the factory slot is `recovery.bin` | §10 | source; where updates are published is TBD |
| Not in the API yet: absolute volume, mute, max volume, now-playing, IR learn | §8, §12 | source; the manual says these need a firmware update |
| Test API under `/api/v1/` (info, state, tone, amp, input, sub, eq, name, reboot, reset, control, selftest); console `hifi …` | §14 | source |
| Licence: upstream at the commit in `firmware/upstream.lock`; ours in `firmware/patches/`, `firmware/components/` | §15 | source |
| Number of remembered Bluetooth phones; time before the setup network comes back; which stream wins | §6, §7 | TBD |
| Plus and Ultra firmware: buttons, remote key map, LED, screen dimming, bit depth | §8, §11, §13 | TBD: not built yet |

## From the web app (261028-165625-c782eda91e)

Its `docs/app.md` and `app/web/app.js`.

| Fact | Status |
|---|---|
| Tabs **Play**, **Sound**, **Setup**; the app shows only what `caps` says the tier has | source |
| Play: now playing, Previous, Play / pause, Next, Volume, Mute, **Input** (Streaming, DJ direct; Plus/Ultra Line in; Ultra Optical), Zone B speakers | source |
| No source picker for streams; no Bluetooth pair button (the Name card says the receiver can be found when no other phone is connected) | source |
| Sound: Equaliser (10 bands, ±12 dB, presets Flat, Bass +, Loudness, Vocal, Treble -), Subwoofers (Sub outputs, Level, Crossover 40–160 Hz, Take the deep bass off the main speakers; per sub Trim −12..+6 dB, Delay 0–20 ms, Phase 0°/180°), Volume limit 30–100 | source |
| Setup: Wi-Fi (Scan for networks, Choose, Join, Forget), Name, Remote (Teach per action, Forget all), Updates (Check for updates, Install, From a file…), About and diagnostics (Run self-test, Restart, Factory reset) | source |
| Notices: Speakers off: charger too weak; Speakers off: the supply is out of range; Power limit; Amplifier fault + Clear; Update failed; "Looking for the receiver…" | source |
| First run: the app opens on Setup with a welcome note; the receiver's own network stays up while it joins | source |
| Android: Add to Home screen (no https, so a shortcut, not an install) | source |

**Mismatches left for the app and firmware:**

- The app's texts call the setup network `HiFi-…`; the firmware names it
  `hifi-lite…`. The manual follows the firmware.
- The app's sub **Level** slider runs −30 to +6 dB; the Lite firmware's sub
  level stops at +4.5 dB. The manual gives no range for Level.
- No balance control in either, so the manual dropped it.

## From the bench (`bench/`, `docs/bench.md`)

| Fact | Status |
|---|---|
| `python3 bench/bench.py find`, `… bringup` | source |
| Measured power, THD, standby power, weight | TBD: replaces the calculated figures in §13 |
