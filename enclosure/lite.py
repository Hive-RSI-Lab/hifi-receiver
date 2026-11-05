"""Lite: lite-main in a two-part PETG box, built and fit-checked by hive-mech's `mech build`.

    mech build enclosure/lite.py -o enclosure/out/lite

It shares core_box.py's pieces with Plus and Ultra: the screws from below
through the base and the board into lid pillars, round RCA holes with a port
per barrel, the light pipe; its speaker outputs are the board's own banana sockets. Everything is in
mech's board frame: mm, x right along the front edge, y back (the rear panel
is the N edge, y = 90), z up, z = 0 the board's bottom face.

The board is changed before mech's fit check sees it: the RCA jacks are cut
to their body inside the rear wall, the four banana sockets (J4-J7) likewise,
their square noses each a port through a square hole in the rear wall, and
the IR receiver's lens is a port through the front wall.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mech.kicad import Port  # noqa: E402

from core_box import GAP, T_PCB, banana_ports, base, block, button, light_pipe, lid, rca_ports, tray  # noqa: E402

BOARD = "lite-main.board.json"   # mech board ../boards/lite-main/kicad/lite-main.kicad_pcb --heights heights.json
PRINT = {"lid": "flip", "btn_SW1": "flip", "btn_SW2": "flip", "btn_SW3": "flip"}
FIT = {"max_recess": {"*": 3.0, "U9": 4.0},   # U9 is seen through its window, not plugged: its lens 3.5 mm in
       "bed": (180, 180, 180)}                # A1 mini, the smallest printer here

GAPS = {k: GAP["W"] for k in "WSEN"}      # 0.6 all round: nothing overhangs Lite's edges
HEADROOM = 29.0     # board top to lid underside: the RCA jacks' 27 mm and 2 mm over them
IR_WINDOW = (9.0, 8.0)                    # w x h round the IRM-3638T's lens
IR_LENS_Z = T_PCB + 5.0
ANT_LEN = 6.4       # the WROVER's antenna end, from its outer edge
ANT_MARGIN = 2.0    # along the wall, past the module each way
ANT_WALL = 1.2      # the west wall beside the antenna: half the usual 2.4


def antenna_zone(board):
    """(x0, y0, x1, y1) of the antenna's keep-out, from the module's bbox: inner wall face to the antenna's inner end."""
    x0, y0, _, y1 = board.part("U5").bbox
    return (-GAPS["W"], y0 - ANT_MARGIN, x0 + ANT_LEN, y1 + ANT_MARGIN)


def build(board):
    z_top = T_PCB + HEADROOM
    board.ports = [p for p in board.ports if p.ref in ("J1", "J4", "J5", "J6", "J7")]   # USB-C, sockets
    feats = {}
    rca_ports(board, ("J2", "J3"), feats, GAPS["N"])
    banana_ports(board, ("J4", "J5", "J6", "J7"), feats, GAPS["N"])
    # IR: the receiver looks forward out of a window in the front wall
    u = board.part("U9")
    board.ports.append(Port("U9", "S", [u.x - IR_WINDOW[0] / 2, u.x + IR_WINDOW[0] / 2,
                                        IR_LENS_Z - IR_WINDOW[1] / 2, IR_LENS_Z + IR_WINDOW[1] / 2],
                            u.bbox[1], 0.0, "top"))
    t = tray(board, "lite", GAPS, z_top)
    b = base(t, feats, ports=("J1", "U9"))
    # vents over and under the amplifier (U8) and its inductors. No label recess underneath: the base
    # prints on its floor, so a recess's roof is a 22 mm bridge off the bed; a sticker goes on flat.
    a = board.part("U8")
    vent = (a.x - 15, 39.0, a.x + 15, 65.0)
    b = t.vents(b, vent, along="y", where="floor")
    l = lid(t, feats)
    l = t.vents(l, vent, along="y")
    # status LED: a light pipe, pulled back from the front wall where the LED sits at the edge
    d2 = board.part("D2")
    od = 4.6
    dy = max(0.0, -GAPS["S"] + od / 2 + 0.1 - (d2.bbox[1] + d2.bbox[3]) / 2)
    l = light_pipe(t, l, "D2", od=od, idd=2.6, dy=dy)
    parts = {"base": b}
    for sw in ("SW1", "SW2", "SW3"):
        l, btn = button(t, l, sw)
        parts[f"btn_{sw}"] = btn
    parts["lid"] = l
    # The ESP32's antenna sits at the west edge. Thin the wall beside it (open at the top, so nothing overhangs),
    # take the lid's lip away over it, and fail the build if any plastic of the box reaches into its zone between
    # floor and lid.
    zx0, zy0, zx1, zy1 = antenna_zone(board)
    parts["lid"] = t.clear_lip(parts["lid"], zx0 - 1, zy0, zx1, zy1)
    parts["base"] = parts["base"] - block(zx0 - (t.wall - ANT_WALL), zy0, t.z_floor, zx0 + 0.01, zy1, z_top + 1)
    zone = block(zx0, zy0, t.z_floor + 0.01, zx1, zy1, z_top - 0.01)
    for name, part in parts.items():
        hit = part & zone
        if hit is not None and hit.volume > 0.001:
            bb = hit.bounding_box(); raise ValueError(f"{name} reaches {hit.volume:.2f} mm3 into the antenna zone, x {bb.min.X:.1f}..{bb.max.X:.1f} y {bb.min.Y:.1f}..{bb.max.Y:.1f} z {bb.min.Z:.1f}..{bb.max.Z:.1f}")
    return parts

