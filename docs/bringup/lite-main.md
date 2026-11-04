# Bring-up plan: Lite (lite-main)

This plan takes a freshly assembled lite-main board from the bag to a
measured, working receiver. **No lite-main has been built or powered yet.**
Every expected value below comes from the design files (`boards/lite-main/kicad/*.kicad_sch`,
`lite-main.net`), the datasheet values recorded in
[known-unknowns.md](../known-unknowns.md), and the firmware
(`firmware/components/hifi/`). A value marked **(estimate)** is my own
arithmetic or judgement and was not taken from a datasheet. Confirm or
correct it on the first board, then update this page.

Related pages: [boards/lite-main/README.md](../../boards/lite-main/README.md),
[bench.md](../bench.md) (the scripted bring-up), [test-api.md](../test-api.md)
(the console and HTTP calls), [firmware.md](../firmware.md),
[known-unknowns.md](../known-unknowns.md).

## Equipment

- **Bench supply:** 0–25 V with a current limit you can set at 50–500 mA.
- **USB-C PD charger:** 65–100 W with a 20 V PDO. A second and third charger
  from other brands are needed for step 9.4.
- **USB-C power breakout:** a breakout or "USB-C trigger/sniffer" board, so
  the bench supply can feed VBUS/GND straight into J1. An inline USB-C power
  meter is also needed: the board has no current sense (`vbus_i_a` is
  always null on Lite).
- **A laptop with USB-C or A-to-C:** used for the console (CH340B, 115200
  8N1), `bench/bench.py`, and esptool.
- **Multimeter, ideally true-RMS AC.**
- **Scope:** 2 channels or more, ≥ 50 MHz, with 10× probes. A short ground
  spring is needed for BCLK.
- **Dummy loads:** 2 × 4 Ω rated ≥ 50 W (the 1 % power is about 43 W/ch).
  Also 6 Ω and 8 Ω if `sweep` covers them.
- **Thermocouple:** fixed to the top of U8 (Kapton tape) for the soak.
- **Line source:** an RCA line source (phone with a DAC, or a signal
  generator at 1 kHz, 0.5–1 Vrms).
