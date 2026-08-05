"""Scripted high-current copper: pours, trunk traces, via arrays — DRC-gated.

Freerouting doesn't understand current density; this applies power copper AFTER
autorouting. Typical flow (PDM-style board):
  1. netlist_to_board.py + route_board.py (all nets, modest class widths)
  2. copper_builder.py <plan>  — replaces the thin autorouted power-net tracks
     with proper pours/trunks, adds stitching vias, refills, runs DRC.

Run with KiCad's bundled python.

copper_plan.json:
{
  "board": "path/to/board.kicad_pcb",
  "kicad_cli": "C:/Program Files/KiCad/10.0/bin/kicad-cli.exe",   # optional
  "items": [
    {"type": "pour", "net": "VBAT_IN", "layer": "F.Cu", "priority": 2,
     "polygon": [[x,y], ...], "min_width": 0.5, "connection": "solid"},
    {"type": "trunk", "net": "OUT_CH1", "layer": "F.Cu", "width": 4.0,
     "points": [[x,y], [x,y], ...], "replace_net_tracks": true},
    {"type": "via_array", "net": "VBAT_IN", "at": [x, y], "rows": 2, "cols": 4,
     "pitch": 1.2, "size": 0.9, "drill": 0.45}
  ]
}

"replace_net_tracks": strips existing track segments (not vias) of that net on
the item's layer before adding the trunk — used to supersede autorouted copper.
Exit 0 only when final DRC reports 0 violations / 0 unconnected.
"""
import json
import os
import subprocess
import sys

import pcbnew

DEFAULT_KICAD_CLI = r"C:\Program Files\KiCad\10.0\bin\kicad-cli.exe"


def mm(v):
    return pcbnew.FromMM(float(v))


