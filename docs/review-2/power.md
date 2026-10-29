# Power review (review-2, domain `power`): Lite (lite-main) and core (Plus/Ultra)

Independent power-electronics pass, 2026-10-28/29. Read-only on `boards/`.
Facts come from the netlists (`boards/lite-main/kicad/lite-main.net`, `boards/core/kicad/core.net`,
parsed directly), the PCB files (copper parsed directly), and vendor datasheets fetched to
`/tmp/ds/power/<part>/`. Simulations are in `docs/review-2/sims/power/`. Re-run one with
`eval "$(sh $HIVE_MODULES/hive-hwde/bin/hwde env)"; $HWDE_PY docs/review-2/sims/power/run.py <tb>.cir`.
Models are in `models.inc`. The AO4485 is a VDMOS fit to AOS fig. 1/2/8: 6 A at Vgs -2.5 V,
19.5 A at -3 V, 99 A at -4.5 V (Vds -5 V), which matches the datasheet curves.

## Findings

| id | board | sev | finding |
|---|---|---|---|
| POWER-1 | core | **blocker** | The VBUS/PVDD power path is routed in 0.25 mm traces and 2-3 vias at every layer change. J1's VBUS pads reach R1/D1 only through 0.255 mm F.Cu tracks, and R1 pad 1 has one via. Q1's source has 3 vias and its drain 2. At 5 A (140 W EPR) that is about 2.5 A per 10 mil trace and per via |
| POWER-2 | core | major | Turn-on SOA of Q1 (AO4485) is exceeded: the firmware turn-on charges 940 uF in about 12 ms. Simulated peak is 46.7 W and the FET absorbs 0.364 J, where the single-pulse rating at 10 ms is about 20 W, i.e. about 0.2 J |
| POWER-3 | lite-main | major | Miller cap C2 (gate-drain) turns Q1 ON at every VBUS rising edge with the firmware off. PVDD charges to 2.8 V on attach and to 10-15.6 V on the 5->20 V PD step. Simulated inrush is 20.6 A at the PD maximum slew (30 V/ms) and 2.0 A at 3 V/ms |
| POWER-4 | lite-main | major | Turn-on SOA of Q1 with the firmware on is marginal: 43 W peak and 0.134 J in about 5 ms (rating about 25 W at 5 ms). Inrush is 2.4 A from a 3 A contract |
| POWER-5 | core | major | TCA9555 internal ~100 k pull-ups fight the 100 k pull-downs R40-R45 (plus R7 100 k at Q2). From power-up until firmware writes the expander, PVDD_ENG sits at 1.1-1.6 V. Q2 2N7002 has Vth 1.0-2.5 V, so a low-Vth part turns PVDD on at boot. AMP_x_RST, DAC_EN, PHY_RST and DJ_DIRECT float at 1.65 V the same way |
| POWER-6 | core | major | Nothing in hardware drops PVDD when VBUS collapses (PD hard reset, renegotiation, unplug-replug), and the core has no VBUS ADC sense. If Q1 is still on when the source returns at 5 V and steps to 28 V, the source charges 940 uF at its own slew. The step bench shows 940 uF tracking the source with Q1 conducting, which means amps of current and a likely OCP/hard-reset loop. CH224Q PG is left unconnected |
| POWER-7 | core | minor | The C8 hold-off works but with thin margin: on a 5->28 V step at 30 V/ms Vsg peaks at 2.17 V, the C4/(C4+C8) = 9 % coupling. That is above Vth min (1.7 V), and a low-Vth FET passes 3.8 A, charging PVDD to 5 V |
| POWER-8 | core | minor | The OV trip (D2 BZT52C33 + Q3) is slow and close: it trips at Vz(60 uA)+1.2 V, about 31-36 V, against 29.4 V EPR max (about 6 % margin at the low corner). Q1 turn-off then takes R4*(C8+C4) = 110 ms, so it cannot protect the TPA3221 (37 V abs) from anything fast |
| POWER-9 | both | minor | The TVS clamp sits above downstream abs-max. Lite SMBJ20A clamps at 32.4 V (TPS54302/TPA3118 30 V, CH224A 32 V). Core SMBJ30A clamps at 48.4 V (TPA3221 37 V, CH224Q 32 V, SY8303 40 V, INA186 42 V). Standoff margins are fine. Real surge energy is small because PD attaches at 5 V |
| POWER-10 | lite-main | minor | TPS54302 loop: the 2 x 22 uF 25 V 0805 X5R output caps are about 22 uF effective at 5.2 V, so fo = 5.1/(Vout*Co) = 45 kHz, above TI's < 40 kHz rule. The 100 pF feed-forward on 100 k (zero at 15.9 kHz) pushes the crossover further up |
| POWER-11 | core | minor | SY8303 C10 (10 uF/50 V X5R 1206) is about 3-4 uF at 28 V, inside the >= 4.7 uF ceramic requirement only nominally. Output 2 x 22 uF 0805 is about 22 uF effective against the ">22 uF" recommendation |
| POWER-12 | lite-main | minor | The Lite VBUS path from J1 B4-A9 crosses one 0.3 mm via (78.97,69.22) carrying about 1.4 A at a 3.5 A load, which is at the 1-1.5 A guideline |
| POWER-13 | core | minor | On a 5 V-only port the SY8303 (min off-time 120 ns at 1.05 MHz, Dmax about 87 %) gives +5V of about 4.35-4.6 V. That is below the TPA3221 VDD minimum of 4.5 V and low for the front board. It is harmless because PVDD is off, but firmware must not enable the amps |
| POWER-14 | both | note | J1: the footprint `USB-C-SMD_MC-311D` does not match the value USB4105-GF-A-120. The lead's swap is checked below: HRO TYPE-C-31-M-12 (Lite) and GCT USB4085-GF-A (core) |
| POWER-15 | core | note | The proposed speaker loopback dividers (150 k/10 k from A_SPK_L+/R+ to AGND) are harmless to the power stage: 87 uA DC and 4.6 mW peak per divider |

