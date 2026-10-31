# Review round 4: core (Plus/Ultra) round 3, ultra-front's first full round

The person (2026-10-31): "have the hifi boards be fully done". A board is done
when a full round of independent reviewers finds no blocker and no major on it
(`docs/convergence.md`). Lite converged in round 3 (`docs/review-4/`). Core
had 1 blocker / 14 majors in round 2 (`docs/review-3/`); every one is now
fixed on main:

- the core amp-area layout ECO (PR #142, "core amp-area layout ECO: TPA3220
  bootstrap and PVDD HF caps at their pins, input caps at the IN pins", plus
  R3-POWER-2's 100 nF // C4 and 2.2 uF // C8);
- core firmware: line in through the ADC and DSP, governed DJ input, Ultra's
  front board driven (commit 7f0b297);
- Zobel networks on the TPA3220 outputs + firmware tone guard (PR #144,
  `docs/boards/core-zobel.md`);
- ESD clamps on core (PR #145, `docs/boards/core-esd.md`) and Lite (PR #146).

ultra-front has only had round 2's light look (0 blockers, 1 firmware major,
since fixed).

Follow `docs/review-3/brief.md` for the method, tools and output format, with
these changes:

- Tools: hwde is `~/modules/bin/hwde` (`eval "$(~/modules/bin/hwde env)"`),
  KiCad CLI in `~/tools/hive-tools/kicad-10.0.6/bin`. Never run hwde gates in
  place on `boards/` (they rewrite `ledger.json`): copy the workspace to /tmp.
- The machine is shared and capped at 8 GB: run one heavy sim at a time, no
  parallel openEMS runs.
- Fresh eyes first (board files, generator sources, vendor datasheets, EVMs,
  app notes, field issues), then read `docs/review-3/<domain>.json`,
  `docs/review-3/dispositions.md` and the docs above, check each round-2 fix
  is really on the board and really works (try to break it: tolerance and
  temperature corners, 5 V-only and EPR chargers, hot plug, 4 and 6 ohm
  speakers, a DJ deck at full line level), and mark findings
  `"repeat_of": "<R3 id>"` or `"new"`.
- Output: `docs/review-5/<name>.md` and `.json`, ids `R5-<NAME>-<n>`,
  `round2_fixes_checked` in place of `round1_fixes_checked`. Tests under
  `docs/review-5/sims/<name>/`. Write as you go.
- Severity: **blocker** = the board won't work or can be damaged on first
  spin; **major** = a likely field failure, a spec miss the person would
  notice, or an unfixable-in-firmware fault that needs a re-spin; **minor** =
  margin, EMI/ESD headroom, cost, a bench check. Give a number for each.
- Read-only on `boards/`, `firmware/` and everything outside
  `docs/review-5/`. Don't commit or push; the lead does.