def main(plan_path):
    plan = json.load(open(plan_path, encoding="utf-8"))
    board_path = os.path.abspath(plan["board"])
    board = pcbnew.LoadBoard(board_path)

    if plan.get("drop_keepouts"):
        # board-level rule areas are routing-phase scaffolding (they reserve
        # the via-well space from the autorouter); the power copper built here
        # intentionally fills them, so they must not survive to DRC
        n = 0
        for z in list(board.Zones()):
            if z.GetIsRuleArea():
                board.RemoveNative(z)
                n += 1
        print(f"dropped {n} routing keepout rule area(s)")

    replaced_nets = set()
    # Snapshot the zones present at load (a previous run's output). Hold these
    # Python refs for the whole run so SWIG doesn't re-wrap them with fresh
    # id()s — the reason the old id()-based staleness check deleted all but the
    # last same-net/layer pour it added this run. We remove PRE-EXISTING zones
    # (once per net+layer), never zones added this run.
    preexisting_zones = [z for z in board.Zones() if not z.GetIsRuleArea()]
    replaced_zone_keys = set()
    for item in plan["items"]:
        net = board.FindNet(item["net"])
        if net is None:
            raise SystemExit(f"net not found: {item['net']}")
        ncode = net.GetNetCode()

        if item.get("replace_net_copper") and item["net"] not in replaced_nets:
            # strip ALL autorouted copper of this net (both layers + vias) —
            # the pour/trunk being added is the net's copper from now on
            n = 0
            for t in list(board.GetTracks()):
                if t.GetNetCode() == ncode:
                    board.RemoveNative(t)
                    n += 1
            print(f"removed {n} autorouted segments+vias of {item['net']} (all layers)")
            replaced_nets.add(item["net"])
        elif item.get("replace_net_tracks") and (item["net"], item.get("layer")) not in replaced_nets:
            layer = board.GetLayerID(item["layer"])
            n = 0
            for t in list(board.GetTracks()):
                if (t.GetNetCode() == ncode and t.GetClass() == "PCB_TRACK"
                        and t.GetLayer() == layer):
                    board.RemoveNative(t)
                    n += 1
            print(f"removed {n} autorouted segments of {item['net']} on {item['layer']}")
            replaced_nets.add((item["net"], item.get("layer")))

        if item["type"] == "pour":
            # idempotency: remove a previous run's pours for this net+layer the
            # first time we pour that net+layer this run (so multiple pours of
            # the same net+layer this run — e.g. 7 EP patches — all survive)
            lay_id = board.GetLayerID(item["layer"])
            key = (ncode, lay_id)
            if key not in replaced_zone_keys:
                stale = [z for z in preexisting_zones
                         if z.GetNetCode() == ncode and z.GetLayer() == lay_id]
                for z in stale:
                    board.RemoveNative(z)
                if stale:
                    print(f"  removed {len(stale)} stale {item['net']} zone(s) on {item['layer']}")
                replaced_zone_keys.add(key)
            zone = pcbnew.ZONE(board)
            zone.SetLayer(lay_id)
            zone.SetNetCode(ncode)
            zone.SetAssignedPriority(int(item.get("priority", 1)))
            conn = item.get("connection", "solid")
            zone.SetPadConnection(pcbnew.ZONE_CONNECTION_FULL if conn == "solid"
                                  else pcbnew.ZONE_CONNECTION_THERMAL)
            zone.SetMinThickness(mm(item.get("min_width", 0.5)))
            ol = zone.Outline()
            ol.NewOutline()
            for x, y in item["polygon"]:
                ol.Append(mm(x), mm(y))
            board.Add(zone)
            print(f"pour: {item['net']} on {item['layer']} "
                  f"({len(item['polygon'])} pts, prio {item.get('priority', 1)})")

        elif item["type"] == "trunk":
            layer = board.GetLayerID(item["layer"])
            pts = item["points"]
            clearance = plan.get("clearance", 0.25)
            guard = mm(item["width"] / 2 + clearance)
            # displace_nets: nets with full zones on this layer may have their
            # blocking SEGMENTS removed (the zone re-establishes connectivity;
            # final DRC validates). Pads are never displaced.
            displace = set(plan.get("displace_nets", []))
            if displace:
                import math as _math
                doomed = {}
                for a, b in zip(pts, pts[1:]):
                    seglen = _math.hypot(b[0] - a[0], b[1] - a[1])
                    steps = max(2, int(seglen / 0.5))
                    for i in range(steps + 1):
                        sx = a[0] + (b[0] - a[0]) * i / steps
                        sy = a[1] + (b[1] - a[1]) * i / steps
                        pt = pcbnew.VECTOR2I(mm(sx), mm(sy))
                        for t in board.GetTracks():
                            if (t.GetClass() == "PCB_TRACK" and t.GetLayer() == layer
                                    and t.GetNetname() in displace
                                    and t.HitTest(pt, guard)):
                                doomed[id(t)] = t
                for t in doomed.values():
                    board.RemoveNative(t)
                if doomed:
                    print(f"  displaced {len(doomed)} zone-backed segment(s) "
                          f"for {item['net']} trunk")
            blocked_reason = None
            for a, b in zip(pts, pts[1:]):
                # sample along the segment; refuse to cross other-net copper
                import math as _math
                seglen = _math.hypot(b[0] - a[0], b[1] - a[1])
                steps = max(2, int(seglen / 0.5))
                for i in range(steps + 1):
                    sx = a[0] + (b[0] - a[0]) * i / steps
                    sy = a[1] + (b[1] - a[1]) * i / steps
                    pt = pcbnew.VECTOR2I(mm(sx), mm(sy))
                    for t in board.GetTracks():
                        if t.GetNetCode() != ncode and t.GetLayer() == layer \
                                and t.HitTest(pt, guard):
                            blocked_reason = (f"({sx:.1f},{sy:.1f}) crosses "
                                              f"{t.GetNetname()} copper")
                            break
                    if blocked_reason is None:
                        for fp in board.GetFootprints():
                            for pad in fp.Pads():
                                if pad.GetNetCode() != ncode and pad.IsOnLayer(layer) \
                                        and pad.HitTest(pt, guard):
                                    blocked_reason = (
                                        f"({sx:.1f},{sy:.1f}) hits pad "
                                        f"{fp.GetReference()}.{pad.GetNumber()}")
                                    break
                            if blocked_reason:
                                break
                    if blocked_reason:
                        break
                if blocked_reason:
                    break
            if blocked_reason:
                if item.get("skip_blocked"):
                    print(f"  SKIPPED trunk {item['net']}: {blocked_reason}")
                    continue
                raise SystemExit(f"trunk {item['net']}: {blocked_reason} — fix plan")
            for a, b in zip(pts, pts[1:]):
                t = pcbnew.PCB_TRACK(board)
                t.SetStart(pcbnew.VECTOR2I(mm(a[0]), mm(a[1])))
                t.SetEnd(pcbnew.VECTOR2I(mm(b[0]), mm(b[1])))
                t.SetWidth(mm(item["width"]))
                t.SetLayer(layer)
                t.SetNetCode(ncode)
                t.SetLocked(True)
                board.Add(t)
            print(f"trunk: {item['net']} {item['width']}mm on {item['layer']} "
                  f"({len(pts) - 1} segments)")

        elif item["type"] == "via_array":
            ax, ay = item["at"]
            pitch = item.get("pitch", 1.2)
            size = item.get("size", 0.9)
            clearance = plan.get("clearance", 0.25)
            skip_blocked = item.get("skip_blocked", False)
            placed = skipped = 0
            for r in range(item.get("rows", 1)):
                for c in range(item.get("cols", 1)):
                    vx, vy = ax + c * pitch, ay + r * pitch
                    guard = mm(size / 2 + clearance)
                    blocker = None
                    for t in board.GetTracks():
                        if t.GetNetCode() != ncode and t.HitTest(
                                pcbnew.VECTOR2I(mm(vx), mm(vy)), guard):
                            blocker = f"{t.GetNetname()} copper"
                            break
                    if not blocker:
                        for fp in board.GetFootprints():
                            for pad in fp.Pads():
                                if pad.GetNetCode() != ncode and pad.HitTest(
                                        pcbnew.VECTOR2I(mm(vx), mm(vy)), guard):
                                    blocker = f"pad {fp.GetReference()}.{pad.GetNumber()}"
                                    break
                            if blocker:
                                break
                    if blocker:
                        if skip_blocked:
                            skipped += 1
                            continue
                        raise SystemExit(
                            f"via_array {item['net']}: ({vx},{vy}) blocked by {blocker}")
                    v = pcbnew.PCB_VIA(board)
                    v.SetPosition(pcbnew.VECTOR2I(mm(vx), mm(vy)))
                    v.SetDrill(mm(item.get("drill", 0.45)))
                    v.SetWidth(mm(size))
                    v.SetNetCode(ncode)
                    v.SetLocked(True)
                    board.Add(v)
                    placed += 1
            msg = f"via array: {item['net']} placed {placed} @ {item['at']}"
            if skipped:
                msg += f" (SKIPPED {skipped} blocked)"
            print(msg)

        else:
            raise SystemExit(f"unknown item type: {item['type']}")

    pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    pcbnew.SaveBoard(board_path, board)

    cli = plan.get("kicad_cli", DEFAULT_KICAD_CLI)
    drc_json = os.path.splitext(board_path)[0] + ".copper_drc.json"
    r = subprocess.run([cli, "pcb", "drc", "--format", "json", "--refill-zones",
                        "--save-board", "-o", drc_json, board_path], capture_output=True)
    if r.returncode != 0:  # kicad-cli refill crash fallback; zones filled in-process
        subprocess.run([cli, "pcb", "drc", "--format", "json", "-o", drc_json,
                        board_path], check=True, capture_output=True)
    d = json.load(open(drc_json, encoding="utf-8"))
    # auto-fix: remove dangling vias (orphaned by displaced/replaced copper)
    for _ in range(2):
        dang = [v for v in d.get("violations", []) if v["type"] == "via_dangling"]
        if not dang:
            break
        board = pcbnew.LoadBoard(board_path)
        removed = 0
        for v in dang:
            pos = v["items"][0].get("pos", {})
            pt = pcbnew.VECTOR2I(mm(pos["x"]), mm(pos["y"]))
            for t in list(board.GetTracks()):
                if t.GetClass() == "PCB_VIA" and t.GetPosition() == pt:
                    board.RemoveNative(t)
                    removed += 1
        print(f"auto-fix: removed {removed} dangling via(s); re-running DRC")
        pcbnew.ZONE_FILLER(board).Fill(board.Zones())
        pcbnew.SaveBoard(board_path, board)
        r = subprocess.run([cli, "pcb", "drc", "--format", "json", "-o", drc_json,
                            board_path], check=True, capture_output=True)
        d = json.load(open(drc_json, encoding="utf-8"))
    # auto-fix: starved thermals -> solid connection on that pad (iterate)
    import re as _re
    for _round in range(4):
        starved = [v for v in d.get("violations", []) if v["type"] == "starved_thermal"]
        if not starved:
            break
        board = pcbnew.LoadBoard(board_path)
        fixed = 0
        for v in starved:
            for it in v.get("items", []):
                m = _re.match(r".*pad (\S+) \[[^\]]*\] of (\S+)", it["description"])
                if not m:
                    continue
                fp = board.FindFootprintByReference(m.group(2))
                if fp:
                    for pad in fp.Pads():
                        if pad.GetNumber() == m.group(1):
                            pad.SetLocalZoneConnection(pcbnew.ZONE_CONNECTION_FULL)
                            fixed += 1
        if not fixed:
            break
        print(f"auto-fix round {_round+1}: {fixed} starved pad(s) -> solid")
        pcbnew.ZONE_FILLER(board).Fill(board.Zones())
        pcbnew.SaveBoard(board_path, board)
        r = subprocess.run([cli, "pcb", "drc", "--format", "json", "-o", drc_json,
                            board_path], check=True, capture_output=True)
        d = json.load(open(drc_json, encoding="utf-8"))
    nviol = len(d.get("violations", []))
    nunc = len(d.get("unconnected_items", []))
    print(f"DRC after copper: violations {nviol}, unconnected {nunc}")
    for v in d.get("violations", [])[:12]:
        print(" -", v["type"], ":", v["description"][:70])
    sys.exit(0 if (nviol == 0 and nunc == 0) else 1)


if __name__ == "__main__":
    main(sys.argv[1])
