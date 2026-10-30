# Review round 3 (Lite) — ALS: Lite's audio chain, layout/DFM/thermal, field use

Board `boards/lite-main` at e2ea89f. Read-only. Tests are in `docs/review-4/sims/als/`, with outputs in `out/`.

**Result: 0 blocker, 0 major.** 4 minor (2 new from the mute's corners, 1 new firmware, 1 repeat ESD with new
numbers), 2 minor repeats left open from round 2, 3 notes.

| id | sev | repeat | one line |
|---|---|---|---|
| R4-ALS-1 | minor | new | Mute off-state with Vth-min 2N7002s in a warm box: THD 0.1–0.39 %, −56 dB L→R at the −6 dB cap. Fix: SUB_CODE_MAX 24 |
| R4-ALS-2 | minor | new | DAC_CONTROLSET init sets LOUT2/ROUT2 to 0 dB for ≥ 2 s before the cap applies. Fix: init regs 48/49 = 26 (or 0) |
| R4-ALS-3 | minor | new | Q12 trip moves −13 mV/K. At 70 °C with Vth-max FETs: −0.33 V thump if the codec collapses at 3.4 V. Fix: bring-up test, R81 → 20k contingency |
| R4-ALS-4 | minor | R3-SYSTEM-7 | Still no TVS at J3. A −8 kV tip strike now puts 31.5 V on the mute FETs' Vgs. Fix: PESD5V0S2BT (C49338) at J3 |
| R4-ALS-5 | minor | R3-LAYOUT-14 | Still no ESD on USB D+/D−/CC |
| R4-ALS-6 | minor | R3-AUDIO-5 | Stale firmware constants (LINE_DIVIDER 2.0 vs 2.5, HIFI_HAS_TAPS 0) |
| R4-ALS-7 | note | new | Mute ECO layout, CPL, GND pour and JLC stock all pass |
| R4-ALS-8 | note | new | Cold-start VMID ramp at the sub: 22–45 mV after the sub's HPF (pass) |
| R4-ALS-9 | note | new | Thermal at 9 W/ch: Tj ~102 °C (6 Ω) / 109 °C (4 Ω) |

## Findings

### R4-ALS-1 (minor, new): the mute leaks below threshold on negative peaks with low-Vth parts, warm
- **Topology.** Q13/Q15 have their drains on SUB_L/SUB_R and their sources on the shared MUTE_S. Q14 has its source on MUTE_S and its drain on GND. All three gates sit on SUB_MUTE, which Q12 holds at Vce(sat) 0.07 V.
- **How it leaks.** On a negative swing, Q13's body diode pulls MUTE_S down to SUB_L + Vf. Q14 then sees Vgs = 0.07 − V(MUTE_S), and Q13's reverse channel narrows the gap further.
- **Why round 2's bench passes.** `tb_play_cap` passes at 0.711 V because its level-1 FET has no subthreshold current and no tempco.
- **New model.** `models_ekv.inc` is a charge-based model:
  - 250 µA at Vth;
  - 89 mV/decade subthreshold slope;
  - −4 mV/K tempco;
  - 1.6 Ω at 4.5 V.
- **Results** (`mute_corners.py`), at the cap of 0.71 Vpk, 40 Hz:

| Vth(25 °C) | 25 °C | 50 °C | 70 °C |
|---|---|---|---|
| 1.0 V, 10k sub | 0.009 % | 0.108 % | 0.388 % (−32 mV peak) |
| 1.0 V, 100k sub | 0.011 % | 0.091 % | 0.237 % (−63 mV peak) |
| 1.5 / 2.1 V | 0.0003 % | 0.0003 % | 0.0003 % |

- **Crosstalk.** L→R through MUTE_S at 1 kHz is −56 dB (10k) / −61 dB (100k) at Vth 1.0 V and 70 °C, and −116 dB with typical parts.
- **DJ-direct level** (FLX4 2.1 Vrms → 0.42 Vpk at the sub): clean in every corner (≤ 0.003 %).
- **Why minor.** It takes a tail-of-spec part plus a hot box, and the distortion is inaudible on a sub.
- **Fix.** SUB_CODE_MAX 24 (−9 dB, 0.50 Vpk). At 70 °C with Vth 1.0 V that gives THD 0.014 %.

