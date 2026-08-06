#!/usr/bin/env python3
"""Generate JLCPCB-format BOM and CPL from a KiCad board, driven by a parts map.

Why a map file rather than reading values straight off the board: a schematic value
like "100nF" or "600R@100MHz" is not a purchasable part. JLC's uploader will happily
auto-match it to *something*, and the failure mode is silent -- a ferrite bead matched
to a resistor of the same number, or a 16V cap fitted on a 12V rail. The map lets each
line carry an explicit LCSC number and a comment written for a human reading a reel
label, and it makes the un-pinned lines obvious instead of invisible.

Map file (JSON):
  {
    "exclude_refs":      ["^TP", "^ST", "^H[0-9]"],   # regex, matched against refdes
    "exclude_footprints":["TestPoint", "MountingHole"],
    "parts": {
      "<value>|<footprint>": {"lcsc": "C1337227", "comment": "STM32F446ZET6 LQFP-144"},
      ...
    }
  }
Key is the raw "value|footprint" as it appears on the board; run once and the script
prints every unmatched key ready to paste in.

DNP parts are dropped from the BOM but KEPT in the CPL only if --dnp-in-cpl is given
(JLC ignores CPL lines with no BOM entry, so the default is to drop them from both).

Usage:
  python gen_jlc_bom_cpl.py <board.kicad_pcb> <parts_map.json> --out DIR [--dnp-in-cpl]
"""
import argparse
import csv
import json
import os
import re
import sys

import pcbnew


