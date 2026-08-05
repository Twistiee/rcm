"""Shared helpers for the scripted-board pipeline (no pcbnew dependency).

Used by netlist_to_board.py (KiCad python) and plan_lint.py (any python).
"""
import math
import re

BS = chr(92)


# ---------------------------------------------------------------------------
# kicadsexpr netlist parsing
# ---------------------------------------------------------------------------
def _blocks(text, kw):
    out = []
    for m in re.finditer(r"\(" + kw + r"\s", text):
        depth = 0
        i = m.start()
        while i < len(text):
            c = text[i]
            if c == '"':
                i += 1
                while text[i] != '"' or text[i - 1] == BS:
                    i += 1
            elif c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
                if depth == 0:
                    out.append(text[m.start():i + 1])
                    break
            i += 1
    return out


def parse_netlist(path):
    """Return (components, nets):
    components: ref -> {value, footprint, dnp, tstamp}
    nets: netname -> [(ref, pad), ...]

    `tstamp` is the schematic symbol's UUID. netlist_to_board writes it into each
    footprint as `(path "/<uuid>")`, which is the ONLY thing KiCad's "Update PCB from
    Schematic" matches on. Boards generated before this was captured have no path at
    all, so a sync matches nothing and adds a duplicate footprint for every symbol --
    duplicating the entire board. See the pdm14-revB write-up.
    """
    text = open(path, encoding="utf-8").read()
    comps = {}
    for b in _blocks(text, "comp"):
        ref = re.search(r'\(ref "([^"]+)"\)', b)
        val = re.search(r'\(value "((?:[^"\\]|\\.)*)"\)', b)
        fpr = re.search(r'\(footprint "([^"]+)"\)', b)
        # NB: a comp block contains TWO (tstamps ...) — the sheetpath's, which is
        # just "/", comes FIRST, and the symbol's own UUID second. Match the UUID
        # shape so we can never pick up the sheet one.
        tst = re.search(r'\(tstamps "([0-9a-fA-F]{8}-[0-9a-fA-F-]{27})"\)', b)
        # DNP appears in a KiCad 10 kicadsexpr netlist as a property whose NAME is on
        # its own sub-expression:
        #     (property
        #         (name "dnp")
        #     )
        # so the old literal test for '(property "dnp"' never fired and every board
        # this pipeline generated came out with DNP silently cleared -- on pdm14 that
        # meant R38, the CAN-termination jumper, would have been fitted and terminated
        # the bus on every board. Accept the old spellings too.
        dnp = bool(re.search(r'\(property\s*\(name\s+"dnp"\s*\)', b)
                   or '(property "dnp"' in b
                   or "(dnp yes)" in b)
        if ref:
            comps[ref.group(1)] = {
                "value": val.group(1) if val else "",
                "footprint": fpr.group(1) if fpr else "",
                "dnp": dnp,
                "tstamp": tst.group(1) if tst else "",
            }
    nets = {}
    for b in _blocks(text, "net"):
        m = re.search(r'\(name "((?:[^"\\]|\\.)*)"\)', b)
        if not m:
            continue
        nodes = re.findall(r'\(node\s+\(ref\s+"([^"]+)"\)\s+\(pin\s+"([^"]+)"\)', b)
        if nodes:
            nets[m.group(1)] = nodes
    return comps, nets


# ---------------------------------------------------------------------------
# Placement plan expansion: literals, anchor-relative entries, groups
# ---------------------------------------------------------------------------
def rotate_point(x, y, deg):
    """Rotate a point about the origin using pcbnew's convention
    (SetOrientationDegrees; empirically rot90 maps (1,0)->(0,-1))."""
    t = math.radians(deg)
    c, s = math.cos(t), math.sin(t)
    return (x * c + y * s, -x * s + y * c)


def expand_placement(plan):
    """Resolve plan['placement'] (+ plan['groups']) into {ref: (x, y, rot)}.

    placement entry forms:
      "REF": [x, y, rot]
      "REF": {"rel": "OTHER", "d": [dx, dy], "rot": r}   # offset from OTHER's position
             (dx,dy are board-axis offsets — NOT rotated by OTHER's rotation)
    groups (optional):
      "groups": [{"members": [[dx,dy,rot], ...],
                  "instances": [{"at": [x,y], "refs": [...], "rot": r?}, ...]}]
      members[i] pairs with refs[i]; instance "rot" rotates member offsets and
      adds to member rotation.
    """
    placement = dict(plan.get("placement", {}))
    resolved = {}
    relative = {}
    for ref, entry in placement.items():
        if isinstance(entry, dict):
            relative[ref] = entry
        else:
            x, y, rot = entry
            resolved[ref] = (float(x), float(y), float(rot))

    for group in plan.get("groups", []):
        members = group["members"]
        for inst in group["instances"]:
            ax, ay = inst["at"]
            grot = float(inst.get("rot", 0))
            refs = inst["refs"]
            if len(refs) != len(members):
                raise ValueError(
                    f"group instance at {inst['at']}: {len(refs)} refs vs "
                    f"{len(members)} members")
            for (dx, dy, mrot), ref in zip(members, refs):
                if ref in resolved or ref in relative:
                    raise ValueError(f"duplicate placement for {ref}")
                rx, ry = rotate_point(dx, dy, grot) if grot else (dx, dy)
                resolved[ref] = (ax + rx, ay + ry, (mrot + grot) % 360)

    # resolve anchor-relative entries (allow chains, detect cycles)
    for _ in range(len(relative) + 1):
        progressed = False
        for ref, entry in list(relative.items()):
            base = entry["rel"]
            if base in resolved:
                bx, by, _brot = resolved[base]
                dx, dy = entry["d"]
                resolved[ref] = (bx + dx, by + dy, float(entry.get("rot", 0)))
                del relative[ref]
                progressed = True
        if not relative:
            break
        if not progressed:
            raise ValueError(
                "unresolvable relative placements (missing anchor or cycle): "
                + ", ".join(sorted(relative)))
    return resolved
