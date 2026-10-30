# review-2 fix check (independent)

Scope: the review-2 ECO on lite-main (commit e1f6078) and core (e1338ed),
checked against the before state 41c5eb3. Read-only on `boards/`; every
number below was recomputed from the working-tree netlists
(`boards/*/kicad/*.net`), PCBs (`*.kicad_pcb`, read through `pcbnew`) and the
generator sources, not taken from the READMEs.

Severity: **blocker** (board does not work / unsafe), **major** (a stated
function or margin is not met), **minor** (works, but should be fixed).

## 1. Netlist connectivity of changed and new nets

Method: parsed both netlists at 41c5eb3 and now, and diffed every net by its
(ref, pin) set; then listed every pin of each new or changed IC.

(in progress)
