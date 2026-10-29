# Parts review: does each pin go to the right pad? (domain `parts`, task 261029-020622)

Reviewer: independent component-engineering audit, 2026-10-28. Boards: `boards/lite-main` (Lite) and
`boards/core` (Plus and Ultra builds). I didn't edit anything under `boards/`.

**Method.** For every active part and every polarized or multi-pin discrete I followed one chain:
datasheet pin function -> symbol pin name/number -> net in the KiCad netlist (`kicad/*.net`) -> pad of
that number in the placed footprint (`.kicad_pcb`: position relative to the footprint origin, size) ->
the LCSC number in `fab/BOM.csv`. `docs/review-2/sims/parts/chain.py` regenerates the
per-pin dump (`python chain.py <board>.net <board>.kicad_pcb out.txt`). Pad coordinates below are in
footprint-local mm, KiCad convention (+Y down, seen from the top). I fetched datasheets from the
vendor or from LCSC's hosted copy; each citation gives the document and page.

The netlist and the PCB agree pin for pin on both boards. There is one exception: core U1 pad 10, which
the netlist leaves unconnected but the PCB puts on GND (PARTS finding below).

Parts are listed in the order of how likely they are to kill a first spin. I wrote the file part by part.

## 1. SOT-23 transistors and the P-FET (both boards): all correct

| Ref | Board | LCSC -> MPN (vendor) | Datasheet pins | Symbol pins -> net | Footprint |
|---|---|---|---|---|---|
| Q2, Q3, Q4 | Lite | C20526 -> MMBT3904 (RANGE:100-300), JSCJ | 1 B, 2 E, 3 C (JSCJ MMBT3904 Rev 2.1 p1) | 1 B, 2 E, 3 C; Q2: B=Q2_B E=RTS C=ESP_EN; Q3: B=Q3_B E=DTR C=IO0/MCLK; Q4: B=PSW_B E=PD_PG C=PSW_PULL | SOT-23-3_L2.9-W1.3-P1.90 |
| Q3-Q6 | core | C2146 -> S8050 J3Y (RANGE:200-350), JSCJ | 1 B, 2 E, 3 C (JSCJ S8050 Rev 2.0 p1) | 1 B, 2 E, 3 C | SOT-23-3_L3.0-W1.7-P0.95 |
| Q2 | core | C8545 -> 2N7002, JSCJ | 1 G, 2 S, 3 D (JSCJ 2N7002 "J, Sep 2016" p1) | 1 G=PVDD_ENG, 2 S=GND, 3 D=PVDD_PD | SOT-23-3_L2.9-W1.3-P1.90 |
| Q1 | Lite | C5125949 -> AO4485, UMW | S 1-3, G 4, D 5-8 (UMW AO4485 sect. 3 "Pinning information" p1) | S 1-3 = VBUS, G 4 = PSW_G, D 5-8 = PVDD | SOP-8_L4.9-W3.9-P1.27 |
| Q1 | core | C51499 -> AO4485, Alpha & Omega | S 1-3, G 4, D 5-8 (AOS AO4485 datasheet p1 SOIC-8 top view) | S 1-3 = VBUS, G 4 = PVDD_G, D 5-8 = PVDD | SOIC-8_L5.0-W4.0-P1.27 |

I checked the pad positions, not just the pad numbers. In both SOT-23 footprints pads 1 and 2 sit on one side
(x = +1.0 or +1.1 mm, y = +0.95 / -0.95) and pad 3 sits opposite (x = -1.0 or -1.1, y = 0). If you rotate pad 3 to the top,
pad 1 lands bottom-left and pad 2 bottom-right. That is the datasheet's top view, so the footprint is not mirrored.
In the SO-8 footprints, pins 1-4 run left to right along the +Y row and 5-8 run right to left along the -Y row, which is
counter-clockwise from the top: correct.

Land patterns, footprint vs datasheet:
- JSCJ SOT-23 suggested pad layout (MMBT3904 Rev 2.1 p3): pad 0.6 x 0.8 mm, pitch 1.9, row centres 2.02.
  The Lite/core SOT-23-3_L2.9-W1.3 footprint: pad 0.65 x 1.00, pitch 1.90, rows 2.00, outer extent 3.00 mm.
  Pads are 0.05 mm wider and 0.2 mm longer than suggested: fine.
- Core S8050 footprint SOT-23-3_L3.0-W1.7: pad 0.532 x 1.037, pitch 1.90, rows 2.20, so the pads span 0.58-1.62 mm
  from the centre line. JSCJ package (p3) puts the lead tips at E1/2 = 1.125-1.275 mm and the body at E/2 = 0.60-0.70 mm,
  so the feet (L1 0.3-0.5) sit fully on the pads. The pad is 0.07 mm narrower than JSCJ's 0.6 but still
  wider than the 0.30-0.50 lead (b): **note**, no action.
- SO-8: Lite pad 0.63 x 1.865 on 5.36 mm row centres; core pad 0.568 x 1.95 on 5.42. IPC-7351 SOIC-8 nominal
  is 0.60 x 1.55-1.95 on 5.4. Both are fine.

Margins on the pins: the Lite Q1 gate is driven by a 100k/47k divider. At VBUS = 20 V that gives
Vgs = -20 x 100/147 = -13.6 V against the +/-20 V rating (AO4485 abs max), 32 % margin. On the core, D4 (BZT52C15)
clamps Vgs to 15 V, 25 % margin.

## 2. Regulators: pinouts all correct, land patterns adequate

| Ref | Board | LCSC -> MPN | Datasheet (pin table) | Pins -> nets (checked all) |
|---|---|---|---|---|
| U2 | Lite | C311983 -> TPS54302DDCR (TI, TSOT-23-6 "DDC") | TI SLVSDG6C (rev. March 2026) Table 4-1 p3: 1 GND, 2 SW, 3 VIN, 4 FB, 5 EN, 6 BOOT | 1 GND, 2 BUCK5_SW, 3 VBUS, 4 BUCK5_FB, 5 open, 6 BUCK5_BST |
| U3 / U5 | Lite / core | C479074 -> SY8089A1AAC (Silergy, SOT23-5) | AN_SY8089A1 Rev 0.0 p2 pinout: 1 EN, 2 GND, 3 LX, 4 IN, 5 FB | 1 EN=+5V, 2 GND, 3 BUCK3_LX, 4 +5V, 5 BUCK3_FB |
| U4 / U6 | Lite / core | C80670 -> LP5907MFX-3.3/NOPB (TI, SOT-23-5 "DBV", 3.3 V) | TI SNVS798Q Table 4-2 p3: 1 IN, 2 GND, 3 EN, 4 N/C, 5 OUT | 1 +5V, 2 GND, 3 EN=+5V, 4 open, 5 +3V3A |
| U4 | core | C97054 -> SY8303AIC (Silergy, TSOT23-8) | AN_SY8303 Rev 0.9A p2: 1 FB, 2 FS, 3 GND, 4 GND, 5 IN, 6 LX, 7 BS, 8 EN | 1 BUCK5_FB, 2 BUCK5_FS (R12 100k to GND), 3/4 GND, 5 VBUS, 6 BUCK5_LX, 7 BUCK5_BS (C12 10 nF to LX), 8 BUCK5_EN (R10/R11 100k/100k divider) |

