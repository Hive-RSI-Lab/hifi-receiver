# Run record: ultra-front (2026-10-28)

The whole run took about 80 minutes (10:57 to 12:15 local), with one worker
running the orchestrator and every design role inline.

- **Phase events in `state.json`:** P1-P4 and P5-P7 were each set in one
  batch, so their timestamps don't show how long each stage took.
- **Where the time went:**
  - P0-P4 (requirements, datasheets, parts, schematic, ERC): about 20 min.
  - First placement and routing: about 15 min.
  - Re-placing and re-routing after board review 1: about 40 min, most of it
    on GND at the 0.5 mm FFC.

## Subagent spawns

All three were fresh-context reviewers:

| Role | Phase | Tokens | Result |
|---|---|--:|---|
| schematic-reviewer | P4 | 102,769 | 0 errors, 6 warnings, all acted on |
| verify-reviewer | P8 | 138,236 | 2 errors (LCD tail), fixed |
| verify-reviewer | P8 | 119,829 | 0 errors, 5 warnings |

## Gate attempts

Each attempt was recorded with `--workspace`:

| Gate | Attempts | Failed | Why |
|---|--:|--:|---|
| erc | 5 | 2 | 72 pin_to_pin warnings from unspecified pulled pins; bidirectional pins on a PWR_FLAG'd GND |
| place | 18 | 8 | J2 mating checks (waivers), tail keepout overlaps, C3/J2 courtyard, R13 at the keepout edge |
| drc_routed | 7 | 1 | first route: silk overlaps and an unconnected net |
| verify | 7 | 2 | J2 mating, before the waiver; C1 decoupling loop, before the GND vias |
| dfm | 3 | 0 | |

No fix-loop budget (`state.py budget`) was consumed: every failure was fixed
directly.

## Sign-offs

H1, H2 and H4 went to the hifi-receiver-2 lead in one packet at the end of the
run. The run went on while they were open.
