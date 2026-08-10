#!/usr/bin/env freecadcmd
# -*- coding: utf-8 -*-
"""
rcm enclosure - parametric builder for the sealed box and its lids.

Run with FreeCAD's console binary:

    "C:/Program Files/FreeCAD 1.1/bin/freecadcmd.exe" build_enclosure.py

Everything the model knows lives in PARAMS below.  Change a number, re-run, and
you get fresh FCStd files plus STEP and STL in exports/.  Nothing is drawn by
hand and no sketch is attached to a generated face, so there is nothing to
break when a dimension moves.

--------------------------------------------------------------------------
WHAT IT BUILDS
--------------------------------------------------------------------------
  Enclosure-Box   open-top tray: PCB standoffs, lid screw bosses, and
                  DEUTSCH DT bulkhead ports in the end walls
  Lid-Keypad      2 x 4 grid of 25 mm anti-vandal switches
  Lid-Blank       same plate, no holes - the starting point for the
                  relay-control lid, whose I/O goes out through the lid

--------------------------------------------------------------------------
COORDINATE SYSTEM
--------------------------------------------------------------------------
  X   along the 140 mm side of the PCB, 0 = box centre
  Y   along the 70 mm side of the PCB, 0 = box centre
  Z   up, 0 = outside of the box floor.  Lids are modelled where they sit
      when fitted, so box and lid can be opened in one document and checked
      against each other.

PCB features are quoted in KiCad board coordinates (origin at the board's
top-left corner, Y running DOWN the screen, as the PCB editor shows them).
The mapping is

    x = board_x - pcb_w/2          y = pcb_d/2 - board_y

--------------------------------------------------------------------------
SEALING
--------------------------------------------------------------------------
Face seal: a groove in the LID underside holds a silicone O-ring cord that
the flat top of the box wall squeezes.  The groove is in the lid because the
lid has material to spare around it.  A locating lip inboard of the groove
drops into the cavity so the parts self-align before the screws go in.

Wire entry is through DEUTSCH DT flange receptacles, not open slots, so the
walls have no unsealed openings.  There is deliberately no USB window - the
service port is reached by taking the lid off.
"""

import math
import os

import FreeCAD as App
import Part

HERE = os.path.dirname(os.path.abspath(__file__))

