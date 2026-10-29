# Fit check: ultra

`mech build enclosure/ultra.py -o enclosure/out/ultra` against boards/core (see enclosure/README.md). 31/31 pass.

| check | result | measured |
|---|---|---|
| board_clear | pass | overlap_mm3=0 |
| parts_apart | pass | clashes=[] |
| parts_clear | pass | hits=[] |
| port_J1 | pass | edge=W, kind=usb-c, blocked_mm3=0, recess_mm=1.9, max_recess_mm=3.0 |
| port_J2 | pass | edge=W, kind=generic, blocked_mm3=0, recess_mm=2.6, max_recess_mm=3.0 |
| port_J4l | pass | edge=N, kind=generic, blocked_mm3=0, recess_mm=3.0, max_recess_mm=3.0 |
| port_J4u | pass | edge=N, kind=generic, blocked_mm3=0, recess_mm=3.0, max_recess_mm=3.0 |
| port_J5l | pass | edge=N, kind=generic, blocked_mm3=0, recess_mm=3.0, max_recess_mm=3.0 |
| port_J5u | pass | edge=N, kind=generic, blocked_mm3=0, recess_mm=3.0, max_recess_mm=3.0 |
| port_U19 | pass | edge=N, kind=generic, blocked_mm3=0, recess_mm=3.7, max_recess_mm=4.0 |
| port_PA1 | pass | edge=N, kind=generic, blocked_mm3=0, recess_mm=2.4, max_recess_mm=3.0 |
| port_PA2 | pass | edge=N, kind=generic, blocked_mm3=0, recess_mm=2.4, max_recess_mm=3.0 |
| port_PA3 | pass | edge=N, kind=generic, blocked_mm3=0, recess_mm=2.4, max_recess_mm=3.0 |
| port_PA4 | pass | edge=N, kind=generic, blocked_mm3=0, recess_mm=2.4, max_recess_mm=3.0 |
| port_PB1 | pass | edge=N, kind=generic, blocked_mm3=0, recess_mm=2.4, max_recess_mm=3.0 |
| port_PB2 | pass | edge=N, kind=generic, blocked_mm3=0, recess_mm=2.4, max_recess_mm=3.0 |
| port_PB3 | pass | edge=N, kind=generic, blocked_mm3=0, recess_mm=2.4, max_recess_mm=3.0 |
| port_PB4 | pass | edge=N, kind=generic, blocked_mm3=0, recess_mm=2.4, max_recess_mm=3.0 |
| port_LCD | pass | edge=S, kind=generic, blocked_mm3=0, recess_mm=22.9, max_recess_mm=25.0 |
| port_F_U2 | pass | edge=S, kind=generic, blocked_mm3=0, recess_mm=21.0, max_recess_mm=25.0 |
| hole_H3 | pass | supported=1.0 |
| hole_H2 | pass | supported=1.0 |
| hole_H1 | pass | supported=1.0 |
| hole_H4 | pass | supported=1.0 |
| bed_base | pass | size_mm=[127.4, 118.6, 47.0], bed_mm=[220.0, 220.0, 250.0] |
| bed_btn_FSW1 | pass | size_mm=[8.5, 8.5, 4.2], bed_mm=[220.0, 220.0, 250.0] |
| bed_btn_FSW2 | pass | size_mm=[8.5, 8.5, 4.2], bed_mm=[220.0, 220.0, 250.0] |
| bed_btn_FSW3 | pass | size_mm=[8.5, 8.5, 4.2], bed_mm=[220.0, 220.0, 250.0] |
| bed_knob | pass | size_mm=[22.0, 22.0, 15.0], bed_mm=[220.0, 220.0, 250.0] |
| bed_lid | pass | size_mm=[127.4, 114.4, 40.4], bed_mm=[220.0, 220.0, 250.0] |
| added_boxes_clear | pass | 49 boxes (posts, heatsinks, front board) vs the core's parts; clashes=[] |

## What is over each top-access part

- D3: open above

## Material

- base: 73.8 cm3, 93.7 g PETG if solid
- btn_FSW1: 0.1 cm3, 0.2 g PETG if solid
- btn_FSW2: 0.1 cm3, 0.2 g PETG if solid
- btn_FSW3: 0.1 cm3, 0.2 g PETG if solid
- knob: 5.3 cm3, 6.8 g PETG if solid
- lid: 36.2 cm3, 45.9 g PETG if solid
