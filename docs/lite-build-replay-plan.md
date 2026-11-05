# Plan: the Lite's build as a demo in a replayed dashboard

The film shows how the Lite got built, played back on a copy of the real dashboard
and ending on the finished board and box. It is a plan: nothing is rendered yet.

## Look

Every shot is either the dashboard being replayed or a 3D render of the product.
There are no drawn graphics and no synthetic titles, as the person asked on
2026-11-05 for the hive films ("all graphics should be the dash being replayed
and 3D renders of the product or graphs").

Tool: `~/modules/video`, which films a real page on a virtual clock, from thread
video-tool: 2560x1440 from 2x captures, one encode, the dark caption strip, camera
pushes capped at 1.04x, 0.25 s crossfades. `video export` makes the site, X,
LinkedIn and vertical cuts.

## Pattern to reuse

`~/projects/demo-board-film` already does this for a board designed from a one-line
brief: a copy of `modules/dash/web`, `replay-shim.js` faking fetch and Date, and
`bundle.py` scrubbing names and paths into `replay/data.json`. Copy that project to
`~/projects/lite-build-film` and change the data source.

## The difference: no recording exists

That film's recorder took 30 s snapshots during the run. Nobody recorded the Lite,
so the data has to be rebuilt from what was kept:

| Source | What it holds | Shows up as |
|---|---|---|
| `~/board.jsonl`, threads hifi-receiver, jlc-quote-hifi-lite, hifi-lite-esd, hifi-fw-build, hifi-lite-antenna, hifi-enclosure | 164 posts from 30 agents, 2026-10-31 to now | chat, comms |
| `~/log.jsonl` | 39 session starts in those threads; ends to be matched | agents, timeline |
| git log of `boards/lite-main` and `enclosure/` | 21 board commits from 2026-10-28, gates and fixes | progress, captions |
| `boards/lite-main/log/P0-P9-digest.md`, `reports/`, `ledger.json` | design steps and check results | captions, end card |

Gaps to accept and state in the film's notes:

- Usage and live CPU were never sampled for these runs, so those panels are left
  out of the shots, not invented.
- The record on this board starts 2026-10-31. Earlier design work exists only in
  git and the digests, so it plays as a short opening from those, not as chat.

"Fake dashboard" means a copy with real events and made-up names: agents, login and
paths scrubbed by `bundle.py`, and the deny list applied. Check the contact sheet
for names before anything leaves the hive.

## Shots, about 60 s

1. **The ask, 6 s.** The person's brief arrives in chat.
2. **The design runs, 15 s.** Timeline and comms at speed: threads open, agents hand
   off, gates fail and get fixed.
3. **A real catch, 10 s.** One fix told through its posts: the ESD fix, or the antenna
   clearance found before ordering.
4. **The board, 8 s.** Blender render of the Lite, from the KiCad 3D export.
5. **The order, 6 s.** Quote and cart posts, with the real price from the reports.
6. **The box, 8 s.** Box renders, lid lifting off the board, from `enclosure.step`.
7. **End card, 7 s.** Board and box, real cost and elapsed time, from the outputs.

If the box has been printed by then, a photo replaces part of shot 6.

## Steps

1. Wait for the antenna and stitching edits to merge, so the last shots show the
   board as ordered.
2. Write `bundle.py` for the sources above, then `pilot.mjs` screenshots of each view
   to check the data, before any filming.
3. Render the board and box in Blender.
4. `heavy timeout 1500 video render film.mjs OUT`, then `video sheet` and
   `video stutters`, then `video export`.
5. Post to the thread for the person's view. Publishing is a separate go.
