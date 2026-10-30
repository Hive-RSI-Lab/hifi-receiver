# R3 AMP: class-D amps and speaker outputs

Reviewer domain AMP. Boards at commit ac484b1 (+ review drafts). Lite U8
TPA3118D2. core U15/U16 TPA3220DDW (the pad-down TPA3221; pin-identical,
SLASEN3B table "Device comparison"). References: TI SLOS708G (TPA3118D2 data
sheet), SLOU342A (TPA3118D2EVM), SLASEN3B (TPA3220 data sheet), SLAU754A
(TPA3220EVM), SLAA701 (LC filter design), LCSC data/data sheets for
C2962885 (PROD PSPMAA0805-100M-ANP) and C48391614 (XR0850-100M).
Tests: `docs/review-3/sims/amp/` (`python3`; `lc_bench.py` needs `$HWDE_PY` for numpy; they read the
boards and change nothing).

## Findings

| id | board | sev | repeat | finding | fix |
|---|---|---|---|---|---|
| R3-AMP-1 | core | **major** | new | Bootstrap caps are far from their BST pins: C222 25.9 mm, C223 13.6, C224 5.8, C225 10.4; C320 29.4, C321 9.6, C322 9.3, C323 15.4 mm (pad to pin). BST traces are 0.2 mm wide and up to 35.6 mm long (B_BST1M 35.6, A_BST1M 32.4, B_BST2P 31.8 mm), with vias. The OUT side of each cap comes back by a second long route (`bst_links.py`). TI puts all four at the row ends, ~1-2 mm from pins 43/44 and 23/24, and loops the other pad straight back to OUT (SLASEN3B fig 53; 11.1.4 "close to the device ... minimum length"). | Move C222/C223 (C320/C321) to the pin-44 end of the row and C224/C225 (C322/C323) to the pin-23 end, ≤ 2 mm from their BST pins. Join the OUT pads to OUT1_P 39/40, OUT1_M 35, OUT2_P 32 and OUT2_M 27/28 with ≤ 5 mm, ≥ 0.4 mm stubs. Those spots now hold the PVDD caps (see AMP-2), so the two moves go together. |
| R3-AMP-2 | core | **major** | new | The PVDD HF decouplers (C207-C210, C307-C310: 1 uF 0805 + 100 nF 0603) sit at the row ends, 6.5-8.1 mm from PVDD pins 29-31/36-38, and nothing is closer. TI puts its 1 uF X7R caps ("T2") on the PVDD copper within ~2 mm of the pins, with the outputs leaving on the bottom layer (SLASEN3B fig 53; 11.1.3: "lack of proper decoupling ... voltage spikes which can damage the device"). Estimate: a ~3.3 nH loop with 7 A commutated in 3 ns gives a 7.7 V spike, so 29.4 V + 7.7 V = 37.1 V against the 37 V absolute maximum (TI-style placement: 33.2 V). The 0805 1 uF/50 V (C28323) also keeps only ~40-60 % of its value at 28 V. | Add a 1 uF/100 V X7R 1206 (e.g. Samsung CL31B105KCHNNNE; confirm the LCSC id and stock) plus a 100 nF/100 V 0603 on each PVDD pin group, ≤ 2 mm from pins 29-31 and 36-38, between PVDD and the adjacent GND pins 33/34 and 41/42 or 25/26 as in fig 53. If the top has no room because the outputs fan out there, put them on B.Cu directly under the pin groups with ≥ 2 vias per pad (JLC two-sided assembly). Keep the present ones. Bench check: scope PVDD at pin 29 to GND pin 33 with a short spring ground, at 4 ohm full power; peak must be ≤ 35 V. |
| R3-AMP-3 | core | minor | new | Inductor margin at the overcurrent limit. PSPMAA0805-100M: 10 uH ±20 %, Isat 10 A at -30 %. At the TPA3220's IOC of 10 A typ (no max given), a -20 % part keeps ~5.6 uH against TI's ≥ 5 uH (SLASEN3B ROC; SLAU754A 3.7). That is 12 % margin on a typ figure. At 4/6/8 ohm, 28 V: peak 7.6/5.5/4.3 A including ripple, under Isat 10 A. Sine Irms 4.9/3.3/2.5 A, under the 6 A rating. | None needed for 4-8 ohm speakers. For more short-circuit margin: the 1050-size alloy choke in a 10x11.5 mm land (same family) or a TI-EVM-class 10 uH/≥ 15 A part on a DNP-compatible footprint. Bench: speaker-wire short at full volume, then a RESET, 10 times. |
| R3-AMP-4 | core | minor | AUDIO-12 | The 4.7 R + 1 nF network on each speaker node is in parallel with 1 uF, so it neither damps the LC (open-load peak +28 dB at 73 kHz) nor does what TI's 3R3 + 10 nF does. SLASEN3B fig 50 uses 1 uF + 1 nF + 3R3/10 nF per leg; the TPA3220EVM fits a 1 uF film only (snubbers DNP). | Change C228/C231/C234/C237 (and the zone-B ones) to 10 nF/50 V X7R 0603 (C57112) and R to 3.3 R. That matches fig 50. Cost-neutral. |
| R3-AMP-5 | lite | minor | new | No switch-node snubber footprints. The TPA3118D2EVM fits 10 R (0805) + 330 pF C0G from each OUT to GND, before the inductor, and 3.3 R + 10 nF after it (SLOU342A fig 3). In BD mode both legs carry a 0.6 Vpp common-mode, 400 kHz ripple at idle onto the speaker wires (`lc_bench.py`). This is an EMI risk, not a function one. | DNP footprints: 10 R 0805 + 330 pF C0G 0603 from OUT_x to GND at U8 pins 21/23/27/29. Fit them only if a radiated-emissions scan asks for it. |
| R3-AMP-6 | both | note | new | Pop on a lost PD contract. PVDD is cut in hardware (PG) while SDZ (Lite) or RESET (core) is still high, against TI's advice to shut the amp down first (SLOS708G 7.3.3; SLASEN3B 9.4.2). The 690-1410 uF bulk makes the decay slow and symmetric in BD/AD mode, so expect a soft thump at most. | Firmware already drops SDZ/RESET when it sees PG high. Nothing to change on the board. |
| R3-AMP-7 | core | note | THERMAL-10 | Plus plays L on zone A and R on zone B, so each chip runs one channel open-load all the time. That is safe: feedback is taken before the filter and the open-load Q peak is at 73 kHz. It still costs AD idle loss plus inductor core loss (unspecified for molded chokes at 1.5 App, 600 kHz). | Bench: idle inductor temperature after 1 h. If it is high, HEAD mode (pin 11 to AVDD) cuts idle ripple. |
| R3-AMP-8 | core | note | new | OTW_CLIP clip pulses (~500 ns, SLASEN3B 9.4.5.2) reach firmware only through the TCA9539, which is polled every 10 ms or read on INT, so clipping isn't reliably seen. OTW (thermal) is level-type and is seen. | None (firmware uses OTW only as a thermal flag). |
| R3-AMP-9 | core | note | (known) | TPA3220DDWR C2879306: LCSC/JLC stock 0. A pre-buy is in fab/prebuy.csv (docs/known-unknowns parts:67). | Keep the pre-buy step. |

