# Review round 2 (review-3): digital, ESP32 and firmware interface

Domain DIGITAL. Boards: lite-main, core (Plus/Ultra), ultra-front, as on main at
ac484b1. Read-only on `boards/` and `firmware/`. hwde gates were run on a copy
in /tmp, not in place.

**Counts:** 0 blocker, 3 major (1 repeat, 2 new), 6 minor (1 repeat, 5 new), 4 notes.
Lite has no blocker or major in this domain. All three majors are on core: one
hardware with a firmware part, one fixable by either, one firmware only.

## Findings

### R3-DIGITAL-1 (major, core, repeat of DIGITAL-1): GPIO0 can latch the 50 MHz clock on a brownout or RTC-watchdog reset

The round-1 fix was a TCA9539 with RESET on ESP_EN and a 2.2k pull-down on Y2 OE.
It covers power-on and every EN reset. It does not cover a reset the ESP32 makes
inside itself:

- ESP32 datasheet v5.3 §3: "At system reset, the latches sample the bit values of
  their respective strapping pins". §4 (watchdogs): the RWDT does a system reset,
  and it runs automatically during flash boot.
- The firmware builds on IDF v4.4.8 with `CONFIG_ESP32_REV_MIN=1`, so
  `SOC_CAPS_ECO_VER` is 100 or more and `SOC_BROWNOUT_RESET_SUPPORTED` is 1
  (soc_caps.h line 105). `esp_brownout_init` then sets
  `reset_enabled = BROWNOUT_RESET_EN = true` (brownout.c v4.4.8). A brownout at
  level 0 (about 2.43 V on +3V3) is therefore a hardware system reset, and the
  straps are sampled again.
- EN does not move in either case. U17 (TCA9539) keeps P05 = PHY_RST high: its
  VPORF is 0.75-1.0 V (SCPS202C, Table 8), and its RESET input falls together
  with VCC. So Y2 (YSO110TR: enabled when OE is at least 70 % VDD) keeps driving
  50 MHz through R60 into GPIO0. GPIO2 (I2S_BCK) is low at reset. A sample of
  GPIO0 = 0 puts the chip in UART download mode, and it waits there until the
  power is cycled. A box that was plugged in and left alone goes silent.
- Firmware duty C1 ("write P05 low before a software restart") is not
  implemented: there is no shutdown handler in `firmware/components`. C1 also
  can't help a brownout, because no code runs before that reset.
- Realistic triggers are a USB-C cable or charger glitch that pulls +3V3 between
  about 1.0 V and 2.43 V for a moment, and a bootloader hang after a software
  reset (RWDT after 9 s, `CONFIG_BOOTLOADER_WDT_TIME_MS=9000`).

Test: `sims/digital/strap_check.py` fails on core GPIO0 in the `sysrst` event and
passes POR and EN on both boards.

Field reference: WT32-ETH01 boards, which use GPIO0 as the RMII clock input,
have the same reported "boots into download mode after a reset" behaviour.
Olimex ESP32-POE avoids it by using GPIO17 as clock out. We can't do that,
because GPIO16/17 carry the WROVER's PSRAM.

**Fix (hardware, cheap):** put a voltage supervisor on ESP_EN: TI **TLV803SDBZR**
(LCSC **C132016**, SOT-23, open drain, 2.93 V, 200 ms). Wire VDD to +3V3, RESET#
to ESP_EN, GND to GND, next to R30/C32. Keep R30/C32 and Q4. The supervisor is
open drain, so the auto-program transistor can still pull EN. Its trip point,
2.86-3.00 V, sits about 0.4 V above the ESP32's level-0 brownout. Any dip
deep enough to brown the ESP32 out pulls EN low first. That resets the
TCA9539, and R45 turns Y2 off before the strap is sampled again. This is the
supervisor that Espressif's Hardware Design Guidelines recommend when the
supply can be unstable.

**Fix (firmware, for the RWDT case):** implement C1. Register an
`esp_register_shutdown_handler` that writes P05 low and waits 1 ms. In
`hifi_bsp_init`, release P05 only after the app is running (it already does
this).

Margin today: none on a system reset (the strap reads a clock). After the fix:
0.38 V between supervisor trip and brownout trip.

### R3-DIGITAL-2 (major, core, new): squeezelite asks the LAN8720A at SMI address 1; the board strapped it to 0, so Ethernet never starts