### R4-ALS-2 (minor, new): the sub cap doesn't hold for the first ≥ 2 s
- **What happens.** The HIFI_LITE DAC_CONTROLSET writes `{"reg":48,"val":30},{"reg":49,"val":30}`, which is 0 dB and 1.41 Vpk. `hifi_bsp_route()` applies code 26 only 2 s after the codec first acks.
- **Why the speakers are fine and the sub isn't.** The amps are held through this window (codec_up + 2.5 s); the sub outs are not.
- **Effect.** At 0 dB with Vth 1.0 V: THD 2.6–3.3 % and 0.4–0.6 V lost off the negative peak. With Vth 1.5 V: 0.14–0.9 %.
- **Fix.** Init regs 48/49 = 26 (or 0). Also set regs 46/47 from 28 to 27 to match ES8388_OUT1_FULL.

### R4-ALS-3 (minor, new): the mute's trip point moves with temperature
- **Q12 DC sweep** with the reservoir at 4.4 V. Columns are the +5V level at which the gate reaches 3 V (mute fully on) and 1 V:

| T | gate 3 V at +5V = | gate 1 V at +5V = | gate at 4.6 V (5 V port) |
|---|---|---|---|
| −20 °C | 4.37 V | 4.57 V | 0.58 V |
| 0 °C | 4.11 V | 4.31 V | 0.10 V |
| 25 °C | 3.77 V | 3.98 V | 0.09 V |
| 50 °C | 3.44 V | 3.65 V | 0.08 V |
| 70 °C | 3.17 V | 3.38 V | 0.08 V |

- **Bench.** `mute_powerloss.py` covers 48 corners. 46 of them give 24–50 mV.
- **The two that fail.** At 70 °C with Vth 2.5 V, a 20 ms +5V fall and the collapse at 3.4 V, the sub sees **−0.33 V** (10k and 100k). That is over round 2's 0.2 V bound but 14 dB under the as-built 1.61 V.
- **What decides it.** Whether this case is real depends on where LOUT2 actually collapses, which is unknown. VMID sits on C39 10 µF, so the collapse is likely below 3.0 V, and there every corner passes.
- **Fix.**
  - Bring-up test: warm box, 10k load on J3, pull the USB-C during play, scope SUB_L and +5V.
  - If LOUT2 collapses at ≥ 3.2 V, change R81 from 22k to 20k (C4184). That raises the trip by ~0.4 V.
  - Don't go lower: at 0 °C a 20k trips near the 5 V-only port's 4.6 V.

### R4-ALS-4 (minor, repeat of R3-SYSTEM-7): ESD at the sub RCA now meets the mute FETs
- **Bench.** `mute_esd.py` models:
  - IEC 61000-4-2 contact discharge (150 pF/330 Ω);
  - a Class-II Y-cap ground (2.2 nF at ±325 V through 10 Ω), the case where the RCA centre pin makes contact before the sleeve;
  - 75 V avalanche on the FETs;
  - Q12's junctions;
  - 10 nH on the SUB_MUTE trace.
- **Results:**

| strike | SUB_L | max Vgs (abs max 20 V) | codec-side current |
|---|---|---|---|
| +8 kV | +112 V | 5.6 V | 1.06 A |
| −8 kV | −110 V | **31.5 V** | 1.07 A |
| −2 kV | −81 V | 17.8 V | 0.79 A |
| Y-cap −325 V | −41 V | 16.9 V | 0.40 A |
| any, with a 6 V bidirectional TVS | ≤ ±7.3 V | ≤ 5.2 V | ≤ 0.07 A |

- **Net effect.** The mute shields the codec: about 1 A now, against round 2's ~19 A. In exchange its gates are exposed, and an 8 kV negative strike exceeds the oxide rating.
- **Fix.** One PESD5V0S2BT (C49338, SOT-23 dual bidirectional, extended part, 22k in stock) at J3: pin 1 SUB_L, pin 2 SUB_R, pin 3 GND by J3 pin 2.

### R4-ALS-5 / -6 (minor, repeats): round-2 minors still open
- USB D+/D− and CC1/CC2 at J1 still have no ESD protection (R3-LAYOUT-14). Only VBUS has D1 SMBJ22A.
- `hifi_board_lite.h` still has LINE_DIVIDER 2.0, though R30/R31 is 2.5. HIFI_HAS_TAPS is 0 with the taps fitted, and the SDZ comment still says 100k (R3-AUDIO-5).

