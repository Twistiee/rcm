#!/usr/bin/env python3
"""Resolve starved_thermal DRC errors by making those pads solid-connected.

A starved thermal means the pad reached the zone with fewer spokes than the
rule demands -- the net is connected (DRC reports it separately from
unconnected), but through a single narrow neck. For a ground pin on a reflowed
IC the thermal relief is buying nothing (its purpose is hand-solder/rework
heat isolation), so a solid connection is both the fix and the better design:
lower inductance, more copper.

Which pads get starved shifts with every re-route, so this reads the DRC report
rather than hard-coding pad numbers -- it stays correct as the board changes.

Usage:
  python fix_starved_thermals.py <board.kicad_pcb> <drc.json> [--dry-run]
      [--only-nets GND,+3V3]   restrict to these nets (default: all)

Re-runnable: setting an already-solid pad solid is a no-op. Refills zones and
saves. Re-run DRC afterwards to confirm.
"""
import argparse
import json
import re
import sys

import pcbnew

# DRC writes "PTH pad 5 [GND] of J_SWD" for through-hole and "Pad 12 [GND] of U1"
# for SMD. Matching only the latter made this silently report "nothing to do"
# while every THT violation remained.
PAD_RE = re.compile(r"(?:PTH |NPTH )?[Pp]ad (\S+) \[([^\]]*)\] of (\S+)")


def starved_pads(report):
    """[(ref, pad_number, net)] from every starved_thermal violation."""
    out = []
    for v in report.get("violations", []):
        if v.get("type") != "starved_thermal":
            continue
        for item in v.get("items", []):
            m = PAD_RE.search(item.get("description", ""))
            if m:
                num, net, ref = m.group(1), m.group(2), m.group(3)
                out.append((ref, num, net))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("board")
    ap.add_argument("drc")
    ap.add_argument("--only-nets", default="")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    report = json.load(open(a.drc, encoding="utf-8"))
    targets = starved_pads(report)
    if not targets:
        print("no starved_thermal violations -- nothing to do")
        return 0

    allow = {n.strip() for n in a.only_nets.split(",") if n.strip()}
    board = pcbnew.LoadBoard(a.board)
    fixed = 0
    for ref, num, net in targets:
        if allow and net not in allow:
            print(f"  skip {ref}.{num} [{net}] -- not in --only-nets")
            continue
        fp = board.FindFootprintByReference(ref)
        if fp is None:
            print(f"  WARN {ref} not found on board")
            continue
        for p in fp.Pads():
            if p.GetNumber() != num:
                continue
            p.SetLocalZoneConnection(pcbnew.ZONE_CONNECTION_FULL)
            print(f"  {ref}.{num} [{net}] -> solid zone connection")
            fixed += 1

    if a.dry_run:
        print(f"dry run -- {fixed} pad(s) would change")
        return 0

    filler = pcbnew.ZONE_FILLER(board)
    filler.Fill(board.Zones())
    board.Save(a.board)
    print(f"{fixed} pad(s) set solid; zones refilled; saved {a.board}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