# =========================================================================
# PARAMETERS - this is the only block you should need to edit
# =========================================================================
PARAMS = {

    # ---------------------------------------------------------------
    # 1. THE PCB  (rcm.kicad_pcb, board ordered 2026-08-08)
    # ---------------------------------------------------------------
    "pcb_w":            140.0,     # mm, board X
    "pcb_d":            70.0,      # mm, board Y
    "pcb_t":            1.6,       # mm
    "pcb_holes": [                 # KiCad board coords of the M3 holes
        (5.0, 5.0), (135.0, 5.0), (5.0, 65.0), (135.0, 65.0),
    ],
    "pcb_gap":          9.0,       # mm, board edge -> cavity wall.  Sets the
                                   #     room the lid screw bosses live in.
    "pcb_tall_comp_h":  12.0,      # mm, tallest thing standing on the board.
                                   #     The Waveshare buck on its header pins
                                   #     is the one to measure - MEASURE THIS
                                   #     on the real assembly, it sets the
                                   #     box height on its own.
    "pcb_edge_comp_h":  3.5,       # mm, tallest part in the end strips below.
                                   #     All SMD there; the USB-C shell is the
                                   #     worst at ~3.2
    "pcb_clear_strip":  20.0,      # mm, strip at each END of the board that
                                   #     was checked against rcm.kicad_pcb and
                                   #     holds nothing taller than the above.
                                   #     A DEUTSCH body may overhang this far.

    # ---------------------------------------------------------------
    # 2. PCB MOUNTING
    # ---------------------------------------------------------------
    "standoff_h":       5.0,       # mm, floor to underside of the board
    "standoff_od":      7.0,       # mm
    "standoff_pilot":   2.6,       # mm, pilot for an M3 self-tapping screw
    "standoff_depth":   7.0,       # mm, how deep that pilot is drilled

    # ---------------------------------------------------------------
    # 3. SHELL
    # ---------------------------------------------------------------
    "wall":             5.0,       # mm.  5 not 3 so the wall top is a wide
                                   #      enough land for the seal groove.
    "floor":            3.0,       # mm
    "inner_r":          3.0,       # mm, cavity corner radius
    "top_clearance":    2.0,       # mm, spare air between a switch tail and
                                   #     the tallest board component
    "outer_chamfer":    1.0,       # mm, chamfer on the box's bottom edge

    # ---------------------------------------------------------------
    # 3b. MOUNTING THE BOX TO THE CAR
    #     Provisional: bolts straight through the floor, heads inside, in
    #     the 9 mm border strip that runs outside the PCB.  Reachable with
    #     a long ball-end key without lifting the board out.
    #
    #     Every one of these is a hole through the sealed floor.  Use a
    #     bonded seal ("dowty") washer under each head.
    # ---------------------------------------------------------------
    "floor_holes": [               # (x, y) in the border strip, y = +/-39.5
        (-55.0, 39.5), (55.0, 39.5), (-55.0, -39.5), (55.0, -39.5),
    ],
    "floor_hole_d":     4.5,       # mm, M4 clearance
    "floor_hole_cbore_d": 0.0,     # mm, 0 = none.  A button head sits proud
                                   #     in the strip and fouls nothing.

    # ---------------------------------------------------------------
    # 4. LID SCREWS  (M3 into brass heat-set inserts in the bosses)
    #    The short walls get no mid-span screw: that is where the DEUTSCH
    #    ports live and a full-height boss would sit in the connector body.
    # ---------------------------------------------------------------
    "boss_od":          8.0,       # mm
    "boss_hole_d":      4.2,       # mm, M3 heat-set insert.  Use 2.6 if you
                                   #     would rather self-tap the plastic.
    "boss_hole_depth":  8.0,       # mm
    "screw_mid_long":   True,      # extra screw mid-way along each long wall
    "screw_mid_short":  False,     # would foul the DEUTSCH ports - see above
    "screw_extra":      [],        # any further (x, y) bosses you want
    "lid_screw_clear":  3.4,       # mm, M3 clearance hole in the lid
    "lid_screw_head":   6.4,       # mm, countersink diameter (M3 CSK head)
    "lid_screw_csk":    True,      # False = plain hole for a cap head

    # ---------------------------------------------------------------
    # 5. LID + SEAL
    # ---------------------------------------------------------------
    "lids":            ["keypad", "blank"],
    "lid_t":            6.0,       # mm.  Thicker is stiffer but eats switch
                                   #      thread - watch the printout.  6 not 4
                                   #      because the bezel transition below
                                   #      needs depth to run its 45 deg cone
                                   #      into, and a stiffer lid seals better.
    "lid_lip_h":        2.0,       # mm, locating lip depth into the cavity
    "lid_lip_w":        2.0,       # mm, lip thickness
    "lid_lip_clear":    0.3,       # mm, lip to cavity wall, per side
    "seal_cord_d":      2.0,       # mm, silicone O-ring cord
    "seal_groove_w":    2.4,       # mm, groove width  (cord_d * 1.2)
    "seal_groove_d":    1.5,       # mm, groove depth  (cord squashed ~25%)

    # ---------------------------------------------------------------
    # 6. SWITCHES  (25 mm anti-vandal, M25x1, from the supplier drawing)
    #    Proven by the printed and fitted Keypad-Face.FCStd - do not change
    #    the hole, bezel or pitch numbers without a test print.
    # ---------------------------------------------------------------
    "sw_cols":          4,
    "sw_rows":          2,
    "sw_pitch_x":       35.0,      # mm
    "sw_pitch_y":       35.0,      # mm
    "sw_hole_d":        25.0,      # mm, clearance for the M25x1 thread
    "sw_bezel_d":       28.0,      # mm, head diameter
    "sw_bezel_depth":   2.0,       # mm, head thickness -> head sits flush
    "sw_body_len":      27.8,      # mm, overall length of the switch
    "sw_thread_len":    10.3,      # mm, threaded length behind the head
    "sw_nut_af":        32.2,      # mm, nut across flats (clearance check)

    # The lid prints FACE DOWN, and the bezel recess used to end in a flat
    # annular ceiling 1.5 mm wide - the one feature on either part that
    # needed support.  Replacing it with a 45 deg cone makes the whole lid
    # self-supporting, and the head still seats flush because the recess is
    # full Ø28 for its whole 2 mm before the cone starts.
    #   "chamfer"  cone at sw_bezel_angle.  45 deg is exactly the limit a
    #              slicer prints unsupported - the reason for this default.
    #   "radius"   quarter-round instead.  Softer to look at, but it runs
    #              tangent to the seat, so a narrow ring of it is near
    #              horizontal and may need support after all.
    #   "step"     the original square shoulder.  Needs support face down.
    "sw_bezel_transition": "chamfer",
    "sw_bezel_angle":   45.0,      # deg from horizontal, "chamfer" only

    # A one-hole test coupon: same thickness, same recess, same cone, so a
    # 10-minute print answers whether the head seats flush with its O-ring
    # under it and whether the nut still turns.  35 mm square is the switch
    # pitch, so the coupon is also a check that neighbouring nuts will clear.
    "build_coupon":     True,
    "coupon_size":      35.0,      # mm square
    "coupon_r":         2.0,       # mm, corner radius

    # ---------------------------------------------------------------
    # 7. DEUTSCH DT BULKHEAD PORTS
    #
    #    One footprint fits DT04-2P / -3P / -4P -L012.  From the TE customer
    #    drawings: all three share the flange (40.51 x 31.75 x 3.18) and the
    #    same 4 mounting holes on 30.35 x 22.45 centres, but their
    #    recommended cutouts differ -
    #        2 way   18.42 x 18.29  R5.08
    #        3 way   round, dia 25.4
    #        4 way   22.10 x 20.98  R6.35
    #    A round 25.4 hole contains the 4 way's rounded rectangle with
    #    0.09 mm to spare, so ONE round aperture takes all three and the
    #    ports become interchangeable.
    # ---------------------------------------------------------------
    "dt_ports": [
        # label,            wall,  offset along that wall, z of centre
        ("PWR  DT04-2P",    "-X",   0.0,   28.0),
        ("CAN  DT04-4P",    "+X",   0.0,   28.0),
    ],
    "dt_aperture_d":    25.8,      # mm, 25.4 nominal + fit clearance
    "dt_hole_dx":       30.35,     # mm, mounting hole pitch, along the wall
    "dt_hole_dy":       22.45,     # mm, mounting hole pitch, vertical
    "dt_hole_d":        4.4,       # mm, clearance for M4 (or #8)
    "dt_flange_w":      40.51,     # mm, flange outline - kept flat outside
    "dt_flange_h":      31.75,     # mm
    "dt_body_in":       22.1,      # mm, how far the connector reaches inboard
    "dt_body_d":        21.0,      # mm, envelope of that inboard body
    "dt_pad_t":         3.0,       # mm, extra wall thickness inside the port,
                                   #     so the nut pockets have a solid floor
    "dt_pad_margin":    2.0,       # mm, pad size = flange + this each side
    "dt_nut_af":        7.2,       # mm, M4 nut across flats
    "dt_nut_depth":     3.2,       # mm, nut pocket depth in the inside face
    "dt_nut_pockets":   True,      # False = plain through holes, use your own
                                   #         nuts and washers

    # ---------------------------------------------------------------
    # 8. OUTPUT
    # ---------------------------------------------------------------
    "export_step":      True,
    "export_mesh":      "stl",     # "stl", "obj" or None
    "mesh_deflection":  0.05,
}


