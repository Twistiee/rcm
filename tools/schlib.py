"""Shared s-expression helpers for KiCad 10 schematic surgery (Exocet PDM port)."""
import re

def blocks_with_span(s, tag, start=0, end=None):
    """Yield (start, end_exclusive, text) for each balanced '(tag ...)' block.
    Only matches tag followed by whitespace/newline/quote/paren."""
    end = len(s) if end is None else end
    out = []
    i = start
    pat = "(" + tag
    L = len(pat)
    while True:
        j = s.find(pat, i)
        if j < 0 or j >= end:
            break
        k = j + L
        if k < len(s) and s[k] not in " \t\r\n(\"":
            i = j + 1
            continue
        depth = 0
        p = j
        inq = False
        while p < len(s):
            c = s[p]
            if inq:
                if c == '"' and s[p-1] != '\\':
                    inq = False
            else:
                if c == '"':
                    inq = True
                elif c == '(':
                    depth += 1
                elif c == ')':
                    depth -= 1
                    if depth == 0:
                        break
            p += 1
        out.append((j, p + 1, s[j:p+1]))
        i = p + 1
    return out

def prop(block, name):
    m = re.search(r'\(property "' + re.escape(name) + r'" "((?:[^"\\]|\\.)*)"', block)
    return m.group(1) if m else None

def at_of(block):
    """First (at x y [rot]) in block."""
    m = re.search(r'\(at (-?[\d.]+) (-?[\d.]+)(?: (-?[\d.]+))?\)', block)
    if not m:
        return None
    return (float(m.group(1)), float(m.group(2)), float(m.group(3) or 0))

def parse_sheet(path):
    """Parse a .kicad_sch into item lists with spans (for splicing)."""
    text = open(path, encoding="utf-8", newline="") .read()
    # lib_symbols block (first one)
    libspan = blocks_with_span(text, "lib_symbols")[0]
    libsyms = {}
    for st, en, b in blocks_with_span(text, "symbol", libspan[0] + 1, libspan[1]):
        m = re.match(r'\(symbol "([^"]+)"', b)
        if m and ":" in m.group(1):
            libsyms[m.group(1)] = b
    # symbol instances: those after lib_symbols end
    instances = []
    for st, en, b in blocks_with_span(text, "symbol", libspan[1]):
        m = re.search(r'\(lib_id "([^"]+)"\)', b[:200])
        if not m:
            continue
        instances.append({"span": (st, en), "lib_id": m.group(1),
                          "ref": prop(b, "Reference"), "at": at_of(b), "text": b})
    wires = [{"span": (st, en), "pts": [(float(a), float(b_)) for a, b_ in
              re.findall(r'\(xy (-?[\d.]+) (-?[\d.]+)\)', b)], "text": b}
             for st, en, b in blocks_with_span(text, "wire", libspan[1])]
    def simple(tag):
        return [{"span": (st, en), "at": at_of(b), "text": b}
                for st, en, b in blocks_with_span(text, tag, libspan[1])]
    labels = [{"span": (st, en), "name": re.match(r'\(global_label "((?:[^"\\]|\\.)*)"', b).group(1),
               "at": at_of(b), "text": b}
              for st, en, b in blocks_with_span(text, "global_label", libspan[1])]
    junctions = simple("junction")
    no_connects = simple("no_connect")
    texts = simple("text")
    return {"text": text, "libspan": libspan, "libsyms": libsyms, "instances": instances,
            "wires": wires, "labels": labels, "junctions": junctions,
            "no_connects": no_connects, "texts": texts}

def lib_pins(libsym_text):
    """Parse pins from a lib symbol def: list of (number, name, x, y, rot)."""
    pins = []
    for st, en, b in blocks_with_span(libsym_text, "pin"):
        a = at_of(b)
        num = re.search(r'\(number "([^"]+)"', b)
        nam = re.search(r'\(name "((?:[^"\\]|\\.)*)"', b)
        if a and num:
            pins.append((num.group(1), nam.group(1) if nam else "", a[0], a[1], a[2]))
    return pins

def pin_pos(inst_at, pin_xy, mirror=None):
    """Sheet position of a symbol pin. inst_at=(X,Y,rot); pin_xy=(px,py) from lib def.
    KiCad: symbol-space +y is up, sheet +y is down."""
    X, Y, rot = inst_at
    px, py = pin_xy
    if mirror == "x":
        py = -py
    elif mirror == "y":
        px = -px
    import math
    th = math.radians(rot)
    # rotate in symbol space (ccw), then flip y to sheet space
    rx = px * math.cos(th) - py * math.sin(th)
    ry = px * math.sin(th) + py * math.cos(th)
    return (round(X + rx, 4), round(Y - ry, 4))
