"""Fit check for the Lite enclosure against lite-main.

    ~/venv-cad/bin/python enclosure/fit.py      prints each check, writes
                                                enclosure/out/lite/fit.md, exits 1 on a failure

It builds the base and lid from lite.py and the board from
lite-main.board.json, and checks, in the assembled position:

- no part of the board (each footprint's courtyard box from the board top to
  its height in heights.json, and down to its pins) touches the base or lid,
  and every one keeps CLEAR from them;
- the board itself sits inside the walls with GAP all round, on its standoffs;
- every port's mating plug fits: an RCA plug shell (11.5 mm) on each barrel's
  axis, a USB-C overmold (12.4 x 6.5 mm) on J1's, a binding post through each
  post hole, each passing the wall clear of plastic;
- each button stalk is over its switch's centre and stops STALK_GAP above it,
  the LED pipe over the LED, the IR window in front of the receiver's lens;
- each pillar lands on a mounting hole and the parts fit a 220 x 220 mm bed.
"""
from __future__ import annotations

import sys
from pathlib import Path

from build123d import Align, Box, Cylinder, Plane, Pos, RectangleRounded, Rot, extrude

sys.path.insert(0, str(Path(__file__).resolve().parent))
import lite as L  # noqa: E402

CLEAR = 0.3
BED = 220.0
RCA_SHELL = 11.5
USB_PLUG = (12.4, 6.5)   # overmold, corners r 1.5
results = []


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print(("PASS " if ok else "FAIL ") + name + (": " + detail if detail else ""))


def through_rear(s):
    assert s.bounding_box().max.Y > L.Y1, "a plug probe that doesn't reach past the rear wall checks nothing"
    return s


def vol(a, b):
    i = a & b
    return sum(s.volume for s in i.solids()) if i else 0.0


