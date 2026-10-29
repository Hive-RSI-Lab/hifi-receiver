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
| L5 | **Turn-on ramp.** PVDD now ramps in about 28 ms (was 6 ms): wait 50 ms after PVDD_EN before reading PVDD_SENSE. | R6 220k + C19 2.2 uF (POWER-3/4). |
| L6 | **Speaker taps.** The ES8388 LIN2/RIN2 inputs carry SPK_L_P / SPK_R_P. The divider with the ADC's input resistance is 0.0625 (not 1/11). One leg of a BTL output also carries half the PVDD ripple, so a single-leg THD reading is an upper bound. | AUDIO-13, docs/remote.md. |
| L7 | **Line-in scale.** The line-in divider is now 15k/10k: the FLX4's 2.1 Vrms reads 0.62 Vrms at LIN1/RIN1, -4.2 dBFS. | AUDIO-7. |
| L8 | **Vol- button on GPIO36.** Debounce by re-reading the level after 30-40 ms; the ESP32's ADC power-up glitch (80 ns, errata 3.11) must not count as a press. | DIGITAL-6. |
| L9 | **Flash layout** for recovery + two OTA slots fits the N8R8: recovery 0x10000/0x1A0000, ota_0 0x1B0000/0x300000, ota_1 0x4B0000/0x300000, settings 0x7B0000/0x40000, coredump 0x7F0000/0x10000. OTA slots 90 % full. | DIGITAL-11. |
| L10 | **USB serial** is now a CH340B (same 1A86:7523 driver as the CH340C). | Stock (docs/bom-stock.md). |
