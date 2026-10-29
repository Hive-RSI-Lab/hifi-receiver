"""The Lite enclosure: a printed base and lid around lite-main.

    ~/venv-cad/bin/python enclosure/lite.py     writes enclosure/out/lite/*.step, *.stl

Everything is placed from enclosure/lite-main.board.json (kicad_board.py's
read of the board), so a moved hole or connector moves its cut-out. The frame:
X = the board's u (left to right seen from the front), Y = 90 - v (front edge
at Y = 0, rear edge at Y = 90), Z up from the underside of the base.

How it goes together (README.md): the board drops onto four standoffs, the
four binding posts go through the rear wall and are wired to J4, the lid goes
on, and four M3 x 12 screws from underneath pass through the base and the
board into the lid's pillars. The screws clamp the board between standoff and
pillar and pull the lid onto the walls; nothing else holds it.

Both parts print without supports in PETG or ASA: the base on its floor, the
lid on its top face. Holes in the walls are teardrops (pointed up) or flat
topped rectangles short enough to bridge.
"""
from __future__ import annotations

import json
import math
from pathlib import Path

from build123d import (Align, Axis, Box, Cylinder, Location, Locations, Plane, Polygon, Pos, Rot,
                       RectangleRounded, Solid, extrude, export_step, export_stl, fillet)

HERE = Path(__file__).resolve().parent
BOARD = json.loads((HERE / "lite-main.board.json").read_text())
HEIGHTS = json.loads((HERE / "heights.json").read_text())

# ---- parameters (mm) ------------------------------------------------------
W, H = BOARD["outline"]["w"], BOARD["outline"]["h"]
T_PCB = BOARD["thickness"]
GAP = 0.6           # board edge to inner wall face
WALL = 2.4          # 6 perimeters at 0.4
FLOOR = 2.0
STANDOFF = 5.0      # floor top to board bottom: clears 2.9 mm of RCA pins
LID = 2.2
R_OUT = 4.0         # outer corner radius, plan view
Z_PB = FLOOR + STANDOFF          # board bottom
Z_PT = Z_PB + T_PCB              # board top
HEADROOM = 36.0     # board top to lid underside: RCA 27 mm + the binding posts above them
Z_LID = Z_PT + HEADROOM          # lid underside = top of the walls
X0, X1 = -GAP - WALL, W + GAP + WALL
Y0, Y1 = -GAP - WALL, H + GAP + WALL

SCREW_CLEAR = 3.4   # M3 through standoff and floor
SCREW_HEAD = 6.0    # counterbore for an M3 socket or pan head
HEAD_DEPTH = 3.0
STANDOFF_OD = 7.0
PILLAR_OD = 6.0
PILLAR_PILOT = 2.5  # M3 thread-forming into PETG
PILLAR_SHORT = 0.2  # pillar ends this far above the board, so the screw pulls the lid onto the walls

RCA_HOLE = 12.0     # passes a plug's outer shell onto the jack's 8.3 mm barrel
RCA_BARRELS = (6.5, 20.5)        # barrel axes above the board top (STEP model, corrected)
USB_OPEN = (13.4, 7.4)           # passes common USB-C overmolds (<= 12.4 x 6.5)
USB_AXIS = 1.65                  # receptacle axis above the board top (USB4105, 3.3 mm tall)
POST_HOLE = 4.3     # M4-thread 4 mm binding posts, the common cheap kind
POST_Z = 31.5       # above the board top: their nuts and lugs pass over the RCA bodies
POST_U = (16.0, 35.0, 65.0, 84.0)  # u of each post; README gives which is which
IR_WINDOW = (9.0, 8.0)           # w x h, centred on the receiver's lens
IR_LENS_Z = 5.0
BUTTON_W = 8.0      # flexure tongue width
BUTTON_L = 14.0     # tongue length, tip to hinge
BUTTON_T = 1.2      # tongue thickness (the lid's top skin)
SLOT = 0.8
STALK_OD = 4.0
STALK_GAP = 0.3     # stalk tip above the switch's actuator
LED_PIPE_OD, LED_PIPE_ID, LED_GAP = 4.6, 2.6, 0.6


def fp(ref):
    return next(f for f in BOARD["footprints"] if f["ref"] == ref)


def xy(u, v):
    return u, H - v


def height(f):
    name = f["footprint"].split(":")[-1]
    return HEIGHTS.get(name, HEIGHTS["default"])


def teardrop(d, length, axis="y"):
    """A hole along Y (through a front or rear wall) with a 45-degree point on
    top, so it prints without support. Centred at the origin."""
    r = d / 2
    # the circle from 45 degrees round the bottom to 135, then the point on top
    pts = [(r * math.cos(math.radians(a)), r * math.sin(math.radians(a))) for a in range(45, -226, -5)]
    pts.append((0, r * math.sqrt(2)))
    prof = Polygon(*pts, align=None)
    return extrude(Plane.XZ * prof, amount=length / 2, both=True)


