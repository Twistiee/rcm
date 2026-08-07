#!/usr/bin/env python3
"""Stitch a poured net's ISOLATED ISLANDS to the main pour, and rescue pads it cannot reach.

Companion to route_board.py --no-route NET. Taking GND out of the DSN stops freerouting
laying traces the pour would have connected anyway, which frees real room for signals --
but signal traces then slice the top pour into islands, and pads boxed in by those traces
end up genuinely unconnected.

TARGETED, not blanket. An earlier version walked a grid over the whole board and dropped a
via wherever one fitted -- 250 of them on a 140x70 board. Most joined the main pour to
itself, achieving nothing, while perforating the pour and adding 250 holes to the drill
file. This version finds the filled polygons, leaves the largest one alone, and stitches
only the islands that actually need it.

EVERY placement IS DRC-VERIFIED. Hand-rolled clearance maths missed two things on the first
attempt: hole-to-hole clearance against THT pads (a via landed 0.077mm from the battery
input), and rescue traces were checked against pads but never against TRACKS -- so one drove
straight through SR_SCK and shorted it. Rather than reimplement KiCad's rule engine, this
places candidates, runs the real DRC, and removes whatever it complains about.

Usage: stitch_zone_vias.py <board.kicad_pcb> [--net GND] [--rescue] [--dry-run]
                           [--drill 0.3] [--diameter 0.6] [--kicad-cli PATH]
"""
import argparse
import json
import math
import os
import re
import subprocess
import tempfile

import pcbnew

CLI = r"C:\Program Files\KiCad\10.0\bin\kicad-cli.exe"


def seg_dist(px, py, x1, y1, x2, y2):
    dx, dy = x2 - x1, y2 - y1
    if dx == 0 and dy == 0:
        return math.hypot(px - x1, py - y1)
    t = max(0.0, min(1.0, ((px - x1) * dx + (py - y1) * dy) / (dx * dx + dy * dy)))
    return math.hypot(px - (x1 + t * dx), py - (y1 + t * dy))


def run_drc(board_path, cli):
    """Real KiCad DRC. Returns the parsed report."""
    fd, path = tempfile.mkstemp(suffix=".json")
    os.close(fd)
    subprocess.run([cli, "pcb", "drc", "--format", "json", "-o", path,
                    "--severity-error", "--severity-warning", board_path],
                   capture_output=True)
    with open(path, encoding="utf-8") as f:
        rep = json.load(f)
    os.unlink(path)
    return rep