## Block by block

### 1. USB-C entry and PD sink

**Lite, CH224A (U1, ESSOP-10).** Pins were checked against WCH CH224 datasheet V2.0, table 4-1:
VHV 1, CFG2/SCL 2, CFG3/SDA 3, DP 4, DM 5, CC2 6, CC1 7, VBUS 8, CFG1 9, PG 10, EPAD GND.
The netlist has pin 1 VHV and pin 8 VBUS both on VBUS, with C1 1 uF. That matches reference
schematic 6.1.1 ("VBUS needs to be shorted to VHV"). CC1/CC2 are swapped against the receptacle,
which is allowed because the orientation is symmetric. Rset R1 = 120 k gives 20 V (table 5-1).
CFG1 abs-max is 3.8 V and it only sees the resistor. PG (open drain, 32 V tolerant) is pulled to
+3V3 by R4 10 k.

- VHV margin: 20 V +5 % = 21 V against 32 V abs-max, 34 % margin. OVR trips at 32-34 V.
- Section 7.1 of the same datasheet says "VBUS of CH224A has 3.8 V tolerant voltage". Table 8.1.1
  says 32 V (VIOHV on PG and VBUS). Table 4-1 and the reference schematic short VBUS to VHV, so the
  3.8 V figure is read as a translation error. It is recorded as a known unknown.

**Core, CH224Q (U1, DFN-10).** Same pin map. VHV and VBUS (pins 1 and 8) are on VBUS_PD, fed
through R17 10 R. ICC is at most 12 mA, so the drop is at most 0.12 V. C1 1 uF/50 V. R2 = 210 k
gives 28 V (table 5-1). PG pin 10 is unconnected; see POWER-6.

- VHV margin: EPR 28 V +5 % = 29.4 V against 32 V abs-max, 8 % margin. OVR is 32 V min.

**Charger cases** (CH224 fallback behaviour is not documented: known unknown):

