#!/usr/bin/env python3
"""Add board-level silkscreen text from a JSON spec. Idempotent + re-runnable.

The board generator (netlist_to_board.py) rewrites the PCB from scratch and has
no silk-text support, so labelling is a POST-ROUTE step. This script is safe to
re-run: it first deletes every board-level text item on the silk layers (the
generator never creates any, so anything found is from a previous run) and then
adds the spec fresh. Footprint reference/value text is untouched.

Spec JSON:
  {"defaults": {"size": 0.8, "thickness": 0.15, "layer": "F.SilkS"},
   "texts": [{"text": "SW1", "at": [7, 2], "rot": 90, "justify": "left"}, ...],
   "rects": [{"at": [17.2, 1.8, 52.8, 20.5], "width": 0.15}, ...]}

Anything in a text entry overrides the defaults. Rects are outlines (x1 y1 x2
y2), useful for showing where a plug-on module body sits -- keep their edges
off pads or you will trip the silk-over-pad DRC check.

  justify: left | center | right   (default center)
  layer:   F.SilkS | B.SilkS       (B is auto-mirrored)

Usage:
  python add_silk_text.py <board.kicad_pcb> <spec.json> [--dry-run]

IMPORTANT: always eyeball a render afterwards. Silk defects survive DRC -- a
literal "&gt;" escape once shipped through every DRC run on pdm14 and was only
ever caught by looking at the picture.
"""
import argparse
import json
import sys

import pcbnew

SILK = {"F.SilkS": pcbnew.F_SilkS, "B.SilkS": pcbnew.B_SilkS}
JUST = {
    "left": pcbnew.GR_TEXT_H_ALIGN_LEFT,
    "center": pcbnew.GR_TEXT_H_ALIGN_CENTER,
    "right": pcbnew.GR_TEXT_H_ALIGN_RIGHT,
}


def mm(v):
    return pcbnew.FromMM(float(v))


def clear_silk_graphics(board):
    """Drop board-level text/shapes on silk layers; leave footprint text alone.

    BOARD.Remove() flips the item's SWIG `thisown` from False to True, handing
    ownership to Python -- so the moment our list goes out of scope the objects
    are freed while the board still references them internally. That is a
    use-after-free which typically survives long enough to save a correct file
    and then segfaults the next time anything walks the board (iterating
    footprints is enough). Hand ownership back so nothing frees them; the
    deliberate leak costs nothing in a short-lived script.
    """
    doomed = [d for d in board.GetDrawings()
              if isinstance(d, (pcbnew.PCB_TEXT, pcbnew.PCB_SHAPE))
              and d.GetLayer() in SILK.values()]
    for d in doomed:
        board.Remove(d)
        d.thisown = 0
    return len(doomed)


def add_rect(board, spec, defaults):
    g = dict(defaults)
    g.update(spec)
    layer = g.get("layer", "F.SilkS")
    if layer not in SILK:
        raise SystemExit(f"bad layer {layer!r}")
    x1, y1, x2, y2 = g["at"]
    s = pcbnew.PCB_SHAPE(board)
    s.SetShape(pcbnew.SHAPE_T_RECT)
    s.SetLayer(SILK[layer])
    s.SetStart(pcbnew.VECTOR2I(mm(x1), mm(y1)))
    s.SetEnd(pcbnew.VECTOR2I(mm(x2), mm(y2)))
    s.SetWidth(mm(g.get("width", 0.15)))
    s.SetFilled(False)
    board.Add(s)
    s.thisown = 0  # see add_text()


def add_text(board, spec, defaults):
    g = dict(defaults)
    g.update(spec)
    if "text" not in g or "at" not in g:
        raise SystemExit(f"text entry needs 'text' and 'at': {spec}")

    layer = g.get("layer", "F.SilkS")
    if layer not in SILK:
        raise SystemExit(f"bad layer {layer!r}, expected one of {list(SILK)}")

    raw = str(g["text"])
    # guard against the pdm14 escape bug -- these are silk, not markup
    for bad in ("&gt;", "&lt;", "&amp;", "&quot;"):
        if bad in raw:
            raise SystemExit(f"HTML escape {bad!r} in silk text {raw!r}")

    t = pcbnew.PCB_TEXT(board)
    t.SetText(raw)
    t.SetLayer(SILK[layer])
    x, y = g["at"]
    t.SetPosition(pcbnew.VECTOR2I(mm(x), mm(y)))
    t.SetTextAngle(pcbnew.EDA_ANGLE(float(g.get("rot", 0)), pcbnew.DEGREES_T))
    size = mm(g.get("size", 0.8))
    t.SetTextSize(pcbnew.VECTOR2I(size, size))
    t.SetTextThickness(mm(g.get("thickness", 0.15)))
    t.SetHorizJustify(JUST[g.get("justify", "center")])
    if g.get("bold"):
        t.SetBold(True)
    if layer == "B.SilkS":
        t.SetMirrored(True)
    board.Add(t)
    # SWIG keeps Python ownership of the object we constructed, so without
    # this the interpreter frees it while the board still points at it. The
    # result is a use-after-free that "works" until something else reuses the
    # heap -- here, iterating footprints afterwards segfaults. Disown it.
    t.thisown = 0
    return raw


def apply_refs(board, entries):
    """Move or hide footprint reference text.

    Auto-placed refs love to sit exactly where a functional label belongs.
    Moving them is usually better than shrinking clearances; hiding suits
    test points, where "CANH" tells you far more than "TP4" and the pad is
    not an assembled part.
    """
    # FindFootprintByReference can hand back an unwrapped SwigPyObject; going
    # through GetFootprints() always yields properly typed FOOTPRINTs.
    by_ref = {fp.GetReference(): fp for fp in board.GetFootprints()}
    n = 0
    for e in entries:
        fp = by_ref.get(e["ref"])
        if fp is None:
            raise SystemExit(f"ref {e['ref']!r} not on board")
        t = fp.Reference()
        if e.get("hide"):
            t.SetVisible(False)
        else:
            t.SetVisible(True)
        if "at" in e:
            x, y = e["at"]
            t.SetPosition(pcbnew.VECTOR2I(mm(x), mm(y)))
        if "rot" in e:
            t.SetTextAngle(pcbnew.EDA_ANGLE(float(e["rot"]), pcbnew.DEGREES_T))
        n += 1
    return n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("board")
    ap.add_argument("spec")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    spec = json.load(open(a.spec, encoding="utf-8"))
    defaults = spec.get("defaults", {})
    texts = spec.get("texts", [])
    rects = spec.get("rects", [])

    board = pcbnew.LoadBoard(a.board)
    removed = clear_silk_graphics(board)
    for entry in texts:
        add_text(board, entry, defaults)
    for entry in rects:
        add_rect(board, entry, defaults)
    nrefs = apply_refs(board, spec.get("refs", []))

    print(f"removed {removed} old silk item(s), "
          f"added {len(texts)} text + {len(rects)} rect, "
          f"adjusted {nrefs} ref(s)")
    if a.dry_run:
        print("dry run -- not saved")
        return
    board.Save(a.board)
    print(f"saved {a.board}")


if __name__ == "__main__":
    sys.exit(main())