# =========================================================================
# DERIVED
# =========================================================================

def derive(P):
    D = {}
    D["cav_w"] = P["pcb_w"] + 2 * P["pcb_gap"]
    D["cav_d"] = P["pcb_d"] + 2 * P["pcb_gap"]
    D["out_w"] = D["cav_w"] + 2 * P["wall"]
    D["out_d"] = D["cav_d"] + 2 * P["wall"]
    D["outer_r"] = P["inner_r"] + P["wall"]

    # how far a switch hangs below the underside of the lid
    D["sw_below_lid"] = P["sw_body_len"] - P["lid_t"]
    # thread left for the nut once the panel has taken its share
    D["thread_left"] = P["sw_thread_len"] - (P["lid_t"] - P["sw_bezel_depth"])

    # depth the bezel transition eats, below the seat
    step = (P["sw_bezel_d"] - P["sw_hole_d"]) / 2.0
    if P["sw_bezel_transition"] == "chamfer":
        D["bezel_trans_h"] = step / math.tan(math.radians(P["sw_bezel_angle"]))
    elif P["sw_bezel_transition"] == "radius":
        D["bezel_trans_h"] = step
    else:
        D["bezel_trans_h"] = 0.0
    D["sw_straight_bore"] = P["lid_t"] - P["sw_bezel_depth"] - D["bezel_trans_h"]

    # Cavity height.  The switch tail and the tallest component stack,
    # because at 35 mm pitch the switches sit directly over the buck module.
    need = (P["standoff_h"] + P["pcb_t"] + P["pcb_tall_comp_h"]
            + D["sw_below_lid"] + P["top_clearance"])
    D["inner_h"] = float(math.ceil(need))
    D["box_h"] = P["floor"] + D["inner_h"]
    D["pcb_top_z"] = P["floor"] + P["standoff_h"] + P["pcb_t"]

    D["lid_z0"] = D["box_h"]                       # underside of the flange
    D["lid_z1"] = D["box_h"] + P["lid_t"]          # front face

    D["seal_land"] = (P["wall"] - P["seal_groove_w"]) / 2.0
    D["boss_xy"] = boss_positions(P, D)
    D["sw_xy"] = switch_positions(P)
    return D


def boss_positions(P, D):
    bx = D["cav_w"] / 2.0 - P["boss_od"] / 2.0
    by = D["cav_d"] / 2.0 - P["boss_od"] / 2.0
    pts = [(bx, by), (bx, -by), (-bx, by), (-bx, -by)]
    if P["screw_mid_long"]:
        pts += [(0.0, by), (0.0, -by)]
    if P["screw_mid_short"]:
        pts += [(bx, 0.0), (-bx, 0.0)]
    return pts + [tuple(p) for p in P["screw_extra"]]


def switch_positions(P):
    pts = []
    for r in range(P["sw_rows"]):
        y = (r - (P["sw_rows"] - 1) / 2.0) * P["sw_pitch_y"]
        for c in range(P["sw_cols"]):
            x = (c - (P["sw_cols"] - 1) / 2.0) * P["sw_pitch_x"]
            pts.append((x, y))
    return pts


def board_to_global(P, bx, by):
    return (bx - P["pcb_w"] / 2.0, P["pcb_d"] / 2.0 - by)


# =========================================================================
# GEOMETRY HELPERS - Part primitives and booleans only
# =========================================================================

