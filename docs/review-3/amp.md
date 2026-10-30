# R3 AMP review: class-D amps and speaker outputs (in progress)

Board files reviewed at the commit on this branch (ac484b1). Tests in `docs/review-3/sims/amp/`.

## Lead's question: Lite U8 SYNC (pin 16) unconnected

Valid. SLOS708G pin table (p4): SYNC is "Clock input/output ... Direction
determined by GAIN/SLV terminal"; section 7.3.1: "In Master mode, SYNC terminal
is an output". Master/slave is set by the GAIN/SLV divider, not the AM2-0 pins
(AM2-0 only pick the PWM frequency, SLOS708G 6.6 / table 6). U8's divider is
R50 20k to GND and R51 100k to GVDD, which is "Master 26 dB" in table 1, so
SYNC is a clock output and may float. The TPA3118D2EVM (SLOU342A fig 3) only
brings SYNC to a test point through R20 4.7k / C30 47 pF. There is one amp on
Lite, so it has nothing to sync with. It doesn't matter.
`sims/amp/check_amp_netlist.py` asserts the master divider and the open SYNC.
