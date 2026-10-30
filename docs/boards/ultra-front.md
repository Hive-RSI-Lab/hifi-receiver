# ultra-front: the Ultra front panel board

The board that makes the Ultra tier's front panel: a 2.0" colour screen, the
volume knob, three buttons, the IR receiver and an RGB status LED, on a
2-layer 110 x 40 mm board with no processor of its own. The `core` board's
ESP32 drives it over a 20-pin 0.5 mm FFC.

- **Design, pinout, decisions, rebuild and bring-up test:**
  [`boards/ultra-front/README.md`](../../boards/ultra-front/README.md).
- **Fab package for JLC:** `boards/ultra-front/fab/` (Gerbers, BOM.csv,
  CPL.csv, prebuy.csv). Nothing is ordered: ordering is the person's.
- **Cost:** parts $2.42 per board at 100, plus the LCD ($2.43, plugged in at
  final assembly). `tools/bom_estimate.py` now prices Ultra at $35.36 a unit
  at 100.

## What the other work items need from it

- **`core` board (plan items 13-17):**
  - J1's pinout, the cable rule (pin N to pin N) and the ESP32-side notes:
    SPI at 15 MHz or less, series resistors at the source, and no strapping
    pins on the nets this board biases.
  - They are in the README's "The connector to the core board" section, and
    were sent to the core board's run.
- **Ultra firmware (item 22):**
  - **Expander:** U1 is a TCA9535 at I2C 0x27.
    - P00-P05 are the knob A, B and push, then BTN1-3, all active low.
    - P10 is the LCD reset.
    - P11-P13 are the LED's red, green and blue, active low. For off, drive
      green and blue high, and set red as an input.
    - The unused pins stay inputs.
  - **Interrupt:** INT is on FFC pin 14. The knob's A/B are also on pins
    16/17, for PCNT if the core wires them.
  - **LCD:** an ST7789, 320x240 over SPI. Its backlight is PWM-dimmable on
    pin 10.
- **Enclosure (item 24):** the README's "Where things are" table, and the STEP
  export.