## Lead's question: Lite U8 SYNC (pin 16) unconnected

Valid. SLOS708G pin table (p4): SYNC is "Clock input/output ... Direction
determined by GAIN/SLV terminal"; section 7.3.1: "In Master mode, SYNC terminal
is an output". Master/slave is set by the GAIN/SLV divider, not the AM2-0 pins
(AM2-0 only pick the PWM frequency, SLOS708G 6.6 / table 6). U8's divider is
R50 20k to GND and R51 100k to GVDD, which is "Master 26 dB" in table 1, so
SYNC is a clock output and may float. The TPA3118D2EVM (SLOU342A fig 3) only
brings SYNC to a test point through R20 4.7k / C30 47 pF. There is one amp on
Lite, so it has nothing to sync with. It doesn't matter.
`check_amp_netlist.py` asserts the master divider and the open SYNC.

## Blocks checked, against the reference, with margin

**Pin-by-pin (`check_amp_netlist.py`, 0 fails).**

- Lite U8: MODSEL GND (BD); MUTE GND; AM2-0 000 (400 kHz); PLIMIT = GVDD; GAIN 20k/100k (master 26 dB, table 1).
- Lite U8: BST 220 nF/25 V X7R on the right BS/OUT pairs (20-21, 24-23, 26-27, 30-29). SLOS708G asks for ≥ 16 V; the cap sees ~6.9 V.
- Lite U8: AVCC on PVDD with 1 uF. Inputs 1 uF on both legs, INN AC-grounded with a matched 1 uF (TI 7.3.7: matched impedance for pop/DC-detect). 1 uF into 30 k is -3 dB at 5.3 Hz; TI table 2 suggests 3.3 uF for 1.6 Hz. This is a note only.
- core U15/U16: VDD on +5V (LDO bypass); AVDD and GVDD each through 4.7 R + 1 uF. TI uses AVDD direct and GVDD through 3R3: within, since 16 mA x 4.7 R = 75 mV. HEAD = GND (AD mode).
- core: CMUTE 33 nF (TI 33 nF). BST 33 nF/50 V X7R on 43-35, 44-39, 23-27, 24-32 (TI 33 nF/50 V). Inputs 10 uF both legs (TI table 3, 24 dB: 10 uF).
- core GAIN/SLV: U15 20k/100k = master 24 dB; U16 75k/47k = slave 24 dB (table 2).
- core FREQ_ADJ: U15 100k ∥ 100k = 50.0 k (49.5-50.5 k → 600 kHz); U16 0R to AVDD (slave).
- core OSC: OSCM 47k to AVDD and OSCP 47k to GND on the master, as SLASEN3B 9.3.2 asks. OSCM-OSCM and OSCP-OSCP, so U16 is Slave 1.

