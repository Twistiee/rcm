#!/usr/bin/env python3
"""Generate THT footprints for a plug-on buck-converter module.

The module is soldered onto the host PCB by header pins through its own holes;
the board provides one 1x02 footprint per side (IN and OUT), each with the
'+' pad on top and the '-' pad below at the module's row pitch.

Deliberate design choices (learned the hard way on keypad, 2026-07-26):
  * Courtyards are PER PAD, not one box spanning the whole connector. A
    full-height courtyard overlaps neighbouring parts and trips DRC for no
    reason -- the module body floats above the board on its pins.
  * The module body is drawn on F.Fab ONLY (no courtyard, no silk), so it is
    visible while placing but has zero DRC effect. Keep parts out from under
    it via the placement plan, not via geometry.

Modules with TWIN (stacked) holes per terminal get both holes, sharing one pad
number each -- more solder joints, and the user can fit ordinary 2-pin 2.54mm
header strips, which self-align perpendicular far better than lone pins.

Usage:
  python gen_buck_module_fp.py --out DIR --name NAME \
      --inner-pitch 10.16 [--twin-pitch 2.54] \
      [--body 33.788x18.0 --body-offset-x 1.654] \
      [--slot-len 3.0] [--drill 1.0] [--pad 1.7]
"""
import argparse
import os
import uuid

_n = 0


def uid():
    global _n
    _n += 1
    return str(uuid.UUID(int=_n, version=4))


def pad(num, shape, x, y, drill, size, slot_len=0.0):
    """slot_len > drill turns the pad into an X-elongated plated slot.

    Used to absorb uncertainty in a module's hole spacing along one axis: fix
    one side of the module with round holes and slot the other, so the exact
    centre-to-centre distance stops being critical.
    """
    if slot_len and slot_len > drill:
        shape = "oval"
        sx, sy = size + (slot_len - drill), size
        drill_s = f'(drill oval {slot_len:g} {drill:g})'
    else:
        sx = sy = size
        drill_s = f'(drill {drill:g})'
    return (
        f'\t(pad "{num}" thru_hole {shape}\n'
        f'\t\t(at {x:g} {y:g})\n'
        f'\t\t(size {sx:g} {sy:g})\n'
        f'\t\t{drill_s}\n'
        f'\t\t(layers "*.Cu" "*.Mask")\n'
        f'\t\t(remove_unused_layers no)\n'
        f'\t\t(uuid "{uid()}")\n'
        f'\t)\n'
    )


def rect(layer, x1, y1, x2, y2, width):
    return (
        f'\t(fp_rect\n'
        f'\t\t(start {x1:g} {y1:g})\n'
        f'\t\t(end {x2:g} {y2:g})\n'
        f'\t\t(stroke\n\t\t\t(width {width:g})\n\t\t\t(type default)\n\t\t)\n'
        f'\t\t(fill no)\n'
        f'\t\t(layer "{layer}")\n'
        f'\t\t(uuid "{uid()}")\n'
        f'\t)\n'
    )


def effects(size, thick):
    return (f'\t\t(effects\n\t\t\t(font\n'
            f'\t\t\t\t(size {size:g} {size:g})\n'
            f'\t\t\t\t(thickness {thick:g})\n\t\t\t)\n\t\t)\n')


def prop(key, value, x, y, layer, size=1.0, thick=0.15, hide=False):
    h = "\n\t\t(hide yes)" if hide else ""
    return (
        f'\t(property "{key}" "{value}"\n'
        f'\t\t(at {x:g} {y:g} 0)\n'
        f'\t\t(layer "{layer}"){h}\n'
        f'\t\t(uuid "{uid()}")\n'
        + effects(size, thick) +
        f'\t)\n'
    )


def fp_text(value, x, y, layer, size=1.0, thick=0.15):
    return (
        f'\t(fp_text user "{value}"\n'
        f'\t\t(at {x:g} {y:g} 0)\n'
        f'\t\t(layer "{layer}")\n'
        f'\t\t(uuid "{uid()}")\n'
        + effects(size, thick) +
        f'\t)\n'
    )


