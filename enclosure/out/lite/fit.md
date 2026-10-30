# Fit check: lite

`mech build enclosure/lite.py -o enclosure/out/lite` against boards/lite-main (see enclosure/README.md). 23/23 pass.

| check | result | measured |
|---|---|---|
| board_clear | pass | overlap_mm3=0 |
| parts_apart | pass | clashes=[] |
| parts_clear | pass | hits=[] |
| port_J1 | pass | edge=N, kind=usb-c, blocked_mm3=0, recess_mm=3.0, max_recess_mm=3.0 |
| port_J2l | pass | edge=N, kind=generic, blocked_mm3=0, recess_mm=3.0, max_recess_mm=3.0 |
| port_J2u | pass | edge=N, kind=generic, blocked_mm3=0, recess_mm=3.0, max_recess_mm=3.0 |
| port_J3l | pass | edge=N, kind=generic, blocked_mm3=0, recess_mm=3.0, max_recess_mm=3.0 |
| port_J3u | pass | edge=N, kind=generic, blocked_mm3=0, recess_mm=3.0, max_recess_mm=3.0 |
| port_PA1 | pass | edge=N, kind=generic, blocked_mm3=0, recess_mm=2.4, max_recess_mm=3.0 |
| port_PA2 | pass | edge=N, kind=generic, blocked_mm3=0, recess_mm=2.4, max_recess_mm=3.0 |
| port_PA3 | pass | edge=N, kind=generic, blocked_mm3=0, recess_mm=2.4, max_recess_mm=3.0 |
| port_PA4 | pass | edge=N, kind=generic, blocked_mm3=0, recess_mm=2.4, max_recess_mm=3.0 |
| port_U9 | pass | edge=S, kind=generic, blocked_mm3=0, recess_mm=3.5, max_recess_mm=4.0 |
| hole_H2 | pass | supported=1.0 |
| hole_H1 | pass | supported=1.0 |
| hole_H4 | pass | supported=1.0 |
| hole_H3 | pass | supported=1.0 |
| bed_base | pass | size_mm=[106.0, 96.0, 44.6], bed_mm=[220.0, 220.0, 250.0] |
| bed_btn_SW1 | pass | size_mm=[6.2, 6.2, 37.4], bed_mm=[220.0, 220.0, 250.0] |
| bed_btn_SW2 | pass | size_mm=[6.2, 6.2, 37.4], bed_mm=[220.0, 220.0, 250.0] |
| bed_btn_SW3 | pass | size_mm=[6.2, 6.2, 37.4], bed_mm=[220.0, 220.0, 250.0] |
| bed_lid | pass | size_mm=[106.0, 96.0, 38.0], bed_mm=[220.0, 220.0, 250.0] |
| added_boxes_clear | pass | 4 boxes (posts, heatsinks, front board) vs the board's parts; clashes=[] |

## What is over each top-access part

- D2: open above
- U9: under lid
- SW3: under btn_SW3
- SW2: under btn_SW2
- SW1: under btn_SW1

## Material

- base: 56.9 cm3, 72.3 g PETG if solid
- btn_SW1: 0.3 cm3, 0.4 g PETG if solid
- btn_SW2: 0.3 cm3, 0.4 g PETG if solid
- btn_SW3: 0.3 cm3, 0.4 g PETG if solid
- lid: 26.2 cm3, 33.2 g PETG if solid