def wall_window(w, h, length, r=1.0):
    """A rounded rectangular hole along Y, centred at the origin."""
    return extrude(Plane.XZ * RectangleRounded(w, h, min(r, w / 2 - 0.01, h / 2 - 0.01)), amount=length / 2, both=True)


# ---- the base -------------------------------------------------------------

def base():
    outer = extrude(Pos((X0 + X1) / 2, (Y0 + Y1) / 2) * RectangleRounded(X1 - X0, Y1 - Y0, R_OUT), Z_LID)
    inner = extrude(Pos(W / 2, H / 2, FLOOR) * RectangleRounded(W + 2 * GAP, H + 2 * GAP, R_OUT - WALL + 0.5), Z_LID)
    b = outer - inner
    # standoffs, through holes and the screw-head counterbores from below
    for h in BOARD["holes"]:
        x, y = xy(h["u"], h["v"])
        b += Pos(x, y, FLOOR) * Cylinder(STANDOFF_OD / 2, STANDOFF, align=(Align.CENTER, Align.CENTER, Align.MIN))
        b -= Pos(x, y, 0) * Cylinder(SCREW_CLEAR / 2, Z_PB, align=(Align.CENTER, Align.CENTER, Align.MIN))
        b -= Pos(x, y, 0) * Cylinder(SCREW_HEAD / 2, HEAD_DEPTH, align=(Align.CENTER, Align.CENTER, Align.MIN))
    y_rear = H + GAP + WALL / 2
    y_front = -GAP - WALL / 2
    cut = WALL + 2
    # rear: the two RCA pairs
    for ref in ("J2", "J3"):
        x, _ = xy(fp(ref)["u"], 0)
        for zb in RCA_BARRELS:
            # round, not a teardrop: two 12 mm holes 14 mm apart leave no room
            # for a point, and PETG bridges the top of a 12 mm circle cleanly
            b -= Pos(x, y_rear, Z_PT + zb) * Rot(90, 0, 0) * Cylinder(RCA_HOLE / 2, cut)
    # rear: USB-C
    x, _ = xy(fp("J1")["u"], 0)
    b -= Pos(x, y_rear, Z_PT + USB_AXIS) * wall_window(*USB_OPEN, cut, r=2.0)
    # rear: binding posts
    for u in POST_U:
        b -= Pos(u, y_rear, Z_PT + POST_Z) * teardrop(POST_HOLE, cut)
    # front: the IR receiver's window
    ir = fp("U9")
    b -= Pos(ir["u"], y_front, Z_PT + IR_LENS_Z) * wall_window(*IR_WINDOW, cut, r=1.0)
    # floor vents under the amplifier (U8) and its inductors
    for x in vent_columns():
        b -= Pos(x, H - 38.0, 0) * Box(2.0, 26.0, FLOOR * 3)
    # underside: label recess and four rubber-foot recesses
    b -= Pos(W / 2 - 20, H / 2 + 2, 0) * Box(40.0, 22.0, 0.8)
    for fx, fy in ((12, 12), (W - 12, 12), (12, H - 12), (W - 12, H - 12)):
        b -= Pos(fx, fy, 0) * Cylinder(5.5, 0.8)
    return b


def vent_columns():
    u8 = fp("U8")
    return [u8["u"] - 14 + 4 * i for i in range(8)]


# ---- the lid --------------------------------------------------------------

