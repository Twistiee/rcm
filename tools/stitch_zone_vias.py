#!/usr/bin/env python3
"""Stitch a poured net's islands together with a grid of vias.

Companion to route_board.py --no-route NET. Taking GND out of the DSN stops freerouting
laying traces the pour would have connected anyway -- a big win on a dense board -- but it
also means nothing ties the F.Cu pour to the B.Cu pour, and signal traces slice the top
pour into islands that are then genuinely unconnected.

Stitching vias fix that: drop vias on a grid wherever both pours exist and there is room,
and the islands merge through the bottom pour. This is what you would do by hand, just
without the clicking.

Candidates are rejected conservatively -- a via must clear every pad, track, via and the
board edge by (its own radius + clearance + the other object's half-width). Being too shy
costs a few stitch points; being too bold costs a short.

WHEN THIS ACTUALLY WORKS -- measured on rcm, 2026-08-07, 140x70mm 2-layer, ~430 nets:

  route with GND     426/426 routed, 0 unconnected, 2244 segments, 87s
  --no-route GND     293/293 routed, 1715 segments (-24%), 57s ... but 49 unconnected GND
                     + 244 stitching vias -> 45
                     + rescue pass         -> 42  (6 pads had no legal via spot at all,
                                                   three of them MCU grounds)
                     + island removal      -> 42  (no change; slivers were not the problem)

So on a board THIS dense the technique buys real signal-routing headroom and then hands
back a GND net that will not close. The pour simply cannot reach pads that signal traces
have boxed in, and no amount of stitching fixes a pad with no escape route.

Use it where the board has room to breathe, or where you are willing to finish GND by hand.
On a congested 2-layer board, letting freerouting route GND normally and relying on the
pour for the rest is the cheaper answer, even though it looks wasteful.
"""
import argparse
import math

import pcbnew


