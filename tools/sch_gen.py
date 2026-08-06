"""Generate a .kicad_sch from a JSON spec — nets connect by-name via global labels.

No pcbnew/MCP dependency; runs under any python. Mirrors the collision-safe
"terminal per pin" strategy: every net endpoint gets a short stub wire + a
global label, so nets join by name and can never short by crossing geometry.

Spec JSON:
{
  "schematic": "out.kicad_sch",
  "project": "boardname",            # instances project name (file stem default)
  "title": "...", "rev": "A",
  "symbol_dirs": ["C:/Program Files/KiCad/10.0/share/kicad/symbols", ...],
  "components": [
    {"ref": "R1", "lib": "Device:R", "value": "10k",
     "footprint": "Resistor_SMD:R_0603_1608Metric", "dnp": false}, ...
  ],
  "nets": [{"name": "GND", "pins": ["R1.1", "U1.23", ...]}, ...],
  "power_flags": ["GND", "+3V3"],    # nets that get a PWR_FLAG symbol
  "no_connects": ["U1.2", ...]       # explicit NC pins
}

Pins in no net and not in no_connects are auto-NC'd (listed loudly).
--verify runs kicad-cli ERC + netlist export and diffs connectivity vs the spec.

Usage: python sch_gen.py <spec.json> [--verify]
"""
import argparse
import json
import os
import re
import subprocess
import sys
import uuid as uuidlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boardlib import parse_netlist  # noqa: E402

KICAD_CLI = r"C:\Program Files\KiCad\10.0\bin\kicad-cli.exe"
GRID = 1.27
STUB = 2.54


# ---------------------------------------------------------------------------
# Symbol library handling (text-level, with `extends` flattening)
# ---------------------------------------------------------------------------
def _extract_block(text, name):
    """Return the balanced `(symbol "name" ...)` block or None."""
    m = re.search(r'\(symbol "%s"' % re.escape(name), text)
    if not m:
        return None
    depth = 0
    i = m.start()
    while i < len(text):
        c = text[i]
        if c == '"':
            i += 1
            while text[i] != '"' or text[i - 1] == "\\":
                i += 1
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return text[m.start():i + 1]
        i += 1
    return None


def _property_blocks(block):
    """{key: full property block text} for top-level (property ...) entries."""
    out = {}
    for m in re.finditer(r'\(property "([^"]+)"', block):
        sub = _balanced(block, m.start())
        out[m.group(1)] = sub
    return out


def _balanced(text, start):
    depth = 0
    i = start
    while i < len(text):
        c = text[i]
        if c == '"':
            i += 1
            while text[i] != '"' or text[i - 1] == "\\":
                i += 1
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
        i += 1
    raise ValueError("unbalanced")


class SymbolDef:
    def __init__(self, lib_id, block):
        self.lib_id = lib_id          # "Device:R"
        self.name = lib_id.split(":", 1)[1]
        self.block = block            # flattened, outer name = lib_id
        self.pins = self._parse_pins()  # number -> (x, y, rot_deg)  (lib coords, y-up)

    def _parse_pins(self):
        pins = {}
        for m in re.finditer(
                r'\(pin \w+ \w+\s*\(at ([-\d.]+) ([-\d.]+) ([-\d.]+)\)'
                r'.*?\(number "([^"]+)"', self.block, re.S):
            x, y, rot, num = float(m.group(1)), float(m.group(2)), float(m.group(3)), m.group(4)
            pins[num] = (x, y, rot)
        return pins

    def bbox(self):
        if not self.pins:
            return (-2.54, -2.54, 2.54, 2.54)
        xs = [p[0] for p in self.pins.values()]
        ys = [p[1] for p in self.pins.values()]
        return (min(xs) - 2.54, min(ys) - 2.54, max(xs) + 2.54, max(ys) + 2.54)


def load_symbol(dirs, lib_id):
    lib, name = lib_id.split(":", 1)
    for d in dirs:
        path = os.path.join(d, lib + ".kicad_sym")
        if not os.path.isfile(path):
            continue
        text = open(path, encoding="utf-8").read()
        block = _extract_block(text, name)
        if block is None:
            continue
        m = re.search(r'\(extends "([^"]+)"\)', block)
        if m:
            parent = _extract_block(text, m.group(1))
            if parent is None:
                raise SystemExit(f"{lib_id}: parent {m.group(1)} not found")
            flat = parent
            # rename sub-units PARENT_U_S -> NAME_U_S
            flat = re.sub(r'\(symbol "%s_(\d+)_(\d+)"' % re.escape(m.group(1)),
                          r'(symbol "%s_\1_\2"' % name, flat)
            # child property blocks override parent's
            child_props = _property_blocks(block)
            parent_props = _property_blocks(flat)
            for key, cblk in child_props.items():
                if key in parent_props:
                    flat = flat.replace(parent_props[key], cblk)
                else:
                    flat = flat[:flat.rindex(")")] + "\t" + cblk + "\n)"
            block = flat
        # outer name -> "LIB:NAME"
        block = re.sub(r'^\(symbol "[^"]+"', '(symbol "%s"' % lib_id, block, count=1)
        # any remaining sub-unit prefix of the original name is already NAME_U_S
        return SymbolDef(lib_id, block)
    raise SystemExit(f"symbol not found: {lib_id}")


