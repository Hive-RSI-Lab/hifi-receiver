# Review round 4 (review-5): DAL — core (Plus and Ultra), digital, audio, layout, system and fab package

Reviewer: DAL, 2026-10-31. Worktree `h2/review-5` = origin/main at 078aaa9, read-only outside `docs/review-5/`.
Board files: `boards/core/kicad/core.kicad_pcb`, `core.net`, `gen/blocks*.py`; firmware `firmware/` (board_core.c,
hifi_main.c, hifi_line.c, the Kconfig patch); fab `boards/core/fab/` and `fab/ultra/`.

Tools: KiCad 10.0.6 CLI (DRC with zone refill and schematic parity, on a copy in `/tmp/dal/core`), hwde `check_si`
and `sim_run` (ngspice 45), round-2 scripts in `docs/review-3/sims/` re-run against the current board, firmware host
tests (`firmware/test`, built in a /tmp copy), and new tests under `docs/review-5/sims/dal/`.

## Verdict

**0 blocker, 0 major, 10 minor, 6 notes.** I found no remaining blocker or major on core in my domains. The round-2
fixes are on the board and in the firmware, with two exceptions:

- **R3-LAYOUT-1** is half done. U15's input caps moved, but U16's did not.
- **R3-DIGITAL-1** is covered for brownouts, and for every reset that pulls EN low. The RTC-watchdog path stays open.

I rate both residuals minor and give the numbers below. Round 2 called R3-LAYOUT-1 major on the premise of a
"10-20 kohm node". The IN_P node is actually driven from the OPA1678 through 10 uF, which is low impedance. Even in DJ
direct the node is only ~1.9 kohm, and the computed pickup is −94 dB at 1 kHz.

## Findings

