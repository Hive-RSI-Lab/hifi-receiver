# Lite spare header J8 (2026-11-05)

Eight SMD pins JLC fits (C2883765, X6511WVS-08H-C60D48R2, 2.54 mm, gold, pins staggered north and south), so a Lite whose audio
side doesn't work can still be used as an ESP32 board. Nothing to solder. No board has
been built, so none of this has been tried on hardware.

Pins, with the board's north edge up, left to right:

| Pin | Net | Silk | Row |
|---|---|---|---|
| 1 | U0TXD (GPIO1) | TX | south |
| 2 | GND | GND | north |
| 3 | U0RXD (GPIO3) | RX | south |
| 4 | +3V3 | 3V3 | north |
| 5 | GPIO5 | IO5 (label sits left of the pin) | south |
| 6 | I2C_SDA (GPIO33) | SDA | north |
| 7 | GPIO15 | IO15 (label sits right of the pin) | south |
| 8 | I2C_SCL (GPIO32) | SCL | north |

## What to respect when wiring to it

- **GPIO5 and GPIO15 are boot straps** that default high (silk: `IO5/IO15: HIGH AT RESET`).
  Don't pull either low through reset: GPIO15 low silences the ROM's boot log, and GPIO5
  sets SDIO timing.
- **UART0 is shared with the CH340B**, which stays powered from +3V3. TX can be read freely.
  An outside driver on RX fights the CH340's TX, so don't drive RX without taking the
  CH340 out of the way (no series resistor separates them).
- **I2C is the codec's and the CH224's bus**, already pulled up by R22 and R23 (2.2k).
  Add devices at other addresses: firmware uses 0x10 (ES8388) and 0x22 (CH224).
- **+3V3 is the digital rail from U3**, which also feeds the ESP32 and the CH340, so keep what
  you draw from it small. Don't back-feed it while U3 is running.
- The ESP32's inputs here have no ESD clamp (`check_esd` lists J8): the header is for
  bench use with the lid off. The lid covers it, and the fit check passes (12 mm of height
  allowed, 29 mm of headroom).

## Programming through it

Not on its own. Serial download needs GPIO0 low at reset. GPIO0 and EN aren't on J8, and
the board has no boot button: only the CH340's DTR/RTS (Q2, Q3) drive them. With USB
working there's no need for the header. With the CH340 dead, you need a wire from R21's GPIO0
side to GND while power comes up, plus TX and RX from J8.

## Why these pins

GPIO2 and GPIO12 were dropped: the copper around them is packed, and GPIO12 sets the flash
voltage, so a wrong level there can stop the module booting.

## Board changes

`kicad/gen/root.py` adds J8. `kicad/gen/header_eco.py` routes it on the placed board
(`kicad/header-placements.json`, run once, after `board_update`). Against main: no part
moved and no track changed; J8, 34 tracks, 6 vias, plus the 5 GND stitching vias below.

## GND stitching vias

The MCLK layer changes at (55.64, 118.87) and (59.60, 134.36) had no GND via near them
(12.6 and 11.0 mm), so the clock's return had a long way to go between planes. Five 0.6/0.3 vias
added at (56.2, 118.3), (59.6, 130.3), (73.5, 116.4), (66.9, 138.1), (76.8, 144.6): those two
gaps are now 0.8 and 4.06 mm. Geometry and DRC only; there's no EMI simulation and nothing
has been measured. Other sparse spots on the board weren't reworked.

## Checks

DRC, ERC, DFM and `cpl_verify` (J8 pin 1 on pad 1 at rotation 0) pass. Lite box fit 44/44.
`check_ratings` and `check_straps` note J8 has no datasheet extraction and no reset-state row, which
doesn't matter for a plain header. Rotation still gets a look in JLC's preview.

## Why this part

JLC's BOM step showed the first choice, C5160791, at quantity 0 with its tick box greyed out, though its search API listed 4,319 in stock; the same upload accepted C2883765 with the same pad layout (`HDR-SMD_8P-P2.54-V-M-LS4.8_R2`), so J8's copper did not change. The API's stock figure doesn't say whether a part can be assembled: try it in the BOM step.
