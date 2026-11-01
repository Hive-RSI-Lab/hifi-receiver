# R5 PA: core (Plus and Ultra) power and amplifier

Reviewer PA, review round 5 (`docs/review-5/brief.md`, method from
`docs/review-3/brief.md`). Board: `boards/core` at origin/main 078aaa9
(netlist dated 2026-10-31 15:10, PCB after the amp ECO, the Zobels and the ESD
change). Domains: the USB-C PD/EPR input (CH224Q), the PVDD switch, soft start
and inrush, the rails, both TPA3220 stages, their bootstrap and PVDD
decoupling after the ECO, the LC filters, the Zobels, the speaker terminals,
thermal, 4 and 6 ohm loads, pour and via current (issue #92), and the
firmware's power sequencing and governor.

References: TI SLASEN3B (TPA3220 data sheet, rev B, Aug 2018), AOS AO4485
Rev 1.1 (Mar 2024) figs 9-11, TI INA186 data sheet table 5-1, WCH CH224 V2.0
(as checked in review-3 `power_netrules.py`), the TPA3220EVM practice as
quoted in review-3 (`docs/review-3/amp.md`, SLAU754A), JLC's 4-layer order
options as written in `boards/core/fab/ORDERING.md`.

Tests are in `docs/review-5/sims/pa/`. Each one is read-only and exits non-zero
on a fail. Its output is saved next to it as `out_*.txt`.

## Verdict

**0 blockers, 2 majors, 7 minors, 4 notes.** Core is not converged in these
domains:

- **R5-PA-1 (major).** The hardware half of R3-POWER-2 is not on the board.
  The brief says PR #142 added 100 nF across C4 and 2.2 uF across C8, but the
  PVDD_G net still has only C4 100 nF and C8 2.2 uF (`core.net`,
  `core.kicad_pcb`), and `docs/boards/core-amp-eco.md` doesn't mention them.
  Q1's turn-on into 1410 uF peaks at Tj 125 C at 25 C ambient, and goes over
  150 C from about 40-50 C ambient. The fix needs no copper: change two values.
- **R5-PA-2 (major, firmware).** DJ direct has no limit on the charger's time
  scale. With a deck at full line level, the input runs 1.13-2.5x the contract
  on Plus with 4 ohm speakers and a 100 W or smaller charger, 1.18-1.7x with
  6 ohm and a 65 W or smaller charger, and 1.2-4.9x on Ultra with both zones
  playing, even on 140 W EPR.

## Findings

| id | sev | repeat_of | finding | fix |
|---|---|---|---|---|
| R5-PA-1 | **major** | R3-POWER-2 | Q1 (AO4485) soft-start SOA. The PVDD ramp is set by C4 = 100 nF gate-drain, as before the round-2 fix. Measured on the board: PVDD_G = {C4 100n, C8 2.2u, D4, Q1, Q3, R4, R5}. Sim (review-3's testbench and models, swept): 14.1-16.8 W peak, 0.55-0.61 J, 0.67-0.75 A. hwde `soa_check` on that waveform gives Tj 122-138 C at 25 C ambient, 137-153 C at 40 C, 147-163 C at 50 C and 157-173 C at 60 C, against the 150 C limit. This happens on every attach after VBUS was lost: a re-plug, or a charger OCP or hard reset mid-play (R5-PA-2). A trench FET in linear mode at 0.7 A and 28 V is far below its ZTC point (~-4 V, ~70 A, fig 2), so it can hot-spot before the thermal SOA says it will. Firmware can't slow this ramp. | **BOM swap on the existing pads, no copper change.** C4 to 220 nF/50 V X7R 0603 (C21120, as on Lite; confirm the 50 V rating). C8 to 4.7 uF/25 V 0805 (C1779, already in core's PASSIVES; C8 sees at most D4's 15 V). Sim with derated values (150 nF, 3.5 uF): 9.2-10.9 W peak, Tj 94-106 C at 25 C and 129-141 C at 60 C. PD-step hold-off Vsg is 0.95 V (as built 1.21 V; Vth min 1.7 V). t90 is 188-257 ms after EN, under the 500 ms `soft_start_ms` (1.9x). Alternative: review-3's parallel parts, which need new pads. |
| R5-PA-2 | **major** | new (gap left by R3-SYSTEM-4 and R3-POWER-3) | In DJ direct the line in reaches the amps through the TS5A23157 with no volume or governor. The guard (`hifi_line.c` `hifi_djg_*`) compares a **60 s** average of input power with the thermal budget (Plus 25.6 W, Ultra two zones 23.3 W) and watches OTW. Nothing acts on the charger's ms time scale, which the governed path covers with `peak_frac` 1.0 per ~5 ms block. FLX4 at 2.1 Vrms gives 0.916-0.950 Vrms at the amp input (4.7k/4.7k, 100k bias, RIN 24k +-20 %), so 14.5-15.1 Vrms (21 Vpk) at the speaker. On a full-level bass passage: Plus 4 ohm draws 113 W on a 20 V contract (1.13x 100 W, 1.74x 65 W, 2.51x 45 W) and 129 W on 140 W EPR (0.92x). Plus 6 ohm draws 77 W at 20 V (1.18x 65 W, 1.70x 45 W). Ultra with two zones draws 254 W (4 ohm) or 171 W (6 ohm) on 140 W EPR, 1.82x and 1.22x. The guard trips only after 5.8-24 s. The charger OCPs, VBUS goes, and the board reboots back into DJ direct, because the input is saved in NVS (R3-SYSTEM-9). Then it repeats, and each re-attach also takes R5-PA-1's 0.55 J pulse in a warm box. | Firmware, no board change. (1) In DJ direct, use the INA186 reading already taken every 10 ms step (`bp.vbus_i_a`). If it is above the contract current (`contract.i_max_a`) for 2 consecutive steps, hand over to the governed line exactly as the guard does now. (2) Offer DJ direct only when the contract covers the worst case for the fitted amps: Plus needs 140 W EPR at 4 ohm or 100 W at 6 ohm, and Ultra two-zone needs one zone, or its own limit. (3) Host test: a 0 dB 50 Hz sine at 0.936 Vrms into the line path on a 65 W contract must leave DJ direct within 30 ms. |
| R5-PA-3 | minor | R3-AMP-2 | The ECO moved each pin group's 1 uF (now 100 V 1206, which keeps its value at 28 V) to B.Cu under the pins. The HF loop now runs through the board: F.Cu strip 3.85 mm, then the PVDD via column, then 1.10-1.12 mm of B.Cu, the cap, 0.49 mm to the EP vias, and up. With review-3's own estimator (7 A in 3 ns, mu0·h/w per segment, 0.5 nH per via group) that is 3.49-3.51 nH, giving an 8.1-8.2 V spike and 37.5-37.6 V peak at 29.4 V. Before the ECO it was 3.3 nH and 37.1 V. The capacitance is better, the loop inductance is not. The estimator is crude, since TPA3220 edge rates aren't specified, and TI's EVM at 32 V max PVDD implies a few volts of overshoot with a ~1.6 nH loop. | Bring-up gate: scope PVDD pin 30 to GND pin 33 on U15/U16 at 28 V into 4 ohm near the clip. The peak must stay ≤ 35 V. Firmware fallback if it doesn't: ask for 20 V, not 28 V (2.9 dB less peak headroom; sustained output is thermally capped well below either). Next layout touch: put a via in each 1206 PVDD pad to the strip and In2, and one at its GND pad to In1/EP. That removes the 1.1 + 0.5 mm B.Cu runs, giving about 1.7 nH and about 4 V. |
| R5-PA-4 | minor | issue #92 | The 5 A path runs on In2 alone. No other layer connects any hop by itself. Eroded-copper necks, all layers joined: J1 to R1 (VBUS_IN) 5.79 mm, R1 to Q1 (VBUS) 5.39 mm, Q1 to U15/U16 PVDD pins 3.88 mm, all on In2 (`power_path.py`). `fab/ORDERING.md` orders **0.5 oz inner**. `constraints.json` power_notes say "In1/In2 are 1 oz", and the KiCad stackup has 35 um inner, which hwde's check_current reads. At 0.5 oz, the 3.88 mm PVDD neck carries 5 A at dT ≈ 35-40 C steady (≈2.7 A at dT 10), and VBUS/VBUS_IN about 3.5 A at dT 10. The real load: sustained input is capped by the thermal governor at about 25.6 W (0.9 A at 28 V, 1.3 A at 20 V). 5 A happens only in bursts of ≤ ~13 s (and in DJ direct, see R5-PA-2). check_current at 5 A no longer reports these necks, because the nets are `plane_fed` (343 advisory warnings, 0 errors). GND return passes: 5.2-5.4 mm on In1 plus B.Cu (`gnd_return.py`). | Order **1 oz inner** (a JLC option; small cost) and set In1/In2 to 35 um in ORDERING.md to match the stackup and power_notes. The PVDD neck then carries 5 A at dT ≈ 12 C. Close #92 on that basis. Optionally widen the In2 PVDD neck at the next touch. |
| R5-PA-5 | minor | new | Speaker-side copper (inductor to terminal) necks to 0.89-1.60 mm. The narrowest is Plus's right speaker +, L8 to J11, at 0.89 mm on F.Cu: 2.2 A at dT 20, 3.0 A at dT 40. A_SPK_L-/A_SPK_R+ are 1.20 mm, the others 1.27-1.60 mm (`spk_path.py`). The OUT/SPK nets are budgeted at 0.5 A in constraints.json, so no gate checks them at the speaker current. Real currents at 4 ohm: 1.55 A rms sustained (Plus thermal cap 9.6 W/ch), 3.7 A rms for 55 W/ch DJ-direct bursts until the guard, 4.7 A rms for a clip sine. The pin-side necks (0.45-0.67 mm) are pad exits off 0.4 mm TPA3220 pins, as on any DDW layout, and are accepted. | Next layout touch: widen the SPK pours to ≥ 1.7 mm (3.74 A at dT 20, 1 oz), starting with B_SPK_R+, or add a stitched B.Cu copy. Set the OUT/SPK budgets in constraints.json to 3.7 A so check_current tests them. |
| R5-PA-6 | minor | R3-AMP-1 | Bootstrap caps are now 1.32-2.52 mm from their BST pins (BST copper 1.3-3.0 mm). For BST1M and BST2P, though, the OUT side goes back through a 0.45 mm via under pin 35/32 and 10.3-11.4 mm of 0.4 mm B.Cu (`core-amp-eco.md`). Review-3 asked for ≤ 5 mm. That leg alone is ~8 nH (0.4 mm over In2 at 0.244 mm), about 3x less than before the ECO (19-34 mm). | Bring-up: scope BST1M to OUT1M at 28 V, 4 ohm near the clip, and confirm no half-bridge drop-outs (bootstrap UVP is non-latching and doesn't show on FAULT). No change for the first spin. |
| R5-PA-7 | minor | new | The firmware's Zobel guard (`hifi_tone_check`, sine above 5 kHz capped at -16 dBFS) covers only the API's own tones. A full-level high-frequency tone from the deck reaches the 4.7 R 0603 (0.1 W) through DJ direct or the governed line in. At 14.8 Vrms that dissipates 79-220 mW at 10 kHz and 177-484 mW at 15 kHz per resistor (`zobel_power.py`). Music stays at 1-7 mW. A resistor that opens leaves the undamped filter (+40 dB open-load peak) and is not a fault. | Accept for the first spin. Note in the DJ guide: no sustained full-level test tones above 5 kHz. Later spin: 1206 resistors (the core-zobel.md "not done"). |
| R5-PA-8 | minor | R3-POWER-7, R3-LAYOUT-15 | SY8303 input capacitance and loop are as in round 2. C10 10 uF/50 V X5R 1206 is ~2.6 uF at 28 V against "≥ 4.7 uF", and C11 sits 4.0/7.2 mm from the pins. The second 10 uF (C13585) wasn't added. | As review-3: add a 10 uF/50 V 1206 (C13585) at U4 IN and put C11 across pins 5-4 at the next layout touch. Bench: IN spike ≤ 35 V at 28 V, 2 A. |
| R5-PA-9 | minor | R3-LAYOUT-1 | Half done. U15's input caps moved (A_IN1P 6.9 mm, A_IN2P 9.3 mm), but U16's didn't: B_IN1P 24.6 mm, B_IN2P 13.5 mm and B_IN2M 39.1 mm of high-impedance copper (measured). On Plus, U16 plays the right channel. Shorter than before (45-56 mm), but the right channel is now the less clean one. | For the audio and layout reviewers to weigh. Same move as U15 (C316/C318 under pins 8/15) at the next touch. |
| R5-PA-N1 | note | R3-POWER-5 | `hifi_ch224_settled()` takes the SPR contract 3 s after a PD contract when EPR is offered but not entered (for example a non-e-marked cable). If the CH224Q entered EPR later, VBUS would step 20 to 28 V with PVDD closed (review-3's 26.6 A ideal-source case), because the EPR bit doesn't reset `t_pd`. That needs an EPR entry later than 3 s, which wasn't seen. | Firmware, cheap: open PVDD (`pg` false for 500 ms) on any change of status bit 4. |
| R5-PA-N2 | note | new | 4 ohm loads. At the 28 V clip, a 4 ohm speaker with a 3.2 ohm impedance minimum draws 26.6/3.2 = 8.3 A plus 0.76 A ripple, so 9.1 A peak. The IOC is 10 A typical, with no minimum given, and CBC over 1.7 ms latches OLP. TI's ROC allows 2.7 ohm at 30 V, so this is inside the data sheet. 6 ohm draws 5.2 A. The inductor's Isat of 10 A (-30 %) keeps more than TI's 5 uH minimum (R3-AMP-3). | none |
| R5-PA-N3 | note | R3-POWER-6 | The OV trip (D2 BZT52C33, R8/R9, Q3a) acts at about 32.3-36.3 V. That is 2.9 V over the 29.4 V EPR maximum and up to 0.7 V under the TPA3220's 37 V absolute maximum. Hot-plug overshoot into a non-compliant live source (43.6 V) is as in round 2. | none |
| R5-PA-N4 | note | new | The speaker terminals are KF301-5.0-2P screw terminals (C474881, 17 A) on J6/J7/J10/J11, not binding posts. They are rated for the current. | none |

## Round-2 fixes checked

| R3 id | holds | how |
|---|---|---|
| R3-POWER-2 | **no** (half) | Firmware half holds: `soft_start_ms` 500 and `settle_ms` 500 (`hifi_power.c`), against t90 110-147 ms after EN as built (3.4x). Hardware half: **absent**. PVDD_G nets and footprints are unchanged (C4 100n, C8 2.2u). See R5-PA-1. |
| R3-POWER-3 | yes (governed path) | `hifi_gov.c`: `peak_frac` 1.0 on each block, held 0.5 s. DJ direct is outside it: R5-PA-2. |
| R3-POWER-4 | yes | `board_core.c` reads SRCCAP 0x60-0x8F and `hifi_ch224_request()` asks for the highest fixed PDO ≤ 20 V. 5/9/12 V stay under the 14 V start floor, so PVDD stays off. 15 V starts with Vsg 7.5 V. |
| R3-POWER-5 | yes, with N1 | PVDD waits for EPR entry (bit 4) up to 3 s, then 500 ms of settle after any VREQ write. |
| R3-POWER-7 | no | Not done (minor): R5-PA-8. |
| R3-AMP-1 | partly | BST side 1.32-2.52 mm. OUT side of BST1M/BST2P 10.3-11.4 mm: R5-PA-6. |
| R3-AMP-2 | partly | Caps 1.91/2.40 mm centre to pin and now 100 V 1206, but on B.Cu, so the loop is ~3.5 nH against 3.3 nH before: R5-PA-3. |
| R3-AMP-4 (issue #137) | yes | 470 nF/50 V X7R 0603 (C513577) on all 8 legs (`core.net`). Open-load peak +15.8 dB per `core-zobel.md`. Tone guard in `hifi_tone.c`. Gaps: R5-PA-7. |
| R3-LAYOUT-1 | partly | U15 yes, U16 no: R5-PA-9. |
| R3-SYSTEM-1 | yes | Gain from the unclipped program power, 50 ms window, 10 ms attack. The core `vfs_peak_v` of 25.8 V is above the measured chain's 22.4 V, which makes it conservative by 1.33x in power. |
| R3-SYSTEM-4 | yes (thermal), gap (charger) | 60 s guard and OTW step-down are in (`hifi_main.c` power task). The charger time scale is missing: R5-PA-2. |
| R3-AMP-6 | yes (note) | PG removes PVDD in hardware. Amps go to reset on the next 10 ms step. |
| check_amp_layout / check_amp_netlist (review-3) | pass | Re-run on this tree: 0 fails each (gain 24 dB master/slave, 600 kHz, OSC polarity, BST 33 nF, CMUTE, OTW/FAULT, decoupling distances). |

## Blocks, against references, with margins

**USB-C PD input (CH224Q).** Wiring is as WCH V2.0 fig 6.1.1, per review-3's
`power_netrules.py`, and nothing has changed since: VHV and VBUS on VBUS_PD
with 1 uF behind R17 10 R, CFG1 210k for 28 V, PG to PD_PG with 10k. The CC
clamps are NUP2105L (24 V working, ~30 pF against PD's 620 pF cReceiver).
D1 SMBJ30A: Vrwm 30 V against the 29.4 V EPR maximum (margin 0.6 V, 1 uA
leakage).

**PVDD switch.** AO4485: 40 V Vds against 29.4 V (1.36x), Vgs ±20 V, with D4
clamping at 15 V. Gate drive is VBUS/2: 7 V at the 14 V start floor and 14 V
at 28 V. The PG interlock is in hardware: Q2's source is on PD_PG. Fast-off
(R404/D5/Q3b) and the OV trip are as in review-3. Turn-on SOA: R5-PA-1.
The INA186 pins match TI table 5-1 (IN+ 4 on the bus side, IN- 5, OUT 6, REF 1
to GND). Its 0.25 V/A goes into the ESP32 ADC's 6 dB range (7 A full
scale).

**Rails.** SY8303 at 29.4 V: 165 ns minimum on-time against 80 ns, and EN
divider 14 V against 40 V (as review-3). Cin: R5-PA-8. The LP5907, SY8089 and
the ferrite branches are unchanged.

**TPA3220 stages** (SLASEN3B tables 2, 3 and 7.3; TI EVM):

- Gain: 20k/100k master and 75k/47k slave, both 24 dB.
- FREQ_ADJ: 50.0k gives 600 kHz (49.5-50.5k).
- GVDD/AVDD: 4.7 R + 1 uF from +5 V, as TI fig 29.
- BST: 33 nF, as the data sheet asks.
- CPVDD: now 1 uF/100 V X7R 1206 + 100 nF per pin group, against TI's 1.0 uF.
- PVDD maximum 29.4 V against 32 V recommended (2.6 V) and 37 V absolute
  (7.6 V, part used by the edge spike: R5-PA-3).
- Input caps 10 uF against TI table 3's 10 uF at 24 dB, giving 0.66 Hz.

**LC filter and Zobels.** 10 uH (PSPMAA0805, Isat 10 A at -30 %) with 1 uF/50 V
0805 per leg. TI's EVM uses 10 uH/1 uF. At 14 V bias the 0805 1 uF is about
0.5-0.6 uF, so f0 is about 65-70 kHz. Zobels are 4.7 R + 470 nF: open-load
peak +15.8 dB at 43 kHz (core-zobel.md), with 9-24 mW at the tone guard's
limit (R5-PA-7).

**Thermal.** TPA3220 pad-down at ~26 C/W. Constraints say that already
includes 0.5 oz inner, so R5-PA-4's stackup mismatch doesn't change it.
Plus's cap is 19 W total (9.6 W/ch, one channel per chip), ~1.0 W per chip
at the governed limit (R3-LAYOUT-3). The firmware's 2.0 W/chip budget gives
Tj ≈ 110 C in the vented box at 30 C room (hwde check_thermal). Both are
under OTW's 115 C minimum, and OTW triggers the -3 dB step. Q1 conduction at 5 A:
5² × 15-21 mΩ = 0.38-0.53 W, a burst only. R1 2512 10 mΩ: 0.25 W at 5 A.

**Current capacity (#92).** R5-PA-4 for the power path, R5-PA-5 for the
speakers. GND return passes.

**Speaker terminals.** R5-PA-N4.

## Field issues searched, and what core does about each

| issue | source | core | how |
|---|---|---|---|
| Hot-swap P-FET dies in linear mode charging a large amp bulk (USB-PD decoy amp boards, TPA31xx boards on PD triggers) | AO4485 SOA fig 9/10, AOS/TI hot-swap app notes (trench FETs below ZTC) | **has it** | R5-PA-1: Tj 125 C at 25 C ambient, over 150 C from ~45 C. |
| PD charger OCP / hard reset on class-D bass peaks | review-3 R3-POWER-3; TPA3116/3118 PD-board threads | governed path immune, **DJ direct has it** | R5-PA-2 |
| TPA32xx PVDD overshoot from distant HF decoupling | SLASEN3B layout section; TI E2E TPA3221 PVDD ringing threads | unverified | R5-PA-3: estimate 37.5 V; bench gate and firmware 20 V fallback |
| Bootstrap UVP drop-outs from long BST/OUT loops | SLASEN3B 9.4 (BST UVP non-latching) | mostly fixed | R5-PA-6 |
| Class-D LC peaking with no load | TI SLAA701; review-2 AUDIO-12 | fixed | Zobels, +15.8 dB |
| Zobel resistor burnout on HF test tones | DIY TPA boards with 0603 Zobels | partly | API tones guarded; DJ/line-in not (R5-PA-7) |
| 4-layer order at 0.5 oz inner while the design assumed 1 oz | JLC 4-layer order options; ORDERING.md | **has it** | R5-PA-4 |
| CH224 with a non-e-marked cable: EPR offered but not entered | WCH CH224 V2.0; PD 3.1 EPR needs a 5 A EPR cable | handled | 20 V fallback after 3 s (N1) |
| Speaker impedance dips under 4 ohm tripping OC | SLASEN3B 7.3 (2.7 ohm min at 30 V), 7.5 OLPC 1.7 ms | within spec | N2 |

## Tests built

All are in `docs/review-5/sims/pa/`. Python 3, read-only. Those marked
`$HWDE_PY` need hwde's venv (`eval "$(~/modules/bin/hwde env)"`).

| test | checks | result |
|---|---|---|
| `pvdd_softstart.py` ($HWDE_PY; ngspice + soa_check) | Q1 turn-on as built, with review-3's fix and with the BOM swap. Vth 1.7/1.9/2.5 V, VBUS 28/29.4 V, Ta 25-60 C. PD-step hold-off. | **fail** as built (Tj 140-153 C at 40 C); swap passes (`out_pvdd_softstart.txt`) |
| `dj_direct_power.py` | DJ-direct input power against 45-140 W contracts, Plus/Ultra, 4/6 ohm; guard trip time | **fail** (`out_dj_direct_power.txt`) |
| `pvdd_hf_loop.py` | Post-ECO PVDD decoupling loop and spike, with review-3's estimator | **fail** by that estimator (37.5-37.6 V); bench gate |
| `power_path.py` ($HWDE_PY) | Per-layer and all-layer necks on VBUS_IN/VBUS/PVDD (issue #92) | pass on all layers; only In2 carries it |
| `gnd_return.py` ($HWDE_PY) | 5 A GND return from U15/U16/C6 to J1 | pass (5.21-5.44 mm) |
| `spk_path.py` ($HWDE_PY) | Speaker-current necks from pins to L and L to J | **fail** at 3.74 A (minor, R5-PA-5) |
| `zobel_power.py` | Zobel R dissipation for music, tone guard and DJ tones | **fail** for full-level HF tones through DJ/line (minor) |
| review-3 `check_amp_layout.py`, `check_amp_netlist.py` | Re-run unchanged | pass, 0 fails |
| hwde `check_current` (on a /tmp copy) | As-is constraints | 343 advisory warnings, 0 errors (plane-fed, so it is blind to the necks above) |