PWR_FLAG_DEF = '''(symbol "power:PWR_FLAG"
\t(power)
\t(pin_numbers (hide yes))
\t(pin_names (offset 0) (hide yes))
\t(exclude_from_sim no) (in_bom yes) (on_board yes)
\t(property "Reference" "#FLG" (at 0 1.905 0) (effects (font (size 1.27 1.27)) (hide yes)))
\t(property "Value" "PWR_FLAG" (at 0 3.81 0) (effects (font (size 1.27 1.27))))
\t(property "Footprint" "" (at 0 0 0) (effects (font (size 1.27 1.27)) (hide yes)))
\t(property "Datasheet" "~" (at 0 0 0) (effects (font (size 1.27 1.27)) (hide yes)))
\t(property "Description" "Special symbol for telling ERC where power comes from" (at 0 0 0) (effects (font (size 1.27 1.27)) (hide yes)))
\t(symbol "PWR_FLAG_0_0"
\t\t(pin power_out line (at 0 0 90) (length 0)
\t\t\t(name "pwr" (effects (font (size 1.27 1.27))))
\t\t\t(number "1" (effects (font (size 1.27 1.27))))
\t\t)
\t)
\t(symbol "PWR_FLAG_0_1"
\t\t(polyline (pts (xy 0 0) (xy 0 1.27) (xy -1.016 1.905) (xy 0 2.54) (xy 1.016 1.905) (xy 0 1.27))
\t\t\t(stroke (width 0) (type default)) (fill (type none))
\t\t)
\t)
)'''


def snap(v):
    return round(v / GRID) * GRID


