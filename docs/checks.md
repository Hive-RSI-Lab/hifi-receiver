# The check and the score

`.hive/project.json` gives this project a test and a score, so the Hive can
measure every merge here and revert one that makes things worse. Both are
`tools/hwcheck.py`:

```json
"test": "python3 tools/hwcheck.py test",
"score": {"command": "python3 tools/hwcheck.py score", "direction": "lower", "tolerance": 0.1,
          "paths": ["tools/hwcheck.py", "tools/pieces.py", "tools/bom_estimate.py", "tools/product.json"]}
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
3. **The firmware's host tests.** `make -C firmware/test` (plain `cc` with
   ASan/UBSan, no ESP-IDF): the power sequencer, governor, PD decode, test
   tones, JSON and every test-API call and console command, against a mock
   board. Built in a scratch folder. Where there is no
   `make` or `cc`, the check exits 127: it couldn't run, no verdict.
   The full ESP-IDF build is not part of the check: it needs
   ESP-IDF and takes minutes ([firmware/README.md](../firmware/README.md)).
   **The firmware's pins against its board,** every run: for each tier
   whose firmware exists (`firmware/board/<fw>`), the score's firmware
   check (below), so a pin the firmware gives a job that isn't on the
   board's ESP32 net for it (Lite's `ADC_PVDD` on GPIO39 is `PVDD_SENSE`)
   fails the test, not only the score. A tier with no firmware yet is left
   to the score.
4. **The hwde gates of each board that changed.** A workspace under
   `boards/` (a folder with a `state.json`) is gated only when a file in it
   differs from the merge base with main (`origin/main`, else `main`, or
   `$HWCHECK_BASE`): committed, edited or new. A PR that touches no board
   runs no gates; one that touches a board runs that board's alone. Every
   board is gated when `tools/hwcheck.py` itself changes, when git can't
   find the merge base (no main ref, not a git checkout), and with
   `test --all` or `HWCHECK_ALL=1`. The first line under `boards/` names
   which boards it gates and why. Workers run their board's gates before
   they finish, and the score re-runs every tier's boards' gates on main
   after every merge, so a change in hive-hwde that breaks a board still shows
   there. A gated board runs all four of hwde's gates: `erc` on
   `kicad/<board>.kicad_sch`, then `drc_routed`, `verify` and `dfm` on
   `kicad/<board>.kicad_pcb`. `<board>` is the `board` field in `state.json`.
   Each gate is `hwde gate --gate <g> <input> --workspace .`, run in a
   scratch copy of the workspace, so the checkout isn't changed. A board
   merges only once it passes all four. A half-built board stays on its
   branch.

When there are no boards yet, the test checks `docs/product.md` and passes.

The gates run through hive-hwde's `bin/hwde`. The script looks for it in
this order: `$HWDE_BIN`, `${HIVE_TOOLS:-/hive/tools}/hwde/bin/hwde` (the
path a check sandbox has), `hwde` on `PATH`, then
`$HIVE_MODULES/hive-hwde/bin/hwde` (with `/hive/modules` used when
`HIVE_MODULES` is unset). If there are boards to check and `hwde` isn't
found, the script prints every place it looked, then runs `sh -c hwde`. The shell prints its own
`hwde: not found` and exits 127. The Hive reads that as "couldn't run": a
fault in the environment, not a verdict on the commit.

**Unverified is neither a pass nor a fail.** A gate that ran but couldn't
fetch some parts' EasyEDA models (hwde's dfm exits 2 with an `unverified`
block naming them; an older hwde exits 1 with only `cpl_fetch_failed`
findings), is listed as unverified. If nothing else failed, the test
prints what is unverified (the parts) and exits 127 through the shell's
own not-found for a tool named for it (`easyeda-models`): the
only 127 the Hive reads as couldn't-run, so it tests the change again
later instead of calling it red. Any real failure beside it exits 1.

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

2. **Each piece: 0 once its check and its tests pass, else 100**
   (`piece_missing`). Each check is in `tools/pieces.py` and reads the
   piece against something else in the repo, so a stub file in the right
   place doesn't count: `tools/tests/test_pieces.py` puts a stub of each
   piece where the real one goes and shows its check failing. The checks:
   - **`firmware`, one per tier** (Lite `firmware/board/lite-main`, Plus
     `firmware/board/core`, Ultra `firmware/board/ultra`). The folder has
     `sdkconfig.defaults` and `partitions.csv`, and the config selects the
     tier's own Kconfig board (`HIFI_LITE`, `HIFI_PLUS`, `HIFI_ULTRA`),
     which a patch in `firmware/patches/` adds. Every GPIO that board's
     defaults give a job (the codec's I2S and I2C in `DAC_CONFIG`, each
     button in `AUDIO_CONTROLS`, IR and the LED in `SET_GPIO`), and every
     `PIN_*`, `I2C_*` and `ADC_*` in a component header whose `HIFI_BOARD`
     names the tier's board, has to land on a pad of the board's ESP32
     module that is wired to another part, on a net named for its job
     (a bit clock on a `BCLK`/`BCK` net, a button on a `BTN`/`SW` net,
     `PIN_AMP_SDZ` on an `AMP_SDZ` net, and so on). The partitions can't
     overlap or run past the flash size the config sets, and the module on
     the board (its `-N8` in the value) has that much flash. Its suite is
     the firmware's host tests, as in the test. The ESP-IDF build itself
     isn't run: it needs ESP-IDF and minutes (see above).
   - **`app`**: `app/build.py` packs `app/web` under its budget into a C
     table that compiles with `cc -Wall -Werror`; every file `index.html`
     loads is in the pack, with a manifest (name, start_url, icons in the
     pack); and the app reads `info` and `state` under `api/v1/`, which the
     firmware's core (`hifi_api.c`) answers. The calls the core doesn't
     answer yet are printed, not counted. Its suite is `app/tests`.
   - **`bench`**: `bench.py --mock bringup` runs end to end against its
     mock board and writes a report with a pass in it, and the bench calls
     the firmware's `info`, `state`, `selftest` and `tone`. Its suite is
     `bench/tests`.
   - **`enclosure`, one per tier** (`enclosure/out/<tier>`, fitted to
     `lite-main` or `core`): hive-mech's `fit.md` has passes and no fail,
     and names each of the board's mounting holes; there are STL and STEP
     files; every STL is a closed mesh (a T-junction along a lone edge is
     allowed); the largest one's footprint holds the board's Edge.Cuts
     outline; every STEP has a solid (`CLOSED_SHELL`).
   - **`manual`**: `docs/manual/sources.md` lists the manual's facts in
     tables, some taken from a committed source; every repo file it names
     in backticks is there; every section it cites (`§N`, `QS N`) is a
     numbered heading in `user-manual.md` or `quick-start.md`; and every
     numbered section of both is cited by at least one fact.

The output gives each tier's cost or why it doesn't count, and each piece's
state. The number alone is on the last line.

**If a gate couldn't run** (hwde exits 2 or more: a broken tool, not a
verdict), the score prints no number and exits 1. The same goes for a
gate whose only findings are `cpl_fetch_failed`: hwde couldn't fetch the
parts' EasyEDA models because of a rate limit or the network, which says
nothing about the board. A board that commits its model cache
(`parts/easyeda/`, as `lite-main` and `core` do, every part of every
population) needs no fetch. If hwde, make or cc
isn't there at all, it exits 127, as the test does. Neither one is a
verdict on the commit. A gate that runs and fails (exit 1) is a verdict:
that tier doesn't count.

**Tolerance 0.1 USD**, about 1% of Lite's estimate. The cost comes only from
committed files, so it's the same on every run. A change that costs
10 cents or less per unit (a fix's extra capacitor) isn't reverted as a loss.

**Evaluator paths:** `tools/hwcheck.py`, `tools/pieces.py`,
`tools/bom_estimate.py` and `tools/product.json`. Changing any of them is the person's to merge, and it
starts a new baseline. Adding a tier's function or a piece goes in
`product.json`.
`docs/product.md` isn't listed, because it changes with most tasks. The
score no longer reads it. The test holds its Boards row to `product.json`.