def rrect_face(w, d, r, z=0.0):
    """Rounded rectangle face, centred on the origin, at height z."""
    if r <= 0.0:
        pts = [App.Vector(-w / 2, -d / 2, z), App.Vector(w / 2, -d / 2, z),
               App.Vector(w / 2, d / 2, z), App.Vector(-w / 2, d / 2, z)]
        return Part.Face(Part.Wire(Part.makePolygon(pts + [pts[0]])))

    hw, hd = w / 2.0 - r, d / 2.0 - r
    corners = [(hw, -hd, -90.0), (hw, hd, 0.0), (-hw, hd, 90.0), (-hw, -hd, 180.0)]

    def pt(cx, cy, a):
        return App.Vector(cx + r * math.cos(math.radians(a)),
                          cy + r * math.sin(math.radians(a)), z)

    edges, first, prev_end = [], None, None
    for cx, cy, a0 in corners:
        s, m, e = pt(cx, cy, a0), pt(cx, cy, a0 + 45.0), pt(cx, cy, a0 + 90.0)
        if first is None:
            first = s
        else:
            edges.append(Part.LineSegment(prev_end, s).toShape())
        edges.append(Part.Arc(s, m, e).toShape())
        prev_end = e
    edges.append(Part.LineSegment(prev_end, first).toShape())
    return Part.Face(Part.Wire(edges))


def rrect_prism(w, d, r, h, z0=0.0):
    return rrect_face(w, d, r, z0).extrude(App.Vector(0, 0, h))


def rrect_ring(w, d, r, t, h, z0):
    """Hollow rounded-rect ring, `t` thick, outer size w x d."""
    outer = rrect_prism(w, d, r, h, z0)
    inner = rrect_prism(w - 2 * t, d - 2 * t, max(r - t, 0.01), h + 2, z0 - 1)
    return outer.cut(inner)


def cyl(d, h, x, y, z):
    return Part.makeCylinder(d / 2.0, h, App.Vector(x, y, z), App.Vector(0, 0, 1))


def box_at(x0, x1, y0, y1, z0, z1):
    return Part.makeBox(x1 - x0, y1 - y0, z1 - z0, App.Vector(x0, y0, z0))


# --- wall frame -----------------------------------------------------------
# `d_in` / `d_out` are distances measured from the cavity's inner wall face,
# positive going outward.  So d_in=0, d_out=wall spans exactly the wall.

def wall_axis(P, D, wall):
    """(outward unit normal, in-wall unit axis, half-size of the cavity)."""
    if wall == "+X":
        return App.Vector(1, 0, 0), App.Vector(0, 1, 0), D["cav_w"] / 2.0
    if wall == "-X":
        return App.Vector(-1, 0, 0), App.Vector(0, 1, 0), D["cav_w"] / 2.0
    if wall == "+Y":
        return App.Vector(0, 1, 0), App.Vector(1, 0, 0), D["cav_d"] / 2.0
    if wall == "-Y":
        return App.Vector(0, -1, 0), App.Vector(1, 0, 0), D["cav_d"] / 2.0
    raise ValueError("unknown wall %r" % wall)


def wall_point(P, D, wall, u, z, d):
    """A point on the given wall: `u` along it, `z` up, `d` outward from the
    cavity face.  (Vector.multiply mutates in place in FreeCAD - always use *.)"""
    n, ax, half = wall_axis(P, D, wall)
    return n * (half + d) + ax * u + App.Vector(0, 0, z)


def wall_box(P, D, wall, u0, u1, z0, z1, d_in, d_out):
    n, ax, half = wall_axis(P, D, wall)
    if wall in ("+X", "-X"):
        s = 1.0 if wall == "+X" else -1.0
        xa, xb = s * (half - d_in), s * (half + d_out)
        return box_at(min(xa, xb), max(xa, xb), u0, u1, z0, z1)
    s = 1.0 if wall == "+Y" else -1.0
    ya, yb = s * (half - d_in), s * (half + d_out)
    return box_at(u0, u1, min(ya, yb), max(ya, yb), z0, z1)


def wall_cyl(P, D, wall, u, z, dia, d_in, d_out):
    n, ax, half = wall_axis(P, D, wall)
    base = wall_point(P, D, wall, u, z, -d_in)
    return Part.makeCylinder(dia / 2.0, d_in + d_out, base, n)


def wall_hex(P, D, wall, u, z, af, d_in, depth):
    """Hex pocket cut into the inside face of a wall, `depth` deep."""
    n, ax, _ = wall_axis(P, D, wall)
    up = App.Vector(0, 0, 1)
    r = af / math.sqrt(3.0)
    base = wall_point(P, D, wall, u, z, -d_in)
    pts = []
    for i in range(7):
        a = math.radians(60 * i)
        pts.append(base + ax * (r * math.cos(a)) + up * (r * math.sin(a)))
    face = Part.Face(Part.Wire(Part.makePolygon(pts)))
    return face.extrude(n * depth)


# =========================================================================
# DEUTSCH PORTS
# =========================================================================