def mm(v):
    return pcbnew.ToMM(v)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("board")
    ap.add_argument("map")
    ap.add_argument("--out", required=True)
    ap.add_argument("--dnp-in-cpl", action="store_true")
    ap.add_argument("--centroid", action="store_true",
                    help="emit the courtyard CENTRE instead of the footprint anchor. "
                         "JLC's columns are 'Mid X'/'Mid Y' and they mean it; KiCad "
                         "puts a connector's anchor on PIN 1, so without this every "
                         "multi-pin connector lands offset by half its body.")
    a = ap.parse_args()

    cfg = json.load(open(a.map, encoding="utf-8"))
    parts = cfg.get("parts", {})
    ex_refs = [re.compile(p) for p in cfg.get("exclude_refs", [])]
    ex_fps = cfg.get("exclude_footprints", [])

    b = pcbnew.LoadBoard(a.board)
    os.makedirs(a.out, exist_ok=True)

    rotations = cfg.get("rotations", {})
    rows, excluded, dnp, rotfix = [], [], [], []
    for fp in b.GetFootprints():
        ref = fp.GetReference()
        fpid = fp.GetFPID()
        fpname = f"{fpid.GetLibNickname()}:{fpid.GetLibItemName()}"
        val = fp.GetValue()

        if any(r.match(ref) for r in ex_refs) or any(e in fpname for e in ex_fps):
            excluded.append(ref)
            continue
        if fp.IsDNP():
            dnp.append(ref)
            if not a.dnp_in_cpl:
                continue

        pos = fp.GetPosition()
        px, py = mm(pos.x), mm(pos.y)
        if a.centroid:
            cc = fp.GetCourtyard(pcbnew.F_CrtYd).BBox().GetCenter()
            if cc.x or cc.y:
                px, py = mm(cc.x), mm(cc.y)

        # Per-footprint rotation correction. JLC's 3D models do not share KiCad's
        # zero-rotation reference -- a KiCad 1xN pin header at rot 0 runs along +Y,
        # JLC's runs along X, so it renders 90 degrees out. Keyed by regex on the
        # footprint name, from the "rotations" block of the parts map so it can be
        # tuned against JLC's preview without touching this file.
        rot = fp.GetOrientationDegrees() % 360
        for pat, delta in rotations.items():
            if re.search(pat, fpname):
                rot = (rot + delta) % 360
                rotfix.append((ref, pat, delta))
                break

        rows.append({
            "ref": ref, "val": val, "fp": fpname,
            # Y IS NEGATED. KiCad stores board coordinates with Y increasing DOWNWARD
            # from the top-left, but every pick-and-place convention -- including
            # KiCad's own `kicad-cli pcb export pos` -- emits Y increasing upward.
            # Writing the raw value mirrors the whole board about the X axis: the file
            # still looks plausible, every part is still on the board, and assembly is
            # nonsense. Verified against `kicad-cli pcb export pos` (U1 = 24.0,-25.0).
            "x": px, "y": -py,
            "rot": rot,
            "side": "bottom" if fp.IsFlipped() else "top",
            "dnp": fp.IsDNP(),
        })

    # ---- BOM: group by (value, footprint) ----------------------------------
    groups = {}
    for r in rows:
        if r["dnp"]:
            continue
        groups.setdefault((r["val"], r["fp"]), []).append(r["ref"])

    def natkey(s):
        return [int(t) if t.isdigit() else t for t in re.split(r"(\d+)", s)]

    def collapse(refs):
        """Every designator, listed explicitly, comma separated.

        This used to emit ranges (C1,C2,C3,C7 -> C1-C3,C7) because they read better.
        Do not. The uploader has to expand them too, and range parsers commonly only
        handle a SINGLE-letter prefix -- this board has DZ1-DZ14, CO1-CO14, CV1-CV14,
        RD1-RD14, RIP1-RIP14, RG/RP/RS/RI/CS the same. If any of those fail to expand
        on their side, their designator count no longer matches the CPL and the upload
        is rejected with "BOM does not match CPL", which is what happened on
        pdm14-revB. Explicit lists cannot be misread. Longest line here is ~250 chars,
        which is nothing for a CSV field.
        """
        return ",".join(sorted(refs, key=natkey))

    # Merge groups that resolve to the SAME pinned part. Values are often per-instance
    # (OUT1..OUT14 for the output terminals, which are one part number), and JLC's
    # uploader shows one row per line -- 14 rows of an identical part is 14 things to
    # confirm by hand instead of one. Only merges where an LCSC number is pinned, so
    # unpinned lines stay separate and visible.
    merged = {}
    for (val, fpname), refs in groups.items():
        info = parts.get(f"{val}|{fpname}", {})
        lcsc = info.get("lcsc", "")
        key = (lcsc, fpname) if lcsc else (val, fpname)
        # "name" is the short manufacturing label; "comment" carries the engineering
        # rationale, which does not belong in a document a fab operator reads.
        e = merged.setdefault(key, {"comment": info.get("name") or info.get("comment", val),
                                    "fp": fpname, "lcsc": lcsc, "refs": []})
        e["refs"].extend(refs)

    bom_path = os.path.join(a.out, "bom_jlc.csv")
    unmatched = []
    with open(bom_path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["Comment", "Designator", "Footprint", "LCSC Part #"])
        for e in sorted(merged.values(),
                        key=lambda z: natkey(sorted(z["refs"], key=natkey)[0])):
            if not e["lcsc"]:
                unmatched.append((f'{e["comment"]}|{e["fp"]}', len(e["refs"])))
            w.writerow([e["comment"], collapse(e["refs"]),
                        e["fp"].split(":")[-1], e["lcsc"]])
    groups = merged

    # ---- CPL ---------------------------------------------------------------
    cpl_path = os.path.join(a.out, "cpl_jlc.csv")
    with open(cpl_path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["Designator", "Mid X", "Mid Y", "Layer", "Rotation"])
        for r in sorted(rows, key=lambda z: natkey(z["ref"])):
            w.writerow([r["ref"], f'{r["x"]:.4f}', f'{r["y"]:.4f}',
                        r["side"].capitalize(), f'{r["rot"]:.0f}'])

    if rotfix:
        import collections as _c
        by = _c.Counter((pat, d) for _, pat, d in rotfix)
        print("rotation corrections applied:")
        for (pat, d), n in by.items():
            print(f"    {n:3d}x  {pat}  {d:+g} deg")
    print(f"positions: {'courtyard CENTRE' if a.centroid else 'footprint anchor'}")
    print(f"BOM  {bom_path}: {len(groups)} line(s)")
    print(f"CPL  {cpl_path}: {len(rows)} placement(s)")
    print(f"excluded (not purchasable): {len(excluded)} -> "
          f"{', '.join(sorted(set(excluded), key=natkey)[:14])}"
          + (" ..." if len(excluded) > 14 else ""))
    print(f"DNP (dropped from BOM): {', '.join(dnp) if dnp else 'none'}")
    if unmatched:
        print(f"\n!! {len(unmatched)} BOM line(s) with NO LCSC number -- these will be "
              f"auto-matched by JLC, which is where wrong parts come from:")
        for key, n in sorted(unmatched, key=lambda z: -z[1]):
            print(f'    x{n:<3d} "{key}"')
    else:
        print("\nevery BOM line has an LCSC number")


if __name__ == "__main__":
    sys.exit(main())
