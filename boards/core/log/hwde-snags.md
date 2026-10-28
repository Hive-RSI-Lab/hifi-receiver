# hwde snags hit on the core board (for the hive-hwde project)

Each entry names the file it lives in, what happened, and what was done here.

1. **`bin/hwde` is not executable in the installed module**
   (`$HIVE_MODULES/hive-hwde/bin/hwde`). Running it gives "Permission
   denied", so every call here is `sh .../bin/hwde`. Fix: commit the file
   with mode 0755.
2. **lib_pull: EasyEDA rate limit** (`scripts/lib_pull.py`). In one 45-part
   batch at the default 15 s pace, 2 parts (C479074 SY8089, C2688377
   ES9018K2M) failed with HTTP 403. A second pull at 20 s worked, so the
   retry pass did not wait long enough. Two more parts (C99865 GC4010-F,
   C17223620 PLR237/T8BK) have no EasyEDA model at all; lib_pull reports
   those the same way as a 403, as `error` with "Failed to fetch".
   - The batch took about 35 min for 45 parts.
3. **Pulled symbols carry `unspecified` pin types**
   (`scripts/lib_pull.py`, `scripts/schlib.py`). `schlib --pins` reports
   them as `passive`, but KiCad ERC sees `unspecified` and raised 394
   pin-to-pin warnings on the first run. `lib_pin_types.py` needs
   datasheet-extract JSON for every IC. Here `lib/aiee.kicad_sym` was
   retyped `unspecified`/`input` to `passive` with sed. Fix: lib_pull
   should write `passive` (what schlib already assumes), or ERC should be
   told.
4. **The placement agent prompt is out of date with place_seed**
   (`agents/placement.md`). It says `place_seed.py --pcb ... --apply`, but
   the script requires `--ops-out`, so it exits 2 with a usage error. The
   ops file also has to be `{"version": 1, "ops": [...]}`, which the prompt
   doesn't say.
5. **rules_gen gives every power netclass 0.2 mm clearance**
   (`scripts/rules_gen.py`). It also sets per-net minimum track widths of
   up to 2.38 mm (5 A, 1 oz). Those nets end on 0.4-0.65 mm pitch pads:
   CH224Q VHV/VBUS, INA186 IN+/IN-, TPA3221 PVDD/OUT, ES9018 AVCC. A track
   at the class width can't enter those pads legally, and board_init's
   self-check already flagged the CH224Q pads (0.176 mm against 0.2 mm). See
   the routing notes in the README for how it was handled.
6. **rules_gen rule-name collision** (`scripts/rules_gen.py` `_safe`).
   `A_SPK_L+` and `A_SPK_L-` both become `aiee_pwr_width_amp_a_A_SPK_L`,
   so the .kicad_dru holds duplicate rule names.
7. **board_init `--mounting-holes auto` gave 0 holes** on a fixed
   `--outline 120x100` (`scripts/board_init.py`). Its help says auto means 4
   once the shorter side is 30 mm or more. `--mounting-holes 4` was needed.
8. **Pulled footprints with defects**:
   - C88733 (50PX470MEFC10X20): its courtyard was two zero-area lines
     ("malformed courtyard"), replaced by a 5.3 mm circle.
   - C5184243 (USB4105-GF-A-120): the pull comes as `USB-C-SMD_MC-311D`,
     whose NPTH pegs sit 0.18 mm from the GND/VBUS pads (hole_clearance).
9. **No assembly-variant support** (`scripts/bom_cpl.py`). One PCB serves
   two products here (Plus fits a set of parts, Ultra fits another). hwde
   has a DNP class but no variant. The Ultra BOM is made by running
   bom_cpl a second time with a parts file whose classes are swapped (see
   README).
10. **kicad-routing-tools is in `/hive/tools`**, but `docs/README.md`
    ("What doesn't work here yet") still says it is missing.