| ID | Sev | Repeat of | Finding | Fix |
|---|---|---|---|---|
| R5-DAL-1 | minor | R3-LAYOUT-1 | U16's IN_P runs are still 24.6 mm (B_IN1P) and 13.5 mm (B_IN2P), and its M partners 11.5 / 39.1 mm (B_IN2M crosses 6 vias). B_IN1P has 17 mm over the PVDD plane and 3 mm within 1 mm of B_OUT2P/B_BST2P pads. U16 is live on both builds (Plus plays R on it). | Next layout touch: put C316/C318/C317/C319 within 5 mm of U16 pins 8/15/9/16, as U15 now has. Bench: idle noise on J10/J11 ≤ 100 uV A-wt, and J11 vs J6 within 3 dB. |
| R5-DAL-2 | minor | new | ETH_CLK (50 MHz REF_CLK) is a 93 mm unbalanced T. R60 (33R) sits 19 mm from Y2 rather than at its pin; the branches are 36.6 mm to U9 CLKIN and 56.4 mm to GPIO0. With 1 ns edges the PHY sees 1.19 V of ring-back after the falling edge (and 0.95 V with a 10 ohm driver). That is 0.39 V over a 0.8 V VIL, but under the 1.41 V typ Schmitt VILT. With 2 ns edges it passes (0.48 V). | Next touch: move R60 to ≤ 3 mm from Y2 pin 3. Add a DNP 0402 C0G footprint (start 10 pF) at U9 pin 5 to GND as a contingency. Bench: scope U9 CLKIN; 10 min `ping -f` with 0 CRC errors (`ethtool`/IDF counters). |
| R5-DAL-3 | minor | R3-DIGITAL-1 | GPIO0 still latches the 50 MHz clock on an RTC-watchdog system reset, because EN does not move. In the bootloader that watchdog is 9 s (`CONFIG_BOOTLOADER_WDT_TIME_MS=9000`), and P05 is still high after any software or panic restart. Brownouts are covered: U21 (TLV803S, 2.93 V) resets EN 0.50 V above the ESP32's 2.43 V BOD (level 0). Panic and `esp_restart` are CPU resets, which don't re-sample the straps. | Firmware: a bootloader hook (`bootloader_before_init`) bit-bangs I2C on GPIO13/14 and writes TCA9539 0x74 OUT0 bit 5 low. A bootloader hang then times out with the clock off. Optionally also drop PHY_RST while there is no Ethernet link for 30 s, which also removes the 50 MHz harmonics from Wi-Fi. |
| R5-DAL-4 | minor | new | The DJ-direct guard looks only at the amps' thermal budget, never at the USB-C contract. FLX4 at its rated 2.1 Vrms into 4 ohm gives 2 × 33.6 W out, so 80.5 W in (0.88 efficiency + 4 W idle). That is 1.24× a 65 W contract, and the 0.163 J of PVDD bulk rides it through only ~10 ms. Sustained sine-like sub-bass at full FLX4 level on a 65 W charger can trip it. 100 W chargers (0.81×) and the 140 W the product specifies (0.57×) are fine. | `hifi_djg_step`: also trip to the governed line in when the INA186's 50 ms mean input exceeds `budget_frac` × contract. Or offer DJ direct only on contracts ≥ 90 W. Host test in `firmware/test/test_line.c`. |
| R5-DAL-5 | minor | new | The OTW step-down in `hifi_main.c` power_task acts on any single 10 ms sample of OTW_CLIP low. Firmware requirement C2 says "low for over 200 ms". OTW_CLIP also pulses ~500 ns on each clipped PWM cycle (SLASEN3B 9.4.5.2). On a 20 V contract the DAC path clips above about −1.4 dBFS (22.4 Vpk vs ~19 Vpk), so clipping triggers "amp too hot" −3 dB trims, down to −12 dB, recovering only +1 dB per 10 s. | Require OTW_CLIP low continuously for ≥ 200 ms before a trim, as `hifi_djg_step` already does. |
| R5-DAL-6 | minor | new | Line-in headroom: FLX4 2.1 Vrms (rated, 10 kohm, Zout ≤ 1 kohm) gives the ES7243E −2.1 dBFS. It clips at 2.66 Vrms at the RCA. The FLX4's maximum output is unpublished, and DJs run into the red. The board then clips the governed line in and, in DJ direct, the subs. The PESD5V0S2BT (VRWM 5 V) leaves +4.5 dB above rated. DJ direct at the amp has 4.2 dB to clip at 28 V and 1.3 dB at 20 V. | Bench: measure the FLX4 at master max with the meter in the red. If it exceeds 2.6 Vrms, R101/R111 4.7k → 3.3k (−1.7 dB at the ADC and on DJ direct) or set the ES7243E digital volume (reg 0x0E/0x0F). No board change before that measurement. |
| R5-DAL-7 | minor | new | Fab package hygiene (Plus and Ultra): (a) `fab/ORDERING.md` says "Ten 2-pad MLCCs are on the bottom ... all non-polar", but there are 14, including the 3-pin D6-D9. (b) `fab/ultra/core-ultra-pos.csv` is stale: 300 rows from b485714, no D6-D9, old positions for 20 amp-area parts. It sits beside the correct `CPL.csv`. (c) D6-D9 bottom rotation: the LCSC model fits the board pads only at CPL+180° under an x-mirror, and that depends on the convention. | Update ORDERING.md (14 bottom parts; D6-D9 are polarised SOT-23, check in JLC's bottom preview). Delete `fab/ultra/core-ultra-pos.csv` and `core-pos.csv` duplicates, or regenerate them. Confirm D6-D9 in JLC's preview at order. |
| R5-DAL-8 | minor | new | A second footprint field, `LCSC Part`, disagrees with the BOM on 170 placed parts (Plus) and 171 (Ultra), e.g. C1 C45783 vs C28323 and C4 C19702 vs C14663. The uploaded BOM is right, because it is built from parts.json, but any tool reading the board field would order wrong parts. | Strip or regenerate the `LCSC Part` field from parts.json in `fab.sh`. |
| R5-DAL-9 | minor | R3-LAYOUT-2 | B.Cu signals still reference In2's split +3V3/PVDD plane rather than GND. Lengths: I2S_DOUT_MAIN 104 of 111.6 mm over +3V3; BCK 79/104.6; LRCK 83/113.7; LIN_L 27/30.9 and LIN_R 19/22.9 (the RCA input, now rerouted through D8); ADC_SRC_L/R 19/32. hwde check_si: 3 corridor voids (BCK, LRCK, RMII_RXD1). | As round 2: GND on In2 under x 100-140 / y 130-180, or 100 nF +3V3-GND stitching caps at the crossings; LIN_L/R to F.Cu at the next touch. |
| R5-DAL-10 | minor | R3-AUDIO-6 | Still no RF shunt at the core RCA line in. Lite has 100 pF. LIN_L/R are the 2.35 kohm divider's input, and DJ direct puts the switch and the TPA3220 input on it. | 100 pF C0G 0402 from LIN_L/LIN_R to GND at D8 (B.Cu, next to the clamp's ground via). |

Notes:

- **R5-DAL-N1.** I2S_BCK is unterminated (hwde check_si warns): ESP32 → 61.9 mm → ES7243E → 27.7 mm → PCM5102A (4.9 mm) /
  ES9018K2M (8.9 mm). The `docs/review-5/sims/dal/i2sbck/` benches pass: no ring-back through 0.7/0.3 VDD at 1.5-2 ns
  edges and 30-45 ohm drivers, minimum high 3.12 V and maximum low 0.17 V. Overshoot reaches 3.89 V (VDD + 0.59) at 1.5 ns.
  A 33R at GPIO2 would take it to 3.31 V. That is optional, at the next touch.
