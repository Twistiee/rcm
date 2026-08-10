#!/usr/bin/env freecadcmd
# -*- coding: utf-8 -*-
"""
Review images of the enclosure, into images/.

    "C:/Program Files/FreeCAD 1.1/bin/freecadcmd.exe" render_views.py

  images/assembly.png   shaded three-quarter views of box, keypad lid and
                        the two together
  images/sections.png   cut through the box at the switch columns and
                        through a DEUTSCH port, with the PCB envelope,
                        switch bodies and seal drawn on top

No GUI needed - everything is tessellated and drawn with matplotlib.
"""

import math
import os

import FreeCAD as App
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
IMG = os.path.join(HERE, "images")

# read the same parameters the models were built from
_path = os.path.join(HERE, "build_enclosure.py")
_g = {"__name__": "be", "__file__": _path}
exec(compile(open(_path).read().replace("\nmain()\n", "\n"), _path, "exec"), _g)
P, derive = _g["PARAMS"], _g["derive"]
D = derive(P)


def load(name):
    doc = App.openDocument(os.path.join(HERE, name + ".FCStd"))
    shape = doc.Objects[0].Shape
    return shape


def _basis(elev_deg, azim_deg):
    e, a = math.radians(elev_deg), math.radians(azim_deg)
    eye = (math.cos(e) * math.sin(a), -math.cos(e) * math.cos(a), math.sin(e))
    right = (math.cos(a), math.sin(a), 0.0)
    up = (-math.sin(e) * math.sin(a), math.sin(e) * math.cos(a), math.cos(e))
    return right, up, eye


def _paint(ax, shapes, elev, azim, tint=(0.20, 0.36, 0.52)):
    """Painter's-algorithm render with Lambert shading."""
    right, up, eye = _basis(elev, azim)
    light = (0.35, -0.55, 0.75)
    ln = math.sqrt(sum(c * c for c in light))
    light = tuple(c / ln for c in light)

    polys = []
    for shape, col in shapes:
        verts, faces = shape.tessellate(0.4)
        pv = [(v.x, v.y, v.z) for v in verts]
        for f in faces:
            a, b, c = pv[f[0]], pv[f[1]], pv[f[2]]
            u = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
            w = (c[0] - a[0], c[1] - a[1], c[2] - a[2])
            n = (u[1] * w[2] - u[2] * w[1],
                 u[2] * w[0] - u[0] * w[2],
                 u[0] * w[1] - u[1] * w[0])
            nl = math.sqrt(sum(t * t for t in n))
            if nl < 1e-12:
                continue
            n = tuple(t / nl for t in n)
            if sum(n[i] * eye[i] for i in range(3)) <= 0:
                continue                                     # back-face cull
            lam = max(0.0, sum(n[i] * light[i] for i in range(3)))
            sh = 0.28 + 0.72 * lam
            depth = sum((a[i] + b[i] + c[i]) / 3.0 * eye[i] for i in range(3))
            pts = [(sum(p[i] * right[i] for i in range(3)),
                    sum(p[i] * up[i] for i in range(3))) for p in (a, b, c)]
            polys.append((depth, pts, tuple(col[i] * sh + 0.11 for i in range(3))))
    polys.sort(key=lambda t: t[0])
    for _, pts, col in polys:
        ax.fill([p[0] for p in pts], [p[1] for p in pts], color=col, lw=0.0)
    ax.set_aspect("equal")
    ax.set_axis_off()
    ax.autoscale_view()


def assembly(box, keypad):
    grey = (0.26, 0.30, 0.34)
    blue = (0.20, 0.36, 0.52)
    views = [
        ("box, from above",                [(box, grey)],                 34, 28),
        ("keypad lid, front",              [(keypad, blue)],              22, 18),
        ("assembled",                      [(box, grey), (keypad, blue)], 26, 34),
        ("end wall - DEUTSCH port",        [(box, grey)],                 10, 92),
    ]
    fig, axes = plt.subplots(2, 2, figsize=(16, 12))
    for ax, (title, shapes, elev, azim) in zip(axes.flat, views):
        _paint(ax, shapes, elev, azim)
        ax.set_title(title)
    fig.tight_layout()
    out = os.path.join(IMG, "assembly.png")
    fig.savefig(out, dpi=100, facecolor="white")
    print("wrote", out)


def _slice(ax, shape, normal, offset, colour="#25415f"):
    for w in shape.slice(App.Vector(*normal), offset):
        for e in w.Edges:
            pts = e.discretize(120)
            if normal[0]:
                ax.plot([p.y for p in pts], [p.z for p in pts], "-", lw=1.4,
                        color=colour)
            else:
                ax.plot([p.x for p in pts], [p.z for p in pts], "-", lw=1.4,
                        color=colour)


def _overlay_pcb(ax, half, label=True):
    """The PCB and its tallest component, as the box sees them."""
    z = D["pcb_top_z"]
    ax.plot([-half, half], [z, z], color="#c0392b", lw=2.2,
            label="PCB top" if label else None)
    ax.plot([-half, half], [z - P["pcb_t"], z - P["pcb_t"]], color="#c0392b",
            lw=0.9, ls="--")
    ax.axhline(z + P["pcb_tall_comp_h"], color="#e67e22", lw=1.0, ls=":",
               label="tallest component (%.0f mm)" % P["pcb_tall_comp_h"]
               if label else None)


