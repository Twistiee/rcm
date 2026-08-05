"""Populate a .kicad_pcb from a kicadsexpr netlist + JSON board plan, headlessly.

Run with KiCad's bundled python: "C:\\Program Files\\KiCad\\10.0\\bin\\python.exe"

Usage: python netlist_to_board.py <plan.json>

plan.json:
{
  "netlist": "path/to/project.net",
  "board": "path/to/project.kicad_pcb",
  "outline_mm": [x0, y0, x1, y1],
  "title": "Board title", "rev": "A",
  "footprint_dirs": ["C:/Program Files/KiCad/10.0/share/kicad/footprints"],
  "placement": {"REF": [x_mm, y_mm, rot_deg], ...},
  "extra_footprints": [
    {"ref": "H1", "lib": "MountingHole", "name": "MountingHole_3.2mm_M3",
     "at": [x, y, rot], "net": "GND", "exclude_bom": true}, ...],
  "grid_start": [x, y]   # fallback grid origin for unplaced refs
}

Notes:
- Components whose ref is missing from "placement" are parked on a grid at
  grid_start so nothing is silently lost.
- Net assignment is by (ref, pad-number) from the netlist; pads sharing a
  number (e.g. multi-drill terminal pads) all get the net.
- Prints a per-connector pad-direction report so orientation mistakes are
  visible immediately.
"""
import json
import os
import sys

import pcbnew

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boardlib import expand_placement, parse_netlist  # noqa: E402


def mm(v):
    return pcbnew.FromMM(float(v))


_IO = pcbnew.PCB_IO_KICAD_SEXPR()


def load_footprint(dirs, lib_id):
    lib, name = lib_id.split(":", 1)
    for d in dirs:
        p = os.path.join(d, lib + ".pretty")
        if os.path.isdir(p):
            fp = _IO.FootprintLoad(p, name)
            if fp:
                return fp
    raise SystemExit(f"footprint not found: {lib_id}")