| case | Lite (asks 20 V) | core (asks 28 V) |
|---|---|---|
| 5 V-only port / PC | PG presumably never asserts, so Q4 cannot pull the gate and PVDD stays off. Logic runs: TPS54302 in dropout gives +5V about 4.7 V; LP5907 and SY8089 are fine | +5V is about 4.35-4.6 V (POWER-13). PVDD only comes on if firmware sets it, and the TPA3221 sits in UVP below 6.4 V |
| 9/15 V-only or 20 V-less | Unknown: the CH224A may take a lower PDO or stay at 5 V. TPA3118 works from 4.5 V | Unknown. TPA3221 works from 7 V. With R4/R5 = 1:1, Vsg = VBUS/2 is 4.5 V at 9 V, which is enough (RDS(on) < 20 mOhm at 4.5 V) |
| 100 W charger (no EPR) | 20 V/5 A | No EPR; expected fallback to 20 V. Vsg = 10 V, OK |
| 140 W + EPR cable | 20 V | 28 V/5 A. The source will not enter EPR without a 5 A EPR e-marked cable, so a non-EPR cable is safe and falls back to SPR |
| hard reset / unplug under load | Bulk back-feeds VBUS through Q1's body diode (Is 3 A max). CH224A loses VHV, PG releases, Q4 opens, Q1 turns off | Q1 stays on as long as +3V3 and the TCA9555 survive (POWER-6) |

**cSnkBulk at attach.** Lite VBUS ceramics are 15.8 uF nominal, about 13 uF at 5 V. Core VBUS
ceramics are C10 10 uF + C8 1 uF + C1 1 uF, about 12 uF. Both are above the Type-C 10 uF
cSnkBulk line but inside PD's cSnkBulkPd of 100 uF. The real problem is the Lite FET conducting
on attach, which hangs PVDD's 690 uF on VBUS (POWER-3): the simulated attach peak is 6.1 A at a
10 V/ms 5 V turn-on.