- **Remote:** any NEC IR remote.
- **Optional:** a USB audio interface and two 10k/1k dividers for
  `bench.py sweep` (wiring in [bench.md](../bench.md#wiring)).

## Safety and handling

- **The amp is bridge-tied (BTL).** Neither speaker terminal is ground, and
  each leg idles at PVDD/2 (≈ 10 V). Never put a scope probe's ground clip on
  J4–J7. Measure each leg against board GND (the RCA shell or a C-pad GND),
  or use a differential probe or the meter across the load.
- **PVDD holds charge.** It has about 0.75 mF of bulk (C58 470 µF, C59
  220 µF, C51–C57) and only R16 22k to bleed it. τ ≈ 15 s, so it takes
  over a minute to fall below 1 V **(estimate)**. Before touching or reworking
  the amp area, check C58+ with the meter, or discharge it through 100 Ω.
- **When the bench supply drives VBUS, nothing else may be plugged into J1.**
  A laptop on the same port would be back-fed.
- **Never set the bench supply above 21 V.** D1 SMBJ22A has VRWM 22 V and
  VBR 24.4–26.9 V. 21 V is PD 20 V + 5 %.
- **USB-C has no reverse protection, but the bench breakout does.** Check
  the polarity of the breakout before the first connection.

## Where to probe

The board has no test points. Probe on the pads below. "C7" means the
non-GND pad of C7. All GND pads are the same net. The RCA shells (J2/J3
pin 2) make the easiest GND clip.

| Net | Pad | Expected (powered) |
|---|---|---|
| VBUS | C3, C4 or C5 (non-GND pad), or D1 cathode | 5 V laptop; 20 V on PD |
| PVDD | C58 + | 0 V until enabled, then ≈ VBUS |
| +5V | C7 | 5.18 V on PD |
| +3V3 | C12 | 3.33 V |
| +3V3A | C16 | 3.30 V |
| VBUS_SENSE (GPIO34) | C17 | VBUS / 11 |
| PVDD_SENSE (GPIO39) | C18 | PVDD / 11 |
| /PD_PG (GPIO4) | R4 pin 1 or U1 pin 10 | 3.3 V no contract, ≈ 0 V good contract |
| /PSW_G (Q1 gate) | Q1 pin 4 | = VBUS off; VBUS − 6.25 V on (20 V in) |
| /AMP_GVDD | C50 | GVDD (≈ 5 V, see 6.1) |
| /AMP_GAIN | R50 pin 1 | GVDD × 20/120 |
| AMP_SDZ | C26 pin 1 | 0 V off, ≈ 3.3 V on |
| AMP_FAULTZ (GPIO23) | R53 pin 2 / U8 pin 3 | 3.3 V (no fault) |
| ES8388 VMID / VREF | C35, C39 | ≈ AVDD/2 ≈ 1.65 V **(estimate)** |
| MUTE_RES | C90 pin 1 | ≈ 4.5 V **(estimate)** |
| /MUTE_DET (Q12 base) | Q12 pin 1 | +5V × 22/142 ≈ 0.80 V, clamped to a Vbe |
| SUB_MUTE | Q12 pin 3 | ≈ 0.1 V while +5V is up |

Pin numbers come from the netlist. Check the pin-1 mark on the part before
probing an IC pin. Use the pads where you can.

---

## 0. Visual (no power)

1. **Orientation:** D1 (cathode bar to VBUS), C58 and C59 (+ marked, to
   PVDD), D2 (LED), U1, U2, U3, U4, U5, U6, U7, U8 (pin-1 dot) and Q1
   (SO-8, pin 1 at the source side).
2. **Solder:** look at J1 (USB-C, 0.5 mm-pitch signal row). Look for bridges
   on the A6/A7/B6/B7 pins and CC.
   - U8 (TSSOP-32 with exposed pad): check for bridges between the output
     pins 21/23/27/29 and their neighbours.
   - U7 (QFN-28) and U1 (DFN-10): look at the side fillets.
3. **Mechanical:** the RCA jacks J2/J3 must sit down on their pegs. The
   banana sockets J4–J7 must sit flush, face past the rear edge.
4. **Fit:** R2 and R3 (0R I2C links to the CH224A) are fitted. R1 is 120k
   (the 20 V request). R6 is 220k and C19 4.7 µF (the PVDD soft start).

**On a fail:** rework before power. A bridge on J1's CC or VBUS pins can put
20 V on the CC lines.

## 1. Resistance to GND (no power)

Take each reading with the meter's black lead on GND. Wait a few seconds:
the bulk caps charge from the meter, and the reading climbs until it
settles. Then swap the leads and read again; the two must roughly agree.
The values are the board's own resistors with the ICs left out. An IC on
the rail usually lowers the reading, so take the ones below as upper
bounds **(estimate)**.

| Rail | Pad | Expected | Fail if |
|---|---|---|---|
| VBUS | C3 | ≤ 110k (R13+R14 ∥ R5-path ∥ ICs) | < 1 kΩ |
| PVDD | C58+ | ≈ 18.3k (R16 22k ∥ R17+R18 110k); climbs slowly (0.75 mF) | < 1 kΩ |
| +5V | C7 | ≈ 63k or lower (R9+R10 113k ∥ R80+R81 142k ∥ U3/U4 inputs) | < 500 Ω |
| +3V3 | C12 | tens of kΩ or lower (pull-ups R4, R20–R23, R53, R60–R63 run to it, plus R11+R12 183k and the ESP32/CH340/ES8388 rails) | < 100 Ω |
| +3V3A | C16 | high, falls with the ES8388 AVDD/HPVDD | < 100 Ω |
| GVDD | C50 | high (R51+R50 120k) | < 100 Ω |

**Any rail under about 10 Ω is a short. Don't power the board.**
- On VBUS: check D1 (fitted backwards conducts at about 0.7 V in one meter
  direction), J1, C1/C3–C5, C19.
- On PVDD: check C51–C59 and the U8 PVCC pins.
- On +5V: check C7–C11, C15, C25 and U2's SW pin to the FB divider.
- On +3V3: check C12–C14, C20–C24, C30, C31 and U5.

**Q1 off when unpowered:** VBUS to PVDD (C3 to C58+) should read high in
both directions, apart from the cap charging. A reading of a few hundred
ohms or less means Q1 is fitted backwards or shorted.

## 2. First power: bench supply on VBUS, no PD

These checks run with the bench supply on the USB-C breakout and nothing
on J1 (no data). The CH224A negotiates nothing, so PG stays high (no
contract) and the hardware AND of Q4 must hold PVDD off at any VBUS. This
is the one test that proves that AND in hardware: the firmware cannot
override it. The ESP32 boots whatever is in its flash. On a fresh module
that is nothing, and GPIO18 floats or is low.

### 2.1 5 V, 150 mA limit

The idle current at 5 V should be under about 100 mA **(estimate)**: ESP32
idle or boot-looping, CH340 unpowered without USB data, codec unconfigured.
Set a 150 mA limit and raise the voltage from 0 to 5.0 V.

| Measure | Pad | Expected |
|---|---|---|
| Supply current | supply readout | < 100 mA **(estimate)**; Wi-Fi bursts if firmware is already in flash |
| +5V | C7 | ≈ VBUS − 0.2 to 0.4 V **(estimate)**. On 5 V the TPS54302 (UVLO 4.1 V) runs in dropout. |
| +3V3 | C12 | 3.33 V ±3 % (0.6 V × (1 + 150k/33k); SY8089 VFB 0.6 V) |
| +3V3A | C16 | 3.30 V ±2 % (LP5907-3.3) |
| PVDD | C58+ | 0 V (< 0.1 V) |
| /PD_PG | R4.1 | 3.3 V (no contract) |
| Q1 gate | Q1.4 | = VBUS |

**On a fail:**
- The supply hits the limit: power off. Feel for a hot part, then repeat
  the step 1 resistances.
- +5V is 0: U2 EN floats (on), so check its VIN/SW/BOOT soldering, L5, and
  the FB divider R9/R10.
- +3V3 is 0 or low: check U3's EN (tied to +5V), L6, and R11/R12.
- +3V3A is 0: check U4's EN pin (tied to +5V).
- PVDD above 0: Q1 is shorted or reversed. Or Q4 is conducting with PG high,
  in which case check that Q4's emitter goes to /PD_PG and not to GND.
  **Stop:** the 5 V protection depends on this.

### 2.2 12 V and 20 V, 100 mA limit

Raise the supply to 12 V, then 20.0 V. Stay at or under 21 V.

| Measure | Pad | Expected |
|---|---|---|
| Supply current at 20 V | readout | < 40 mA **(estimate)**: about 5 V × 100 mA / 0.9 / 20 V |
| +5V | C7 | 5.18 V ±2 % (0.596 V × (1 + 100k/13k)) |
| +3V3, +3V3A | C12, C16 | unchanged from 2.1 |
| VBUS_SENSE | C17 | 20 V / 11 = 1.82 V ±2 % |
| PVDD | C58+ | **0 V**: PG is high with no contract, so Q4 has no emitter path |
| Q1 gate | Q1.4 | = VBUS (Vsg 0) |
| U1 CFG2 / CFG3 | U1.2 / U1.3 | record them now and in step 4; see open item 9.3 |

**On a fail:**
- +5V is high: R9/R10 are swapped or the wrong values.
- **PVDD rises on a non-PD supply:** this hardware AND is the only thing
  that keeps a non-PD supply off the amp. Check Q4's orientation and that
  PG really is high at R4.1. If PG is low with no contract, U1 is faulty or
  something on its pins is shorted.

## 3. ESP32: flash, first boot, console (laptop USB, 5 V)

Disconnect the bench supply. Connect J1 to the laptop with a USB-C data
cable.

1. **Enumeration:** the CH340B shows up (`python3 bench/bench.py find`, or
   `lsusb` shows 1a86:7523, plus a new `/dev/ttyUSB*`).
   - **Fail:** check D7/D8 (USB ESD on USB_DP/DM) for shorts, U6's
     solder, and the J1 D+/D− routing. Also check U6's VCC/V3 (both on
     +3V3).
2. **Flash:** `python3 bench/bench.py flash firmware/build` (an IDF build
   dir with `flash_args`, or a merged `.bin` at 0x0; `--dry-run` shows the
   esptool line). Auto-program through Q2/Q3 needs no buttons.
   - **Fail with "Failed to connect":** scope GPIO0 (U5) and EN during the
     esptool reset. GPIO0 must be low as EN rises
     ([known-unknowns digital:3](../known-unknowns.md)). Check Q2/Q3 and
     whether MCLK on GPIO0 holds the strap (see 5.3).
3. **Console:** use 115200 8N1 on the same port. The squeezelite-esp32 boot
   log appears.
   - `hifi info` prints the board (`lite`), the firmware version and the
     MAC address.
   - `hifi state` reports `power.state` as 5 V or no contract,
     `power.vbus_v` ≈ 5.0 (4.75–5.25), `power.pg` false, `power.pvdd_on`
     false, and `power.pvdd_v` < 0.5.
4. **Self-test:** run `hifi selftest`, or `python3 bench/bench.py selftest`
   (bench.md stage 1).

   | Check | Expected on a 5 V port |
   |---|---|
   | `pd_sink_i2c` | pass, "0x22 acks". A fail means R2/R3, the U1 pins 2/3, or the pull-ups R20/R21 |
   | `pd_contract` | **fail, expected**: no PD on a laptop port |
   | `vbus_matches` | pass |
   | `codec_i2c` | pass, "0x10 acks". A fail means U7: CE on GND, DVDD on +3V3, solder |
   | `pvdd_on`, `pvdd_sensed`, `amp_no_fault` | fail, expected (the amp is refused on 5 V) |
   | `psram` | pass, 8 MB (WROVER-E N8R8) |
   | `heap` | pass, > 20000 B |
   | `network` | fail until 8.1 |

   `bench.py selftest` also asks for the amp and **must get a 409**. Check by
   hand too: `hifi amp on` must leave PVDD (C58+) at 0 V.

## 4. PD contract and PVDD (charger)

Now the charger. The board's only USB port carries power, so the console
is gone. Connect the board to Wi-Fi first (8.1, `api wifi`) so that
`bench.py` reaches it over mDNS. Or do this step with only the meter and
the scope, and read `state` after it.

Put the inline USB-C meter between the charger and J1. Leave the speaker
terminals open for this step.

| Measure | Where | Expected |
|---|---|---|
| VBUS | C3 | 20.0 V ±5 % (R1 120k = 20 V, WCH CH224 V2.0 table 5-1) |
| /PD_PG | R4.1 | low (< 0.4 V): good contract |
| `power.contract` | `hifi state` / `GET /api/v1/state` | `protocol` pd, `v` 20, and `i_max_a` the charger's 20 V PDO (3.25 A for 65 W, 5 A for 100 W with a 5 A cable) |
| `power.vbus_v` | state | 20.0 ±0.5 |
| CH224A registers | I2C 0x22, reg 0x09 (status) and 0x50 (current) | PD active; the PDO current. **No console op reads these directly. See the gaps list.** `contract` is the firmware's decoding of them. |
| `selftest` | `pd_contract`, `vbus_matches`, `pvdd_on`, `pvdd_sensed`, `amp_no_fault` | all pass. `pvdd_sensed` needs PVDD ≥ 0.85 × VBUS. |

**PVDD soft start (scope):**
- CH1 on Q1.4 (gate) and CH2 on C58+ (PVDD), both against GND.
- Trigger on CH2 rising, single shot, at 20 ms/div.
- Power-cycle the board from the charger. The firmware raises GPIO18
  once the contract is up. To trigger by hand, use `hifi amp off` and then
  `hifi amp on`. That only toggles SDZ, though; PVDD_EN follows the power
  state, so a cold start is the real test.

| Measure | Expected |
|---|---|
| Delay from GPIO18 high to PVDD starting to rise | ≈ 80–160 ms ([firmware.md](../firmware.md)): C19 4.7 µF charges toward Vth through R5 ∥ R6 = 68.75k, τ ≈ 0.32 s **(estimate)** |
| PVDD 10–90 % rise | ≈ 25–40 ms **(estimate)**: the Miller plateau, C2 100 nF through R15, about 55 µA. The known-unknowns power:9 sim gives about 28 ms. |
| Final gate voltage | VBUS − 6.25 V (Vsg = VBUS × 100k / 320k) |
| PVDD final | VBUS − < 0.1 V (AO4485 ≈ 15 mΩ) |
| Inrush | ≈ 0.4–0.5 A **(estimate)**, 0.75 mF at about 550 V/s. The charger must not drop out. |
| PVDD_SENSE | C18 ≈ 1.82 V; `power.pvdd_v` ≈ 20 |
| Turn-off (unplug) | slow, τ = R5 × C19 ≈ 0.47 s gate, then the R16 bleed. The firmware drops SDZ first (`hifi_bsp_power_out`). |

**On a fail:**
- **VBUS stays at 5 V:**
  - Check R1. 120k gives 20 V; a wrong value gives another voltage. 210k is
    the core board's 28 V.
  - Check that U1 CC1/CC2 (pins 7/6) go to J1 A5/B5.
  - Check that `pd_sink_i2c` passes.
  - Try another charger.
- **VBUS is 20 V but PG stays high:** U1 PG (pin 10) or R4. The README's
  rework for a missing PG is to short Q4's emitter pad to GND, which loses
  the hardware AND. Only do that if U1's PG is proven dead.
- **PG is low but PVDD stays at 0:**
  - GPIO18: scope R7's ESP32 side. It must be high after the contract.
  - Check Q4 (base about 0.7 V, collector pulled low).
  - Check R6 (220k to /PSW_PULL).
- **The charger drops out on enable:** inrush or cSnkBulk. Scope VBUS at
  the moment PVDD rises. Try another charger (9.4).
- **`pvdd_sensed` fails while C58+ reads right:** check R17/R18 and C18,
  and the ADC calibration (12 dB attenuation, 8-sample average).

## 5. Codec (ES8388, U7)

The board stays on the charger with the amp running and the speaker
terminals open.

1. `codec_i2c` passes (selftest).
2. **Supplies:** DVDD/PVDD at +3V3 and AVDD/HPVDD at +3V3A (C16): 3.3 V.
   VMID (C35) and VREF (C39) ≈ 1.65 V **(estimate**, mid-supply). If they
   read 0, the codec isn't powered up by firmware: check that `codec_i2c`
   passes and that a stream or tone has started.
3. **Clocks.** Play `hifi tone sine 1000 -40` and scope:
   - **MCLK, GPIO0:** 12.288 MHz at 48 kHz from the APLL
     ([digital:21](../known-unknowns.md)). Also run the open item in 9.6.
   - **BCLK, GPIO27:** 64 × fs = 3.072 MHz at 48 kHz.
   - **LRCK, GPIO25:** 48 kHz.
   - **DOUT, GPIO26:** data.
   - The firmware sets the I2S pins to GPIO_DRIVE_CAP_0. Look at BCLK's
     edges at the ES8388 end (open item 9.5).
4. **DAC output, no amp:** LOUT1/ROUT1 drive the amp inputs AMP_L_INP/INN
   through the coupling caps. At −20 dBFS the codec output is about
   0.1 × (3.3/3.3) ≈ 0.1 Vrms **(estimate**, full scale AVDD/3.3 Vrms per
   [audio:10](../known-unknowns.md)), less the OUT1 volume code. This is a
   presence check, not a level check.

**On a fail:**
- No MCLK: GPIO0 conflicts with Q3, or the APLL is off. See the
  `DAC_CONFIG` `mck=0`.
- No BCLK: the pin map is wrong in the image (DAC_CONFIG for Lite is
  `model=ES8388,bck=27,ws=25,do=26,di=35,sda=33,scl=32,i2c=16,mck=0`).

## 6. Amp (TPA3118D2, U8) into dummy loads at low level

Connect 4 Ω dummy loads across J4/J5 (L) and J6/J7 (R). **Don't connect a
ground clip to any of these.** Keep the inline meter on the charger.

### 6.1 Idle with the amp enabled and no signal

Run `hifi tone silence 30`, or leave it idle with SDZ high.

| Measure | Pad | Expected |
|---|---|---|
| GVDD | C50 | GVDD, about 5 V **(estimate**, not re-read from SLOS708G. Record it.) |
| GAIN | R50.1 | GVDD × 20k/120k ≈ 0.83 V at 5 V. 20k to GND with 100k to GVDD is 26 dB master ([audio:1](../known-unknowns.md)). |
| SDZ | C26.1 | ≈ 3.2 V. The worst case is 2.7 V, against a VIH of 2.0 V ([audio:3](../known-unknowns.md)). |
| FAULTZ | R53.2 | 3.3 V |
| Each output leg DC (meter against GND) | J4, J5, J6, J7 | PVDD/2 ≈ 10 V ±0.5 V **(estimate)** |
| Differential DC across each load (meter across J4–J5, J6–J7) | | < 50 mV **(estimate)** |
| Idle input power (inline meter) | | record it. The governor assumes `p_idle_w` 2.5 W. |
| `amp.faults`, `amp.latched` | state | 0, false |

Switching ripple on a leg (scope at 1 µs/div, leg to GND): about 400 kHz
(AM2..0 = 000, [audio:2](../known-unknowns.md)), small after the LC
filter.

### 6.2 Low-level tone

1. Run `hifi volume 30`, then `hifi tone sine 1000 -40 both 10`. Note that
   the tone plays at unity and the receiver volume does not apply to it
   (`hifi_main.c`).
2. Measure the AC volts across each load with the meter, true-RMS. The
   expected value is −40 dBFS × `vfs_peak_v` 17.98 V / √2 ≈ 0.127 Vrms
   **(estimate)**, from `hifi_gov_cfg_lite`:
   0.9 × 0.7079 × 19.95 × √2, the codec FS × OUT1 −3 dB × 26 dB gain.
   Record the measured value. It is the calibration point (9.7).
3. Repeat with `left` and `right`. Each load must sound only on its own
   channel. Expect AMP_L_* → amp RINP/RINN → J4/J5. **J4/J5 is left
   (U8's "R" channel). Check it is not swapped.**
4. Run `hifi tone sine 1000 -20 both 5`: about 1.27 Vrms per load
   **(estimate)**. The scope differential (two probes, Math A−B) shows a
   clean sine.
5. `hifi amp off`: SDZ goes low, the outputs stop, and the leg DC falls
   towards 0 V as the outputs go Hi-Z. `hifi amp on` brings it back. Note
   FAULTZ's level while SDZ is low (open item 9.8).
6. `hifi amp reset` clears latched faults.

**On a fail:**
- FAULTZ low at once: a short on an output (OCP 7.5 A,
  [audio:5](../known-unknowns.md)), a bridge at U8, a bad L1–L4, or a
  shorted C68–C71. Power off and check the output nets' resistance to GND
  and to PVDD.
- One leg not at PVDD/2: bootstrap C64–C67, or the output pin's solder.
- No output with SDZ high: GAIN voltage, MUTE (pin 12 on GND), the input
  caps, and the codec DAC routing (5.4).
- Wrong gain: R50/R51 values, GVDD.

## 7. Analog extras

### 7.1 Line in, both routes

Feed J2 (RCA) a 1 kHz sine at 0.5 Vrms.

- **Stream path / ADC:** `hifi input stream` with the line playing. Run
  `api capture "{\"source\": \"line\", \"frames\": 4096}"` while a tone
  plays (capture needs the output running). The line divider is 2.5
  (`LINE_DIVIDER`). Read `v_per_fs` and `stats`.
- **DJ direct (analog bypass):** `hifi input dj_direct`. The speakers play
  the line through the ES8388's bypass mixer.
  - Measured across the load: about 0.5 Vrms × the OUT1 volume code × 20
    (26 dB) **(estimate)**. Start low.
  - This route has no governor, so the source level sets the power.
  - Expected registers: ADCPOWER 0x38, DACCONTROL17/20 = 0x58,
    LOUT1/ROUT1 = the volume code. **No console op reads ES8388 registers;
    see the gaps.**
  - Lite answers `hifi input line` with 409.

### 7.2 Sub outputs (J3) and the power-fail mute

1. Run `hifi sub 0` and play `hifi tone sine 60 -20`. Measure SUB_L and
   SUB_R (J3) unloaded against GND. The cap is about 2 Vrms at full scale
   (README). The sub code caps at 24 (−9 dB), so a full-scale output
   is lower. Measure and record it.
2. `hifi sub -30`, then `hifi sub` with `{"on": false}`
   (`api sub "{\"on\": false}"`): the level drops, then the output stops.
3. **Power-fail mute:** play into J3, scope SUB_L and SUB_MUTE (Q12.3),
   and unplug the charger.
   - Expected: SUB_MUTE rises toward MUTE_RES (C90, about 4.5 V
     **(estimate)**) as +5V falls. MUTE_DET (R80/R81 ≈ 0.80 V at 5.18 V)
     drops below Q12's Vbe, and Q13/Q15 shunt the outputs before the codec
     collapses.
   - Look for a thump on the sub out of more than about 100 mV
     **(estimate)**.
   - While +5V is up, SUB_MUTE must sit at about 0.1 V. If it is high, the
     outputs are muted all the time: check Q12 and R80/R81.

### 7.3 Speaker taps (self-measurement)

R66/R67 and R68/R69 (100k/10k) feed SPK_L_P and SPK_R_P into LIN2/RIN2
(divider 16 with the ADC's 20k input, `TAP_DIVIDER`).

- Play `hifi tone sine 1000 -20`, then run
  `api capture "{\"source\": \"loopback\", \"frames\": 4096}"`.
- Each tap sees one leg: PVDD/2 DC (blocked) plus half the differential
  swing.
- Compare `rms_fs × v_per_fs` with the meter's leg-to-GND AC reading
  (step 6.2's load voltage ÷ 2 per leg).
- The ratio corrects `ADC_FS_PEAK_V` (1.27, a datasheet figure).
- Taps are on by default (`HIFI_HAS_TAPS` 1; NVS `hifi_taps`=0 turns them
  off).

## 8. Network, controls, LED

### 8.1 Wi-Fi

This step runs on the laptop console before step 4:
`api wifi "{\"ssid\": \"...\", \"password\": \"...\"}"`, or
`bench.py bringup --wifi`.
- `selftest` `network` passes.
- `python3 bench/bench.py find` lists the board on mDNS `_hifi._tcp`, with
  TXT `board=lite`.
- RSSI is in squeezelite's status page. Lite has no copper under the
  antenna ([digital:33](../known-unknowns.md)).

Lite has no Ethernet.

### 8.2 IR (U9 IRM-3638T on GPIO14)

- IR_VCC at C80: +3V3 less R65 100 Ω × about 1 mA ≈ 3.2 V **(estimate)**.
- Run `hifi ir learn volume_up`, then press a key on the NEC remote. A
  following `hifi state` shows the key in `ir.learned`. The learned key
  then changes the volume: `hifi state` shows the volume step.
- Scope GPIO14 while pressing a key. The output idles high and drops to
  bursts of NEC frames.

### 8.3 Buttons (GPIO13, GPIO36, GPIO19; 10k pull-ups R60–R62; press to GND)

| Button | Net | Short press | Long press |
|---|---|---|---|
| SW on GPIO13 | volume up | `hifi state` volume +step | next |
| SW on GPIO36 | volume down | volume −step | previous |
| SW on GPIO19 | toggle | play/pause | power |

- Meter on each GPIO pad: 3.3 V when released, 0 V when pressed.
- With nothing streaming, check the volume step with `hifi state`. No
  event is reported (see the gaps).
- GPIO36 is input-only, so it has no internal pull-up. R61 alone holds it,
  so check it reads 3.3 V when released.

### 8.4 LED D2 (red, GPIO22 through R64 1k, active high)

The LED should light per squeezelite's LED config (`22=green:1`; it is
named "green", but the part is red). About (3.3 − 1.9)/1k ≈ 1.4 mA
**(estimate)**, so it is dim but visible. No op drives the LED (see the
gaps).

## 9. Open items to measure at bring-up

These come from the README's open items and known-unknowns. Each needs a
number written back to the README or known-unknowns.

### 9.1 U8 temperature at 1/8 power: the thermal waiver

Sources: README "Bring-up must measure it"; known-unknowns thermal:8, 9,
12.
- **Setup:** a thermocouple on top of U8, the board in the box if
  possible (the 22 K air rise is unverified, thermal:12), 4 Ω on both
  channels, a 20 V charger.
- **Run:**
  `python3 bench/bench.py soak --ohm 4 --temp-cmd '...'`, or
  `--temp-manual` (bench.md stage 4; 600 s by default).
- **Load:** 1/8 of the measured 1 % power, or the predicted 43 W → about
  5.4 W/ch. Chip loss is about 1.75 W **(estimate**, thermal:9).
- **Pass:** case under 100 °C for the whole soak, with FAULTZ high.
- **Above 100 °C:** put a heatsink on the package top, or lower the
  governor's `thermal_out_w` (18 W, a compile-time constant in
  `hifi_gov.c`).

### 9.2 Power sweep

Run `python3 bench/bench.py calibrate --vrms V`, then `sweep --loads 4,6,8
--governor both` (bench.md stage 3).
- **Expect:** about 43 W/ch into 4 Ω at clipping. The verdict passes at
  ≥ 85 % of the prediction with the governor off.
- **Record:** PVDD under load (`power.pvdd_v`) and the charger's behaviour
  with the governor on: `governor.in_w` against `budget_w` (0.9 ×
  contract W).

### 9.3 CH224A CFG2/CFG3 idle level (README open item 4; digital:37)

- **When:** at 20 V with the ESP32 running.
- **Measure:** U1.2 and U1.3 against GND (the I2C SDA/SCL side of R2/R3).
- **Pass:** below 3.6 V.
- **Above 3.6 V:** depopulate R2/R3. The I2C readout of U1 is lost, but
  VBUS sense still works.
- **Also record** the levels with the ESP32 held in reset (EN low),
  because the internal pull-up rail is unknown.

### 9.4 cSnkBulk and charger compatibility (README open item 6; power:27)

- **Background:** VBUS carries about 15 µF of ceramic, above USB-C's 10 µF
  attach limit.
- **Run:** step 4 with 2–3 chargers from different brands (`bench.py
  bringup --chargers 65w,100w,...`).
- **Pass:** each attaches, reaches 20 V, and holds through the PVDD enable.
- **Record:** the brand, any dropout, and VBUS on the scope at attach.

### 9.5 BCLK edge and crosstalk (lite-preorder.md)

- **Why:** the firmware uses GPIO_DRIVE_CAP_0 on BCLK/LRCK/DOUT.
- **Measure:** scope BCLK at U7 with the ground spring. Record the rise
  time, overshoot and ringing.
- **Listen:** for BCLK coupling into the line input or the sub outs.
  Play `hifi tone silence`, then look at the sub outs and the capture noise
  floor.
- **On edge errors (glitches, codec loss of lock):** raise the drive cap
  in `hifi_board_lite.h` and re-check.

### 9.6 MCLK on GPIO0 with the auto-program circuit

- **Measure:** MCLK at U7's MCLK pin while playing.
- **Pass:**
  - It is a clean 12.288 MHz, not loaded down by Q3's collector.
  - The board still boots normally from a cold power-up on the charger
    (GPIO0 must not be read low at reset).
  - Auto-program still works afterwards (step 3.2).

### 9.7 Calibration constants

Feed the step 6.2 and 7.3 measurements into these:

- **`vfs_peak_v`:** expected 17.98 V. Use the measured load Vrms at
  −20 dBFS × 10 × √2.
- **`ADC_FS_PEAK_V`:** expected 1.27. Use the tap ratio from 7.3.
- **`efficiency`:** expected 0.88. Use the inline meter's input power
  against the load power at several levels.
- **`p_idle_w`:** expected 2.5 W. Use the idle input power from 6.1.
- **`bench.py calibrate`:** stores `volts_per_fs` for the bench only.

The firmware's own constants are compile-time (see the gaps).

### 9.8 Other checks

- **FAULTZ while SDZ is low** (6.2 step 5): record whether FAULTZ stays
  high in shutdown. The firmware's `amp_no_fault` reads it 200 ms after
  enable.
- **Absolute polarity at J4** (audio:4, unverified):
  - Play a positive-going step or the start of a low-frequency tone, and
    check that J4 (red) goes positive with respect to J5.
  - Alternatively, use the 7.3 capture with a DC-coupled view on the
    scope.
  - Record the result. If it is inverted, swap the documentation or the
    firmware's sign.
- **The IRM-3638T supply range** (digital:24, unverified): it runs at
  about 3.2 V (8.2). Check that it decodes at 3 m.
- **J1's 5 A rating** (power:25, unverified): during the sweep at 100 W,
  measure J1's temperature with the thermocouple. Under 30 K rise
  **(estimate**, my threshold).
- **Mechanical** (README open items 2 and 3): check the depth of RCA J2/J3
  against a real XDJK-0271-1650. Check the banana collar against the
  10.2 mm rear-wall hole, and test 15 mm and 19 mm plugs in J4–J7.

## 10. Firmware coverage gaps

These steps need firmware, console or test-API support that does not
exist. The list was sent to the firmware owner (af884, thread
hifi-receiver).

1. **Calibration constants are compile-time.**
   - Affects `vfs_peak_v`, `efficiency`, `p_idle_w` and `thermal_out_w` in
     `hifi_gov_cfg_lite`, and `ADC_FS_PEAK_V`.
   - There is no NVS key or API to set them, so 9.7 needs a rebuild per
     board batch.
2. **No raw I2C register read.**
   - This blocks reading the CH224A 0x09/0x50 (step 4) and the ES8388
     DJ-direct registers (7.1).
   - It is unverified whether squeezelite-esp32's `i2cget`/`i2cdump`
     console tools are in the image.
3. **No button or IR event in `state` or the log.** Steps 8.2 and 8.3 can
   only be seen through volume changes. A `last_input` field in `state`,
   with the source, action and time, would make them scriptable.
4. **No LED test op** (8.4).
5. **No raw GPIO readout** for the button pins, SDZ, FAULTZ or PG (an
   `io` object in `state`). FAULTZ and PG are reported, but not the
   buttons or SDZ.
6. **No input current on Lite** (`vbus_i_a` is null). Efficiency and idle
   power need the inline meter. That is a hardware limit, not a firmware
   one: listed so nobody expects the bench to compute it.

## 11. Doc inconsistencies found while writing this

These were reported to af884 and not fixed here:
- **bench.md stage 1:** it says Lite's PVDD_EN "isn't ANDed with the
  CH224Q's power-good". The board ANDs it in hardware through Q4 (emitter
  on /PD_PG), and the part is the CH224A.
- **README "How to test":**
  - It says PVDD "rises in ~6 ms" with "inrush ~1.6 A". The current R6
    220k / C19 4.7 µF soft start gives about 28–36 ms and about 0.5 A
    **(estimate)**.
  - The open item "USB ESD: none" is stale: D7/D8 PESDNC2FD5VB sit on
    USB_DP/DM.
- **known-unknowns thermal:20:** still says R6 47k and VGS −13.6 V. The
  board has R6 220k and VGS ≈ −6.25 V at 20 V.
- **known-unknowns power:9:** cites C19 2.2 µF. The board has 4.7 µF.
- **firmware.md:** "rev A has no taps (409)". The taps are fitted and
  `HIFI_HAS_TAPS` defaults to 1.
- **hifi_board_lite.h comment:** it says "SDZ through 100k". R54 is 10k.
- **test-api.md:** names the console chip as CH340C. Lite has a CH340B (U6).
- **LED:** configured as `green`, but D2 is red (cosmetic).
