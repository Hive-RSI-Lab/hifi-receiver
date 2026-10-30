# Review round 2 (review-3): system and field use (SYSTEM)

Reviewer: SYSTEM domain, task 261030-151656. Draft in progress; findings are
written as they are found.

## Findings

### R3-SYSTEM-1 (major, both, new): the power governor lets the input pass the charger's contract

The governor (`firmware/components/hifi/core/hifi_gov.c`) is what keeps a USB-C
charger from tripping. Two faults, measured by linking the unchanged C file
into `docs/review-3/sims/system/run_gov.sh`:

1. **It can't see clipping.** `channel_w()` caps each channel's power at the
   clip power, and the limiter takes `sqrt(avail / p_out_pre)` from that
   capped figure. When the program is louder than the rail, which happens
   on core whenever the contract is 20 V (vfs_peak 25.8 V against a
   19.0 V clip), the gain it applies leaves the peaks still clipping. The
   amp then draws the clip power while the governor thinks it is on budget.
   Core on a 20 V / 3.25 A (65 W) charger, 0 dBFS: **104.4 W in, 1.61x the
   contract**, against its own estimate of 58.5 W. Core 20 V / 5 A: 106.6 W
   (1.07x). Lite on a 15 V contract (it runs 14-25 V): 60.6 W against 45 W.
   Most chargers have no 28 V EPR, so 20 V is core's normal case.
2. **Its 1 s window is slower than a charger.** After a quiet passage, a
   0.6 s 0 dBFS 45 Hz drop runs 1.39-1.64x over the contract for
   **507-597 ms** before the 1 s average catches up. The PVDD bulk holds
   0.08 J (Lite 690 uF, 20 to 13 V) to 0.43 J (core 1410 uF, 28 to 13 V).
   The excess is 9-21 J, so the charger has to carry it. A PD source's
   overload tolerance is optional (PDO "peak current" bits) and usually
   1.1-1.5x for milliseconds. So: OCP or droop, VBUS under the 13 V floor,
   the amp off, music gone for 1 s or more, or the charger latches off.

Fix (firmware, no board change): compute the limiter target from the
**unclipped** power (keep the clip cap for reporting only), and shorten
`tau_s` to 0.05 s with `attack_s` 0.01 s. With the fix, `run_gov.sh` shows
every steady case at or under 0.9x the contract (core 20 V/65 W: 58.6 W).
Bursts still run 1.32-1.44x for 91-117 ms, part of which is the 45 Hz
power ripple in the 21 ms measurement. Bring-up check: a 45 Hz 0 dBFS drop
on a 65 W charger, while scoping VBUS.
