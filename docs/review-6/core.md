# Review 6: core board (Plus/Ultra), pre-order, after PR #149

Tree: `~/projects/hifi-receiver` main at 09677be, read-only. All measurements ran on copies in `/tmp/review-6-core/` (scripts and renders are there).
No hwde gates were run. Severity follows `docs/review-5/brief.md`.

**Result: 0 blockers, 2 majors, 8 minors.** I found nothing that makes the PCBA spin fail or damages it.
M1 is a one-line decision to settle before paying. M2 affects the printed box only, not the JLC order.

## Findings

| id | severity | what | evidence | fix |
|---|---|---|---|---|
| R6-M1 | major | **The inner copper weight is contradictory on the order step.** ORDERING.md tells the owner to order 0.5 oz inner. The README and the current design need 1 oz. The named stackup exists only as 0.5 oz. Followed as written, the In2 VBUS/PVDD planes are half the thickness the current budget assumes. | `boards/core/fab/ORDERING.md:42` says "1 oz outer and 0.5 oz inner". `boards/core/README.md:46-48` says "Order it with inner copper 1 oz, not JLC's 0.5 oz default". `kicad/gen/power_trunk.py:7-9`: 5 A at dT 10 C needs 8.06 mm on In2 at 0.5 oz, 3.5 mm at 1 oz. `docs/review-5/pa.md` R5-PA-4 measured the Q1-to-amp In2 neck at 3.88 mm, which passes only at 1 oz. Board stackup block: In1/In2 0.035 mm. `~/modules/hwde/reference/stackups.yaml:154-165`: JLC04161H-1080B is "1 oz outer / 0.5 oz inner". `docs/known-unknowns.md` power:29 still says 0.5 oz "verified", thermal:37 and `constraints.json` theta notes assume 1 oz / 0.5 oz, and `known_unknowns.py --check` reports 0 open, so the tool does not catch this. | Choose 1 oz inner. Fix ORDERING.md:42, confirm on JLC's quote page which layup a 1 oz inner build gets (it is not the 0.5 oz 1080B template, so dielectrics may change), and correct the README's stackup name and the known-unknowns rows. |
| R6-M2 | major (box only) | **Every banana hole in both boxes is 0.30 mm off the socket axis.** The hole centre comes from the part's bbox, and the bbox is inflated on one side by a stray silk line in the EasyEDA footprint. Nominal clearance on one side falls from 0.30-0.35 mm to 0.00-0.05 mm. The board's M3 holes allow only about 0.1 mm of shift, so a typical undersized FDM hole will bind the collar. The fit check cannot see this: its port is the square inscribed in the collar. | `enclosure/core_box.py:157` uses `x = (p.bbox[0] + p.bbox[2]) / 2`. `enclosure/out/plus/board.json`: J20 x 15.4125, bbox 9.762-20.462 (-5.65/+5.05). Measured from `out/plus/base.stl` and `out/ultra/base.stl`: J20 hole centre 18.112 in box frame = 15.112 in board frame (wall 2.4 + gap 0.6), all 8 holes offset alike. RCA holes are exact (110.412 - 3.0 = J4's 107.412). Footprint: collar silk ±3.0 and body ±5.05 are symmetric on the pad line; the stray line is at -5.65. **Lite has the same offset**: hole 80.1 - 3.0 = 77.1 against J4 77.4. | In `banana_ports()`, use the part's own x, not the bbox centre (`x = p.x`). Rebuild Plus, Ultra and Lite. Add a fit check on hole-to-axis offset. |
| R6-m1 | minor (repeat R4-BAN-5) | **Nothing tells JLC which way the sockets face.** They are symmetric 2-pin THT with their silk removed, and ORDERING.md does not mention them at all: no orientation, no 2.65 mm overhang, no preview check (its preview note covers only bottom parts). Back to front would put 8 sockets over the snubber row. The CPL itself is right. Lite's remark was never carried over. | `grep socket/24.243/THT boards/core/fab/ORDERING.md`: no hits. `kicad/gen/banana_silk.py`. `docs/convergence.md:57`: Lite's remark is in Lite's README only. CPL: J20-J27 rotation 90, centred on the pin midpoint. EasyEDA footprint at 0 deg has the nose toward +x, which at 90 deg points to the rear edge. | Add an ORDERING.md remark: "J20-J27: collar overhangs the rear edge by 2.65 mm, check in JLC's preview". Add a small silk arrow or F.Fab note at y ~130. |
| R6-m2 | minor (repeat R4-BAN-3) | **GND pour is about 0.1 mm from all socket rings, on both faces** (raster resolution 0.05 mm). Lite was fixed to 0.3 mm; core was not. The rings are hand or wave soldered and carry up to 28 V swing. A bridge shorts a speaker leg to GND; the amp's protection should trip, so this costs rework rather than a dead board. | Raster of saved fills: J20 and J27, pins 1 and 2, F.Cu and B.Cu all 0.10 mm. `core.kicad_dru:9` floor 0.1016 mm. `docs/convergence.md:53`. | Add a 0.3 mm clearance rule for the SPK nets' pads against GND, then refill. |
| R6-m3 | minor (new in #149; estimate) | **LIN_L now runs next to I2S_DOUT_MAIN on B.Cu.** 15.4 mm at 0.39 mm centre to centre (0.185 mm edge gap), plus about 9 mm under 1 mm. The old route had no neighbour under 0.5 mm. There is no RC on this input before the ADC other than the 4.7k/4.7k divider. Estimated Cm 0.6 pF gives 4-17 mV of about 3 MHz at LIN_L for a 100-470 ohm source, and 44 mV if unplugged. That is out of band but data-correlated, and it reaches a delta-sigma ADC. Not simulated or measured. | `/tmp/review-6-core/oldpair.py`: old LIN_L 30.9 mm, no neighbour; new 69.8 mm, DOUT 15.4 mm, near (126, 138). `kicad/gen/blocks_audio.py` R100/R101. | Re-route LIN_L 1 mm or more away from DOUT on this run. If left as is, bench-check the ADC noise floor with DAC audio playing. |
| R6-m4 | minor | **C229 (A L+'s 1 uF filter cap, Plus's left +) reaches L2 only through the socket.** Path is about 70 mm (Manhattan upper bound): vias, B.Cu column, band, spine, J20 pin 1, then 13.9 mm of 0.205 mm F.Cu. Its partner C226 is 6 mm. So 600 kHz ripple (about 1.17 A p-p, 0.34 A rms, calculated) runs the full staircase, and a full-level 20 kHz signal puts about 1.6 A (calculated) through the 0.205 mm track. banana_snub.py hooked A R- and B L+ but not this leg. Ripple at the socket itself is still filtered, so this is EMI and margin, not function. | `/tmp/review-6-core/path.py`: C229 69.7, C226 6.1, C235 16.6, C232 19.0, C327 30.5, C330 19.0, C333 7.8, C324 6.1 mm. | Add a via and short B.Cu stub from C229/R230 into the A L+ column, as the snub script does for the others. |
| R6-m5 | minor (repeat R5-PA-5) | **The README's "pours are 2 mm wide" is not true of the whole path.** Pieces the ECO kept are narrower. By IPC-2221 external, 3.7 A rms gives about 20 C rise at 1.2 mm and about 29 C at 0.95 mm. Sustained output is governed to 9.6 W/ch (about 1.5 A rms), so there is no failure at realistic current. | Slices of saved fills: F.Cu strips y 134.05-135.25 are 1.2 mm wide, 5-12 mm long (A L-, A R+, B L-, B R+). B_SPK_R+ (Plus's right +) necks to 0.95 mm at x 90.5-91.75, from GND via (90.54, 135.70). B.Cu columns are 1.6-1.75 mm for y 132-146. Staircases themselves 2.0-2.6 mm. `boards/core/README.md:120` "The pours are 2 mm wide". | Correct the README. Optionally move the GND via at (90.54, 135.70). |
| R6-m6 | minor | **ORDERING.md step 3's heading undercounts the amp.** It says "U15, and U16 on Ultra", but Plus also fits U16. Buying from the heading gives a Plus with no right channel. The sub-bullet "2 per board" and `prebuy.csv` are correct. | `boards/core/fab/ORDERING.md:16` and `:29`. `fab/prebuy.csv:42` lists U15,U16. | Change the heading to "U15 and U16, both tiers". |
| R6-m7 | minor (untested) | **Plus/Ultra box assembly is tight and undocumented.** The collars go 2.05 mm into the N wall (0.3 mm lateral play), and J1 goes 1.93 mm into the W wall. J1's opening allows only ±1.65 mm in y, against the collars' 2.05 mm of travel. Geometry suggests it fits only with a slight in-plane swing about the socket row. The enclosure README has assembly steps for Lite only. Ultra also has 7 webs of 0.8 mm between holes at the wall minimum. | `out/plus/board.json` J1 bbox x min -2.53. `enclosure/README.md:102`: USB-C opening 13.4 mm, shell 10.1 mm; "Assembly" at line 46 is Lite. `fit.md` does not check insertion. R4-BAN-1 suggested a slot. | Test on a printed base with a sample socket before printing more. Write the steps. Consider a slot across the Ultra row. |
| R6-m8 | minor | **Stale text and files.** None of these change the board. | `boards/core/README.md:282` "about 15 ms" and `:66` "~60 ms soft start", against 188-257 ms t90 in R5-PA-1. `firmware/components/hifi/esp/hifi_board_core.h:24` says J6/J11. `fab/ultra/core-ultra_BOM_mouser.csv:24` and its digikey twin (Oct 31) still list KF301 on J6,J7,J10,J11 next to the fresh `core_BOM_*`. `enclosure/heights.json` note says bore axis 7.5 (8.1 is correct). | Update the text and delete the two stale BOMs. |

## Checked and clean

**1. Speaker path**

- **Net identity (from the PCB pads):**

| Socket | Net | Colour | Inductor | Amp output | Fitted |
|---|---|---|---|---|---|
| J20 | A L+ | red | L2 | U15 OUT1P | Plus + Ultra |
| J21 | A L- | black | L1 | U15 OUT1M | Plus + Ultra |
| J22 | A R+ | red | L4 | U15 OUT2P | Ultra |
| J23 | A R- | black | L3 | U15 OUT2M | Ultra |
| J24 | B L+ | red | L6 | U16 OUT1P | Ultra |
| J25 | B L- | black | L5 | U16 OUT1M | Ultra |
| J26 | B R+ | red | L8 | U16 OUT2P | Plus + Ultra |
| J27 | B R- | black | L7 | U16 OUT2M | Plus + Ultra |

- **Firmware and polarity:** `HIFI_SPLIT` runs both amps on Plus, so L plays on J20/J21 and R on J26/J27. U11 is inverting (DAC P to the - input) and undoes the amp's inversion, so OUTxP is + (`blocks_audio.py:43`, known-unknowns audio:17, which cites the TPA3221 datasheet; I did not reopen it).
- **Connectivity:** every socket's pin 2 reaches its inductor. Pin 1s of J23/J24/J25 join through 1 mm In2 links. No signal on B.Cu crosses those links.
- **Vias:** 6 x 0.6/0.3 mm at each B.Cu leg's inductor.
- **Shorts and clearance:** KiCad DRC on the saved fills gives 0 violations and 0 unconnected. No floating GND islands (F.Cu 106 polygons, B.Cu 28, all with a via or pad).

**2. Fab package**

- **Gerbers are current:** the PCB last changed in 631739f and the fab was regenerated afterwards in 2546fea. A fresh export from the current PCB matches F/In1/In2/B Cu, mask, paste and Edge.Cuts and the drill file, ignoring header lines. The zip matches `fab/gerbers/`. Silkscreen was not diffed.
- **BOM, CPL, PCB:** Plus 298 placed / 88 BOM lines, Ultra 302 / 86, both as documented. No mismatch in refs, position, side or rotation.
- **DNP:** Plus omits J22-J25, U19, FB3, C126, J3, R402, R403. Ultra omits SW1-3, U18, R400, C400.
- **Parts:** C4 220 nF C344195 and C8 4.7 uF C1779 are in both BOMs and on the PCB. `bom_stock.py core` at 100 boards: 87 lines ok, including C7437321/2; only TPA3220 is short, which is the documented pre-buy.
- **Bottom count:** 14 as ORDERING.md states (C207-210, C307-310, C218, C220, D6-D9).

**3. Mechanical**

- **Mounting holes:** H1-H4 match `enclosure/core.board.json`. Clear of J20 and of the A L+ column (1.26 mm).
- **Rear edge:** collar 2.65 mm past the edge, body 0.2 mm past, inside the 0.6 mm gap. J27 to J4 and J5 to U19 clear.
- **Hole height:** matches 8.1 mm over the board.
- **Antenna:** no copper, track, via or pad on any layer in x 148.9-154.64, y 189.6-213.5. The right edge there is unchanged, the wing starts 40 mm away, and the box has open air beside it.

**4. RCA/TOSLINK relocation**

- **Plane splits:** no crossing. LIN, SUB and SPDIF on B.Cu sit over In2 +3V3 throughout (the +3V3 zone starts at x 98, and these nets stay at x >= 104). On F.Cu they sit over In1 GND. The small "no plane" lengths are at via and pad antipads, not splits; I inferred that from the short lengths rather than plotting each one.
- **Class-D proximity:** LIN_L, LIN_R and OPT_SPDIF have no OUT/SPK copper within 5 mm. SUB1/SUB2 have about 11 mm within 5 mm and 2 mm within 1 mm, near the B R- band; they are low-impedance outputs.
- **Remaining audio note:** R6-m3 (digital neighbour, not class-D). SUB1/SUB2 run 47 mm side by side, which is sub-to-sub only.

## Not checked

- hwde gates, per the brief.
- A from-scratch zone refill. DRC was on the saved fills, and the gate reports were not re-derived.
- JLC's own library model and preview for C7437321/2, and bottom-side rotations (JLC preview only).
- Amass's drawing: collar and bore figures are from R4-BAN-1. No physical sample.
- Hard-reset sim with C8 4.7 uF. By hand, fast-off roughly doubles from the ~2 ms README figure. R5-PA-1's turn-on sim of these values was read, not rerun.
- Thermal or chimney effect of the new outline. RF or SI simulation.
- `bom_stock.py core/ultra` was not run (Ultra's extra lines were not stock-checked).
- Firmware beyond the channel map and PVDD sequence.
