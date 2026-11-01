# Fit check: plus

`mech build enclosure/plus.py -o enclosure/out/plus` against boards/core (see enclosure/README.md). 46/46 pass.

| check | result | measured |
|---|---|---|
| board_clear | pass | overlap_mm3=0 |
| board_gap | pass | min_gap_mm=0.1, within_mm3=0 |
| lid_covers | pass | open_mm2=0.0, where=[] |
| parts_apart | pass | clashes=[] |
| parts_clear | pass | hits=[] |
| added_parts_clear | pass | added=3, clashes=[] |
| port_J2 | pass | edge=W, kind=generic, blocked_mm3=0, recess_mm=2.6, max_recess_mm=3.0 |
| port_J1 | pass | edge=W, kind=generic, blocked_mm3=0, recess_mm=0.47, max_recess_mm=3.0 |
| port_J4l | pass | edge=N, kind=generic, blocked_mm3=0, recess_mm=3.0, max_recess_mm=3.0 |
| port_J4u | pass | edge=N, kind=generic, blocked_mm3=0, recess_mm=3.0, max_recess_mm=3.0 |
| port_J5l | pass | edge=N, kind=generic, blocked_mm3=0, recess_mm=3.0, max_recess_mm=3.0 |
| port_J5u | pass | edge=N, kind=generic, blocked_mm3=0, recess_mm=3.0, max_recess_mm=3.0 |
| port_J20 | pass | edge=N, kind=generic, blocked_mm3=0, recess_mm=3.0, max_recess_mm=3.0 |
| port_J21 | pass | edge=N, kind=generic, blocked_mm3=0, recess_mm=3.0, max_recess_mm=3.0 |
| port_J26 | pass | edge=N, kind=generic, blocked_mm3=0, recess_mm=3.0, max_recess_mm=3.0 |
| port_J27 | pass | edge=N, kind=generic, blocked_mm3=0, recess_mm=3.0, max_recess_mm=3.0 |
| port_U18 | pass | edge=S, kind=generic, blocked_mm3=0, recess_mm=9.9, max_recess_mm=11.0 |
| hole_H3 | pass | supported=1.0 |
| screw_H3 | pass | axis_blocked_mm3=0, peg=False |
| hole_H2 | pass | supported=1.0 |
| screw_H2 | pass | axis_blocked_mm3=0, peg=False |
| hole_H1 | pass | supported=1.0 |
| screw_H1 | pass | axis_blocked_mm3=0, peg=False |
| hole_H4 | pass | supported=1.0 |
| screw_H4 | pass | axis_blocked_mm3=0, peg=False |
| bed_base | pass | size_mm=[166.8, 125.4, 37.6], bed_mm=[220.0, 220.0, 250.0] |
| bed_btn_SW2 | pass | size_mm=[6.2, 6.2, 30.4], bed_mm=[220.0, 220.0, 250.0] |
| bed_btn_SW3 | pass | size_mm=[6.2, 6.2, 30.4], bed_mm=[220.0, 220.0, 250.0] |
| bed_knob | pass | size_mm=[25.0, 25.0, 33.2], bed_mm=[220.0, 220.0, 250.0] |
| bed_lid | pass | size_mm=[166.8, 125.4, 31.0], bed_mm=[220.0, 220.0, 250.0] |
| overhang_base | pass | steep_mm2=0, faces=[], max_deg=45.0 |
| bridge_base | pass | flat_faces=91, worst_bridge_mm=12.01, worst_ledge_mm=0.11, max_bridge_mm=15.0, max_ledge_mm=1.0, unsupported=[] |
| wall_base | pass | min_wall_mm=0.8, thin=[] |
| overhang_btn_SW2 | pass | steep_mm2=0, faces=[], max_deg=45.0 |
| bridge_btn_SW2 | pass | flat_faces=0, worst_bridge_mm=0.0, worst_ledge_mm=0.0, max_bridge_mm=15.0, max_ledge_mm=1.0, unsupported=[] |
| wall_btn_SW2 | pass | min_wall_mm=0.8, thin=[] |
| overhang_btn_SW3 | pass | steep_mm2=0, faces=[], max_deg=45.0 |
| bridge_btn_SW3 | pass | flat_faces=0, worst_bridge_mm=0.0, worst_ledge_mm=0.0, max_bridge_mm=15.0, max_ledge_mm=1.0, unsupported=[] |
| wall_btn_SW3 | pass | min_wall_mm=0.8, thin=[] |
| overhang_knob | pass | steep_mm2=0, faces=[], max_deg=45.0 |
| bridge_knob | pass | flat_faces=0, worst_bridge_mm=0.0, worst_ledge_mm=0.0, max_bridge_mm=15.0, max_ledge_mm=1.0, unsupported=[] |
| wall_knob | pass | min_wall_mm=0.8, thin=[] |
| overhang_lid | pass | steep_mm2=0, faces=[], max_deg=45.0 |
| bridge_lid | pass | flat_faces=0, worst_bridge_mm=0.0, worst_ledge_mm=0.0, max_bridge_mm=15.0, max_ledge_mm=1.0, unsupported=[] |
| wall_lid | pass | min_wall_mm=0.8, thin=[] |
| added_boxes_clear | pass | 3 boxes (heatsinks, front board) vs the board's parts; clashes=[] |

## What is over each top-access part

- SW3: under btn_SW3
- SW2: under btn_SW2
- SW1: under knob
- U18: open above
- D3: open above

## Material

- base: 82.2 cm3, 104.4 g PETG if solid
- btn_SW2: 0.3 cm3, 0.4 g PETG if solid
- btn_SW3: 0.3 cm3, 0.4 g PETG if solid
- knob: 8.4 cm3, 10.7 g PETG if solid
- lid: 46.6 cm3, 59.2 g PETG if solid