- **R5-DAL-N2.** The governor and the DJ guard use `vfs_peak_v` = 25.8 Vpk (1.15 Vrms × 24 dB). The board computes 22.4 Vpk
  (MAIN 1.0 Vrms; `check_audio_nets.py`), so the firmware overestimates power by 1.24 dB (1.33×). That is safe, but the
  guard hands DJ direct over at ~14.3 W real against a 19 W Plus limit. Calibrate at bring-up, as the code comment says.
- **R5-DAL-N3.** Power loss: Q1 is a single P-FET with its source on VBUS, so its body diode backfeeds VBUS from the 1410 uF
  PVDD bulk. +5V (SY8303) outlives PVDD, so the TPA3220s reach PVDD UVLO with VDD and VREF still steady: no VREF
  thump. The ESP32 then goes down through U21 (EN held low, expander and Y2 reset). The PCM5102A sub outs are
  ground-centred. The firmware can't act in time (PD polled every 500 ms) and doesn't need to. Bench: unplug at full
  level; speaker and sub-out transients ≤ 100 mV.
- **R5-DAL-N4.** TPA3220 C2879306: the generator's cached JLC stock is −2 against 10 needed. Pre-buy, as before.
- **R5-DAL-N5.** RCA ESD (D7/D8, PESD5V0S2BT, Nexperia, bidirectional, VRWM 5 V, IRM 5 nA, Cd 35-45 pF). The signals are
  ±2.97 V peak at the rated FLX4 / PCM5102A level. C(V) distortion into a 1 kohm source at 20 kHz is under −100 dB. The
  stubs are 1.5-6 mm, and each clamp has its own GND via. Pass.
- **R5-DAL-N6.** R3-AUDIO-7 (RCA sleeve earth loop), R3-AUDIO-8/9 (PCM5102A 1 uF supply caps, XSMT on DAC_EN) and
  R3-DIGITAL-9 (antenna side clearance 0.85 mm) are unchanged from round 2 and stay minor/accepted. I didn't re-derive them.

## Round-2 fixes checked

| Fix | Holds | How |
|---|---|---|
| R3-LAYOUT-12 (J4/J5 RCA footprint) | yes | `fp_vs_lib.py`: J4/J5 match the library (slots and leg holes). The only mismatches are D6-D9, and only because they are flipped bottom-side footprints. |
| R3-DIGITAL-1 / R3-SYSTEM-3 (GPIO0 strap) | partly | U21 TLV803S on the board: pin 1 GND, 2 ESP_EN, 3 +3V3, at (137.0, 183.5), 5.2 mm from R30. `esp_register_shutdown_handler(phy_clock_off)` is in `hifi_bsp_init`. `strap_check.py` still fails the sysrst case, which is now only the RTC watchdog (R5-DAL-3). |
| R3-DIGITAL-2 (PHY address) | yes | `fw_pinmap.py`: PHYAD0 strap → 0, firmware auto (patch 0002). ETH_CONFIG `model=LAN8720,mdc=23,mdio=18,rst=-1`. |
| R3-AUDIO-1 (VREF) | yes | C64 100 uF 1206 on VREF at (122.85, 170.6), 6.8 Hz corner (~13 Hz at a 50 % DC-bias derate). The round-2 sim gives 3.5 mVpp vs 27. |
| R3-LAYOUT-1 (amp input caps) | partly | U15: A_IN1P 6.9 mm, A_IN2P 9.3 mm (were 45.2/55.8), caps on B.Cu under pins 8/15, 0 mm within 1 mm of OUT/BST. U16: not moved (R5-DAL-1). AMP_IN_L/R are now 91/116 mm on the low-impedance side. |
| R3-AUDIO-3 (subs in DJ direct) | yes | `fw_duties.py` A3: I2S0 at 32-bit slots (64 fs, `bits=32` in DAC_CONFIG); ES7243E with MCLK from BCLK (reg 0x02 bit 7), ×8 (reg 0x04 = 3); ADC → I2S1 slave → PCM5102A, inverted per C5. Host tests `line` 40/40. |
| R3-SYSTEM-4 (DJ thermal + OTW) | yes, with R5-DAL-4/5 | Guard trips to the governed line in on a 60 s mean over 19 W/0.88 + 4 W = 25.6 W in (Plus), or on OTW low for 200 ms. The OTW −3 dB/2 s step-down exists, but triggers on a single sample (R5-DAL-5). |
| R3-SYSTEM-1 (governor) | yes | `run_gov.sh`: 0 fail; core 65 W burst 1.08× for 5 ms (0.027 J vs 0.163 J bulk), 140 W 0.85×. Host tests: gov+tone 38449/38449. |
| R3-SYSTEM-9 (input saved) | yes | `hifi_input` in NVS ("dj" / "line"); the guard's hand-over isn't saved (by design). |
| R3-DIGITAL-4 (P17 TAP_SEL) | yes | EXP_CFG1 0x7F (P17 output, low = RCA line in); `fw_pinmap.py` bit 15 → TAP_SEL_IO. |