**Connectors (lead's swap).**
- Lite: HRO TYPE-C-31-M-12 (C165948), rated 20 V / 5 A per LCSC attributes; I could not fetch
  the datasheet (LCSC PDFs are stubbed through the proxy). Load: 2 x 30 W / 0.9 + 2 W, about
  3.5 A peak at 20 V, so 30 % current margin. Voltage: PD 20 V +5 % = 21 V against the 20 V
  rating. That is the normal SPR class, but it is formally -5 %.
- Core: GCT USB4085-GF-A (C7095263), THT, 48 V / 5 A per LCSC. At 28 V / 5 A the voltage margin
  is 42 %, but the current margin is 0 % at the full 140 W contract. With one TPA3221 (Plus) the
  real draw is lower.
- Both connectors are 16-pin USB 2.0 parts. Wire all four VBUS contacts (A4/A9/B4/B9) and all
  four GND contacts. CC1 (A5) and CC2 (B5) go directly to CH224 pins 7/6 (either way round) with
  no 5.1 k Rd: the CH224Q/A reference schematic 6.1 has none. Leave SBU unconnected.
- On the core, use the THT VBUS pins to drop straight into a VBUS_IN pour on every layer. This
  is part of the POWER-1 fix.

### 2. TVS against every downstream abs-max

| board | TVS | VR / VBR min / Vc@Ipp (MDD datasheet table) | worst operating V | standoff margin | downstream abs-max |
|---|---|---|---|---|---|
| Lite | D1 SMBJ20A | 20 / 22.2 / 32.4 V @ 18.6 A | 21 V | VBR 5.7 % above | TPS54302 30 V, TPA3118 30 V (behind Q1), CH224A 32 V, caps 35-50 V |
| core | D1 SMBJ30A | 30 / 33.3 / 48.4 V @ 12.4 A | 29.4 V | VBR 13 % above | TPA3221 37 V (behind Q1 + 940 uF), CH224Q 32 V (behind 10 R + 1 uF), SY8303 40 V, INA186 42 V, caps 50 V |

On both boards the clamp exceeds some abs-max at rated Ipp. The threat that remains is small:
PD always attaches at vSafe5V, and ESD charge (8 kV x 150 pF = 1.2 uC) moves 13 uF of VBUS
ceramics by only 0.09 V. A sustained charger overvoltage cannot be stopped by any TVS. This is
POWER-9, minor. The only cheap improvement would be on the core: a 28 V-standoff part such as
SMBJ28A (VBR 31.1 V min, Vc 45.4 V). That puts VBR 5.8 % over 29.4 V and clamps marginally lower.
The extra margin is small, so I would not change the part.

### 3. PVDD high-side switches

**Lite (Q1 AO4485 UMW clone C5125949; driver Q4 MMBT3904 with its emitter on PD_PG).**
The gate divider R5 100 k / R6 47 k gives Vsg = 20 x 100/147 = 13.6 V (sim: 13.57 V). Against
VGS +-20 V that is a 32 % margin. At the 32.4 V TVS clamp it would be 22 V, over the limit
(transient only). PG sinks 0.73 mA when on (simulated). The CH224A PG sink rating is not
specified (known unknown).

- Firmware off (POWER-3). Benches `tb_lite_pd_step_fw_off_sr{30,3,0.5}.cir`:

| 5->20 V slew | PVDD after step | peak source current | Q1 peak power |
|---|---|---|---|
| 30 V/ms (PD max) | 15.6 V | 20.6 A | 109 W |
| 3 V/ms | 14.8 V | 2.0 A | 10.6 W |
| 0.5 V/ms | 10.5 V | 0.30 A | 2.0 W |
| attach 0->5 V in 0.5 ms | 2.8 V | 6.1 A | - |

  Mechanism: C2 (100 nF, gate to drain) holds the gate at PVDD while the source rises. Cgs is only
  about 2.5 nF, so Vsg = dVBUS and Q1 runs as a follower about Vth below VBUS until R5 recharges
  C2 (tau 10 ms). The AND with PG does not help because Q4 is not in this path. The amp stays in
  SDZ, so nothing is damaged. But the charger sees up to 20 A during its transition, so a
  fast-slewing charger trips OCP and hard-resets. That happens only on some chargers, which is why
  this is major.
- Firmware on, 20 V (POWER-4). Bench `tb_lite_fw_on_20v.cir`: 10-90 % in 4.6 ms, 2.40 A FET,
  2.49 A source, 43 W peak, 0.134 J. AOS fig. 9/10 single-pulse is about 50 W at 1 ms and about
  20 W at 10 ms, so the energy limit is about 0.12-0.15 J at TA = 25 C: roughly no margin. The
  source sees 2.5 A plus about 0.1 A of buck load against a 3 A contract, a 13 % margin.
- **Fix (simulated, `lite_pvdd_fix.inc`):** change R6 47 k to 220 k (0603) and add a new
  2.2 uF/25 V 0603 X5R from VBUS to PSW_G (gate-source hold-off, as on the core). Keep C2 100 nF.
  Results:
  - Step at 30 V/ms with a low-Vth FET (1.7 V): Vsg peak 0.77 V, PVDD 3 mV, source 0.59 A (the
    VBUS ceramics only) (`tb_lite_pd_step_fw_off_sr30_FIX`).
  - Firmware on: 0.41 A, 5.8 W peak, 10-90 % 28 ms, 0.138 J spread over 28 ms, against about
    20 W at 30 ms, i.e. 3.5x margin (`tb_lite_fw_on_20v_FIX`).
  - Final Vsg 5.5 V: RDS(on) <= 20 mOhm at 4.5 V, so 3.5 A gives 0.25 W.
  - At the 32 V clamp Vsg becomes 10 V, which also clears the VGS transient.
  - If more gate drive is wanted, R6 = 150 k gives Vsg 8 V and about 0.9 V/ms (0.6 A); this was
    not simulated.

**Core (Q1 AO4485 AOS C51499, R4/R5 100 k/100 k, C8 1 uF G-S, C4 100 nF G-D, D4 15 V clamp, Q2 2N7002).**
Vsg = 14 V at 28 V (sim 13.9 V); D4 BZT52C15 (13.8-15.6 V) clamps it. VGS margin is 30 %. D4 also
forward-clamps gate-above-source on a fast VBUS collapse.

- Firmware off, 5->28 V step (POWER-7). Benches `tb_core_pd_step_fw_off_*`. Attach at 5 V:
  PVDD stays at 0.5 mV, so the hold-off works.

| slew | Vth | Vsg peak | FET peak | PVDD after |
|---|---|---|---|---|
| 30 V/ms | 1.9 V typ | 2.17 V | 1.69 A | 2.9 V |
| 30 V/ms | 1.7 V min | 2.13 V | 3.80 A | 5.1 V |
| 30 V/ms | 2.5 V max | 2.20 V | 2 mA | 0 V |
| 3 V/ms | typ | 2.08 V | 0.86 A | 2.3 V |
| 0.5 V/ms | typ | 1.78 V | 0.03 A | 0.2 V |

- Firmware on, 28 V (POWER-2). Bench `tb_core_fw_on_28v.cir`: 10-90 % in 10 ms (2.24 V/ms),
  2.16 A FET, 2.30 A source, 46.7 W peak, 0.364 J. Against about 20 W at 10 ms (about 0.2 J),
  that is about 1.8x over the single-pulse SOA at 25 C ambient. Estimated junction rise is about
  30 W average x Zth(10 ms, single pulse about 0.1 x 75 C/W) = about 200 C. It may survive once on
  a cold board, but it is outside the rating on every power-up.
- **Fix (simulated, `core_pvdd_fix.inc`):** change R4 and R5 100 k to 470 k (0402) and C8 1 uF to
  2.2 uF/50 V 0805. C4 stays 100 nF. Results:
  - Firmware on: 0.47 A FET, 0.61 A source, 9.8 W peak, 10-90 % in 46 ms (about 60 ms total),
    0.37 J over 60 ms, against about 15 W at 50-100 ms: 2.5x margin (`tb_core_fw_on_28v_FIX`).
  - Step at 30 V/ms with Vth 1.7 V: Vsg 1.19 V, FET 70 uA, PVDD 7 mV.
  - Costs: turn-on delay about 60 ms; turn-off tau = 470 k x 2.3 uF = 1.1 s. Firmware must shut
    the amps (RESET low) before dropping PVDD_EN, so Q1 never turns off under audio load in its
    linear region.
- OV trip (POWER-8). BZT52C33 is 31-35 V at 2 mA (BORN datasheet, table row BZT52C33) with
    leakage 0.1 uA at 23.1 V. At about 60 uA in R9 the knee sits about 1 V lower, so the trip is
    about 31-35 V. That leaves about 2 V (6 %) above 29.4 V at the low corner and about 1 V below
    TPA3221 abs-max (37 V) at the high corner. D2 polarity was checked: footprint pad 1 has the
    silk bar (cathode) and is on VBUS. The trip only releases Q2, and the gate then rises with
    tau 110 ms (1.1 s after the fix), so it is a sustained-fault cut-off, not surge protection.
    Severity minor; this belongs in the documentation, not a change.
- POWER-5 (TCA9555). TI SCPS200E says "identical to TCA9535 except for the inclusion of the
  internal I/O pull-up"; IIL is -100 uA max at VI = GND (VCC up to 5.5 V), so Rpu >= 55 k.
  PVDD_EN sees Rpu against R43 100 k || R7 100 k (through R6 1 k), which puts PVDD_ENG at
  3.3 x 50/(Rpu + 50) = 1.1 V (Rpu 100 k) to 1.57 V (Rpu 55 k). JSCJ 2N7002 Vth is 1.0/1.6/2.5 V
  at 250 uA, and Q1 needs only 0.14 mA through R5 to turn on. A low-Vth part or a strong pull-up
  therefore switches PVDD on before firmware runs: an unintended inrush, then a 5->28 V transition
  with the switch closed.
  **Fix:** change R40-R45 100 k to 10 k (C25744, already on the BOM). PVDD_ENG then stays at or
  below 3.3 x 9.1/(55 + 9.1) = 0.47 V, a 0.53 V margin to Vth min. When driven high it draws
  0.33 mA, which the TCA9555 drives easily. The alternative is TCA9535PWR (no pull-ups; confirm
  the LCSC number).
- POWER-6. **Fix:** move Q2's source from GND to a new net PD_PG = CH224Q pin 10 (PG, open-drain,
  low = contract good), with 10 k to +3V3. This is the Lite AND function. Q2 then conducts only
  when firmware AND the PD contract agree; Q2's gate-source current is about 0.14 mA, inside an
  open-drain sink. Optionally route PD_PG to a free TCA9555 input. As a contingency, add a 0 R
  from PD_PG to GND, DNP, to restore today's behaviour if PG timing turns out wrong.

