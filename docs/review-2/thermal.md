# Thermal / power-dissipation review: lite-main and core (Plus/Ultra)

Reviewer domain: `thermal`. Date 2026-10-28. Read-only on `boards/`.
Inputs: `boards/*/kicad/*.kicad_pcb` (parsed directly; scripts in
`docs/review-2/sims/thermal/`), the generator sources for values, the BOMs,
and vendor datasheets: TI SLASEE9C (TPA3221, rev C May 2025), TI SLOS708G
(TPA3116D2/3118D2/3130D2, Dec 2017), TI SLOU342A (TPA3118D2EVM), plus the
LCSC parameter records for the passives and FETs.

Assumptions used everywhere:
- Air inside the box: 35-40 C was the brief. My own estimate for Lite is below
  (section 6). For heavy listening I use **45 C** inside the box as the worst
  case.
- Supply: Lite PVDD = 20 V (CH224A, R1 = 120k). Core PVDD = 28 V EPR (CH224Q,
  R2 = 210k).
- Loads: 4 ohm and 8 ohm. "1/8 power" means 1/8 of the 1 % THD power, both
  channels driven.
- The output and thermal limits are: TPA3221 OTW 115/125/135 C (min/typ/max)
  and OTE 145/155/165 C (SLASEE9C 6.5, p9). TPA3118D2 thermal trip is "150+" C
  with 15 C hysteresis (SLOS708G 6.6) and has no warning pin.

---

## 1. Core: TPA3221 (U15, and U16 on Ultra). BLOCKER

### 1.1 Package: it is the pad-UP DDV part, and the board has no heatsink

- **BOM:** U15/U16 = TPA3221**DDV**R (LCSC C475500). `blocks_amp.py` uses the
  symbol `TPA3221DDVR`.
- **Datasheet:** SLASEE9C section 5 says "The package type contains a heat slug
  that is located on the top side of the device". In table 5-1 the PowerPad is
  "Ground, connect to grounded heatsink. Placed on top side of device". The
  front-page bullet "Thermal pad located on package bottom" contradicts this.
  It is wrong for DDV: the drawing DDV0044D, note 5, says "The exposed
  thermal pad is designed to be attached to an external heatsink".
- **Footprint:** `aiee:HTSSOP-44_L14.0-W6.1-P0.64-LS8.1-BL` has 44 pads and
  **no exposed pad**. U15 is at (46.09, 161.70) and U16 at (78.99, 161.70),
  both on F.Cu.
- **Board:** the only holes are H1-H4, the M3 holes in the corners. Nothing
  holds a heatsink, and no keep-out is reserved for one.
- **Top slug:** 6.72-7.30 x 3.85-4.43 mm, about 29 mm2 (DDV0044D).

### 1.2 Dissipation of the chip

SLASEE9C figure 6-9 gives the system loss, read at 250 dpi. It is measured at
30 V, AD mode, TC = 75 C, and includes TI's inductors.

| 2-ch output | 4 ohm loss | 8 ohm loss |
|---|---|---|
| 0 W (idle) | 0.37 W (table 6.6, AD) | 0.37 W |
| 10 W | ~3.3 W | ~2.2 W |
| 25 W | ~6.5 W | ~4.5 W |
| 50 W | ~11.5 W | ~8 W |
| 100 W | ~20.5 W | ~11 W |
| 150 W | ~26 W | - |

On this board HEAD is tied to GND, so it runs in **AD mode**, the
higher-idle-loss mode. VDD/AVDD/GVDD come from the external +5 V, with the
internal LDO bypassed. The chip's share of the loss below is the figure-6-9
loss minus the output inductors (section 5: DCR loss plus about 0.3 W of core
loss each), and scaled by 28/30 on the switching part.

| Case (per TPA3221) | Output, 2 ch | Chip dissipation |
|---|---|---|
| Idle, AD mode, 28 V | 0 | 0.30 W PVDD (11 mA, fig 6-16) + 0.13 W gate/analog from 5 V (GVDD 16 mA + AVDD 10 mA) = **0.45 W** |
| 1/8 power, 4 ohm (P1% = 77 W/ch at 28 V) | 19.2 W | system ~5.2 W, so chip **3.5 W (3.0-4.5)** |
| 1/8 power, 8 ohm (P1% = 43 W/ch) | 10.8 W | system ~2.4 W, so chip **1.5 W (1.2-2.0)** |
| Full power, 4 ohm, sine, both ch | 154 W (supply-limited: see 1.6) | chip **~22 W** (20-24) |
| Supply-limited continuous, 4 ohm (~113 W out of 140 W in) | 113 W | chip **~19 W** |
| Full power, 8 ohm, both ch | 86 W | chip **~9 W** |

### 1.3 Without a heatsink (as built)

- **Thermal resistance:** SLASEE9C 6.4 gives RthetaJA = 44.8 C/W on a JEDEC
  2s2p board with no heatsink. JESD51-7 has 2 oz outer and 1 oz inner copper.
  The core board is 1 oz outer and 0.5 oz inner (stackup in `core.kicad_pcb`:
  0.035 / 0.0152 / 0.0152 / 0.035 mm), and it sits in a box. So the real
  figure is worse: I use **45-55 C/W**.
- **Idle:** Tj = 45 + 0.45 x 50 = **68 C**. Margin to OTW min (115 C) is 47 K.
  Pass.
- **1/8 power, 8 ohm:** Tj = 45 + 1.5 x 50 = **120 C**. That is above OTW min
  (115 C), and 25 K under OTE min. OTW_CLIP asserts. **Fails** the "no warning
  in normal use" bar.
