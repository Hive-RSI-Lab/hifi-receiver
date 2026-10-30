# Round 2: every blocker and major, and what was done

Task 261030-151656 (issue #118). Round 2's reviews are the other files in this
folder: power, amp, audio, digital, layout and system. **fixed** means
changed on the board or in the firmware, with the check that shows it.
**filed** means handed to a separate task with its own PR, because the
change re-places and re-routes copper that another task is also editing.

| Finding | Board | Severity | What it was | Disposition |
|---|---|---|---|---|
| R3-LAYOUT-12 | core | blocker | J4/J5 (sub out, line in) were still the old RCA footprint: no holes for the jack's legs, so it can't be inserted. Round 1 fixed the library only. | **fixed**: `kicad/gen/eco_rca.py` puts the library's slots and leg holes on J4/J5. A stitch via and a via under the new slot went, and I2S_DOUT_MAIN was rerouted clear. DRC clean. `docs/review-3/sims/layout/fp_vs_lib.py` catches a recurrence. |
| R3-AUDIO-2 | lite-main | major | The sub outs thump at full scale (-1.61 V into 10k) when power is lost. | **fixed**: a +5V-sensed mute (Q11-Q15, R80-R82, C90). Sim `docs/review-3/sims/fix/lite_mute/`: -25 mV at power loss. The signal is untouched up to the firmware's new -6 dB sub cap. |
| R3-SYSTEM-1 | both | major | The governor took its gain from a clip-capped power and averaged over 1 s, so the charger saw 1.6x its contract. | **fixed** in `hifi_gov.c`: the gain comes from the unclipped power, with a 50 ms window and 10 ms attack. `docs/review-3/sims/system/run_gov.sh` passes; host tests added. |
| R3-POWER-3 | both | major | An average limit lets a bass tone's loudest half-cycle draw 1.7x the PDO current. | **fixed**: a per-block (~10 ms) peak limit at 1.0x the contract, held for 0.5 s. A host test drives 45 Hz at 0 dBFS on 65 W and checks every block is within the contract. |
| R3-SYSTEM-2 | lite-main | major | Lite's thermal limit, duty L1, wasn't implemented. | **fixed**: thermal limit of 9 W/ch over 30 s. That is below L1's 13 W, which R3-LAYOUT-13 showed reaches 126-149 C. `thermal_lite.c` passes. |
| R3-SYSTEM-3 / R3-DIGITAL-1 | core | major | GPIO0 can latch the 50 MHz PHY clock on a restart or a brownout reset, and the box sits in download mode. | **fixed**: a shutdown handler turns the PHY clock and the amps off before any software restart (duty C1). U21, a TLV803S 2.93 V supervisor on ESP_EN, resets the expander and the clock on a brownout. Left open: an RTC watchdog reset (a firmware hang). |
| R3-DIGITAL-2 | core | major | squeezelite looks for the LAN8720A at SMI address 1, but the board straps it to 0, so Ethernet never starts. | **fixed** in firmware: `patches/0002` sets phy_addr -1, ESP-IDF's auto-detect. `fw_pinmap.py` reads our patches. |
| R3-POWER-2 | core | major | PVDD (1410 uF) takes 122-196 ms to charge, but the amps came out of reset at 100 ms. Q1's turn-on reaches Tj 125-135 C. | **fixed** (firmware half): soft start 500 ms, and settle 500 ms after any contract change. **filed** (hardware half: 100 nF parallel to C4, 2.2 uF parallel to C8, which takes Q1 to 96 C): task 261030-215553. |
| R3-POWER-4 | core | major | Core took every non-EPR PD contract to be 20 V, so the 14 V floor and the budget were wrong on 5/9/12/15 V chargers. | **fixed**: the firmware reads the CH224Q's source capabilities (0x60-0x8F) and asks for the highest fixed PDO up to 20 V. With an unreadable list it assumes 20 V as before. Host tests cover header, bare and nonsense layouts. |
| R3-POWER-5 | core | major | PVDD could close on the 20 V SPR contract just before the step to 28 V EPR. | **fixed**: when EPR is offered, PVDD waits for EPR entry (up to 3 s) plus a 500 ms settle. |
| R3-AUDIO-1 | core | major | VREF was filtered by only 10 uF (68 Hz corner), so Wi-Fi dips on +5VA ticked at the speaker. | **fixed**: C64 is 100 uF 1206 (6.8 Hz corner; the sim gives 3.5 mVpp against 27). |
| R3-AMP-1 | core | major | Bootstrap caps are 6-29 mm from their BST pins. | **filed**: task 261030-215553, the core amp-area layout ECO. |
| R3-AMP-2 | core | major | PVDD HF caps are 6.5-8 mm from the PVDD pins (estimated spike 37.1 V against 37 V abs max). | **filed**: task 261030-215553. |
| R3-LAYOUT-1 | core | major | Amp input caps are far from the IN pins: 45-56 mm high-impedance runs over PVDD. | **filed**: task 261030-215553. |
| R3-AUDIO-3 | core | major | In DJ-direct the subs are silent, because the ES7243E is never set up. | **filed**: task 261030-215602, core firmware for the line in through the ADC/DSP. |
| R3-SYSTEM-4 | core | major | DJ-direct has no thermal limit, and OTW was never acted on. | OTW step-down **fixed** (-3 dB every 2 s while OTW, duty C2). The governed line-in path is **filed** in task 261030-215602. |
| R3-DIGITAL-3 | ultra-front | major | No firmware drives Ultra's front board. | **filed**: task 261030-215602. |

Minors done along the way:
- R3-SYSTEM-5 (L4/C5 pop duties): amps held until the codec settles and across an input change.
- R3-SYSTEM-9: the input is saved in NVS.
- R3-DIGITAL-5 and R3-AUDIO-5: Lite's tap and line-in constants match the board.
- R3-DIGITAL-4: P17 is named EXP_TAP_SEL and left an input by design.
- R3-LAYOUT-13: Lite's governor limit is 9 W/ch.

The other minors and notes stand as written in their reviews. Each is cheap,
an EMI or ESD margin, or a bench check, and none stops a board.