def add_dt_ports(P, D, shape, log):
    for label, wall, u, z in P["dt_ports"]:
        pad_w = P["dt_flange_w"] + 2 * P["dt_pad_margin"]
        pad_h = P["dt_flange_h"] + 2 * P["dt_pad_margin"]
        shape = shape.fuse(wall_box(P, D, wall, u - pad_w / 2.0, u + pad_w / 2.0,
                                    z - pad_h / 2.0, z + pad_h / 2.0,
                                    P["dt_pad_t"], 0.0))
        shape = shape.cut(wall_cyl(P, D, wall, u, z, P["dt_aperture_d"],
                                   P["dt_pad_t"] + 1.0, P["wall"] + 1.0))
        for du in (-P["dt_hole_dx"] / 2.0, P["dt_hole_dx"] / 2.0):
            for dz in (-P["dt_hole_dy"] / 2.0, P["dt_hole_dy"] / 2.0):
                shape = shape.cut(wall_cyl(P, D, wall, u + du, z + dz,
                                           P["dt_hole_d"],
                                           P["dt_pad_t"] + 1.0, P["wall"] + 1.0))
                if P["dt_nut_pockets"]:
                    shape = shape.cut(wall_hex(P, D, wall, u + du, z + dz,
                                               P["dt_nut_af"], P["dt_pad_t"],
                                               P["dt_nut_depth"]))
        log.append("DT port %-14s %s wall, u=%.1f z=%.1f, %.1f aperture, "
                   "4 x %.1f on %.2f x %.2f"
                   % (label, wall, u, z, P["dt_aperture_d"], P["dt_hole_d"],
                      P["dt_hole_dx"], P["dt_hole_dy"]))
    return shape


# =========================================================================
# THE BOX
# =========================================================================

def build_box(P, D):
    log = []
    shape = rrect_prism(D["out_w"], D["out_d"], D["outer_r"], D["box_h"], 0.0)
    log.append("shell %.1f x %.1f x %.1f mm, %.1f walls, %.1f floor"
               % (D["out_w"], D["out_d"], D["box_h"], P["wall"], P["floor"]))

    shape = shape.cut(rrect_prism(D["cav_w"], D["cav_d"], P["inner_r"],
                                  D["inner_h"] + 1.0, P["floor"]))
    log.append("cavity %.1f x %.1f x %.1f deep"
               % (D["cav_w"], D["cav_d"], D["inner_h"]))

    for bx, by in P["pcb_holes"]:
        x, y = board_to_global(P, bx, by)
        shape = shape.fuse(cyl(P["standoff_od"], P["standoff_h"], x, y, P["floor"]))
    for bx, by in P["pcb_holes"]:
        x, y = board_to_global(P, bx, by)
        top = P["floor"] + P["standoff_h"]
        shape = shape.cut(cyl(P["standoff_pilot"], P["standoff_depth"] + 0.1,
                              x, y, top - P["standoff_depth"]))
    log.append("%d PCB standoffs, %.1f tall, %.1f pilot"
               % (len(P["pcb_holes"]), P["standoff_h"], P["standoff_pilot"]))

    for x, y in D["boss_xy"]:
        shape = shape.fuse(cyl(P["boss_od"], D["inner_h"], x, y, P["floor"]))
    for x, y in D["boss_xy"]:
        shape = shape.cut(cyl(P["boss_hole_d"], P["boss_hole_depth"] + 0.1,
                              x, y, D["box_h"] - P["boss_hole_depth"]))
    log.append("%d lid bosses, %.1f OD, %.1f x %.1f deep"
               % (len(D["boss_xy"]), P["boss_od"], P["boss_hole_d"],
                  P["boss_hole_depth"]))

    for x, y in P["floor_holes"]:
        shape = shape.cut(cyl(P["floor_hole_d"], P["floor"] + 2.0, x, y, -1.0))
        if P["floor_hole_cbore_d"] > 0:
            shape = shape.cut(cyl(P["floor_hole_cbore_d"],
                                  P["floor"] - 1.5 + 0.001, x, y, P["floor"] - 1.5))
    if P["floor_holes"]:
        log.append("%d floor mounting holes, %.1f, in the border strip"
                   % (len(P["floor_holes"]), P["floor_hole_d"]))

    shape = add_dt_ports(P, D, shape, log)

    if P["outer_chamfer"] > 0:
        shape = chamfer_bottom(shape, P["outer_chamfer"], D)
        log.append("bottom edge chamfered %.1f" % P["outer_chamfer"])

    return shape.removeSplitter(), log


def chamfer_bottom(shape, c, D):
    """Chamfer the outer perimeter where it meets z=0 - and only that, so the
    floor mounting holes keep their sharp mouths."""
    def on_perimeter(e):
        bb = e.BoundBox
        return (max(abs(bb.XMin), abs(bb.XMax)) > D["out_w"] / 2.0 - 1.0
                or max(abs(bb.YMin), abs(bb.YMax)) > D["out_d"] / 2.0 - 1.0)

    edges = [e for e in shape.Edges
             if all(abs(v.Point.z) < 1e-6 for v in e.Vertexes) and on_perimeter(e)]
    if not edges:
        return shape
    try:
        return shape.makeChamfer(c, edges)
    except Exception as exc:          # cosmetic only - never fail the build
        print("  (bottom chamfer skipped: %s)" % exc)
        return shape


# =========================================================================
# THE LIDS
# =========================================================================