The LAN8720A's RXER/PHYAD0 (U9 pin 10) is unconnected. Its internal pull-down
(DS00002165C pin table, "PHYAD0 VIS (PD)"; §3.7.1 "default = 0b") gives
address 0. squeezelite-esp32 at the pinned commit ee1720c,
`components/wifi-manager/network_driver_LAN8720.c`, hard-codes
`phy_config.phy_addr = 1;`. IDF v4.4.8 `lan87xx_init` detects the address only
when it is `ESP_ETH_PHY_ADDR_AUTO`. Otherwise it reads the ID at address 1,
gets 0xFFFF (MDIO pulled up by R62), and fails with "wrong chip ID". Our
`ETH_CONFIG` (`model=LAN8720,mdc=23,mdio=18,rst=-1`) has no field that changes
the address. Plus and Ultra would be Wi-Fi only.

WT32-ETH01, which squeezelite's default targets, straps PHYAD0 high (address 1).
Olimex ESP32-POE uses address 0.

Test: `sims/digital/fw_pinmap.py` fails on "LAN8720A PHYAD0 strap -> address 0;
squeezelite queries address 1".

**Fix:** add **R68 10k (C25744, 0402) from U9 pin 10 (RXER/PHYAD0) to +3V3**
(VDDIO, per DS00002165C Note 3-3). That gives address 1 and matches upstream with
no firmware change. RXER is an output after reset, and 10k on it is harmless.
Also patch `network_driver_LAN8720.c` to `phy_addr = -1` (auto), so either strap
works. With the patch alone, the hardware can stay as it is.

### R3-DIGITAL-3 (major, ultra-front + core firmware, new): nothing in the firmware drives Ultra's front board

- `firmware/board/ultra/sdkconfig.defaults` has `CONFIG_DISPLAY_CONFIG=""` and
  `CONFIG_SPI_CONFIG=""`. The patch adds no display default, so the LCD on
  GPIO32/33/15 is never initialised.
- Nothing reads the front TCA9535 at 0x27. `EXP_ADDRS` scans only 0x20 and
  0x74-0x77, and `controls()` sets `.buttons = {-1,-1,-1,-1}` for Ultra. So
  ENC_SW and BTN1-3 on the front are dead, and the RGB LED and LCD_RST (P10-P13)
  are never driven.
- The TCA9535's INT is wired-OR onto EXP_INT/GPIO39. It never gets cleared,
  because nothing reads 0x27. ENC_A/ENC_B also go to its P00/P01, so the first
  turn of the knob latches GPIO39 low for good. After that, core's NEGEDGE
  interrupt never fires again and the knob falls back to the 10 ms poll. At 4
  edges per detent the poll decodes at most 25 detents/s, about 1.25 rev/s on
  an EC11, so a fast spin skips or reverses.

The hardware is sound. ST7789-class 4-wire SPI with CS held low (R57) works in
SPI mode 3. LCD_RST is held high by R8 10k.

**Fix (firmware):**
1. Set `DISPLAY_CONFIG` and `SPI_CONFIG` for Ultra: SPI host, `clk=32,mosi=33,dc=15,cs=-1`,
   the panel's driver, width and height, `mode=3`, `back=-1` (backlight is on the
   expander, P06).
2. Probe 0x27, set its outputs (P10 LCD_RST, P11-P13 LED_R/G/B), and read its
   inputs in `controls()` so the INT clears. Map ENC_SW and BTN1-3 to actions.
3. Pulse LCD_RST low for 10 ms before the panel init.

### R3-DIGITAL-4 (minor, core firmware, new): speaker-tap select P17 is never driven, and the header says P17 is LCD_CS

`hifi_board_core.h`: "P17 is LCD_CS on Ultra's FFC, pulled up; the firmware leaves
it an input". The board has P17 = TAP_SEL_IO (R133 to Q7). LCD_CS is R57 to GND,
not on the expander (since round-1 DIGITAL-16). `EXP_CFG1 = 0xFF` keeps P17 an
input, and R134 100k holds it off, so the ADC always hears the RCA input. Firmware
duty C6 (speaker taps) is not implemented. `HIFI_HAS_CAPTURE` is 0 on core.

Test: `fw_pinmap.py`, "expander bit 15 is net TAP_SEL_IO".

**Fix:** add `#define EXP_TAP_SEL 15`, set `EXP_CFG1` to 0x7F, and drive it in the
capture path. Fix the comment.

### R3-DIGITAL-5 (minor, lite-main firmware, new): firmware constants lag the board

- `LINE_DIVIDER 2.0f`; the board's R30/R31 are 15k/10k, a ratio of 2.50 (round-1 AUDIO-7).
- `HIFI_HAS_TAPS 0` ("rev A has no taps"); the board has R66-R69/C81-C84 into LIN2/RIN2.
- The `PIN_AMP_SDZ` comment says "through 100k"; it is now 10k (R54).

