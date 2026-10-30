# Convergence: review rounds until a round finds no blocker or major

The person, 10-30: "did you do many step backs, consider tons of app notes,
look at other designs, their issues, build new tests etc? Until you reached
diminishing returns and converged on a design". This file is the trend, board
by board. The rule: a board has converged when a full round of independent
reviewers finds no blocker and no major on it. Each round's reviewers read
the KiCad files, the generator sources and the vendors' documents. They don't
read our own conclusions until they have formed their own view.

| Round | Folder | Lens | Reviewers |
|---|---|---|---|
| 1 | `docs/review-2/` | datasheets, margins, sims | 6 domains + a second read (10-29) |
| 2 | `docs/review-3/` | vendor app notes and EVMs, other designs' field issues, new tests | 6 domains: power, amp, audio, digital, layout, system (10-30) |
| 3 | `docs/review-4/` | Lite only: round 2's fixes tried to break, the least-looked-at corners | 2 cross-domain reviewers (10-30) |

## Findings per board, as filed (a finding on "both" counts once on each)

| Board | Round 1 | Round 2 | Round 3 |
|---|---|---|---|
| lite-main | 0 blockers, 6 majors, 16 minors | 0 blockers, 4 majors, 17 minors | **0 blockers, 0 majors**, 11 minors |
| core (Plus/Ultra) | 6 blockers, 16 majors, 26 minors | 1 blocker, 14 majors, 20 minors | not yet (below) |
| ultra-front | not reviewed | 0 blockers, 1 major (firmware), 1 minor | not yet |

### Lite: converged

Round 3 found no blocker or major. Two reviewers covered:
- power, amp and digital (`docs/review-4/pad.md`);
- audio, layout and field use (`docs/review-4/als.md`).

Their minors and what became of them:

| Minor | What | Done |
|---|---|---|
| R4-PAD-1, R4-ALS-3 | The mute's sense transistor drifts with temperature. At 70 C, a slow unplug leaves -0.33 V at worst, against -1.61 V unmuted. | R36/R38 100R -> 1k, which divides that further and limits an ESD strike on the tip. Bring-up: scope SUB_L and +5V on a warm unplug. |
| R4-ALS-1 | A minimum-Vth 2N7002 leaks on the sub line's negative peaks when warm (0.1-0.4 % THD at -6 dB). | The sub cap is now -9 dB (0.014 % worst corner). |
| R4-PAD-5, R4-ALS-2 | squeezelite's codec init put the sub outs at 0 dB for 2 s before the cap applied. | The init (`patches/0001`) now starts them at the cap. |
| R4-PAD-7, R4-ALS-6 | Stale firmware constants. | Already fixed (commit 6db7273; the reviewers read the tree before it). |
| R4-ALS-4 | No TVS at the sub RCA: an 8 kV strike puts 31.5 V on the mute FETs' gates. | Open: a PESD5V0S2BT (C49338) at J3 at the next layout touch. The 1k build-out cuts the strike current. |
| R4-PAD-8, R4-ALS-5 | No ESD part on USB D+/D- (core has one). | Open: a USBLC6-2SC6 at J1 at the next layout touch. |
| R4-PAD-4 | 13-16 uF on VBUS at attach (USB-C allows 10 uF). | Bring-up: Apple and multi-port chargers. If one refuses, C4 goes unfitted. |

The trend on Lite was 6 majors, then 4, then 0. Rounds 1 and 2 kept finding
real faults, each from a new lens: datasheet margins, then reference designs
and field issues. Round 3, built to break round 2's fixes, found only corner
cases at the minor level. That is the diminishing return the person asked
for.

### core: not converged; round 2's majors are fixed or filed

Round 2 found one blocker: J4/J5 couldn't take the RCA jack, because round
1's fix never reached the board. It is fixed. Of round 2's 14 core majors:
- 9 are fixed (`docs/review-3/dispositions.md`).
- 5 are filed as two tasks, each with its own PR:
  - **261030-215553, core amp-area layout ECO.** The TPA3220 bootstrap and
    PVDD HF caps are 6-29 mm from their pins, and the amp input caps are
    45-56 mm from the IN pins (R3-AMP-1, R3-AMP-2, R3-LAYOUT-1). It also
    covers the hardware half of R3-POWER-2. It re-places the densest copper
    on the board while task 261030-140102 re-routes the speaker outputs, so
    it starts after that lands.
  - **261030-215602, core firmware.** The line in goes through the ADC and
    DSP, so the subs play in DJ mode and DJ input is governed; and Ultra's
    front board gets driven (R3-AUDIO-3, R3-SYSTEM-4, R3-DIGITAL-3).

The trend on core so far was 6 blockers and 16 majors, then 1 blocker and 14
majors. Round 2 was still finding majors, many of them layout faults that
round 1 didn't look at. **Core needs round 3 once 261030-215553 lands:** two
or three reviewers, `docs/review-4/brief.md`'s pattern pointed at
`boards/core`, and more rounds after it if round 3 still finds majors.
ultra-front gets its first full round with it.

## Tests the rounds built

Every round left re-runnable checks:
- Round 1: `docs/review-2/sims/` (PVDD switch, LC filters, RMII clock).
- Round 2: `docs/review-3/sims/`:
  - power: PD steps, hot plug, EPR step, turn-on SOA;
  - amp: netlist against TI's tables, LC and inductor saturation, cap-to-pin distance;
  - audio: VREF and crosstalk sims, the level budget;
  - digital: strap levels at every reset kind, firmware pins and Ethernet address against the netlist, I2C;
  - layout: CPL pin 1 against the LCSC models, footprint against library, JLC stock, ground reference, coupling, ESD scan, thermal;
  - system: the governor against charger contracts, Lite thermal, the firmware duties.
- The fixes' own sims: `docs/review-3/sims/fix/`.
- Round 3: `docs/review-4/sims/`: mute corners across Vth, temperature and sub impedance; power loss; ESD; power-on; GND islands; a netlist and pin-map check of Lite's power and digital side.

Some of these exit non-zero on purpose: they record an open minor, or a
filed core major, until its fix lands.
