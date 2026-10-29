# Fit check: plus

`mech build enclosure/plus.py -o enclosure/out/plus` against boards/core (see enclosure/README.md). 24/24 pass.

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
| port_PA1 | pass | edge=N, kind=generic, blocked_mm3=0, recess_mm=2.4, max_recess_mm=3.0 |
| port_PA2 | pass | edge=N, kind=generic, blocked_mm3=0, recess_mm=2.4, max_recess_mm=3.0 |
| port_PA3 | pass | edge=N, kind=generic, blocked_mm3=0, recess_mm=2.4, max_recess_mm=3.0 |
| port_PA4 | pass | edge=N, kind=generic, blocked_mm3=0, recess_mm=2.4, max_recess_mm=3.0 |
| port_U18 | pass | edge=S, kind=generic, blocked_mm3=0, recess_mm=4.5, max_recess_mm=5.0 |
| hole_H3 | pass | supported=1.0 |
| hole_H2 | pass | supported=1.0 |
| hole_H1 | pass | supported=1.0 |
| hole_H4 | pass | supported=1.0 |
| bed_base | pass | size_mm=[127.4, 106.0, 37.6], bed_mm=[220.0, 220.0, 250.0] |
| bed_btn_SW2 | pass | size_mm=[6.2, 6.2, 30.4], bed_mm=[220.0, 220.0, 250.0] |
| bed_btn_SW3 | pass | size_mm=[6.2, 6.2, 30.4], bed_mm=[220.0, 220.0, 250.0] |
| bed_knob | pass | size_mm=[25.0, 25.0, 33.2], bed_mm=[220.0, 220.0, 250.0] |
| bed_lid | pass | size_mm=[127.4, 106.0, 31.0], bed_mm=[220.0, 220.0, 250.0] |
| added_boxes_clear | pass | 6 boxes (posts, heatsinks, front board) vs the core's parts; clashes=[] |

## What is over each top-access part

- SW3: under btn_SW3
- SW2: under btn_SW2
- SW1: under knob
- D3: open above
- U18: open above

## Material

- base: 63.1 cm3, 80.1 g PETG if solid
- btn_SW2: 0.3 cm3, 0.4 g PETG if solid
- btn_SW3: 0.3 cm3, 0.4 g PETG if solid
- knob: 8.4 cm3, 10.7 g PETG if solid
- lid: 33.1 cm3, 42.0 g PETG if solid
