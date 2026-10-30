# Review round 2 (review-3): POWER

Reviewer: power domain (POWER), all three boards (lite-main, core, ultra-front's supply), as on main at ac484b1.
Tests are in `docs/review-3/sims/power/`; their results are in section 4. Vendor documents were fetched with `sims/power/fetch.sh` into `/tmp/ds3/power/<doc>/`:

- TI SLVSDG6C rev C (TPS54302)
- TI SLVUAP9B (TPS54302EVM-716)
- TI SLASEN3B (TPA3220)
- TI SLOS708G (TPA3118)
- WCH CH224 datasheet V2.0
- Silergy AN_SY8303 Rev 0.9A
- AOS AO4485 Rev 1.1
- the LCSC PX-series sheet (C88733)

**Count: 0 blockers; 4 majors, all new, 1 of them a regression of round-1 POWER-2; 2 minors (both repeats); 3 notes.** Three of the four majors are fixed in firmware. The one board change is two parts placed in parallel on core.

## 1. Findings

| id | board | sev | repeat_of | finding (one line) | fix |
|---|---|---|---|---|---|
| R3-POWER-2 | core | **major** | POWER-2 (regression) | Two problems in the PVDD turn-on. First, firmware releases the amps (`soft_start_ms` = 100 ms) while PVDD is still ramping: PVDD reaches 90 % 122/155/196 ms after PVDD_EN at 28/20/15 V. Second, the third 470 uF bulk cap (C311, zone B on Plus) raised Q1's turn-on stress to 14.8 W peak and 0.55 J. hwde soa_check puts that at Tj 125 °C (40 °C ambient) or 135 °C (50 °C). If an amp draws 1 A while Q1 is still in its linear ramp, Tj reaches 286 °C. | Add a parallel 100 nF/50 V 0603 (C14663) to C4, making 200 nF gate-drain. Add a parallel 2.2 uF/50 V 0805 (C377773) to C8, making about 4 uF gate-source. Set firmware `hifi_power_cfg_core` `soft_start_ms` to 500. Sim with the fix: 7.7 W peak, t90 244 ms after EN, Tj 96 °C at 50 °C ambient, PD-step Vsg 1.15 V (Q1 stays off). |
| R3-POWER-3 | both | **major** | new | The governor limits the **1 s mean** input power to 0.9 × the contract. A sustained low-crest signal at that budget (test tone, room-measurement sweep, sub-bass) draws sin² current, peaking at **1.7-1.8 × the PDO current** with every charger tried (Lite 60 W: 5.3 A against 3 A; core 45 W: 3.8 A against 2.25 A). The bulk caps can't carry 80-200 Hz ripple: Zc is 1.1 Ω at 80 Hz against a 0.15 Ω source. Lite has no thermal limiter configured, because `hifi_gov_cfg_lite` leaves `thermal_out_w` at 0 and the L1 limit of 13 W/ch isn't implemented, so it can sit there indefinitely. On core this holds for the first ~60 s. Result: charger OCP, then a hard reset, then an ESP32 brown-out and reboot loop. | Firmware: (a) add a short-window (10-20 ms) limit on instantaneous input power, idle + p_out_inst/eff ≤ 1.0 × V·I_pdo; or, simplest, budget the 1 s mean at idle + (0.5 × contract − idle) whenever the crest factor is under 6 dB. (b) Core: clamp gain when INA186 ISENSE exceeds i_max. (c) Lite: implement L1 (13 W/ch, 30 s). On its own that brings a 60 W charger to about 1.0 × its PDO. No hardware change: bulk capacitance doesn't fix 40-100 Hz. This overlaps R3-SYSTEM-1. Its uncommitted change to `hifi_gov.c` (tau 1 s → 50 ms) is still a mean limit: a 40 Hz sine peaks at 2 × its 50 ms mean, so it doesn't remove the 1.7x. |
| R3-POWER-4 | core | **major** | new | Core has no VBUS ADC, so `hifi_ch224_vbus()` assumes VBUS = 20 V for **any** non-EPR PD contract. With a charger that has no 20 V PDO (18-30 W phone chargers offer 5/9/12(/15) V), the 14 V start floor is bypassed and the governor budgets 20 V × I. That overdraws by up to 20/9 = 2.2 × on top of R3-POWER-3. It is also unknown what the CH224Q falls back to when the requested PDO isn't offered. | Firmware: read the CH224Q's SRCCAP (0x60-0x8F, which only the CH224Q has). Take 20 V only if a 20 V fixed PDO is listed; otherwise budget for the highest fixed PDO ≤ 20 V. Don't close PVDD unless a ≥ 15 V PDO is present. Bench check: 18 W and 30 W chargers, logging the CH224Q status and the INA186 reading. |
| R3-POWER-5 | core | **major** | new | Firmware can close PVDD on the SPR contract (status bit 3, PG low, 200 ms settle) before the CH224Q finishes EPR entry. The 20 → 28 V step then charges 1410 uF with Q1 on, or with Q1 still mid-ramp. An ideal source supplies 26.6 A at 30 V/ms (`core_epr_step_20to28_on`); a real one goes into its current limit. Whether the charger rides through (11.3 mC; 2.3 ms at 5 A) or OCPs and hard-resets depends on the charger. | Firmware: when status bit 5 (EPR offered) is set, set PVDD_EN only after bit 4 (EPR active), and only 500 ms after the last change to the contract or to the firmware's own VREQ write. |
| R3-POWER-6 | both | minor | POWER-9 | Hot plug into a **non-compliant** live source (1 uH / 50 mΩ cable): Lite VBUS peaks at 32.2 V, over the 30 V absolute maximum of the TPS54302 and TPA3118. Core VBUS peaks at 43.6 V, over the SY8303's 40 V and the INA186's 42 V. The CH224Q's VBUS_PD stays at 28.8 V behind R17/C1. A compliant attach at 5 V rings to only 9.0/8.9 V. | Keep. This only happens with sources that violate PD (attach must be at vSafe5V). |
| R3-POWER-7 | core | minor | POWER-11 | The SY8303's input cap C10 (10 uF/50 V X5R 1206) is worth 2.6 uF (1.75 uF low) at 28 V per hwde mlcc_dcbias, against the datasheet's "at least 4.7 uF". Round 1 justified this by "the 470 uF bulk through Q1's body". That only holds with Q1 on: the body diode conducts PVDD → VBUS and is never in the buck's input loop at HF while PVDD is off. | Add a 10 uF/50 V 1206 (C13585, Basic) beside U4 IN. Contingency: a DNP footprint. |
| R3-POWER-1 | lite-main | note | new | TPS54302 EN floating is correct (see section 2). | none |
| R3-POWER-8 | both | note | new | Inductor Isat is below the bucks' peak current limit. Core L9 (SWPA4030S4R7MT) saturates at 3.2 A against the SY8303's 4-6.8 A limit; Lite L5 (SWPA6045S100MT) at 3.2-3.5 A against the TPS54302's 4-6 A. Normal peaks are 1.73 A (core, about 1.3 A load) and about 1.3 A (Lite), so this only matters in an output short. TI's EVM uses a 4.3 A inductor. | none (optional: a 4.5 A+ part) |
| R3-POWER-9 | core | note | POWER-13 | On a 5 V-only port, core +5V sits at about 4.4 V (SY8303 Dmax ~87 %) and the ultra-front backlight dims. With a 5 V-only *PD* source, status bit 3 is set, so firmware thinks the board has 20 V (R3-POWER-4). The hardware PG keeps Q1 off and the TPA3220s sit in UVP, so this is harmless but silent. Covered by the R3-POWER-4 fix. | via R3-POWER-4 |

## 2. Lead's question: Lite U2 (TPS54302) EN left unconnected

**Floating is correct. Don't add a divider. Never tie EN to VBUS.**

- **On the board.** U2 pad 5 is alone on net `unconnected-(U2-EN-Pad5)`, and no copper lands on it (`lite_u2_en.py`, PASS). Its neighbours are pad 4 BUCK5_FB and pad 6 BUCK5_BST, each 0.95 mm away.
- **Datasheet.** SLVSDG6C table 4-1 says "Float the EN pin to enable", and section 6.3.5 describes an internal pull-up current source (Ip 0.7 uA). The enable threshold is 1.23-1.28 V rising and 1.1-1.16 V falling. EN's absolute maximum is 7 V, so tying it to VBUS (20 V) would destroy it.
- **5 V-only port.** The internal VIN UVLO turns on at 3.8-4.4 V and off at 3.3-3.9 V. The buck starts from 4.75 V minus a ~0.05 V cable drop and runs at 100 % duty (section 6.3.10), giving +5V of about 4.6-4.7 V. The SY8089 and LP5907 still make their 3.3 V rails, so the ESP32 runs and can report that there's no contract.
- **Higher charger voltages.** QC 9/12 V and PD 9-20 V (+5 %) sit inside the 4.5-28 V recommended range.
- **Why not copy TI's EVM.** Its R4 510k / R5 105k divider starts at 6.74 V. On a 5 V port that leaves the board dark.
- **Noise.** BOOT coupling into the 0.7 uA node is capacitive, about 0.05 pF against a few pF. That is a few hundred mV of AC, well under the 1.1 V falling threshold.
- **Optional for a later spin.** A DNP 1-10 nF EN-GND footprint (start delay 1.8-18 ms).

## 3. Blocks, with reference comparison and margin

**USB-C PD input, CH224A (Lite) and CH224Q (core)**, compared against WCH V2.0 fig 6.1.1. `power_netrules.py` confirms:

- VHV and VBUS are tied, with 1 uF.
- Rset is 120k on Lite (20 V) and 210k on core (28 V).
- PG goes to PD_PG with a pull-up, and the EP goes to GND.
- There is no external Rd, which matches the reference schematic.

Differences from the reference and their margins:

- Core feeds VHV through R17 10R with C1 1 uF (the reference connects it directly). The 1.8-12 mA ICC drops 0.12 V at most. The R17/C1 RC (≈6 us) shaves the 43.6 V non-compliant spike down to 28.8 V at the chip.
- VHV absolute maximum is 32 V. Against EPR's 28 V + 5 % = 29.4 V that is a 2.6 V margin. The CH224's own OVR trips at 32-34 V.
- TVS stand-off: SMBJ22A against 21 V (1 V) on Lite, and SMBJ30A against 29.4 V (0.6 V) on core.
- Compliant 5 V hot plug with a ceramic-only input rings to 9.0 V on Lite and 8.9 V on core (`*_hotplug_5v`). That is 3x under the lowest absolute maximum. Q1 stays off: Vsg is 0.20 V on Lite and 0.47 V on core.

**PD transitions with the firmware off** (`*_pdstep_*`). At the PD maximum slew of 30 V/ms:

- Lite 5 → 20 V: Vsg 0.79 V against Vth min 1.7 V (0.9 V margin), PVDD 3 mV.
- Core 5 → 28 V: Vsg 1.21 V (0.5 V margin), PVDD 2 mV.

Round-1 POWER-3 and POWER-7 hold.

**PVDD turn-on with the firmware on:**

- Lite at 20 V: 0.41 A, 5.6 W, 0.14 J, t90 75 ms after EN. soa_check gives Tj 65 °C at 40 °C ambient. At 15 V: 0.26 A, and Vsg settles at 4.7 V. Against firmware `soft_start_ms` = 400 and `pvdd_timeout` = 1 s, both pass with 2-5x margin. POWER-4 holds.
- Core fails: see R3-POWER-2.

**Hard reset with PVDD on** (`*_hardreset_pvdd_on`). The source drops VBUS through a 200 Ω discharge. VBUS reaches 0.16 V on core and 0 V on Lite by tSafe0V (650 ms). The bulk drains back through Q1's body diode, peaking at 0.34 A on core. Core's fast-off (R404/D5/Q3b) holds. The margin depends on the source's discharge strength; at ≥1 kΩ it is unverified.

**TPS54302 (Lite)**, compared against SLVSDG6C table 7-2 and the EVM BOM:

| | ours | TI | verdict |
|---|---|---|---|
| L | 10 uH | 10 uH | ✓ |
| Cout | 3 × 22 uF 0805 X5R = 38.7 uF effective (30 uF low) | 2 × 22 uF X7R 1210 ≈ 38 uF effective | equal |
| fo (eq 14) | 25-33 kHz | < 40 kHz | ✓ |
| Feed-forward | 47 pF | eq 16 gives 48-64 pF; EVM 75 pF | ✓ |
| Vout | 5.18 V (100k/13k) | 5.08 V | fine |
| Cin | 7.5 uF effective at 20 V | EVM 10 uF/35 V ≈ 6 uF at 24 V | ✓ |
| Input ripple (eq 4) | 83 mV | 400 mV spec | ✓ |
| BOOT cap | 100 nF | 100 nF | ✓ |

TI field issue (E2E): oscillation with all-ceramic Cout and no feed-forward. Immune, thanks to fo < 40 kHz with 47 pF.

**SY8303 (core)**, compared against AN_SY8303 Rev 0.9A:

- Vout 5.10 V; fsw 1.05 MHz (105/RFS).
- Minimum on-time at 29.4 V is 165 ns against 80 ns (2x margin).
- BS cap 10 nF, as specified.
- EN on a 100k/100k divider: 2.5 V at 5 V input and 14 V at 28 V, against 40 V absolute maximum and 1.5 V high threshold.
- Inductor ripple at 29.4 V is 0.86 A, so the peak is 1.73 A against Isat 3.2 A.
- Cout 2 × 22 uF = 25.8 uF effective against the datasheet's "> 22 uF" (1.17x).
- Cin: R3-POWER-7.
- Feedback divider: R14 = 2k is below the recommended 10k-1M. That costs 2.5 mA and is harmless.

**SY8089 (both)**: 3.33 V. Cout is 22 uF + 10 uF on Lite (23 uF at 3.3 V) and 2 × 22 uF on core (34 uF). Lite has 22 pF feed-forward. Matches Silergy's typical 2.2 uH / 22 uF.

**LP5907 (both)**: +3V3A from +5V. Dropout 250 mV at 250 mA, against an input of 4.4-5.18 V. ✓

**Bulk caps**:

- Lite: C58 polymer, 3.8 A at 100 kHz, 16 mΩ; C59 is low-frequency only.
- Core: 3 × PX 470 uF/50 V. Per the LCSC sheet: 600 mA rms at 120 Hz, × 1.2 above 10 kHz = 0.72 A each, 2.16 A total; tanδ 0.12, so ESR ≈ 0.34 Ω at 120 Hz.
- TI SLASEN3B section 10.2.1.2.2 recommends 470 uF/50 V *low-ESR*. Ours is general-purpose. The governed power (19 W, 60 s) keeps ripple well under the rating. That is THERMAL-5's justification, which still holds. Round 1's sims modelled 50 mΩ; this round uses 0.2 Ω.

**ESP32 brown-out**:

- With cable sag at 5 A × 0.2 Ω = 1 V, the bucks never approach their UVLO. The ESP32 brown-out threshold is ~2.43-2.7 V against a 3.3 V rail.
- Wi-Fi TX bursts of 0.5 A against the SY8089's 2 A rating: ✓
- The only real brown-out path is a charger trip, which is R3-POWER-3.
- On a hard reset the bulk holds the buck input for ~0.2 s (Lite: 0.13 J at ~0.6 W), shorter than tSrcRecover. So the ESP32 reboots, as expected.

**Ultra-front supply**: +5V and +3V3, each on 2 FFC pins (3/18 and 2/19). The backlight draws (5 − 3.1)/40 Ω ≈ 48 mA and the LCD ~10 mA, against ~0.5 A per pin: more than 10x margin. C2/C3 provide 10 uF bulk at the connector. ✓

## 4. Tests built

| path | checks | result |
|---|---|---|
| sims/power/lite_u2_en.py | Lite U2 EN pad has no net and no copper | pass |
| sims/power/power_netrules.py | CH224 wiring against the WCH reference, Rset, PG interlock, buck set points and fsw/on-time, EN handling, TVS position, bulk count | pass |
| sims/power/core_power_vias.py | ≥ 5 vias (6 A) at R1, Q1 S/D and each amp on the 5 A nets (round-1 POWER-1) | pass (5-10) |
| sims/power/charger_peak.py | charger peak current at the governor's budget for sustained tones | **fail** (1.68-1.77x), R3-POWER-3 |
| sims/power/pvdd/*.cir + bounds (gen.py; run with hwde sim_run.py from that directory) | 16 benches: firmware turn-on (inrush/SOA/t90), PD step with firmware off, hot plug at 5 V and live 20/28 V, EPR step with Q1 on, hard reset; core FIX benches | lite pass, except hotplug_20v (warning); core_fw_on_* **fail** (t90, pfet), FIX pass; core_epr_step warning. See pvdd/RESULTS.txt |

## 5. Field issues searched

| issue | source | our board | how |
|---|---|---|---|
| CH224 fallback when the requested PDO isn't offered | felixardyansyah/CH224A_MODULE_FGV tests (Dell 65 W: 28 V "not supported"); prototypeguru CH224K guide | **unknown** | Core's firmware assumption is R3-POWER-4. Bench test with 18/30/45/65 W chargers. |
| CH224 on QC/BC chargers: A-to-C cables give QC3 9/12 V only | felixardyansyah tests (IP6505, power bank) | immune | Lite's v_start of 14 V keeps the amp off at 12 V. Core needs PD bit 3. |
| PD charger trips or browns out under class-D bursts | diyaudio "TPA3116 cutting out"; dorkythorpy TPA3116 supply notes | **has it** | R3-POWER-3 (1.7x PDO peak at budget) |
| TPS54302 instability with all-ceramic Cout | TI SLVSDG6C 7.2.3.5.2/3 (fo < 40 kHz, C6 eq 16) | immune | fo 25-33 kHz, 47 pF |
| Type-C hot-plug overshoot with ceramic-only input | TI Type-C hot-plug app notes (not retrieved: the TI site returned HTML) + our sim | immune (compliant) / has it (non-compliant) | 9 V at a 5 V attach; R3-POWER-6 |
| Large sink capacitance back-feeding VBUS through the P-FET body diode on hard reset | USB PD tSafe0V/vSafe0V; our sim | immune with ≥ 200 Ω source discharge | VBUS 0.16 V at 650 ms |
| Inrush into large bulk exceeding a high-side FET's SOA | hwde soa_check / AO4485 SOA | **has it** (core, regression) | R3-POWER-2 |

## 6. Round-1 fixes re-checked

POWER-1, holds:

- `core_power_vias.py` finds 5-10 vias per node.

POWER-2, **regressed**:

- C311 raised the bulk from 940 to 1410 uF. That gives 14.8 W peak and Tj 125-135 °C. See R3-POWER-2.

POWER-3 and POWER-4, hold:

- Vsg 0.79 V.
- Turn-on 0.41 A / 5.6 W.

POWER-5, holds:

- TCA9539, with R43 100k holding PVDD_EN low.

POWER-6, holds:

- Q2's source is on PD_PG.

POWER-7, holds:

- Vsg 1.21 V against 1.7 V.

POWER-8, POWER-9 and POWER-12, justified:

- POWER-9 is now R3-POWER-6.
- POWER-12 was not re-measured (layout).

POWER-10, holds:

- 38.7 uF Cout.
- 47 pF feed-forward.

POWER-11, **justification wrong**:

- The body diode doesn't help at HF. See R3-POWER-7.

POWER-13, holds on Lite but leaks on core:

- See R3-POWER-4 and R3-POWER-9.

Core fast-off (R404/D5/Q3b), holds:

- The hard-reset sim gives VBUS 0.16 V at 650 ms.
