"""PNG views of the Lite enclosure, with the board's parts as boxes.

    ~/venv-cad/bin/python enclosure/render.py   writes enclosure/out/lite/render-*.png

Flat-shaded matplotlib views, enough to see that the cut-outs are where the
ports are; the STEP files are the model.
"""
from __future__ import annotations

import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
from mpl_toolkits.mplot3d.art3d import Poly3DCollection  # noqa: E402
from build123d import Pos  # noqa: E402

sys.path.insert(0, str(Path(__file__).resolve().parent))
import lite as L  # noqa: E402

LIGHT = np.array([0.4, -0.5, 0.75])
LIGHT /= np.linalg.norm(LIGHT)


def tris(shape, tol=0.2):
    verts, faces = shape.tessellate(tol, 0.5)
    v = np.array([[p.X, p.Y, p.Z] for p in verts])
    return v[np.array(faces)]


def shaded(shape, rgb):
    t = tris(shape)
    n = np.cross(t[:, 1] - t[:, 0], t[:, 2] - t[:, 0])
    n /= np.linalg.norm(n, axis=1)[:, None] + 1e-12
    shade = 0.35 + 0.65 * np.abs(n @ LIGHT)
    return t, np.clip(np.array(rgb)[None, :] * shade[:, None], 0, 1)


def view(name, items, elev, azim):
    fig = plt.figure(figsize=(9, 7), dpi=110)
    ax = fig.add_subplot(projection="3d")
    # one collection, so matplotlib depth-sorts every triangle against every other
    parts = [shaded(shape, rgb) for shape, rgb, _ in items]
    ax.add_collection3d(Poly3DCollection(np.concatenate([t for t, _ in parts]),
                                         facecolors=np.concatenate([c for _, c in parts]), edgecolor="none"))
    ax.set_xlim(-10, 110); ax.set_ylim(-15, 105); ax.set_zlim(-5, 115)
    ax.set_box_aspect((1, 1, 1))
    ax.view_init(elev=elev, azim=azim)
    ax.set_axis_off()
    fig.tight_layout()
    fig.savefig(L.OUT / f"render-{name}.png")
    plt.close(fig)


def main():
    L.OUT.mkdir(parents=True, exist_ok=True)
    base, lid = L.base(), L.lid()
    board = [(L.board_solid(), (0.1, 0.45, 0.2), 1.0)] + \
            [(box, (0.75, 0.75, 0.8), 1.0) for _, box in L.part_boxes()]
    up = Pos(0, 0, 45)
    exploded = [(base, (0.25, 0.3, 0.38), 1.0)] + board + [(up * lid, (0.55, 0.6, 0.7), 1.0)]
    view("front", exploded, 22, -60)
    view("rear", exploded, 22, 120)
    view("closed-rear", [(base, (0.25, 0.3, 0.38), 1.0), (lid, (0.55, 0.6, 0.7), 1.0)], 15, 105)
    view("closed-front", [(base, (0.25, 0.3, 0.38), 1.0), (lid, (0.55, 0.6, 0.7), 1.0)], 30, -70)


if __name__ == "__main__":
    main()