### 4. Buck converters

| | Lite U2 TPS54302 (VBUS->+5V) | core U4 SY8303 (VBUS->+5V) | Lite U3 / core U5 SY8089 (+5V->+3V3) |
|---|---|---|---|
| Vin max / abs | 21 V / 30 V: 30 % margin (rec 28 V) | 29.4 V / 40 V: 27 % | 5.31 V max / 5.5 rec / 6.0 abs: 3.5 % / 11 % |
| FB -> Vout | 0.596 x (1 + 100 k/13 k) = 5.18 V (5.05-5.31) | 0.6 x (1 + 15 k/2 k) = 5.10 V (+-1.5 % +-1 %) | 0.6 x (1 + 150/33) = 3.33 V (Lite); 0.6 x (1 + 100/22) = 3.33 V (core) |
| fsw | 400 kHz (290-510) | 105/100 k = 1.05 MHz (+-15 %) | 1.5 MHz |
| min on-time | ton = 0.247/510 k = 484 ns against 110 ns: 4.4x | 0.173/1.21 MHz = 143 ns against 80 ns: 1.8x | n/a |
| L, Isat / Irms | 10 uH SWPA6045S100MT, 3.2 / 2.45 A | 4.7 uH SWPA4030S4R7MT, 2.9 (guaranteed) / 2.0 A | 2.2 uH SWPA4020S2R2NT 3.4/1.85 A (Lite); SWPA4030S2R2 4.9/2.95 A (core) |
| ripple / peak | 0.96 A pp; peak at 0.6 A load 1.08 A: 66 % below Isat | 0.85 A pp at 29.4 V; peak at 1 A load 1.43 A: 51 % | 0.36 A pp; peak at 0.8 A 0.98 A: 71 % |
| Cin | 10 uF + 4.7 uF/50 V (about 11 uF at 20 V) | 10 uF/50 V about 3-4 uF at 28 V (POWER-11) | 10 uF/10 V 0603 at 5.2 V about 5 uF |
| Cout | 2 x 22 uF/25 V 0805, about 22 uF eff (POWER-10) | 2 x 22 uF/25 V 0805, about 22 uF eff | 22 + 10 uF (Lite); 2 x 22 + 22 uF (core) |
| comp | internal + 100 pF FF: see POWER-10 | internal, no FF: within the ">22 uF" guidance | internal; 22 pF FF (Lite) per table 1 |
| EN / UVLO | EN floats = on (internal 0.7 uA pull-up); UVLO 3.8-4.4 V rising | EN = VBUS/2 (R10/R11), ON above about 3 V, 14 V at 28 V against 40 V abs; UVLO 4.35 V | EN = +5V |
| current limit vs Isat | HS limit 4-6 A > Isat 3.2 A: saturates on a hard short (minor) | 4-6.8 A > 2.9 A: same | 3.5 A min ~ Isat |

