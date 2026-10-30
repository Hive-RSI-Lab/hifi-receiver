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
3. **Pre-buy the TPA3220 (U15, and U16 on Ultra) into your JLC parts
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
   - Quantity: 1 per Plus board, 2 per Ultra board, plus JLC's attrition
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
  - 4 layers, 1.6 mm, 1 oz outer and 0.5 oz inner copper.
  - The stackup is in `gerbers/core-job.gbrjob`.
  - ENIG is the better finish: HASL is uneven on the 0.65 mm SOT-363 (Q3) and
    the 1006-size BAS16L (D5).
  - The TPA3220's thermal pad has 15 open 0.3 mm vias. No via filling is
    needed: the paste is cut to 63 % in 4 windows to allow for the solder
    that wicks into them.
- **Assembly, top side only:**
  - Plus: `BOM.csv` and `CPL.csv`. U16 and zone B are not fitted.
  - Ultra: `ultra/BOM.csv` and `ultra/CPL.csv`, with the same Gerbers.
- **Not on the board:** the binding posts, the remote, Ultra's front board
  and its FFC. They are in `docs/bom-estimate.csv`. No heatsink is needed.
