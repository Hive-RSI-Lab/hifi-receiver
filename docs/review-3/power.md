# Review round 2 (review-3): POWER

Reviewer: power domain (POWER). Boards as on main at ac484b1. Work in progress; sections are filled in as they are checked.

## Lead's question: Lite U2 (TPS54302) EN left unconnected

**Answer: floating EN is correct here. Don't add a divider. Never tie EN to VBUS.**

- Netlist `lite-main.net`: U2 pad 5 is its own net, `unconnected-(U2-EN-Pad5)`. PCB: no track or via lands on it (`sims/power/lite_u2_en.py`, PASS). The nearest copper is pad 4 BUCK5_FB and pad 6 BUCK5_BST, each 0.95 mm away, plus a B.Cu PD_PG track 0.45 mm from the pad centre on the far side of the board.
- TI SLVSDG6C (rev C, March 2026) says, in table 4-1, "EN: Float the EN pin to enable." Section 6.3.5 adds: "The EN pin has an internal pullup-current source which allows the user to float the EN pin to enable the device." Ip is 0.7 uA at VEN 1 V, and the enable threshold is 1.23-1.28 V rising and 1.1-1.16 V falling (section 5.5).
- With EN floating, the only undervoltage lockout is the internal VIN UVLO. It turns on at 3.8-4.4 V (rising) and off at 3.3-3.9 V (falling). Each charger voltage in turn:
  - **5 V-only port or the attach phase, 4.75-5.25 V at the source.** The buck starts, because 4.75 V minus a ~0.05 V cable drop is above the 4.4 V maximum start point. It then runs in dropout: section 6.3.10 says it runs at 100 % duty while BOOT-SW stays above 2.1 V. That gives +5V ≈ VIN − I·(R_HS + DCR) ≈ 4.6-4.7 V at 0.5 A. That is enough for U3 (SY8089, 2.5-5.5 V) to make 3.3 V and for U4 (LP5907, 250 mV dropout) to make +3V3A. So the ESP32 runs, joins Wi-Fi and reports "no PD contract".
  - **9/12/15/20 V PD and QC 9/12 V.** These are all within VIN 4.5-28 V (recommended) and 30 V (absolute maximum). PD 20 V + 5 % = 21 V leaves 7 V of margin to the recommended maximum.
- **Why a divider would be wrong.** TI's EVM, TPS54302EVM-716 (SLVUAP9B, section 1.3.3, R4 510k / R5 105k), sets start at 6.74 V and stop at 5.83 V. Copy that and a 5 V-only port, or a dead PD handshake, leaves the board fully dark: no Wi-Fi and no way to report the problem.
- **Why tying EN to VBUS would be wrong.** EN's absolute maximum is 7 V (section 5.1). VBUS is 20 V.
- **Noise.** EN is a ~0.7 uA high-impedance node next to BOOT, which swings 0 to 25 V at 400 kHz. Coupling is capacitive (≈0.05 pF pad-to-pad against a few pF on the pin), so a few hundred mV of AC that settles back. That is well under the 1.1 V falling threshold from the pulled-up level, and the TPS54302's own guidance is to leave EN floating.
- Severity: **note** (R3-POWER-1). There is no required change. If the worker wants a contingency on a later spin, a DNP 0603 footprint from EN to GND makes room for a 1-10 nF filter cap. It would delay start by 1.28 V × C / 0.7 uA, which is 1.8-18 ms.

