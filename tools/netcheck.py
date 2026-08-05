"""Proper net membership check on a netlist export."""
import re, sys
sys.stdout.reconfigure(encoding="utf-8", errors="replace")
from schlib import blocks_with_span

def parse_nets(path):
    text = open(path, encoding="utf-8").read()
    nets = {}
    for st, en, b in blocks_with_span(text, "net"):
        m = re.search(r'\(name "((?:[^"\\]|\\.)*)"\)', b)
        if not m:
            continue
        nodes = re.findall(r'\(node\s+\(ref\s+"([^"]+)"\)\s+\(pin\s+"([^"]+)"\)(?:\s+\(pinfunction\s+"((?:[^"\\]|\\.)*)"\))?', b)
        nets[m.group(1)] = [(r, p, f) for r, p, f in nodes]
    return nets

if __name__ == "__main__":
    nets = parse_nets(sys.argv[1])
    only = sys.argv[3].split(",") if len(sys.argv) > 3 else None
    filt = sys.argv[2].split(",") if len(sys.argv) > 2 else None
    for name in sorted(nets):
        if filt and not any(re.fullmatch(f, name) for f in filt):
            continue
        nodes = nets[name]
        s2 = [f"{r}.{p}" for r, p, f in nodes]
        print(f"{name} ({len(nodes)}): {', '.join(s2[:16])}{' ...' if len(s2) > 16 else ''}")
