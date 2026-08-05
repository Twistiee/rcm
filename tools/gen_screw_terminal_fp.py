#!/usr/bin/env python3
"""Generate a KiCad footprint for a horizontal-entry PCB screw terminal block.

The cheap Chinese blocks (Degson DB125, Kefa KF350/KF128L, Ningbo XY350V ...) are
all the same shape: a row of pins on the pitch, a plastic body exactly
poles x pitch long so the blocks butt together seamlessly, screws on top above
each pin, and the wire entering horizontally through one face. KiCad ships
footprints for the western equivalents (Metz, Phoenix, WAGO) but almost none of
these, and they are the parts JLC actually stocks -- so they get hand-drawn, and
hand-drawn footprints are where courtyard and pin-1 mistakes come from.

Everything here comes off the manufacturer drawing. The two numbers worth being
careful about are --rear and --front: the pin row is NOT centred in the body, and
which face the wire enters matters for whether the harness leaves the board or
runs back across it. Both are measured from the pin centreline, and the wire
entry is always +Y in the generated footprint.

Pin 1 is marked two ways, neither of which is an arrow: a roundrect pad (vs round
for the rest) and a filled dot inside the body at the pin-1 corner. A triangle
beside a connector reads as a direction indicator to anyone looking at the board,
which is exactly the wrong thing to imply about a wire entry.

Example -- Degson DB125-3.5, 4 poles, from datasheet DB125-3.5-XXP-C-S rev TO-1:

  python gen_screw_terminal_fp.py \
      --name TerminalBlock_DB125-3.5-4P_1x04_P3.50mm_Horizontal \
      --poles 4 --pitch 3.5 --drill 1.2 --pad 2.3 --rear 3.5 --front 3.9 \
      --height 8.6 --mpn DB125-3.5-4P-GN-S --lcsc C2757925 \
      --extra "10A 300V, 28-16AWG, M2 screw" --out ./footprints/pdm14.pretty
"""
import argparse
import os
import sys
import uuid

NS = uuid.UUID("6f1d4e10-0000-4000-8000-000000000000")


def uid(name, tag):
    return str(uuid.uuid5(NS, f"{name}/{tag}"))


def fp_line(x1, y1, x2, y2, layer, width, name, tag):
    return (f'\t(fp_line\n'
            f'\t\t(start {x1:g} {y1:g})\n\t\t(end {x2:g} {y2:g})\n'
            f'\t\t(stroke (width {width:g}) (type default))\n'
            f'\t\t(layer "{layer}")\n\t\t(uuid "{uid(name, tag)}")\n\t)\n')


def fp_circle(cx, cy, r, layer, width, name, tag, fill=False):
    return (f'\t(fp_circle\n'
            f'\t\t(center {cx:g} {cy:g})\n\t\t(end {cx + r:g} {cy:g})\n'
            f'\t\t(stroke (width {width:g}) (type default))\n'
            f'\t\t(fill {"solid" if fill else "none"})\n'
            f'\t\t(layer "{layer}")\n\t\t(uuid "{uid(name, tag)}")\n\t)\n')


