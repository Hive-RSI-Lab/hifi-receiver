# Review 2: audio and class-D (domain `audio`)

Reviewer: independent audio/class-D. Boards: `lite-main` (TPA3118D2 + ES8388) and `core` (ES9018K2M + OPA1678, PCM5102A, ES7243E, TS5A23157, TPA3221 zone A and Ultra zone B).
Sources: TI SLOS708G (TPA3118D2), TI SLASEE9C (TPA3221, rev May 2025), TI SBOS855E (OPA1678), TI SLAS859C (PCM5102A), TI SCDS165F (TS5A23157), ESS ES9018K2M datasheet v3.7 (2021-04-22), Everest ES8388 rev 5.0 (2018-07), Everest ES7243E rev 5.0, Pioneer DDJ-FLX4 spec (MASTER 2.1 Vrms rated into 10 kOhm, output impedance 1 kOhm or less). All fetched 2026-10-28/29.
Simulations: `docs/review-2/sims/audio/` (`gen_lc.py` writes 16 LC-filter testbenches, run with hwde `sim_run.py --dir`; `power.py` is the output-power model; `pcbq.py`/`netq.py` are the read-only PCB and netlist queries used for the layout and net checks).

## Findings, most severe first

| ID | Board | Sev | Finding | Fix |
|---|---|---|---|---|
| AUDIO-1 | core | major | The OPA1678 runs on one 5.1 V rail. At 0 dBFS its + input peaks at 3.05 V against a common-mode limit of V+ − 2 V = 3.10 V, a 48 mV margin. The margin goes negative below a 5.03 V supply. The output swings 0.92 to 4.18 V against limits of 0.8 and 4.3 V, a 0.12 V margin on each side. | Change R73, R75, R81 and R83 from 1.5k to 1.3k (gain 0.463). The CM peak drops to 2.75 V (0.35 V margin) and the output to ±1.41 V (0.34 V margin). 0 dBFS then gives 63 W into 4 Ω, 1.0 dB below clipping. |
| AUDIO-2 | core | major | Proposed loopback taps: the RCA side of the new TS5A23157 would sit at LDIV, which is at 0 V DC and swings ±0.92 Vpk with the FLX4. That is below the switch's 0..V+ range (abs min −0.5 V). The ADC path distorts, and the DJ-direct path is loaded through the same node. | Switch the AC-coupled, biased nodes instead. NC = LSW_x (already 2.55 V via R102/R112). NO = tap → 1 µF → node biased with 100k to VREF. COM → C101/C111 → AINxP. |
| AUDIO-3 | core | major | Proposed taps: the new switch's select would come from a 3.3 V ESP32 pin or the ES9018K2M GPIO. The TS5A23157 at V+ = 5.1 V needs VIH ≥ 0.7·V+ = 3.57 V. The ES9018K2M GPIO is also tri-stated while RESETB is low. | Copy the Q6 circuit: S8050 + 10k base + 10k pull-up to +5VA, plus 100k base pull-down so the default selects RCA. |
| AUDIO-4 | lite-main | major | SDZ is driven from GPIO21 (3.3 V) through R54 = 100k. SLOS708G 6.3 allows I_IH up to 50 µA. Above 13 µA the drop leaves SDZ below VIH = 2.0 V, so the amp stays in shutdown. | Change R54 to 22k (C31850). At 50 µA SDZ sees 2.2 V. Injection with PVDD off is 150 µA or less. |
| AUDIO-5 | core | minor | The ES7243E input coupling is 1 µF into 8 kΩ, a −3 dB point of 19.9 Hz (about 25 Hz with X5R DC bias). This is on the line-in to ADC to DSP path, which also feeds the subs. | Change C101, C102, C111 and C112 to 4.7 µF 0603 (C19666): 4.2 Hz. |
| AUDIO-6 | lite-main | minor | ES8388 full scale is 1 Vrms at 0 dB volume. The TPA3118 at 26 dB clips at 0.62 Vrms in (20 V, 4 Ω). At 0 dB volume, 0 dBFS overdrives by 4.1 dB. The analog bypass at 0 dB with the FLX4 overdrives by 2.1 dB. | Firmware: cap LOUT1/ROUT1 volume at −4.5 dB and LI2LOVOL at −3 dB (or drop to 20 dB gain: R50 = 5.6k, R51 DNP). |
| AUDIO-7 | lite-main | minor | Line-in headroom: FLX4 2.1 Vrms → 0.79–0.84 Vrms at LIN1 against 1.0 Vrms full scale, only 1.5–2.0 dB. The PGA cannot attenuate (minimum 0 dB). | Change R30 and R33 to 15k: 0.62 Vrms (−4.2 dBFS). |
| AUDIO-8 | core | minor | DJ-direct polarity: the TPA3221 inverts (SLASEE9C 9.2.1.2). The DAC path is pre-inverted in U11, but the DJ-direct path is not. In DJ mode the mains are inverted relative to the PCM5102A sub (fed through the ADC), so they cancel at the crossover. | Firmware: invert the sub/ADC channel polarity while DJ_DIRECT = 1. |
| AUDIO-9 | core | minor | DJ_DIRECT switching pop. MAIN DC = VREF + up to about 50 mV (ES9018 internal 806 Ω ±11 % plus 1 % resistors), against LSW at exactly VREF. The step is ×15.85, about 0.8 V at the speaker. | Firmware: RESET both amps low (20 ms ramp), switch, then release both in one TCA9555 port-0 write. |
| AUDIO-10 | core | minor | ES9018K2M MCLK is 50 MHz, which is the maximum with internal DVDD (ds v3.7 p.9, p.31 note 2): 0 % margin. | Fit a 49.152 MHz oscillator in the same 3225 footprint (192 kHz needs > 36.9 MHz in ASRC mode). |
| AUDIO-11 | core | minor | FREQ_ADJ = 2 × 100k 1 % = 50k ±1 %. The ROC window is 49.5–50.5k, so margin is 0 before TCR. | Accept, or use 49.9k 0.5 %. |
| AUDIO-12 | both | note | Open-load LC resonance (sim): +38 to +40 dB at 50 kHz (nominal) to 73 kHz (derated). The core 4.7 Ω + 1 nF snubber does not damp it (\|Z\| of 1 nF = 3.2 kΩ at 50 kHz). | Optional DNP Zobel footprint per leg. Bench-check "speakers off" captures for ringing. |
| AUDIO-13 | both | note | Loopback tap scale differs from remote.md. Lite: the 20 kΩ LIN2 load makes the ratio 0.0625, not 1/11. Core: the 8 kΩ ES7243E load makes it 0.0276, not 1/16. One leg also carries PVDD ripple/2 and common-mode products. | Firmware scales by the loaded ratio and calibrates once. Quote single-leg THD as an upper bound. |
| AUDIO-14 | core | note | DJ-direct maximum output with the FLX4 at rated 2.1 Vrms is 0.65 Vrms at the amp, about 27 W into 4 Ω (4.7 dB below clipping). The FLX4 is loaded by 7.0–7.3 kΩ, below its 10 kΩ rated load. | Design trade (the ADC shares the node). Document it. |
| AUDIO-15 | lite-main | note | AMP_IN_R crosses under BUCK3_LX (other layer, 0.21 mm offset). AMP_IN_L runs 1.25 mm from it. The source is the low-Z ES8388 LOUT, so the risk is low. | Route clear if any re-route happens. |
| AUDIO-16 | both | note | Continuous full power: 2 × 79 W (core, 28 V, 4 Ω) needs about 176 W in, against a 140 W PD contract. TPA3221 RθJA is 44.8 °C/W without a heatsink. | For the power and thermal reviewers. The firmware governor (INA186) must limit. |
| AUDIO-17 | lite-main | note | Pops. Power-down: +3V3A (codec) collapses before PVDD (690 µF bulk). Power-up: the amp must wait for ES8388 VMID (10 µF; τ = 0.5 s at 50k VMIDSEL). | Firmware: SDZ low on a VBUS_SENSE drop. Raise SDZ at least 2.5 s after VMID enable (or use the 5k fast-charge first). |

