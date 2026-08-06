#!/usr/bin/env python3
"""Narrow track segments that miss a clearance rule by a hair, instead of moving them.

Freerouting works in its own grid and rounds on the way back out through the SES file, so
it occasionally lands a track a micron or two inside a clearance rule -- 0.1987mm against
a 0.2000mm rule, say. That is physically meaningless (fabs hold roughly +/-0.05mm) but it
is still a DRC error, and shipping a board with DRC errors in it is how the real ones get
lost in the noise.

Narrowing beats nudging. Moving a segment risks breaking the connection at whichever end
sits on a pad, and shifts the problem into whatever the track was avoiding. Taking width
off gains clearance on BOTH sides for free and cannot change connectivity at all.

Only touches segments whose shortfall is under --max-shortfall, because a track that is
genuinely in the wrong place needs rerouting, not a diet. Refuses to go below --min-width.

Usage: fix_marginal_clearance.py <board.kicad_pcb> <drc.json> [--max-shortfall 0.02]
                                 [--min-width 0.15] [--dry-run]
"""
import argparse
import json
import re

import pcbnew


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("board")
    ap.add_argument("drc")
    ap.add_argument("--max-shortfall", type=float, default=0.02,
                    help="mm; above this the track is misplaced, not marginal")
    ap.add_argument("--min-width", type=float, default=0.15,
                    help="mm; JLC's floor is 0.127 for standard 1oz")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    with open(a.drc, encoding="utf-8") as f:
        drc = json.load(f)

    # collect (net, length_mm, shortfall_mm) for each marginal track/pad clearance
    wanted = []
    for v in drc.get("violations", []):
        if v["type"] != "clearance":
            continue
        m = re.search(r"clearance ([\d.]+) mm; actual ([\d.]+) mm", v.get("description", ""))
        if not m:
            continue
        short = float(m.group(1)) - float(m.group(2))
        if short <= 0 or short > a.max_shortfall:
            continue
        for i in v["items"]:
            d = i.get("description", "")
            t = re.match(r"Track \[([^\]]*)\] on \S+, length ([\d.]+) mm", d)
            if t:
                wanted.append((t.group(1), float(t.group(2)), short))

    if not wanted:
        print("no marginal track/pad clearance violations")
        return

    board = pcbnew.LoadBoard(a.board)
    fixed = 0
    for net, length, short in wanted:
        for tr in board.GetTracks():
            if tr.GetClass() != "PCB_TRACK" or tr.GetNetname() != net:
                continue
            if abs(tr.GetLength() / 1e6 - length) > 0.001:
                continue
            w = tr.GetWidth() / 1e6
            # narrowing by X gains X/2 of clearance per side; take double the shortfall
            # plus a little, so the fix is not itself sitting on the limit
            new_w = round(w - (2 * short + 0.02), 3)
            if new_w < a.min_width:
                print("SKIP %s len=%.4f: would need %.3fmm, below floor %.3f"
                      % (net, length, new_w, a.min_width))
                continue
            print("%s len=%.4f: %.3f -> %.3fmm (shortfall %.4f)"
                  % (net, length, w, new_w, short))
            if not a.dry_run:
                tr.SetWidth(pcbnew.FromMM(new_w))
            fixed += 1
            break

    if not a.dry_run:
        board.Save(a.board)
    print("%s %d segment(s)" % ("would narrow" if a.dry_run else "narrowed", fixed))


if __name__ == "__main__":
    main()
