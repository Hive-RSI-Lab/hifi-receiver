#!/usr/bin/env python3
"""plot.py - crop render of copper around a point (review aid, review-3 LAYOUT).

plot.py BOARD.kicad_pcb X Y HALF_MM OUT.png [LAYER ...]
Draws filled zones (GND dark, other nets coloured), tracks, vias, pads, and
labels refs. Requires PIL (hwde python). Layers default F.Cu B.Cu, drawn
side by side.
"""
import sys, hashlib
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from pcbgeo import load_board
from PIL import Image, ImageDraw


def col(net, a=255):
    if net in (None, ""):
        return (120, 120, 120)
    if net == "GND":
        return (40, 90, 40)
    h = hashlib.md5(net.encode()).digest()
    return (80 + h[0] % 175, 80 + h[1] % 175, 80 + h[2] % 175)


def main():
    pcb, x0, y0, half, out = sys.argv[1], float(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4]), sys.argv[5]
    layers = sys.argv[6:] or ["F.Cu", "B.Cu"]
    b = load_board(pcb)
    S = int(__import__("os").environ.get("PXMM", "20"))  # px per mm
    W = int(2 * half * S)
    img = Image.new("RGB", (W * len(layers) + 10 * (len(layers) - 1), W), (255, 255, 255))
    for li, lay in enumerate(layers):
        sub = Image.new("RGB", (W, W), (255, 255, 255))
        ox = 0
        d = ImageDraw.Draw(sub)
        P = lambda x, y: (ox + (x - x0 + half) * S, (y - y0 + half) * S)
        for z in b["zones"]:
            for l, pts in z["filled"]:
                if l == lay and len(pts) > 2:
                    c = col(z["net"])
                    d.polygon([P(*p) for p in pts], fill=tuple(int(v * 0.5 + 127) for v in c))
        for t in b["tracks"]:
            if t["layer"] == lay:
                d.line([P(*t["s"]), P(*t["e"])], fill=col(t["net"]), width=max(1, int(t["w"] * S)))
        for r, f in b["footprints"].items():
            for p in f["pads"]:
                if lay in p["layers"] or "*.Cu" in p["layers"]:
                    w, h = p["w"], p["h"]
                    import math
                    rr = abs(math.sin(math.radians(f["rot"])))
                    if rr > 0.7:
                        w, h = h, w
                    d.rectangle([P(p["x"] - w / 2, p["y"] - h / 2), P(p["x"] + w / 2, p["y"] + h / 2)], outline=(0, 0, 0), fill=col(p["net"]))
            if abs(f["x"] - x0) < half and abs(f["y"] - y0) < half:
                d.text(P(f["x"], f["y"]), r, fill=(200, 0, 0))
        for v in b["vias"]:
            x, y = v["xy"]
            rr = v["size"] / 2
            d.ellipse([P(x - rr, y - rr), P(x + rr, y + rr)], fill=(230, 180, 0), outline=(0, 0, 0))
        d.text((ox + 4, 4), f"{lay}  {2*half:.0f} mm window @({x0},{y0})", fill=(0, 0, 0))
        img.paste(sub, (li * (W + 10), 0))
    img.save(out)


if __name__ == "__main__":
    main()