def main(plan_path):
    plan = json.load(open(plan_path, encoding="utf-8"))
    comps, nets = parse_netlist(plan["netlist"])
    # always rebuild from a pristine board so reruns are idempotent
    open(plan["board"], "w", encoding="utf-8").write(
        '(kicad_pcb (version 20240108) (generator "netlist_to_board") '
        '(generator_version "10.0"))\n')
    board = pcbnew.LoadBoard(plan["board"])
    if plan.get("copper_layers"):
        board.SetCopperLayerCount(int(plan["copper_layers"]))

    netinfo = {}
    for name in nets:
        ni = pcbnew.NETINFO_ITEM(board, name)
        board.Add(ni)
        netinfo[name] = ni
    pad2net = {}
    for name, nodes in nets.items():
        for ref, pad in nodes:
            pad2net[(ref, pad)] = name

    placement = expand_placement(plan)
    gx, gy = plan.get("grid_start", [10, 10])
    parked = []
    dirs = plan["footprint_dirs"]

    for ref, c in sorted(comps.items()):
        if not c["footprint"]:
            print(f"SKIP {ref}: no footprint")
            continue
        fp = load_footprint(dirs, c["footprint"])
        fp.SetReference(ref)
        fp.SetValue(c["value"])
        if c["dnp"]:
            fp.SetDNP(True)
        # Library-qualified FPID. FootprintLoad() returns a bare name, which makes
        # KiCad's "Replace footprints with those specified by symbols" think every
        # footprint differs from the schematic and replace all of them.
        lib, fpname = c["footprint"].split(":", 1)
        fp.SetFPID(pcbnew.LIB_ID(lib, fpname))
        # The schematic-symbol link. Without it, "Update PCB from Schematic" matches
        # nothing and adds a duplicate of every footprint (see parse_netlist docstring).
        if c.get("tstamp"):
            fp.SetPath(pcbnew.KIID_PATH("/" + c["tstamp"]))
        board.Add(fp)
        if ref in placement:
            x, y, rot = placement[ref]
        else:
            x, y, rot = gx, gy, 0
            gx += 6
            if gx > 190:
                gx, gy = plan.get("grid_start", [10, 10])[0], gy + 6
            parked.append(ref)
        fp.SetPosition(pcbnew.VECTOR2I(mm(x), mm(y)))
        fp.SetOrientationDegrees(rot)
        for pad in fp.Pads():
            key = (ref, pad.GetNumber())
            if key in pad2net:
                pad.SetNet(netinfo[pad2net[key]])
        # unnumbered pads (EP paste/thermal sub-pads) inherit the net of the
        # nearest numbered pad — otherwise they read as foreign copper
        numbered = [p_ for p_ in fp.Pads() if p_.GetNumber() and p_.GetNetCode()]
        for pad in fp.Pads():
            if not pad.GetNumber() and numbered:
                near = min(numbered, key=lambda q: (q.GetPosition() - pad.GetPosition()).EuclideanNorm())
                if (near.GetPosition() - pad.GetPosition()).EuclideanNorm() < pcbnew.FromMM(3):
                    pad.SetNet(near.GetNet())

    for xf in plan.get("extra_footprints", []):
        fp = load_footprint(dirs, xf["lib"] + ":" + xf["name"])
        fp.SetReference(xf["ref"])
        fp.SetValue(xf.get("value", xf["name"]))
        fp.SetFPID(pcbnew.LIB_ID(xf["lib"], xf["name"]))
        # no (path): these are board-only (mounting holes, fiducials) with no symbol.
        # Keep "Delete footprints with no symbols" UNCHECKED when syncing, or they go.
        board.Add(fp)
        x, y, rot = xf["at"]
        fp.SetPosition(pcbnew.VECTOR2I(mm(x), mm(y)))
        fp.SetOrientationDegrees(rot)
        if xf.get("exclude_bom"):
            fp.SetAttributes(fp.GetAttributes() | pcbnew.FP_EXCLUDE_FROM_BOM
                             | pcbnew.FP_EXCLUDE_FROM_POS_FILES)
        if xf.get("hide_ref"):
            fp.Reference().SetVisible(False)
        if xf.get("net") and xf["net"] in netinfo:
            for pad in fp.Pads():
                pad.SetNet(netinfo[xf["net"]])

    x0, y0, x1, y1 = plan["outline_mm"]
    r = plan.get("corner_radius", 3.0)  # radiused corners: handling-safety default

    def edge_seg(ax, ay, bx, by):
        sh = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_SEGMENT)
        sh.SetStart(pcbnew.VECTOR2I(mm(ax), mm(ay)))
        sh.SetEnd(pcbnew.VECTOR2I(mm(bx), mm(by)))
        sh.SetLayer(pcbnew.Edge_Cuts)
        sh.SetWidth(mm(0.1))
        board.Add(sh)

    def edge_arc(sx, sy, mx, my, ex, ey):
        sh = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_ARC)
        sh.SetArcGeometry(pcbnew.VECTOR2I(mm(sx), mm(sy)),
                          pcbnew.VECTOR2I(mm(mx), mm(my)),
                          pcbnew.VECTOR2I(mm(ex), mm(ey)))
        sh.SetLayer(pcbnew.Edge_Cuts)
        sh.SetWidth(mm(0.1))
        board.Add(sh)

    if r <= 0:
        rect = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_RECT)
        rect.SetStart(pcbnew.VECTOR2I(mm(x0), mm(y0)))
        rect.SetEnd(pcbnew.VECTOR2I(mm(x1), mm(y1)))
        rect.SetLayer(pcbnew.Edge_Cuts)
        rect.SetWidth(mm(0.1))
        board.Add(rect)
    else:
        k = r * (1 - 0.7071067811865476)  # r - r/sqrt(2), arc midpoint offset
        edge_seg(x0 + r, y0, x1 - r, y0)  # top
        edge_seg(x1, y0 + r, x1, y1 - r)  # right
        edge_seg(x1 - r, y1, x0 + r, y1)  # bottom
        edge_seg(x0, y1 - r, x0, y0 + r)  # left
        edge_arc(x1 - r, y0, x1 - k, y0 + k, x1, y0 + r)  # top-right
        edge_arc(x1, y1 - r, x1 - k, y1 - k, x1 - r, y1)  # bottom-right
        edge_arc(x0 + r, y1, x0 + k, y1 - k, x0, y1 - r)  # bottom-left
        edge_arc(x0, y0 + r, x0 + k, y0 + k, x0 + r, y0)  # top-left

    # routing reservations: rule areas that block tracks/vias but allow the
    # later power pour to fill inside them (power corridors reserved pre-route)
    for ko in plan.get("keepouts", []):
        z = pcbnew.ZONE(board)
        z.SetIsRuleArea(True)
        z.SetDoNotAllowTracks(bool(ko.get("no_tracks", True)))
        z.SetDoNotAllowVias(bool(ko.get("no_vias", True)))
        z.SetDoNotAllowZoneFills(False)
        z.SetDoNotAllowPads(False)
        z.SetDoNotAllowFootprints(False)
        z.SetLayer(board.GetLayerID(ko["layer"]))
        ol = z.Outline()
        ol.NewOutline()
        for x, y in ko["polygon"]:
            ol.Append(mm(x), mm(y))
        board.Add(z)
    if plan.get("keepouts"):
        print(f"added {len(plan['keepouts'])} routing keepout(s)")

    tb = board.GetTitleBlock()
    if plan.get("title"):
        tb.SetTitle(plan["title"])
    if plan.get("rev"):
        tb.SetRevision(plan["rev"])

    pcbnew.SaveBoard(plan["board"], board)

    print(f"placed {len(comps)} components, {len(nets)} nets")
    if parked:
        print("PARKED (no placement):", ", ".join(parked))
    # orientation report for multi-pad through-hole connectors
    for fp in board.GetFootprints():
        pads = {p.GetNumber(): p.GetPosition() for p in fp.Pads()}
        if "1" in pads and fp.GetReference().startswith("J"):
            last = "4" if "4" in pads else ("2" if "2" in pads else None)
            if last:
                d = pads[last] - pads["1"]
                print(f"{fp.GetReference()}: pad1=({pcbnew.ToMM(pads['1'].x):.2f},"
                      f"{pcbnew.ToMM(pads['1'].y):.2f}) pad1->pad{last}="
                      f"({pcbnew.ToMM(d.x):.2f},{pcbnew.ToMM(d.y):.2f})")
        bb = fp.GetBoundingBox(False)
        offb = (pcbnew.ToMM(bb.GetLeft()) < x0 - 3 or pcbnew.ToMM(bb.GetRight()) > x1 + 3
                or pcbnew.ToMM(bb.GetTop()) < y0 - 3 or pcbnew.ToMM(bb.GetBottom()) > y1 + 3)
        if offb:
            print(f"WARN off-board: {fp.GetReference()} bbox "
                  f"({pcbnew.ToMM(bb.GetLeft()):.1f},{pcbnew.ToMM(bb.GetTop()):.1f})-"
                  f"({pcbnew.ToMM(bb.GetRight()):.1f},{pcbnew.ToMM(bb.GetBottom()):.1f})")


if __name__ == "__main__":
    main(sys.argv[1])
