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

