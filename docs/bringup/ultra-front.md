# Bring-up: Ultra front board (`boards/ultra-front`)

This plan is for a freshly assembled front board. Nothing in it has been on
hardware yet. Every value comes from the design files cited, except where it's
marked **estimate**.

The board has no processor. It is a TCA9535 expander (U1, 0x27), an
IRM-H638T IR receiver (U2), an RGB LED (D1), the EC11 knob (SW4), three
buttons (SW1-SW3), the backlight switch (Q1) and the LCD socket (J2). The core
board's ESP32 drives all of it over a 20-pin 0.5 mm FFC into J1. So the plan
has two halves:

- **A. On the bench, with no core** (steps 1-4): resistance checks, then
  current-limited power through a J1 breakout. These steps cover everything
  that needs only power: the rails, the idle current, the knob and buttons on a
  meter, the IR receiver on the scope and the backlight current.
- **B. On a working Ultra core** (steps 5-11): the expander over I2C, the
  buttons through the firmware, the LED, IR decoding, the LCD, and the open
  items.

Sources: `boards/ultra-front/README.md` (design, "How to test a built
board"), `boards/ultra-front/kicad/ultra-front.net`,
`boards/ultra-front/fab/BOM.csv`, `boards/core/kicad/core.net` (J3, the core
end), `firmware/components/hifi/esp/board_core.c` and `hifi_board_core.h`,
`firmware/board/ultra/sdkconfig.defaults`, `docs/firmware.md` ("Ultra's front
board"), `docs/test-api.md`, `docs/bench.md`.

## What you need

- A bench supply with two outputs, or two supplies: 3.3 V and 5.0 V, each with
  a current limit and a readout down to 1 mA.
- A multimeter and a scope (2 channels is enough).
- A 20-pin 0.5 mm FFC-to-0.1" breakout, and a 20-pin 0.5 mm **type B** FFC
  (opposite-side contacts), 30-50 mm long (`parts:64` in
  `docs/known-unknowns.md`; 50 mm is the enclosure length).
- An Ultra core board that has passed its own bring-up (`docs/bringup/core.md`),
  flashed with the Ultra build (`CONFIG_HIFI_ULTRA`), with its USB-C PD charger
  and USB console (`bench.py find`).
- Any NEC remote. A cheap "car MP3" remote is NEC.
- The LCD, HS20HS072RX (LCSC C5329582). JLC doesn't assemble it.
- For step 11 only: an ESD gun and the enclosure.

The dummy load isn't needed for this board.

## Reference: J1 pinout and the parts behind each pin

J1 (FPC-05F-20PH20, bottom contact) has the same pinout as core J3, pin N to
pin N. The rails are mirror-symmetric (1/20 GND, 2/19 +3V3, 3/18 +5V), so a
cable of the wrong type shorts no rail. It does put signals on the wrong pins:
BL_PWM lands on GND, SCK on ENC_A, and MOSI on INT.

| J1 pin | Net | On this board | Core end (core.net) |
|---|---|---|---|
| 1, 4, 6, 11, 20, 21, 22 | GND | | GND |
| 2, 19 | +3V3 | U1, U2 (via R10 100 R), pull-ups R1-R9, C1/C2/C4 | +3V3 |
| 3, 18 | +5V | D1 anode, backlight R13/R17/R18, C3 | +5V |
| 5 | LCD_SCK | J2.4 | GPIO32 via R402 33 R |
| 7 | LCD_MOSI | J2.5 | GPIO33 via R403 33 R |
| 8 | LCD_CS | J2.2 | R57 10 k to GND (CS held low) |
| 9 | LCD_DC | J2.3 | GPIO15 |
| 10 | LCD_BL_PWM | R12 100 R to Q1 gate, R11 100 k to GND | core expander U17 P06 (on/off) |
| 12 | I2C_SCL | U1.22 (no pull-up here) | GPIO14, R35 2.2 k pull-up |
| 13 | I2C_SDA | U1.23 (no pull-up here) | GPIO13, R34 2.2 k pull-up |
| 14 | FRONT_INT_N | U1 INT, R7 10 k pull-up | GPIO39 (EXP_INT), wired-OR with U17 INT, R36 10 k |
| 15 | IR_OUT | U2 VOUT, R9 10 k pull-up | GPIO35 (IR_IN), R58 10 k pull-up |
| 16 | ENC_A | SW4.A, R1 10 k, C6 10 nF, U1 P00 | U17 P12, R52 10 k, C37 10 nF |
| 17 | ENC_B | SW4.B, R2 10 k, C7 10 nF, U1 P01 | U17 P13, R53 10 k, C38 10 nF |

U1 TCA9535 (A0-A2 tied to +3V3, address 0x27). It has no internal pull-ups.

| Bit | Pin | Net | Pull | Meaning |
|---|---|---|---|---|
| P00 | 4 | ENC_A | R1 10 k up | knob phase A |
| P01 | 5 | ENC_B | R2 10 k up | knob phase B |
| P02 | 6 | ENC_SW | R3 10 k up | knob push, low = pressed |
| P03 | 7 | BTN1 (SW1) | R4 10 k up | low = pressed |
| P04 | 8 | BTN2 (SW2) | R5 10 k up | low = pressed |
| P05 | 9 | BTN3 (SW3) | R6 10 k up | low = pressed |
| P06, P07, P14-P17 | 10, 11, 17-20 | EXP_UNUSED | R19 10 k down (shared) | read 0 |
| P10 | 13 | LCD_RST (J2.6) | R8 10 k up | high = panel runs |
| P11 | 14 | LED_R, via R14 1 k to D1 R cathode | | low output = lit; off = **input** |
| P12 | 15 | LED_G, via R15 680 R | | low = lit, high = off |
| P13 | 16 | LED_B, via R16 680 R | | low = lit, high = off |

The registers: input 0x00/0x01, output 0x02/0x03, config 0x06/0x07
(1 = input). At power-up every pin is an input.

Every resistor is 1 % (BOM.csv: UniRoyal 0603 parts C25804, C22775, C25803,
C22787, C21190, C23228). That sets the tolerances below.

---

## A. On the bench, no core

### Step 1. Visual check

Look under a loupe, with no power and no cable:

- **U1 (TSSOP-24):** look for bridges, and check that pin 1 matches the silk.
- **J1 and J2:** check that the contact rows are soldered across their full
  width, with no bridges at 0.5 mm pitch. The lids open and close. On
  both, the lid's pull tab faces the mouth (README "Rotations").
- **Polarity and rotation of D1, U2, Q1, D2 and D3:** check them against the
  JLC placement preview and the silk. JLC rotations are a README open item;
  they were never checked visually.
  - D1 is a 1.6 x 1.5 mm 4-pad RGB part. A rotated D1 puts the wrong colour
    (or the anode) on each pad. The firmware would still run, but the LED
    would show the wrong colours. Step 8 catches it.
- **SW4 (EC11, through-hole):** all 7 pins and both frame tabs soldered.
- **The area under the LCD and along its tail's path:** no parts there
  (README "Keepouts").

**Fail:** rework before going on. If a fault would short a rail (a U1
bridge, a J1 pin bridge, a tab touching a pin), fix it before step 2.

### Step 2. Resistance checks, no power, no cable, no LCD

Use the multimeter in resistance mode on J1's pads (or the breakout's pins,
with the board plugged in and no supply connected). Wait for the reading to
settle: C2 (10 µF) on +3V3 and C3 (10 µF) on +5V make it creep upward.

| Measure | Expect | If not |
|---|---|---|
| +3V3 (pin 2) to GND (pin 1) | > 100 k with the knob in an open position. Down to about 5 k (10 k ∥ 10 k: R1, R2 through SW4's A/B contacts to GND) if the knob rests where A or B is closed. Turn it a little and read again. **Estimate:** the open-position value depends on IC leakage. | < 1 k: a shorted cap (C1, C2, C4), a U1 bridge, or a J1 pin-2/pin-1 bridge. Lift C2 first, then C1/C4. |
| +5V (pin 3) to GND (pin 1) | > 1 MΩ, settling slowly (C3). D1 is reverse-biased through U1's inputs, and Q1 is off. | < 10 k: C3 shorted, Q1 drain/source shorted with the LCD absent (BL_K is open then, so check C3 first), or a J1 bridge. |
| Pin 2 to pin 19, pin 3 to pin 18 | 0 Ω | An open rail pad: re-solder J1. |
| Pins 1, 4, 6, 11, 20 and the tabs, each to the others | 0 Ω | A GND pad open. |
| Pin 2 to pin 3 (+3V3 to +5V) | > 10 k **(estimate)** | A bridge between J1 pins 2 and 3. |
| Pin 10 (BL_PWM) to pin 1 | 100.1 k ±1 % (R12 100 R + R11 100 k) | Open: R12 or R11 missing. 100 R: R11 shorted, or Q1's gate shorted to source. |
| Pin 14 (INT) to pin 2 | 10.0 k ±1 % (R7) | |
| Pin 15 (IR_OUT) to pin 2 | 10 k or a little lower: R9, in parallel with U2's own output stage if it has a pull-up (**estimate** 7-10 k) | Open: R9 missing. |
| Pin 16 (ENC_A), pin 17 (ENC_B) to pin 2 | 10.0 k ±1 % (R1, R2) with the contact open | |
| Pin 16, pin 17 to pin 1 while turning the knob slowly | toggles between about 0 Ω (contact closed) and > 100 k (open) | Stuck open: SW4 A/B or C pin not soldered. Stuck at 0: a bridge or a damaged switch. |
| Pins 5, 7, 8, 9, 12, 13 to pin 1 | open (> 1 MΩ). With no LCD fitted, these pins go only to J2 (5, 7, 8, 9) and U1 (12, 13). | A low reading: a J1/J2 bridge. |
| R3 pad (ENC_SW), R4 (BTN1), R5 (BTN2), R6 (BTN3), the net side, to GND | > 100 k released. Pressing SW4/SW1/SW2/SW3 in turn gives 0 Ω on that pad only. | A wrong pad going to 0: a bridge. None goes to 0: switch pins 3/4 (GND) or 1/2 not soldered. |

No firmware is needed for this step.

### Step 3. First power, current-limited, through the breakout

1. **Check the breakout's orientation first.** With no power, plug the board
   into the breakout with the FFC. Measure breakout pin 14 to breakout pin 2:
   it should read 10 k (R7).
   - **0 or open:** the cable or breakout is mirrored. Breakout pin 14 then
     reaches J1 pin 7. Fix the cable before you rely on any signal pin.
   - The rails line up either way.
2. **Set the supplies before connecting:**
   - +3V3: 3.30 V, limit 50 mA.
   - +5V: 5.00 V, limit 100 mA.
   - GND to breakout pin 1/20.
   - Leave pin 10 (BL_PWM) unconnected; R11 holds Q1 off.
3. **Bring up +3V3 first, then +5V.** U1 is 5 V tolerant, so the order doesn't
   matter. Doing one at a time just isolates a fault.

| Measure | Expect | If not |
|---|---|---|
| +3V3 supply current, idle, knob in an open position | < 2 mA. **Estimate:** U2's supply current plus U1's standby. The README says "a few mA". Add up to 0.33 mA for each of ENC_A/ENC_B resting closed (3.3 V / 10 k). | At the 50 mA limit: a short that step 2 missed (a part that only shorts when powered, or U1 reversed). Switch off and feel for a hot part, or use a thermal camera. U1 and U2 are the suspects. 5-50 mA: U1 latched or damaged, or a solder bridge between a pull-up and GND (watch which button pad reads 0 V). |
| +5V supply current, idle | < 0.5 mA **(estimate)**. U1's pins are all inputs at power-up, so D1 is dark, and Q1 is off. | > 5 mA with D1 dark: Q1 is leaking (but BL_K is open with no LCD, so then look at C3 or a bridge). D1 lit at power-up: an LED cathode is shorted to GND or a U1 pin is damaged. |
| IR_VCC (C5 +) | 3.30 V minus U2's current × 100 R, so about 3.25-3.30 V **(estimate)** | Near 0: R10 open or C5 shorted. |
| U1 pin 24 (VCC) | 3.30 V | |
| LCD_RST (J2.6 or R8) | 3.30 V (R8 pull-up, P10 an input) | |
| BTN/ENC_SW pads (R3-R6 net side) | 3.30 V released, < 0.1 V pressed | |
| ENC_A/ENC_B (pins 16, 17), scope, turn the knob | 0 ↔ 3.3 V quadrature, 90° apart. Edges rounded by 10 k × 10 nF, τ = 100 µs. | One phase dead: SW4 pin or R1/R2. |
| INT (pin 14), scope, press any button | High at power-up. On the first press, it falls and **stays low**, because no master reads the port. Power-cycle to reset it. | It never falls: U1 is not running (check VCC, pin 1, solder), or R7 is open, or the scope is on the wrong pin. It's low at power-up: an EXP_UNUSED pin is floating (R19 open), so the TCA9535 sees a change on its own. |
| IR_OUT (pin 15), scope, press a remote key at 0.3-1 m | Idle 3.3 V; a burst of low pulses, the NEC frame (9 ms low lead-in, then 560 µs marks), about 67 ms per frame | Flat high: U2's VCC/GND, or U2 rotated. Flat low: U2 dead or rotated. |

No firmware is needed for this step.

### Step 4. Backlight, on the bench (fit the LCD)

The front board is the only place to measure the backlight current directly.
Inside the box it comes from core +5V with no meter in line.

1. Switch off. Fit the LCD into J2: the tail lies flat, contacts up, so J2
   contacts on both sides (README). Close the lid.
2. Power up as in step 3. **Check that +3V3 idle rises by only a few mA
   (estimate: the ST7789 asleep, before init).**
3. Connect breakout pin 10 (BL_PWM) to +3V3 through a jumper.

| Measure | Expect | If not |
|---|---|---|
| +5V supply current | 39-61 mA, about 50 mA typical (README: (5 − Vf 2.8-3.2 V) / 40 Ω, against the panel's 80 mA maximum). | ~0: Q1 not switching (gate below), the tail not seated, BL_A/BL_K swapped on the panel (the datasheet doesn't show which face of the sub-flex carries them; the open item), or R13/R17/R18 missing. > 80 mA: hits the limit; a BL_A/BL_K short to GND or the sub-flex touching R13. **Switch off at once** (see step 10, the sub-flex item). |
| +5V to BL_A (J2.10) | 1.56-2.44 V; 2.0 V typical (that current × 40 Ω) | |
| BL_K (J2.11, Q1 drain) to GND | < 50 mV **(estimate)**: AO3400A on at Vgs 3.3 V | Several volts: Q1 isn't on. Check the gate. |
| Q1 gate (R11 or R12 pad) | 3.30 V × 100 k / 100.1 k = 3.30 V | |
| Each of R13, R17, R18, by touch or thermal camera | Warm, each about 1/3 of the current (about 17 mA, 35 mW) | One cold: that resistor is open. Current is then 2/3. |

Leave the backlight on for 5 minutes and check that the current stays steady.
Remove the jumper; the current should fall back to the step-3 idle.

No firmware is needed for this step.

---

## B. On an Ultra core board

From here on the board is plugged into a working Ultra core with the type B
FFC. Power comes from the core's PD charger, so there is no current limit on
the front's rails. Steps 2-4 have already shown they aren't shorted.

**Before plugging in:** with the core unpowered, check the cable as in step 3.
J3 pin 14 to front J1 pin 2 should be 10 k ∥ core R36 10 k ≈ 5 k, not open.
For the core-side numbers, see `docs/bringup/core.md`.

### Step 5. Boot with the front board connected

Connect the core's USB console (`python3 bench/bench.py find`, 115200 8N1), then
power the core from the PD charger.

| Check | Expect | Firmware or console | If not |
|---|---|---|---|
| Boot log | `front board at 0x27`. The display init then logs the ST7789 (squeezelite). | `board_core.c` `front_init()` | `front board: no TCA9535 at 0x27 (the FFC?): no buttons or LED`: the cable (type, orientation, lid not closed), U1's supply (J1 pin 2, U1 pin 24), or SDA/SCL. Scope J1 pins 12/13 during boot for I2C traffic; there should be 0x27 address frames with an ACK. |
| Self-test | `front_i2c` pass, `"0x27 acks"` | `hifi selftest` / `bench.py selftest` / `POST /api/v1/selftest` | `"0x27 doesn't answer (the FFC?)"`: as above. Also check the core's own `expander_i2c` passes: a dead bus fails both. |
| I2C bus levels, scope at J1 pins 12/13 | Idle 3.3 V. Rise time 10-90 % below 300 ns at 100 kHz, or below 1 µs in standard mode. The core's 2.2 k pull-ups drive the bus, and the FFC adds a few pF (`digital:36`). **Estimate.** | Slow edges: the core pull-ups. |
| D1 at power-up | Blue (starting), then green with a network or blue blinking without | `front_led()` in the controls task | No light: see step 8. |
| Backlight | On for the first 60 s after boot (`now < FRONT_DIM_MS` in `board_core.c`), then off unless audio plays or a control was touched in the last 60 s. `hifi backlight on` / `off` forces it, `auto` gives it back. | `exp_set(EXP_LCD_BL, …)` | Never on: step 9. |
| INT (pin 14, GPIO39), scope | High at rest. Not stuck low. | the controls task reads port 0 and 1 on every INT or every 10 ms | Stuck low: the core's U17 or the front's U1 isn't being read. This was R3-DIGITAL-3 before the fix. If `front_i2c` fails, U1's INT can stay low for good; the 10 ms poll keeps the core working, but GPIO39 is useless until it's fixed. |

### Step 6. Buttons and knob through the firmware

Run `hifi state` (or `api state`) before and after each action. Besides
the effect, `controls.front` shows U1's port 0 (bit 0 low while a button is
held, and so on; read it with the control held), `controls.last` names the
control the firmware saw (`"button play"`, `"knob -1"`), and
`controls.expander` the core's port with the knob's ENC_A/B.

| Action | Expect in `hifi state` / on the box | If not |
|---|---|---|
| Turn the knob clockwise, then counter-clockwise | `audio.volume` goes up, then down | Wrong direction: swap `enc_a`/`enc_b` in `board_core.c` (`docs/firmware.md`, an open bring-up item). Dead: the knob is read by **core U17 P12/P13**, not the front's U1. So check J1 pins 16/17 on the scope (step-3 waveform), then the core side (R52/R53, U17). Buttons working with the knob dead points at the ENC lines; the knob working with the buttons dead points at U1/INT/I2C. |
| Push the knob (short) | `audio.playing.state` play ↔ pause (needs a source playing) | No change: scope the R3 pad (ENC_SW). 3.3 → 0 V: firmware side (bit P02, `FRONT_ENC_SW`). Stays at 3.3 V: SW4.D/E. |
| Hold the knob | Standby (`power.state`) | |
| BTN1 (SW1) short | `audio.input` steps to the next input | Scope the R4 pad. |
| BTN1 hold | `audio.mute` true | |
| BTN2 (SW2) | previous track (needs a stream) | Scope the R5 pad. |
| BTN3 (SW3) | next track | Scope the R6 pad. |
| Any control | Backlight on, off again 60 s later with nothing playing (`FRONT_DIM_MS`) | |
| INT, scope on pin 14 while pressing | A low pulse per change, cleared within about 10 ms (the read) | Stays low: the read isn't reaching U1. |

`bench.py bringup` stage 5 asks "the buttons" as one y/n by hand. Use the
table above to fill it in.

### Step 7. Red-off and INT noise

The red die's cathode is an input when off (README B8), so its level isn't
driven. Check it doesn't make the expander chatter.

| Measure | Expect | If not |
|---|---|---|
| INT (pin 14) on the scope, 10 s, nothing touched, LED green | No pulses. Blinking blue or red adds one INT per blink step, which is expected: P11's input level changes with its config. | Steady pulses with nothing touched: an input is floating (P06/P07/P14-P17 on R19), or the red cathode's level sits near U1's threshold. Measure P11 (R14's U1 side) with red off. **Estimate:** it should sit well above 2.3 V (0.7 × VCC). |
| D1 red die, LED green or blue, in a dark room | Red fully dark | A faint red glow: P11 is being driven high instead of set as an input. That's a firmware bug in `front_led()`: `cfg1` bit 1 must be 1. |

