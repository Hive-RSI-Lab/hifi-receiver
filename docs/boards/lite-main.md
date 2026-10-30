# lite-main: the Lite tier board

The Lite tier's only board. The design lives in [`boards/lite-main-main/`](../../boards/lite-main-main/):
its [README](../../boards/lite-main-main/README.md) covers what the board is, every design
decision and why, how to rebuild and re-check it, and how to test a built one.
[`HWDE-SNAGS.md`](../../boards/lite-main-main/HWDE-SNAGS.md) lists what hwde got wrong on
the way.

- **What it is:** a 2-layer, 100 x 90 mm board.
  - **Power:** USB-C PD at 20 V (CH224A).
  - **Streaming and control:** an ESP32-WROVER-E with a CH340C for console and
    flashing.
  - **Audio:** an ES8388 codec, wired like the ESP32-A1S kit so
    squeezelite-esp32 runs on it as is, and a TPA3118D2 amp (2 x 43 W into
    4 ohm at 20 V).
  - **Rear edge:** RCA line in, RCA sub out, USB-C and four 4 mm banana
    sockets for the speakers (J4-J7, fitted by JLC; 118 x 90 mm board).
  - **Front edge:** three buttons, IR and an LED.
- **Package:** `boards/lite-main-main/fab/` holds the Gerbers, BOM and CPL with LCSC
  numbers, ready for JLC's assembly flow. Nothing has been ordered; ordering is
  the person's.
- **Cost:** $14.54 per board at 100: parts, assembly and PCB
  (`python3 tools/hwcheck.py cost`).
  - The product page estimated $12.3. Most of the difference is the CH224A,
    the polymer bulk cap and the TVS/IR changes.
  - Since 10-30 the speaker outputs are board-mounted banana sockets (J4-J7):
    the board is $14.54 at 100 with them, and nothing is bought off-board.
- **Open before ordering:**
  - the RCA jacks' exact depth against a real part
  - the banana sockets' nose against the rear wall's 6.8 mm square holes
  - the IR receiver's lens direction

  All three are mechanical and only matter for the enclosure. Details are in
  the README.
