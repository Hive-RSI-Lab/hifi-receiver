# The check and the score

`.hive/project.json` gives this project a test and a score, so the Hive can
measure every merge here and revert one that makes things worse. Both are
`tools/hwcheck.py`:

```json
"test": "python3 tools/hwcheck.py test",
"score": {"command": "python3 tools/hwcheck.py score", "direction": "lower", "tolerance": 0.1,
          "paths": ["tools/hwcheck.py", "tools/bom_estimate.py", "tools/product.json"]}
```

Run either one by hand from the repo root. `python3 tools/hwcheck.py cost
[BOARD ...]` prints the cost breakdown on its own, without running the gates.

## The test

It fails (exit 1) if any of these fails:

1. **`docs/product.md`.** The tier table (its header row names Lite, Plus
   and Ultra) has to have:
   - a **Target retail** price for every tier
   - a **Unit cost at 100** for every tier, matching
     `tools/bom_estimate.py --unit <tier> 100` to within 5 cents (the page
     rounds to the dime), so the page can't drift from `bom-estimate.csv`
   - a **Boards** row that names each tier's board(s) in backticks: the
     same boards `tools/product.json` counts for that tier
   - a **Build order** row with exactly one tier marked `first`
2. **The software's unit tests.** For each folder in `hwcheck.py`'s
   `SOFTWARE` (`tools`, `bench`, `app`) that has a `tests/` folder,
   `python3 -m unittest discover -s <dir>/tests`. These are stdlib only,
   need no board, and take about 10 s. `tools/tests` tests the score's own
   logic, and the bench's run against its mock board ([bench.md](bench.md)).
   When `firmware/test/Makefile` exists, the firmware's host tests run too:
   `make -C firmware/test BIN=<scratch>`, with make and cc, no ESP-IDF.
3. **Every board's hwde gates.** Every workspace under `boards/` (a folder
   with a `state.json`) runs all four of hwde's gates: `erc` on
   `kicad/<board>.kicad_sch`, then `drc_routed`, `verify` and `dfm` on
   `kicad/<board>.kicad_pcb`. `<board>` is the `board` field in `state.json`.
   Each gate is `hwde gate --gate <g> <input> --workspace .`, run in a
   scratch copy of the workspace, so the checkout isn't changed. A board
   merges only once it passes all four. A half-built board stays on its
   branch.

When there are no boards yet, the test checks `docs/product.md` and passes.

The gates run through hive-hwde's `bin/hwde`. The script looks for it in
this order: `$HWDE_BIN`, `hwde` on `PATH`, then
`$HIVE_MODULES/hive-hwde/bin/hwde` (with `/hive/modules` used when
`HIVE_MODULES` is unset). If there are boards to check and `hwde` isn't
found, the script runs `sh -c hwde`. The shell prints its own
`hwde: not found` and exits 127. The Hive reads that as "couldn't run": a
fault in the environment, not a verdict on the commit.

## The score

**The whole product, in USD, lower is better.** The person asked for the
whole product (`.hive/goals.md`, 10-28 16:55Z): every tier's board, plus the
firmware, the app and the enclosures. So the score is the sum of two kinds
of term. Each tier counts at its unit cost once its boards are done. Each
piece costs a flat amount until it is there and its tests pass. What counts
is in [`tools/product.json`](../tools/product.json). That file is an
evaluator file, like `hwcheck.py`.