def lid():
    plate = Pos(0, 0, Z_LID) * extrude(Pos((X0 + X1) / 2, (Y0 + Y1) / 2) * RectangleRounded(X1 - X0, Y1 - Y0, R_OUT), LID)
    l = plate
    # pillars down to the board's mounting holes
    plen = HEADROOM - PILLAR_SHORT
    for h in BOARD["holes"]:
        x, y = xy(h["u"], h["v"])
        l += Pos(x, y, Z_LID) * Cylinder(PILLAR_OD / 2, plen, align=(Align.CENTER, Align.CENTER, Align.MAX))
        l -= Pos(x, y, Z_LID - plen) * Cylinder(PILLAR_PILOT / 2, 13.0, align=(Align.CENTER, Align.CENTER, Align.MIN))
    # locating tabs just inside the walls, clear of the binding posts on the rear
    tab = 1.2
    inset = GAP - 0.25
    for (cx, cy, sx, sy) in ((-inset + tab / 2 - 0.0, H / 2, tab, 20), (W + inset - tab / 2, H / 2, tab, 20),
                             (W / 4, -inset + tab / 2, 20, tab), (3 * W / 4 - 10, -inset + tab / 2, 14, tab)):
        l += Pos(cx, cy, Z_LID) * Box(sx, sy, 3.0, align=(Align.CENTER, Align.CENTER, Align.MAX))
    # button flexures: a tongue of the lid's top skin, hinged towards the rear
    for ref in ("SW1", "SW2", "SW3"):
        s = fp(ref)
        x, y = xy(s["u"], s["v"])
        y_tip = max(y - BUTTON_W / 2 + 0.5, -GAP + 0.6)    # stay inside the front wall
        y_hinge = y_tip + BUTTON_L
        cx, cy = x, (y_tip + y_hinge) / 2
        ring = Pos(cx, cy, Z_LID - 1) * Box(BUTTON_W + 2 * SLOT, BUTTON_L + SLOT, LID + 2) \
            - Pos(cx, cy + SLOT / 2, Z_LID - 1) * Box(BUTTON_W, BUTTON_L + SLOT, LID + 2)
        l -= ring
        # thin the tongue from below to BUTTON_T
        l -= Pos(cx, cy + SLOT / 2, Z_LID) * Box(BUTTON_W, BUTTON_L + SLOT, LID - BUTTON_T, align=(Align.CENTER, Align.CENTER, Align.MIN))
        stalk_top = Z_LID + (LID - BUTTON_T)
        stalk_bot = Z_PT + height(s)["above"] + STALK_GAP
        l += Pos(x, y, stalk_bot) * Cylinder(STALK_OD / 2, stalk_top - stalk_bot + 0.01, align=(Align.CENTER, Align.CENTER, Align.MIN))
    # status LED light pipe
    d = fp("D2")
    x, y = xy(d["u"], d["v"])
    y = max(y, -GAP + LED_PIPE_OD / 2 + 0.1)
    bot = Z_PT + height(d)["above"] + LED_GAP
    l += Pos(x, y, bot) * Cylinder(LED_PIPE_OD / 2, Z_LID - bot + 0.01, align=(Align.CENTER, Align.CENTER, Align.MIN))
    l -= Pos(x, y, bot - 1) * Cylinder(LED_PIPE_ID / 2, Z_LID + LID - bot + 2, align=(Align.CENTER, Align.CENTER, Align.MIN))
    # vent slots over the amplifier
    for xv in vent_columns():
        l -= Pos(xv, H - 38.0, Z_LID - 1) * Box(2.0, 26.0, LID + 2, align=(Align.CENTER, Align.CENTER, Align.MIN))
    return l


# ---- the board, as keep-out boxes, for the fit check and the assembly view --

def board_solid():
    s = Pos(W / 2, H / 2, Z_PB) * extrude(RectangleRounded(W, H, max(BOARD["outline"]["corner_radius"], 0.1)), T_PCB)
    for h in BOARD["holes"]:
        x, y = xy(h["u"], h["v"])
        s -= Pos(x, y, Z_PB - 1) * Cylinder(h["drill"] / 2, T_PCB + 2, align=(Align.CENTER, Align.CENTER, Align.MIN))
    return s


def part_boxes():
    """(ref, solid) for every footprint: its courtyard, from the board top to
    its height, and its through-hole pads' box from the board bottom down to
    the pin ends."""
    out = []
    for f in BOARD["footprints"]:
        if not f["courtyard"] or f["ref"].startswith("H"):
            continue
        u0, v0, u1, v1 = f["courtyard"]
        hgt = height(f)
        x0, x1 = u0, u1
        y0, y1 = H - v1, H - v0
        solid = Pos((x0 + x1) / 2, (y0 + y1) / 2, Z_PT) * Box(x1 - x0, y1 - y0, hgt["above"], align=(Align.CENTER, Align.CENTER, Align.MIN))
        if hgt["below"] > 0 and f.get("tht_pads"):
            p0, q0, p1, q1 = f["tht_pads"]
            solid += Pos((p0 + p1) / 2, H - (q0 + q1) / 2, Z_PB) * Box(p1 - p0, q1 - q0, hgt["below"], align=(Align.CENTER, Align.CENTER, Align.MAX))
        out.append((f["ref"], solid))
    return out


OUT = HERE / "out" / "lite"


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    parts = {"lite-base": base(), "lite-lid": lid()}
    for name, p in parts.items():
        export_step(p, str(OUT / f"{name}.step"))
        # STLs in their print orientation: base on its floor, lid on its top face
        pr = p if name == "lite-base" else Rot(180, 0, 0) * p
        bb = pr.bounding_box()
        pr = Pos(-bb.min.X, -bb.min.Y, -bb.min.Z) * pr
        export_stl(pr, str(OUT / f"{name}.stl"), tolerance=0.02, angular_tolerance=0.2)
        print(name, "volume %.1f cm3" % (p.volume / 1000), "bbox %.1f x %.1f x %.1f" % (bb.size.X, bb.size.Y, bb.size.Z))
    return parts


if __name__ == "__main__":
    main()