Captures would read 1.9 dB low on line-in, and the loopback self-test would be
off. Test: `fw_pinmap.py`. **Fix:** set `LINE_DIVIDER 2.5f` and `HIFI_HAS_TAPS 1`,
and fix the comment.

### R3-DIGITAL-6 (minor, lite-main, new): no ESD protection on USB D+/D-

The CH340B UD+/UD- (U6 pins 5/6) go straight to J1 A6/B6/A7/B7. Core has a
USBLC6-2SC6 (U2) on the same lines. The CH340 datasheet (V3D) gives no ESD
rating. The person plugs a laptop in for setup and recovery, and the port is a
touchable USB-C receptacle. A dead CH340 removes the USB recovery path; Wi-Fi
OTA still works.

**Fix:** add a USBLC6-2SC6 (C2687116) as on core: pins 1/6 on USB_DP, 3/4 on
USB_DM, 2 on GND, 5 on +5V, placed at J1. Contingency if it won't fit: two 0402
ESD diodes (e.g. ESD5Z3.3 class) to GND.

### R3-DIGITAL-7 (note, both, repeat of DIGITAL-13): CH340B RST# (pin 7) unconnected on Lite U6 and core U8 is OK (the lead's question)

WCH CH340 datasheet V3D pin table: "RST#, Input, CH340B: External reset input,
active low, built-in pull-up resistor". §6.3 at 3.3 V: IUP 3/70/200 µA
(min/typ/max), so the pull-up is about 16k-1.1 MΩ. VIL max is 0.8 V. WCH's own
application circuit leaves it open, and the chip also has its own power-on reset
(VR 2.4-2.8 V). The pin has no trace on either board, so nothing couples into
it. Worst case, a reset would only drop the CH340's USB session, never the
ESP32's. Leave it open. Optional: a 100 nF pad from pin 7 to GND, DNP.

### R3-DIGITAL-8 (minor, core, new): the CH340B V3 pin (U8 pin 4) has no decoupling cap of its own

WCH V3D §7: "C8 of the V3 pin 0.1 µF ... C9 0.1 µF ... as close as possible to the
connected pins". Lite has C23 on pin 16 and C24 on pin 4. Core has only C33, on
pin 16, which is on the other side of the SOP-16. **Fix:** add 100 nF (C1525,
0402) at U8 pin 4 to GND.

### R3-DIGITAL-9 (minor, both, repeat of DIGITAL-18): side clearance beside the antenna

hwde `check_antenna` (run on a copy) finds no copper under the antenna on either
board, so DIGITAL-19's keep-out holds. Copper starts 2.25 mm beside the antenna
on Lite and 0.85 mm on core, against Espressif HDG §1.4.8's 15 mm. Antenna end:
Lite 0.64 mm inside the edge, core 0.0 mm. The round-1 disposition (RSSI check
at bring-up; WROVER-IE contingency) still applies. Note that core's Ethernet
fallback depends on R3-DIGITAL-2 being fixed.

### R3-DIGITAL-10 (minor, ultra-front, new): the front TCA9535 has no reset, and its INT shares EXP_INT

Its outputs (LCD_RST, LEDs) survive an ESP32 reset, and its INT can hold GPIO39
low (see R3-DIGITAL-3). This is harmless once firmware re-initialises 0x27 at
boot and reads it in the control loop. Optional hardware: drop the front's own
ENC_A/ENC_B to U1 P00/P01 (core reads them already), so knob turns don't raise
the front INT.

### R3-DIGITAL-11 (note, core): LAN8720A nRST low level through R67

With PHY_RST at 0 V, nRST sits at 50 µA (typ PU) x 10k = 0.50 V, or 0.61 V
through 2.2k + 10k when the expander pin is an input. VIS VILT at 3.3 V is
1.41 V typ (DS00002165C Table 5-5), a margin of 0.8 V at typical pull-up
current. nRST rises at 0.69 x 10 ms = 6.9 ms after OE. Y2 starts in at most 3 ms
(YSO110TR t_str). tpurstd (25 ms) is met because P05 is released only after the
app boots. Passes.

### R3-DIGITAL-12 (note, lite-main): the CH224A's I2C VIH table maximum is 3.3 V; the pull-ups go to +3V3 = 3.33 V

The CH224 datasheet V2.0 lists VIHI2C 1.5-3.3 V, but the same datasheet (§5.2.1)
says CFG2/CFG3 "support 3.3V or 5V level input". That 30 mV is within the
part's stated tolerance. Keep the 0R links fitted (R2/R3).

