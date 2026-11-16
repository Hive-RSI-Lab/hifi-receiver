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

from core_box import GAP, LID, T_PCB, banana_ports, base, block, cylinder, light_pipe, lid, rca_ports, tray  # noqa: E402

BOARD = "lite-main.board.json"   # mech board ../boards/lite-main/kicad/lite-main.kicad_pcb --heights heights.json
PRINT = {"lid": "flip"}
FIT = {"max_recess": {"*": 3.01,   # 3.0 plus the bbox slack the lid's text curves add
        "U9": 4.0},   # U9 is seen through its window, not plugged: its lens 3.5 mm in
       "bed": (180, 180, 180)}                # A1 mini, the smallest printer here

GAPS = {k: GAP["W"] for k in "WSEN"}      # 0.6 all round: nothing overhangs Lite's edges
HEADROOM = 29.0     # board top to lid underside: the RCA jacks' 27 mm and 2 mm over them
IR_WINDOW = (9.0, 8.0)                    # w x h round the IRM-3638T's lens
IR_LENS_Z = T_PCB + 5.0
ANT_LEN = 6.4       # the WROVER's antenna end, from its outer edge
ANT_MARGIN = 2.0    # along the wall, past the module each way
ANT_WALL = 1.2      # the west wall beside the antenna: half the usual 2.4
# The buttons are spring tongues cut in the lid, one piece with it: nothing loose to fall into the box, and
# each plunger hangs fixed over its switch. 1.0 mm PLA arm, 7 wide, ~15 long: ~1.8 N/mm, so the 0.65 mm
# press (0.4 gap + the switch's 0.25 travel) takes ~1.2 N on top of the switch's own, at ~0.5 % strain.
KEY_W = 7.0         # tongue width
KEY_SLOT = 1.0      # the U-slot round it, through the lid
KEY_FRONT = 0.5     # the tongue's front edge (y), just inside the front wall's inner face
KEY_ROOT = 22.0     # where the tongue joins the lid (y)
KEY_PAD = 7.5       # full-thickness pad from the front edge back to here (y); the arm behind it is thinned
KEY_ARM = 1.0       # the arm's thickness, kept at the lid's top face
KEY_GAP = 0.4       # plunger tip above the switch top at rest
# Labels, debossed so nothing overhangs: on the lid's top face (it prints face down, so they're the first layers).
# Bold sans, DejaVu as shipped with the OS.
FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
FONT_SMALL = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"   # port names: thinner strokes keep the islands in B, O, A, R printable
DEBOSS = 0.5
KEY_LABELS = {"SW1": "\u2212", "SW2": "+", "SW3": "\u25b6"}   # vol-, vol+, play/source
KEY_LABEL_Y = 26.0  # label centre, behind the tongue roots


def antenna_zone(board):
    """(x0, y0, x1, y1) of the antenna's keep-out, from the module's bbox: inner wall face to the antenna's inner end."""
    x0, y0, _, y1 = board.part("U5").bbox
    return (-GAPS["W"], y0 - ANT_MARGIN, x0 + ANT_LEN, y1 + ANT_MARGIN)


def keys(t, l, refs):
    """Cut a spring tongue for each tact switch in `refs` into the lid `l`, with a plunger hanging from its pad
    onto the switch. Prints with the lid, top face down: the arm and plunger grow straight off the bed."""
    z_top = t.z_top
    for ref in refs:
        p = t.board.part(ref)
        cx, cy = (p.bbox[0] + p.bbox[2]) / 2, (p.bbox[1] + p.bbox[3]) / 2
        x0, x1 = cx - KEY_W / 2, cx + KEY_W / 2
        s = KEY_SLOT
        # the lip hangs where the tongue's front edge moves: take it away there
        l = t.clear_lip(l, x0 - s - 0.5, t.inner[1] - 1, x1 + s + 0.5, KEY_FRONT + 3)
        cut = (block(x0 - s, KEY_FRONT - s, z_top - 5, x0, KEY_ROOT, z_top + LID + 1)
               + block(x1, KEY_FRONT - s, z_top - 5, x1 + s, KEY_ROOT, z_top + LID + 1)
               + block(x0 - s, KEY_FRONT - s, z_top - 5, x1 + s, KEY_FRONT, z_top + LID + 1))
        l -= cut
        l -= block(x0 - 0.01, KEY_PAD, z_top - 1, x1 + 0.01, KEY_ROOT, z_top + LID - KEY_ARM)
        tip = p.z[1] + KEY_GAP
        l += cylinder(cx, cy, tip + 3.0, z_top + 0.5, 4.0)
        l += cylinder(cx, cy, tip, tip + 3.0 + 0.01, 3.0)
    return l


def deboss(part, txt, size, origin, x_dir, normal, font=None):
    """Cut `txt` DEBOSS deep into `part`'s face at `origin`, reading along x_dir, the face's outward normal `normal`."""
    from build123d import Align, Plane, Text, extrude
    pl = Plane(origin=origin, x_dir=x_dir, z_dir=normal)
    t = pl * Text(txt, font_size=size, font_path=font or FONT, align=(Align.CENTER, Align.CENTER))
    return part - extrude(t, amount=DEBOSS + 0.2, dir=tuple(-n for n in normal)).translate(
        tuple(n * 0.2 for n in normal))


def labels(t, board, lid_part):
    """Debossed on the lid's top face, so they print as the first layers of the face-down lid and nothing
    overhangs: each button's symbol behind its tongue, and along the rear edge each rear port's name over it,
    read from the front. (Cut into the rear wall instead, every letter's roof is an overhang the base's
    supports would fill.) The line-in pair goes unmarked: every name tried over it (IN, LINE, AUX) left
    hairline slivers the slicer can't print, so the one unmarked pair is the input by elimination."""
    zt = t.z_top + LID
    for ref, txt in KEY_LABELS.items():
        p = board.part(ref)
        lid_part = deboss(lid_part, txt, 5.0, ((p.bbox[0] + p.bbox[2]) / 2, KEY_LABEL_Y, zt), (1, 0, 0), (0, 0, 1))
    ports = {q.ref: q.opening() for q in board.ports}
    row = t.inner[3] - 5.0   # clear of the lid's rear edge
    rear = [("SUB", ports["J3l"]), ("5V", ports["J1"]),
            ("L+", ports["J4"]), ("L\u2212", ports["J5"]), ("R+", ports["J6"]), ("R\u2212", ports["J7"])]
    for txt, (x0, x1, _, _) in rear:
        lid_part = deboss(lid_part, txt, 4.5, ((x0 + x1) / 2, row, zt), (1, 0, 0), (0, 0, 1), FONT_SMALL)
    return lid_part


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
    parts["lid"] = labels(t, board, keys(t, l, ("SW1", "SW2", "SW3")))
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