def build_coupon(P, D):
    """One switch hole in a small square - a test print, not a part of the
    assembly.  Sits on z=0 with its front face up, same as the lids."""
    log = []
    s, t = P["coupon_size"], P["lid_t"]
    shape = rrect_prism(s, s, P["coupon_r"], t, 0.0)
    shape = shape.cut(cyl(P["sw_hole_d"], t + 2.0, 0, 0, -1.0))
    shape = shape.cut(cyl(P["sw_bezel_d"], P["sw_bezel_depth"] + 0.001,
                          0, 0, t - P["sw_bezel_depth"]))
    trans = bezel_transition(P, D, 0, 0, t - P["sw_bezel_depth"])
    if trans is not None:
        shape = shape.cut(trans)
    log.append("%.0f x %.0f x %.1f coupon, one %.1f hole with the %s "
               "transition" % (s, s, t, P["sw_hole_d"], P["sw_bezel_transition"]))
    log.append("%.2f mm of material each side of the bezel recess; the "
               "%.1f AF nut spans %.1f across corners"
               % ((s - P["sw_bezel_d"]) / 2.0, P["sw_nut_af"],
                  P["sw_nut_af"] * 2 / math.sqrt(3.0)))
    return shape.removeSplitter(), log


def bezel_transition(P, D, x, y, z_seat):
    """The solid to remove below a bezel seat so the recess does not end in a
    flat unsupported ceiling when the lid is printed face down.

    z_seat is the underside of the bezel recess.  The transition runs DOWN
    from there, opening the bore back out to sw_bezel_d at the seat itself,
    so the switch head still drops the full sw_bezel_depth and sits flush."""
    r_out = P["sw_bezel_d"] / 2.0
    r_in = P["sw_hole_d"] / 2.0
    h = D["bezel_trans_h"]
    if h <= 0.0:
        return None
    if P["sw_bezel_transition"] == "chamfer":
        return Part.makeCone(r_in, r_out, h, App.Vector(x, y, z_seat - h),
                             App.Vector(0, 0, 1))
    # "radius": quarter-round, tangent to the bore at the bottom and to the
    # seat at the top.  Revolve the profile rather than filleting a face, so
    # nothing depends on generated topology.
    rad = r_out - r_in
    c = App.Vector(x + r_out, y, z_seat - rad)          # arc centre, in XZ
    a = App.Vector(x + r_in, y, z_seat - rad)           # tangent to the bore
    b = App.Vector(x + r_out, y, z_seat)                # tangent to the seat
    m = App.Vector(x + r_out - rad * math.cos(math.radians(45)), y,
                   z_seat - rad + rad * math.sin(math.radians(45)))
    prof = [Part.Arc(a, m, b).toShape(),
            Part.LineSegment(b, App.Vector(x + r_in, y, z_seat)).toShape(),
            Part.LineSegment(App.Vector(x + r_in, y, z_seat), a).toShape()]
    face = Part.Face(Part.Wire(prof))
    return face.revolve(App.Vector(x, y, z_seat), App.Vector(0, 0, 1), 360)


def build_lid(P, D, style):
    log = []
    z0, z1 = D["lid_z0"], D["lid_z1"]
    shape = rrect_prism(D["out_w"], D["out_d"], D["outer_r"], P["lid_t"], z0)
    log.append("plate %.1f x %.1f x %.1f" % (D["out_w"], D["out_d"], P["lid_t"]))

    lip = rrect_ring(D["cav_w"] - 2 * P["lid_lip_clear"],
                     D["cav_d"] - 2 * P["lid_lip_clear"],
                     max(P["inner_r"] - P["lid_lip_clear"], 0.5),
                     P["lid_lip_w"], P["lid_lip_h"], z0 - P["lid_lip_h"])
    for x, y in D["boss_xy"]:
        lip = lip.cut(cyl(P["boss_od"] + 1.0, P["lid_lip_h"] + 2.0,
                          x, y, z0 - P["lid_lip_h"] - 1.0))
    shape = shape.fuse(lip)
    log.append("locating lip %.1f wide x %.1f deep, %.1f clearance per side"
               % (P["lid_lip_w"], P["lid_lip_h"], P["lid_lip_clear"]))

    # seal groove, centred on the wall below
    gw = D["cav_w"] + P["wall"] + P["seal_groove_w"]
    gd = D["cav_d"] + P["wall"] + P["seal_groove_w"]
    groove = rrect_ring(gw, gd,
                        D["outer_r"] - P["wall"] / 2.0 + P["seal_groove_w"] / 2.0,
                        P["seal_groove_w"], P["seal_groove_d"] + 0.001, z0)
    shape = shape.cut(groove)
    log.append("seal groove %.1f x %.1f deep for %.1f cord, %.2f land each side"
               % (P["seal_groove_w"], P["seal_groove_d"], P["seal_cord_d"],
                  D["seal_land"]))

    if style == "keypad":
        for x, y in D["sw_xy"]:
            shape = shape.cut(cyl(P["sw_hole_d"], P["lid_t"] + 2.0, x, y, z0 - 1.0))
            shape = shape.cut(cyl(P["sw_bezel_d"], P["sw_bezel_depth"] + 0.001,
                                  x, y, z1 - P["sw_bezel_depth"]))
            trans = bezel_transition(P, D, x, y, z1 - P["sw_bezel_depth"])
            if trans is not None:
                shape = shape.cut(trans)
        log.append("%d switch holes, %.1f x %.1f pitch, %.1f through + "
                   "%.1f x %.1f recess"
                   % (len(D["sw_xy"]), P["sw_pitch_x"], P["sw_pitch_y"],
                      P["sw_hole_d"], P["sw_bezel_d"], P["sw_bezel_depth"]))
        log.append("bezel transition: %s, %.2f mm deep, leaving %.2f mm of "
                   "straight %.1f bore"
                   % (P["sw_bezel_transition"], D["bezel_trans_h"],
                      D["sw_straight_bore"], P["sw_hole_d"]))
    else:
        log.append("blank face - starting point for the relay-control lid")

    for x, y in D["boss_xy"]:
        shape = shape.cut(cyl(P["lid_screw_clear"], P["lid_t"] + 2.0,
                              x, y, z0 - 1.0))
        if P["lid_screw_csk"]:
            depth = (P["lid_screw_head"] - P["lid_screw_clear"]) / 2.0
            shape = shape.cut(Part.makeCone(
                P["lid_screw_head"] / 2.0, P["lid_screw_clear"] / 2.0, depth,
                App.Vector(x, y, z1), App.Vector(0, 0, -1)))
    log.append("%d screw holes, %.1f clear%s"
               % (len(D["boss_xy"]), P["lid_screw_clear"],
                  ", %.1f countersink" % P["lid_screw_head"]
                  if P["lid_screw_csk"] else ""))

    return shape.removeSplitter(), log