### R3-DIGITAL-13 (note, core): RJ45 has magnetics but no TVS

The HR911105A gives 1500 Vrms isolation. WT32-ETH01, Olimex ESP32-POE and
LILYGO T-ETH add none either. Indoor patch cable to a home router: acceptable.

## Blocks passed, with the reference and the margin

| Block | Reference | Ours | Margin / verdict |
|---|---|---|---|
| EN RC (both) | Espressif HDG: R 10k, C 1 µF on CHIP_PU; datasheet v5.3 Table 3-2 tH 1 ms | R20/R30 10k, C22/C32 1 µF, tau 10 ms | EN crosses 0.75 VDD at 13.9 ms, after +3V3 settles (SY8089 SS about 1 ms). Pass |
| Auto-program (both) | ESP32-DevKitC V4: two NPNs, base 10k, DTR->EN-NPN base, RTS->IO0-NPN base, emitters crossed | Lite Q2/Q3 MMBT3904 + R24/R25 10k; core Q4/Q5 S8050 + R32/R33 10k, same crossing | Same topology. Pass |
| ESP32 decoupling | HDG: 10 µF + 0.1 µF at 3V3 | Lite C20 10 µF + C21 100 nF; core C30 22 µF + C31 100 nF | Pass |
| Strapping pins | Datasheet v5.3 Table 3-1 | see strap_check.py | Lite: all pass in POR, EN and sysrst. Core: pass except GPIO0 sysrst (R3-DIGITAL-1). GPIO12: Lite unconnected, core PCM5102A DIN (input), so 0 = 3.3 V VDD_SDIO |
| GPIO6-11 / 16-17 | WROVER-E: flash/PSRAM | unconnected on both | Pass |
| Input-only GPIO34-39 | Datasheet | Lite 34/39 ADC, 35 I2S in, 36 button. Core 34 ADC, 35 IR, 36 I2S in, 39 INT | No outputs. Pass |
| ADC2/Wi-Fi | Datasheet | ADC on GPIO34/39 (ADC1) only | Pass |
| GPIO36/39 errata (3.11) | ESP32 errata | Lite BTN_VOLDN debounced 40 ms (squeezelite buttons). Core EXP_INT is polled every 10 ms anyway | A spurious 80 ns pulse produces no event. Pass |
| MCLK on GPIO0 (Lite) | ESP32-LyraT: ES8388 MCLK on GPIO0 with the auto-program NPN on the same net | Same, R21 10k pull-up | Pass. ES8388 CE (pin 26) on GND = 0x10 (ES8388 DS rev 5.0 pin table) |
| I2C bus | NXP UM10204 fast mode: tr <= 300 ns, 3 mA | 2.2k pull-ups; see i2c_bus.py | Lite 27 pF, 50 ns. Plus 53 pF, 99 ns. Ultra (with an estimated 150 mm FFC) 78 pF, 146 ns: 2x margin. 1.32 mA. Addresses unique: Lite 0x10/0x22; core 0x10/0x22/0x27/0x48/0x74 |
| TCA9539 | SCPS202C | A1=A0=GND -> 0x74; RESET = ESP_EN; P0 outputs pulled down (R40-R45); INT 10k | Matches the firmware map (fw_pinmap.py), except P17 (R3-DIGITAL-4) |
| Y2 OE | YSO110TR: disable <= 30 % VDD, tri-state; start-up 3 ms | R45 2.2k to GND, TCA9539 hi-Z at reset | 0 V vs 0.99 V. Pass for POR/EN |
| LAN8720A straps and passives | DS00002165C | MODE[2:0] PU = 111 (RXD0/1, CRS_DV are ESP32 inputs at reset); nINTSEL PU = REF_CLK in; REGOFF PD = regulator on; RBIAS 12k (spec 12.1k, -0.8 %); 49.9R to +3V3E; MDIO 1.5k; VDDCR 1 µF + 100 nF (spec 1 µF + 470 pF); CLKIN on GPIO0 with `CONFIG_ETH_RMII_CLK_IN_GPIO=0` | Pass, except PHYAD (R3-DIGITAL-2) |
| IR receivers | Everlight IRM: RC supply filter, pull-up | 100R/4.7 µF (fc 339 Hz), 10k pull-up. Ultra has 10k at both ends of the FFC (5k) | Pass |
| Encoder | EC11 + RC debounce | 10k/10 nF (Ultra: 5k/20 nF, tau 100 µs) | Pass. Firmware decode limit, see R3-DIGITAL-3 |
| Firmware pin maps | netlist | fw_pinmap.py | Lite: every GPIO matches; 2 constant mismatches (R3-DIGITAL-5). Core: every GPIO and expander bit 0-14 match; P17 and PHY address fail |
| USB recovery over laptop USB alone | docs/remote.md, docs/bench.md | CH340B plus auto-program on both. 5 V from a laptop runs everything except PVDD (CH224 PG high, PVDD off). An EN pulse resets core's expander, so Y2 is off during flashing | Works. A recovery app sits in the factory partition. Risk: R3-DIGITAL-1 can leave core in download mode until it is power-cycled, but USB flashing still recovers it |

