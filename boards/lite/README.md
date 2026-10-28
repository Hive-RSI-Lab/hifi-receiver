# lite-main: the Lite tier of hifi-receiver-2

The Lite tier's one board, as `docs/product.md` defines it. You plug a 65–100 W
USB-C laptop charger and a pair of speakers into it. It streams over Wi-Fi
(Spotify Connect, AirPlay, Lyrion) and Bluetooth, takes a DJ deck on RCA, and
drives two subwoofers from RCA sub outs. A web app on your phone, three buttons
or any IR remote control it.

It was designed with the hive-hwde module (KiCad 10), from schematic to JLC fab
package, on 2026-10-28. **Nothing has been ordered.** Ordering and paying are
the person's.

![top](reports/renders/lite-main-top.png)

## What it is

- **Board:** 2 layers, 1.6 mm FR-4, 1 oz, HASL, 100 x 90 mm, four M3 plated
  holes, 2 mm corners. 130 parts: 127 on the top side, SMT plus 3
  through-hole connectors; the 4 mounting holes are not BOM parts. JLC
  assembly with LCSC part numbers.
- **Rear edge, left to right:**
  - **J2:** RCA line in (FLX4). Top jack white = L, bottom red = R.
  - **J3:** RCA sub out L/R.
  - **J1:** USB-C.
  - **J4:** speaker terminal, L+ L- R+ R-. The enclosure's 4 mm binding posts
    are wired to it.
- **Front edge:** SW1 vol-, SW2 vol+, SW3 play/source (hold = BT pairing),
  the IR receiver U9 (top-looking: it needs a window above it) and a red
  status LED D2.
- **ESP32 antenna:** at the left edge. A copper keep-out on both layers sits
  under it.