1. **Each tier (Lite, Plus, Ultra): its unit cost at 100.** It counts only
   when all of these hold:
   - every board the tier is built from is under `boards/`. Lite is
     `lite-main`. Plus is `core` with its Plus population (`fab/BOM.csv`,
     `parts/parts.json`). Ultra is `core` with its Ultra population
     (`fab/ultra/BOM.csv`, `parts/parts-ultra.json`) plus `ultra-front`.
   - every one of those boards passes all four hwde gates, as in the test.
   - every BOM line has a price.
   - **the tier's boards carry every function `product.json` lists for
     it.** These are what `product.md` promises for the tier: the USB-C PD
     input, the amplifier(s), the DAC(s), the line-in path, Wi-Fi/BT,
     Ethernet on Plus and Ultra, the RCA jacks, the speaker contacts, the IR
     receiver, the knob and buttons, the screen connector on Ultra, and so
     on. Each function is a regular expression over the BOM line's comment
     and its part's MPN, value and LCSC description (or its `role`, where
     `in` says so). The matching lines' parts are counted (or their pads,
     for speaker contacts) against the number needed. A regular expression
     names every part that does the job, so a cheaper part that still does
     the job still counts.

   A tier that falls short of any of these costs a flat **1000**
   (`tier_not_counted`), and the score says why. 1000 is far above any
   tier's real cost, so it works both ways. A board can't lower the score
   by dropping a function the product needs, because the drop costs far
   more than the part saves. And a tier landing for the first time always
   reads as a gain.

   The cost of a counted tier is the sum of its boards' costs. Each
   board's cost per unit at 100 boards is the sum of:
   - **Parts.** For each line in the population's BOM, its quantity times
     its unit price. The price comes from the `price_breaks` of the matching
     LCSC number in the population's parts file, at the highest break at or
     below the quantity a 100-board order buys. Prices are the ones
     committed with the board, so the score doesn't drift with LCSC's live
     prices. `python3 tools/price_parts.py <parts.json> ...` fills in the
     missing ones from a live search (hwde's `parts_search`), and
     `--refresh` re-prices them all. Commit the result.
   - **JLC assembly.** This uses the model and constants in
     `tools/bom_estimate.py`: setup $8 + stencil $8 + $3 per unique
     extended part (a part whose `basic` isn't true in the parts file), plus
     $0.0017 per SMT pad and $0.0173 per through-hole pad on the BOM's
     footprints, counted from the `.kicad_pcb`. The total is spread over
     100 boards.
   - **PCB.** This is `bom_estimate.py`'s price point for the board's copper
     layer count (2 or 4; any other count is an error), scaled by the
     Edge.Cuts outline's bounding box past 100 × 100 mm, times its 2.5
     calibration, and spread over 100 boards.

   Off-board items (heatsinks, the remote, the LCD panel, binding posts) and
   the charger aren't on a board's BOM, so they aren't in the score.
   `product.md`'s estimate counts them.

2. **Each piece: 0 once it is there and passes its tests, else 100**
   (`piece_missing`). A piece is there when each of its `needs` globs
   matches a file. Its `test` is one of three kinds:
   - `firmware`: the host tests, as in the test above
   - `unittest <dir>`: that stdlib suite
   - `fit <fit.md>`: hive-mech's fit report. It has at least one `pass`
     row and no `fail` row.

   The pieces are:
   - the firmware for Lite, Plus and Ultra (`firmware/test/Makefile` plus
     `firmware/board/<board>/sdkconfig.defaults`)
   - the web app (`app/web/` with its manifest, and `app/tests`)
   - the bench scripts
   - the enclosure for each tier (`enclosure/out/<tier>/` with STL, STEP
     and `fit.md`)
   - the user manual and quick start

The output gives each tier's cost or why it doesn't count, and each piece's
state. The number alone is on the last line.

**If a gate couldn't run** (hwde exits 2 or more: a broken tool, not a
verdict), the score prints no number and exits 1. If hwde, make or cc
isn't there at all, it exits 127, as the test does. Neither one is a
verdict on the commit. A gate that runs and fails (exit 1) is a verdict:
that tier doesn't count.

**Tolerance 0.1 USD**, about 1% of Lite's estimate. The cost comes only from
committed files, so it's the same on every run. A change that costs
10 cents or less per unit (a fix's extra capacitor) isn't reverted as a loss.

**Evaluator paths:** `tools/hwcheck.py`, `tools/bom_estimate.py` and
`tools/product.json`. Changing any of them is the person's to merge, and it
starts a new baseline. Adding a tier's function or a piece goes in
`product.json`.
`docs/product.md` isn't listed, because it changes with most tasks. The
score no longer reads it. The test holds its Boards row to `product.json`.