def rect_path(x0, y0, x1, y1, cham):
    """Rectangle corner list with the pin-1 (top-left) corner chamfered."""
    return [(x0 + cham, y0), (x1, y0), (x1, y1), (x0, y1), (x0, y0 + cham)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", required=True)
    ap.add_argument("--poles", type=int, required=True)
    ap.add_argument("--pitch", type=float, required=True)
    ap.add_argument("--drill", type=float, required=True)
    ap.add_argument("--pad", type=float, required=True)
    ap.add_argument("--rear", type=float, required=True,
                    help="pin centreline to the REAR face, mm (drawn at -Y)")
    ap.add_argument("--front", type=float, required=True,
                    help="pin centreline to the WIRE ENTRY face, mm (drawn at +Y)")
    ap.add_argument("--height", type=float, default=0.0, help="body height, doc only")
    ap.add_argument("--mpn", default="")
    ap.add_argument("--lcsc", default="")
    ap.add_argument("--extra", default="", help="ratings etc for the description")
    ap.add_argument("--out", required=True, help=".pretty directory")
    a = ap.parse_args()

    n, p = a.poles, a.pitch
    span = (n - 1) * p                 # pin 1 to pin N
    x0, x1 = -p / 2, span + p / 2      # body ends: total length is exactly n*pitch
    y0, y1 = -a.rear, a.front
    cx = span / 2
    cham = min(1.0, p / 2 - 0.25)
    screw_r = min(1.35, p / 2 - 0.3)
    name = a.name

    body_len = n * p
    descr = (f"{a.mpn or name} PCB screw terminal block, {n} poles, {p:g}mm pitch, "
             f"horizontal wire entry. "
             f"Body {body_len:g} x {a.rear + a.front:g}"
             + (f" x {a.height:g}" if a.height else "") + "mm, "
             f"pin centreline {a.rear:g}mm from the rear face and {a.front:g}mm "
             f"from the wire-entry face. PCB hole {a.drill:g}mm. "
             + (f"{a.extra}. " if a.extra else "")
             + (f"LCSC {a.lcsc}. " if a.lcsc else "")
             + "Wire entry faces +Y -- rotate so it points off the board edge, "
               "or the harness has to run back across the board.")

    s = [f'(footprint "{name}"\n'
         f'\t(version 20240108)\n'
         f'\t(generator "gen_screw_terminal_fp.py")\n'
         f'\t(generator_version "10.0")\n'
         f'\t(layer "F.Cu")\n'
         f'\t(descr "{descr}")\n'
         f'\t(tags "THT terminal block screw {p:g}mm {n}P horizontal")\n'
         f'\t(attr through_hole)\n']

    for prop, val, py, layer, tag in (
            ("Reference", "REF**", y0 - 1.2, "F.SilkS", "ref"),
            ("Value", name, y1 + 1.2, "F.Fab", "val")):
        s.append(f'\t(property "{prop}" "{val}"\n'
                 f'\t\t(at {cx:g} {py:g} 0)\n'
                 f'\t\t(layer "{layer}")\n'
                 f'\t\t(uuid "{uid(name, tag)}")\n'
                 f'\t\t(effects (font (size 1 1) (thickness 0.15)))\n\t)\n')

    # ---- F.Fab: true body outline, chamfered at pin 1 ----------------------
    pts = rect_path(x0, y0, x1, y1, cham)
    for i in range(len(pts)):
        ax, ay = pts[i]
        bx, by = pts[(i + 1) % len(pts)]
        s.append(fp_line(ax, ay, bx, by, "F.Fab", 0.1, name, f"fab{i}"))

    # wire-entry mouth: the lip the wire actually goes into, so the drawing shows
    # which face is which without having to read the description.
    mouth = min(1.5, a.front - 0.5)
    s.append(fp_line(x0, y1 - mouth, x1, y1 - mouth, "F.Fab", 0.1, name, "mouth"))

    # screw heads, slot across each
    for i in range(n):
        px = i * p
        s.append(fp_circle(px, 0, screw_r, "F.Fab", 0.1, name, f"screw{i}"))
        r = screw_r * 0.75
        s.append(fp_line(px - r, -r, px + r, r, "F.Fab", 0.1, name, f"slot{i}"))

    # ---- F.SilkS: body offset outward; never crosses a pad ------------------
    o = 0.12
    spts = rect_path(x0 - o, y0 - o, x1 + o, y1 + o, cham)
    for i in range(len(spts)):
        ax, ay = spts[i]
        bx, by = spts[(i + 1) % len(spts)]
        s.append(fp_line(ax, ay, bx, by, "F.SilkS", 0.12, name, f"silk{i}"))

    # pin-1 dot, inside the body at the pin-1 corner. Inside rather than outside
    # so it cannot collide with a neighbour's pads on a tight board.
    dot = 0.3
    s.append(fp_circle(x0 + 0.6, y0 + 0.6, dot, "F.SilkS", 0.12, name, "pin1", fill=True))

    # ---- courtyard ----------------------------------------------------------
    c = 0.25
    cpts = [(x0 - c, y0 - c), (x1 + c, y0 - c), (x1 + c, y1 + c), (x0 - c, y1 + c)]
    for i in range(4):
        ax, ay = cpts[i]
        bx, by = cpts[(i + 1) % 4]
        s.append(fp_line(ax, ay, bx, by, "F.CrtYd", 0.05, name, f"crt{i}"))

    # ---- pads ---------------------------------------------------------------
    for i in range(n):
        shape = "roundrect" if i == 0 else "circle"
        rr = '\n\t\t(roundrect_rratio 0.25)' if i == 0 else ""
        s.append(f'\t(pad "{i + 1}" thru_hole {shape}\n'
                 f'\t\t(at {i * p:g} 0)\n'
                 f'\t\t(size {a.pad:g} {a.pad:g})\n'
                 f'\t\t(drill {a.drill:g})\n'
                 f'\t\t(layers "*.Cu" "*.Mask"){rr}\n'
                 f'\t\t(uuid "{uid(name, f"pad{i}")}")\n\t)\n')

    s.append(')\n')

    os.makedirs(a.out, exist_ok=True)
    path = os.path.join(a.out, f"{name}.kicad_mod")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("".join(s))

    print(f"{path}")
    print(f"  {n}P  body {body_len:g} x {a.rear + a.front:g}mm, pins 0..{span:g} "
          f"@ {p:g}mm, drill {a.drill:g} pad {a.pad:g}")
    print(f"  outline x {x0:g}..{x1:g}   y {y0:g}..{y1:g} (wire entry +Y)")
    print(f"  courtyard x {x0 - c:g}..{x1 + c:g}   y {y0 - c:g}..{y1 + c:g}")


if __name__ == "__main__":
    sys.exit(main())