def uid():
    return str(uuidlib.uuid4())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("spec")
    ap.add_argument("--verify", action="store_true")
    args = ap.parse_args()
    spec = json.load(open(args.spec, encoding="utf-8"))

    out_path = os.path.abspath(spec["schematic"])
    project = spec.get("project", os.path.splitext(os.path.basename(out_path))[0])
    dirs = spec["symbol_dirs"]
    comps = spec["components"]
    nets = spec.get("nets", [])
    power_flags = spec.get("power_flags", [])
    explicit_nc = set(spec.get("no_connects", []))

    root_uuid = uid()

    # load symbol defs
    defs = {}
    for c in comps:
        if c["lib"] not in defs:
            defs[c["lib"]] = load_symbol(dirs, c["lib"])

    # net membership per pin
    pin_net = {}
    for net in nets:
        for p in net["pins"]:
            if p in pin_net:
                raise SystemExit(f"pin {p} in multiple nets ({pin_net[p]}, {net['name']})")
            pin_net[p] = net["name"]

    # ---------------- layout: simple row packing ----------------
    placements = {}  # ref -> (x, y)
    x, y = 30.0, 30.0
    row_h = 0.0
    X_LIMIT = 1100.0
    for c in sorted(comps, key=lambda c: c["ref"]):
        d = defs[c["lib"]]
        bx0, by0, bx1, by1 = d.bbox()
        # budget for stub + global-label text on both sides
        lbl = max((len(pin_net.get(f"{c['ref']}.{n}", "")) for n in d.pins), default=0)
        pad_w = STUB + 2.0 + lbl * 1.3
        w, h = (bx1 - bx0) + 2 * pad_w, (by1 - by0) + 14.0
        if x + w > X_LIMIT:
            x = 30.0
            y += row_h + 8.0
            row_h = 0.0
        # anchor so that symbol origin lands on grid
        placements[c["ref"]] = (snap(x - bx0), snap(y + by1))
        x += w
        row_h = max(row_h, h)

    # ---------------- emit ----------------
    parts = []
    parts.append('(kicad_sch\n\t(version 20250316)\n\t(generator "sch_gen")\n'
                 '\t(generator_version "1.0")\n\t(uuid "%s")\n\t(paper "A0")' % root_uuid)
    if spec.get("title") or spec.get("rev"):
        parts.append('\t(title_block\n\t\t(title "%s")\n\t\t(rev "%s")\n\t)'
                     % (spec.get("title", ""), spec.get("rev", "")))
    parts.append("\t(lib_symbols")
    for d in defs.values():
        parts.append("\t" + d.block.replace("\n", "\n\t"))
    if power_flags:
        parts.append("\t" + PWR_FLAG_DEF.replace("\n", "\n\t"))
    parts.append("\t)")

    wires, labels, ncs, instances = [], [], [], []
    auto_nc = []

    for c in comps:
        ref = c["ref"]
        d = defs[c["lib"]]
        sx, sy = placements[ref]
        pins_sexpr = "".join('\t\t(pin "%s"\n\t\t\t(uuid "%s")\n\t\t)\n' % (n, uid())
                             for n in sorted(d.pins))
        dnp = "yes" if c.get("dnp") else "no"
        # Arbitrary extra fields (LCSC, MPN, ...) from spec.json. Hidden, so they carry
        # through to netlist/BOM tooling without cluttering the drawing.
        extra = "".join(
            '\t\t(property "%s" "%s"\n\t\t\t(at %.2f %.2f 0)\n'
            '\t\t\t(effects (font (size 1.27 1.27)) (hide yes))\n\t\t)\n'
            % (k, v, sx, sy)
            for k, v in sorted(c.get("fields", {}).items()) if v)
        instances.append(
            '\t(symbol\n\t\t(lib_id "%s")\n\t\t(at %.2f %.2f 0)\n\t\t(unit 1)\n'
            '\t\t(exclude_from_sim no)\n\t\t(in_bom yes)\n\t\t(on_board yes)\n'
            '\t\t(dnp %s)\n\t\t(uuid "%s")\n'
            '\t\t(property "Reference" "%s"\n\t\t\t(at %.2f %.2f 0)\n'
            '\t\t\t(effects (font (size 1.27 1.27)))\n\t\t)\n'
            '\t\t(property "Value" "%s"\n\t\t\t(at %.2f %.2f 0)\n'
            '\t\t\t(effects (font (size 1.27 1.27)))\n\t\t)\n'
            '\t\t(property "Footprint" "%s"\n\t\t\t(at %.2f %.2f 0)\n'
            '\t\t\t(effects (font (size 1.27 1.27)) (hide yes))\n\t\t)\n'
            '\t\t(property "Datasheet" "~"\n\t\t\t(at %.2f %.2f 0)\n'
            '\t\t\t(effects (font (size 1.27 1.27)) (hide yes))\n\t\t)\n'
            '%s'
            '%s'
            '\t\t(instances\n\t\t\t(project "%s"\n\t\t\t\t(path "/%s"\n'
            '\t\t\t\t\t(reference "%s") (unit 1)\n\t\t\t\t)\n\t\t\t)\n\t\t)\n\t)'
            % (c["lib"], sx, sy, dnp, uid(),
               ref, sx, sy - 2.0, c.get("value", ""), sx, sy + 2.0,
               c.get("footprint", ""), sx, sy, sx, sy,
               extra, pins_sexpr, project, root_uuid, ref))

        for num, (px, py, prot) in d.pins.items():
            # schematic pin position: y flips
            gx, gy = sx + px, sy - py
            key = f"{ref}.{num}"
            # outward direction (away from body) in schematic coords
            out_dir = {0: (-1, 0), 180: (1, 0), 90: (0, 1), 270: (0, -1)}.get(prot % 360, (-1, 0))
            if key in pin_net:
                ex, ey = gx + out_dir[0] * STUB, gy + out_dir[1] * STUB
                wires.append(
                    '\t(wire\n\t\t(pts\n\t\t\t(xy %.2f %.2f) (xy %.2f %.2f)\n\t\t)\n'
                    '\t\t(stroke (width 0) (type default))\n\t\t(uuid "%s")\n\t)'
                    % (gx, gy, ex, ey, uid()))
                lrot, just = ((0, "left") if out_dir == (1, 0) else
                              (180, "right") if out_dir == (-1, 0) else
                              (90, "left") if out_dir == (0, -1) else (270, "left"))
                labels.append(
                    '\t(global_label "%s"\n\t\t(shape bidirectional)\n'
                    '\t\t(at %.2f %.2f %d)\n'
                    '\t\t(effects (font (size 1.27 1.27)) (justify %s))\n'
                    '\t\t(uuid "%s")\n\t)'
                    % (pin_net[key], ex, ey, lrot, just, uid()))
            elif key in explicit_nc:
                ncs.append('\t(no_connect\n\t\t(at %.2f %.2f)\n\t\t(uuid "%s")\n\t)'
                           % (gx, gy, uid()))
            else:
                auto_nc.append(key)
                ncs.append('\t(no_connect\n\t\t(at %.2f %.2f)\n\t\t(uuid "%s")\n\t)'
                           % (gx, gy, uid()))

    # PWR_FLAG per flagged net, parked in a dedicated row
    fx, fy = 30.0, y + row_h + 20.0
    for i, net in enumerate(power_flags):
        px, py = snap(fx + i * 25.4), snap(fy)
        instances.append(
            '\t(symbol\n\t\t(lib_id "power:PWR_FLAG")\n\t\t(at %.2f %.2f 0)\n\t\t(unit 1)\n'
            '\t\t(exclude_from_sim no)\n\t\t(in_bom yes)\n\t\t(on_board yes)\n\t\t(dnp no)\n'
            '\t\t(uuid "%s")\n'
            '\t\t(property "Reference" "#FLG%02d"\n\t\t\t(at %.2f %.2f 0)\n'
            '\t\t\t(effects (font (size 1.27 1.27)) (hide yes))\n\t\t)\n'
            '\t\t(property "Value" "PWR_FLAG"\n\t\t\t(at %.2f %.2f 0)\n'
            '\t\t\t(effects (font (size 1.27 1.27)))\n\t\t)\n'
            '\t\t(property "Footprint" ""\n\t\t\t(at %.2f %.2f 0)\n'
            '\t\t\t(effects (font (size 1.27 1.27)) (hide yes))\n\t\t)\n'
            '\t\t(property "Datasheet" "~"\n\t\t\t(at %.2f %.2f 0)\n'
            '\t\t\t(effects (font (size 1.27 1.27)) (hide yes))\n\t\t)\n'
            '\t\t(pin "1"\n\t\t\t(uuid "%s")\n\t\t)\n'
            '\t\t(instances\n\t\t\t(project "%s"\n\t\t\t\t(path "/%s"\n'
            '\t\t\t\t\t(reference "#FLG%02d") (unit 1)\n\t\t\t\t)\n\t\t\t)\n\t\t)\n\t)'
            % (px, py, uid(), i + 1, px, py - 4, px, py - 6, px, py, px, py,
               uid(), project, root_uuid, i + 1))
        labels.append(
            '\t(global_label "%s"\n\t\t(shape bidirectional)\n\t\t(at %.2f %.2f 0)\n'
            '\t\t(effects (font (size 1.27 1.27)) (justify left))\n\t\t(uuid "%s")\n\t)'
            % (net, px, py, uid()))

    parts += instances + wires + labels + ncs
    parts.append('\t(sheet_instances\n\t\t(path "/"\n\t\t\t(page "1")\n\t\t)\n\t)')
    parts.append('\t(embedded_fonts no)\n)')

    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    open(out_path, "w", encoding="utf-8", newline="\n").write("\n".join(parts))
    print(f"wrote {out_path}: {len(comps)} components, {len(nets)} nets, "
          f"{len(power_flags)} PWR_FLAGs")
    if auto_nc:
        print(f"AUTO-NC ({len(auto_nc)}): {', '.join(auto_nc)}")

    if args.verify:
        rep = out_path + ".erc.json"
        r = subprocess.run([KICAD_CLI, "sch", "erc", "--format", "json",
                            "--severity-error", "-o", rep, out_path],
                           capture_output=True, text=True)
        try:
            d = json.load(open(rep, encoding="utf-8"))
            nerr = sum(len(s.get("violations", [])) for s in d.get("sheets", []))
            print(f"ERC errors: {nerr}")
            for s in d.get("sheets", []):
                for v in s.get("violations", [])[:8]:
                    print("  -", v.get("type"), ":", v.get("description", "")[:70])
        except Exception:
            print("ERC run problem:", r.stdout[-300:], r.stderr[-300:])
            nerr = -1
        netf = out_path + ".net"
        subprocess.run([KICAD_CLI, "sch", "export", "netlist", "--format", "kicadsexpr",
                        "-o", netf, out_path], check=True, capture_output=True)
        _c, gen_nets = parse_netlist(netf)
        want = {}
        for net in nets:
            want[net["name"]] = sorted((r_, p_) for r_, p_ in
                                       (pp.split(".", 1) for pp in net["pins"]))
        got = {}
        for name, nodes in gen_nets.items():
            if len(nodes) < 2 and name not in want:
                continue  # single-pin autogenerated nets
            got[name] = sorted(set(nodes))
        ok = True
        for name, pins in want.items():
            g = got.get(name)
            if g != pins:
                ok = False
                print(f"NET MISMATCH {name}:\n  want {pins}\n  got  {g}")
        extra = set(got) - set(want)
        if extra:
            print("EXTRA NETS:", sorted(extra))
            ok = False
        print("netlist round-trip:", "MATCH" if ok else "MISMATCH")
        sys.exit(0 if ok and nerr == 0 else 1)


if __name__ == "__main__":
    main()