**Zone A/B sync and RESET.**
- SLASEN3B 9.3.2: synchronised only if every RESET is released at the same time. `board_core.c` `hifi_bsp_power_out` sets both TCA9539 port-0 bits in one write, and puts both back in reset for one step whenever zone B changes. **Holds.**
- R40/R41 100k pull-downs hold both RESETs low through any ESP32/expander reset (PARTS-4 fix). **Holds.**

**Fault recovery.**
- core: FAULT and OTW are wired-OR open-drain with 10k to +3V3 (abs max 5.5 V: OK). OLP and OTE latch until RESET toggles (table 6). Firmware holds the amp in reset for `fault_retry_ms` 1000 ms (TI asks ≥ 4 ms after FAULT falls), then latches off after 3 faults in 60 s. `fault_blank_ms` 50 ms covers PPSC, which TI gives as < 15 ms/uF on ~1 uF of filter: ~3x margin.
- Lite: FAULTZ on GPIO23 with 10k to 3V3. The same sequencer cycles SDZ low for 1000 ms; TI asks for a low pulse and the 1.4 s figure applies only to its MUTE scheme (SLOS708G 7.3.10).
- **Pass** on both.

**Pop sequencing.**
- Lite: SDZ is released ≥ 400 ms after PVDD_EN (S-2 fix) through 10k/47 nF (7 V/ms < 10 V/ms abs, SLOS708G 6.1). Its 3.3 V high stays under PVCC + 0.3 once PVDD is up. While PVDD is down, the 10k limits any back-feed to 0.27 mA.
- core: RESET is released 100 ms after PVDD_EN. PVDD is at 10-90 % in ~47 ms (firmware comment, hwde sim_inrush). CMUTE then gives a 20 ms ramp, as TI recommends.
- One residual: the hardware PG cut (R3-AMP-6).

**LC filter and inductors (`lc_bench.py`, 0 fails).**

| load | Lite (21 V, XR0850-100M, Isat 8 A, Irms 6.5 A) | core (29.4 V, PSPMAA0805-100M, Isat 10 A, Irms 6 A) |
|---|---|---|
| 3.2 ohm | 7.00 A peak, L 6.2 uH, 20 kHz -1.2 dB | 9.21 A peak, Irms 5.97 A (0.5 % under rating) |
| 4 ohm | 5.82 A peak (27 % under Isat), Irms 3.5 A | 7.63 A peak (24 % under Isat), Irms 4.9 A (19 % under) |
| 6 ohm (Mozart nominal) | 4.21 A, 20 kHz +0.1 dB | 5.45 A, 20 kHz -0.3 dB |
| 8 ohm | 3.38 A, +0.4 dB, peak +2.6 dB | 4.32 A, 0.0 dB, peak +1.4 dB |
| open | peak +29 dB at 61 kHz | peak +28 dB at 73 kHz |

Assumptions:
- Inductors: -20 % tolerance, with a soft roll-off through Isat at -30 % (both data sheets define Isat at -30 %).
- Caps: Lite 1 uF 1206 at 10 V (0.85); core 1 uF 0805 at 14 V (0.60; hwde `mlcc_dcbias` generic curve says 0.86).
- Short circuit: Lite keeps 5.9 uH at the 7.5 A OC limit (TI minimum 1 uH). core: see R3-AMP-3.

Against the references:
- TPA3118D2EVM uses 10 uH/5.8 A + 0.68 uF; Lite uses 10 uH/8 A + 1 uF, which is within (SLOS708G 6.3 allows 1 uF).
- TPA3220EVM uses 10 uH/18 A-sat + 1 uF film; core uses 10 uH/10 A + 1 uF X7R. That holds for 3.2-8 ohm and is thin at IOC.
- Idle ripple per inductor: Lite 1.64 App common-mode (BD); core 1.53 App differential (AD).

