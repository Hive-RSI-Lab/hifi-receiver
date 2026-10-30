# What the firmware must do for the boards to be safe and work

The first-spin review (task 261029-020622) found these duties that the hardware
leaves to the firmware. Each one names the finding it comes from
(`docs/review-2/<domain>.md`). Without them a board can shut down, distort
or pop; none of them is optional.

## Lite (lite-main)

| # | Duty | Why (finding) |
|---|---|---|
| L1 | **Output governor.** Keep each channel's 30 s RMS output at or below 13 W into 4 ohm (20 W into 8 ohm): about -4.6 dB under the 1 % clip level at 4 ohm. Short peaks pass. | The TPA3118 reaches its latched 150 C shutdown in continuous full power in the box (THERMAL-3). At 1/8 power it runs at 94-106 C. |
| L2 | **Fault recovery.** FAULTZ (GPIO23) low: mute, SDZ (GPIO21) low for at least 100 ms, then retry; at most 3 retries in 10 min, then stay muted and report. | The over-temperature and short-circuit faults latch (THERMAL-3). |
| L3 | **Volume caps.** Codec LOUT1/ROUT1 volume at most -4.5 dB; the analog bypass (LI2LOVOL) at most -3 dB. | At 0 dB the ES8388 overdrives the 26 dB amp by 4.1 dB at 20 V (AUDIO-6). |
| L4 | **Power order.** PVDD_EN (GPIO18) only with a PD contract of 9 V or more (CH224A at 0x22) and VBUS_SENSE (GPIO34) in range; SDZ high only once PVDD_SENSE (GPIO39) reads the rail and at least 2.5 s after the codec's VMID is enabled; SDZ low at once when VBUS_SENSE drops. | Pop and brown-out behaviour (AUDIO-17, POWER-13). |
| L5 | **Turn-on ramp.** PVDD now ramps in about 28 ms (was 6 ms): wait 50 ms after PVDD_EN before reading PVDD_SENSE. | R6 220k + C19 4.7 uF/16 V (about 2.5-3 uF at its 6 V bias) (POWER-3/4). |
| L6 | **Speaker taps.** The ES8388 LIN2/RIN2 inputs carry SPK_L_P / SPK_R_P. The divider with the ADC's input resistance is 0.0625 (not 1/11). One leg of a BTL output also carries half the PVDD ripple, so a single-leg THD reading is an upper bound. | AUDIO-13, docs/remote.md. |
| L7 | **Line-in scale.** The line-in divider is now 15k/10k: the FLX4's 2.1 Vrms reads 0.62 Vrms at LIN1/RIN1, -4.2 dBFS. | AUDIO-7. |
| L8 | **Vol- button on GPIO36.** Debounce by re-reading the level after 30-40 ms; the ESP32's ADC power-up glitch (80 ns, errata 3.11) must not count as a press. | DIGITAL-6. |
| L9 | **Flash layout** for recovery + two OTA slots fits the N8R8: recovery 0x10000/0x1A0000, ota_0 0x1B0000/0x300000, ota_1 0x4B0000/0x300000, settings 0x7B0000/0x40000, coredump 0x7F0000/0x10000. OTA slots 90 % full. | DIGITAL-11. |
| L10 | **USB serial** is now a CH340B (same 1A86:7523 driver as the CH340C). | Stock (docs/bom-stock.md). |

## core (Plus / Ultra)

| # | Duty | Why (finding) |
|---|---|---|
| C1 | **Expander at 0x74.** U17 is now a TCA9539 (was TCA9555 at 0x20); same port map. Its RESET follows ESP_EN, so after any EN reset every pin is an input and R40-R45 hold the amps, PVDD and the PHY oscillator off. Before a software restart (panic, OTA reboot), write P05 (PHY_RST) low first: a software reset doesn't move EN, and the 50 MHz oscillator on GPIO0 would otherwise be running when the strap is read after an RTC watchdog reset. | DIGITAL-1/2, PARTS-4 |
| C2 | **Output governor and fault handling.** TPA3220s (pad-down, no heatsink, ~26 C/W into the board). The 60 s average speaker power, both channels, is at most **10 W on Plus**. Plus fits zone B and plays L on zone A and R on zone B (`HIFI_SPLIT`), so each chip carries one channel, ~1.3 W, Tj ~104 C in the box as drawn at 30 C room (hwde check_thermal). It is at most **8 W on Ultra with one zone, 12 W with both** (two channels per chip). Implemented in `hifi_gov` (`thermal_out_w`, `thermal_out2_w`, 60 s). Plus rises to 19 W once the box has ~1500 mm2 inlet, riser and outlet (docs/review-2/heatsink-options.md). Over 5 s: at most 70 W in (2.5 A). OTW_CLIP (expander P11) low for over 200 ms: -3 dB every 2 s. FAULT (P10) low: mute, 1 s, toggle the amp's RESET, at most 3 retries in 10 min. | THERMAL-2; one pad-down chip can't hold loud 4 ohm music |
| C3 | **Thermal check at bring-up.** The board is the TPA3220's heatsink. Bring-up plays 1/8 power into 8 ohm for 10 min and must see no OTW. Then it plays 1/8 power into 4 ohm with the C2 governor on, and must see no FAULT. It logs the board temperature beside U15, so C2's 16 W can be raised to what the board really holds. | THERMAL-1 |
| C4 | **PVDD order.** PVDD_EN (P03) only with a PD contract of 9 V or more; hold both amps in RESET before dropping PVDD_EN (it decays with a 1.1 s time constant). PVDD now also drops in hardware when the CH224Q's PG goes high (contract lost). PVDD ramps in about 60 ms (was 12 ms). | POWER-2/6/13 |
| C5 | **DJ-direct.** Hold both amps in RESET across a DJ_DIRECT change and release them in one port-0 write (also what controller/peripheral sync needs); invert the sub/ADC path while DJ_DIRECT is on (the analog path reaches the speakers inverted). | AUDIO-8/9 |
| C6 | **Speaker taps.** The ADC (ES7243E) hears the RCA input by default. Drive TCA9539 P17 (TAP_SEL_IO) high to switch the ADC to the speaker taps (Q7 inverts it to TAP_SEL low; the ES9018K2M GPIO2 is not used). Ratio A_SPK_x+ to ADC input 0.0276 with the ADC's input resistance (full power is about -11 dBFS). Ultra has taps on zone A only. | AUDIO-2/3, DIGITAL-12, docs/remote.md |
| C7 | **DAC clock** is now 49.152 MHz (was 50 MHz): set the ES9018K2M's clock-dependent registers (ASRC/DPLL) for it. | PARTS-3, AUDIO-10 |
| C8 | **Sub DAC** takes its own I2S data line (IO12) on shared BCK/LRCK: the firmware needs I2S1 as a TX slave on those pads, started with I2S0 under one critical section; squeezelite drives one I2S output only. | DIGITAL-3 |
| C9 | **Ethernet**: the PHY's nRST now lags OE by about 10 ms (C47 1 uF); the IDF LAN87xx driver's soft reset after enabling stays. | DIGITAL-5 |
| C10 | **ES7243E**: register 0x02 bit 7 set (clock from BCLK, MCLK pin grounded); probe I2C 0x10 and 0x14. | PARTS (open item), AUDIO |
| C11 | **USB serial** is a CH340B (same driver as the CH340C). | Stock |