## Blocks passed, with reference and margin

| Block | Reference | Ours | Margin |
|---|---|---|---|
| KiCad DRC (refilled, schematic parity) | — | 0 violations, 0 unconnected, 0 parity on 078aaa9 | Pass. Courtyard/lib-mismatch rules are still ignored in the project (R3-LAYOUT-10). |
| ESP32 straps | ESP32 datasheet table 3-1 | GPIO2 BCK (inputs only) 0; GPIO12 PCM5102A DIN (hi-Z) 0 = 3.3 V flash; GPIO15 LCD_DC 1; GPIO5 DATA 1 | Pass in POR and EN; GPIO0 sysrst → R5-DAL-3 |
| EN / reset supervisor | TLV803S 2.93 V, 200 ms; HDG 10k/1 uF | U21 open drain wired with Q4 and C32; BOD level 0 = 2.43 V | 0.50 V between trips; 3.33 V rail is 0.40 V above U21 |
| Firmware pin maps | core.net | `fw_pinmap.py`: all GPIOs, RMII/SMI, expander bits 0-15 | 0 mismatches |
| I2C bus | UM10204 fast mode | Plus 53 pF / 99 ns; Ultra 78 pF / 146 ns; 0x10/0x22/0x27/0x48/0x74 unique | 2× on rise time |
| RMII clock | LAN8720A REF_CLK in; ESP32 GPIO0 CLK_IN | ETH_CLK bench | Passes at 2 ns edges; R5-DAL-2 at 1 ns |
| I2S clocks | check_si | BCK bench | No double clock; overshoot ≤ 3.89 V (N1) |
| Line in level | FLX4 2.1 Vrms / ES7243E 1 Vrms FS / TPA3220 24 dB | ADC −2.1 dBFS; DJ 11.6 Vrms vs 18.7 Vrms clip at 28 V | 2.1 dB ADC (R5-DAL-6), 4.2 dB amp |
| DJ switch | TS5A23157, 0..V+ | LSW 2.5 ± 1.4 V, MAIN 1.03..3.86 V on 5VA; DJ_SEL 5 V by default (DAC path) | ≥ 1.1 V to either rail |
| Fab Gerbers | fresh kicad-cli export | 13/13 files identical, zip identical | Pass |
| Fab CPL/BOM | PCB + parts.json | 298 placed (284 top / 14 bottom / 13 THT Plus, 12 Ultra); 87/85 BOM lines; 0 ref, value, LCSC or position mismatches | Pass; hygiene in R5-DAL-7/8 |

## Tests built

- `docs/review-5/sims/dal/fab/check_fab.py`: Gerbers, CPL, BOM vs PCB and parts for Plus and Ultra (0 FAIL, 10 WARN;
  output in `check_fab.out`).
- `docs/review-5/sims/dal/fab/bottom_fit.py`: fits the bottom-side D6-D9 EasyEDA models to the board pads.
- `docs/review-5/sims/dal/ethclk/` (`gen.py` + 3 `tb_*.cir` / `.bounds.json`): `sim_run.py --dir .` gives 1 pass and
  2 fail (the 1 ns cases).
- `docs/review-5/sims/dal/i2sbck/` (`gen.py` + 4 benches): all pass.
- `docs/review-5/sims/dal/q.py`, `netpath.py`: net, pad and routed-branch-length queries on the PCB.
- Re-run from round 2:
  - `check_audio_nets.py`: 0 FAIL, 5 WARN.
  - `strap_check.py`: 1 fail (sysrst GPIO0).
  - `fw_pinmap.py`: 0.
  - `i2c_bus.py`: pass.
  - `fp_vs_lib.py`: J4/J5 pass.
  - `check_amp_layout.py`: 0 fail.
  - `gnd_ref.py`, `coupling.py`: numbers above.
  - `fw_duties.py`: 0 missing.
  - `run_gov.sh`: 0 fail.
  - `firmware/test`: all suites 0 failed.

## Known unknowns

- FLX4 maximum output above its rated 2.1 Vrms (retailer spec sheets; no Pioneer page found): could move R5-DAL-6.
- Y2 (OT322550MJBA4SL) edge rate, and the LAN8720A CLKIN input thresholds: they decide R5-DAL-2 (1 ns fails, 2 ns passes).
- JLC's bottom-side rotation convention for D6-D9 (R5-DAL-7c).
- Whether the ESP32 re-samples straps on the RWDT `RESET_SYSTEM` stage. Datasheet §3 says it does at system reset.