## Loopback taps (remote.md "The board measures its own output")

**Lite.** Tap is SPK_x_P → 100k → node (10k, 470 pF to AGND) → 1 µF → LIN2 (pin 22) / RIN2 (pin 21). The ES8388 input is 20 kΩ (rev 5.0 analog table).
- BD-mode leg: DC = PVDD/2 = 10 V, blocked by the 1 µF. AC = Vo/2 = ±8.8 Vpk at full 4 Ω power and ±10 V when hard-clipped (0..20 V).
- Loaded ratio = (10k‖20k)/(100k + 6.67k) = 0.0625. The ADC sees ±0.55 Vpk at full power and ±0.63 Vpk clipped, against a 1.41 Vpk full scale: −7.6 dBFS and −7.0 dBFS. No clipping.
- The pin moves at most ±0.63 V around the internal bias (about 1.65 V), so it stays within 0..AVDD. The power-up step of tap DC 0 → 0.91 V reaches the pin as +0.62 V, transient only. Pass.
- Carrier: BD mode puts common-mode 400 kHz on each leg. The 12.7 Vpk fundamental is attenuated by the leg LC at 1/(ω²LC − 1) = 1/62, giving 0.2 Vpk at the speaker. At the tap it is 0.2 × 847 Ω/100k = 1.7 mVpk (−58 dBFS) before the decimation filter. Pass.
- High-pass: 1 µF × (20k + 9.1k) = 5.5 Hz. Low-pass: 470 pF × 9.1k = 37 kHz.
- ES8388 Reg10 LINSEL (ADC input) and Reg38 LMIXSEL (output mixer) are separate registers. So the claim "the DJ bypass still works while the ADC listens to LIN2" is verified. The ADC cannot record line-in and the taps at the same time.
- Layout: LIN2/RIN2 (pins 21/22) sit next to LIN1/RIN1 (pins 23/24). Keep the tap traces away from the high-Z line-in traces.