def seg_dist(px, py, x1, y1, x2, y2):
    dx, dy = x2 - x1, y2 - y1
    if dx == 0 and dy == 0:
        return math.hypot(px - x1, py - y1)
    t = max(0.0, min(1.0, ((px - x1) * dx + (py - y1) * dy) / (dx * dx + dy * dy)))
    return math.hypot(px - (x1 + t * dx), py - (y1 + t * dy))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("board")
    ap.add_argument("--net", default="GND")
    ap.add_argument("--pitch", type=float, default=5.0)
    ap.add_argument("--drill", type=float, default=0.3)
    ap.add_argument("--diameter", type=float, default=0.6)
    ap.add_argument("--clearance", type=float, default=0.25)
    ap.add_argument("--margin", type=float, default=2.0, help="keep-out from board edge, mm")
    ap.add_argument("--rescue-drc", help="DRC json: rescue every unconnected pad of the "
                    "net by dropping a via beside it and a short track to reach it. These "
                    "are pads the pour cannot reach because signal traces box them in -- "
                    "the handful of hand-fixes that --no-route leaves behind.")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    b = pcbnew.LoadBoard(a.board)
    net = b.FindNet(a.net)
    if net is None:
        raise SystemExit("net not found: %s" % a.net)
    ncode = net.GetNetCode()
    via_r = a.diameter / 2.0

    # obstacles: every pad and track, whatever the net. Same-net copper is skipped only
    # for PADS of the stitch net -- a via on top of a GND pad is pointless, not unsafe.
    pads, tracks = [], []
    for f in b.GetFootprints():
        for p in f.Pads():
            c = p.GetPosition()
            r = max(p.GetSize().x, p.GetSize().y) / 2e6
            pads.append((c.x / 1e6, c.y / 1e6, r))
    for t in b.GetTracks():
        if t.GetClass() == "PCB_VIA":
            c = t.GetPosition()
            pads.append((c.x / 1e6, c.y / 1e6, t.GetWidth(pcbnew.F_Cu) / 2e6))
        else:
            s, e = t.GetStart(), t.GetEnd()
            tracks.append((s.x / 1e6, s.y / 1e6, e.x / 1e6, e.y / 1e6,
                           t.GetWidth() / 2e6))

    box = b.GetBoardEdgesBoundingBox()
    x0, y0 = box.GetLeft() / 1e6 + a.margin, box.GetTop() / 1e6 + a.margin
    x1, y1 = box.GetRight() / 1e6 - a.margin, box.GetBottom() / 1e6 - a.margin

    placed = 0
    y = y0
    while y <= y1:
        x = x0
        while x <= x1:
            ok = True
            for px, py, pr in pads:
                if math.hypot(x - px, y - py) < via_r + pr + a.clearance:
                    ok = False
                    break
            if ok:
                for sx, sy, ex, ey, hw in tracks:
                    if seg_dist(x, y, sx, sy, ex, ey) < via_r + hw + a.clearance:
                        ok = False
                        break
            if ok and not a.dry_run:
                v = pcbnew.PCB_VIA(b)
                v.SetPosition(pcbnew.VECTOR2I(pcbnew.FromMM(x), pcbnew.FromMM(y)))
                v.SetDrill(pcbnew.FromMM(a.drill))
                v.SetWidth(pcbnew.FromMM(a.diameter))
                v.SetNetCode(ncode)
                v.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu)
                b.Add(v)
            if ok:
                placed += 1
            x += a.pitch
        y += a.pitch

    # ---- rescue pass -------------------------------------------------------
    rescued = 0
    if a.rescue_drc:
        import json as _json
        import re as _re
        drc = _json.load(open(a.rescue_drc, encoding="utf-8"))
        want = set()
        for v in drc.get("unconnected_items", []):
            for i in v["items"]:
                m = _re.match(r"Pad (\S+) \[%s\] of (\S+)" % _re.escape(a.net),
                              i.get("description", ""))
                if m:
                    want.add((m.group(2), m.group(1)))
        for ref, padname in sorted(want):
            fp = b.FindFootprintByReference(ref)
            if fp is None:
                continue
            pad = next((p for p in fp.Pads() if p.GetPadName() == padname), None)
            if pad is None:
                continue
            cx, cy = pad.GetPosition().x / 1e6, pad.GetPosition().y / 1e6
            best = None
            for rad in [r * 0.25 for r in range(3, 25)]:      # 0.75mm out to 6mm
                for k in range(48):
                    ang = 2 * math.pi * k / 48
                    vx, vy = cx + rad * math.cos(ang), cy + rad * math.sin(ang)
                    if not (x0 <= vx <= x1 and y0 <= vy <= y1):
                        continue
                    okv = all(math.hypot(vx - px, vy - py) >= via_r + pr + a.clearance
                              for px, py, pr in pads
                              if math.hypot(cx - px, cy - py) > 0.01)
                    if okv:
                        okv = all(seg_dist(vx, vy, sx, sy, ex, ey) >= via_r + hw + a.clearance
                                  for sx, sy, ex, ey, hw in tracks)
                    if not okv:
                        continue
                    # the rescue TRACK must clear other copper too, or we just made a short
                    okt = all(seg_dist(px, py, cx, cy, vx, vy) >= 0.1 + pr + a.clearance
                              for px, py, pr in pads
                              if math.hypot(cx - px, cy - py) > 0.01)
                    if okt and best is None:
                        best = (vx, vy)
                        break
                if best:
                    break
            if best is None:
                print("  RESCUE FAILED %s.%s -- no legal via spot, needs a hand fix"
                      % (ref, padname))
                continue
            vx, vy = best
            if not a.dry_run:
                v = pcbnew.PCB_VIA(b)
                v.SetPosition(pcbnew.VECTOR2I(pcbnew.FromMM(vx), pcbnew.FromMM(vy)))
                v.SetDrill(pcbnew.FromMM(a.drill))
                v.SetWidth(pcbnew.FromMM(a.diameter))
                v.SetNetCode(ncode)
                v.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu)
                b.Add(v)
                tr = pcbnew.PCB_TRACK(b)
                tr.SetStart(pad.GetPosition())
                tr.SetEnd(pcbnew.VECTOR2I(pcbnew.FromMM(vx), pcbnew.FromMM(vy)))
                tr.SetWidth(pcbnew.FromMM(0.2))
                tr.SetLayer(pcbnew.F_Cu)
                tr.SetNetCode(ncode)
                b.Add(tr)
            rescued += 1
        print("  rescued %d isolated pad(s)" % rescued)

    if not a.dry_run:
        pcbnew.ZONE_FILLER(b).Fill(b.Zones())
        b.Save(a.board)
    print("%s %d stitching via(s) on %s at %.1fmm pitch"
          % ("would place" if a.dry_run else "placed", placed, a.net, a.pitch))


if __name__ == "__main__":
    main()