def main():
    base, lid = L.base(), L.lid()
    shell = base + lid
    board = L.board_solid()

    # parts against the enclosure
    worst = None
    for ref, box in L.part_boxes():
        d = box.distance_to(shell)
        if worst is None or d < worst[1]:
            worst = (ref, d)
        if d < CLEAR:
            check(f"{ref} clear of the enclosure", False, f"{d:.2f} mm")
    check("every part clear of base and lid", worst[1] >= CLEAR, f"closest {worst[0]} at {worst[1]:.2f} mm")

    # board in the walls and on the standoffs
    v = vol(board, shell)
    check("board doesn't intersect base or lid", v < 1e-6, f"{v:.3f} mm3")
    bb = board.bounding_box()
    side = min(bb.min.X - (L.X0 + L.WALL), (L.X1 - L.WALL) - bb.max.X, bb.min.Y - (L.Y0 + L.WALL), (L.Y1 - L.WALL) - bb.max.Y)
    check("board edge to inner wall", side >= L.GAP - 0.01, f"{side:.2f} mm")
    rest = Pos(0, 0, -0.05) * board
    check("board rests on the standoffs", vol(rest, base) > 0.5, f"{vol(rest, base):.1f} mm3 contact layer")

    # ports
    y_out = L.Y1 + 5
    for ref in ("J2", "J3"):
        x = L.fp(ref)["u"]
        for zb in L.RCA_BARRELS:
            z = L.Z_PT + zb
            plug = Pos(x, (L.H + y_out) / 2, z) * Rot(90, 0, 0) * Cylinder(RCA_SHELL / 2, y_out - L.H)
            check(f"{ref} RCA plug at +{zb} mm passes the rear wall", vol(through_rear(plug), shell) < 1e-6)
    j1 = L.fp("J1")
    z = L.Z_PT + L.USB_AXIS
    plug = Pos(j1["u"], L.H, z) * extrude(Plane.XZ * RectangleRounded(*USB_PLUG, 1.5), amount=-(y_out - L.H))
    check("J1 USB-C overmold passes the rear wall", vol(through_rear(plug), shell) < 1e-6)
    for u in L.POST_U:
        z = L.Z_PT + L.POST_Z
        post = Pos(u, L.H + L.GAP + L.WALL / 2, z) * Rot(90, 0, 0) * Cylinder(4.0 / 2, L.WALL + 0.2)
        check(f"binding post hole at u={u}", vol(post, shell) < 1e-6)
        nut = Pos(u, L.H + L.GAP - 1.5, z) * Rot(90, 0, 0) * Cylinder(8.2 / 2, 3.0)  # M4 nut + lug inside
        hit = [ref for ref, box in L.part_boxes() if vol(nut, box) > 1e-6]
        check(f"binding post nut at u={u} clear of parts and lid", not hit and vol(nut, shell) < 1e-6, ",".join(hit))

    # buttons, LED, IR
    for ref in ("SW1", "SW2", "SW3"):
        s = L.fp(ref)
        x, y = L.xy(s["u"], s["v"])
        probe = Pos(x, y, L.Z_PT + L.height(s)["above"] + L.STALK_GAP + 0.05) * Cylinder(0.5, 0.1)
        below = Pos(x, y, L.Z_PT + L.height(s)["above"] + L.STALK_GAP - 0.1) * Cylinder(0.5, 0.1)
        check(f"{ref} stalk tip {L.STALK_GAP} mm over its actuator", vol(probe, lid) > 0 and vol(below, lid) == 0)
    d = L.fp("D2")
    x, y = L.xy(d["u"], d["v"])
    y = max(y, -L.GAP + L.LED_PIPE_OD / 2 + 0.1)
    bore = Pos(x, y, L.Z_PT + 1.0) * Cylinder(L.LED_PIPE_ID / 2 - 0.1, L.HEADROOM + L.LID, align=(Align.CENTER, Align.CENTER, Align.MIN))
    led_off = abs(y - L.xy(d["u"], d["v"])[1])
    check("D2 light pipe open over the LED", vol(bore, lid) < 1e-6 and led_off < 0.8, f"pipe axis {led_off:.2f} mm from the LED")
    ir = L.fp("U9")
    ray = Pos(ir["u"], -10, L.Z_PT + L.IR_LENS_Z) * Rot(90, 0, 0) * Cylinder(2.5, 20)
    check("U9 lens sees out of the front window (5 mm beam)", vol(ray, shell) < 1e-6)

    # pillars on the holes, and the bed
    for h in L.BOARD["holes"]:
        x, y = L.xy(h["u"], h["v"])
        probe = Pos(x, y, L.Z_PT + L.PILLAR_SHORT + 0.05) * Cylinder(L.PILLAR_OD / 2 - 0.2, 0.1)
        check(f"lid pillar lands on {h['ref']}", vol(probe, lid) > 0)
    for name, p in (("base", base), ("lid", lid)):
        bb = p.bounding_box()
        check(f"{name} fits a {BED:.0f} mm bed", bb.size.X <= BED and bb.size.Y <= BED,
              f"{bb.size.X:.1f} x {bb.size.Y:.1f} x {bb.size.Z:.1f} mm")

    grams = (base.volume + lid.volume) / 1000 * 1.27
    lines = ["# Lite enclosure fit check", "",
             f"Board: `{L.BOARD['source']}` ({L.W:.0f} x {L.H:.0f} mm), enclosure "
             f"{L.X1 - L.X0:.0f} x {L.Y1 - L.Y0:.0f} x {L.Z_LID + L.LID:.1f} mm, "
             f"about {grams:.0f} g of PETG if printed solid (about ${grams * 0.02:.2f} at $20/kg).", "",
             "| Check | Result | Detail |", "|---|---|---|"]
    lines += [f"| {n} | {'pass' if ok else '**FAIL**'} | {d} |" for n, ok, d in results]
    (L.OUT).mkdir(parents=True, exist_ok=True)
    (L.OUT / "fit.md").write_text("\n".join(lines) + "\n")
    failed = [r for r in results if not r[1]]
    print(f"{len(results) - len(failed)}/{len(results)} pass")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