- POWER-10 fix (Lite): add a third 22 uF/25 V 0805 (C45783) on +5V, or change C7/C8 to 1206
  25 V parts, and change C9 100 pF to 47-68 pF. Contingency: C9 is a removable 0603. Without FF the
  loop crosses at about 45 kHz with ceramic Cout, which is acceptable. Check on the bench with a
  0.3->1 A load step.
- POWER-11 fix (core): add a second 10 uF/50 V 1206 (C13585) at U4 IN, and a third 22 uF on +5V.

### 5. LDO

LP5907-3.3 (both boards; SNVS798Q). The input is +5V at 5.10-5.31 V against 5.5 V rec / 6.0 V
abs, a 3.5 % margin on the Lite at the high corner. Dropout is 250 mV max at 250 mA; the load is
under 50 mA, so headroom is about 1.8 V. Cout must be 0.7-10 uF. Lite +3V3A has 1 + 4.7 +
0.2 uF = 5.9 uF nominal, about 4.5 uF effective. Core +3V3A has 6 x 1 uF + 0.3 uF = 6.3 uF
nominal (0402 at 3.3 V, about 4 uF). Both are inside the range. Noise is 6.5 uVrms typ. The pin
map is IN 1, GND 2, EN 3, NC 4, OUT 5 on both boards, which is correct.