Pin-level conditions:
- **TPS54302 EN left open (Lite U2): allowed.** SLVSDG6C Table 4-1: "Float the EN pin to enable"; sect. 7.3: "The EN pin has an
  internal pullup-current source which allows the user to float the EN pin". The rail can't be switched off, but that's a design choice.
- **LP5907 pin 4 N/C open: allowed** ("No internal electrical connection", Table 4-2). EN is tied to IN, which overrides the internal
  1 MOhm pull-down: correct.
- **SY8303 EN must not float** ("Pull high to turn on. Do not float", p2). It is driven by a 100k/100k divider from VBUS. At 5 V VBUS
  that gives 2.5 V > 1.5 V VENH (67 % above). At 28 V it gives 14 V < 40 V abs max (65 % margin). FS: 100 kOhm.
  The datasheet's formula as printed, "Fsw = 105/RFS kHz, RFS in kOhm", is off by 10^3 against its own table
  (RFS = 200k gives 0.5 MHz). Read as 10^5/RFS kHz, 100k gives 1.05 MHz, which matches the design intent. BS cap 10 nF = the value the datasheet
  recommends.
- SY8089 EN tied to IN: allowed (EN "pull high to enable").

Land patterns, footprint vs datasheet:
- TPS54302 DDC: TI DDC0006A example board layout (SLVSDG6C p30, 4214841/E): pad 1.1 x 0.6, pitch 0.95, row centres **2.7**.
  Footprint SOT-23-6_L2.9-W1.6-P0.95-LS2.8-BL: pad 1.072 x 0.532, pitch 0.95, row centres **2.30**. The pads therefore span
  0.61-1.69 mm from the centre line against TI's 0.80-1.90. The package lead tips are at 1.30-1.50 mm (E 2.6-3.0), so every foot is still on copper
  with a 0.19-0.39 mm toe. Pins are CCW from the top: pad 1 (-0.95, +1.15), 3 (+0.95, +1.15), 4 (+0.95, -1.15), 6 (-0.95, -1.15). **Minor**
  (PARTS finding): this is a generic EasyEDA SOT-23-6, about 0.4 mm narrower than TI's pattern, so the toe fillet is shorter. It's the same footprint
  as core U2 (USBLC6). It solders, but TI's pattern is better for the 3 A SW/VIN pins.
- SY8089 SOT23-5: Silergy recommended pad layout (AN_SY8089A1 p9): 0.60 x 1.00 pads, 0.95 pitch, rows 2.40. Footprint
  TSOT-23-5_L2.9-W1.6: 0.6 x 1.0 pads at x = +/-1.2, i.e. rows 2.40. **Exact match.**
- LP5907 DBV: TI DBV0005A land pattern (SNVS798Q p49-50): 5X 1.1 x 0.6, rows 2.6. Footprint SOT-23-5_L3.0-W1.7: 1.1 x 0.6 at
  x = +/-1.3 (rows 2.6). **Exact match.**
- SY8303 TSOT23-8: Silergy gives no land pattern (AN_SY8303 p8 has the outline only: span 2.60-3.00, lead width 0.20-0.40,
  foot 0.30-0.60, pitch 0.65). Footprint: pad 0.364 x 1.222, rows 2.42, so the pads span 0.60-1.82 mm against lead tips at 1.30-1.50.
  Pad gap 0.65 - 0.364 = 0.286 mm. That's adequate against IPC-7351 nominal (0.40 x 1.2, rows 2.6).

## 3. USB-PD sinks: CH224A (Lite U1) and CH224Q (core U1): pinouts correct

Source: WCH "CH224 Datasheet" CH224DS1-EN **V2.0** (wch-ic.com/download/file?id=302, fetched 2026-10-29). Table 4-1 p3 gives the
same pin numbers for CH224Q (DFN10) and CH224A (ESSOP10): 0 GND/EPAD, 1 VHV, 2 CFG2/SCL, 3 CFG3/SDA, 4 DP, 5 DM, 6 CC2, 7 CC1,
8 VBUS ("needs to be shorted to VHV"), 9 CFG1, 10 PG (open drain, active low). So a common symbol is legitimate.

| Pin | Datasheet | Lite U1 (CH224A, C42459160, ESSOP-10) net | Core U1 (CH224Q, C46061833, DFN-10-EP 2x2) net |
|---|---|---|---|
| 0/EP | GND | GND (pad 11, 3.3 x 2.1) | GND (pad 11, 1.4 x 0.9) |
| 1 | VHV (1 uF to GND) | VBUS + C1 1 uF | VBUS_PD (R17 10R from VBUS) + C1 1 uF |
| 2 | CFG2/SCL (I, internal PU, 6.5 V abs) | PD_SCL -> R2 0R -> I2C_SCL | I2C_SCL (2.2k to 3V3) |
| 3 | CFG3/SDA | PD_SDA -> R3 0R -> I2C_SDA | I2C_SDA |
| 4, 5 | DP, DM | open | open |
| 6 | CC2 | /CC1 (J1 A5) | CC2 (J1 B5) |
| 7 | CC1 | /CC2 (J1 B5) | CC1 (J1 A5) |
| 8 | VBUS, short to VHV | VBUS | VBUS_PD |
| 9 | CFG1 (Rset to GND) | R1 120k -> 20 V (Table 5-1) | R2 210k -> 28 V (Table 5-1) |
| 10 | PG (OD) | PD_PG, R4 10k to 3V3 | netlist: unconnected; **PCB: GND** |

