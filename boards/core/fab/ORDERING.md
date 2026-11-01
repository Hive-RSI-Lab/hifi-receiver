# Ordering core (Plus and Ultra) at JLCPCB

Ordering is the person's: it spends money. These notes say what goes where,
and the one step that has to happen before the assembly order can go in.

## Before ordering

1. **Check the known unknowns:**
   - Plus: `python3 tools/known_unknowns.py --check --tier plus`
   - Ultra: `python3 tools/known_unknowns.py --check --tier ultra`

   Neither should list anything open.
2. **Check stock:** `python3 tools/bom_stock.py core` (Plus) and
   `python3 tools/bom_stock.py core/ultra` (Ultra). The step below shows up
   there as short.
3. **Pre-buy the TPA3220 (U15 and U16, on both Plus and Ultra) into your JLC parts
   inventory.** LCSC and JLC have none: C2879306 read 0 on 2026-10-29. The
   TPA3221 has its pad on top and needs a heatsink fitted by hand, so the
   board takes this pad-down twin instead.
   - Where to buy it: Mouser (595-TPA3220DDWR, 6,566 in stock, about
     $3.74 at 100) or DigiKey (296-51851-1-ND, 2,032 in stock). Both were
     checked through Octopart on 2026-10-29.
   - How it gets into the inventory, either way:
     - **JLC Global Sourcing** (Parts Manager → Order Parts → Global
       Sourcing): search TPA3220DDWR, buy the quantity, and JLC receives
       it into your inventory.
     - **Consigned parts:** buy from Mouser or DigiKey yourself and ship
       the reel or cut tape to JLC as consigned parts for your account.
   - Quantity: 2 per board (Plus fits zone B too), plus JLC's attrition
     (2-3 extra). `prebuy.csv` (Plus) and `ultra/prebuy.csv` (Ultra) list
     it with the build's quantity.
   - Wait until the parts show as **in stock in your inventory**. JLC's BOM
     review then matches U15/U16 to them. If you order before that, it
     shows "shortage" and leaves the amp unplaced.
4. **Buy the other Extended parts ahead** if bom_stock.py marks them thin.
   Some are idle stock that JLC's BOM review shows as unselected; the rest
   of `prebuy.csv` lists them.

## The upload

- **PCB:** `core_gerbers.zip`.
  - 4 layers, 1.6 mm, 1 oz outer and **1 oz inner copper. Change this on the
    quote page: JLC's default is 0.5 oz.** In2 carries the full 5 A alone
    (`kicad/gen/power_trunk.py`): 5 A needs 3.5 mm at 1 oz and 8.06 mm at
    0.5 oz, against a measured 3.88 mm neck. If the quote page has no 1 oz
    option, stop: don't order at 0.5 oz.
  - The stackup is in `gerbers/core-job.gbrjob`.
  - ENIG is the better finish: HASL is uneven on the 0.65 mm SOT-363 (Q3) and
    the 1006-size BAS16L (D5).
  - The TPA3220's thermal pad has 15 open 0.3 mm vias. No via filling is
    needed: the paste is cut to 63 % in 4 windows to allow for the solder
    that wicks into them.
- **Assembly, both sides** (choose JLC's two-sided PCBA). Fourteen parts
  are on the bottom (`Layer` = Bottom in the CPL): ten 2-pad MLCCs under the
  amps, each TPA3220's PVDD HF pair (C207-C210, C307-C310), right under its
  PVDD pins, and U15's input caps C218/C220; and four SOT-23 ESD clamps at
  their connectors, D6/D9 at the USB-C's CC pins and D7/D8 at the RCA jacks.
  Check their pads line up in JLC's bottom-side preview: hwde can't verify
  bottom-side rotations (`reports/dfm-waivers.json`). The MLCCs are non-polar,
  so only their axis matters; the clamps' pin 3 is their ground.
  - Plus: `BOM.csv` and `CPL.csv`. Zone B (U16) is fitted too: Plus plays L on zone A and R on zone B.
  - Ultra: `ultra/BOM.csv` and `ultra/CPL.csv`, with the same Gerbers.
- **Banana sockets J20-J27 (through-hole, fitted by JLC):** Plus fits J20, J21,
  J26, J27; Ultra fits all 8. In JLC's preview each collar must overhang the
  rear edge by 2.65 mm. The two pins are symmetric, so nothing else shows a
  socket turned back to front: it would sit over the snubber row.
- **Not on the board:** the remote, Ultra's front board
  and its FFC. They are in `docs/bom-estimate.csv`. No heatsink is needed.
