import json, re, math, sys
from collections import defaultdict

# Board path used to be HARDCODED to the SmartSwitchesModule project, so running
# this from any other project silently edited the wrong board. Now it is an
# argument, with the old path as a default only if a single arg is given.
#   python relocate_refs.py <drc.json|reflist.txt> [board.kicad_pcb]
DRC = sys.argv[1]
PCB = (sys.argv[2] if len(sys.argv) > 2
       else r'C:/github/pdm/KiCad_Schematics_SmartSwitchesModule/SmartSwitchesModule.kicad_pcb')
BS = chr(92)

# ---------- minimal s-expr parser ----------
def parse_sexp(s):
    i = 0
    n = len(s)
    def skip_ws(j):
        while j < n and s[j] in ' \t\r\n':
            j += 1
        return j
    def parse(j):
        j = skip_ws(j)
        if s[j] == '(':
            j += 1
            out = []
            while True:
                j = skip_ws(j)
                if s[j] == ')':
                    return out, j + 1
                node, j = parse(j)
                out.append(node)
        elif s[j] == '"':
            k = j + 1
            buf = []
            while True:
                c = s[k]
                if c == BS:
                    buf.append(s[k+1]); k += 2; continue
                if c == '"':
                    break
                buf.append(c); k += 1
            return ('STR', ''.join(buf)), k + 1
        else:
            k = j
            while k < n and s[k] not in ' \t\r\n()':
                k += 1
            return s[j:k], k
    node, _ = parse(0)
    return node

def sval(x):
    return x[1] if isinstance(x, tuple) else x

def find_all(node, tag):
    return [c for c in node if isinstance(c, list) and c and c[0] == tag]

def find_one(node, tag):
    r = find_all(node, tag)
    return r[0] if r else None

src = open(PCB, encoding='utf-8').read()
board = parse_sexp(src)

# ---------- extract footprints ----------
def get_at(node):
    a = find_one(node, 'at')
    if not a:
        return (0.0, 0.0, 0.0)
    x = float(sval(a[1])); y = float(sval(a[2]))
    r = float(sval(a[3])) if len(a) > 3 else 0.0
    return (x, y, r)

def rot_pt(x, y, ang, mode):
    a = math.radians(ang)
    c, si = math.cos(a), math.sin(a)
    if mode == 'A':   # math CCW (y-down -> visually CW)
        return (x*c - y*si, x*si + y*c)
    else:             # mode B: visually CCW with y-down
        return (x*c + y*si, -x*si + y*c)

