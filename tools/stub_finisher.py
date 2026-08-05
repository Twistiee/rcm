"""Finish short leftover unconnected pairs with L-trunks via copper_builder.

Reads a KiCad DRC json (copper_builder's output), takes each unconnected pair
whose endpoints are <= max_len apart, and emits a copper_builder plan with one
skip_blocked L-trunk per pair (elbow at (x1,y2) on pass 1, (x2,y1) on pass 2 —
run again after a rebuild to try the other orientation for survivors). Skips
zone-only pairs (pour islands: fix the pour, not the ratsnest).

Usage: python stub_finisher.py <drc.json> <board.kicad_pcb> <out_plan.json>
           [--orientation 1|2] [--max-len 8] [--layer F.Cu] [--width 0.3]
"""
import argparse
import json
import re

ap = argparse.ArgumentParser()
ap.add_argument("drc_json")
ap.add_argument("board")
ap.add_argument("out_plan")
ap.add_argument("--orientation", type=int, default=1)
ap.add_argument("--max-len", type=float, default=8.0)
ap.add_argument("--layer", default="F.Cu")
ap.add_argument("--width", type=float, default=0.3)
args = ap.parse_args()

d = json.load(open(args.drc_json, encoding="utf-8"))
items = []
for u in d.get("unconnected_items", []):
    a, b = u["items"][0], u["items"][1]
    if "Zone" in a["description"] and "Zone" in b["description"]:
        continue
    net_m = re.search(r"\[([^\]]+)\]", a["description"])
    if not net_m:
        continue
    x1, y1 = a["pos"]["x"], a["pos"]["y"]
    x2, y2 = b["pos"]["x"], b["pos"]["y"]
    if abs(x2 - x1) + abs(y2 - y1) > args.max_len:
        continue
    elbow = [x1, y2] if args.orientation == 1 else [x2, y1]
    pts = [[x1, y1], elbow, [x2, y2]]
    pts = [p for i, p in enumerate(pts) if i == 0 or p != pts[i - 1]]
    items.append({"type": "trunk", "net": net_m.group(1), "layer": args.layer,
                  "width": args.width, "skip_blocked": True, "points": pts})

plan = {"board": args.board.replace("\\", "/"), "clearance": 0.25,
        "displace_nets": ["GND"], "items": items}
json.dump(plan, open(args.out_plan, "w", encoding="utf-8"), indent=1)
print(f"{args.out_plan}: {len(items)} stub trunk(s)")
