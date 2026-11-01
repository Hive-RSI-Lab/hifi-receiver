# ultra-front: ESD clamps at the user-touched switches (review round 4, R5-UF-2)

**Finding** (`docs/review-5/uf.md`, R5-UF-2). The TS-1187A-B-A-B buttons have a
**brass** keystake. An 8 kV IEC air strike creeps about 1 mm per kV along
seams. The path from the panel face to the switch top, past the printed cap,
is about 5.8 mm, so a strike can reach the switch metal. BTN1-3 ran from the
switch to U1 (TCA9535, about 2 kV HBM) with only a 10 k pull-up: no series R,
no capacitor, no TVS.

**Fix: two PESD5V0S2BT** (Nexperia, LCSC C49338, SOT-23; already on core as
D7/D8). Each is a dual bidirectional clamp, 5 V working, so the 3.3 V lines sit
well inside it. That's $0.086 each at 10 boards, one new extended line
(7 in all).

| Ref | Pin 1 | Pin 2 | Where | Line track from the switch pad | Ground |
|---|---|---|---|---|---|
| D2 | BTN2 | BTN1 | gap between SW1 and SW2 (74.0, 40.8) | BTN2: 4.0 mm F.Cu, no via. BTN1: 8 mm, 2 vias (6 mm on B.Cu) | own via, 0.4 mm track |
| D3 | BTN3 | ENC_SW | gap between SW2 and SW3 (74.0, 53.6) | BTN3: 3.2 mm F.Cu, no via. ENC_SW: 19 mm F.Cu from SW4 pin D | own via, 0.4 mm track |

- **Why the clamps aren't next to every pad:** each TS-1187A has its BTN
  pins on top and its GND pins below. The gaps between the switches are the
  only free F.Cu near them. The 12 mm switch pitch means one channel of a dual
  clamp is always the far one.
- **Why BTN1 goes down through vias:** SW1's GND pad row (with its track),
  and ENC_A/ENC_B, cross under SW1 on F.Cu. BTN1 drops to B.Cu at SW1's pin-2
  run (split there, so the stub joins at a track end), and comes back up
  beside D2.
- **Why ENC_SW has the long channel:** the knob's frame (F/G) is GND on
  both layers, so a strike on the knob lands on GND. ENC_SW's push contacts
  are inside the encoder. It takes the spare channel, routed on F.Cu from pin
  D under the knob's pin row (0.4 mm clear of pin E), then up to D3.
- **Pairing:** as the review proposed (BTN1/BTN2 and BTN3/ENC_SW). Pin 1 of
  each is the channel that drops straight from the switch below, so its stub
  stays short.

**Not done here:** the review's alternatives (1 nF X7R per line, or a plastic
keystake) and the gun test. The gun test is in the README's bring-up list
(step 7): 8 kV air on each cap and on the knob.

**How it was done** (from `boards/ultra-front`, starting from the board on
h2/review-5):

1. Copy the PESD5V0S2BT,215 symbol and the
   `SOT-23_L2.9-W1.3-P1.90-LS2.4-BR` footprint from core's `lib/` into this
   board's `lib/`, and core's `parts/easyeda/C49338.json`.
2. Add D2/D3 in `kicad/gen/root.py`, and run it (README, "How to rebuild").
   Keep `kicad/ultra-front.kicad_pro` as it was. Export
   `kicad/ultra-front.net` with `kicad-cli sch export netlist`. The netlist
   differs from the old one only by D2/D3's six pins.
3. `hwde parts_search --query C49338 --any-stock --out parts/search/C49338.json`,
   add the line in `kicad/gen/parts_build.py`, and run it.
4. `hwde board_update --pcb kicad/ultra-front.kicad_pcb --netlist
   kicad/ultra-front.net --placements kicad/esd-placements.json --state
   state.json`.
5. Under KiCad's python, run `kicad/gen/esd_eco.py kicad/ultra-front.kicad_pcb`
   (stubs, two BTN1 vias, two ground vias, zone refill).
6. Run `fab_export` and `bom_cpl --build-qty 10` into `fab/`.

KiCad DRC with schematic parity is clean afterwards: 0 violations (warnings
included), 0 unconnected, 0 parity. `tools/hwcheck.py test`: ultra-front's erc,
drc_routed, verify and dfm all pass.

**Tool drift in the regenerated fab:** the current hive-hwde's `bom_cpl`
marks SW4 (EC11) `tht` in BOM-full and the distributor BOMs, where the old
run said `smt_placed`. That is correct: the README already says SW4 goes
through JLC's THT service.
