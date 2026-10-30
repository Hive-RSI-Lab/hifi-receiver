# Review round 2 (review-3): AUDIO, small-signal chain, both boards

DRAFT: written as I go; findings below are provisional until the "Final" line appears.

## Findings so far (provisional)

- R3-AUDIO-1 (major, core, new): VREF = +5VA/2 through R90/R91 470R/470R with C64 10 uF (corner 68 Hz) reaches MAIN_L/R
  at gain 1 (difference amp) and then the TPA3220 at 24 dB. Rail steps from Wi-Fi bursts become ticks at the speaker.
  Simulation pending (sims/audio/tb_vref_*.cir).
- R3-AUDIO-2 (major, lite-main, new): sub out C47/C48 10 uF from the ES8388 LOUT2/ROUT2, which sit at VMID 1.65 V:
  on power loss the 1.65 V collapses in a few ms and the step reaches the sub input (tau 82 ms with a 10k sub input). There is no mute.
- R3-AUDIO-3 (minor, lite-main, new): the round-1 caps (LOUT1 -4.5 dB, LI2LOVOL -3 dB) plus the 15k/10k divider together limit DJ-direct to about 5.5 Vrms, 7.8 dB under clip.
- R3-AUDIO-4 (minor, lite-main, repeat AUDIO-13/AUDIO-7): firmware constants LINE_DIVIDER 2.0, TAP_DIVIDER 11 and HIFI_HAS_TAPS 0 are stale.
- R3-AUDIO-5 (minor, core, new): 16-bit I2S, so BCLK = 32 fs. The ES7243E (MCLK from BCLK) needs MULT_PRE x8, and the core capture is not implemented.
- R3-AUDIO-6 (minor, core, new): no RF shunt cap at the core RCA line-in (Lite has 100 pF).
- R3-AUDIO-7 (minor, both, new): ground loop with an earthed laptop and an earthed sub; no lift or sense option.