### 6. Sequencing, brown-out, back-feed

- ESP32 EN: 10 k / 1 uF on both boards, tau 10 ms after +3V3. The SY8089 turn-on delay is
  350 us after EN, so EN rises well after the rail. This meets Espressif's EN-after-supply
  requirement.
- Wi-Fi TX bursts (about 0.5 A peak at 3.3 V): the SY8089 has 2 A capability, a 3.5 A limit, and
  32-66 uF on +3V3. That is 0.75 A at 3.3 V, about 0.55 A at 5 V, well under the 3 A buck. Pass.
- Amp supply collapse when the charger drops: the bulk back-feeds VBUS through Q1's body diode,
  and both bucks ride down to their UVLO (TPS54302 3.3-3.9 V falling; SY8303 about 4.15 V). The
  TPA3118 PVCC UVLO (4.5 V) and the TPA3221 UVP (6.4 V) turn the amps off before the logic dies.
  Pass.
- Back-feed:
  - CH340C runs from +3V3 on both boards and its D+/D- go only to the host, so there is no
    5 V path.
  - Core USBLC6-2SC6 VBUS pin is tied to +5V, not 28 V. Correct.
  - CH224 CFG2/CFG3 have internal 7-15 k pull-ups. With the I2C 2.2 k pull-ups to +3V3 they leak
    about 0.3 mA per pin into +3V3 while 3V3 is still off, which is harmless. The CH224 samples
    CFG2/3 while +3V3 is still 0 V (low). Single-resistor mode should be set by the CFG1
    resistor, but this is unverified (known unknown).
  - TPA3221 VDD/AVDD/GVDD come from +5V while PVDD is off. TI 8.4.1 says sequencing is not
    required.
- Codec (Lite ES8388): +3V3 and +3V3A both derive from +5V and ramp within about 1 ms of each
  other. Pass.

### 7. Current sense (core U3 INA186A1, SBOS318B)

Pins (DCK) REF 1, GND 2, VS 3, IN+ 4, IN- 5, OUT 6, matching the netlist.
- Common mode is 0-29.4 V against a -0.2 to 40 V range (26 % margin) and 42 V abs.
- Gain 25 V/V with R1 10 mOhm: 5 A gives 1.25 V. The ESP32 ADC1 at 11 dB is usable to about
  2.45 V, so the range reaches 9.8 A (2x headroom). Offset is 50 uV RTI, i.e. 5 mA.
- The filter is 2 x 10 R and 100 nF, fc = 80 kHz. The bias-current error through 20 R is 3 nA,
  which is negligible.
- R1 dissipates 0.25 W at 5 A in a 3 W 2512 part (8 % of rating).
- Inputs are allowed above VS when unpowered. Pass.

### 8. Copper (POWER-1, POWER-12)

Core stackup: F/B.Cu 35 um, In1/In2 15.2 um. VBUS_IN (251 mm2), VBUS (240 mm2) and PVDD
(2777 mm2) are pours on In2 only. The series path at 5 A:

| segment | geometry (parsed from core.kicad_pcb) | load |
|---|---|---|
| J1 A4-B9 to D1 / VBUS_IN via (45.59,175.71) | 0.255 mm F.Cu, about 4.5 mm, about 8.7 mOhm | about 2.5 A, so about +60-90 C rise (IPC-2152 external 10 mil 1 oz: about 1 A at 10 C) |
| J1 B4-A9 to R1 pad 1 | 0.255 mm F.Cu, about 2.3 mm + 1 via (43.87,180.70) | about 2.5 A + whatever arrives from In2 |
| R1 pad 2 to In2 VBUS | 3 vias at 0.3 mm | 1.7 A/via |
| In2 VBUS to Q1 S (pins 1-3) | 3 vias + 0.25 mm stubs | 1.7 A/via |
| Q1 D (pins 5-8) to In2 PVDD | 2 vias (54.16 / 58.44, 170.25) | 2.5 A/via |
| In2 PVDD to each TPA3221 PVDD group | 3 vias + small F.Cu pours | up to 6 A peak per amp at full sine |