Bounds:
- 20 kHz response is within ±3 dB at ≥ 4 ohm.
- The ripple at fsw is -33 dB (Lite) and -37 dB (core) on the speaker.

**PVDD decoupling.**
- Lite: 3 x 1 uF/50 V + 2 x 100 nF + 2 x 1 nF at 2.9-4.7 mm, plus 470 uF polymer + 220 uF. The TPA3118D2EVM has 2 x 220 uF + 100 nF + 1 nF. **Within.**
- core: R3-AMP-2. Bulk is C5/C6 (zone A, 12.9-20.7 mm) and C311 (zone B, 12.9 mm), 470 uF general-purpose. TI uses 2 x 1000 uF at 0.034 ohm; THERMAL-5 judged the bulk acceptable under the governor.

**Placement (`check_amp_layout.py`, 16 fails, all core BST/PVDD).**
- Lite: all HF caps 2.9-5.6 mm, BST 3.5-4.7 mm. **Pass.**
- core: AVDD 2.6 mm, GVDD 2.0 mm, VDD 2.8-2.9 mm. **Pass.**
- core BST and PVDD: R3-AMP-1/2.

## Round-1 fixes checked

| id | holds | how |
|---|---|---|
| AUDIO-4 / DIGITAL-14 | yes | lite-main.net: R54 10k AMP_SDZ_IO-AMP_SDZ, C26 47 nF on AMP_SDZ, R52 100k pull-down. (The firmware header `hifi_board_lite.h` comment still says "through 100k": stale.) |
| THERMAL-1 / DFM-3 | yes | U15/U16 are TPA3220 with pad 45 = GND; every pin matches SLASEN3B (`check_amp_netlist.py`). |
| PARTS-4 | yes | U17 TCA9539; R40/R41 100k hold AMP_A_RST/AMP_B_RST low. |
| C5 (sync) | yes | One port-0 write releases both resets (`board_core.c`). |
| S-2 | yes | Lite `soft_start_ms` 400 (`hifi_power.c`). |
| POWER-13 | yes | `v_start` 14 V: no amp on a 5 V port. |
| AUDIO-12 | open (now R3-AMP-4, minor) | The snubber still does not damp; a value change is proposed. |

## Field issues searched and what our boards do

Sources are TI's own documents where cited; forum knowledge is marked unverified.

- **Bootstrap undervoltage / high-side dropouts from far or small BST caps** (TI E2E TPA3116/TPA32xx threads; SLASEN3B 9.4.5 "Bootstrap UVP ... shuts down the respective half-bridge"). Lite immune (220 nF, 3.5-4.7 mm). **core has it**: R3-AMP-1.
- **Turn-on pop / DC-detect nuisance trips from mismatched input caps** (SLOS708G 7.3.7, 7.3.9). Lite immune: matched 1 uF/1 uF, and SDZ is held until 400 ms after PVDD. core immune: matched 10 uF/10 uF, CMUTE 33 nF.
- **Inductor heating / core loss at idle** (common on TPA3116 boards with small drum cores). Both boards have soft-saturation alloy chokes at ≤ 82 % of Irms at 4 ohm. Core loss is unknown: AMP-7, bench.
- **Oscillation or ringing with open load** (cheap boards that use output-side feedback). Both chips take feedback before the filter. Open-load Q peak is at 61/73 kHz, far above audio. Immune; the AMP-4 value change is optional.
- **Shorted speaker cable** (latches, needs a reset). Both latch, and firmware cycles SDZ/RESET then latches off after 3 in 60 s. TPA3220 PPSC holds off a start into a short. Immune, with the core L margin of AMP-3.
- **PBTL/BTL misconfiguration** (TPA3220 PBTL is set by IN2_M = IN2_P = 0). core IN2 pins are AC-coupled, so BTL. Lite LINP/LINN are AC-coupled, so BTL. Immune.
- **Bootstrap failure at very low frequency / long duty extremes** (TI: 33 nF is sized for minimum duty; pulse injector at clipping, SLASEN3B 9.4.5.2). Values match TI. Immune apart from layout (AMP-1).
- **What Chinese TPA3116/3118 boards change from the EVM** (unverified, from common XH-M543-style designs):
  - They drop the 10 R/330 pF snubbers and the RC Zobel. Lite does the same: AMP-5.
  - They use 22 uH/0.68 uF or ferrite filters.
  - They tie MUTE to GND and SDZ to a pull-up with no sequencing; Lite sequences SDZ.
  - They run 24 V close to the 26 V maximum. Lite runs 20-21 V against the 27 V OVLO: 6 V margin.
- **Unsynchronised multi-chip beat tones** (SLASEN3B 9.3.2). core uses master/slave with a simultaneous RESET release. Immune.