def bezel_detail(keypad):
    """The switch hole profile, drawn the way it prints: face down.

    Anything steeper than 45 degrees off vertical here needs support, which
    is the whole reason the square shoulder became a cone."""
    sx, sy = D["sw_xy"][0]
    fig, ax = plt.subplots(figsize=(7, 6))
    for w in keypad.slice(App.Vector(0, 1, 0), sy):
        for e in w.Edges:
            pts = e.discretize(200)
            # flip into print orientation: front face on the bed at z = 0
            ax.plot([p.x - sx for p in pts], [D["lid_z1"] - p.z for p in pts],
                    "-", lw=2.0, color="#25415f")
    lim = P["sw_bezel_d"] / 2.0 + 6
    ax.plot([-lim, lim], [0, 0], color="#7f8c8d", lw=3, alpha=0.5)
    ax.text(0, -0.9, "print bed - lid goes face down", ha="center", fontsize=9,
            color="#7f8c8d")
    ax.annotate("", xy=(P["sw_hole_d"] / 2.0, P["sw_bezel_depth"] + D["bezel_trans_h"]),
                xytext=(P["sw_bezel_d"] / 2.0, P["sw_bezel_depth"]),
                arrowprops=dict(arrowstyle="-", color="#c0392b", lw=2.5))
    ax.text(P["sw_bezel_d"] / 2.0 + 0.6, P["sw_bezel_depth"] + 0.6,
            "%s, %.0f deg" % (P["sw_bezel_transition"],
                              math.degrees(math.atan2(D["bezel_trans_h"],
                                                      (P["sw_bezel_d"] - P["sw_hole_d"]) / 2.0))),
            fontsize=9, color="#c0392b")
    ax.set_xlim(-lim, lim)
    ax.set_ylim(-2, P["lid_t"] + 2)
    ax.set_aspect("equal")
    ax.grid(alpha=0.25)
    ax.set_xlabel("mm from the switch centre")
    ax.set_ylabel("mm above the bed")
    ax.set_title("switch hole in print orientation")
    fig.tight_layout()
    out = os.path.join(IMG, "bezel_detail.png")
    fig.savefig(out, dpi=110, facecolor="white")
    print("wrote", out)


def sections(box, keypad):
    fig, axes = plt.subplots(2, 1, figsize=(15, 11))

    # --- through a row of switches, looking along Y
    ax = axes[0]
    y = P["sw_pitch_y"] / 2.0
    _slice(ax, box, (0, 1, 0), y)
    _slice(ax, keypad, (0, 1, 0), y, colour="#1f6f4a")
    _overlay_pcb(ax, D["out_w"] / 2.0)
    for sx, sy in D["sw_xy"]:
        if abs(sy - y) > 0.1:
            continue
        top = D["lid_z1"]
        bot = top - P["sw_body_len"]
        ax.add_patch(plt.Rectangle((sx - P["sw_hole_d"] / 2.0, bot),
                                   P["sw_hole_d"], P["sw_body_len"],
                                   fc="#95a5a6", ec="#5d6d7e", alpha=0.55))
    ax.set_title("section at y = %.1f - switch bodies over the board" % y)
    ax.legend(loc="lower right", fontsize=8)

    # --- through the DEUTSCH ports, looking along X
    ax = axes[1]
    _slice(ax, box, (1, 0, 0), 0.0)
    _slice(ax, keypad, (1, 0, 0), 0.0, colour="#1f6f4a")
    _overlay_pcb(ax, D["out_d"] / 2.0, label=False)
    for label, wall, u, z in P["dt_ports"]:
        if wall not in ("+X", "-X"):
            continue
        ax.add_patch(plt.Rectangle((u - P["dt_flange_w"] / 2.0,
                                    z - P["dt_flange_h"] / 2.0),
                                   P["dt_flange_w"], P["dt_flange_h"],
                                   fc="none", ec="#8e44ad", ls="--", lw=1.2))
        ax.plot([u], [z], "x", color="#8e44ad")
        ax.text(u, z + P["dt_flange_h"] / 2.0 + 2, label, ha="center",
                fontsize=8, color="#8e44ad")
    ax.set_title("section at x = 0 - DEUTSCH flange envelope (dashed) "
                 "projected onto the end walls")

    for ax in axes:
        ax.set_aspect("equal")
        ax.grid(alpha=0.25)
        ax.set_xlabel("mm")
        ax.set_ylabel("z (mm)")
    fig.tight_layout()
    out = os.path.join(IMG, "sections.png")
    fig.savefig(out, dpi=100, facecolor="white")
    print("wrote", out)


def main():
    if not os.path.isdir(IMG):
        os.makedirs(IMG)
    box = load("Enclosure-Box")
    keypad = load("Lid-Keypad")
    assembly(box, keypad)
    sections(box, keypad)
    bezel_detail(keypad)


main()