## Field issues searched

| Issue | Our board |
|---|---|
| ESP32 Ethernet boards with RMII clock in on GPIO0 that boot into download mode (WT32-ETH01; Espressif's RMII clock notes) | core: **has it** on brownout/RWDT system resets (R3-DIGITAL-1). Covered at POR and EN |
| squeezelite-esp32 LAN8720 driver: phy_addr hard-coded to 1 | core: **has it**, strapped to 0 (R3-DIGITAL-2) |
| Brownout resets on Wi-Fi TX (weak 3V3, small bulk) | Lite and core: immune. 2 A SY8089 fed from a 5 V buck; 22 µF + 10 µF + 10/22 µF at the module |
| ESP32 won't boot on some chargers (slow ramp, EN too early) | immune: 10 ms EN RC, and +3V3 comes from bucks with soft start. The PD 5 V -> 20/28 V step never lowers +5V |
| GPIO12 high at boot sets 1.8 V flash, so the WROVER-E won't boot | immune (strap_check.py) |
| GPIO36/39 80 ns glitch when the ADC powers up (errata 3.11) | immune (debounce or polling) |
| ADC2 unusable with Wi-Fi | immune (ADC1 only) |
| LAN8720 fails to start (clock not running at nRST, straps loaded wrong by MCU pins) | immune: nRST about 7 ms after OE, oscillator 3 ms max; ESP32 RMII pins are inputs at strap time |
| ESP32 MCLK/I2S clock on GPIO0 jitter | Lite: same as LyraT (APLL to CLK_OUT1). Core: no MCLK from the ESP32 (Y1 49.152 MHz for the Sabre; PCM5102A and ES7243E clock from BCK) |
| USB-C ESD | Lite: **has it** (R3-DIGITAL-6). Core: USBLC6 fitted |
| RJ45 ESD | core: magnetics only, like the reference designs (note) |

## Round-1 fixes checked

| Id | Holds | How |
|---|---|---|
| DIGITAL-1 | partly | POR and EN resets: yes (TCA9539 RESET = ESP_EN, R45 2.2k, YSO110TR tri-state). Internal system reset: no, and duty C1 is not in firmware (R3-DIGITAL-1) |
| DIGITAL-2 | yes | TCA9539 has no pull-ups; R40-R45 hold P00-P05 at 0 V at power-on |
| DIGITAL-3 | yes | board_core.c sub_start: I2S1 TX slave on GPIO2/4, data on GPIO12 |
| DIGITAL-5 | yes | R67 10k / C47 1 µF |
| DIGITAL-6 | yes | AUDIO_CONTROLS gpio36 debounce 40 ms |
| DIGITAL-13 | yes | CH340B RST# internal pull-up (WCH V3D) |
| DIGITAL-14 | yes | R54 10k (root.py, netlist) |
| DIGITAL-16 | yes in hardware | R57 10k to GND; the firmware comment is stale (R3-DIGITAL-4) |
| DIGITAL-19 | yes | check_antenna: no copper under core's antenna |
| DIGITAL-4 | yes | Y1 49.152 MHz |

## Tests built (docs/review-3/sims/digital/)

- `netlib.py`: KiCad .net reader used by the scripts below.
- `strap_check.py`: ESP32 strap levels at POR, at an EN reset, and at an internal system reset, from the netlists. **Fails** on core GPIO0 sysrst (R3-DIGITAL-1).
- `fw_pinmap.py`: firmware headers and Kconfig defaults against both netlists (GPIO nets, directions, input-only, PSRAM pins, ADC1, expander bits, ES8388 address strap, divider constants, LAN8720A PHY address vs squeezelite's). **Fails** with 4 mismatches (R3-DIGITAL-2/4/5).
- `i2c_bus.py`: routed I2C length (measured), bus capacitance, rise time, sink current, address uniqueness and their straps. **Passes**.

Run: `python3 -I docs/review-3/sims/digital/<script>.py` from the repo root. Each exits 1 on a failure.
