# Review round 3 (Lite): fresh eyes on the board after round 2's fixes

Task 261030-151656 (issue #118). Round 1 is `docs/review-2/`, round 2 is
`docs/review-3/` (its brief, `docs/review-3/brief.md`, holds the tools, rules
and output format: follow it, with the paths below). We stop when a round
finds no blocker or major. Round 2 found on Lite one board major (the sub-out
thump on power loss) and several firmware majors; all were fixed:

- `boards/lite-main/kicad/gen/root.py` "sub-out power-fail mute" (Q11-Q15,
  R80-R82, C90) and its sims in `docs/review-3/sims/fix/lite_mute/`.
- Firmware (`firmware/components/hifi/`): the governor (`core/hifi_gov.c`:
  unclipped power, 50 ms window, per-block peak limit, Lite thermal 9 W/ch),
  power sequencing (`esp/hifi_main.c`: amps held until the codec settles and
  across an input change, OTW step-down, input saved), the sub level cap
  (`esp/board_lite.c` SUB_CODE_MAX).

This round covers **Lite only** (`boards/lite-main/`, and the firmware that
drives it). Read the board files and the sources yourself; don't take
round 1's or round 2's conclusions as given. Form your view first, then read
`docs/review-3/<domain>.json` for what round 2 found, and mark each of your
findings `"repeat_of"` a round-1/round-2 id or `"new"`.

Two things this round must do:

1. **Check every round-2 fix on Lite is really there and really works**
   (netlist, placement, routing, values, the firmware path, and the sims:
   re-run them, and try to break them: tolerance corners, temperature, the
   5 V-only port, a hot-plug, a charger that renegotiates).
2. **Look where rounds 1 and 2 looked least**, with vendor app notes,
   reference designs and other people's field issues as the lens: anything
   that would stop a first-spin Lite working at the person's home.

Write `docs/review-4/<domain>.md` and `.json` in round 2's format
(`R4-<DOMAIN>-<n>` ids; `round2_fixes_checked` in place of
`round1_fixes_checked`). Tests go under `docs/review-4/sims/<domain>/`.
Read-only on `boards/` and `firmware/`; never run hwde gates in place on
`boards/` (they rewrite `ledger.json`): copy the workspace to /tmp first.
Write your md/json as you go, not only at the end.