- **Package suffix**: C42459160 is "CH224A, ESSOP-10" and C46061833 is "CH224Q, DFN-10-EP(2x2)" (LCSC). Each matches its footprint. Verified.
- **Lite CC swap**: the CH224A's CC2 pin goes to receptacle CC1 and vice versa. The two CC pins are symmetric Type-C CC inputs
  (Table 4-1 lists both as "Type-C CC signal line", and the reference schematic in sect. 6.1 connects them the same way), so the swap only
  changes which pin sees the plug orientation. **Note**, no action.
- **DP/DM open, CFG2/CFG3 on a 3.3 V I2C bus**: allowed. Sect. 6.1: "If I2C configuration is not used, CFG2 and CFG3 can be left
  floating"; the CFG2/3 abs max is 6.5 V (sect. 8.1.1), so a 3.3 V bus has 97 % headroom. Single-resistor mode keeps I2C enabled (sect. 5.2.3),
  at address 0x22/0x23.
- **Voltage on VHV/VBUS/PG/CC**: abs max 32 V (sect. 8.1.1). At a 20 V Lite contract that's 37.5 % margin; at 28 V core EPR, 12.5 % margin.
  **Datasheet inconsistency**: sect. 7.1 says "VBUS of CH224A has 3.8V tolerant voltage", which contradicts Table 4-1 ("shorted to VHV")
  and the 32 V abs max in 8.1.1. The design follows Table 4-1 and the reference schematics, as every WCH application does, so I've logged it as a
  known-unknown, not a finding.
- **Core U1 pad 10 (PG)**: the netlist leaves it unconnected, but in `core.kicad_pcb` the pad carries net GND. That is a schematic/PCB
  parity break. Electrically it's harmless: PG is an open-drain output, so a short to GND only sinks its own pull-down and
  nothing reads PG on the core. **Minor** (PARTS finding): re-sync, either by tying PG to GND in the schematic or by clearing the pad net, so
  that the KiCad parity check is clean.

Footprints:
- DFN10 (V2.0 p18): body 2.0 +/-0.1, pitch 0.4, lead 0.2 +/-0.05 x 0.3 +/-0.05, EP 1.5 +/-0.2 x 0.9 +/-0.15, pin 1 bottom-left in the top view,
  1-5 along the bottom, 6-10 back along the top. Footprint: pads 0.224 x 0.505 at y = +/-0.93 (span 0.68-1.18 mm from the centre,
  lead 0.70-1.00, so 0.18 mm of toe outside the body). Pad 1 is at (-0.8, +0.93) and pad 10 at (-0.8, -0.93): CCW, matches.
  EP 1.4 x 0.9 vs 1.5 x 0.9 nominal. EP-to-pad gap 0.68 - 0.45 = 0.23 mm. The gap between adjacent pads is 0.4 - 0.224 = 0.176 mm, too
  narrow for JLC to keep a solder-mask web between 0.4 mm-pitch pads (their minimum mask dam is about 0.2 mm on green). The mask opening
  will probably be ganged, which is normal for 0.4 mm DFN and is a **note**.
- ESSOP10 (V2.0 p17): lead span 6.0, pitch 1.0, foot 0.5, lead width 0.43; EP drawn but not dimensioned. Footprint: pads
  1.5 x 0.6 at y = +/-3.0 (rows 6.0), spanning 2.25-3.75 mm from the centre against the foot at 2.5-3.0. EP 3.3 x 2.1, long side along
  the pin rows; gap to the pad rows is 2.25 - 1.05 = 1.2 mm. Pin 1 is bottom-left and the numbering runs CCW. Matches. The EP size can't be verified from WCH's document,
  so a smaller real EP just means some exposed copper under the body, which is harmless.

## 4. Class-D amplifiers: pinouts, bootstrap pairing and footprints correct

### Lite U8 TPA3118D2 (C46497 -> TPA3118D2DAPR, HTSSOP-32 "DAP", PowerPAD **down**)
Source: TI SLOS708G (Apr 2012, rev. Dec 2017), sect. 5 pin diagram (DAP, top view) and Pin Functions table p3-4; DAP0032C outline
and example board layout p46-47 (4223691/A).

- **Pins: all 32 + PowerPAD match** the symbol names. In the netlist: 1 MODSEL = GND (BD mode), 2 SDZ = AMP_SDZ, 3 FAULTZ, 4/5 RINP/RINN,
  6 PLIMIT = GVDD (no limit, as the datasheet allows), 7 GVDD + C50 1 uF, 8 GAIN/SLV = R51 100k to GVDD / R50 20k to GND (Table 1:
  master, 26 dB), 9/22/25/28 GND, 10/11 LINP/LINN, 12 MUTE = GND, 13-15 AM2..0 = GND, 16 SYNC open (master output), 17 AVCC and
  18/19/31/32 PVCC = PVDD, 33 PowerPAD = GND.
