# hifi-receiver-2: the plan

The work after [product.md](product.md), in order. Each item is sized for one
worker session and names what "done" looks like.

- **Boards:** every board is designed with the **hive-hwde** module: research,
  schematic, layout, checks and JLC package, gated stage by stage. Anything
  in hwde that gets in the way is filed as its own change to project
  hive-hwde, not worked around here.
- **Docs:** every task keeps `docs/` current. That means `product.md` when a
  decision moves, plus a page per board and per piece of software that says
  what it is, why it is that way, and how to build and test it.
- **Money:** any JLC or parts order goes to the person, never placed by a
  worker.

Lite is built first (why: product.md, "Which tier is built first"). Items 1–12
take Lite to a product that works. Items 13–20 do Plus on the `core` board.
Items 21–24 do Ultra.

## Lite

1. **Bench and score.** Done in brief 261028-141256: `tools/hwcheck.py`
   ([checks.md](checks.md)). The test re-runs every board's hwde gates, and
   the score is Lite's unit cost at 100 from `lite-main`'s own BOM (1000
   until that board passes its gates; the estimate isn't used as a
   stand-in).
2. **`lite-main` research and block diagram (hwde stages through
   architecture).** Fix the pin map on the WROVER-E against its strapping
   pins (GPIO0/2/5/12/15) and PSRAM (GPIO16/17):
   - ES8388 I²S plus MCLK on GPIO0
   - I²C
   - amp mute/fault
   - IR
   - 3 buttons
   - CH340C auto-program
   - CH224Q

   Settle the CH224Q's I²C and fallback behaviour from its datasheet, and
   check the ES8388's bypass path for the line in. Write
   `docs/boards/lite-main.md`. *Done:* hwde's architecture gate passed.
3. **`lite-main` schematic.** Covers the power tree (USB-C → TVS → CH224Q → soft-start PMOS → PVDD 20 V; SY8303 → 5 V; SY8089 → 3.3 V; LP5907 analog), the ESP32 with CH340C, the ES8388, and the TPA3118D2 stage reused from hwde's measured `stereo-class-d-amp` with its gain set for 20 V. It also includes the RCA in/sub-out, the banana sockets, IR and buttons. *Done:* the schematic passes hwde's schematic checks and the BOM is all LCSC parts, with `docs/bom-estimate.csv` updated to the real BOM.
4. **`lite-main` layout.** 2-layer, about 100 × 80 mm, with the connectors on
   the rear edge and buttons/IR/LED at the front. Keep the 3118's thermal
   copper as on the made board, and the antenna keep-out at a board edge.
   *Done:* DRC is clean, and hwde's layout and verification gates pass with
   no unsigned waivers. The made amp board was ordered with eight unsigned
   waivers; do not repeat that.
5. **`lite-main` JLC package and quote.** Gerbers, BOM, CPL and a rendered
   check, then a JLC quote at 10 and 100. *Done:* the package is ready and the
   quote is in `docs/boards/lite-main.md`. The order goes to the person.
6. **Firmware base.** Fork squeezelite-esp32 into `firmware/` and build it
   for the WROVER-E. Add a `lite-main` board config: the ES8388 on our pins,
   the buttons, IR, and amp mute/fault. *Done:* the build is reproducible
   (`firmware/README.md`), and on an ESP32-A1S-class dev kit, if the bench
   has one, it plays Spotify, AirPlay and BT.
7. **Firmware: power, bring-up and test API.**
   - PD contract readout from the CH224Q.
   - Amp held off below 9 V.
   - The power governor: a level-based limiter holding about 90% of the PDO.
   - mDNS `_hifi._tcp` and the HTTP test API: state, tones, sweeps,
     self-test.
   - Serial console commands.

   *Done:* the code is unit-tested where it can be, and the API is documented
   in `docs/test-api.md`.
8. **Web app (PWA).**
   - Volume, source, EQ, sub level, the input/DJ-direct switch and Wi-Fi
     setup, plus updates.
   - Served from the firmware's flash.
   - Installable on iOS and Android.

   *Done:* it works against the firmware or a mock of the test API, and
   screenshots are in `docs/app.md`.
9. **Bench scripts.** In `bench/`: find the board over USB or mDNS, flash it,
   run the self-test, then sweep power into 4/6/8 Ω dummy loads and log THD,
   power, PVDD and temperature. Use hwde's `bench.py` where it fits, and file
   gaps to hive-hwde. *Done:* the scripts run against a mock and are ready
   for the first board.
10. **Lite bring-up and test** (needs boards in hand, ordered by the person).
    - **Power up:** in stages, from a laptop port at 5 V, then from 65 W and
      100 W chargers.
    - **Streaming:** Spotify, AirPlay, BT, Lyrion, RCA in and DJ bypass.
    - **Amp:** power into 4 Ω against the 43 W prediction, and the **chip
      temperature at 1/8 power**, which was never measured on the made board.
    - **Sub outputs:** checked as well.

    *Done:* a results page, with every item measured or marked as failing.