**Core.** Tap is A_SPK_x+ → 150k → node (10k, 470 pF) → … → ES7243E AINxP (8 kΩ).
- Leg: DC 14 V; AC ±12.6 Vpk at full 4 Ω power, ±14 V clipped.
- Selected (loaded) ratio = (10k‖100k‖8k)/(150k + 4.26k) = 0.0276. The ADC sees ±0.39 V (−11 dBFS). Pass.
- Deselected (only the 100k bias load) ratio = 0.057, so ±0.80 V around the 2.55 V bias = 1.75..3.35 V. Inside the switch's 0..5.1 V range with 1.75 V margin.
- Carrier: AD mode at 600 kHz. Leg ripple ≈ 17.8 Vpk × −37 dB (derated LC) = 0.25 Vpk. At the tap: 0.25 × 564 Ω/150k = 0.94 mVpk. During clipping, the 150 kHz injected pulses give about 45 mVpk at the tap. Both are removed by the decimator. Pass.
- AUDIO-2 and AUDIO-3 above are what must change. Resistor values 150k/10k/470 pF stay as proposed.
- Additions per channel: 1 µF (C15849-class 0402/0603) from the tap node to the switch NO, and 100k from NO to VREF.
- The new switch's COM drives the existing C101/C111, which become 4.7 µF per AUDIO-5. NC connects to LSW_x.
- Ultra zone B uses the same circuit through the third TS5A23157 (cascaded into this switch's NO).
- TS5A23157 at 5.1 V: ron ≤ 10 Ω, flatness 4 Ω (SCDS165F 6.5). Against an 8 kΩ load that is 4/8000, about 0.05 % worst-case gain modulation. THD is negligible next to the ADC's −90 dB.

## Lite (lite-main)

**TPA3118D2 configuration** (SLOS708G pin table p.4 and 6.3–6.6; the netlist matches the generator):
- MODSEL = GND → BD mode.
- AM2..0 = 000 → 400 kHz (376–424).
- MUTE = GND → enabled. SYNC is NC (master output).
- PLIMIT = GVDD → no limit.
- GAIN: 20k to GND, 100k to GVDD → master, 26 dB (25–27), Zi = 30 kΩ ±20 %.
- GVDD 1 µF (C50 is X5R 50 V; TI asks for X7R: note).
- Pin map: chip R channel (pins 4/5 → 29/27) = speaker L. Consistent with the outputs.
- FAULTZ: 10k to 3.3 V, 0.33 mA sink. Pass.
- Input HPF: 1 µF × 30 kΩ = 5.3 Hz (6.6 Hz at Zi −20 %): −0.29 dB at 20 Hz. Single-ended drive with INN AC-grounded through 1 µF, per the TI single-ended scheme. DC across C60/C62 is 3 V − VMID ≈ 1.35 V, rated 50 V.
- Bootstraps 220 nF 25 V X7R (C21120). They see GVDD ≈ 6.9 V: 72 % margin.

**Output filter**, 10 µH XR0850 (30 mΩ, Irated 6.5 A, Isat 8 A) + 1 µF 1206 50 V X7R per leg. Sim (nominal / L −20 % with C −15 % at 10 V):

| Load | f0 | peak | at 20 kHz | −3 dB |
|---|---|---|---|---|
| 4 Ω | 50.7 / 61.5 kHz | −0.13 dB (none) | −0.55 / −0.33 dB | 44 / 55 kHz |
| 6 Ω | | +0.84 / +1.01 dB | +0.43 / +0.32 dB | 62 / 76 kHz |
| 8 Ω | | +2.65 / +2.86 dB | +0.85 / +0.58 dB | 69 / 85 kHz |
| open | | +38.2 / +38.8 dB | +1.49 / +0.99 dB | |

- Q into 8 Ω = 1.26. Carrier attenuation at 400 kHz: −36 / −33 dB.
- Inductor current at 4 Ω full power: 4.40 A peak + 0.78 A half-ripple (20 V, 400 kHz, L −20 %) = 5.2 A. Against Isat 8 A that is a 35 % margin. Irms is 3.1 A against 6.5 A.
- At the OCP trip of 7.5 A typ: Isat margin 6.7 %. The inductor stays ≥ 1 µH (SLOS708G 6.3 minimum under short circuit). Pass.
- Caps see at most 10 V DC + 8.8 Vpk = 18.8 V on a 50 V rating: 62 % margin.

**Output power** (`power.py`: SLOS708G eq. 2, RS = 120 mΩ + 30 mΩ DCR + 10 mΩ copper, 0.95 maximum modulation):

| Load at PVDD 20 V | 1 % THD | 10 % THD | Input at clipping |
|---|---|---|---|
| 4 Ω | 38.7 W | 48.4 W | 0.623 Vrms |
| 6 Ω | 27.1 W | 33.9 W | |
| 8 Ω | 20.9 W | 26.1 W | 0.647 Vrms |

These are electrical figures; 2-layer thermal limits continuous power (AUDIO-16). SLOS708G 6.6 lists 50 W at 10 % for 21 V 4 Ω, consistent. Headroom against DAC full scale: AUDIO-6.

**ES8388** (rev 5.0 pin table p.6; the netlist matches):
- DVDD and PVDD are on +3V3. AVDD and HPVDD are on +3V3A (LP5907). LP5907 Cout totals 5.9 µF, within its ≤ 10 µF limit.
- VREF, ADCVREF and VMID each have 10 µF + 100 nF.
- CE = GND → I2C 0x10.
- Line in: 10k/10k divider. Its shunt is 10k‖20k = 6.67k, giving a ratio of 0.40 (0.377 with the FLX4's 1 kΩ source). The FLX4 load is 16.7 kΩ against its ≥ 10 kΩ rating. Pass.
- Input HPF: 1 µF × 25 kΩ = 6.4 Hz. The 100 pF at the jack is only an RF shunt, with the source impedance.
- Sub outs (LOUT2/ROUT2): 10 µF X5R 10 V (C19702) at 1.65 V bias, 100 Ω, 47k bleed. HPF ≈ 2 Hz into 10 kΩ. Level 1 Vrms at full scale.

**Layout / grounding.**
- 2-layer board with GND pours on F.Cu and B.Cu.
- The class-D loop (J1 USB at (81, 65), Q1, C58/C59 at y = 108, U8 at (102, 105), L1–L4 at y = 87, J4 at (106, 66)) lies in x = 81–118 mm. The codec U7 at (53, 95) and line-in J2 at (42, 75) are at least 28 mm west of that loop.
- AMP_IN_L/R run 60/75 mm from U7 to C60/C62 and pass the 3.3 V buck (AUDIO-15). AMP_IN_L comes within 4.5 mm of OUT_L_P. Both are driven from the low-Z codec output.
- Ground continuity under that route was not simulated (see tools wanted).

## Core (Plus/Ultra)

**TPA3221 zone A / zone B** (SLASEE9C 5-1, 6.3, 6.5, 8.3; the netlist matches):
- VDD = +5V direct, LDO bypassed (4.5–5.5 V). 5.1 V gives 7 % to the maximum.
- AVDD and GVDD each come through 4.7 Ω (TI uses 3R3). GVDD drops 16 mA × 4.7 Ω = 75 mV → 5.03 V, 0.53 V above the 4.5 V minimum. AVDD drops 47 mV.
- HEAD = GND → AD mode. CMUTE = 33 nF (20 ms ramps).
- Gain A: 20k/100k → controller 24 dB. Gain B: 75k/47k → peripheral 24 dB. RIN = 24 kΩ ±20 %.
- FREQ_ADJ: A = 50k (AUDIO-11); B = AVDD (peripheral, oscillator off).
- OSCM/OSCP: A to B with the same polarity → peripheral 1. 47k pull-up on OSCM and 47k pull-down on OSCP, as in SLASEE9C 8.3.2.
- The RESETs are separate TCA9555 pins on port 0. Write them in one I2C byte so they release together for sync.
- FAULT and OTW are wired-OR with 10k to 3.3 V. The internal 26k is to AVDD with VOH 3.3 V. Pins are rated 5.5 V. Pass.
- PVDD 28 V: 12.5 % below the 32 V ROC maximum and 24 % below the 37 V abs max.
- Bootstraps 33 nF 50 V X7R, as TI specifies.
- Inputs: 10 µF/25 V X5R × 24 kΩ = 0.66 Hz.
- Inverting gain: see AUDIO-8.

**Output filter**: PSPMAA0805-100M (10 µH, 30 mΩ, Isat 10 A, Irated 6 A) + 1 µF 0805 50 V X7R per leg. The snubber (4.7 Ω + 1 nF) is at the speaker node. Sim (nominal / L −20 % with C −40 % at 14 V DC):

| Load | f0 | peak | at 20 kHz | −3 dB |
|---|---|---|---|---|
| 4 Ω | 50.7 / 73.1 kHz | −0.13 dB | −0.55 / −0.56 dB | 44 / 52 kHz |
| 6 Ω | | +0.85 / +0.21 dB | | |
| 8 Ω | | +2.65 / +1.71 dB | +0.85 / +0.31 dB | |
| open | | +38.5 / +40.3 dB | | |

- Carrier at 600 kHz: −43 / −37 dB.
- Inductor current at 4 Ω full power: 6.30 A + 0.73 A half-ripple = 7.0 A against Isat 10 A, a 30 % margin. Irms is 4.5 A against 6 A.
- At IOC = 10 A typ the inductor is at Isat. If Isat means L −30 %, L stays ≥ 7 µH against the 5 µH minimum at IOC (SLASEE9C 6.3): 40 % margin. The Isat definition is unverified.
- Caps see at most 28 V on a 50 V rating: 44 %.
- Snubber dissipation at the filtered node is < 1 mW (0.25 Vpk at 600 kHz into 1 nF). If it were at OUT before the inductor it would be C·V²·f = 0.47 W. It is not.

**Output power** at PVDD 28 V (model 3.6 % above the datasheet at 30 V: 91.2 W against 88 W):

| Load | 1 % THD | 10 % THD | Input at clipping |
|---|---|---|---|
| 4 Ω | 79.5 W | 99.3 W | 1.125 Vrms |
| 6 Ω | 54.9 W | 68.6 W | |
| 8 Ω | 41.9 W | 52.4 W | 1.155 Vrms |
| 4 Ω at 20 V (PD fallback) | 40.5 W | | |

- DAC full scale at MAIN is 1.153 Vrms, so headroom is −0.2 dB at 4 Ω and 0 dB at 8 Ω: 0 dBFS just reaches clipping.
- After the AUDIO-1 fix, 0 dBFS gives 1.00 Vrms at MAIN: 63 W, +1.0 dB headroom.
- At the 20 V PD fallback, firmware must cap the volume at −2.9 dB.

**ES9018K2M** (ds v3.7 pin table p.4, p.9, p.28–31):
- The PCB footprint is QFN-28 5 × 5 mm, 0.5 mm pitch, EP 3.25. This matches ESS p.36–37. The generator comment saying "4x4 0.4" is stale.
- ADDR = GND → 0x90 (7-bit 0x48).
- XO is NC. MCLK is 50 MHz through 33 Ω (AUDIO-10).
- AVCC_L, AVCC_R and VCCA are on +3V3A. DVCC is on +3V3.
- DVDD has 4.7 µF 16 V X5R. The minimum is 1 µF at 1.2 V. Pass.
- RESETB = DAC_EN (100k pull-down, VIH = DVCC/2 + 0.4 = 2.05 V against 3.3 V). Pass.
- GPIO1 = OPT_SPDIF with 10k pull-down. DATA2/DATA1/DATA_CLK = DOUT_MAIN/LRCK/BCK.
- Voltage mode: each leg is 1.65 V DC ± 1.525 Vpk through 806 Ω (±11 %).
- AVCC is shared with the PCM5102A charge-pump CPVDD. Voltage-mode full scale is 0.924·AVCC, so AVCC noise appears at the output (note).

**OPA1678 stage**: see AUDIO-1.
- Gain = 1.5k/(2k + 806) = 0.535. Output = 1.153 Vrms at 0 dBFS.
- DC out = VREF = 2.55 V (470/470 from +5VA, 10 µF).
- Inverting connection (DAC P leg to the − input) cancels the TPA3221 inversion. Pass.
- 1 nF across 1.5k gives a pole at 106 kHz.
- Load: 3.3k feedback ‖ 24k amp (12k on Ultra) ≈ 2.6k. Pass.

**PCM5102A** (SLAS859C pin table, 6.3, Fig. 33):
- FLT = L (normal filter), DEMP = L, FMT = L (I2S), SCK = GND (BCK PLL, 3-wire).
- XSMT = DAC_EN: muted at reset. Pass.
- CAPP–CAPM 2.2 µF, VNEG 2.2 µF, LDOO 2.2 µF.
- AVDD and CPVDD on +3V3A (min 3.0/3.1 V). DVDD on +3V3.
- Output: 2.1 Vrms ground-centred through 470 Ω + 1 nF (339 kHz; TI uses 2.2 nF). The 1 kΩ minimum load is met by sub-amp inputs of 10 kΩ or more.
- DIN is on ESP32 IO12 (MTDI strap): it must not be pulled high at boot (MCU reviewer).

**ES7243E** (rev 5.0):
- AD0 = AD1 = 0. AD2 is held at 0 by 100k on SDOUT → address 0x10. No clash with 0x20, 0x22, 0x27, 0x48.
- MCLK = GND, so the internal clock must be taken from SCLK (register 0x01 MCLK_SEL, firmware).
- Full scale 1 Vrms (VDDA/3.3). Input 8 kΩ at 0 dB PGA.
- Line level at the ADC with the FLX4: 0.653 Vrms Plus / 0.61 Vrms Ultra = −3.7 / −4.3 dBFS. Pass, with 3.7 dB headroom.
- HPF: AUDIO-5.

**TS5A23157 U14 + Q6**:
- V+ = 5.1 V (≤ 5.5 V).
- Select: Q6 pulls DJ_SEL to 5.1 V (off) or about 0.1 V. VIH 3.57 V gives a 1.5 V margin; VIL 1.53 V gives a 1.4 V margin. Default DJ_DIRECT low → IN high → NO = MAIN (DAC path), as intended (SCDS165F Table 1).
- Signal ranges: MAIN 0.92–4.18 V; LSW 2.55 ± 0.92 V = 1.63–3.47 V. Both inside 0..5.1 V, with 0.92 V and 1.63 V margin.
- ron flatness 4 Ω into 24 kΩ: under 0.02 % gain modulation.

**Layout / grounding**:
- 4-layer board with a solid In1.Cu GND zone.
- PVDD/class-D loop: J1 (38.6, 178.7) → R1, Q1 (56.6, 172.7) → C5/C6 (62.7, 140–151) → U15 (46, 162) / U16 (79, 162) → J6–J11 (49–94, 126). All of it is at x < 95 mm.
- DAC, op-amp, ADC, switch and RCA (U10–U14, J4/J5) are at x = 106–135 mm. Speaker current does not run under the converters.
- AMP_IN_L/R run x = 78–108 along y = 162–179 to the amp input caps.

## Tools wanted
- A return-path current-density solver on the real 2-layer Lite copper: does PVDD return between C58/C59, U8 and J1 cross under U7/LIN traces? Placement says no, but this was not simulated.
- A class-D behavioural model (TPA3118/TPA3221 PWM with LC and load) in ngspice: carrier residue at the taps and clipping recovery (pulse injection at fs/4), rather than hand estimates.
- MLCC DC-bias curves (Samsung CL21B105KBFNNNE at 14 V, CL31B105KBHNNNE at 10 V). The −40 % / −15 % used here are assumptions.
