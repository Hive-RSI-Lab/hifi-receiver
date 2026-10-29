# The check and the score

`.hive/project.json` gives this project a test and a score, so the Hive can
measure every merge here and revert one that makes things worse. Both are
`tools/hwcheck.py`:

```json
"test": "python3 tools/hwcheck.py test",
"score": {"command": "python3 tools/hwcheck.py score", "direction": "lower", "tolerance": 0.1,
          "paths": ["tools/hwcheck.py", "tools/bom_estimate.py"]}
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
   - a **Boards** row that names each tier's board(s) in backticks
   - a **Build order** row with exactly one tier marked `first`
2. **The software's unit tests.** For each folder in `hwcheck.py`'s
   `SOFTWARE` (today `bench`), `python3 -m unittest discover -s
   <dir>/tests`. These are stdlib only, need no board, and take about
   10 s. The bench's run against its mock board ([bench.md](bench.md)).
   Add the app's here the same way.
3. **The firmware's host tests.** `make -C firmware/test` (plain `cc` with
   ASan/UBSan, no ESP-IDF): the power sequencer, governor, PD decode, test
   tones, JSON and every test-API call and console command, against a mock
   board. Built in a scratch folder. Skipped with a note where there is no
   `make` or `cc`. The full ESP-IDF build is not part of the check: it needs
   ESP-IDF v4.4.8 and takes minutes ([firmware/README.md](../firmware/README.md)).
4. **Every board's hwde gates.** Every workspace under `boards/` (a folder
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

**The unit cost of the first-built tier, in USD, at 100 units, worked out
from its boards' real BOMs. Lower is better.** It counts only when the
gates pass.

1. `docs/product.md` gives the tier (the Build order row's `first`, Lite
   today) and its board(s) (the Boards row: `lite-main`).
2. The script runs the four gates on each of those boards, as the test does.
   **If any gate fails, the score prints no number**, so a cheaper board that
   breaks a gate never counts as a gain. (The test is red on it too, and
   the Hive measures tests first.)
3. Each board's cost per unit at 100 boards is the sum of:
   - **Parts.** For each line in `fab/BOM.csv`, its quantity times its unit
     price. The price comes from the `price_breaks` of the matching LCSC
     number in `parts/parts.json`, at the highest break at or below the
     quantity a 100-board order buys. Prices are the ones committed with
     the board, so the score doesn't drift with LCSC's live prices. To
     refresh them, re-source the parts and commit `parts.json`.
   - **JLC assembly.** This uses the model and constants in
     `tools/bom_estimate.py`: setup $8 + stencil $8 + $3 per unique
     extended part (a part whose `basic` isn't true in `parts.json`), plus
     $0.0017 per SMT pad and $0.0173 per through-hole pad on the BOM's
     footprints, counted from the `.kicad_pcb`. The total is spread over
     100 boards.
   - **PCB.** This is `bom_estimate.py`'s price point for the board's copper
     layer count (2 or 4; any other count is an error), scaled by the
     Edge.Cuts outline's bounding box past 100 × 100 mm, times its 2.5
     calibration, and spread over 100 boards.
4. A tier with several boards sums them. The output gives the breakdown per
   board and the total against the tier's target retail price. The number
   alone is on the last line.

A BOM line with no price in `parts.json`, or a reference that isn't on the
board, is an error, and the score prints no number.

Off-board items (heatsink, remote, cable) and the charger aren't on a
board's BOM, so they aren't in the score. Lite has none. `product.md`'s
estimate counts them for Plus and Ultra.

**Before the first tier's board exists, the score is 1000** (`NO_BOARD`).
That is above any real unit cost, so the first board that passes its gates
reads as a gain and becomes the baseline. The estimate in `bom-estimate.csv`
($12.26 for Lite) isn't used as a stand-in, for two reasons. It isn't the
board's BOM. And a first board that came in above the estimate would then
read as a loss and be reverted. If some of the tier's boards exist but not
all of them, the score prints no number.

**Tolerance 0.1 USD**, about 1% of Lite's estimate. The cost comes only from
committed files, so it's the same on every run. A change that costs
10 cents or less per unit (a fix's extra capacitor) isn't reverted as a loss.

**Evaluator paths:** `tools/hwcheck.py` and `tools/bom_estimate.py`. Changing
either one is the person's to merge, and it starts a new baseline.
`docs/product.md` isn't listed, because it changes with most tasks. A change
to its Build order or Boards row changes what the score measures, so review
reads such a change as touching the score.