# =========================================================================
# CHECKS - cheap 2D tests that catch the mistakes that matter
# =========================================================================

def checks(P, D):
    warn = []
    nut_r = P["sw_nut_af"] / math.sqrt(3.0)        # across corners / 2
    lip_x = D["cav_w"] / 2.0 - P["lid_lip_clear"] - P["lid_lip_w"]
    lip_y = D["cav_d"] / 2.0 - P["lid_lip_clear"] - P["lid_lip_w"]

    for sx, sy in D["sw_xy"]:
        for bx, by in D["boss_xy"]:
            gap = math.hypot(sx - bx, sy - by) - nut_r - P["boss_od"] / 2.0
            if gap < 1.0:
                warn.append("switch nut at (%.1f, %.1f) is %.2f mm from the "
                            "boss at (%.1f, %.1f)" % (sx, sy, gap, bx, by))
        if abs(sx) + nut_r > lip_x or abs(sy) + nut_r > lip_y:
            warn.append("switch nut at (%.1f, %.1f) fouls the lid lip" % (sx, sy))

    for label, wall, u, z in P["dt_ports"]:
        n, ax, half = wall_axis(P, D, wall)
        half_w = max(P["dt_flange_w"], P["dt_hole_dx"] + P["dt_hole_d"] + 4) / 2.0
        # against the lid screw bosses
        for bx, by in D["boss_xy"]:
            bu = by if wall in ("+X", "-X") else bx
            bn = bx if wall in ("+X", "-X") else by
            if bn * (1 if wall in ("+X", "+Y") else -1) < 0:
                continue                       # boss is on the far side
            gap = abs(bu - u) - P["boss_od"] / 2.0 - P["dt_aperture_d"] / 2.0
            if gap < 1.0:
                warn.append("DT port %s is %.2f mm from the boss at (%.1f, %.1f)"
                            % (label.split()[0], gap, bx, by))
        # against the board and the lid
        low = z - P["dt_body_d"] / 2.0
        over_board = low - (D["pcb_top_z"] + P["pcb_edge_comp_h"])
        if over_board < 0.5:
            warn.append("DT port %s sits %.2f mm above the board's end-strip "
                        "components" % (label.split()[0], over_board))
        top = z + P["dt_flange_h"] / 2.0
        if top > D["box_h"] - 1.0:
            warn.append("DT port %s flange reaches z=%.1f, wall top is %.1f"
                        % (label.split()[0], top, D["box_h"]))
        # does the connector body reach past the strip we know to be clear?
        reach = half - P["dt_body_in"]
        board_edge = (P["pcb_w"] if wall in ("+X", "-X") else P["pcb_d"]) / 2.0
        overhang = board_edge - reach
        if (overhang > P["pcb_clear_strip"]
                and low < D["pcb_top_z"] + P["pcb_tall_comp_h"]):
            warn.append("DT port %s body overhangs the board by %.1f mm, past "
                        "the %.0f mm strip known to be clear"
                        % (label.split()[0], overhang, P["pcb_clear_strip"]))

    for fx, fy in P["floor_holes"]:
        r = P["floor_hole_d"] / 2.0
        if abs(fx) - r < P["pcb_w"] / 2.0 and abs(fy) - r < P["pcb_d"] / 2.0:
            warn.append("floor hole at (%.1f, %.1f) is under the PCB - you "
                        "could not get a bolt in it" % (fx, fy))
        if (abs(fx) + r > D["cav_w"] / 2.0) or (abs(fy) + r > D["cav_d"] / 2.0):
            warn.append("floor hole at (%.1f, %.1f) breaks into a wall"
                        % (fx, fy))
        for bx, by in D["boss_xy"]:
            gap = math.hypot(fx - bx, fy - by) - r - P["boss_od"] / 2.0
            if gap < 1.0:
                warn.append("floor hole at (%.1f, %.1f) is %.2f mm from the "
                            "boss at (%.1f, %.1f)" % (fx, fy, gap, bx, by))

    if D["sw_straight_bore"] < 1.5:
        warn.append("only %.2f mm of straight bore left under the bezel "
                    "transition - thicken the lid" % D["sw_straight_bore"])
    if D["thread_left"] < 5.0:
        warn.append("only %.1f mm of switch thread left for the nut - the lid "
                    "is too thick" % D["thread_left"])
    if D["seal_land"] < 1.0:
        warn.append("seal land is only %.2f mm - widen the wall" % D["seal_land"])
    return warn


