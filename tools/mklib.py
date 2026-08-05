"""Rebuild .kicad_sym symbol libraries from the embedded lib_symbols in a KiCad
project's schematic sheets.

Usage:  python mklib.py <project_dir> [lib_name ...]

Writes one <lib_name>.kicad_sym into <project_dir> for each library prefix found
in the embedded defs (or only the ones named on the command line). Registering
the result in sym-lib-table is up to the caller — and note KiCad only reads
project lib tables at PROJECT OPEN (close/reopen, File->Revert is not enough).

Why this exists (pdm/synapse_pdm, 2026-07): EasyEDA->KiCad imports embed every
symbol in the .kicad_sch files but may leave the referenced .kicad_sym missing or
unloadable, making ERC flag every symbol instance with lib_symbol_issues. The
embedded defs are the render/netlist ground truth, so the library is rebuilt FROM
them. Gotchas encoded here:
  - Same-name defs with different bodies across sheets (variants): the variant
    with the most placed instances wins; losers are printed — rename them to
    their own symbol name (def + instance lib_ids) if ERC flags the mismatch.
  - Unprefixed defs (KiCad's lib_name variant cache, e.g. "10nF_1") are skipped;
    promote them to prefixed first-class defs before running this.
  - Child unit names must track the parent stem ("100nF_0_1" under "100nF") or
    KiCad refuses to load the library.
  - Re-run after ANY edit to embedded defs (even property changes) or every
    instance mismatches against the stale library.
"""
import re, glob, os, sys, collections
sys.stdout.reconfigure(encoding="utf-8", errors="replace")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from schlib import blocks_with_span

HEADER = ('(kicad_symbol_lib\n\t(version 20251024)\n'
          '\t(generator "kicad_symbol_editor")\n\t(generator_version "10.0")\n')

def embedded_defs(path):
    t = open(path, encoding="utf-8").read()
    m = re.search(r'\(lib_symbols', t)
    blks = blocks_with_span(t, "lib_symbols", m.start()) if m else []
    if not blks:
        return {}, t
    body = blks[0][2][len("(lib_symbols"):]
    out = {}
    for s0, s1, b in blocks_with_span(body, "symbol"):
        nm = re.match(r'\(symbol\s+"([^"]+)"', b).group(1)
        if ":" in nm:
            out[nm] = b
    return out, t

def instance_count(text, lib_id):
    return len(re.findall(r'\(lib_id "' + re.escape(lib_id) + r'"\)', text))

def collect(project_dir):
    variants = collections.defaultdict(list)  # name -> [(count, sheet, body)]
    for f in sorted(glob.glob(os.path.join(project_dir, "*.kicad_sch"))):
        defs, text = embedded_defs(f)
        for nm, b in defs.items():
            variants[nm].append((instance_count(text, nm), os.path.basename(f), b))
    chosen = {}
    for nm, vs in variants.items():
        groups = collections.defaultdict(lambda: [0, [], None])
        for c, s, b in vs:
            g = groups[re.sub(r'\s+', ' ', b)]
            g[0] += c; g[1].append((s, c)); g[2] = b
        ranked = sorted(groups.values(), key=lambda g: -g[0])
        if len(ranked) > 1:
            print(f"CONFLICT {nm}: keeping {ranked[0][0]}-instance variant from "
                  f"{ranked[0][1]}; losers: {[g[1] for g in ranked[1:]]}")
        chosen[nm] = ranked[0][2]
    return chosen

def write_lib(chosen, libname, path):
    out = [HEADER]
    for nm in sorted(n for n in chosen if n.startswith(libname + ":")):
        body = chosen[nm]
        short = nm.split(":", 1)[1]
        body = body.replace(f'(symbol "{nm}"', f'(symbol "{short}"', 1)
        # embedded defs sit at depth 2 (under lib_symbols); a lib file wants depth 1
        lines = body.split("\n")
        lines = [lines[0]] + [l[1:] if l.startswith("\t") else l for l in lines[1:]]
        out.append("\t" + "\n".join(lines) + "\n")
    out.append(")\n")
    open(path, "w", encoding="utf-8", newline="").write("".join(out))
    n = len([x for x in chosen if x.startswith(libname + ":")])
    print(f"wrote {path}: {n} symbols")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    proj = sys.argv[1]
    chosen = collect(proj)
    libs = sys.argv[2:] or sorted({nm.split(":")[0] for nm in chosen})
    for lib in libs:
        write_lib(chosen, lib, os.path.join(proj, lib + ".kicad_sym"))