11. **Lite enclosure.** In Onshape if the person grants access (an API key
    pair is a credential, so it goes through them). Otherwise CadQuery or
    OpenSCAD in `enclosure/`, from the board's STEP export.
    - PETG or ASA, printable without supports.
    - Rear cut-outs for USB-C, RCA and banana; front for buttons, IR and LED.
    - A label recess.

    *Done:* STL and STEP files are in the repo, with a fit check against the
    STEP board.
12. **User manual.** One manual for all three tiers, each section marked
    with the tiers it applies to: `docs/manual/user-manual.md`, the boxed
    `docs/manual/quick-start.md`, and `docs/manual/sources.md` (where each fact
    comes from and what is still TBD). Exported to PDF once the TBDs are closed:
    - what's in the box, and the charger and cable to use
    - connecting speakers, subs and the FLX4
    - Wi-Fi setup
    - each streaming service
    - buttons and remote learning
    - updates, troubleshooting and the safety/power notes

    *Done:* a reader with no context can set it up from the manual alone.

## Plus (the `core` board)

13. **`core` research and block diagram.**
    - **Pin map:** RMII with the 50 MHz clock in on GPIO0; I²S0 TX/RX for the
      Sabre and ES7243E; I²S1 TX as a slave for the PCM5102A; I²C, IR,
      encoder, INA180 ADC, amp control, and the zone B and FFC signals.
    - **To settle:** the ES7243E without MCLK, the ES9018K2M's S/PDIF mux, and
      whether the TPA3251 is pin-compatible with the TPA3221.
    - **28 V protection:** the over-voltage cut-out and inrush design.
    - **Doc:** write `docs/boards/core.md`.

    *Done:* hwde's architecture gate passed.
14. **`core` schematic, power and amplifier half.** USB-C 48 V-rated →
    CH224Q at 28 V EPR → soft-start, with the over-voltage cut-out → PVDD. Then
    the INA180 sense, two TPA3221 stages (zone B unpopulated on Plus) with
    their heatsink mounting, and the banana outputs. *Done:* hwde's schematic
    checks pass on this half.
15. **`core` schematic, signal half.** The ESP32 with CH340C, LAN8720A and
    magjack, the ES9018K2M with its oscillator and OPA1678 stage, the
    PCM5102A for the subs, the ES7243E with the TS5A23157 DJ-direct switch,
    and the RCA jacks. Also the optical footprint, the knob, buttons and IR,
    and the FFC to the front board. *Done:* the full schematic passes, and
    `bom-estimate.csv` is updated for Plus and Ultra.
16. **`core` layout.** 4 layers, about 120 × 100 mm:
    - **Placement:** the class-D stages and their 5 A loops away from the
      Sabre's analog ground.
    - **Heatsinks:** room above both TPA3221s.
    - **Edges:** the antenna at an edge; the rear edge for USB-C, RJ45, RCA
      and banana; the front edge for the knob and IR.

    *Done:* hwde's layout and verification gates pass.
17. **`core` JLC package and quote.** Two assembly variants from one PCB: Plus
    and Ultra populations. *Done:* the package and quote are ready, and the
    order goes to the person.
18. **Firmware for `core`.**
    - **Board config:** the Sabre, PCM5102A and ES7243E drivers, and Ethernet.
    - **DSP:** bass management (crossover, level, delay and phase per sub,
      optional high-pass on the mains), and the DJ-direct switch.
    - **Power:** the INA180-measured governor at 28 V and 20 V contracts.
    - **App:** the web app gains the sub and DSP pages.

    *Done:* it builds, and the logic is tested.
19. **Plus bring-up and test.** At 28 V EPR on a 140 W charger:
    - power into 4 Ω against the 77 W prediction
    - the governor holding the charger without dropouts on music and sine
    - the TPA3221 heatsink temperature
    - Ethernet, the Sabre's noise and THD, the sub crossover, and the
      fallback to 20 V

    *Done:* a results page.
20. **Plus enclosure and manual.** Same as items 11–12, with heatsink venting
    and the remote. *Done:* the files, and the manual's Plus TBDs closed.

## Ultra

21. **`ultra-front` schematic, layout and package.** 2-layer, about
    100 × 40 mm: LCD, knob, 3 buttons, IR, LED and the FFC. *Done:* hwde's
    gates pass and the package is ready.
22. **Ultra firmware.** The ST7789 now-playing UI, optical input via the
    Sabre's S/PDIF, zone B control, and the governor sharing power across 4
    channels. *Done:* it builds, and the UI is tested on the bench.
23. **Ultra bring-up and test.** The full `core` population plus the front
    board: optical, zone A+B under the 140 W budget, and the display.
    *Done:* a results page.
24. **Ultra enclosure and manual.** *Done:* the files, and the manual's
    Ultra TBDs closed.

## Running alongside

- **hive-hwde snags:** file each one to project hive-hwde the moment it
  bites. Don't wait for the board task to end.
- **Prices:** re-run `tools/bom_estimate.py` after each schematic, and refresh
  the prices before each order.
