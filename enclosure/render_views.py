#!/usr/bin/env freecadcmd
# -*- coding: utf-8 -*-
"""
Review images of the enclosure, into images/.

    "C:/Program Files/FreeCAD 1.1/bin/freecadcmd.exe" render_views.py

  images/assembly.png   shaded three-quarter views of box, keypad lid and
                        the two together
  images/sections.png   cut through the box at the switch columns and
                        through a DEUTSCH port, with the PCB envelope,
                        switch bodies, the harness channel and the seal
                        drawn on top
  images/guard_detail.png
                        which button carries the start/stop guard, in car
                        terms, and a true section through both locating
                        pins

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
VARIANTS, derive = _g["VARIANTS"], _g["derive"]

# Which variant to draw.  Named on the command line, else the last one in
# VARIANTS - which is the current build, not the historical one.
import sys
_asked = [a for a in sys.argv[1:] if not a.endswith(".py")]
VARIANT = _asked[0] if _asked else list(VARIANTS)[-1]
if VARIANT not in VARIANTS:
    raise SystemExit("unknown variant %r - have %s"
                     % (VARIANT, ", ".join(VARIANTS)))
P = dict(_g["PARAMS"])
P.update(VARIANTS[VARIANT])
D = derive(P)
print("drawing variant %s" % VARIANT)


def load(name, variant=True):
    if variant:
        name = "%s-%s" % (name, VARIANT)
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


def assembly(box, keypad, ring):
    grey = (0.26, 0.30, 0.34)
    blue = (0.20, 0.36, 0.52)
    red = (0.62, 0.22, 0.20)
    lid = [(keypad, blue)] + ([(ring, red)] if ring else [])
    views = [
        ("box, from above",                [(box, grey)],                 34, 28),
        ("keypad lid, front (%s = front of car)" % P["car_front"], lid,   22, 18),
        ("assembled",                      [(box, grey)] + lid,           26, 34),
        ("end wall - DEUTSCH port",        [(box, grey)],                 10, 92),
    ]
    fig, axes = plt.subplots(2, 2, figsize=(16, 12))
    for ax, (title, shapes, elev, azim) in zip(axes.flat, views):
        _paint(ax, shapes, elev, azim)
        ax.set_title(title)
    fig.tight_layout()
    out = os.path.join(IMG, "assembly-%s.png" % VARIANT)
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


def _slice_dir(ax, shape, origin, angle_deg, colour="#25415f"):
    """Section on a vertical plane through `origin` at `angle_deg` in plan,
    plotted against distance along that plane.  The guard pins sit on a
    diagonal, so an axis-aligned cut would miss them."""
    a = math.radians(angle_deg)
    u = (math.cos(a), math.sin(a), 0.0)                 # in-plane horizontal
    n = (-math.sin(a), math.cos(a), 0.0)                # plane normal
    off = origin[0] * n[0] + origin[1] * n[1]
    for w in shape.slice(App.Vector(*n), off):
        for e in w.Edges:
            pts = e.discretize(120)
            ax.plot([(p.x - origin[0]) * u[0] + (p.y - origin[1]) * u[1]
                     for p in pts], [p.z for p in pts], "-", lw=1.4,
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
    out = os.path.join(IMG, "bezel_detail-%s.png" % VARIANT)
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
    for port in D["dt_ports"]:
        fp, wall, u, z = port["fp"], port["wall"], port["u"], port["z"]
        if wall not in ("+X", "-X"):
            continue
        ax.add_patch(plt.Rectangle((u - fp["flange_w"] / 2.0,
                                    z - fp["flange_h"] / 2.0),
                                   fp["flange_w"], fp["flange_h"],
                                   fc="none", ec="#8e44ad", ls="--", lw=1.2))
        ap = fp["aperture"]
        if ap[0] == "round":
            ax.add_patch(plt.Circle((u, z), ap[1] / 2.0, fc="none",
                                    ec="#8e44ad", lw=1.2))
        else:
            ax.add_patch(plt.Rectangle((u - ap[1] / 2.0, z - ap[2] / 2.0),
                                       ap[1], ap[2], fc="none", ec="#8e44ad",
                                       lw=1.2))
        for du in (-fp["hole_dx"] / 2.0, fp["hole_dx"] / 2.0):
            for dz in (-fp["hole_dy"] / 2.0, fp["hole_dy"] / 2.0):
                ax.plot([u + du], [z + dz], "o", ms=3.5, color="#8e44ad")
        ax.text(u, z + fp["flange_h"] / 2.0 + 2,
                "%s  %s%s" % (port["label"], fp["label"],
                              "" if fp["verified"] else "   UNVERIFIED"),
                ha="center", fontsize=8, color="#8e44ad")
    # the channel the harness climbs in, off both long edges
    y0, y1 = D["wire_band_y"], D["cav_d"] / 2.0
    z0 = D["board_bot_z"]
    for sgn in (1, -1):
        ax.add_patch(plt.Rectangle((sgn * y0 if sgn > 0 else -y1, z0),
                                   y1 - y0, P["wire_clear_h"],
                                   fc="#27ae60", ec="#1e8449", alpha=0.18,
                                   lw=1.0, ls="--"))
    ax.axhline(D["wire_ceiling_z"], color="#1e8449", lw=1.0, ls="-.")
    ax.text(0, D["wire_ceiling_z"] + 0.8,
            "harness ceiling z=%.1f  (%.1f of %.0f mm needed, %+.1f)"
            % (D["wire_ceiling_z"], D["wire_avail"], P["wire_clear_h"],
               D["wire_avail"] - P["wire_clear_h"]),
            ha="center", fontsize=8, color="#1e8449")
    ax.set_title("section at x = 0 - DEUTSCH flange envelope (dashed) and "
                 "the harness channel off the long edges (green)")

    for ax in axes:
        ax.set_aspect("equal")
        ax.grid(alpha=0.25)
        ax.set_xlabel("mm")
        ax.set_ylabel("z (mm)")
    fig.tight_layout()
    out = os.path.join(IMG, "sections-%s.png" % VARIANT)
    fig.savefig(out, dpi=100, facecolor="white")
    print("wrote", out)


def guard_detail(keypad, ring):
    """Which button carries the guard, said in car terms, and a true
    section through both locating pins."""
    gx, gy = D["guard_xy"]
    fig, axes = plt.subplots(1, 2, figsize=(15, 7))

    # --- plan view, oriented as it sits in the car
    ax = axes[0]
    w, d = D["out_w"] / 2.0, D["out_d"] / 2.0
    ax.add_patch(plt.Rectangle((-w, -d), 2 * w, 2 * d, fc="#eef2f5",
                               ec="#5d6d7e", lw=1.4))
    for sx, sy in D["sw_xy"]:
        guarded = (sx, sy) == (gx, gy)
        ax.add_patch(plt.Circle((sx, sy), P["sw_bezel_d"] / 2.0,
                                fc="#d5dbe0" if not guarded else "#f5b7b1",
                                ec="#5d6d7e", lw=1.0))
        ax.add_patch(plt.Circle((sx, sy), P["sw_hole_d"] / 2.0, fc="none",
                                ec="#95a5a6", lw=0.7, ls=":"))
    ax.add_patch(plt.Circle((gx, gy), P["guard_od"] / 2.0, fc="none",
                            ec="#c0392b", lw=2.0))
    ax.add_patch(plt.Circle((gx, gy), P["guard_id"] / 2.0, fc="none",
                            ec="#c0392b", lw=2.0))
    for px, py in D["guard_pins"]:
        ax.add_patch(plt.Circle((px, py), P["guard_pin_d"] / 2.0,
                                fc="#c0392b", ec="#7b241c", lw=0.8))
    for bx, by in D["boss_xy"]:
        ax.add_patch(plt.Circle((bx, by), P["lid_screw_head"] / 2.0,
                                fc="none", ec="#7f8c8d", lw=0.8))
    f, drvs = _g["car_axes"](P)
    fx = w if f > 0 else -w                      # which end faces the front
    ax.annotate("FRONT of car", (fx + 4 * f, 0), rotation=-90 * f,
                va="center", ha="left" if f > 0 else "right",
                fontsize=11, color="#c0392b", weight="bold")
    ax.annotate("REAR", (-fx - 4 * f, 0), rotation=90 * f, va="center",
                ha="right" if f > 0 else "left", fontsize=10, color="#5d6d7e")
    dy = d if drvs > 0 else -d
    ax.annotate("driver's side (%sY)" % ("+" if drvs > 0 else "-"),
                (0, dy + 4 * drvs), ha="center",
                va="bottom" if drvs > 0 else "top", fontsize=10,
                color="#5d6d7e")
    ax.annotate("kerb side (%sY)" % ("-" if drvs > 0 else "+"),
                (0, -dy - 4 * drvs), ha="center",
                va="top" if drvs > 0 else "bottom", fontsize=10,
                color="#5d6d7e")
    ax.set_title("start/stop guard on the %s %s button, at (%.1f, %.1f)"
                 % (P["guard_sw"][1], P["guard_sw"][0], gx, gy))
    ax.set_xlim(-w - 16, w + 16)
    ax.set_ylim(-d - 12, d + 12)

    # --- true section through both pins
    ax = axes[1]
    _slice_dir(ax, keypad, (gx, gy), P["guard_pin_angle"], colour="#1f6f4a")
    if ring:
        _slice_dir(ax, ring, (gx, gy), P["guard_pin_angle"], colour="#c0392b")
    # The head drops only as far as the register, so it stands proud.
    head_z = D["lid_z1"] - P["sw_bezel_depth"]
    ax.add_patch(plt.Rectangle((-P["sw_head_d"] / 2.0, head_z),
                               P["sw_head_d"], P["sw_head_t"], fc="#95a5a6",
                               ec="#5d6d7e", alpha=0.6))
    ax.text(0, head_z - 1.0, "button head, %.1f proud" % D["sw_head_proud"],
            ha="center", va="top", fontsize=8, color="#5d6d7e")
    ax.annotate("", (-P["guard_od"] / 2.0 - 3, head_z + P["sw_head_t"]),
                (-P["guard_od"] / 2.0 - 3, D["lid_z1"] + P["guard_h"]),
                arrowprops=dict(arrowstyle="<->", color="#1f6f4a"))
    ax.text(-P["guard_od"] / 2.0 - 4, head_z + P["sw_head_t"]
            + D["guard_above_button"] / 2.0,
            "%.1f above button" % D["guard_above_button"], fontsize=8,
            color="#1f6f4a", va="center", ha="right")
    ax.annotate("", (P["guard_od"] / 2.0 + 3, D["lid_z1"]),
                (P["guard_od"] / 2.0 + 3, D["lid_z1"] + P["guard_h"]),
                arrowprops=dict(arrowstyle="<->", color="#c0392b"))
    ax.text(P["guard_od"] / 2.0 + 4, D["lid_z1"] + P["guard_h"] / 2.0,
            "%.1f mm ring" % P["guard_h"], fontsize=9, color="#c0392b",
            va="center")
    ax.set_title("section on the pin diagonal (%.0f deg) - ring in red, "
                 "lid in green" % P["guard_pin_angle"])

    for ax in axes:
        ax.set_aspect("equal")
        ax.grid(alpha=0.25)
        ax.set_xlabel("mm")
    axes[1].set_ylabel("z (mm)")
    fig.tight_layout()
    out = os.path.join(IMG, "guard_detail-%s.png" % VARIANT)
    fig.savefig(out, dpi=110, facecolor="white")
    print("wrote", out)


def corner_coupons(lid_c, box_c):
    """The two corner coupons, mated and exploded.  Both were moved by the
    same amount in X and Y when they were cut, so the only offset needed to
    put them back together is in Z."""
    # box coupon holds the top corner_coupon_box_h of the box, so the wall
    # top is at its own z = corner_coupon_box_h; the lid coupon starts at
    # the bottom of its locating lip, lid_lip_h below the wall top.
    dz = P["corner_coupon_box_h"] - P["lid_lip_h"]
    grey = (0.26, 0.30, 0.34)
    blue = (0.20, 0.36, 0.52)

    fig, axes = plt.subplots(1, 2, figsize=(15, 7))
    for ax, gap, title in ((axes[0], 0.0, "mated"),
                           (axes[1], 22.0, "exploded")):
        lid = lid_c.copy()
        lid.translate(App.Vector(0, 0, dz + gap))
        _paint(ax, [(box_c, grey), (lid, blue)], 24, 38)
        ax.set_title("corner coupons - %s" % title)
    fig.tight_layout()
    out = os.path.join(IMG, "corner_coupon-%s.png" % VARIANT)
    fig.savefig(out, dpi=100, facecolor="white")
    print("wrote", out)


def main():
    if not os.path.isdir(IMG):
        os.makedirs(IMG)
    box = load("Enclosure-Box")
    keypad = load("Lid-Keypad")
    ring = load("Guard-Ring") if P["guard_ring"] else None
    assembly(box, keypad, ring)
    sections(box, keypad)
    bezel_detail(keypad)
    if ring:
        guard_detail(keypad, ring)
    if P["corner_coupon"] and P["guard_ring"]:
        corner_coupons(load("Corner-Coupon-Lid"), load("Corner-Coupon-Box"))


main()