def build(name, inner_pitch, twin_pitch, drill, pad_dia, body=None,
          body_off_x=None, side="IN", descr="", tags="", slot_len=0.0,
          per_terminal=None):
    """One column of holes: '+' terminal on top, '-' terminal below.

    A terminal may be a GROUP of `per_terminal` holes spaced `twin_pitch` apart
    (modules commonly duplicate each terminal for current and for solder area).
    Every hole in a group carries the SAME pad number, so a 6-hole footprint
    still maps onto a 2-pin schematic symbol -- KiCad treats same-numbered pads
    as one net. A group of 3 on a 2.54mm grid also takes an ordinary 3-pin
    header strip, which self-aligns far better than lone pins.

    `per_terminal` defaults to 2 when twin_pitch is given, else 1, so the
    original twin/single calls keep working unchanged.

    `inner_pitch` is the gap between the INNERMOST holes of the two groups.
    Origin (0,0) is the topmost hole.
    Column span = 2*(per_terminal-1)*twin + inner.
    """
    if per_terminal is None:
        per_terminal = 2 if twin_pitch else 1
    if per_terminal > 1 and not twin_pitch:
        raise ValueError("--holes-per-terminal > 1 needs --twin-pitch")

    cy = pad_dia / 2 + 0.25  # per-pad courtyard half-size
    cx = cy + (max(slot_len - drill, 0.0) / 2)  # slots grow the courtyard in X

    # y of every hole, grouped by terminal
    grp = (per_terminal - 1) * twin_pitch
    plus = [i * twin_pitch for i in range(per_terminal)]
    minus_y = grp + inner_pitch
    minus = [minus_y + i * twin_pitch for i in range(per_terminal)]
    span = 2 * grp + inner_pitch
    mid = span / 2

    s = [f'(footprint "{name}"\n',
         '\t(version 20240108)\n',
         '\t(generator "gen_buck_module_fp")\n',
         '\t(generator_version "10.0")\n',
         '\t(layer "F.Cu")\n',
         f'\t(descr "{descr}")\n',
         f'\t(tags "{tags}")\n',
         '\t(attr through_hole)\n']

    s.append(prop("Reference", "REF**", 0, -2.6, "F.SilkS"))
    s.append(prop("Value", name, 0, span + 2.6, "F.Fab"))
    s.append(prop("Datasheet", "", 0, 0, "F.Fab", hide=True))
    s.append(prop("Description", descr, 0, 0, "F.Fab", hide=True))
    s.append(fp_text("${REFERENCE}", 0, mid, "F.Fab", 0.8, 0.12))

    # pad 1 = '+', pad 2 = '-'; first hole of pad 1 is rect as the pin-1 marker
    for i, y in enumerate(plus):
        s.append(pad(1, "rect" if i == 0 else "circle", 0, y, drill, pad_dia,
                     slot_len))
    for y in minus:
        s.append(pad(2, "circle", 0, y, drill, pad_dia, slot_len))

    # polarity + side marks on silk, clear of the pads
    s.append(fp_text("+", -cx - 1.0, sum(plus) / len(plus), "F.SilkS"))
    s.append(fp_text("-", -cx - 1.0, sum(minus) / len(minus), "F.SilkS"))
    s.append(fp_text(side, cx + 1.4, mid, "F.SilkS", 0.9))

    # PER-PAD courtyards (see module docstring)
    for y in plus + minus:
        s.append(rect("F.CrtYd", -cx, y - cy, cx, y + cy, 0.05))

    # module body outline, F.Fab only -- placement aid, no DRC effect
    if body:
        bw, bh = body
        x1 = -body_off_x
        y1 = -(bh - span) / 2
        s.append(rect("F.Fab", x1, y1, x1 + bw, y1 + bh, 0.1))

    s.append(')\n')
    return "".join(s)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True, help="target .pretty directory")
    ap.add_argument("--name", required=True, help="footprint name (no lib)")
    ap.add_argument("--inner-pitch", type=float, required=True,
                    help="mm between the two INNERMOST holes (the + terminal's "
                         "lowest hole to the - terminal's highest)")
    ap.add_argument("--twin-pitch", type=float, default=0.0,
                    help="mm between the stacked duplicate holes of one "
                         "terminal; 0 for a single hole per terminal. All "
                         "holes of a terminal share a pad number, so the "
                         "schematic symbol stays 1x02.")
    ap.add_argument("--holes-per-terminal", type=int, default=None,
                    help="holes in each terminal group (default 2 when "
                         "--twin-pitch is given, else 1). Use 3 for modules "
                         "that triple up a terminal.")
    ap.add_argument("--body", help="module body size, e.g. 33.788x18.0")
    ap.add_argument("--body-offset-x", type=float,
                    help="mm from the module's left edge to the IN hole column")
    ap.add_argument("--drill", type=float, default=1.0)
    ap.add_argument("--pad", type=float, default=1.7)
    ap.add_argument("--slot-len", type=float, default=0.0,
                    help="X length of a plated slot drill; absorbs uncertainty "
                         "in the module's IN->OUT hole spacing. Slot covers "
                         "nominal +/- (slot_len - drill)/2.")
    ap.add_argument("--side", default="IN", choices=["IN", "OUT"])
    ap.add_argument("--descr", default="")
    ap.add_argument("--tags", default="")
    a = ap.parse_args()

    body = None
    if a.body:
        if a.body_offset_x is None:
            ap.error("--body requires --body-offset-x")
        bw, bh = (float(v) for v in a.body.lower().split("x"))
        body = (bw, bh)

    os.makedirs(a.out, exist_ok=True)
    path = os.path.join(a.out, a.name + ".kicad_mod")
    with open(path, "w", encoding="utf-8") as f:
        f.write(build(a.name, a.inner_pitch, a.twin_pitch, a.drill, a.pad,
                      body, a.body_offset_x, a.side, a.descr, a.tags,
                      a.slot_len, a.holes_per_terminal))
    k = a.holes_per_terminal if a.holes_per_terminal else (2 if a.twin_pitch else 1)
    span = 2 * (k - 1) * a.twin_pitch + a.inner_pitch
    tol = (a.slot_len - a.drill) / 2 if a.slot_len > a.drill else 0.0
    n = 2 * k
    print(f"wrote {path}  ({n} holes, {span:g}mm span"
          + (f", slotted +/-{tol:.2f}mm in X" if tol else "") + ")")


if __name__ == "__main__":
    main()
