#!/usr/bin/env python3
"""Shrink reference-designator silkscreen text so crowded refs have somewhere to go.

relocate_refs.py moves refs out of collisions, but on a dense board it runs out of free
spots and gives up ("no free spot"). A ref that stays overlapped is unreadable, which is
worse for hand-rework than one that is simply small -- so trading a little height for a
placement is a net win on a board built for repairability.

0.8mm is JLC's stated minimum silkscreen text height (they *recommend* >=1mm). Thickness
scales with it to keep the stroke-to-height ratio legible; 0.13mm clears their 0.15mm
minimum line width only if you keep an eye on it, so the floor here is 0.15.

Values are left alone -- only reference fields move, and only on footprints, never
free-standing board text.

Usage: shrink_refs.py <board.kicad_pcb> [size_mm] [--only refs.txt]
"""
import sys

import pcbnew

MIN_TH = 0.15   # JLC minimum silkscreen line width, mm


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    board_path = args[0]
    size = float(args[1]) if len(args) > 1 else 0.8

    only = None
    if "--only" in sys.argv:
        with open(sys.argv[sys.argv.index("--only") + 1], encoding="utf-8") as f:
            only = {ln.strip() for ln in f if ln.strip()}

    th = max(MIN_TH, round(size * 0.16, 3))
    board = pcbnew.LoadBoard(board_path)
    n = 0
    for fp in board.GetFootprints():
        ref = fp.Reference()
        if ref.IsVisible() is False:
            continue
        if only is not None and fp.GetReference() not in only:
            continue
        ref.SetTextSize(pcbnew.VECTOR2I(pcbnew.FromMM(size), pcbnew.FromMM(size)))
        ref.SetTextThickness(pcbnew.FromMM(th))
        n += 1
    board.Save(board_path)
    print("shrank %d reference fields to %.2fmm (thickness %.3fmm)" % (n, size, th))


if __name__ == "__main__":
    main()