- **1/8 power, 4 ohm** (the product's own use case: Viennas, 4 ohm, "driven
  hard"): Tj = 45 + 3.5 x 50 = **220 C**, which is not reachable. The part
  hits OTE (145-165 C) and **latches off**, because OTE needs RESET toggled.
  Even at 35 C air and 44.8 C/W, Tj = 192 C.
- **Highest chip loss that stays under OTW min with no heatsink:**
  (115 - 45)/50 = **1.4 W**. That is about 2 x 4 W into 4 ohm, or 2 x 6 W into
  8 ohm, continuous.

=> **THERMAL-1 (blocker): the TPA3221 has no heatsink. It will
thermal-shutdown at normal listening levels into 4 ohm.**

### 1.4 The enclosure worker's proposal: an adhesive 19 x 12 mm heatsink up to the lid

**Proposal:** the free volume over U15 is x 2-21, y 52.5-64.5 mm in board
frame, i.e. 19 x 12 mm, to the lid. A 19 x 12 mm adhesive heatsink, with lid
vents above it.

Thermal network for each TPA3221:
- **Junction to slug:** RthetaJC(top) = 1.1 C/W (SLASEE9C 6.4, JEDEC column).
- **Interface, adhesive thermal tape** (typical 0.25 mm, ~1 W/mK) over the
  29 mm2 slug: 0.25e-3 / (1.0 x 29e-6) = **8.6 C/W**.
  - A 0.1 mm, 3 W/mK pad with a clamp gives 1.1 C/W.
  - TI's own number assumes 2 mil grease at 0.7 W/mK: 2.5 C/W, which with
    RthetaJC(top) 2.0 is their 5.5 C/W "fixed 85 C heatsink" figure.
- **Spreading in the heatsink base:** ~1 C/W.
- **Heatsink to air,** for a 19 x 12 mm footprint with fins H tall in still
  air:
  - H = 25-30 mm: about **25 C/W ±30 %**. This is scaled by footprint area
    from typical 19 x 19 x 25 mm BGA pin-fin parts (~20-22 C/W natural
    convection) and 20 x 20 x 10 mm parts (~28-30 C/W). An enclosure with lid
    vents directly above can do ~20 % better. Without vents it is worse.
  - H = 10 mm: ~38 C/W.
- **Board path in parallel:** about 50 C/W (section 1.3).

| Variant | Heatsink path | Total thetaJA | 1/8 P, 4 ohm (3.5 W, 4.5 W) | 1/8 P, 8 ohm (1.5 W) | Max chip W under OTW min - 5 K (110 C) |
|---|---|---|---|---|---|
| 19x12x25 mm, adhesive tape 0.25 mm/1 W/mK | 1.1+8.6+1+25 = 35.7 | 20.8 | **118 C / 139 C** (OTW; 6 K under OTE min at 4.5 W) | 76 C | 3.1 W |
| 19x12x25 mm, 0.1-0.2 mm 3-6 W/mK pad, clamped | 1.1+1.1+1+25 = 28.2 | 18.0 | **108 C / 126 C** | 72 C | 3.6 W |
| 19x12x30 mm, clamped pad, lid vents over it (-20 %) | 1.1+1.1+1+20 = 23.2 | 15.9 | **101 C / 117 C** | 69 C | 4.1 W |

All rows assume 45 C box air.

**Verdict on the 19 x 12 mm adhesive heatsink:**
- **1/8 power, 8 ohm:** enough in every variant. Tj 69-76 C, at least 39 K
  under OTW min.
- **1/8 power, 4 ohm:** **not enough with adhesive tape.** Tj is 118 C at the
  centre estimate (OTW asserts) and 139 C at the upper estimate (6 K from OTE
  min).
  - It **becomes enough (marginal)** only with all three of: a thin
    high-conductivity pad (≤ 0.2 mm, ≥ 3 W/mK), mechanical clamping (an
    adhesive tape is ~8x the interface resistance), and fins ≥ 25-30 mm tall
    under lid vents.
  - Then Tj is 101-108 C centre and 117-126 C upper. That is 7-14 K under OTW
    min at the centre estimate, and OTW at the upper.
- **Full power (continuous sine, 4 ohm, chip ~19-22 W):** **no passive
  heatsink in this volume is enough.**
  - Steady state at 22 W x 15.9 C/W is +350 K.
  - Transient: the heatsink is about 2.7 cm3 of aluminium at 40 % fill,
    ~7.4 g, ~6.7 J/K. The junction-to-heatsink drop at 22 W is 22 x 3.2 = 70 K
    with a clamped pad, so OTE min (145 C) arrives when the heatsink reaches
    ~75 C. From 45 C that takes 30 K x 6.7 J/K / 22 W ≈ **9 s**. With
    adhesive tape (22 x 10.7 = 235 K drop), the junction is past OTE
    immediately.
  - Even TI's 5.5 C/W junction-to-heatsink figure with an 85 C heatsink cannot
    hold 22 W (85 + 121 = 206 C). Continuous full power is a firmware limit,
    not a heatsink problem: see 1.6.
- **Full power, 8 ohm (chip ~9 W):** OTE in ~30 s. A firmware limit again.
- **Ultra zone B (U16):** same land, same loss, the same per-chip result.
  - The box gets twice the amp heat, and the two heatsinks are 33 mm apart
    (U15 at x 46.09, U16 at x 78.99).
  - Each needs its own heatsink. U16 has the same y-limits (output inductors
    L5-L8 above it, and its 33 nF/1 uF parts below).
  - The 140 W input is shared, so per-chip continuous loss in the
    supply-limited case falls to about 2 x 9 W. The governor below covers it.

### 1.5 Specification (fix for THERMAL-1)

**Heatsink** (per TPA3221):
- **Thermal:** RthetaSA ≤ **20 C/W in natural convection**. Total
  junction-to-air ≤ 16 C/W with the board path.
- **Size:** base 19 x 12 mm, the maximum allowed by the free volume. Fins
  vertical, parallel to the 12 mm side, **≥ 25 mm tall** (to the lid).
- **Finish:** black anodised.
- **Fin overhang:** if the lid height allows, the fins may overhang the 19 x
  12 base above about 6 mm height. The output inductors are about 5.4 mm
  tall. Check the enclosure STEP. This is the cheapest way to more area.
- **Parts:** LCSC has no part that fits 19 x 12 mm.
  - XSD C286198 (20 x 20 x 10, ~28-30 C/W) and C286230 (14 x 14 x 10,
    ~35-40 C/W) are too short or too wide: 20 mm in y collides with L2/L3
    (U15) and L6/L7 (U16).
  - C32487 (30 x 30 x 25, 2 pins) is far too wide.
  - The practical source is a cut length of 12 mm-wide extruded fin profile, or
    a 19 x 12 x 25 mm "VRAM/MOSFET" stick-on heatsink (common on
    Amazon/AliExpress).
  - I found none on LCSC. Treat this as a hand-fit part, not an SMT line item.

**Interface:**
- Do **not** use a plain adhesive tape. Use a 0.2 mm, ≥ 3 W/mK silicone
  thermal pad, or a thermal adhesive that is ≥ 1.5 W/mK and ≤ 0.1 mm
  bondline, with a mechanical hold-down.
- The slug is GND. TI note T3 asks for a grounded heatsink. An isolating pad
  leaves the fin floating over a 600 kHz switch node, which is an EMI
  question for the EMC reviewer.

**Hold-down: the board needs mounting provision.** No through-hole fits beside
U15:
- Left: U15's body starts about 4.5 mm from the board's left edge.
- Above (-y): the inductors L2/L3, from dy -7.8 mm.
- Below (+y): D1 (SMB) from dy +8.6, and the decoupling caps.

So the provision has to be one of:
1. **Two plated 1.5 mm slots for a wire clip,** at dx = ±(9.5-10) mm on the
   package's long axis. Each needs a ≥ 2 mm keep-out from C209 (dx -9.3) and
   C207 (dx +9.7), so **C207/C209 move about 2 mm out first**. Connect the
   slots to GND so the clip grounds the heatsink. This is the TO-220-clip
   style.
2. **A lid boss,** a printed rib in the lid pressing on the heatsink top
   through a 1 mm silicone pad. It costs nothing on the board, but it depends
   on print tolerances and PETG creep at 60-70 C. It is weak.
3. **Keep-out:** in either case, reserve the 19 x 12 mm area in the PCB as a
   courtyard/keep-out on F.Cu for parts over 1.0 mm tall. The package is
   1.0-1.2 mm (DDV0044D). 0805 caps are 1.25 mm, so none may sit under the
   heatsink base. Today C202/C204/C211 (0402/0603) are inside the 12 mm
   y-window. 0603 is 0.8 mm, so they are OK.

**Alternative that removes the heatsink** (record it as a contingency): the
pin-compatible **pad-down TPA3220DDW** (SLASEE9C table 4: "TPA3220 ... Bottom
... Y"; LCSC C2879306) with a 4-layer EP land plus a via array.
- LCSC stock was 0 on 2026-10-28.
- It is rated 35 W stereo continuous, 100 W peak, which matches what a 140 W
  USB-C input can sustain anyway.

### 1.6 THERMAL-2 (major): firmware power governor (needed with or without the heatsink)

The board has the inputs for it:
- `ISENSE`: INA186A1 x25 on a 10 mohm shunt, to ESP32 IO34.
- `AMP_OTW` (OTW_CLIP, both chips wired-OR) to TCA9555 P11.
- `AMP_FAULT` to TCA9555 P10.

Thresholds, for the heatsink in 1.5 (total ~16 C/W, chip ≤ 4 W for Tj ≤ 110 C
at 45 C air):

- **Input-power limits from ISENSE** (VBUS at 28 V):
  - Long term (60 s moving average) **≤ 30 W in, about 1.05 A**. That is
    about 20 W out into 4 ohm per TPA3221, or 30 W into 8 ohm.
  - Short term (5 s average) ≤ 70 W in, 2.5 A.
  - Instantaneous: let the bulk caps carry peaks, and cap the PD contract at
    ≤ 4.8 A.
  - The heatsink time constant is about 6.7 J/K x 20 C/W ≈ 130 s, so a 60 s
    window is safe.
  - **Ultra:** the limit is per chip. Total input ≤ 55 W, 2.0 A, long-term.
- **OTW_CLIP low for > 200 ms with no clipping** (OTW_CLIP also pulses on
  clipping, so filter it: OTW holds steadily low, while clip is a 500 ns to
  ms-scale pulse train): step the volume -3 dB every 2 s until it releases.
  The datasheet's recovery hysteresis is 20 K.
- **FAULT low:** mute, wait 1 s, toggle RESET (OTE is latched). Allow no more
  than 3 retries in 10 min, then stay off with the status LED flashing.
- **Contingency:** an NTC thermistor next to U15, footprint fitted and part
  populated (cost $0.02). Details:
  - Part: 10 k, B3435, 0603, LCSC C2889048 (CMFA103F3435, ±1 %).
  - Placement: within 3 mm of the GND pins 18-20, on the GND pour.
  - Circuit: a 10 k 0402 pull-up to +3V3A into a spare ESP32 ADC1 pin, or the
    TCA9555 cannot read analog, so it needs an ADC pin.
  - The NTC reads board temperature, about Tj - 3.5 W x 14.7 C/W (psiJB) ≈
    Tj - 50 K at 1/8 power. Thresholds: board **> 70 C**, -3 dB per 10 s;
    **> 85 C**, mute.

---
### 1.7 hwde's verify gate: "U15 has 9 thermal vias, wants ≥ 36 for 3 W". The check is the wrong tool for this part

`check_thermal` was re-run on 2026-10-29 on core with `core/kicad/constraints.json`:
- **Warning:** "U15 (3.00 W) needs a thermal-via array to GND (copper alone tops
  out ~51 C/W); found 9 via(s), want >= 36".
- **Passed:** the U15 rise itself is reported as 24 K, because constraints.json
  gives U15 `theta_datasheet = 8.0 C/W`.

Both halves are wrong for this part:

1. **There is no bottom pad for vias to drain.** On the DDV package the slug is
   on top (section 1.1). The only path into the board is through the 12 GND
   leads (RthetaJB = 14.9 C/W, psiJB = 14.7, SLASEE9C 6.4).
   - My count from the board file is **7 GND vias** (0.3 mm) inside U15's pin
     field and 16 GND vias within 12 mm. U16 has 5 inside.
   - In1.Cu GND covers 431 of 452 mm2 within 12 mm, and B.Cu 363 mm2.
   - Going from 9 to 36 vias only lowers the board-to-air part of the
     ~45-50 C/W board path. It cannot take it below RthetaJB = 14.9 C/W. My
     estimate is ~40 C/W at best.
   - That moves Tj at 3.5 W from ~220 C to ~185 C, which is still OTE. **More
     vias are not the fix. The top heatsink is.**
   - Extra GND stitching vias around the pin rows are still worth adding, at
     zero cost: every via near pins 5-7, 18-20, 25-26, 33-34 and 41-42 helps
     the parallel board path. With the heatsink, total thetaJA goes from
     ~16 to ~15 C/W.
2. **THERMAL-9 (minor): `theta_datasheet = 8.0 C/W` in constraints.json has no source.**
   - SLASEE9C gives 44.8 C/W with no heatsink.
   - 5.5 C/W applies only junction-to-heatsink with the heatsink held at
     85 C.
   - 8 C/W junction-to-ambient would need a ~2.5 C/W heatsink.
   - So the gate passes U15 on a number the board can't meet.
   - **Fix:** set it to 44.8 (no heatsink), or to ~16 once the section 1.5
     heatsink is fitted, and set `power_w` to 3.5 (4.5 W worst case).

`check_thermal`'s blind spot, for tools wanted: it has no notion of a
pad-up/top-slug package, so it treats every power IC as pad-down. It also
takes a `theta_datasheet` override without checking whether a heatsink is
assumed.

---

## 2. Lite: TPA3118D2 (U8, DAP pad-down)

### 2.1 Land, vias and copper (measured from `lite-main.kicad_pcb`)

- **Exposed-pad land:** pad 33, **3.81 x 3.71 mm**, on GND. The package pad
  is 2.46-3.04 x 3.16-3.74 mm (SLOS708G, DAP0032C), so the land covers it.
  TI's example land extends the metal to 5.2 x 11 mm under mask.
- **Paste:** 4 windows of 1.4 x 1.4 mm, 55 % of the land (OK).
- **Vias in the pad:** **16 in-pad vias, 0.3 mm drill / 0.61 mm pad,** in a
  4 x 4 grid at 0.85 mm pitch. They are footprint pads with `zone_connect 2`,
  i.e. solid, and they are not tented or plugged.
- **F.Cu GND pour:** connects solid to the EP and pins. Within 10 mm of the EP
  edge it covers 267 of 478 mm2 (56 %). The rest is pins and signal traces.
- **B.Cu GND pour:** covers 449 of 478 mm2 (94 %) within 10 mm, and
  432 of 452 mm2 within 12 mm. The pour as a whole is 7447 mm2. The B.Cu zone
  uses thermal-relief connect, but the in-pad vias override it to solid.
- **Stackup:** 1 oz / 1.6 mm, 2 layers, HASL.

**My thetaJA estimate:**
- **Via array:** one 0.3 mm via with ~20 um plating is
  1.6e-3 / (385 x 2.0e-8 m2) ≈ 207 C/W, so 16 in parallel ≈ **13 C/W**.
  Solder wicking lowers it.
- **Spreading** in 2 x 35 um copper (k·t = 0.027 W/K) with h ≈ 8 W/m2K on
  both faces: fin parameter m = 24 /m, so the radial-fin resistance from an
  r0 = 4 mm source ≈ **15 C/W**.
- **Board to air:** about 10 C/W overall (90 x 100 mm, 2 faces, h ≈ 5-6 in
  the box).
- **Result:** with top and bottom paths in parallel, thetaJA ≈ **25-30 C/W**
  to box air. I use 28 C/W.
- **TI anchor:** 22 C/W on the TPA3118D2EVM (SLOS708G 6.4 note 3). That board
  is **2-layer**: SLOU342A section 3.2 shows a top and a bottom layer only.
  Its bottom is a near-solid GND plane, and the via array is about 32 vias,
  twice ours. It runs in open air.

**hwde `check_thermal` (2026-10-29)** says U8 at 2.0 W has theta 47.5 C/W, a
95 K rise, and flags an error.
- That is ~1.7-2x pessimistic against TI's measured 22 C/W on a comparable
  2-layer board.
- Its own docstring concedes this ("~54 C/W ... against TI's 22 C/W
  measured"). It ignores solder fill in the vias, FR4 conduction, and lateral
  spreading in a near-solid back pour.
- Its 2.0 W input is also above the realistic 1/8-power load (1.75 W, below).

### 2.2 Dissipation (20 V)

- **Power ratings:** SLOS708G fig 14 gives P1% ≈ 38 W/ch (4 ohm) and
  P10% ≈ 46 W/ch at 20 V. Fig 13 gives P1% ≈ 23 W/ch into 8 ohm.
- **Efficiency:** fig 16 (24 V, 4 ohm) reads ~80 % at 5 W/ch, ~85.5 % at
  18 W/ch and ~88 % at 38 W/ch. At 20 V the switching loss is lower; I use
  +2 points at low power.
- **Idle:** ICC = 32 mA typ at 24 V, no filter (SLOS708G 6.5). With the
  10 uH/1 uF filter in BD mode I take ~30 mA at 20 V.

| Case | Output 2 ch | System loss | Inductor DCR (4 x 30 mohm) | Chip | Tj at 45 C box air, 28 C/W | Margin to 150 C trip |
|---|---|---|---|---|---|---|
| Idle | 0 | 0.6 W | 0 | **0.6 W** | 62 C | 88 K |
| 1/8 P, 4 ohm | 9.5 W | 2.1 W (eta 82 %) | 0.14 W | **1.75 W** | **94 C** (upper: 1.75 W x 35 C/W, 106 C) | **56 K** (44 K) |
| 1/8 P, 8 ohm | 5.8 W | 1.0 W | 0.05 W | **0.85 W** | 69 C | 81 K |
| Full, 4 ohm, 1 % both ch | 76 W | 10.4 W (eta 88 %) | 1.14 W | **~9 W** | ~300 C: **trips** | - |
| Full, 8 ohm | 46 W | 4.5 W | 0.3 W | **~4.2 W** | 163 C: **trips** | - |

- **Verdict:** 1/8 power passes into both loads.
- **Continuous full power trips the over-temperature protection.**
  - SLOS708G table "Over Temperature, Tj > 150 C ... Latched": FAULTZ goes
    low, the outputs go Hi-Z, and firmware must cycle SDZ.
  - The time to trip is set by the local copper mass: tens of seconds to a
    couple of minutes.
- **Highest sustainable chip loss** for Tj ≤ 135 C (15 K under the trip) at
  45 C air: 90/28 = **3.2 W**. That is about 2 x 13 W continuous sine into
  4 ohm, or about 2 x 20 W into 8 ohm.
- **Supply:** full power at 4 ohm draws 86 W, 4.3 A at 20 V. A 65 W charger
  can't supply it, so the power review's governor limit is lower anyway.

**THERMAL-3 (major, Lite):** there is no thermal telemetry and no governor
input except FAULTZ.
- The TPA3118 has no OTW pin, and the Lite has no current shunt.
- A sustained loud sine or a heavily clipped track latches the amp off,
  silently from the user's point of view.
- **Fix:**
  - **Firmware:** an output-RMS governor in the DSP path, since firmware owns
    the volume and the samples. Limit the 30 s RMS of each channel to
    ≤ 13 W-equivalent at 4 ohm (≤ 20 W at 8 ohm). That is -4.6 dB below the
    1 % full-scale for 4 ohm.
  - **Firmware:** on FAULTZ low, mute, then pulse SDZ low for ≥ 100 ms.
  - **Contingency on the board:** an **NTC 10 k B3435 0603, LCSC C2889048**.
    Place it within 3 mm of U8's GND pins 13-15 on the F.Cu GND pour. Feed it
    from a 10 k 0603 pull-up to +3V3A, into a free ESP32 ADC1 pin.
  - **NTC thresholds:** the board reads about Tj - 1.75 W x 4.7 C/W (psiJB)
    ≈ Tj - 8 K. Board **> 100 C**: -3 dB per 10 s. **> 120 C**: mute.

**THERMAL-4 (minor, Lite):** only 16 in-pad vias, against about 32 on TI's
EVM.
- **Fix:** add a second ring of 8-12 GND vias (0.3 mm) in the F.Cu GND copper
  just outside the EP, between the GND pins, plus stitching vias at the ends
  of the package.
- **Gain:** about -2 to -3 C/W, i.e. -4 to -5 K at 1/8 power. Cost is zero.
- **In-pad vias:** they are open 0.3 mm vias in the pad. Ask JLC to tent them
  on the bottom side (via-in-pad "tented") so solder doesn't wick out of the
  EP joint. Voids in the EP cost thermal resistance; that is for the DFM
  reviewer.

---
## 3. Regulators, PVDD switch, shunt, TVS

Air temperatures used here: Lite box air 52 C at 1/8 power in a 30 C room
(section 6). Core box air 55 C, since the enclosure is not yet designed.

| Part | Operating point | Loss | thetaJA used | Tj | Limit, margin |
|---|---|---|---|---|---|
| Lite U2 TPS54302, 20 V to 5.18 V, 400 kHz | 0.2 A avg, 0.4 A Wi-Fi TX peaks | 0.12 W avg / 0.2 W peak (switching 0.5·Vin·I·20 ns·f + Coss + Eco-mode Iq; eta ≈ 80 %) | 118.9 C/W (TI SLVSDG6C 5.4, DDC) | 66 C / 76 C | 125 C operating, **49 K** |
| Lite U3 SY8089A1, 5 V to 3.3 V | 0.15 A avg, 0.4 A peak | 0.06 W / 0.15 W (eta ≈ 90 %) | 120 C/W (Silergy SY8089 DS note 2) | 59 C / 70 C | 125 C recommended, **55 K** |
| Lite U4 LP5907-3.3 (ES8388 analog) | ~25 mA (unverified) | (5.18-3.3) x 0.025 = **0.047 W** | 193.4 C/W (TI SNVS798Q 5.4, DBV) | 61 C | 125 C, **64 K**. At its 250 mA rating it would be 0.47 W and 143 C, so do not hang more load on it. |
| core U4 SY8303AIC, 28 V to 5.1 V, 1.05 MHz | 0.35 A avg, 0.6 A peak (TPA3221 GVDD+AVDD 26 mA each, ESP32, LAN8720, ES9018, OPA1678, PCM5102A, ES7243E; Ultra adds the FFC/LCD) | conduction I²(0.11·D + 0.07·(1-D)) = 0.01 W, switching 0.5·28·0.35·20 ns·1.05 MHz = 0.10 W, Coss 0.5·200 pF·28²·f = 0.08 W, Iq 0.03 W, total **0.22 W / 0.33 W** | 60.2 C/W (Silergy SY8303 DS note 2, large copper), I use 80 C/W | 73 C / 81 C | 150 C abs Tj, 105 C ambient rating, **69 K** |
| core U5 SY8089A1 | 0.2 A avg | 0.08 W | 120 C/W | 65 C | **60 K** |
| core U6 LP5907-3.3 (+3V3A: ES9018K2M AVCC, ES7243E) | ~50 mA (unverified) | (5.1-3.3) x 0.05 = **0.09 W** | 193 C/W | 72 C | **53 K**. Above 120 mA it would cross 100 C: check the converters' AVCC sum. |
| Lite Q1 AO4485 (UMW), VGS = -20 x 100k/147k = **-13.6 V** | full power 4 ohm: (76 + 10.4 + 1.5)/20 = **4.4 A**; 1/8 P: 0.6 A | RDS(on) 12.5 mohm max at 10 V and 25 C (C5125949), x1.3 at 75 C = 16 mohm, so 4.4² x 0.016 = **0.31 W**; 1/8 P: 6 mW | drain copper: F.Cu PVDD 21 mm2 + B.Cu PVDD 593 mm2 through 6 vias, so ~70 C/W. AOS AO4485 is 75 C/W steady state on 1 in2 (AO4485 DS RthetaJA note A). | 74 C | 150 C, **76 K** |
| core Q1 AO4485 (AOS), VGS = -28 x 100k/200k = **-14 V** (D4 15 V clamp) | supply-limited 140 W / 28 V = **5.0 A**; 1/8 P: ~1.0 A | 23 mohm max at TJ 125 C, VGS -10 V (AOS DS); 19 mohm at ~90 C, so 25 x 0.019 = **0.48 W** | drain copper: **no PVDD copper on F.Cu** (0 mm2 in an 8 mm radius); only an In2.Cu PVDD plane (2811 mm2) through **4 vias**. ~90 C/W | 98 C | 150 C, **52 K**. Pass, but see THERMAL-7. |
| core R1 10 mohm 2512, 3 W (TA-I RLP25) | 5.0 A max; 1 A at 1/8 P | **0.25 W** (8 % of rating); 10 mW | ~40 C/W on its pads | +10 K | Pass. Derating starts at 70 C typically: 0.25 W is far below even a 50 % derated rating. |
| Lite D1 SMBJ20A standby | VBUS 20 V nominal, up to 21 V (PD +5 %) | IR 1 uA at VRWM 20 V and 25 C. At 21 V (> VRWM, 95 % of VBR min 22.2 V) and 60 C leakage can reach ~0.1-1 mA: **≤ 21 mW** | - | negligible | Thermal pass. That 21 V exceeds VRWM is a rating question (power/ratings review). |
| core D1 SMBJ30A standby | 28 V, ≤ 29.4 V | ≤ 1 uA nominal, ≤ 50 uA hot: **≤ 1.5 mW** | - | negligible | Pass, VRWM 30 V > 29.4 V |

**THERMAL-7 (minor, core):** Q1's drain has no top copper. All its heat and
its 5 A go through 4 vias into In2.Cu, whose plane is 0.5 oz (0.0152 mm).
- Tj ≈ 98 C at the 5 A supply limit with 55 C air. That passes with 52 K
  margin.
- **Fix, at zero cost:** add a ≥ 100 mm2 F.Cu PVDD pour on drain pins 5-8,
  plus 4-6 more vias into In2. That takes thetaJA to ~65 C/W and Tj to
  ~86 C.

---

## 4. Output inductors

| Ref | Part | DCR | Irms rating (dT ≈ 40 K) / Isat | 1/8 P 4 ohm | Full power 4 ohm | Peak current at full power |
|---|---|---|---|---|---|---|
| Lite L1-L4 | XR0850-100M (C48391614) | 30 mohm | 6.5 A (XR DS note 3: dT ≈ 40 C) / 8 A | Irms 1.09 A, 36 mW each, dT ≈ 1 K | Irms 3.08 A, **0.29 W each**, dT ≈ 40 x (3.08/6.5)² = **9 K** | 17.4 V / 4 ohm = 4.4 A, under 8 A Isat (**45 % margin**) |
| core L1-L4 (L5-L8 Ultra) | PROD PSPMAA0805-100M-ANP (C2962885) | 30 mohm | 6 A (PSPMAA DS: Irms at dT ≈ 40 C) / 10 A | Irms 1.55 A, 72 mW each, 3 K | Irms 4.39 A, **0.58 W each**, dT ≈ 40 x (4.39/6)² = **21 K** | 24.8/4 = 6.2 A + 0.6 A ripple = 6.8 A, under 10 A Isat (**32 % margin**). The TPA3221 OC limit is 10 A, the same as Isat. |

**Core loss** is not given in either datasheet.
- The TPA3221 runs AD mode at 600 kHz from 28 V, so each inductor's idle
  ripple is 28 x 0.25 / (10 uH x 600 kHz) = **1.17 A p-p**.
- For an 8 x 8 mm ferrite part at 600 kHz, ~0.2-0.4 W each is typical
  (unverified). That makes **~1-1.5 W of idle heat in the four core
  inductors, more than the TPA3221 itself** (0.45 W), and it is there 24/7
  whenever PVDD is up.
- The Lite in BD mode has small idle ripple, so its core loss is much lower.
- **Contingency (note):** set HEAD = 1 on the core. That cuts idle PVDD
  current from 11 mA to 8 mA (fig 6-16) and the duty-cycle ripple at idle,
  so it reduces the inductor core loss. It needs HEAD tied to AVDD through a
  0R/DNP pair instead of hard to GND. Firmware should also put the amp in
  RESET when no source has played for > 10 min.
- Part temperature in full-power bursts is 55 C box + 21 K DCR + core loss,
  about 85 C. That is under the parts' 125 C limit (XR note 5), **40 K
  margin**.

---

## 5. Bulk capacitors: ripple current and life

PVDD ripple current at the class-D switching frequency is roughly
Iout,rms x 0.5 per BTL channel at full swing (the buck-input form
I·sqrt(D(1-D))). It is shared by the ceramics and the bulk capacitors in
inverse proportion to their impedance.

**Lite** (C58 polymer, C59 electrolytic, 400 kHz):
- **C58, JIERR MA35V470M10X13 polymer:** 3.8 A at 100 kHz, 16 mohm.
  - Full power 4 ohm: about 2 x 1.54 A, partly cancelling, so ~2.0 A total.
    Nearly all of it goes into C58: the ceramics are 3 x 1 uF + 2 x 100 nF,
    about 1.5 uF after DC bias, 0.27 ohm at 400 kHz.
  - **2.0 A / 3.8 A = 53 %.** At 1/8 P, 0.7 A (18 %).
  - Life, polymer rule x10 per 20 K: at 60 C, 2000 h x 10^(45/20) =
    **356,000 h**. Pass.
- **C59, KNSCHA RVT 220 uF/35 V:** 195 mA at 120 Hz, ESR not given.
  - With an ESR of ~0.3-0.5 ohm it takes ~4 % of the ripple, ~0.08 A. That is
    41 % of the 120 Hz rating, before any frequency multiplier credit.
  - Life at 60 C: 2000 x 2^((105-60)/10) = **45,000 h ≈ 5 years** running
    loud 24/7. At idle-on (45 C local): 128,000 h. **Note.**
- **Voltage:** 35 V parts on 21 V max is 60 %. Pass.

**Core** (C5, C6, and C311 on Ultra; Rubycon 50PX470MEFC10X20,
general-purpose, 600 mA at 120 Hz, 2000 h at 105 C):
- **Ripple rating:** the PX multiplier for 100-1000 uF at ≥ 10 kHz is
  **1.20** (Rubycon PX DS), so **0.72 A at 600 kHz**.
- **ESR:** from tan d 0.10 (50 V) it is 0.28 ohm at 120 Hz. I take ~0.1 ohm
  at 100 kHz+.
- **Impedance split:** the core has **no polymer and no extra ceramic bulk**.
  Per TPA3221 there are 2 x (1 uF/50 V 0805 + 100 nF), about 1 uF after DC
  bias at 28 V, 0.27 ohm at 600 kHz. C7 10 uF/50 V 1206 sits at the input.
  C5/C6 are ~16 mm from U15.
- **Ripple current, per cap:**
  - **1/8 P 4 ohm:** ~1.1 A total, so **~0.55 A per cap = 76 % of rating**.
    Self-heating 0.55² x 0.1 = 30 mW, ~4 K.
  - **Full power 4 ohm:** ~2.5 A total, so **~1.25 A per cap = 174 % of
    rating**. Self-heating 0.16 W x ~127 C/W for a 10 x 20 can
    (7.9 cm2 at h = 10) = **+20 K**.
- **Life:** at 55 C air + 4 K, 2000 x 2^4.6 = **48,000 h ≈ 5.5 years** of
  loud playing. In sustained full-power bursts, life is halved per 10 K.
- **Ultra:** C311 adds a third cap, but zone B doubles the ripple. The
  per-cap figures stay about the same.

**THERMAL-5 (minor, core):** C5/C6 at 76 % of their ripple rating at 1/8
power, and 174 % at full power. Both are general-purpose electrolytics with no
high-frequency bulk near the amp.
- **Fix:** add 2 x 10 uF/50 V X7R 1210 per TPA3221, one at each PVDD pin
  group (29-31, 36-38). Suggested MPN: Samsung CL32B106KBJNNNE.
  - **The LCSC number is unverified.** The worker should pick one with a
    50 V rating and 1210 size.
  - These take ~40 % of the 600 kHz ripple.
- **Or (better):** change C5/C6 to a low-impedance 50 V series with
  ≥ 1.5 A at 100 kHz, e.g. Rubycon ZLH 50 V 470 uF 10 x 20, with the LCSC
  number to be verified. The footprint is unchanged.

---
## 6. Enclosure: total heat, air temperature, plastic

### 6.1 Lite (enclosure/lite.py: 106 x 96 x 46.8 mm, PETG/ASA per its README)

**Heat budget, W:**

| Item | Idle (PVDD on, Wi-Fi) | 1/8 P, 4 ohm | Governed max (chip 3.2 W) | Full 4 ohm (until trip) |
|---|---|---|---|---|
| TPA3118 + inductors | 0.65 | 2.1 | 3.8 | 10.4 |
| ESP32-WROVER (Wi-Fi streaming ~120 mA at 3.3 V) | 0.4 | 0.5 | 0.5 | 0.5 |
| Bucks + LDO | 0.23 | 0.25 | 0.3 | 0.3 |
| ES8388, CH340C, IR, LED, dividers, R16 bleeder (20²/22k = 18 mW) | 0.15 | 0.15 | 0.15 | 0.15 |
| Q1 | 0 | 0.01 | 0.05 | 0.31 |
| **Total** | **~1.9** | **~3.5** | **~5.1** | **~11.7** |

**Box conductance to the room:**
- **Walls:** surface 2(106·96 + 106·46.8 + 96·46.8) = 39,300 mm2 =
  0.0393 m2. Use U ≈ 3.5-4 W/m2K in series: inside ~5 W/m2K
  convection + radiation, 2.4 mm PETG (k 0.2) = 83 W/m2K, outside ~10 W/m2K.
  Take off part of the base resting on a table. Result: about **0.13 W/K**.
- **Vents:** 8 + 8 slots of 2 x 26 mm (416 mm2 each) in floor and lid, both
  under/over U8.
- **Edge-gap flow:** the board fills the box: 0.6 mm from each wall
  (lite.py `GAP`). So the floor vents feed only the 5 mm space under the
  board, and the only through-path to the lid is the edge gap,
  2 x (100 + 90) x 0.6 = 228 mm2. Stack flow through it (H ≈ 40 mm,
  dT 15 K):
  Q = 0.6 x 228e-6 x sqrt(2g·H·dT/T) = 2.6e-5 m3/s, i.e.
  **~0.03 W/K**.
- **Total** G ≈ **0.16 W/K**.

**Internal air rise:**

| Case | Rise | Air, 25 C room | Air, 30 C room |
|---|---|---|---|
| Idle | 12 K | 37 C | 42 C |
| 1/8 P, 4 ohm | 22 K | 47 C | **52 C** |
| Governed max | 32 K | 57 C | **62 C** |
| Full 4 ohm steady (not reachable: amp trips) | 73 K | - | - |

The brief's "35-40 C in the box" is right only for idle. **At 1/8 power the
box is ~47-52 C.** The Tj figures in section 2 use 45 C, and they stay inside
their margins at 52 C too: U8 is then 101 C, 49 K under its trip.

**THERMAL-6 (minor, Lite enclosure):** the floor vents are blocked by the
board.
- The board covers the whole floor (0.6 mm edge gap), so floor-to-lid
  chimney flow is ~0.03 W/K. The floor slots under U8 do almost nothing.
- **Fix, enclosure only (no board change):** widen `GAP` at the left and
  right walls to ≥ 3 mm (the box grows 5 mm), giving 540 mm2 of riser. Or add
  10 x 2 mm slots high on the rear wall plus low on the front wall.
- **Effect:** either roughly doubles the vent conductance, to ~0.2 W/K, so
  1/8-power air drops ~5 K.

**Plastic:**
- At governed maximum the air is 57-62 C, and the lid just above the U8 vent
  plume a few K more.
- **PLA (softens 55-60 C) would creep there.** PETG (Tg ~80 C) has ≥ 18 K
  margin. ASA (Tg ~100 C) has more.
- The enclosure README already says "PETG or ASA, not PLA". Keep that, and
  put it on the product's build sheet.
- The binding-post nuts and the M3 screws threaded into PETG are under clamp
  load near 60 C, so PETG creep is a slow-loosening risk. Note only.

### 6.2 Core (Plus/Ultra): enclosure not designed yet

**Heat at 1/8 P into 4 ohm:**
- **Plus:** TPA3221 system 5.2 W + inductor core loss ~1 W + electronics
  ~1.5 W = **~7.7 W**. The electronics are ESP32 0.5, LAN8720 ~0.25,
  ES9018K2M ~0.15, PCM5102A ~0.08, ES7243E/OPA1678/TCA9555/CH340C ~0.15,
  SY8303 0.22, SY8089 0.08, LP5907 0.09.
- **Ultra:** ~14 W, adding zone B and the LCD.
- **Idle:** Plus ~3.2 W, Ultra ~5 W. Inductor core loss in AD mode
  dominates.

**Requirement:** to hold the box air ≤ 15 K over the room at 1/8 power, the
enclosure needs G ≥ 0.5 W/K (Plus) or ≥ 0.95 W/K (Ultra).
- A closed 200 x 160 x 60 mm box gives ~0.43 W/K from its walls.
- So the core enclosure **must have through-flow vents**:
  - ≥ 1500 mm2 (Plus) or ≥ 2500 mm2 (Ultra) of inlet low on the walls or
    floor, beside the board and not under it.
  - The same area of outlet in the lid, directly over the U15/U16 heatsinks.
  - A clear vertical path past the board edge (≥ 3 mm gap, or vents outside
    the board outline).
- **Effect:** 1500 mm2 of chimney adds ~0.2 W/K, giving Plus about +13 K and
  Ultra about +23 K. The heatsink numbers in section 1 assume 45 C air, so
  **Ultra needs the larger vent** to stay within them.

---

## 7. ESP32 module and CH340C

- **ESP32-WROVER-E-N8R8:** -40 to +85 C ambient (LCSC C2973667 listing).
  Self-heating ~0.5 W in a metal-canned module adds ~10 K.
  - **Lite:** 1/8 power in a 30 C room gives 52 + 10 = 62 C, **23 K
    margin**. Governed maximum gives 72 C, 13 K margin.
  - **Core:** depends on the enclosure (6.2).
  - U5 (Lite) at (42, 128) and U7 (core) at (137, 201) both sit far from the
    amplifier (Lite U8 at (102, 105); core U15/U16 at y 162). Pass.
- **CH340C:** **-20 to +70 C operating** (WCH CH340 DS, "operating
  temperature" table: CH340C/N/K/E/B). Self-heating ~0.05 W, about +3 K.
  - **Lite:** 1/8 power in a 30 C room gives 55 C, **15 K margin**.
    Governed maximum gives 65 C, **5 K margin**.
  - **THERMAL-8 (minor, both):** a commercial-grade part in a warm box.
    **Fix/contingency:** accept it, since the margin is positive at the
    governor's limit. If the core enclosure runs hotter than 60 C, swap to
    CH343P (-40 to +85 C). Its pinout differs, so that is a board change.

---

## 8. Summary of margins (pass/fail at the standard 1/8 power, 4 ohm)

| Block | Board | Result at 1/8 P 4 ohm | Margin |
|---|---|---|---|
| TPA3221, no heatsink (as built) | core | Tj ~220 C → OTE latch | **-75 K (FAIL)** |
| TPA3221 + 19x12x25 adhesive-tape heatsink | core | 118 C (139 C upper) | -3 K to OTW min, 6-27 K to OTE min: **insufficient** |
| TPA3221 + spec heatsink (1.5) | core | 101-108 C | 7-14 K to OTW min |
| TPA3221 continuous full power | core | OTE in ~9 s | firmware governor required |
| TPA3118D2 | lite | 94 C (106 C upper) | 44-56 K to the 150 C trip |
| TPA3118D2 continuous full power | lite | trips | governor (THERMAL-3) |
| TPS54302 / SY8303 / SY8089 / LP5907 | both | 59-81 C | 49-69 K |
| AO4485 Q1 at the supply limit | lite / core | 74 / 98 C | 76 / 52 K |
| Shunt R1 | core | 0.25 W of 3 W | 92 % |
| Inductors | both | dT 9 K / 21 K at full power | Isat margin 45 % / 32 % |
| C58 polymer / C59 | lite | 53 % / ~41 % of rating at full power | life 356k h / 45k h |
| C5/C6 | core | 76 % of rating at 1/8 P, 174 % at full | life ~48k h |
| Box air | lite | 47-52 C | PETG 28 K, PLA ~5 K (do not use PLA) |
| CH340C | both | 55 C (lite) | 15 K (5 K governed) |