**Fix:**
- F.Cu pours: VBUS_IN from J1 pads to R1 pad 1 and D1; VBUS from R1 pad 2 to Q1 pins 1-3;
  PVDD on Q1 pins 5-8. Each at least 2.5 mm wide (1 oz external, 5 A, about 20 C).
- At least 6 vias of 0.3 mm (or 4 of 0.4 mm) at each layer change: R1 pad 1 side, R1 pad 2, Q1
  source, Q1 drain.
- At least 6 vias per TPA3221 PVDD pin group.
- With the THT USB4085-GF-A, connect its VBUS pins to pours on F.Cu and In2.
- In2 at 15 um is 1.13 mOhm/sq. The PVDD plane run from Q1 to U15 (about 15 mm by at least
  15 mm, about 1 square) costs 1.1 mOhm, 5.7 mV at 5 A. Fine once the vias are there.

Lite (1 oz both sides):
- A4-B9 path: 0.6 mm x 1.4 mm, then 1.0 mm x 3.1 mm (1.5 mOhm) into the F.Cu VBUS pour at Q1.
- B4-A9 path: 0.5 mm, 1 via 0.3 mm, B.Cu 1.2 mm x 8.1 mm, 1 via 0.4 mm.
- At 3.5 A the split is about 2.1 / 1.4 A, so the single 0.3 mm via is at the guideline
  (POWER-12; fix: add 2 vias next to (78.97,69.22)).
- Q1 drain: 4 vias of 0.4 mm, 0.9 A each. Amp side: 10 vias of 0.4 mm, plus a 535 mm2 B.Cu PVDD
  pour. Pass.

### 9. Capacitor voltage ratings (worst V / rating)

- Lite:
  - C58 polymer 35 V and C59 35 V at 21 V: 60 %.
  - C3/C4/C1/C51-55/C68-71 50 V at 21 V: 42 %.
  - C64-67 25 V (bootstrap, about 5 V).
  - C7/C8/C12 25 V at 5.3 V; C10/C13/C20 10 V at 5.3/3.3 V: 53 %.
  - C32 16 V at 3.3 V.
- Core:
  - C5/C6/C311 470 uF/50 V at 29.4 V: 59 %.
  - C7/C10/C1/C8/C4/C9/C207-210/C307-310 50 V at 29.4 V or below.
  - C13/C14/C16/C17/C30 25 V at 5.1 V.
  - C15/C21/C22 10 V at 5.1 V: 51 %.
  - C18 and the 1 uF 0402 (C52923) on +5V/+3V3A: rating not confirmed (known unknown).
- Ripple current: C58 polymer is rated 3.8 A at 100 kHz. C59 (RVT, 195 mA at 120 Hz) carries
  little ripple in parallel with the polymer. Core C5/C6 (Rubycon PX) ripple rating was not
  checked against TPA3221 ripple; that belongs to the amp domain.

### 10. Speaker loopback dividers (core, proposed)

150 k / 10 k from A_SPK_L+/R+ to AGND:
- Load: the BTL output common mode is PVDD/2 = 14 V, so 14 V / 160 k = 87 uA DC. Peak dissipation
  in the 150 k is (28 - 1.75)^2 / 150 k = 4.6 mW, against 63 mW for 0402 (7 %).
- Voltage: 28 V across the resistor against a 50 V 0402 rating, 44 % margin.
- Tap voltage: 0-1.75 V (0.875 V DC + audio), within a 3.3 V ADC.
- The 160 k is negligible against the 1 uF filter cap and the 4 Ohm load.
- If the receiving converter can be unpowered while the amp runs, up to 28 V / 150 k = 0.19 mA
  flows into its input clamp, below the usual 1-10 mA abs-max. Add a 1 nF to AGND at the tap for
  the class-D carrier.
- The AGND return of 87 uA is negligible.

## Tools wanted

See `power.json`:
- an SOA / transient-thermal checker that integrates a sim's Vds*Id against Zth(t);
- a series-path copper checker (pad -> track -> via count -> plane, per net and per current);
- an MLCC DC-bias lookup.

hwde's `check_current` / `check_irdrop` were not run in this pass: the core's series bottleneck
was found by parsing the PCB directly. `check_current` looks at track widths only and is blind to
via counts per layer transition.