fps = []
for f in find_all(board, 'footprint'):
    fx, fy, frot = get_at(f)
    layer = sval(find_one(f, 'layer')[1])
    entry = {'x': fx, 'y': fy, 'rot': frot, 'layer': layer,
             'pads': [], 'silk_segs': [], 'silk_boxes': [], 'ref': None}
    for p in find_all(f, 'pad'):
        px, py, pang = get_at(p)
        sz = find_one(p, 'size')
        sx = float(sval(sz[1])); sy = float(sval(sz[2]))
        ptype = sval(p[2])
        players = find_one(p, 'layers')
        lay = [sval(t) for t in players[1:]] if players else []
        entry['pads'].append({'lx': px, 'ly': py, 'ang': pang,
                              'sx': sx, 'sy': sy, 'type': ptype, 'layers': lay})
    for tag in ('fp_line', 'fp_rect', 'fp_circle', 'fp_poly', 'fp_arc'):
        for g in find_all(f, tag):
            gl = find_one(g, 'layer')
            if not gl or 'SilkS' not in sval(gl[1]):
                continue
            w = 0.12
            st = find_one(g, 'stroke')
            if st:
                wn = find_one(st, 'width')
                if wn:
                    w = float(sval(wn[1]))
            side = 'F' if sval(gl[1]).startswith('F') else 'B'
            if tag == 'fp_line':
                s0 = find_one(g, 'start'); e0 = find_one(g, 'end')
                entry['silk_segs'].append((float(sval(s0[1])), float(sval(s0[2])),
                                           float(sval(e0[1])), float(sval(e0[2])), w, side))
            elif tag == 'fp_rect':
                s0 = find_one(g, 'start'); e0 = find_one(g, 'end')
                x1, y1 = float(sval(s0[1])), float(sval(s0[2]))
                x2, y2 = float(sval(e0[1])), float(sval(e0[2]))
                entry['silk_boxes'].append((min(x1,x2)-w/2, min(y1,y2)-w/2,
                                            max(x1,x2)+w/2, max(y1,y2)+w/2, side))
            elif tag == 'fp_circle':
                c0 = find_one(g, 'center'); e0 = find_one(g, 'end')
                cx, cy = float(sval(c0[1])), float(sval(c0[2]))
                ex, ey = float(sval(e0[1])), float(sval(e0[2]))
                r = math.hypot(ex-cx, ey-cy) + w/2
                entry['silk_boxes'].append((cx-r, cy-r, cx+r, cy+r, side))
            elif tag == 'fp_poly':
                pts = find_one(g, 'pts')
                xs = []; ys = []
                for xy in find_all(pts, 'xy'):
                    xs.append(float(sval(xy[1]))); ys.append(float(sval(xy[2])))
                entry['silk_boxes'].append((min(xs)-w/2, min(ys)-w/2,
                                            max(xs)+w/2, max(ys)+w/2, side))
            elif tag == 'fp_arc':
                s0 = find_one(g, 'start'); m0 = find_one(g, 'mid'); e0 = find_one(g, 'end')
                xs = [float(sval(s0[1])), float(sval(m0[1])), float(sval(e0[1]))]
                ys = [float(sval(s0[2])), float(sval(m0[2])), float(sval(e0[2]))]
                entry['silk_boxes'].append((min(xs)-w/2, min(ys)-w/2,
                                            max(xs)+w/2, max(ys)+w/2, side))
    for pr in find_all(f, 'property'):
        if sval(pr[1]) == 'Reference':
            refname = sval(pr[2])
            ax, ay, arot = get_at(pr)
            lay = sval(find_one(pr, 'layer')[1])
            eff = find_one(pr, 'effects')
            font = find_one(eff, 'font')
            szn = find_one(font, 'size')
            tsz = float(sval(szn[1]))
            thn = find_one(font, 'thickness')
            th = float(sval(thn[1])) if thn else 0.15
            hidden = any((isinstance(c, list) and c[0] == 'hide' and sval(c[1]) == 'yes')
                         for c in pr) or 'hide' in [c for c in pr if not isinstance(c, (list, tuple))]
            entry['ref'] = {'name': refname, 'lx': ax, 'ly': ay, 'rot': arot,
                            'size': tsz, 'th': th, 'layer': lay, 'hidden': hidden}
    fps.append(entry)

print(f'parsed {len(fps)} footprints')

# ---------- calibrate rotation convention against DRC-reported ref positions ----------
drc = json.load(open(DRC)) if DRC.endswith('.json') else None
drc_ref_pos = {}
if drc is not None:
    for v in drc['violations']:
        for i in v['items']:
            m = re.match(r'Reference field of (\S+)', i.get('description', ''))
            if m and 'pos' in i:
                drc_ref_pos[m.group(1)] = (i['pos']['x'], i['pos']['y'])

err = {'A': 0.0, 'B': 0.0}
cnt = 0
for fp in fps:
    r = fp['ref']
    if not r or r['name'] not in drc_ref_pos:
        continue
    gx, gy = drc_ref_pos[r['name']]
    for mode in ('A', 'B'):
        dx, dy = rot_pt(r['lx'], r['ly'], fp['rot'], mode)
        err[mode] += math.hypot(fp['x']+dx-gx, fp['y']+dy-gy)
    cnt += 1
print(f'calibration over {cnt} refs: errA={err["A"]:.2f} errB={err["B"]:.2f}')
MODE = ('A' if err['A'] <= err['B'] else 'B') if cnt else 'B'
print('using mode', MODE, f'(calibrated over {cnt})')

# ---------- build obstacle model (per side) ----------
CLR_PAD = 0.18
CLR_SILK = 0.12
CLR_TEXT = 0.12
EDGE = (62.345, 27.3675, 157.345, 177.3675)
EDGE_M = 0.35

grid = defaultdict(list)   # (side, cx, cy) -> list of rects
CELL = 2.0