def electrical(rep):
    """Violations that matter -- silkscreen is cosmetic and never caused by a via."""
    return [v for v in rep.get("violations", []) if not v["type"].startswith("silk")]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("board")
    ap.add_argument("--net", default="GND")
    ap.add_argument("--drill", type=float, default=0.3)
    ap.add_argument("--diameter", type=float, default=0.6)
    ap.add_argument("--clearance", type=float, default=0.3)
    ap.add_argument("--rescue", action="store_true",
                    help="also try to reach pads the pour cannot, with a via + short track")
    ap.add_argument("--kicad-cli", default=CLI)
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    board_path = os.path.abspath(a.board)
    b = pcbnew.LoadBoard(board_path)
    net = b.FindNet(a.net)
    if net is None:
        raise SystemExit("net not found: %s" % a.net)
    ncode = net.GetNetCode()
    via_r = a.diameter / 2.0

    # --- obstacles, for a cheap first filter only. DRC is the real judge. ---
    pads, tracks = [], []
    for f in b.GetFootprints():
        for p in f.Pads():
            c = p.GetPosition()
            pads.append((c.x / 1e6, c.y / 1e6,
                         max(p.GetSize().x, p.GetSize().y) / 2e6))
    for t in b.GetTracks():
        if t.GetClass() == "PCB_VIA":
            c = t.GetPosition()
            pads.append((c.x / 1e6, c.y / 1e6, t.GetWidth(pcbnew.F_Cu) / 2e6))
        else:
            s, e = t.GetStart(), t.GetEnd()
            tracks.append((s.x / 1e6, s.y / 1e6, e.x / 1e6, e.y / 1e6,
                           t.GetWidth() / 2e6, t.GetNetCode()))

    def clear_of_copper(x, y, r):
        if any(math.hypot(x - px, y - py) < r + pr + a.clearance for px, py, pr in pads):
            return False
        return not any(seg_dist(x, y, sx, sy, ex, ey) < r + hw + a.clearance
                       for sx, sy, ex, ey, hw, _n in tracks)

    added = []

    # --- island stitching -------------------------------------------------------
    # Only islands. The largest filled polygon on each layer IS the main pour and needs
    # nothing; stitching it to itself is the waste the blanket grid was full of.
    for zi in range(b.GetAreaCount()):
        z = b.GetArea(zi)
        if z.GetIsRuleArea() or z.GetNetCode() != ncode:
            continue
        if z.GetLayer() != pcbnew.F_Cu:
            continue                      # bottom pour is the thing we stitch *to*
        polys = z.GetFilledPolysList(pcbnew.F_Cu)
        n = polys.OutlineCount()
        if n < 2:
            continue
        areas = []
        for i in range(n):
            ol = polys.Outline(i)
            pts = [(ol.CPoint(k).x / 1e6, ol.CPoint(k).y / 1e6)
                   for k in range(ol.PointCount())]
            xs = [p[0] for p in pts]
            ys = [p[1] for p in pts]
            areas.append(((max(xs) - min(xs)) * (max(ys) - min(ys)), i, pts))
        areas.sort(reverse=True)
        for _area, i, pts in areas[1:]:          # skip the main pour
            xs = [p[0] for p in pts]
            ys = [p[1] for p in pts]
            placed_here = False
            # try the centroid, then walk the island's own vertices inward
            cands = [(sum(xs) / len(xs), sum(ys) / len(ys))]
            cx0, cy0 = cands[0]
            cands += [((px + cx0) / 2, (py + cy0) / 2) for px, py in pts[::2]]
            for vx, vy in cands:
                if clear_of_copper(vx, vy, via_r):
                    added.append(("via", vx, vy, None))
                    placed_here = True
                    break
            if not placed_here:
                print("  island %d (%.1fmm span): no room for a via" % (i, _area ** 0.5))

    # --- rescue isolated pads ---------------------------------------------------
    if a.rescue:
        rep = run_drc(board_path, a.kicad_cli)
        want = set()
        for v in rep.get("unconnected_items", []):
            for i in v["items"]:
                m = re.match(r"Pad (\S+) \[%s\] of (\S+)" % re.escape(a.net),
                             i.get("description", ""))
                if m:
                    want.add((m.group(2), m.group(1)))
        for ref, padname in sorted(want):
            fp = b.FindFootprintByReference(ref)
            pad = next((p for p in fp.Pads() if p.GetPadName() == padname), None) if fp else None
            if pad is None:
                continue
            cx, cy = pad.GetPosition().x / 1e6, pad.GetPosition().y / 1e6
            best = None
            for rad in [r * 0.25 for r in range(3, 25)]:
                for k in range(48):
                    ang = 2 * math.pi * k / 48
                    vx, vy = cx + rad * math.cos(ang), cy + rad * math.sin(ang)
                    if not clear_of_copper(vx, vy, via_r):
                        continue
                    # THE BUG THAT SHORTED SR_SCK: the trace was only ever checked
                    # against pads. It must clear every track of a DIFFERENT net too.
                    hw = 0.1
                    if any(seg_dist(sx, sy, cx, cy, vx, vy) < hw + thw + a.clearance or
                           seg_dist(ex, ey, cx, cy, vx, vy) < hw + thw + a.clearance
                           for sx, sy, ex, ey, thw, tn in tracks if tn != ncode):
                        continue
                    best = (vx, vy)
                    break
                if best:
                    break
            if best is None:
                print("  RESCUE %s.%s: boxed in, no legal escape" % (ref, padname))
            else:
                added.append(("rescue", best[0], best[1], pad))

    if a.dry_run:
        print("would add %d item(s)" % len(added))
        return

    # --- apply, then let the REAL DRC judge, and back out anything it dislikes ----
    before = len(electrical(run_drc(board_path, a.kicad_cli)))
    objs = []
    for kind, x, y, pad in added:
        v = pcbnew.PCB_VIA(b)
        v.SetPosition(pcbnew.VECTOR2I(pcbnew.FromMM(x), pcbnew.FromMM(y)))
        v.SetDrill(pcbnew.FromMM(a.drill))
        v.SetWidth(pcbnew.FromMM(a.diameter))
        v.SetNetCode(ncode)
        v.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu)
        b.Add(v)
        group = [v]
        if kind == "rescue":
            tr = pcbnew.PCB_TRACK(b)
            tr.SetStart(pad.GetPosition())
            tr.SetEnd(pcbnew.VECTOR2I(pcbnew.FromMM(x), pcbnew.FromMM(y)))
            tr.SetWidth(pcbnew.FromMM(0.2))
            tr.SetLayer(pcbnew.F_Cu)
            tr.SetNetCode(ncode)
            b.Add(tr)
            group.append(tr)
        objs.append(group)
    pcbnew.ZONE_FILLER(b).Fill(b.Zones())
    b.Save(board_path)

    after = len(electrical(run_drc(board_path, a.kicad_cli)))
    if after > before:
        print("  DRC went %d -> %d electrical violations; backing out the additions"
              % (before, after))
        # bisect would be nicer, but one-at-a-time is honest and this runs once
        for group in objs:
            for o in group:
                b.RemoveNative(o)
        pcbnew.ZONE_FILLER(b).Fill(b.Zones())
        b.Save(board_path)
        kept = 0
        for group in objs:
            for o in group:
                b.Add(o)
            pcbnew.ZONE_FILLER(b).Fill(b.Zones())
            b.Save(board_path)
            if len(electrical(run_drc(board_path, a.kicad_cli))) > before:
                for o in group:
                    b.RemoveNative(o)
            else:
                kept += 1
        pcbnew.ZONE_FILLER(b).Fill(b.Zones())
        b.Save(board_path)
        print("  kept %d of %d additions" % (kept, len(objs)))
    else:
        print("  added %d item(s), DRC electrical violations %d -> %d"
              % (len(added), before, after))


if __name__ == "__main__":
    main()