## Blocks passed, with margin and reference

- **Mute ECO netlist and placement.** Every pin in `.net` and in the PCB pads matches root.py (Q13: 1 SUB_MUTE, 2 MUTE_S, 3 SUB_L, and so on).
- **CPL** (`cpl_pin1.py`, with the C8545 model from `boards/lite-main/parts/easyeda/` added to the cache because review-3's cache lacks it): 154 parts fitted, 0 FAIL. The 2 CHECKs are Q14 (D on GND, intended) and U8 (known).
- **GND pour** (`gnd_islands.py`): 31 fragments, 1 group, no islands. The ECO's cut pieces are stitched: the R37/Q14 F.Cu piece by 2 vias, the R39 piece by 1, and the B.Cu piece under the mute by 5. The return current is µA-level, line-level signal (0.7 V / 10k).
- **Coupling.** Sub nets are ≥ 7.6 mm from BUCK5_SW. MUTE_DET (an 18.6 kΩ Thévenin node) runs 3.7 mm from SW. Q12 is in saturation, so 0.15 V of pickup would be needed to matter.
- **JLC stock** (2026-10-30): C8545 3.4 M, C20526 2.5 M, C45783 3.8 M, C25808 0.8 M, C31850 2.9 M, C25819 3.1 M. All basic parts.
- **DAC → amp.** LOUT1 −4.5 dB (0.6 Vrms) × 26 dB gives 11.9 Vrms, against 13.4 Vrms clip at 20 V (round 2 R3-AUDIO-12). C60 1 µF with 30 kΩ puts the corner at 5.3 Hz.
- **FLX4 line in.** 2.1 Vrms through 15k/10k is 0.84 Vrms at LIN1, inside the 3.3 V rails. In DJ-direct the sub sees 0.42 Vpk, which is clean in every mute corner.
- **Speaker taps.** The DC step at amp on/off is ±0.9 V into LIN2, keeping the pin between 0.75 and 2.55 V, inside the rails.
- **Thermal at 9 W/ch.** Tj ~102 °C (6 Ω) / 109 °C (4 Ω) in the 62 °C box, at 22 °C/W (TI EVM). That leaves ~40 °C to the OTP.
- **Cold-start VMID ramp** (`sub_poweron.py`): 0.32–0.90 V at the RCA, 22–45 mV after a 20 Hz sub HPF. That passes against the 0.14 V bound.

## Field use (the person's flow)

- **USB-C charger.** Steady 20 V. A renegotiation 20→5→20 V keeps +5V ≥ 4.6 V, so the mute stays off. A PD hard reset is a power loss, which R4-ALS-3 covers.
- **Laptop bring-up (5 V).** Q12 holds the gate at 0.10 V at 4.6 V and 0 °C, so the sub outs work. At −20 °C the gate is 0.58 V, still below Vth.
- **6 Ω Mozarts.** Thermal passes. The governor assumes 4 Ω, which is conservative here.
- **FLX4 on the RCA in.** Levels pass. DJ-direct bypasses the governor, but the codec caps limit it to ≤ 7.3 W/4 Ω (4.9 W/6 Ω).
- **Powered sub on the RCA out, 10k–100k input.** The sub line passes; see R4-ALS-1/2 for the tail cases. Plugging the cable with a Class-II charger or sub: R4-ALS-4.
- **Unplugging mid-play.** Muted (24–50 mV) in every corner except the hot/Vth-max one in R4-ALS-3.

## Round-2 fixes checked

- **R3-AUDIO-2 mute: holds.** Round 2's benches re-run and pass (play cap 0.711 V with the mute gate at 0.075 V; power loss −25 mV). They do break at the corners in R4-ALS-1, -3 and -4.
- **SUB_CODE_MAX 26: holds only after the route is applied** (R4-ALS-2).
- **Thermal 9 W/ch** (thermal_out_w 18 W, tau 30 s): holds.
- **Amps held until the codec has settled** (codec_up + 2.5 s): holds.
- **Input saved to NVS:** holds.
- **GND vias rejoining the pour:** hold.

## Known unknowns
- The +5V level at which the ES8388 output collapses on power loss. This decides R4-ALS-3.
- The Vth distribution and tempco of CJ's 2N7002 (C8545).
- What a disabled ES8388 output pin does (DACPOWER 0x30 with the sub off). Assumed held to VREF by VROI, so no step.
