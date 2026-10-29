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

=> **CORE-THERM-1 (blocker): the TPA3221 has no heatsink. It will
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

### 1.5 Specification (fix for CORE-THERM-1)

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

### 1.6 Firmware power governor (needed with or without the heatsink)

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