11. **bin/hwde doesn't point HWDE_KRT_DIR at the Hive's KRT**
    (`bin/hwde`, `scripts/lib/env.py find_krt`). route_critical fails with
    "KiCadRoutingTools not found" even though `/hive/tools/kicad-routing-tools`
    is there. env.py wants the directory holding `route_diff.py`, which is
    `.../KiCadRoutingTools-0.19.0/plugins`, not the release root. Workaround:
    `export HWDE_KRT_DIR=/hive/tools/kicad-routing-tools/KiCadRoutingTools-0.19.0/plugins`.
12. **board_init moves the outline origin between runs** (`board_init.py`).
    The same netlist and `--outline 120x100` gave origin (34.65, 113.31)
    once and (33.54, 119.72) the next time (shelf packing). Placement ops in
    absolute mm break across a re-init, so `kicad/gen/anchors.py` reads
    `outline_origin` from `reports/board_init.json`.
13. **place_anneal never finished** on this board: 272 parts, 52 locked
    anchors, machine load ~30 on 20 cores. It produced no candidate in 22
    min. place_seed with `placement.fixed` pushed the satellites outside the
    outline. A small nearest-free-spot placer (`kicad/gen/satellites.py`)
    gave a place-gate pass in under 3 s.
    - **EasyEDA courtyards:** they are often the body outline only (pads
      stick out), so any courtyard-only collision test under-sizes them.
      Use the union with pads + 0.25 mm, which is what the place gate uses.
14. **route_critical's KRT pass shorted the board** (`scripts/route_critical.py`).
    It ran 18 min on this board and rolled back: 199 shorting_items, 292
    clearance, 7 diff-gap errors. route_auto's KRT finish (`_krt_finish`)
    routes power nets at KRT's default width, so 50-199 track_width errors
    appeared each time and the pass was discarded. KRT itself has
    `--power-nets/--power-nets-widths` and `--hole-to-hole-clearance`, but
    hwde passes neither. Passing them by hand made things worse here (499
    clearance), so the KRT path was dropped for this board.
15. **Freerouting doesn't see pre-placed copper pours as connecting.** The DSN
    leaves zones out, so it tried to track nets the pours already joined and
    threaded other nets through them, splitting the pours. Rule-area
    keepouts over the fan-in pours fixed it (`kicad/gen/pre_route.py`).
16. **The ladder left 37-70 unconnected items** on a 278-part 4-layer board.
    Most were GND pour fragments with no via, plus pads boxed in by
    neighbouring tracks. There is no hwde tool to close the last few
    percent short of a re-route; this board used a small maze router
    (`kicad/gen/miniroute.py`). hwde would benefit from one: grid A* on F/B
    plus vias, using rules_gen widths, hole-to-hole and keepouts.
17. **stitch_vias ignores hole-to-hole** (as the Lite worker found). It
    placed vias 0.25 mm hole-to-hole, against JLC's 0.5 mm.
18. **route_cleanup won't snap off-angle segments on `_P`/`_N`/`+`/`-`
    nets** (`kept_net`). Its name-based pair detection catches speaker nets
    `A_SPK_L+`/`A_SPK_L-`, and the explicitly empty `diff_pairs: []` in
    constraints doesn't turn the detection off. The verify route_style
    check then fails on them.
19. **dfm catches what drc_routed doesn't:** gaps of 0.025-0.06 mm on the
    Gerbers, all on DRC-clean copper.
    - A GND pour 0.028 mm from a no-net pad (CH224Q PG).
    - A same-net pour edge 0.025 mm from a pad: different features, no net
      in the Gerber.
    - A via 0.06 mm outside its own net's pour.
20. **lib_pull writes absolute 3D model paths** (`/work/boards/<b>/lib/...`),
    and verify's check_model_paths fails on them. Here they were rewritten to
    `${KIPRJMOD}/../lib/aiee.3dshapes/`.
21. **KiCad 10 SWIG:** `FOOTPRINT.GraphicalItems()` and `BOARD.Zones()`
    sometimes return bare SwigPyObjects (no `m_Uuid`, no `GetNetname`),
    which breaks pcbnew scripts that work on the same objects elsewhere.

hwde version: started on 0b4bd0d. All gates were last run and passed on
94ec4b7, the installed module at 2026-10-28 18:15Z, which includes the
fpfix courtyard rule E and check_courtyard. fpfix was re-run on `lib/`.
