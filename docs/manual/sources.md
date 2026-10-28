# Where the manual's facts come from

The manual ([`user-manual.md`](user-manual.md), [`quick-start.md`](quick-start.md))
has to stay in step with the boards, the firmware, the web app and the
enclosure. This page lists each fact the manual depends on, where it comes
from, and its status. When something here changes, change the manual too.

`grep -rn "TBD" docs/manual/` lists every open item in the text.

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
| Zone B shares 140 W; no parallel speakers (2 Ω) | §2, §3 |
| 5 V port boots with the amp held off | QS 2, §5, §14 |
| Power governor: 90% of the PDO, averaged over 1 s | §5 |
| DAC/amp/ADC part numbers, 24/96 target | §13 |
| Sub outs: Lite full range; Plus/Ultra bass-managed (crossover, level, delay, phase), optional mains high-pass | §4, §9 |
| Lite line in is the codec's analog bypass; Plus/Ultra DJ direct switch; 5–15 ms digital path | §4, §9 |
| Ultra optical in (TOSLINK) | §4 |
| Wi-Fi 2.4 GHz only; Ethernet on Plus/Ultra | §6 |
| Streaming: Spotify (cspot, Premium), AirPlay 1, BT A2DP SBC, Lyrion | §7 |
| Installable PWA; LMS apps also work (product.md's `hifi.local` became `hifi-<model>.local`, see the web app) | §8 |
| Lite buttons vol−, vol+, play (hold = BT pairing); Plus knob + source + preset; Ultra screen + knob + 3 buttons | §8 |
| IR learn mode; Plus/Ultra ship a 21-key NEC remote | §8 |
| CH340C serial on USB-C; `_hifi._tcp` mDNS; test API (PD, VBUS I, faults, tones, self-test) | §14 |
| Enclosure PETG/ASA, not PLA (heatsink 60–80 °C) | §2, §13 |
| Not certified: cspot, AirPlay 1, BT SIG, FCC/CE; the module's modular approval | §15 |
| Lite power is calculated from the 10.9 W/6 Ω/12 V board | §13 |

## From the boards

| Fact | Source | Status |
|---|---|---|
| Ultra front: screen, knob (EC11, no detents), 3 buttons, IR, RGB status LED | `boards/ultra-front/README.md` (branch of 261028-145644-e1975aa761, PR #10) | source |
| Rear panel order and printed labels | Lite board 261028-145601-969b9c9f7c, core 261028-145641-7c4786f95c, enclosure 261028-170022-cd0342e233 | TBD |
| Speaker terminals: banana socket or binding posts (product.md open item 4) | Lite board / enclosure | TBD |
| Lite and Plus status LED (count, colour) | Lite board, core | TBD |

## From the firmware (261028-165619-70c34f28b3)

| Fact | Manual says now | Status |
|---|---|---|
| Default display name in Spotify, AirPlay and BT | `HiFi` | TBD (the app's header defaults to HiFi) |
| Setup network | SoftAP `HiFi-…` (the app's text), portal at 192.168.4.1 | TBD: exact name, password |
| Fallback to setup when the saved Wi-Fi is gone | after a timeout | TBD: the timeout |
| BT pairing | Lite: hold ▶; app: Bluetooth → Pair; discoverable 2 min, blinking blue | proposed; TBD: hold time, Plus button |
| BT memory and "Forget all" | | proposed; TBD: device count |
| Source rule | last stream to start takes over; line in and optical are picked by hand | proposed |
| Button holds, Lite double press = next source, Plus preset | | TBD |
| Factory reset combination | hold − and + for 10 s | proposed |
| Status light colours | white = ready, blue = BT, amber = reduced power, red = fault | proposed |
| Auto-standby timeout, screen dim timeout | | TBD |
| Update: falls back to the old image if interrupted (OTA A/B) | | proposed; TBD: where updates are published |
| Test API base path and endpoints; licence file location | | TBD |

## From the web app (261028-165625-c782eda91e)

Read from its draft `app/web/index.html` and `app.js` (in its turn log, not
yet on a branch), against `docs/test-api.md`'s app API.

| Fact | Status |
|---|---|
| Tabs **Play**, **Sound**, **Setup**; Setup cards Wi-Fi, Name, Updates, About and diagnostics | source (draft) |
| Wi-Fi flow: Scan for networks → Choose → Join; "Joined …"; Forget | source (draft) |
| Setup network named `HiFi-…`; the app opens on Setup in AP mode | source (draft); TBD firmware: exact name, password |
| EQ: 10 bands 31 Hz–16 kHz, ±12 dB, presets Flat, Bass +, Loudness, Vocal, Treble − | source (draft) |
| Subwoofers: On/Off, Level −15..+10 dB, Crossover 40–160 Hz, Delay 0–20 ms, Phase 0°/180°, mains high-pass | source (draft). One setting for both subs: product.md plans one per sub |
| Line in: DJ direct / Through EQ; Zone B speakers + Level | source (draft) |
| Notices: charger too weak, power limit, amplifier fault + Clear; "Looking for the receiver…" | source (draft) |
| Updates: Check for updates, Install, From a file… | source (draft) |
| Hostname `hifi-<model>.local` (lite/plus/ultra); rename changes only the display name | source (app worker, 14:22) |
| Setup → About and diagnostics: Run self-test, Restart, Factory reset (erases Wi-Fi, name, sound settings) | source (app worker, 14:22) |
| Android: Add to Home screen (no https, so no full install) | source (app worker, 14:22) |
| "Update failed: … The old firmware is still in place." | source (app worker, 14:22) |
| No power-mode setting: the limiter is automatic | source (app worker, 14:22) |
| Screenshots: `docs/app.md` on the app's branch | to read when it lands |
| Remote learn, Bluetooth pair button, per-sub settings, volume limit, balance | proposed: not in the app yet |
## From the enclosure (261028-170022-cd0342e233)

| Fact | Status |
|---|---|
| Size and weight; rear and front panel drawings | TBD |
| Ventilation clearance (manual says 5 cm above and behind) | proposed |

## From the bench (261028-165628-ce2b3389d8)

| Fact | Status |
|---|---|
| Measured power, THD, standby power | TBD: replaces the calculated figures in §13 |
| Bench script path and command | TBD |
