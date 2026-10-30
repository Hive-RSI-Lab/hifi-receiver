# Fit check: lite

`mech build enclosure/lite.py -o enclosure/out/lite` against boards/lite-main (see enclosure/README.md). 44/44 pass.

| check | result | measured |
|---|---|---|
| board_clear | pass | overlap_mm3=0 |
| board_gap | pass | min_gap_mm=0.1, within_mm3=0 |
| lid_covers | pass | open_mm2=0.0, where=[] |
| parts_apart | pass | clashes=[] |
| parts_clear | pass | hits=[] |
| port_J1 | pass | edge=N, kind=usb-c, blocked_mm3=0, recess_mm=2.97, max_recess_mm=3.0 |
| port_J2l | pass | edge=N, kind=generic, blocked_mm3=0, recess_mm=3.0, max_recess_mm=3.0 |
| port_J2u | pass | edge=N, kind=generic, blocked_mm3=0, recess_mm=3.0, max_recess_mm=3.0 |
| port_J3l | pass | edge=N, kind=generic, blocked_mm3=0, recess_mm=3.0, max_recess_mm=3.0 |
| port_J3u | pass | edge=N, kind=generic, blocked_mm3=0, recess_mm=3.0, max_recess_mm=3.0 |
| port_J4 | pass | edge=N, kind=generic, blocked_mm3=0, recess_mm=3.0, max_recess_mm=3.0 |
| port_J5 | pass | edge=N, kind=generic, blocked_mm3=0, recess_mm=3.0, max_recess_mm=3.0 |
| port_J6 | pass | edge=N, kind=generic, blocked_mm3=0, recess_mm=3.0, max_recess_mm=3.0 |
| port_J7 | pass | edge=N, kind=generic, blocked_mm3=0, recess_mm=3.0, max_recess_mm=3.0 |
| port_U9 | pass | edge=S, kind=generic, blocked_mm3=0, recess_mm=3.5, max_recess_mm=4.0 |
| hole_H2 | pass | supported=1.0 |
| screw_H2 | pass | axis_blocked_mm3=0, peg=False |
| hole_H1 | pass | supported=1.0 |
| screw_H1 | pass | axis_blocked_mm3=0, peg=False |
| hole_H4 | pass | supported=1.0 |
| screw_H4 | pass | axis_blocked_mm3=0, peg=False |
| hole_H3 | pass | supported=1.0 |
| screw_H3 | pass | axis_blocked_mm3=0, peg=False |
| bed_base | pass | size_mm=[124.0, 96.0, 37.6], bed_mm=[220.0, 220.0, 250.0] |
| bed_btn_SW1 | pass | size_mm=[6.2, 6.2, 30.4], bed_mm=[220.0, 220.0, 250.0] |
| bed_btn_SW2 | pass | size_mm=[6.2, 6.2, 30.4], bed_mm=[220.0, 220.0, 250.0] |
| bed_btn_SW3 | pass | size_mm=[6.2, 6.2, 30.4], bed_mm=[220.0, 220.0, 250.0] |
| bed_lid | pass | size_mm=[124.0, 96.0, 31.0], bed_mm=[220.0, 220.0, 250.0] |
| overhang_base | pass | steep_mm2=0, faces=[], max_deg=45.0 |
| bridge_base | pass | flat_faces=16, worst_bridge_mm=12.01, worst_ledge_mm=0.11, max_bridge_mm=15.0, max_ledge_mm=1.0, unsupported=[] |
| wall_base | pass | min_wall_mm=0.8, thin=[] |
| overhang_btn_SW1 | pass | steep_mm2=0, faces=[], max_deg=45.0 |
| bridge_btn_SW1 | pass | flat_faces=0, worst_bridge_mm=0.0, worst_ledge_mm=0.0, max_bridge_mm=15.0, max_ledge_mm=1.0, unsupported=[] |
| wall_btn_SW1 | pass | min_wall_mm=0.8, thin=[] |
| overhang_btn_SW2 | pass | steep_mm2=0, faces=[], max_deg=45.0 |
| bridge_btn_SW2 | pass | flat_faces=0, worst_bridge_mm=0.0, worst_ledge_mm=0.0, max_bridge_mm=15.0, max_ledge_mm=1.0, unsupported=[] |
| wall_btn_SW2 | pass | min_wall_mm=0.8, thin=[] |
| overhang_btn_SW3 | pass | steep_mm2=0, faces=[], max_deg=45.0 |
| bridge_btn_SW3 | pass | flat_faces=0, worst_bridge_mm=0.0, worst_ledge_mm=0.0, max_bridge_mm=15.0, max_ledge_mm=1.0, unsupported=[] |
| wall_btn_SW3 | pass | min_wall_mm=0.8, thin=[] |
| overhang_lid | pass | steep_mm2=0, faces=[], max_deg=45.0 |
| bridge_lid | pass | flat_faces=0, worst_bridge_mm=0.0, worst_ledge_mm=0.0, max_bridge_mm=15.0, max_ledge_mm=1.0, unsupported=[] |
| wall_lid | pass | min_wall_mm=0.8, thin=[] |
| added_boxes_clear | pass | 0 boxes (posts, heatsinks, front board) vs the board's parts; clashes=[] |

## What is over each top-access part

- D2: open above
- U9: under lid
- SW3: under btn_SW3
- SW2: under btn_SW2
- SW1: under btn_SW1

## Material

- base: 57.4 cm3, 72.9 g PETG if solid
- btn_SW1: 0.3 cm3, 0.4 g PETG if solid
- btn_SW2: 0.3 cm3, 0.4 g PETG if solid
- btn_SW3: 0.3 cm3, 0.4 g PETG if solid
- lid: 29.3 cm3, 37.2 g PETG if solid