def add_rect(side, rect, tag):
    x1, y1, x2, y2 = rect
    for cx in range(int(x1 // CELL), int(x2 // CELL) + 1):
        for cy in range(int(y1 // CELL), int(y2 // CELL) + 1):
            grid[(side, cx, cy)].append((rect, tag))

def query(side, rect):
    x1, y1, x2, y2 = rect
    out = []
    for cx in range(int(x1 // CELL), int(x2 // CELL) + 1):
        for cy in range(int(y1 // CELL), int(y2 // CELL) + 1):
            for (r, tag) in grid.get((side, cx, cy), []):
                if not (r[2] < x1 or r[0] > x2 or r[3] < y1 or r[1] > y2):
                    out.append(tag)
    return out

def fp_to_global(fp, lx, ly):
    dx, dy = rot_pt(lx, ly, fp['rot'], MODE)
    return (fp['x'] + dx, fp['y'] + dy)

for fp in fps:
    fside = 'F' if fp['layer'].startswith('F') else 'B'
    for p in fp['pads']:
        gx, gy = fp_to_global(fp, p['lx'], p['ly'])
        a = p['ang'] % 180
        sx, sy = (p['sx'], p['sy']) if a < 45 or a > 135 else (p['sy'], p['sx'])
        hx, hy = sx/2 + CLR_PAD, sy/2 + CLR_PAD
        rect = (gx-hx, gy-hy, gx+hx, gy+hy)
        if p['type'] == 'thru_hole' or p['type'] == 'np_thru_hole':
            add_rect('F', rect, 'pad'); add_rect('B', rect, 'pad')
        else:
            side = 'F' if any(l.startswith('F') for l in p['layers']) else 'B'
            add_rect(side, rect, 'pad')
    for (x1, y1, x2, y2, w, side) in fp['silk_segs']:
        g1 = fp_to_global(fp, x1, y1); g2 = fp_to_global(fp, x2, y2)
        h = w/2 + CLR_SILK
        add_rect(side, (min(g1[0],g2[0])-h, min(g1[1],g2[1])-h,
                        max(g1[0],g2[0])+h, max(g1[1],g2[1])+h), 'silk')
    for (x1, y1, x2, y2, side) in fp['silk_boxes']:
        c1 = fp_to_global(fp, x1, y1); c2 = fp_to_global(fp, x2, y2)
        add_rect(side, (min(c1[0],c2[0])-CLR_SILK, min(c1[1],c2[1])-CLR_SILK,
                        max(c1[0],c2[0])+CLR_SILK, max(c1[1],c2[1])+CLR_SILK), 'silk')

# board-level silk graphics
for tag in ('gr_line', 'gr_rect', 'gr_text'):
    for g in find_all(board, tag):
        gl = find_one(g, 'layer')
        if gl and 'SilkS' in sval(gl[1]):
            print('NOTE: board-level silk item present:', tag)

# text bbox model — KiCad stroke font; underscores/descenders extend below baseline
ADV = 1.02   # advance per char as fraction of size (conservative)
DESC = set('_gjpqy')
def text_rect(cx, cy, name, size, th, trot):
    nchars = len(name)
    w = nchars * size * ADV + th + 0.2
    h = (size * 1.8 if any(c in DESC for c in name) else size * 1.4) + th
    if trot % 180 >= 45:
        w, h = h, w
    return (cx - w/2, cy - h/2, cx + w/2, cy + h/2)

# seed all visible refs as text obstacles
ref_rects = {}
for fp in fps:
    r = fp['ref']
    if not r or r['hidden']:
        continue
    side = 'F' if r['layer'].startswith('F') else 'B'
    gx, gy = fp_to_global(fp, r['lx'], r['ly'])
    disp_rot = r['rot'] % 180   # property angles are ABSOLUTE in file
    rect = text_rect(gx, gy, r['name'], r['size'], r['th'], disp_rot)
    ref_rects[r['name']] = (side, rect)

# refs to fix
fix = set()
if drc is not None:
    for v in drc['violations']:
        if not v['type'].startswith('silk'):
            continue
        for i in v['items']:
            m = re.match(r'Reference field of (\S+)', i.get('description', ''))
            if m:
                fix.add(m.group(1))
else:
    fix = set(x.strip() for x in open(sys.argv[1]) if x.strip())
print(f'{len(fix)} refs to relocate')

# non-moving refs become fixed obstacles
for name, (side, rect) in ref_rects.items():
    if name not in fix:
        x1, y1, x2, y2 = rect
        add_rect(side, (x1-CLR_TEXT, y1-CLR_TEXT, x2+CLR_TEXT, y2+CLR_TEXT), 'text:'+name)

fpmap = {fp['ref']['name']: fp for fp in fps if fp['ref']}

def in_board(rect):
    return (rect[0] >= EDGE[0]+EDGE_M and rect[1] >= EDGE[1]+EDGE_M and
            rect[2] <= EDGE[2]-EDGE_M and rect[3] <= EDGE[3]-EDGE_M)

# candidate offsets: sorted ring
offsets = []
step = 0.25
R = 9.0
k = int(R/step)
for ix in range(-k, k+1):
    for iy in range(-k, k+1):
        dx, dy = ix*step, iy*step
        d = math.hypot(dx, dy)
        if d > R:
            continue
        bonus = -0.35 if (abs(dx) < 0.13 or abs(dy) < 0.13) else 0.0
        offsets.append((d + bonus, dx, dy))
offsets.sort()

# process worst-constrained first: longer names first
order = sorted(fix, key=lambda r: -len(r))
moves = {}
unplaced = []
pending = {}  # moving refs' rects, checked mutually
for name in order:
    if name in ref_rects:
        pending[name] = ref_rects[name]

def clashes_pending(side, rect, me):
    x1, y1, x2, y2 = rect
    for nm, (s2, r2) in pending.items():
        if nm == me or s2 != side:
            continue
        if not (r2[2] < x1-CLR_TEXT or r2[0] > x2+CLR_TEXT or
                r2[3] < y1-CLR_TEXT or r2[1] > y2+CLR_TEXT):
            return True
    return False

for name in order:
    fp = fpmap.get(name)
    if not fp:
        unplaced.append((name, 'no fp'))
        continue
    r = fp['ref']
    side = 'F' if r['layer'].startswith('F') else 'B'
    placed = False
    for (score, dx, dy) in offsets:
        for trot in (0, 90):
            cx, cy = fp['x'] + dx, fp['y'] + dy
            rect = text_rect(cx, cy, name, r['size'], r['th'], trot)
            if not in_board(rect):
                continue
            if query(side, rect):
                continue
            if clashes_pending(side, rect, name):
                continue
            pending[name] = (side, rect)
            moves[name] = (cx, cy, trot)
            placed = True
            break
        if placed:
            break
    if not placed:
        unplaced.append((name, 'no free spot'))

print(f'placed {len(moves)}, unplaced {len(unplaced)}')
for u in unplaced:
    print('UNPLACED:', u)

# ---------- write back ----------
def find_block(s, start):
    depth = 0; i = start; inq = False
    while i < len(s):
        c = s[i]
        if inq:
            if c == '"' and s[i-1] != BS:
                inq = False
        else:
            if c == '"':
                inq = True
            elif c == '(':
                depth += 1
            elif c == ')':
                depth -= 1
                if depth == 0:
                    return i + 1
        i += 1
    raise ValueError('unbalanced')

def inv_rot(fp, gx, gy):
    dx, dy = gx - fp['x'], gy - fp['y']
    lx, ly = rot_pt(dx, dy, -fp['rot'], MODE)
    return lx, ly

out = src
applied = 0
for name, (cx, cy, trot) in moves.items():
    fp = fpmap[name]
    lx, ly = inv_rot(fp, cx, cy)
    stored_rot = trot   # absolute angle
    key = f'(property "Reference" "{name}"'
    idx = out.find(key)
    assert idx >= 0 and out.find(key, idx+1) < 0, name
    end = find_block(out, idx)
    block = out[idx:end]
    new_at = f'(at {lx:.3f} {ly:.3f} {stored_rot:g})'
    nb, n1 = re.subn(r'\(at [^)]*\)', new_at, block, count=1)
    assert n1 == 1, name
    out = out[:idx] + nb + out[end:]
    applied += 1

print(f'applied {applied} moves')

# normalize any leftover 180/270 stored ref angles to 0/90 (same bbox, tidy display)
norm = 0
for fp in fps:
    r = fp['ref']
    if not r or r['name'] in moves:
        continue
    if r['rot'] % 360 in (180.0, 270.0):
        key = f'(property "Reference" "{r["name"]}"'
        idx = out.find(key)
        end = find_block(out, idx)
        block = out[idx:end]
        new_at = f'(at {r["lx"]:.6g} {r["ly"]:.6g} {r["rot"] % 180:g})'
        nb, n1 = re.subn(r'\(at [^)]*\)', new_at, block, count=1)
        assert n1 == 1, r['name']
        out = out[:idx] + nb + out[end:]
        norm += 1
print(f'normalized {norm} ref angles')
open(PCB, 'w', encoding='utf-8', newline='\n').write(out)
