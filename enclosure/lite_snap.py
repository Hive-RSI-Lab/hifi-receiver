"""Lite, screwless: the same box as lite.py with no screws at all.

    mech build enclosure/lite_snap.py -o enclosure/out/lite-snap

The board drops onto a pin in each of its four mounting holes (on the base's standoffs) and the lid's
posts come down onto the board round those holes, so it can't move in any direction once the lid is on.
The lid clicks on: a hook hangs from it inside the W and E walls and its catch springs out into a window
in the wall. To take the lid off, push each catch in through its window (a pen tip, a fingernail) and
lift that side.

Everything else (ports, vents, the IR window, the light pipe, the spring-tongue buttons, the labels, the
antenna zone) is lite.py's, built by lite.py's own build().
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import lite  # noqa: E402
from core_box import (FOOT_D, FOOT_DEPTH, LID, STANDOFF_OD, T_PCB, CoreTray, cut_vents, cylinder,  # noqa: E402
                      feet, rounded, teardrop_y, RCA_HOLE, RCA_CAP, BANANA_HOLE)
from mech.box import block  # noqa: E402

BOARD = lite.BOARD
PRINT = lite.PRINT
FIT = lite.FIT

PIN_D = 2.85        # into the 3.2 mm mounting holes: 0.17 mm a side
PIN_H = 1.3         # above the standoff, under the board's top face (1.6)
POST_OD = 6.0       # the lid's posts onto the board round each hole
POST_GAP = 0.3      # post end over the board top: no preload, so nothing pushes the lid up
# Snap hooks: a 1.6 mm PLA cantilever 18 mm long whose catch moves 1.8 mm going on: ~1.3 % strain.
# v1 (1.4 x 14 mm, 1.0 mm catch, 0.1 mm window play) didn't engage: the catch reached only 0.75 mm past
# the wall's inner face (the lip gap takes 0.25) and any lift of the lid held it above the window.
HOOK_Y = 50.0       # both hooks' centre (y): W clear of the antenna (y < 34) and the RCA jacks (y > 70)
HOOK_W = 13.5       # along the wall
HOOK_T = 1.6        # thickness
HOOK_L = 18.0       # lid underside to the hook's tip
CATCH = 1.8         # how far the catch stands out from the hook, into the window
CATCH_FLAT = 1.2    # the catch's square face before its 45-degree lead-in
CATCH_HOLD = 1.0    # the flat (holding) part of the catch's top; outside it the top slopes 45 deg so the flipped lid
                    # prints it without a ledge over 1 mm
WIN_PLAY = 0.6      # window top over the catch: room for the lid sitting a little high
WIN_EDGE = 0.5      # window wider than the hook each side
WIN_BELOW = 4.2     # window bottom under the catch's top: room to get a tip onto the catch


def base(t: CoreTray, feats: dict, ports=("J1", "U9")):
    """core_box.base with each screw's standoff, counterbore and hole replaced by a pin in the mounting hole."""
    x0, y0, x1, y1 = t.outer
    ix0, iy0, ix1, iy1 = t.inner
    b = rounded(x0, y0, x1, y1, t.z_bottom, t.z_top, t.corner + t.wall)
    b -= rounded(ix0, iy0, ix1, iy1, t.z_floor, t.z_top + 1, t.corner)
    for h in t.board.holes:
        b += cylinder(h.x, h.y, t.z_floor - 0.01, 0.0, max(STANDOFF_OD, h.pad))
        b += cylinder(h.x, h.y, -0.01, PIN_H, PIN_D)
    for fx, fy in feet(t):
        b -= cylinder(fx, fy, t.z_bottom - 1, t.z_bottom + FOOT_DEPTH, FOOT_D)
    b = t.cut_ports(b, refs=list(ports))
    cut = t.wall + 2
    yn = y1 - t.wall / 2
    for _, x, z in feats.get("rca", []):
        b -= teardrop_y(x, yn, z, RCA_HOLE, cut, cap=RCA_CAP)
    for _, x, z in feats.get("banana", []):
        b -= teardrop_y(x, yn, z, BANANA_HOLE, cut)
    if "vents" in feats:
        b = cut_vents(b, feats["vents"]["wall"], rounded_ends=False)
        b = cut_vents(b, feats["vents"]["floor"])
    return b


def lid(t: CoreTray, feats: dict):
    """mech's lid with a post down onto the board round each mounting hole."""
    l = t.lid()
    for h in t.board.holes:
        l += cylinder(h.x, h.y, T_PCB + POST_GAP, t.z_top + 0.01, POST_OD)
    return l


def catch_z(t):
    return t.z_top - HOOK_L + CATCH_FLAT + CATCH + (CATCH - CATCH_HOLD)


def hook(t, side):
    """The hook inside the `side` ("W" or "E") wall: a tab off the lid's underside, its catch facing out.
    Square on top (it holds), 45 degrees underneath (it rides over the wall's top edge going on)."""
    from build123d import Plane, Polygon, extrude
    s = -1 if side == "W" else 1                       # outward along x
    xi = t.inner[0] if side == "W" else t.inner[2]     # the wall's inner face
    lg = t.lip[2]
    xo = xi - s * lg                                   # the hook's outer face
    xn = xo - s * HOOK_T                               # its inner face
    xc = xo + s * CATCH                                # the catch's outer face
    zc = catch_z(t)
    zt, zb = t.z_top + 0.01, t.z_top - HOOK_L
    xh, zs = xo + s * CATCH_HOLD, zc - (CATCH - CATCH_HOLD)
    pts = [(xn, zt), (xo, zt), (xo, zc), (xh, zc), (xc, zs), (xc, zs - CATCH_FLAT), (xo, zb), (xn, zb)]
    prof = Polygon(*[(x, z) for x, z in pts], align=None)
    solid = extrude(Plane.XZ * prof, amount=HOOK_W)    # Plane.XZ's normal is -y
    bb = solid.bounding_box()
    return solid.translate((0, HOOK_Y + HOOK_W / 2 - bb.max.Y, 0))


def window(t, side):
    xi = t.inner[0] if side == "W" else t.inner[2]
    w = t.wall + 1
    x0, x1 = (xi - w, xi + 0.1) if side == "W" else (xi - 0.1, xi + w)
    zc = catch_z(t)
    return block(x0, HOOK_Y - HOOK_W / 2 - WIN_EDGE, zc - WIN_BELOW,
                 x1, HOOK_Y + HOOK_W / 2 + WIN_EDGE, zc + WIN_PLAY)


def build(board):
    lite.base, lite.lid = base, lid
    t_box = {}
    tray = lite.tray

    def keep(*a, **k):
        t_box["t"] = tray(*a, **k)
        return t_box["t"]
    lite.tray = keep
    try:
        parts = lite.build(board)
    finally:
        lite.tray = tray
    t = t_box["t"]
    l = parts["lid"]
    for side in ("W", "E"):
        xi = t.inner[0] if side == "W" else t.inner[2]
        l = t.clear_lip(l, xi - 1 if side == "W" else xi - 3, HOOK_Y - HOOK_W / 2 - 1.5,
                        xi + 3 if side == "W" else xi + 1, HOOK_Y + HOOK_W / 2 + 1.5)
        l += hook(t, side)
        parts["base"] = parts["base"] - window(t, side)
    parts["lid"] = l
    return parts
