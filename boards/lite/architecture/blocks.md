# lite-main: blocks

One 2-layer board. Every choice below follows `docs/product.md` (Lite column) unless it
says why it differs.

| Block | Parts | What it does |
|---|---|---|
| B1 USB-C + PD | J1 USB4105-GF-A-120, U1 CH224Q, D1 SMBJ24A | 48 V/5 A receptacle; CH224Q asks for 20 V (Rset 120k on CFG1, datasheet table 5-1); TVS on VBUS. CC1/CC2 to the CH224Q. CH224Q I2C (0x22) on the codec's bus so firmware reads the contract. |
| B2 PVDD switch | Q1 AO4485 (P-MOSFET 40 V) | Connects VBUS to PVDD only once CH224Q's PG goes low (contract good), with a gate RC soft-start, so the 440 uF of amp bulk never loads VBUS during PD negotiation and a 5 V-only port never powers the amp. |
| B3 5 V buck | U2 TPS54302 | VBUS (5-21 V) -> 5 V, 3 A part, about 0.6 A used. TI design-example values (10 uH, 2 x 22 uF, 100k/13.3k, 75 pF). |
| B4 3.3 V buck | U3 SY8089A1AAC | 5 V -> +3V3 digital (ESP32, CH340C, codec DVDD/PVDD). |
| B5 audio LDO | U4 LP5907-3.3 | 5 V -> +3V3A, codec AVDD and HPVDD only. |
| B6 MCU | U5 ESP32-WROVER-E-N8R8 | Wi-Fi + BT Classic + 8 MB PSRAM; antenna at the board's top edge with a keep-out. |
| B7 USB-UART | U6 CH340C, Q2/Q3 MMBT3904 | Console + flashing on the USB-C D+/D-; the usual DTR/RTS auto-program pair on EN and GPIO0. |
| B8 codec | U7 ES8388 | I2S slave, MCLK from GPIO0; LOUT1/ROUT1 to the amp, LOUT2/ROUT2 to the sub RCAs, LIN1/RIN1 from the line-in RCA (ADC + analog bypass). |
| B9 amplifier | U8 TPA3118D2, L1-L4 10 uH, 1 uF filter caps | BTL stereo, 26 dB gain (20k/100k on GAIN/SLV), BD mode, no power limit, 400 kHz. Single-ended drive from the codec (INN AC-grounded). |
| B10 I/O | J2 dual RCA line in, J3 dual RCA sub out, J4-J7 4 mm banana | Rear edge. |
| B11 UI | SW1-SW3, U9 IRM-H638T, D2 status LED | Front edge. |

## ESP32 pin map

The I2S/I2C/MCLK pins are the ESP32-A1S AudioKit's (ES8388), so squeezelite-esp32's
existing board profile applies.

| Signal | GPIO | Notes |
|---|---|---|
| I2S MCLK | 0 | strapping pin; codec MCLK input is high-Z, auto-program pulls it low |
| I2S BCLK | 27 | |
| I2S LRCK | 25 | |
| I2S DOUT (to codec DSDIN) | 26 | |
| I2S DIN (from codec ASDOUT) | 35 | input-only pin |
| I2C SCL | 32 | 2.2k pull-up; codec 0x10, CH224Q 0x22 |
| I2C SDA | 33 | 2.2k pull-up |
| Amp SDZ | 21 | 100k pull-down: amp off until firmware enables it |
| Amp FAULTZ | 23 | open drain, 10k pull-up |
| PD PG | 4 | CH224Q PG (open drain, 10k pull-up), low = contract good |
| VBUS sense | 34 | 100k/10k divider, ADC1 |
| IR in | 39 | input-only, 10k pull-up |
| Button vol- | 36 | input-only, 10k pull-up |
| Button vol+ | 13 | 10k pull-up |
| Button play/source | 19 | 10k pull-up |
| Status LED | 22 | 1k to the LED |
| UART0 TX/RX | 1/3 | CH340C |
| Not used | 2, 5, 12, 14, 15, 18 | 12 left floating: its internal pull-down keeps the flash at 3.3 V |

## Power tree

USB-C VBUS (5 V at attach, 20 V after the PD contract)
- -> TVS SMBJ24A
- -> Q1 AO4485 (on when PG low) -> PVDD -> TPA3118D2 (PVCC, AVCC), 2 x 220 uF + per-side 1 uF/100 nF/1 nF
- -> U2 TPS54302 -> +5V
  - -> U3 SY8089 -> +3V3 (ESP32 0.5 A peak, CH340C, ES8388 DVDD/PVDD, IR, LED, pull-ups)
  - -> U4 LP5907 -> +3V3A (ES8388 AVDD, HPVDD)

## Ground

One GND net and one bottom-layer GND pour. The amp's power-ground and the codec's
analog ground are kept apart by placement (amp on the right, codec/RCA on the left),
not by a split.