- **Bootstrap pairing**: rev G of this datasheet corrected a pin 20/24 description error ("Changed pin 20 Description From: ceramic cap to
  OUTPL To: ... OUTNL"), so I checked each cap against its output. C67 joins BSNL(20) and OUTNL(21), C66 joins BSPL(24) and OUTPL(23), C65 joins BSNR(26)
  and OUTNR(27), C64 joins BSPR(30) and OUTPR(29). All four are 220 nF as the table specifies. **Correct.**
- The chip's "R" channel drives the nets named L (RINP = AMP_L_INP, OUTPR = OUT_L_P -> L1 -> SPK_L_P). It is consistent end to end,
  so this is a **note** only.
- **Footprint** HTSSOP-32_L11.0-W6.1-P0.65-LS8.1-BL-EP vs DAP0032C: TI land 32 x 1.5 x 0.45 at 0.65 pitch, rows **7.5** c-c;
  footprint 1.731 x 0.343 at 0.65 pitch, rows **7.44**, so the pads span 2.85-4.59 mm from the centre against a lead foot at 3.3-4.15. The pads are
  0.1 mm narrower than TI's (lead 0.19-0.30 wide), which is fine. Pin 1 is at (-4.88, +3.72), with 1-16 along +Y and 17-32 back along -Y: CCW, correct.
  Thermal pad: package exposed pad 2.46-3.04 x 3.16-3.74; TI's solder-mask-defined opening is 3.04 x 3.74 inside 5.2 mm of metal. Footprint
  EP copper 3.81 (along the body) x 3.71 (across), with 16 vias of 0.61 mm pad and 4 paste windows of 1.4 x 1.4 (7.84 mm^2 = 69 % of the 11.4 mm^2
  pad). The gap from the EP to the pin pads is 2.85 - 1.855 = 1.0 mm. **Verified.**

### Core U15/U16 TPA3221 (C475500 -> TPA3221DDVR, HTSSOP-44 "DDV", PowerPAD **up**)
Source: TI SLASEE9C (Sep 2017, rev. May 2025), Fig. 5-1 and Table 5-1 p3-4; DDV0044D outline and land pattern p47-48 (4218830/A).

- **Pins: all 44 match** the symbol. 1 VDD = +5V, 2 GAIN/SLV, 3 OTW_CLIP and 4 FAULT (open drain), 5-7/18-20/25-26/33-34/41-42 GND,
  8/9 IN1_P/M, 10 RESET, 11 HEAD = GND (AD mode, Table 5-2), 12/13 OSCM/OSCP shared U15 to U16, 14 FREQ_ADJ, 15/16 IN2_P/M,
  17 CMUTE = 33 nF, 21 AVDD, 22 GVDD, 23 BST2_M, 24 BST2_P, 27/28 OUT2_M, 29-31/36-38 PVDD, 32 OUT2_P, 35 OUT1_M, 39/40 OUT1_P,
  43 BST1_M, 44 BST1_P.
- **Bootstrap pairing** (Table 5-1: "0.033 uF capacitor to OUTx required"): U15 C222 joins BST1M(43) and OUT1M(35), C223 joins BST1P(44) and OUT1P(39/40),
  C224 joins BST2M(23) and OUT2M(27/28), C225 joins BST2P(24) and OUT2P(32); U16 C320-C323 are the same. All 33 nF. **Correct.**
- **Package suffix**: DDV is the only TPA3221 package. Its thermal pad is on the **top** ("PowerPad: Ground, connect to grounded heatsink.
  Placed on top side of device", Table 5-1; DDV0044D note 5: "designed to be attached to an external heatsink"). So the footprint correctly
  has no bottom exposed pad. Each amp is only usable with a heatsink clamped to the top pad, and neither core BOM has a heatsink line.
  I haven't raised that as a parts finding because it belongs to the thermal review. I've logged it as a known-unknown that could stop the board.
- **Footprint** HTSSOP-44_L14.0-W6.1-P0.64-LS8.1-BL vs DDV0044D land: TI 44 x 1.45 x 0.4, pitch 0.635, rows **7.5**; footprint
  1.6 x 0.4, rows **7.6**. Pad x-coordinates are rounded to 0.01 mm (alternating 0.64/0.63 steps); pin 1 to pin 22 = 13.34 mm vs
  21 x 0.635 = 13.335, so the worst accumulated error is 0.005 mm. The pads span 3.0-4.6 mm from the centre against a foot at 3.3-4.15. Pin 1 is at
  (-6.67, +3.8), pins 1-22 run along +Y and 23-44 back along -Y: CCW, correct. Pad gap 0.235 mm. **Verified.**

## 5. Codec, DACs, ADC and PHY in QFN/TSSOP: pinouts and footprints correct, one clock-margin item

| Ref | Board | LCSC -> MPN | Datasheet used | Result |
|---|---|---|---|---|
| U7 | Lite | C365736 -> ES8388 (QFN-28-EP 4x4) | Everest ES8388 Revision 5.0 (July 2018), sect. 2 pin table p5-6, sect. 9 package p35 (QFNWB4x4-28L, P0.45) | all 28 + EP match |
| U10 | core | C2688377 -> ES9018K2M (QFN-28-EP 5x5) | ESS ES9018K2M Datasheet v3.7 (22 Apr 2021), pin layout p3, pin descriptions p4, mechanical p36, land pattern p37 | all 28 + EP match |
| U13 | core | C2929446 -> ES7243E (QFN-20 3x3) | Everest ES7243E Revision 2.0 (June 2019), sect. 1 p4, register 0x02 p13, package p28 | all 20 + EP match |
| U12 | core | C107671 -> PCM5102APWR (TSSOP-20) | TI SLAS859C (May 2015), Pin Functions p5, PW0020A land p40-41 | all 20 match |
| U9 | core | C45223 -> LAN8720A-CP-TR (QFN-24 4x4) | SMSC/Microchip LAN8720A Rev 1.4 (08-23-12), Table 2.8 p15, REGOFF p11, nINTSEL sect. 3.7.4, ch. 6 package p75 | all 24 + EP match |

Per-part pin conditions (each read from the datasheet):
- **ES8388**: NC pins 9 and 25 are open ("No connect"). CE (26) on GND selects 2-wire mode with AD0 = 0 (p9). LIN2/RIN2 open is
  harmless for unused inputs. DVDD/PVDD go to +3V3 and AVDD/HPVDD to +3V3A, inside the 1.8-3.3 V range. EP on GND. The pin-19 name
  differs between the diagram ("DACVREF") and the table ("ADCVREF") in Everest's own document, but the function (decoupling cap) is the same.
- **ES9018K2M**: N.C. pins 2/19/22 are open ("may be grounded if desired"). ADDR (5) on GND gives 0x90 write = 0x48 7-bit (p17).
  XO (6) is open with an oscillator driving XI. GPIO1 (24) = OPT_SPDIF is legitimate: "GPIO1 - SPDIF input 4", spdif_sel 3'd3 (p9, reg 11).
  DATA_CLK/DATA1/DATA2 = BCK/LRCK/DOUT_MAIN. RESETB = DAC_EN (TCA9555 P04, R44 100k). DVDD (21) carries the internal regulator with
  C56 4.7 uF (datasheet minimum 2.2 uF: 114 % over). EP on GND ("must be connected to digital ground").
- **ES9018K2M MCLK = 50 MHz is exactly at the internal-DVDD limit.** p7 "DVDD Supply": "The internal DVDD should be used except ...
  PCM: MCLK > 50MHz or FSR > 192kHz ... An External DVDD (+1.3V) supply must be used above those frequencies." Y1 is a 50 MHz
  oscillator (C717684, YXC OT322550MJBA4SL), so the margin is 0 % before the oscillator's ppm error, which can put it 50-100 ppm *above* the limit.
  The MCLK pin itself is fine: TMCY >= 10 ns (100 MHz) on p30. **Minor** (PARTS finding). The cheapest fix also gives
  the 44.1/48 kHz families a synchronous ratio: replace Y1 with a 49.152 MHz 3.3 V CMOS oscillator in the same 3225 footprint (for example
  C7431354 SX3M49.152B10F20TNN, SCTF, 502 in stock; its pinout must be checked against Y1's before the swap), which gives 1.7 % margin.
- **ES7243E MCLK (20) tied to GND: allowed**, provided firmware sets register 0x02 bit 7 MCLK_SEL = 1 ("1 - from BCLK PAD", p13). The default (0) takes MCLK from the pad,
  so with the pad grounded the ADC is dead until firmware writes that bit: a firmware dependency, logged as a known-unknown.
  TDMIN (2) on GND is fine for an unused TDM input. AD0 = AD1 = GND. **AD2 is the SDOUT pin** ("SDOUT/AD2", chip address
  0010 AD2 AD1 AD0, p6), and SDOUT drives ESP32 IO36 (input-only, no pull). The address is therefore 0x10 or 0x14 depending on the pin's
  undocumented power-up state. Firmware must probe both: **note**. Neither collides with core's other I2C devices (0x20 TCA9555, 0x22/0x23 CH224Q,
  0x48 ES9018K2M).
- **PCM5102A**: SCK (12) on GND selects the BCK PLL ("a PCM510xA device supplies a SCK if an external SCK is not present at powerup",
  sect. 9.3.5.3). FMT = GND = I2S, FLT = GND = normal latency, DEMP = GND = off, XSMT = DAC_EN.
- **LAN8720A**: LED2/nINTSEL (2) open means the internal pull-up sets nINTSEL = 1 = "REF_CLK In Mode ... REF_CLK ... must be driven on the
  XTAL1/CLKIN pin" (sect. 3.7.4). That matches XTAL1/CLKIN = ETH_CLK from Y2, with XTAL2 (4) open. LED1/REGOFF (3) open means the internal
  pull-down keeps the 1.2 V regulator on (p11: "When REGOFF is floating or pulled low, the internal regulator is enabled"); VDDCR (6) is decoupled.
  RXER/PHYAD0 (10) open means PHY address 0. nINT/REFCLKO (14) open is an unused output.

Footprints (pad coordinates from the footprints; every one numbers its pads CCW from the top with pin 1 next to the last pin at the same corner):
- ES8388: package (p35) 4.0 BSC body, **e = 0.45**, b 0.18-0.28, L 0.274-0.426, EP 2.5-2.7. Footprint QFN-28_L4.0-W4.0-P0.45:
  0.23 x 0.665 pads at 0.45 pitch, centre 1.93 (spans 1.60-2.26 against a lead at 1.57-2.00), EP 2.4 x 2.4. EP-to-pad gap 0.40, pad gap 0.22.
  The unusual 0.45 pitch is correct for this part. **Verified.**
- ES9018K2M: ESS example land (p37): pads 0.25(-0.30) x 0.95 from 1.95 to 2.90 mm off-centre, EP 3.25 x 3.25 SMD. Footprint: 0.28 x 1.10
  from 1.95 to 3.05, EP 3.25 x 3.25. Inner edge identical, 0.15 mm longer outward. EP gap 0.325. **Verified.**
- ES7243E: package (p28) 3.0 BSC, e 0.4, b 0.15-0.25, L 0.2-0.4, EP 1.6-1.8. Footprint 0.20 x 0.665 at centre 1.41 (spans 1.08-1.74
  against a lead at 1.10-1.50), EP 1.65. EP gap 0.25, pad gap 0.20. **Verified.**
- PCM5102A: TI PW0020A land 20 x 1.5 x 0.45, rows 5.8. Footprint 1.742 x 0.364, rows 5.74. **Verified.**
- LAN8720A: package (ch. 6) 4.0 body, e 0.5, b 0.18-0.30, L 0.30-0.50, EP 2.40-2.60. Footprint 0.28 x 0.665 at 1.91 (spans 1.58-2.24),
  EP 2.55. EP gap 0.30. **Verified.**

## 6. I/O expander and the 50 MHz oscillators: TCA9555 internal pull-ups defeat the 100k pull-downs (blocker)

**U17 TCA9555PWR** (C465732, TSSOP-24 "PW"). Source: TI SCPS200E (Jul 2009, rev. Apr 2019), Pin Functions p3, sect. 5, electrical
characteristics p6, Figure "Simplified schematic of P-port I/Os" p16.
- **Pinout: all 24 match**: 1 INT, 2 A1, 3 A2, 4-11 P00-P07, 12 GND, 13-20 P10-P17, 21 A0, 22 SCL, 23 SDA, 24 VCC.
  A0 = A1 = A2 = GND gives 0x20. The footprint TSSOP-24_L7.8-W4.4 has 0.4 x 1.65 pads on 6.04 mm rows at 0.65 pitch against TI's PW land
  (1.5 x 0.45 pads on 5.8 rows), and pin 1 is at (-3.57, +3.02), CCW. **Verified.**
- **Part-choice problem.** SCPS200E sect. 5: "The TCA9555 is identical to the TCA9535, except for the inclusion of the internal I/O
  pull-up resistor, which pulls the I/O to a default high when configured as an input". The pull-up is 100 kOhm in the p16 schematic, and
  IIL is as much as -100 uA at VI = GND (p6), which means it can be as strong as 33 kOhm at 3.3 V. Every P-port is an input with this pull-up from
  power-on until firmware writes the configuration register. The design holds its enables low at boot with **100 kOhm pull-downs**
  (R40 AMP_A_RST, R41 AMP_B_RST, R43 PVDD_EN, R44 DAC_EN, R45 PHY_RST, all to GND), and those only form a divider with the internal
  pull-up:
  - 100k up / 100k down (typ.): V = 1.65 V = 50 % VCC.
  - 33k up (IIL max) / 100k down: V = 3.3 x 100/133 = 2.48 V = 75 % VCC.
  - PVDD_EN has R43 100k plus R7 100k (2N7002 gate leg, via R6 1k), 50k total down: 1.10 V typ., 3.3 x 50/83 = 1.99 V worst.
  Consequences before firmware runs (and indefinitely if firmware hangs):
  1. **PHY_RST drives Y2 pin 1 (OE/tri-state).** The oscillator enables at >= 70 % VDD *or floating*, so it has its own pull-up, and disables at <= 30 % VDD
     (YXC YSO110TR datasheet p1, "Tri-State" row, the document LCSC hosts for C717684). 1.65-2.48 V is 50-75 % VDD, i.e.
     undefined to enabled. Y2 OUT goes through R60 33R to ETH_CLK = **ESP32 IO0 (pin 25), the boot strap**. The ESP32 samples IO0 when EN
     rises (R30 10k / C32 1 uF, about 10 ms; oscillator start-up <= 3 ms), so a running 50 MHz clock on IO0 makes the boot mode (SPI boot vs
     download) random. The board then intermittently fails to boot, or boots into the ROM loader and sits there. The design's stated intent
     ("held off (OE low) until...") is exactly what this part defeats. **Blocker.**
  2. PVDD_EN reaches the 2N7002 gate at 1.10 V typ./1.99 V worst, against a JSCJ 2N7002 VGS(th) of 1.0-2.5 V. Q2 can partially
     conduct, pull PVDD_ENG and turn Q1 on, so PVDD is applied to both amps before firmware has configured them. **Part of the same finding.**
  3. AMP_A_RST/AMP_B_RST (TPA3221 RESET, active low) and DAC_EN (ES9018K2M RESETB, VIH = DVCC/2 + 0.4 = 2.05 V, p29; PCM5102A XSMT)
     sit mid-rail, so the amps and DACs can come out of reset in an undefined order.
  **Fix (cheapest, no layout change):** replace U17 with **TCA9535PWR (TI C130204**, or UMW C22396388), which is the identical part with no I/O pull-ups,
  with the same pinout, footprint and address 0x20 (SCPS200E sect. 5 says so explicitly). Then 100k pull-downs hold everything low. Also change **R45 to 10 kOhm**,
  because the oscillator's own OE pull-up is undocumented ("High voltage or Floating" = enable). 10k against a typical 47k-1M internal pull-up
  stays below 30 % VDD; bench-check this. As a contingency, keep a DNP 0R from Y2 pin 1 to GND so the oscillator can be strapped off and Ethernet
  dropped if needed.

**Y1/Y2 50 MHz oscillator** (C717684 -> YXC OT322550MJBA4SL, SMD3225-4P). Source: YXC "YSO110TR Wide Voltage" datasheet (the LCSC copy
for C717684) p1 specifications, p2 pin assignment, p3 3225 dimensions and land.
- Pins: 1 Tri-state (OE), 2 GND, 3 OUTPUT, 4 VDD (p2), with 1 bottom-left, 2 bottom-right, 3 top-right, 4 top-left in the top view. Symbol and footprint
  match: pad 1 (-1.1, +0.88), 2 (+1.1, +0.88), 3 (+1.1, -0.88), 4 (-1.1, -0.88). Y1 pin 1 = +3V3 (always on). Y2 pin 1 = PHY_RST (see above).
- Land: YXC recommends 1.3 x 1.2 pads with gaps of 0.8 (x) and 0.7 (y). The footprint has 1.4 x 1.2 pads with gaps of 0.8 (x) and 0.56 (y). The pads are 0.07 mm
  longer each, which is harmless. **Verified.**
- The datasheet is a series document (YSO110TR) with no part-number decoder showing that OT322550MJBA4SL = 3.3 V CMOS 50 MHz. I accepted LCSC's
  description (3225, 50 MHz). **Unverified** voltage/OE suffix.

## 7. ESP32 module, USB-UART and USB-C receptacles (plus the swaps proposed on 10-29)

### ESP32-WROVER-E-N8R8 (C2973667; Lite U5, core U7)
Source: Espressif "ESP32-WROVER-E & ESP32-WROVER-IE Datasheet" **v2.4**, Table 3 Pin Definitions p11-12, sect. 4 strapping p13-15,
Figure 15 Recommended PCB Land Pattern p44.
- **Pins: all 38 + pin 39 GND pad match** (1 GND, 2 3V3, 3 EN, 4 SENSOR_VP, 5 SENSOR_VN, 6 IO34, 7 IO35, 8 IO32, 9 IO33, 10 IO25, 11 IO26,
  12 IO27, 13 IO14, 14 IO12, 15 GND, 16 IO13, 17-22 NC, 23 IO15, 24 IO2, 25 IO0, 26 IO4, 27/28 NC, 29 IO5, 30 IO18, 31 IO19, 32 NC,
  33 IO21, 34 RXD0, 35 TXD0, 36 IO22, 37 IO23, 38 GND). All NC pins are open on both boards.
- **Land pattern**: Espressif gives 38 pads of 1.5 x 0.9 at 1.27 pitch on 17.5 mm column centres (8.0-9.5 mm from the centre line, 0.5 mm
  outside the 18 mm module), pin 1 7.44 mm below the antenna end and pin 19 1.1 mm above the bottom edge, and a 3.7 x 3.7 thermal pad of 9 x 0.9 squares
  with its centre 7.5 mm from the pin-1 edge and 16.16 mm above the bottom edge. Footprint WIFI-SMD_ESP32-WROVER-IE8MB has 2.0 x 0.9 pads at x = +/-9.0
  (8.0-10.0 mm: same inner edge, 0.5 mm longer outward), pin 1 at y = -11.43 and pin 19 at y = +11.43 (22.86 = 18 x 1.27). The EP is 3 x 3 squares of 0.9 at a 1.39 pitch
  centred at (-1.5, -3.62), which converts exactly to 7.5 mm from the pin-1 edge and 16.16 mm from the bottom (module bottom at y = +12.53). Pins 1-19
  are on the -X side, so the pattern isn't mirrored. The "-IE" name doesn't matter: Espressif states that the IE pin layout is the same as the E (sect. 3.1). **Verified.**
- **Strapping pins** (sect. 4, Table 4 defaults: IO0 pull-up, IO2 pull-down, IO5 pull-up, IO12/MTDI pull-down, IO15 pull-up):
  Lite: IO0 = I2S_MCLK (the ES8388 MCLK input is high-Z, so the auto-reset Q3 decides the boot mode), and IO2/IO5/IO12/IO15 are open. Core: IO12 = PCM5102A DIN (a high-Z
  "failsafe" input, so the module's pull-down keeps MTDI = 0 and VDD_SDIO = 3.3 V, which this module's flash/PSRAM need). IO2 = I2S_BCK (every listener is
  an input at reset; the ES7243E defaults to slave, reg 0x00 MSC = 0 p12). IO5 = ES9018K2M DATA2 (input). IO15 = LCD_DC (on the FFC). **IO0 = ETH_CLK** is
  the failing case: see PARTS-4.
- Input-only pins (IO34-39) are used only as inputs on both boards. Lite IO36 = BTN_VOLDN has an external 10k pull-up (R60), which it needs because
  IO34-39 have no internal pulls.

### CH340C (C84681; Lite U6, core U8) and the proposed CH340B (C81010)
Source: WCH CH340DS1 "CH340 Datasheet" **V3C** (LCSC copy for C84681) sect. 4 pin table p3-4. CH340B: the Chinese edition **3D** (LCSC copy for C81010),
same table.
- CH340C SOP-16: 1 GND, 2 TXD, 3 RXD, 4 V3, 5 UD+, 6 UD-, 7 NC ("CH340C: Empty pin, must be suspended"), 8 OUT#, 9 CTS#, 10 DSR#, 11 RI#,
  12 DCD#, 13 DTR#, 14 RTS#, 15 R232 (internal pull-down), 16 VCC. **All 16 match.** V3 is tied to VCC = +3V3, as the 3.3 V supply requires ("When the
  power supply voltage is 3.3V, connect VCC"). UD+/UD- connect directly, as WCH recommends ("not connected with additional resistors"). Unused pins 7-12 and 15 are open, which WCH allows
  ("The unused I/O pins of CH340 can be dangled").
- **CH340B swap**: in the same SOP-16 package, the only pin changes are **7 = RST#** (input, internal pull-up), **8 = NC** ("must be suspended")
  and **15 = TNOW** (output). Both boards leave 7, 8 and 15 unconnected, so **the CH340B drops in with no netlist or footprint change**. Only the
  symbol names on 7/8/15 go stale: rename them so the schematic stays honest. Verified against both editions.

### USB-C: GCT USB4105-GF-A-120 (C5184243, both boards, current)
Source: GCT drawing USB4105 rev B3 (4 Oct 2019), sheet 1 "Recommended PCB Layout, as viewed from component side".
- Pad order matches: A1/B12, A4/B9, B8, A5, B7, A6, A7, B6, A8, B5, B4/A9, B1/A12 at x = -3.20 / -2.40 / -1.75 / -1.25 / -0.75 / -0.25 / +0.25 / +0.75 /
  +1.25 / +1.75 / +2.40 / +3.20 (GCT: 6.40, 4.80, 3.50, 0.50 pitch). Signal pads 0.30 x 1.15, power pads 0.60 x 1.08 (Lite) or 1.05 (core) against
  GCT's 0.60 x 1.15; the outer edge is identical. Pegs: NPTH 0.65 at +/-2.89 (GCT 2x 0.65 at 5.78). Shell slots: 0.6 x 1.7 drill in a 1.0 x 2.1 pad, and 0.6 x 1.4
  in 1.0 x 1.8, at x = +/-4.32 (GCT 8.64). The pad row sits 4.76 mm from the rear slots (GCT 4.18 to the pad edge + 1.15/2) and 4.18 mm from the front slots. Annulus 0.20 mm.
  A1 is on the left with the pads at the top: not mirrored. **Verified.** CC1 = A5 and CC2 = B5 go to the CH224 (Lite crossed: note in sect. 3).
  Rating 48 V / 5 A (GCT spec), giving 42 % margin at a 28 V EPR contract.

### Proposed Lite J1 swap: HRO TYPE-C-31-M-12 (C165948)
Source: Korean Hroparts drawing TYPE-C-31-M-12 rev A (2020-12-08), LCSC copy.
- Pin order is identical (A1/B12, A4/B9, B8, A5, B7, A6, A7, B6, A8, B5, B4/A9, B1/A12) and so are the pad pitches (0.50 / 3.50 / 4.80 / 6.40, 8 x 0.30 + 4 x 0.60 pads).
  Slot pitch is 8.65 (vs 8.64) with 0.60 drills, and the pegs are 2 x 0.50 in 0.60 holes (the current footprint has 0.65 holes: 0.15 mm clearance, OK). The pad row is 4.18 mm from the rear slots, the same as GCT.
  **The existing footprint fits within 0.05 mm, so this is a BOM-only swap.** Verified against the drawing.
- **Rating: "VOLTAGE RATING: 20V", 5 A** (drawing note 4-1). The Lite asks for a 20 V PD contract, so that is **0 % margin** on the connector's rated voltage
  (GCT USB4105: 48 V). It's acceptable only because USB-PD SPR caps at 20 V +5 %. **Minor** (PARTS finding): a 21 V source tolerance is 5 % over the
  rating. Keep the GCT part if the cost delta allows.

### Proposed core J1 swap: GCT USB4085-GF-A (C7095263)
Source: GCT drawing USB4085 rev B (28/02/23), sheets 1-2.
- This is a **"Dip Type, PCB Top Mount"** receptacle: 16 *through-hole* pins in two staggered rows (A1 A4 A5 A6 A7 A8 A9 A12 / B12 B9 B8 B7 B6 B5
  B4 B1, 0.85 mm pitch, rows 0.85 mm apart; outer pin centres 5.95, 4.25, 2.55 apart), holes of **0.40 mm in 0.65 mm pads**, and 4 shell slots (0.6 x 2.1 in 0.9 x 2.4,
  0.6 x 1.4 in 0.9 x 1.7, 8.65 mm apart). **It does not fit the present SMD footprint or symbol.** The present symbol merges A4-B9 and A1-B12 into shared
  pads, and this part has every pin separate. Pin functions are the standard USB 2.0 set (sheet 1 table: A1/A12/B1/B12 GND, A4/A9/B4/B9 VBUS, A5 CC1, B5
  CC2, A6/B6 Dp, A7/B7 Dn, A8/B8 SBU). The swap needs a **new footprint and a 16-pin symbol**, with each VBUS and GND pin tied in the schematic.
  GCT's own 0.65/0.40 pad gives a 0.125 mm annular ring, below JLC's 0.15 mm PTH floor (hwde fp_verify uses the same floor). The 0.85 mm
  pitch allows at most a 0.70 mm pad (0.15 mm gap), so expect a DFM warning. The rating is the same as the USB4105 (48 V, 5 A). **Major if the swap is made
  without the footprint work** (PARTS finding).

## 8. Protection, sensing, LEDs and optical receivers

| Ref | Board | Part (LCSC) | Datasheet pins | Result |
|---|---|---|---|---|
| D1 | Lite | SMBJ20A (C364296) | cathode band (unidirectional) | pad 1 = K (silk bar at x = -1.02 beside pad 1; JLC/EasyEDA model C364296: pin 1 = K) -> VBUS, pad 2 = A -> GND. Correct. |
| D1 | core | SMBJ30A, MDD (C113998) | MDD SMBJ datasheet p1: "Cathode Band (for Uni-directional products only)" | pad 1 = K -> VBUS_IN, A -> GND. Correct. Same footprint as Lite. |
| D2 | core | BZT52C33, BORN (C266574) | BORN BZT52Cxx p1: "Polarity: Color band denotes cathode" | the symbol names its pins "1"/"2", but the footprint's cathode bar (x = -0.97) and red marks are at pad 1 = VBUS, with OV_Z on pad 2: reverse-biased from VBUS, as an over-voltage zener must be. Correct. **Minor**: rename the symbol pins K/A so the schematic shows the polarity. |
| D4 | core | BZT52C15, MDD (C173427) | same convention | K (pad 1) = VBUS = Q1 source, A = PVDD_G = gate: clamps Vgs to -15 V. Correct. |
| D2 / D3 | Lite / core | KT-0603R, Kento (C2286) | Kento 0603 red datasheet p2: "1" = + (anode), "2" = - (cathode, green mark); pads 0.70 x 0.70, 0.70 gap | symbol 1 = A (LED_A via R64/R401), 2 = K (GND); footprint pad 1 at +0.75, pad 2 at -0.75 (0.8 x 0.8, 0.7 gap). JLC's EasyEDA model C2286 uses the same numbering (pin 1 A, pin 2 K). Correct. |
| U2 | core | USBLC6-2SC6, UMW (C2687116) | UMW USBLC6-2SC6 sect. 4: 1 I/O1, 2 GND, 3 I/O2, 4 I/O2, 5 VBUS, 6 I/O1 | 1/6 = USB_DP, 3/4 = USB_DM, 2 = GND, 5 = +5V. Correct (the pinout is symmetric, so orientation can't swap D+/D-). |
| U3 | core | INA186A1IDCKR, TI (C2058311) | TI SBOS318B Table 5-1: DCK 1 REF, 2 GND, 3 VS, 4 IN+, 5 IN-, 6 OUT | REF = GND (unidirectional), VS = +3V3, IN+ = ISNS_P (R18 10R from VBUS_IN = bus side of R1), IN- = ISNS_N (R19 10R from VBUS = load side), OUT = ISENSE_RAW. Correct, high-side polarity as the table specifies ("connect IN+ to bus voltage side"). |
| U9 | Lite | IRM-3638T, Everlight (C42421366) | Everlight IRM-36xxT DMO-0000371 Rev.4 p1: 1 OUT, 2 GND, 3 Vcc; Vcc 2.7-5.5 V (p3) | 1 IR_IN, 2 GND, 3 IR_VCC (3V3 via R65 100R + C80 4.7 uF RC filter, as p2 requires). 2.54 pitch. Correct. LCSC has no datasheet for C42421366; I used the Everlight series sheet (part-elec.com copy). |
| U18 | core (Plus) | IRM-H638T/TR2, Everlight (C91447) | Everlight IRM-H6XXT/TR2 DMO-0000008 Rev 3 (2025-01-14) p6: 1 GND, 2 GND, 3 OUT, 4 Vcc; land 1.5 x 1.1 pads, pitch 2.54, inner gap 4.8 | symbol 1/2 GND, 3 VOUT = IR_IN, 4 VCC = IR_VCC (R400 100R). Footprint: pads 1/2 on one side, 3/4 on the other, 2.54 pitch. Pads are 0.9 x 1.5 spanning 2.50-3.40 mm from the centre, against the vendor's 1.5 x 1.1 spanning 2.40-3.90 and lead tips at 3.5. Footprint pads are 0.5 mm shorter outboard but still cover the foot: **note**. The drawing doesn't say which side 3 (OUT) and 4 (Vcc) sit on relative to the lens, so that's verified by number only. |
| U19 | core (Ultra) | PLR237/T8BK, Everlight (C17223620) | Everlight PLR237/T8BK DPL-0000263 Rev.1 (2021-03-09) p5: **1 Vout, 2 GND, 3 Vcc**, 4/5 NC; PCB layout: 3 x 0.80 holes at 2.54, 2 x 1.10 at 5.08 (2.63 forward), **2 x 1.70 at 5.60 (a further 2.61 forward)** | see below |

**U19 PLR237/T8BK: wrong footprint variant, and a symbol numbering that disagrees with Everlight (blocker for the Ultra build).**
- The symbol (named "PLR237_T10BK") numbers **1 VCC, 2 GND, 3 VOUT**, the reverse of Everlight's "1. Vout 2. GND 3. Vcc" for the T8BK. The
  footprint OPTO-TH_PLR237-T10BK (EasyEDA, tagged LCSC C17427482, which is the T10BK) puts pad 1 at x = -2.54 on the same side as post 4. Everlight's
  bottom view puts pin 1 on the same side as post 5, so in a top view with the port forward, Everlight's pin 1 sits where the footprint's pad 3 is.
  If that reading holds, the EasyEDA part reverses the numbers *and* the positions, and the copper is right (Vcc copper under the Vcc lead). It
  hinges on reading a mirrored view of an unnumbered drawing, so I've marked it **unverified, could stop the board**: a swap feeds 3.3 V into the
  receiver's output.
- **Definite problem: the T8 lead form needs two 1.70 mm locating-peg holes** 5.60 mm apart, 2.61 mm in front of the 1.10 mm post holes (T8BK p5
  "PCB Layout for Electrical Circuit"). The T10BK footprint has only the 3 signal holes and the 2 x 1.10 holes. Without the peg holes the T8BK
  can't sit down on the board. **Blocker (Ultra only)** (PARTS finding). Fix: either order the part the footprint was made for (PLR237/T10BK,
  C17427482; confirm its pin numbering against its own Everlight sheet) or add 2 x NPTH 1.70 mm at (+/-2.80, +3.93) relative to the signal row,
  with the port side forward. Whichever part is used, renumber the symbol to Everlight's numbering and check pad 1's position against the vendor
  drawing before ordering. Supply: Vcc 3.3 V (FB3 from +3V3) against a 3.0-5.5 V operating range (T8BK p2), so 10 % margin at the low end.

