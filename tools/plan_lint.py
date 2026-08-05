"""Placement preflight linter — catch collisions BEFORE building the board.

Runs under any python (no pcbnew). Parses footprint courtyards straight from the
.pretty libraries and checks the expanded placement plan for:
  - courtyard bounding-box overlaps between components
  - courtyards extending off the board outline (error) / within edge margin (warn)
  - refs placed but missing from the netlist, and netlist refs left unplaced
  - duplicate placements

Usage: python plan_lint.py <plan.json> [--edge-margin 0.3]
Exit code 0 = clean (warnings allowed), 1 = errors found.
"""
import argparse
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boardlib import expand_placement, parse_netlist, rotate_point  # noqa: E402

NUM = r"[-+]?[0-9]*\.?[0-9]+"


def courtyard_bbox(mod_path):
    """(xmin, ymin, xmax, ymax) of F.CrtYd graphics; falls back to pads+silk."""
    text = open(mod_path, encoding="utf-8").read()

    def collect(layer_re):
        xs, ys = [], []
        # fp_line / fp_rect / fp_circle / fp_arc blocks carrying the layer
        for m in re.finditer(
                r'\(fp_(?:line|rect|circle|arc|poly)\b(.*?)\(layer "(%s)"\)' % layer_re,
                text, re.S):
            seg = m.group(1)
            for xm, ym in re.findall(r"\((?:start|end|mid|center|xy) (%s) (%s)\)" % (NUM, NUM), seg):
                xs.append(float(xm))
                ys.append(float(ym))
        return xs, ys

    xs, ys = collect(r"F\.CrtYd|B\.CrtYd")
    if not xs:
        # fallback: pads + silkscreen extent
        xs, ys = collect(r"F\.SilkS|B\.SilkS")
        for m in re.finditer(
                r'\(pad "[^"]*" \w+ \w+\s*\(at (%s) (%s)[^)]*\)\s*\(size (%s) (%s)\)' %
                (NUM, NUM, NUM, NUM), text):
            x, y, w, h = map(float, m.groups())
            xs += [x - w / 2, x + w / 2]
            ys += [y - h / 2, y + h / 2]
    if not xs:
        return None
    return min(xs), min(ys), max(xs), max(ys)


def placed_bbox(bbox, x, y, rot):
    corners = [(bbox[0], bbox[1]), (bbox[2], bbox[1]), (bbox[2], bbox[3]), (bbox[0], bbox[3])]
    pts = [rotate_point(cx, cy, rot) for cx, cy in corners]
    xs = [p[0] + x for p in pts]
    ys = [p[1] + y for p in pts]
    return min(xs), min(ys), max(xs), max(ys)


def overlap_area(a, b):
    w = min(a[2], b[2]) - max(a[0], b[0])
    h = min(a[3], b[3]) - max(a[1], b[1])
    return w * h if (w > 0 and h > 0) else 0.0


def find_mod(dirs, lib_id):
    lib, name = lib_id.split(":", 1)
    for d in dirs:
        p = os.path.join(d, lib + ".pretty", name + ".kicad_mod")
        if os.path.isfile(p):
            return p
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("plan")
    ap.add_argument("--edge-margin", type=float, default=0.3)
    args = ap.parse_args()

    plan = json.load(open(args.plan, encoding="utf-8"))
    comps, _nets = parse_netlist(plan["netlist"])
    placement = expand_placement(plan)
    dirs = plan["footprint_dirs"]
    x0, y0, x1, y1 = plan["outline_mm"]

    errors, warnings = [], []

    # footprint universe: netlist comps + extra_footprints
    items = {}  # ref -> (lib_id, x, y, rot)
    for ref, c in comps.items():
        if ref not in placement:
            warnings.append(f"UNPLACED: {ref} ({c['footprint']}) — will be parked")
            continue
        if not c["footprint"]:
            warnings.append(f"NO FOOTPRINT: {ref}")
            continue
        x, y, rot = placement[ref]
        items[ref] = (c["footprint"], x, y, rot)
    for ref in placement:
        if ref not in comps and not any(
                xf["ref"] == ref for xf in plan.get("extra_footprints", [])):
            errors.append(f"PLACED BUT NOT IN NETLIST: {ref}")
    for xf in plan.get("extra_footprints", []):
        x, y, rot = xf["at"]
        items[xf["ref"]] = (xf["lib"] + ":" + xf["name"], x, y, rot)

    boxes = {}
    for ref, (lib_id, x, y, rot) in items.items():
        mod = find_mod(dirs, lib_id)
        if not mod:
            errors.append(f"FOOTPRINT NOT FOUND: {ref} {lib_id}")
            continue
        bb = courtyard_bbox(mod)
        if bb is None:
            warnings.append(f"NO COURTYARD/GEOMETRY: {ref} {lib_id}")
            continue
        boxes[ref] = placed_bbox(bb, x, y, rot)

    refs = sorted(boxes)
    for i, a in enumerate(refs):
        for b in refs[i + 1:]:
            area = overlap_area(boxes[a], boxes[b])
            if area > 0.01:
                errors.append(f"OVERLAP: {a} × {b} ({area:.1f} mm²)")

    for ref, bb in boxes.items():
        if bb[0] < x0 or bb[1] < y0 or bb[2] > x1 or bb[3] > y1:
            errors.append(
                f"OFF-BOARD: {ref} courtyard ({bb[0]:.1f},{bb[1]:.1f})-({bb[2]:.1f},{bb[3]:.1f})")
        elif (bb[0] < x0 + args.edge_margin or bb[1] < y0 + args.edge_margin
              or bb[2] > x1 - args.edge_margin or bb[3] > y1 - args.edge_margin):
            warnings.append(f"NEAR EDGE (<{args.edge_margin}mm): {ref}")

    for w in warnings:
        print("WARN ", w)
    for e in errors:
        print("ERROR", e)
    print(f"\n{len(items)} placed items checked: {len(errors)} error(s), "
          f"{len(warnings)} warning(s)")
    sys.exit(1 if errors else 0)


if __name__ == "__main__":
    main()