| Block | Parts | Notes |
|---|---|---|
| USB-C + PD | J1 USB4105 (48 V/5 A), U1 CH224A, D1 SMBJ20A | asks for 20 V (Rset 120k); I2C 0x22 on the codec's bus, so firmware reads the contract |
| PVDD switch | Q1 AO4485, Q4 MMBT3904 | on only when GPIO18 is high **and** the PD contract is good (Q4's emitter is CH224A PG); Miller soft start ~6 ms, ~1.6 A inrush; R16 bleeder |
| 5 V | U2 TPS54302, L5 10 uH | TI design-example values |
| 3.3 V | U3 SY8089 (digital), U4 LP5907 (codec analog) | |
| MCU | U5 ESP32-WROVER-E-N8R8, U6 CH340C, Q2/Q3 | console + flashing over the same USB-C; auto-program on DTR/RTS |
| Codec | U7 ES8388 | pinned like the ESP32-A1S AudioKit (I2S MCLK 0, BCLK 27, LRCK 25, DOUT 26, DIN 35; I2C 32/33): squeezelite-esp32's board profile applies |
| Amp | U8 TPA3118D2, L1-L4 10 uH 8 A, C68-C71 1 uF | BTL, 26 dB, BD mode, no power limit; 2 x 43 W into 4 ohm at 20 V |
| Bulk | C58 470 uF polymer, C59 220 uF | |

ESP32 pin map:

| GPIO | Signal |
|---|---|
| 0 | MCLK |
| 4 | PD power-good |
| 13 | vol+ |
| 14 | IR |
| 18 | PVDD enable |
| 19 | play |
| 21 | amp SDZ (through 100k) |
| 22 | LED |
| 23 | amp fault |
| 34 | VBUS/11 |
| 36 | vol- |
| 1/3 | UART0 |

The full table is in [`architecture/blocks.md`](architecture/blocks.md).

## Design decisions, and why

Each one is also in `state.json` (`decisions`). The schematic review and what
was done about each finding is in
[`reports/review-schematic-response.md`](reports/review-schematic-response.md).

- **CH224A, not the CH224Q product.md named.** Same die and registers. The
  CH224Q's 2 x 2 mm, 0.4 mm-pitch DFN puts VBUS between CC1 and CFG1, right
  under the USB-C, and it couldn't be fanned out on 2 layers. The ESSOP-10 is
  1.0 mm pitch, same price, 9.5k in stock. The CH224K has 1 in stock.
- **The CC lines are crossed on purpose.** The CH224 treats CC1/CC2 alike, so
  J1's B5 goes to U1 pin 7 and A5 to pin 6. That way neither crosses the
  other or the D+/D- escape.
- **Speaker terminal instead of banana sockets on the board.** The GC4010-F has
  no PCB footprint anywhere (EasyEDA has no data) and is an RC bullet
  connector. A 16 A 5.08 mm screw terminal (DB128V) takes 4 mm-binding-post
  wires from the rear panel. product.md open item 4 is now settled.
- **Amp channels swapped at the chip.** The TPA3118's "right" channel drives
  the left speaker, so its output row runs L+ L- R+ R- straight into the
  terminal with no crossing. Nets are named by speaker channel.
- **PVDD needs firmware and the PD contract.** GPIO18 AND CH224A PG (hardware
  AND through Q4's emitter). A 5 V laptop port can never power the amp, and
  the firmware can drop it before a PD renegotiation.
- **5 V buck is a TPS54302**, not the SY8303 in the cost estimate: TI's
  datasheet design example gives every value. 5.18 V (100k/13k, Basic parts).
- **SMBJ20A TVS** (breakdown 22.2–24.5 V), not 24 V: it clamps nearer the
  30 V abs-max of the TPS54302 and TPA3118. A TVS can't hold a 20 V bus under
  30 V at its full rated surge; that residual risk is noted.
- **TPA3118 gain 26 dB.** The ES8388's full scale (~0.9 Vrms) x 20 clips at
  20 V into 4 ohm 3.7 dB before 0 dBFS, so firmware caps the codec volume.
  20 dB would leave only 20 W.
- **Power copper is pours, not tracks.** VBUS (J1 to Q1) and PVDD (Q1 down a
  F.Cu strip between the filter caps, a B.Cu pour under the inductors, F.Cu
  islands at both PVCC groups) carry up to 5 A, joined by 17 vias. The pours
  are in `kicad/gen/power.py`. Tracks only feed the small taps.
- **Amp OUT runs (pin to inductor) are 0.31 mm minimum** at the HTSSOP pins
  (0.65 mm pitch, 0.34 mm pins), sized for 1.0 A rms / 30 C. That is music at
  full power with a 9 dB crest factor. They are under 15 mm long. The
  inductor-to-terminal runs are 1.21 mm (3.5 A, 30 C).
- **USB is not impedance-controlled.** Full speed, 12 Mb/s; a 90 ohm pair on
  1.6 mm 2-layer would be 1.1 mm wide.
- **No USB ESD array** on the cost-first tier: the CH340C and CH224A have
  their own. Plus/Ultra should add a USBLC6-2SC6.
- **Board 100 x 90 mm**, not the ~100 x 80 product.md estimated: 80 left no
  room for the PVDD switch between USB-C and the amp.
- **Red LED (Basic part) at 1.3 mm.** The white one the first schematic had ran
  at 0.2-0.6 mA, too dim.
- **IR on GPIO14**, not 39: GPIO36/39 glitch with ADC/Wi-Fi (errata 3.11).
  vol- stays on 36; a polled, debounced button tolerates the glitch.

## The checks it passed

| Gate | Result |
|---|---|
| ERC (`erc`) | TBD |
| Placement (`place`) | TBD |
| Routed DRC (`drc_routed`) | TBD |
| Verify suite (`verify`) | TBD |
| JLC DFM on the exported Gerbers (`dfm`) | TBD |

TBD reviewers.

## Cost

TBD

## How to rebuild and re-check it

You need a Hive checkout with the hive-hwde module (`$HIVE_MODULES/hive-hwde`),
its KiCad 10 and Freerouting.

```sh
cd boards/lite
sh build.sh sch      # footprint repairs, schematic from kicad/gen/root.py, ERC gate, netlist, parts.json
sh build.sh board    # board_init, rules_gen, placement (kicad/gen/place.py), keep-outs, pours, power vias
sh build.sh route    # Freerouting (route_auto), stitch vias, drop vias under the hole-to-hole floor
```

`sh build.sh board` re-creates the board from scratch, so it throws the
routing away. Freerouting doesn't route the same way twice. The committed
`kicad/lite-main.kicad_pcb` is the run that passed, with the finishing steps
below applied. To re-check the committed board without rebuilding it:

```sh
H="sh $HIVE_MODULES/hive-hwde/bin/hwde"
for g in place drc_routed verify dfm; do $H gate --gate $g kicad/lite-main.kicad_pcb --workspace .; done
$H gate --gate erc kicad/lite-main.kicad_sch --workspace .
```

TBD finishing steps.

## How to test a built board

TBD

## Open items

TBD