def screw_spans(P, D):
    """Longest unsupported run of the seal, per wall - drives how well it
    actually seals."""
    out = {}
    for wall, coords, other in (
            ("long (+/-Y)", [x for x, y in D["boss_xy"] if abs(y) > D["cav_d"] / 4],
             D["cav_w"]),
            ("short (+/-X)", [y for x, y in D["boss_xy"] if abs(x) > D["cav_w"] / 4],
             D["cav_d"])):
        vals = sorted(set(round(c, 3) for c in coords))
        out[wall] = max(b - a for a, b in zip(vals, vals[1:])) if len(vals) > 1 else other
    return out


# =========================================================================
# OUTPUT
# =========================================================================

def save(name, shape, P):
    doc = App.newDocument(name)
    obj = doc.addObject("Part::Feature", name.replace("-", ""))
    obj.Label = name
    obj.Shape = shape
    doc.recompute()
    fcstd = os.path.join(HERE, name + ".FCStd")
    doc.saveAs(fcstd)
    print("  wrote %s" % os.path.relpath(fcstd, HERE))

    exp = os.path.join(HERE, "exports")
    if not os.path.isdir(exp):
        os.makedirs(exp)
    if P["export_step"]:
        p = os.path.join(exp, name + ".step")
        shape.exportStep(p)
        print("  wrote %s" % os.path.relpath(p, HERE))
    if P["export_mesh"]:
        import Mesh
        fmt = P["export_mesh"]
        p = os.path.join(exp, "%s.%s" % (name, fmt))
        Mesh.Mesh(shape.tessellate(P["mesh_deflection"])).write(p, fmt.upper(), name)
        print("  wrote %s" % os.path.relpath(p, HERE))
    App.closeDocument(doc.Name)


def main():
    P = PARAMS
    D = derive(P)

    air = D["box_h"] - D["pcb_top_z"]
    print("")
    print("rcm enclosure")
    print("  outside      %.1f x %.1f x %.1f mm  (box %.1f + lid %.1f)"
          % (D["out_w"], D["out_d"], D["box_h"] + P["lid_t"], D["box_h"], P["lid_t"]))
    print("  cavity       %.1f x %.1f x %.1f mm" % (D["cav_w"], D["cav_d"], D["inner_h"]))
    print("  board top    z = %.1f mm, %.1f mm of air above it" % (D["pcb_top_z"], air))
    print("  switch tail  hangs %.1f mm below the lid, %.1f mm clear of a "
          "%.1f mm component" % (D["sw_below_lid"],
                                 air - D["sw_below_lid"] - P["pcb_tall_comp_h"],
                                 P["pcb_tall_comp_h"]))
    print("  switch nut   %.1f mm of thread left behind a %.1f mm panel"
          % (D["thread_left"], P["lid_t"]))
    print("  bezel        %s transition %.2f mm deep, %.2f mm of straight bore "
          "under it" % (P["sw_bezel_transition"], D["bezel_trans_h"],
                        D["sw_straight_bore"]))
    for wall, span in sorted(screw_spans(P, D).items()):
        print("  seal span    %-12s %.0f mm between screws" % (wall, span))
    print("")

    builds = [("Enclosure-Box", lambda: build_box(P, D))]
    for style in P["lids"]:
        builds.append(("Lid-%s" % style.capitalize(),
                       lambda s=style: build_lid(P, D, s)))
    if P["build_coupon"]:
        builds.append(("Test-Coupon", lambda: build_coupon(P, D)))

    for name, builder in builds:
        shape, log = builder()
        print("%s:" % name)
        for line in log:
            print("  " + line)
        if len(shape.Solids) != 1:
            print("  !! %d solids - not one printable piece" % len(shape.Solids))
        if not shape.isValid():
            print("  !! shape is not valid")
        bb = shape.BoundBox
        print("  bbox %.1f x %.1f x %.1f, volume %.1f cm3"
              % (bb.XLength, bb.YLength, bb.ZLength, shape.Volume / 1000.0))
        save(name, shape, P)
        print("")

    warn = checks(P, D)
    if warn:
        print("CLEARANCE WARNINGS")
        for w in warn:
            print("  ! " + w)
    else:
        print("clearance checks: all clear")
    print("")


main()
