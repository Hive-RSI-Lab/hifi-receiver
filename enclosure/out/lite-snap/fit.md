# Fit: enclosure

30/31 checks pass, some fail.

| check | result | measured |
|---|---|---|
| board_clear | pass | overlap_mm3=0 |
| board_gap | pass | min_gap_mm=0.1, within_mm3=0 |
| lid_covers | pass | open_mm2=0.0, where=[] |
| parts_apart | pass | clashes=[] |
| parts_clear | pass | hits=[] |
| port_J1 | pass | edge=N, kind=usb-c, blocked_mm3=0, recess_mm=2.97, max_recess_mm=3.01 |
| port_J2l | pass | edge=N, kind=generic, blocked_mm3=0, recess_mm=3.0, max_recess_mm=3.01 |
| port_J2u | pass | edge=N, kind=generic, blocked_mm3=0, recess_mm=3.0, max_recess_mm=3.01 |
| port_J3l | pass | edge=N, kind=generic, blocked_mm3=0, recess_mm=3.0, max_recess_mm=3.01 |
| port_J3u | pass | edge=N, kind=generic, blocked_mm3=0, recess_mm=3.0, max_recess_mm=3.01 |
| port_J4 | pass | edge=N, kind=generic, blocked_mm3=0, recess_mm=3.0, max_recess_mm=3.01 |
| port_J5 | pass | edge=N, kind=generic, blocked_mm3=0, recess_mm=3.0, max_recess_mm=3.01 |
| port_J6 | pass | edge=N, kind=generic, blocked_mm3=0, recess_mm=3.0, max_recess_mm=3.01 |
| port_J7 | pass | edge=N, kind=generic, blocked_mm3=0, recess_mm=3.0, max_recess_mm=3.01 |
| port_U9 | pass | edge=S, kind=generic, blocked_mm3=0, recess_mm=3.5, max_recess_mm=4.0 |
| hole_H2 | pass | supported=1.0 |
| screw_H2 | pass | axis_blocked_mm3=1.492, peg=True |
| hole_H4 | pass | supported=1.0 |
| screw_H4 | pass | axis_blocked_mm3=1.492, peg=True |
| hole_H1 | pass | supported=1.0 |
| screw_H1 | pass | axis_blocked_mm3=1.492, peg=True |
| hole_H3 | pass | supported=1.0 |
| screw_H3 | pass | axis_blocked_mm3=1.492, peg=True |
| bed_base | pass | size_mm=[124.0, 96.0, 37.6], bed_mm=[180, 180, 180] |
| bed_lid | pass | size_mm=[124.0, 96.0, 30.9], bed_mm=[180, 180, 180] |
| overhang_base | pass | steep_mm2=0, faces=[], max_deg=45.0 |
| bridge_base | pass | flat_faces=14, worst_bridge_mm=14.51, worst_ledge_mm=0.11, max_bridge_mm=15.0, max_ledge_mm=1.0, unsupported=[] |
| wall_base | pass | min_wall_mm=0.8, thin=[] |
| overhang_lid | pass | steep_mm2=0, faces=[], max_deg=45.0 |
| bridge_lid | pass | flat_faces=12, worst_bridge_mm=2.25, worst_ledge_mm=1.01, max_bridge_mm=15.0, max_ledge_mm=1.0, unsupported=[] |
| wall_lid | fail | min_wall_mm=0.8, thin=[[23.5, 6.5, 0.2, 24.3, 8.1, 0.2], [22.8, 6.7, 0.2, 23.5, 8.3, 0.2]] |

## Reached from above

- D2: open above
- U9: under lid
- J8: under lid
- SW3: under lid
- SW2: under lid
- SW1: under lid

## Material

- base: 56.0 cm3, 71.2 g PETG if solid
- lid: 30.9 cm3, 39.3 g PETG if solid

## Print (PrusaSlicer 2.8.1, mech/slicer.ini)

| part | filament g | support g | support cm3 | time | cost |
|---|---|---|---|---|---|
| base | 73.09 | 3.42 | 2.7 | 4h 27m 0s | 1.83 |
| lid | 37.28 | 0.6 | 0.47 | 2h 29m 16s | 0.93 |
| all | 110.37 | 4.02 | 3.17 | 6h 56m | 2.76 |