### Step 8. LED

`hifi led test` steps D1 red, green, blue, white, a second each, for 30 s
(`hifi led auto` ends it). Check each die lights alone, and white has all
three. The states it shows on its own:

| State | How to get it | Expect |
|---|---|---|
| Blue, steady | the first moments after boot | blue only |
| Blue, blinking | no network (unprovisioned core) | blue blinks; green and red dark |
| Green, steady | join Wi-Fi (`bench.py bringup --wifi`) | green only |
| Red, blinking | amps latched off by faults: hard to cause on purpose on the bench | red blinks |

**Expected currents (estimate),** worked from the BOM resistors and typical
die voltages (the TJ-S1615 datasheet isn't in the repo):

| Die | Calculation | Current |
|---|---|---|
| Red | (5 − about 2.0 V) / 1 k | about 3 mA |
| Green, blue | (5 − about 2.9 V) / 680 R | about 3 mA |

To check one, measure the drop across R14, R15 or R16 with that colour lit:
about 3 V across R14, and about 2 V across R15 or R16 **(estimate)**.

| Fail | Likely cause |
|---|---|
| The wrong colour for the state | D1 rotated (step 1) |
| One colour dead | That resistor, D1's pad, or a U1 pin. Scope the U1 side of R14, R15 or R16: low while lit, high (green/blue) or floating high (red) while off. |
| All dead | D1's anode pad (+5V) |

Red can't be checked without latching the amps. Until a test command exists,
either get the red state by a deliberate amp fault on the core bench, or
check red with a temporary firmware build.

### Step 9. IR

| Check | Expect | Firmware or console | If not |
|---|---|---|---|
| Press a key on an NEC remote at 1-3 m in front of the board | `ir.last_code` in `hifi state` changes to the key's code (e.g. `0x00ff40bf`) | squeezelite `set_GPIO 35=ir:nec`; `hifi state` | No change, but J1 pin 15 pulses on the scope: the core side (GPIO35, R58). Nothing on pin 15: U2 (step 3). |
| Learn a key | `hifi ir learn volup`, press the key: `ir.learned` lists `volup`, and the key then moves `audio.volume` | `hifi ir learn <action>` / `POST /api/v1/ir/learn` | |
| Range and angle, in the enclosure behind its window | Works from the listening position (README: U2 "needs a window"). **No number in the design files.** Record the distance and angle at which it stops working. | | |

Core U18 (its own IR receiver) is left off the Ultra build (it's not in
`boards/core/fab/ultra/CPL.csv`), so IR_IN carries only the front's U2. If a
core has U18 fitted, the two outputs fight on one line. Take it off.

`bench.py bringup` stage 5 asks "the IR remote" y/n. Use the rows above.

### Step 10. LCD

The LCD is squeezelite's own display. On the Ultra build the strings are:

- `CONFIG_SPI_CONFIG="data=33,clk=32,dc=15"`
- `CONFIG_DISPLAY_CONFIG="SPI,driver=ST7789,width=320,height=240,cs=-1,mode=3,speed=13333333,rotate"`

The display init calls `hifi_display_reset()` first.

| Check | Expect | If not |
|---|---|---|
| LCD_RST (J2.6), scope, trigger on the falling edge at boot | One low pulse of 10 ms, then high, then 120 ms before any SPI | No pulse: U1's P10 isn't being driven (`front_i2c` failing?). It's low for good: P10 is stuck as an output low, or R8 is open. |
| SCK (J1 pin 5 or J2.4), scope | Bursts at the configured clock. **The design limit is ≤ 15 MHz (ST7789 66 ns write period, README).** The defaults set 13.33 MHz (80 MHz / 6). Measure the actual period: it must be ≥ 66 ns; expect 75 ns. | |
| SCK and MOSI edges at J2 | Clean, with overshoot below about 0.5 V **(estimate)**, through R402/R403 33 R at the core end | Heavy ringing: the SPI ground return is a README open item. Fine at ≤ 15 MHz. Lower the speed with `nvsset` on `display_config`. |
| DC (J1 pin 9) at boot | No pull on the front board. Check that GPIO15 (a strapping pin) still boots normally with the panel fitted: the boot log is present. | No boot log with the panel fitted but fine without: the panel is pulling DC. Tell the coordinator. |
| Image | Squeezelite's screen (now playing or idle), the right way up, correct colours | Mirrored or rotated: add `HFlip` or `VFlip`, or drop `rotate`, in `display_config` (NVS, `nvsset`; `docs/firmware.md`). Colours swapped red↔blue: colour order, an open item in `docs/firmware.md`. Blank with the backlight on: no SPI (scope SCK/MOSI/DC), the panel's tail not seated, or a missing reset (row 1). Snow or garbage: SPI too fast, or bad contact. |
| Backlight with playback | On while audio plays (`hifi tone sine 1000 -20` is enough), off 60 s after it stops | |
| Backlight voltage in the box: +5V to BL_A (J2.10) | 1.56-2.44 V (39-61 mA, the same as step 4) | |

Record the SPI clock, orientation and colour order you end up with. The
orientation and colour order are listed open in `docs/firmware.md`.

### Step 11. Open items to measure at bring-up

These come from the README review items and `docs/firmware.md`. Each is
unknown until a built board answers it.

1. **JLC rotations** (README): J1, J2, D1, U2, SW4 and U1. Check them in step 1
   and by function: D1's colours in step 8, U2 in steps 3 and 9.
2. **The backlight sub-flex over R13** (README): the LCD's backlight sub-flex
   passes 0.13 mm over R13. The datasheet doesn't show which face carries
   its K/A. With the panel fitted and pressed into place:
   - measure the +5V current with the backlight on (step 4) and off;
   - check with a meter, panel fitted and unpowered, that nothing exposed on
     the sub-flex reads 0 Ω to R13's pads.
   - Any contact: put a strip of Kapton under the tail (the fallback) and
     record it.
3. **The front-panel height stack** (README): the LCD (2.05 mm), the button
   caps, the knob and the panel. Check the fit in the enclosure: each button
   clicks through the panel, and the knob turns without rubbing.
4. **SPI clock** (step 10): measured at ≤ 15 MHz after the speed fix, with a
   clean image.
5. **LCD orientation and colour order** (step 10).
6. **The knob's direction** (step 6).
7. **IR window range** (step 9).
8. **ESD** (README step 7): in the enclosure, 8 kV air discharge on each button
   cap (SW1-SW3) and on the knob, 10 strikes each. After each strike, check
   that:
   - the `hifi selftest` `front_i2c` item still passes;
   - every button and the knob still act (step 6);
   - the LCD still shows an image.
   D2/D3 clamp BTN1-3 and ENC_SW. ENC_A/B have only 10 nF and the core's parts.
   **Fail:** note which strike and what stopped. A lock-up that a reboot
   clears is a firmware item (retry the I2C, re-init the front). Lasting
   damage is a hardware item: tell the coordinator.
9. **Cable length:** run steps 5, 6 and 10 on the 50 mm enclosure cable. If
   they were run on a shorter one, repeat them on it.

---

## Firmware gaps

These steps needed firmware, console or test-API support that didn't exist at
the head this plan was written against. None of it is needed to order the
boards. 1-4 and 6 are now closed (`docs/firmware.md`); 5 and 7 are open.

1. **Closed: `hifi i2c <addr> <reg> [val]`** (console only). Was: no raw I2C read/write over the console or API. The README's step 3
   reads U1's port 0 and checks bits 0-5. Neither `hifi` nor `api` can read
   or write a register at 0x27, and nothing in the repo shows
   squeezelite-esp32's console has `i2cget`/`i2cset`. Wanted: a command that
   reads and writes a register at an I2C address, e.g.
   `hifi i2c <addr> <reg> [value]`.
2. **Closed: `controls.front` and `controls.last` in `hifi state`.** Was: no readout of the front's controls. `hifi state` has no field for
   port 0's bits (ENC_SW, BTN1-3) or the knob count, so steps 6 and 11.8 go by
   side effects. BTN2/BTN3 need a playing stream to show anything. Wanted:
   `state.front: {"present": true, "buttons": "0x3f", "int": false}`, or the
   raw bits in `hifi state`.
3. **Closed: `hifi led test|auto`.** Was: no LED test. Red only lights when the amps latch off. Wanted:
   `hifi led r|g|b|off` (and an API equivalent) that overrides the status
   colour for a few seconds.
4. **Closed: `hifi backlight on|off|auto`.** Was: no backlight control. The backlight follows play state and touches only.
   Wanted: `hifi backlight on|off|auto`.
5. **No LCD test pattern.** The image check relies on squeezelite drawing
   something. Wanted: a colour-bar or RGB-fill command, to judge colour order
   and dead pixels.
6. **Docs and defaults disagreed (fixed).** The SPI speed is now 13.33 MHz
   in `sdkconfig.defaults` and `docs/firmware.md`. The backlight's on-time
   after boot is now 60 s by the clock (`now < FRONT_DIM_MS`); it was a
   loop count (`tick < 6000`, up to 60 s at the 10 ms poll, less when
   INT fires).
7. **`bench.py` has no front-board stage.** Stage 5 asks "the buttons" and "the
   IR remote" as two y/n questions. A front stage needs gaps 1-3. It would:
   - read the port bits while asking for each press;
   - drive each LED colour and ask y/n;
   - check `ir.last_code` changes.
