#!/usr/bin/env freecadcmd
# -*- coding: utf-8 -*-
"""
rcm enclosure - parametric builder for the sealed box and its lids.

Run with FreeCAD's console binary:

    "C:/Program Files/FreeCAD 1.1/bin/freecadcmd.exe" build_enclosure.py

Everything the model knows lives in PARAMS below.  Change a number, re-run, and
you get fresh FCStd files plus STEP and 3MF in exports/.  Nothing is drawn by
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
  Guard-Ring      raised collar round the start/stop button, a SEPARATE
                  part because the lid prints face down - see below

--------------------------------------------------------------------------
COORDINATE SYSTEM
--------------------------------------------------------------------------
  X   along the 140 mm side of the PCB, 0 = box centre
  Y   along the 80 mm side of the PCB, 0 = box centre
  Z   up, 0 = outside of the box floor.  Lids are modelled where they sit
      when fitted, so box and lid can be opened in one document and checked
      against each other.

--------------------------------------------------------------------------
ORIENTATION IN THE CAR
--------------------------------------------------------------------------
The keypad build mounts behind the gearstick, to the driver's left, with
the board LONGWAYS - a short side facing the front of the car.

WHICH short side is set by car_front, and it is -X, because the loom comes
from the front of the car and enters through the DEUTSCH port, which is on
the -X wall.  So:

  -X  is the FRONT of the car          +X  is the rear
  +Z  is up

Forward and up fix the rest: in a right-handed frame the car's LEFT is +Y
times the front sign, so with the front at -X the car's RIGHT is +Y - and
on a RHD car that is the driver's side.

Buttons run 4 deep front-to-rear and 2 across.  Say which button you mean
in car terms - ("front", "driver") - and let guard_switch_xy() map it.
NEVER hand-write one of these signs: getting front and rear the wrong way
round is what once put the starter on the diagonally opposite corner, and
the geometry looked perfectly reasonable the whole time.

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

--------------------------------------------------------------------------
WHY THE GUARD RING IS A SEPARATE PART
--------------------------------------------------------------------------
The lid prints FACE DOWN - that is the whole reason the bezel recess ends
in a 45 deg cone instead of a square shoulder.  Anything standing proud of
the front face therefore points straight into the bed and cannot print.
Flipping the lid over only moves the problem to the 2 mm locating lip,
which would hold the whole lid 2 mm off the bed.

So the ring is its own part.  It prints bond-face UP (guard end on the
bed, pins last), and two moulded pins drop into two blind holes in the lid
so it cannot be bonded off-centre or crooked.
"""

import math
import os

import FreeCAD as App
import Part

HERE = os.path.dirname(os.path.abspath(__file__))

# =========================================================================
# DEUTSCH DT FOOTPRINTS
#
# One entry per connector size.  A port in PARAMS names one of these, so
# changing connector is a one-word edit and the two sizes cannot get their
# dimensions crossed.
#
# `verified` is the important field.  True means every number below was
# taken from a TE customer drawing or measured off the part; False means
# some were inferred, and the build says so loudly on every run until
# somebody puts calipers on the connector.
# =========================================================================
DT_FOOTPRINTS = {

    # ---------------------------------------------------------------
    # The small shell: 2, 3 and 4 way are interchangeable in one hole.
    # All three share the flange and the mounting pattern; their
    # recommended cutouts differ -
    #     2 way   18.42 x 18.29  R5.08
    #     3 way   round, dia 25.4
    #     4 way   22.10 x 20.98  R6.35
    # and a round 25.4 contains the 4 way's rectangle with 0.09 to
    # spare, so ONE round aperture takes all three.
    # ---------------------------------------------------------------
    "dt-2-4way": {
        "label":      "DT04-2P / -3P / -4P -L012",
        "ways":       "2/3/4",
        "flange_w":   40.51,
        "flange_h":   31.75,
        "hole_dx":    30.35,
        "hole_dy":    22.45,
        "hole_d":     4.4,          # M4 / #8 clearance
        "aperture":   ("round", 25.8),        # 25.4 nominal + fit
        "body_in":    22.1,         # reach inboard of the wall
        "body_d":     21.0,         # envelope of that inboard body
        "verified":   True,         # printed and fitted
    },

    # ---------------------------------------------------------------
    # The 8 way, for the keypad build: one connector carries the lot.
    #
    # CONFIRMED from TE's published data for DT04-08PA-L012 -
    #     flange     54.99 x 35.51      ("Width" x "Height")
    #     overall    45.67 long
    #     contacts   9.09 pitch, 4.45 row-to-row, size 16
    # The same fields on TE's DT04-3P-L012 page read 40.51 x 31.75,
    # which is exactly the flange this model has used since day one -
    # that is how the mapping above was checked rather than assumed.
    #
    # INFERRED, and the reason verified is False.  TE's customer
    # drawing is behind a login, so these three came from applying the
    # small shell's own proportions to the bigger flange:
    #     hole_dx    inset 5.08 (0.200") per side, as on the 2-4 way
    #     hole_dy    inset 4.65 per side, as on the 2-4 way
    #     aperture   sized to clear the body and stay well inside the
    #                flange; the 8 way needs a rounded RECTANGLE, not a
    #                round hole - its pin field alone is 27.3 wide and
    #                a round hole big enough would be wider than the
    #                flange is tall
    #     body_in    45.67 - flange 3.18 - the 3 way's 17.85 front end
    # MEASURE THESE before you print the wall.  They are a two-minute
    # job with calipers on the connector and a ruined print if wrong.
    # ---------------------------------------------------------------
    "dt-8way": {
        "label":      "DT04-08PA-L012",
        "ways":       "8",
        "flange_w":   54.99,        # confirmed
        "flange_h":   35.51,        # confirmed
        "hole_dx":    44.83,        # INFERRED
        "hole_dy":    26.21,        # INFERRED
        "hole_d":     4.4,
        "aperture":   ("rrect", 38.0, 24.0, 5.0),   # INFERRED  w, h, r
                                    # 38 x 24, not 40 x 26: the bigger one
                                    # left only 1.8 mm between the aperture
                                    # and the mounting holes.  This leaves
                                    # 3.2 - see the check in checks().
        "body_in":    24.7,         # INFERRED
        "body_d":     27.0,         # INFERRED, deliberately generous
        "verified":   False,
    },
}


# =========================================================================
# PARAMETERS - this is the only block you should need to edit
# =========================================================================
PARAMS = {

    # ---------------------------------------------------------------
    # 0. ORIENTATION IN THE CAR
    #
    #    car_front says which way the box points.  Everything else about
    #    left and right follows from it and +Z being up, so DO NOT write
    #    a sign by hand anywhere - ask car_axes().
    #
    #    The loom comes from the FRONT of the car and enters through the
    #    DEUTSCH port, which is on the -X wall.  So -X is the front, and
    #    with +Z up that makes +Y the car's right, which on a RHD car is
    #    the driver's side.
    # ---------------------------------------------------------------
    "car_front":        "-X",      # which model axis faces the car's front
    "car_rhd":          True,      # right-hand drive

    # ---------------------------------------------------------------
    # 1. THE PCB  (rcm.kicad_pcb, board ordered 2026-08-08)
    #
    #    pcb_w/pcb_d are the ROUTED OUTLINE, which is what KiCad's
    #    Edge.Cuts says and what pcb_holes are measured against.  The
    #    boards as delivered carry a manufacturing rail on each long
    #    edge - the components sit close to the edge, so the panel
    #    needs something to hold - and those are being kept and used
    #    as a labelling surface rather than snapped off.  That extra
    #    material is pcb_tab_w and it only widens the ENVELOPE; it
    #    does not move the outline, the holes or the standoffs.
    # ---------------------------------------------------------------
    "pcb_w":            140.0,     # mm, routed outline X
    "pcb_d":            70.0,      # mm, routed outline Y
    "pcb_t":            1.6,       # mm
    "pcb_tab_w":        5.0,       # mm, snap-off rail left ON, per long
                                   #     edge.  70 + 2*5 = 80 overall.
                                   #     Set to 0 if you ever snap them off.
    "pcb_holes": [                 # KiCad board coords of the M3 holes,
                                   # against the 140 x 70 routed outline
        (5.0, 5.0), (135.0, 5.0), (5.0, 65.0), (135.0, 65.0),
    ],
    "pcb_gap":          9.0,       # mm, board ENVELOPE edge -> cavity wall.
                                   #     Sets the room the lid screw bosses
                                   #     live in, and the channel the harness
                                   #     climbs in off the long edges.
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
    # 1b. THE HARNESS OFF THE LONG EDGES
    #
    #    Every screw terminal on this board sits on one of the two long
    #    edges - J_CH1/J_CH2 on one, IGN/AUX/CAN_A/CAN_B on the other -
    #    with the plugs turned so the wires leave the board rather than
    #    running back across it.  The wires then have to climb, in the
    #    channel between the board edge and the cavity wall.
    #
    #    wire_clear_h is the requirement, and it is measured from the
    #    BOTTOM MOUNTING FACE of the board, not from the board top or
    #    the floor, because that is the face you can actually put a rule
    #    on.  checks() proves it against the switch hardware above.
    # ---------------------------------------------------------------
    "wire_clear_h":     30.0,      # mm, clear height needed above the
                                   #     board's bottom mounting face
    "conn_pin_y":       30.0,      # mm, |y| of the two terminal pin rows
                                   #     (KiCad board y = 5 and y = 65)
    "conn_body_out":    4.2,       # mm, plug body reach PAST the pin row,
                                   #     towards the wire entry
    "conn_body_in":     3.4,       # mm, and back the other way
    "conn_body_h":      9.0,       # mm, plug body height above the board

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
    "wall":             2.0,       # mm
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
    "floor_holes": [               # (x, y) in the border strip, y = +/-44.5
        (-55.0, 44.5), (55.0, 44.5), (-55.0, -44.5), (55.0, -44.5),
    ],                             # moved out from 39.5 when the rails were
                                   # kept: 39.5 is now UNDER the board
    "floor_hole_d":     4.5,       # mm, M4 clearance
    "floor_hole_cbore_d": 0.0,     # mm, 0 = none.  A button head sits proud
                                   #     in the strip and fouls nothing.

    # ---------------------------------------------------------------
    # 4. LID SCREWS  (M3 into brass heat-set inserts in the bosses)
    #    The short walls get no mid-span screw: that is where the DEUTSCH
    #    ports live and a full-height boss would sit in the connector body.
    # ---------------------------------------------------------------
    "boss_od":          8.0,       # mm
    "boss_wall_bite":   1.0,       # mm the boss sits INTO the wall it
                                   #     stands against.  At 0 it is exactly
                                   #     tangent - touching along a line and
                                   #     held only by the floor, which is
                                   #     fragile geometry and falls apart the
                                   #     moment you cut a coupon out of it.
                                   #     Moves the screw outboard by this
                                   #     much; the seal groove has room.
    "boss_hole_d":      4.2,       # mm, M3 heat-set insert.  Use 2.6 if you
                                   #     would rather self-tap the plastic.
    "boss_hole_depth":  8.0,       # mm
    "screw_mid_long":   True,      # extra screw mid-way along each long wall
    "screw_mid_short":  False,     # would foul the DEUTSCH ports - see above.
                                   # True, False, or a list of walls: with
                                   # only one port fitted the other short
                                   # wall is free, e.g. ["+X"]
    "screw_extra":      [],        # any further (x, y) bosses you want
    "lid_screw_clear":  3.4,       # mm, M3 clearance hole in the lid
    "lid_screw_head":   6.4,       # mm, countersink diameter (M3 CSK head)
    "lid_screw_csk":    True,      # False = plain hole for a cap head

    # ---------------------------------------------------------------
    # 5. LID + SEAL
    # ---------------------------------------------------------------
    "lids":            ["keypad", "blank"],
    "lid_t":            3.5,       # mm.  Thicker is stiffer but eats switch
                                   #      thread, and the thread is what won:
                                   #      3.5 is the most the nut can leave
                                   #      enough of.  It only fits because
                                   #      sw_bezel_depth came down to 0.5 -
                                   #      the cone still needs its 1.6 mm of
                                   #      depth to run into.  Was 6.0.
    "lid_lip_h":        2.0,       # mm, locating lip depth into the cavity
    "lid_lip_w":        2.0,       # mm, lip thickness
    "lid_lip_clear":    0.3,       # mm, lip to cavity wall, per side
    # The MATING PLUG and its wires reach up into where the lip drops.
    # Only the receptacle body is modelled, so no check could catch it -
    # it turned up on a real print.  The lip is therefore cut back at each
    # DEUTSCH port, but only across the plug's own span: the corners stay,
    # so the lid still self-aligns.  Set lid_lip_omit to a list of walls
    # (e.g. ["-X"]) to strip a whole wall run instead.
    "lid_lip_omit":     None,      # None = cut back at the ports only
    "lid_lip_port_margin": 5.0,    # mm past the flange, each side of a port
    # No seal.  The wall went to 2 mm and a 2.4 mm groove will not fit on
    # it - there is no land left at all, and the lid screws cut into the
    # groove as well.  Rather than shrink the cord onto a 0.4 mm land, the
    # groove is gone: the lid lands flat on the wall top, located by its
    # lip and pulled down by the screws.  Splash-resistant, not sealed.
    #
    # To put it back you need a wall that can carry it - set lid_seal True
    # AND wall to at least seal_groove_w + 2.6, or thicken the wall top
    # from the inside.  checks() will tell you if you have not.
    "lid_seal":         False,
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
    "sw_head_d":        28.0,      # mm, head diameter, as made
    "sw_head_t":        2.0,       # mm, head thickness.  Kept separate from
                                   #     sw_bezel_depth now that the head no
                                   #     longer sits flush - it is what tells
                                   #     the model how proud the head stands,
                                   #     which the guard ring has to clear.
    "sw_bezel_d":       28.2,      # mm, the recess on the TOP FACE.  0.2
                                   #     over the head so it drops in rather
                                   #     than being pressed in.
    "sw_bezel_depth":   0.5,       # mm, the drop from the top face down to
                                   #     the chamfer.  Only a register now,
                                   #     not the full head thickness: the
                                   #     head lands on its O-ring, which
                                   #     lands on the chamfer.  Was 2.0, and
                                   #     dropping it is what buys the thread
                                   #     back for the nut.
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

    # A bigger coupon: the actual CORNER of the lid nearest the start/stop
    # button, cut out of the real part, plus the matching top of the box
    # corner.  Between them they answer the things the flat coupon cannot:
    # does the switch fit, does the guard ring locate on its pins, and does
    # the lid corner actually sit down on the box - lip into the cavity,
    # seal groove over the wall, screw into the boss.
    #
    # Cut by intersection with the built parts, never re-modelled, so it
    # cannot drift from the lid and box it is supposed to be testing.
    "corner_coupon":    True,
    "corner_coupon_w":  None,      # mm from the outer corner along X.
    "corner_coupon_d":  None,      # mm from the outer corner along Y.
                                   #     None = work out what it takes to
                                   #     swallow the switch nut, the guard
                                   #     ring and the corner screw boss.
    "corner_coupon_margin": 4.0,   # mm of spare past the widest of those
    "corner_coupon_box_h": 20.0,   # mm of box, measured DOWN from the wall
                                   #     top.  Only the top matters for a
                                   #     fit test, and the whole 48 would be
                                   #     a needless three-hour print.

    # ---------------------------------------------------------------
    # 6b. START/STOP GUARD RING  (a separate printed part)
    #
    #    A raised collar around one button so a knee, a sleeve or a
    #    dropped hand cannot start or stop the car.  It is separate
    #    because the lid prints face down and this stands proud of the
    #    front face - see WHY THE GUARD RING IS A SEPARATE PART above.
    #
    #    It is located by two pins into two blind holes in the lid, so
    #    the only skill in the glue-up is not using too much glue.
    #    Say which button in CAR terms; guard_switch_xy() maps it.
    # ---------------------------------------------------------------
    "guard_ring":       True,
    "guard_sw":         ("front", "driver"),   # "front"/"rear",
                                   #             "driver"/"kerb"
                                   # The loom leaves by the same end the
                                   # starter is on, and that end is the
                                   # FRONT - see car_front above.
    "guard_id":         30.0,      # mm, bore.  1 mm clear of the 28 head,
                                   #     and a fingertip drops in easily.
    "guard_od":         38.0,      # mm.  At 35 pitch this leaves 2 mm to
                                   #     the neighbouring button's head.
    "guard_h":          6.5,       # mm above the LID FACE.  The head is no
                                   #     longer flush - it stands proud by
                                   #     sw_head_t - sw_bezel_depth - so the
                                   #     guard you actually get is less than
                                   #     this.  6.5 keeps 5.0 above the
                                   #     button, which is the minimum asked
                                   #     for.  derive() does that sum and
                                   #     checks() enforces it.
    "guard_min_above_button": 5.0,  # mm of guard above the button's face
    # Both top edges are broken at 45 deg - they are the two edges a
    # finger actually meets, and the ring is there to BE prodded at.
    # The base is left square: a chamfer there would only open a dirt
    # trap round the bond line, and no finger reaches it.
    "guard_chamfer_in":  1.0,      # mm, on the bore edge
    "guard_chamfer_out": 1.0,      # mm, on the outer edge

    # The pins.  Printed standing up (the ring prints bond-face UP), so
    # keep them fat and short rather than thin and long.
    "guard_pin_d":      3.0,       # mm
    "guard_pin_len":    1.8,       # mm, how far they stand out of the ring.
                                   #     1.8, not 3.0: the lid is 3.5 thick
                                   #     now and a 3.5 deep hole would go
                                   #     straight through it.
    "guard_pin_r":      17.0,      # mm, pin circle radius, mid-wall
    "guard_pin_angle":  45.0,      # deg.  Two pins, this angle and +180.
                                   #     45 puts them on the diagonal, the
                                   #     furthest from both neighbours.
    "guard_pin_clear":  0.2,       # mm, added to the LID hole diameter
    "guard_pin_hole_extra": 0.3,   # mm, lid hole deeper than the pin, so
                                   #     the ring seats on the face and not
                                   #     on the bottom of a blind hole

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
    # Each port is (label, footprint key, wall, offset along it, z).
    # z = None means "as low as the board and its end-strip components
    # allow", and the box then grows to cover the flange - which is how
    # the 8 way sizes the revB box without anybody doing the sums.
    "dt_ports": [
        ("PWR", "dt-2-4way", "-X", 0.0, 28.0),
        ("CAN", "dt-2-4way", "+X", 0.0, 28.0),
    ],
    "dt_top_margin":    2.0,       # mm of wall left above the flange
    "dt_board_clear":   1.5,       # mm the body must clear the end-strip
                                   #     components by when z is automatic
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
    "export_mesh":      ["3mf"],   # any of "3mf", "stl", "obj"; [] for none.
                                   #     3MF is what goes into Prusa/Orca -
                                   #     it carries its units (mm), so it
                                   #     never imports 25.4x too small.
    "mesh_deflection":  0.05,
}


# =========================================================================
# VARIANTS
#
# Each entry is a set of overrides on PARAMS, and each builds its own set
# of files with the variant name on them.  Both are kept because both are
# real: revA is the board as ordered, with power and CAN on separate small
# DEUTSCH ports at opposite ends; revB is the keypad build, where all
# eight wires arrive on one 8 way at one end.
#
# Build one with:  freecadcmd build_enclosure.py revB-8way
# =========================================================================
VARIANTS = {

    "revA": {},                    # exactly as before - nothing overridden

    "revB-8way": {
        # Eight wires, one connector, one end.  The +X wall is now blank,
        # so it can finally take a mid-span lid screw: that halves the
        # 90 mm screw span which was the weakest part of the box.
        "dt_ports": [
            ("ALL", "dt-8way", "-X", 0.0, None),
        ],
        "screw_mid_short": ["+X"],
    },
}


# =========================================================================
# DERIVED
# =========================================================================

def derive(P):
    D = {}
    # The board's real footprint in the box is the routed outline plus the
    # manufacturing rails left on the long edges.  Everything that has to
    # clear the BOARD uses the envelope; everything that has to line up
    # with a FEATURE on the board uses the outline, via board_to_global.
    D["pcb_env_w"] = P["pcb_w"]
    D["pcb_env_d"] = P["pcb_d"] + 2 * P["pcb_tab_w"]
    D["cav_w"] = D["pcb_env_w"] + 2 * P["pcb_gap"]
    D["cav_d"] = D["pcb_env_d"] + 2 * P["pcb_gap"]
    D["out_w"] = D["cav_w"] + 2 * P["wall"]
    D["out_d"] = D["cav_d"] + 2 * P["wall"]
    D["outer_r"] = P["inner_r"] + P["wall"]

    # how far a switch hangs below the underside of the lid
    D["sw_below_lid"] = P["sw_body_len"] - P["lid_t"]
    # thread left for the nut once the panel has taken its share
    D["thread_left"] = P["sw_thread_len"] - (P["lid_t"] - P["sw_bezel_depth"])
    # The head only sinks as far as the register, so the rest stands proud.
    # Nominal: a thick O-ring under it will lift the head further still.
    D["sw_head_proud"] = P["sw_head_t"] - P["sw_bezel_depth"]

    # depth the bezel transition eats, below the seat
    step = (P["sw_bezel_d"] - P["sw_hole_d"]) / 2.0
    if P["sw_bezel_transition"] == "chamfer":
        D["bezel_trans_h"] = step / math.tan(math.radians(P["sw_bezel_angle"]))
    elif P["sw_bezel_transition"] == "radius":
        D["bezel_trans_h"] = step
    else:
        D["bezel_trans_h"] = 0.0
    D["sw_straight_bore"] = P["lid_t"] - P["sw_bezel_depth"] - D["bezel_trans_h"]

    D["pcb_top_z"] = P["floor"] + P["standoff_h"] + P["pcb_t"]

    # Where a DEUTSCH port can sit: high enough that the body clears the
    # board's end-strip components, and then the flange has to fit above.
    D["dt_z_min"] = (D["pcb_top_z"] + P["pcb_edge_comp_h"]
                     + P["dt_board_clear"])
    D["dt_ports"] = resolve_dt_ports(P, D)

    # Cavity height.  Two things want a say and the taller one wins:
    #   - the switch tail and the tallest component, which stack because
    #     at 35 mm pitch the switches sit directly over the buck module;
    #   - the DEUTSCH flange, which has to fit on the wall with a little
    #     wall left above it.  The 8 way flange is 35.5 tall, and this is
    #     what grows the revB box to suit without anybody doing the sums.
    need = (P["standoff_h"] + P["pcb_t"] + P["pcb_tall_comp_h"]
            + D["sw_below_lid"] + P["top_clearance"])
    D["h_driver"] = "switch stack"
    for port in D["dt_ports"]:
        want = (port["z"] + port["fp"]["flange_h"] / 2.0 + P["dt_top_margin"]
                - P["floor"])
        if want > need:
            need, D["h_driver"] = want, "%s flange" % port["fp"]["label"]
    D["inner_h"] = float(math.ceil(need))
    D["box_h"] = P["floor"] + D["inner_h"]

    D["lid_z0"] = D["box_h"]                       # underside of the flange
    D["lid_z1"] = D["box_h"] + P["lid_t"]          # front face

    # Walls the locating lip is left off.  Default: any wall with a
    # DEUTSCH port, because the mating plug and its wires - which this
    # model does not draw - come up into where the lip would drop.
    if P["lid_lip_omit"] is None:
        D["lip_omit"] = []                     # no whole wall run removed
        D["lip_port_gaps"] = list(D["dt_ports"])
    else:
        D["lip_omit"] = list(P["lid_lip_omit"])
        D["lip_port_gaps"] = []
    # shortest run of lip left beside a port, which is what still locates
    D["lip_run_left"] = None
    for pt in D["lip_port_gaps"]:
        half_wall = ((D["cav_d"] if pt["wall"] in ("+X", "-X") else D["cav_w"])
                     / 2.0 - P["lid_lip_clear"] - P["lid_lip_w"])
        gap_half = pt["fp"]["flange_w"] / 2.0 + P["lid_lip_port_margin"]
        run = half_wall - abs(pt["u"]) - gap_half
        D["lip_run_left"] = (run if D["lip_run_left"] is None
                             else min(D["lip_run_left"], run))

    D["seal_land"] = ((P["wall"] - P["seal_groove_w"]) / 2.0
                      if P["lid_seal"] else None)
    D["boss_xy"] = boss_positions(P, D)
    D["sw_xy"] = switch_positions(P)

    # ---- the harness channel off the long edges -------------------
    # The bottom mounting face of the board: the face the standoffs touch,
    # and the datum the 30 mm requirement is quoted from.
    D["board_bot_z"] = P["floor"] + P["standoff_h"]
    # Worst case the switch nut fills the whole protruding thread.
    D["nut_r_corner"] = P["sw_nut_af"] / math.sqrt(3.0)
    D["nut_bot_z"] = D["lid_z0"] - D["thread_left"]
    # Wires climb OUTBOARD of the plug bodies, so the channel starts here.
    D["wire_band_y"] = P["conn_pin_y"] + P["conn_body_out"]
    D["plug_top_z"] = D["pcb_top_z"] + P["conn_body_h"]

    ceiling = D["lid_z0"]                      # nothing above but the lid
    for sx, sy in D["sw_xy"]:
        if abs(sy) + D["nut_r_corner"] > D["wire_band_y"]:
            ceiling = min(ceiling, D["nut_bot_z"])
        if abs(sy) + P["sw_hole_d"] / 2.0 > D["wire_band_y"]:
            ceiling = min(ceiling, D["lid_z0"] - D["sw_below_lid"])
    D["wire_ceiling_z"] = ceiling
    D["wire_avail"] = ceiling - D["board_bot_z"]

    # ---- the corner coupon ----------------------------------------
    # How far in from the outer corner it has to reach to contain the
    # switch nut, the guard ring and the nearest lid screw boss.
    if P["guard_ring"]:
        gx, gy = guard_switch_xy(P, D)
        wide = max(D["nut_r_corner"], P["guard_od"] / 2.0)
        need_w = abs(D["out_w"] / 2.0 - abs(gx)) + wide
        need_d = abs(D["out_d"] / 2.0 - abs(gy)) + wide
        for bx, by in D["boss_xy"]:
            if bx * gx > 0 and by * gy > 0:         # a true corner boss,
                                                    # not a mid-span one
                need_w = max(need_w, abs(D["out_w"] / 2.0 - abs(bx))
                             + P["boss_od"] / 2.0)
                need_d = max(need_d, abs(D["out_d"] / 2.0 - abs(by))
                             + P["boss_od"] / 2.0)
        m = P["corner_coupon_margin"]
        D["corner_w"] = P["corner_coupon_w"] or math.ceil(need_w + m)
        D["corner_d"] = P["corner_coupon_d"] or math.ceil(need_d + m)

    # ---- the start/stop guard -------------------------------------
    D["guard_xy"] = guard_switch_xy(P, D) if P["guard_ring"] else None
    D["guard_pins"] = []
    # What the guard is worth is its height above the BUTTON, not the lid.
    D["guard_above_button"] = (P["guard_h"] - D["sw_head_proud"]
                               if P["guard_ring"] else 0.0)
    if P["guard_ring"]:
        gx, gy = D["guard_xy"]
        for a in (P["guard_pin_angle"], P["guard_pin_angle"] + 180.0):
            t = math.radians(a)
            D["guard_pins"].append((gx + P["guard_pin_r"] * math.cos(t),
                                    gy + P["guard_pin_r"] * math.sin(t)))
    return D


def resolve_dt_ports(P, D):
    """Turn PARAMS' terse port list into fully specified ports.

    Each entry is (label, footprint key, wall, u, z).  z = None asks for
    the lowest the board allows, which is what you want under a big
    flange: it keeps the connector clear of the lid seal and lets the box
    height fall out of the connector rather than the other way round."""
    out = []
    for label, key, wall, u, z in P["dt_ports"]:
        if key not in DT_FOOTPRINTS:
            raise ValueError("unknown DT footprint %r - have %s"
                             % (key, sorted(DT_FOOTPRINTS)))
        fp = DT_FOOTPRINTS[key]
        out.append({"label": label, "key": key, "fp": fp, "wall": wall,
                    "u": float(u), "auto_z": z is None,
                    "z": (D["dt_z_min"] + fp["body_d"] / 2.0 if z is None
                          else float(z))})
    return out


def car_axes(P):
    """(sign of X that faces the car's front, sign of Y on the driver's side).

    Forward is car_front and up is +Z, so in a right-handed frame the car's
    LEFT is +Y times the front sign, and its right the other way.  A RHD
    driver sits on the right.

    Everything that needs to know about front, rear, driver or kerb comes
    through here.  Hand-written signs are how the starter ended up on the
    wrong corner once already."""
    if P["car_front"] not in ("+X", "-X"):
        raise ValueError("car_front must be '+X' or '-X', not %r"
                         % P["car_front"])
    f = 1.0 if P["car_front"] == "+X" else -1.0
    right = -f
    return f, (right if P["car_rhd"] else -right)


def car_wall_name(P, wall):
    """A wall of the box, said in car terms."""
    f, drv = car_axes(P)
    if wall in ("+X", "-X"):
        s = 1.0 if wall == "+X" else -1.0
        return "front" if s == f else "rear"
    s = 1.0 if wall == "+Y" else -1.0
    return "driver's side" if s == drv else "kerb side"


def guard_switch_xy(P, D):
    """Resolve guard_sw, written in CAR terms, into a switch centre."""
    fore, side = P["guard_sw"]
    if fore not in ("front", "rear"):
        raise ValueError("guard_sw[0] must be 'front' or 'rear', not %r" % fore)
    if side not in ("driver", "kerb"):
        raise ValueError("guard_sw[1] must be 'driver' or 'kerb', not %r" % side)
    xs = sorted(set(round(x, 4) for x, y in D["sw_xy"]))
    ys = sorted(set(round(y, 4) for x, y in D["sw_xy"]))
    f, drv = car_axes(P)
    front_x, rear_x = (xs[-1], xs[0]) if f > 0 else (xs[0], xs[-1])
    driver_y, kerb_y = (ys[-1], ys[0]) if drv > 0 else (ys[0], ys[-1])
    return (front_x if fore == "front" else rear_x,
            driver_y if side == "driver" else kerb_y)


def boss_positions(P, D):
    # Sit the boss INTO the wall rather than tangent to it, so the two
    # genuinely fuse.  See boss_wall_bite.
    bx = D["cav_w"] / 2.0 - P["boss_od"] / 2.0 + P["boss_wall_bite"]
    by = D["cav_d"] / 2.0 - P["boss_od"] / 2.0 + P["boss_wall_bite"]
    pts = [(bx, by), (bx, -by), (-bx, by), (-bx, -by)]
    if P["screw_mid_long"]:
        pts += [(0.0, by), (0.0, -by)]
    mid = P["screw_mid_short"]
    if mid is True:
        pts += [(bx, 0.0), (-bx, 0.0)]
    elif mid:                              # a list of walls, e.g. ["+X"]
        for w in mid:
            pts.append((bx if w == "+X" else -bx, 0.0))
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


def wall_rrect(P, D, wall, u, z, w, h, r, d_in, d_out):
    """A rounded-rectangle hole through a wall, built from boxes and
    cylinders so it stays Part primitives all the way down.  The 8 way DT
    needs this: its pin field alone is 27.3 wide, so a round hole big
    enough to pass the body would be wider than the flange is tall."""
    r = min(r, w / 2.0, h / 2.0)
    parts = [wall_box(P, D, wall, u - w / 2.0 + r, u + w / 2.0 - r,
                      z - h / 2.0, z + h / 2.0, d_in, d_out),
             wall_box(P, D, wall, u - w / 2.0, u + w / 2.0,
                      z - h / 2.0 + r, z + h / 2.0 - r, d_in, d_out)]
    for du in (-(w / 2.0 - r), w / 2.0 - r):
        for dz in (-(h / 2.0 - r), h / 2.0 - r):
            parts.append(wall_cyl(P, D, wall, u + du, z + dz, 2 * r,
                                  d_in, d_out))
    shape = parts[0]
    for q in parts[1:]:
        shape = shape.fuse(q)
    return shape


def wall_aperture(P, D, wall, u, z, spec, d_in, d_out):
    """Cut the shape named by a footprint's `aperture` field."""
    kind = spec[0]
    if kind == "round":
        return wall_cyl(P, D, wall, u, z, spec[1], d_in, d_out)
    if kind == "rrect":
        return wall_rrect(P, D, wall, u, z, spec[1], spec[2], spec[3],
                          d_in, d_out)
    raise ValueError("unknown aperture kind %r" % kind)


def aperture_edge_gap(spec, du, dz):
    """Distance from a point on the wall to the nearest edge of the
    aperture.  Positive means the point is outside it."""
    if spec[0] == "round":
        return math.hypot(du, dz) - spec[1] / 2.0
    w2, h2, r = spec[1] / 2.0, spec[2] / 2.0, spec[3]
    dx, dy = abs(du) - (w2 - r), abs(dz) - (h2 - r)
    if dx > 0.0 and dy > 0.0:
        return math.hypot(dx, dy) - r      # out by a rounded corner
    if dx > 0.0:
        return abs(du) - w2                # out by a vertical edge
    return abs(dz) - h2                    # out by a horizontal edge


def aperture_half(spec):
    """Half-width and half-height of an aperture, for clearance sums."""
    if spec[0] == "round":
        return spec[1] / 2.0, spec[1] / 2.0
    return spec[1] / 2.0, spec[2] / 2.0


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
    for port in D["dt_ports"]:
        fp, wall, u, z = port["fp"], port["wall"], port["u"], port["z"]
        pad_w = fp["flange_w"] + 2 * P["dt_pad_margin"]
        pad_h = fp["flange_h"] + 2 * P["dt_pad_margin"]
        shape = shape.fuse(wall_box(P, D, wall, u - pad_w / 2.0, u + pad_w / 2.0,
                                    z - pad_h / 2.0, z + pad_h / 2.0,
                                    P["dt_pad_t"], 0.0))
        shape = shape.cut(wall_aperture(P, D, wall, u, z, fp["aperture"],
                                        P["dt_pad_t"] + 1.0, P["wall"] + 1.0))
        for du in (-fp["hole_dx"] / 2.0, fp["hole_dx"] / 2.0):
            for dz in (-fp["hole_dy"] / 2.0, fp["hole_dy"] / 2.0):
                shape = shape.cut(wall_cyl(P, D, wall, u + du, z + dz,
                                           fp["hole_d"],
                                           P["dt_pad_t"] + 1.0, P["wall"] + 1.0))
                if P["dt_nut_pockets"]:
                    shape = shape.cut(wall_hex(P, D, wall, u + du, z + dz,
                                               P["dt_nut_af"], P["dt_pad_t"],
                                               P["dt_nut_depth"]))
        ap = fp["aperture"]
        ap_txt = ("%.1f round" % ap[1] if ap[0] == "round"
                  else "%.1f x %.1f R%.1f" % (ap[1], ap[2], ap[3]))
        log.append("DT port %-4s %-26s %s wall, u=%.1f z=%.1f%s, aperture %s, "
                   "4 x %.1f on %.2f x %.2f"
                   % (port["label"], fp["label"], wall, u, z,
                      " (auto)" if port["auto_z"] else "", ap_txt,
                      fp["hole_d"], fp["hole_dx"], fp["hole_dy"]))
        if not fp["verified"]:
            log.append("!! %s footprint is UNVERIFIED - check hole pitch, "
                       "aperture and body depth before printing"
                       % fp["label"])
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


def build_port_coupon(P, D, port):
    """A flat patch of the port wall - same 5 mm wall, same 3 mm inner pad,
    same aperture, same four holes and nut pockets - so the connector can be
    test-fitted for a few grams of filament instead of a 185 cm3 box.

    Worth printing FIRST whenever the footprint is unverified, because the
    three inferred numbers (hole pitch, aperture, body depth) all fail in
    the same way: a box you cannot use.

    Built flat, outer face down on z=0, which is also how it prints."""
    log = []
    fp = port["fp"]
    t_wall, t_pad = P["wall"], P["dt_pad_t"]
    pad_w = fp["flange_w"] + 2 * P["dt_pad_margin"]
    pad_h = fp["flange_h"] + 2 * P["dt_pad_margin"]
    w, h = pad_w + 8.0, pad_h + 8.0

    shape = rrect_prism(w, h, 3.0, t_wall, 0.0)
    shape = shape.fuse(rrect_prism(pad_w, pad_h, 2.0, t_pad, t_wall))

    ap = fp["aperture"]
    thru = t_wall + t_pad + 2.0
    if ap[0] == "round":
        shape = shape.cut(cyl(ap[1], thru, 0, 0, -1.0))
        ap_txt = "%.1f round" % ap[1]
    else:
        aw, ah, ar = ap[1], ap[2], ap[3]
        cut = box_at(-aw / 2.0 + ar, aw / 2.0 - ar, -ah / 2.0, ah / 2.0,
                     -1.0, thru - 1.0)
        cut = cut.fuse(box_at(-aw / 2.0, aw / 2.0, -ah / 2.0 + ar,
                              ah / 2.0 - ar, -1.0, thru - 1.0))
        for sx in (-(aw / 2.0 - ar), aw / 2.0 - ar):
            for sy in (-(ah / 2.0 - ar), ah / 2.0 - ar):
                cut = cut.fuse(cyl(2 * ar, thru, sx, sy, -1.0))
        shape = shape.cut(cut)
        ap_txt = "%.1f x %.1f R%.1f" % (aw, ah, ar)

    top = t_wall + t_pad
    for du in (-fp["hole_dx"] / 2.0, fp["hole_dx"] / 2.0):
        for dv in (-fp["hole_dy"] / 2.0, fp["hole_dy"] / 2.0):
            shape = shape.cut(cyl(fp["hole_d"], thru, du, dv, -1.0))
            if P["dt_nut_pockets"]:
                r = P["dt_nut_af"] / math.sqrt(3.0)
                pts = [App.Vector(du + r * math.cos(math.radians(60 * i)),
                                  dv + r * math.sin(math.radians(60 * i)),
                                  top - P["dt_nut_depth"]) for i in range(7)]
                hexp = Part.Face(Part.Wire(Part.makePolygon(pts)))
                shape = shape.cut(hexp.extrude(App.Vector(0, 0,
                                                          P["dt_nut_depth"] + 1)))

    log.append("%.0f x %.0f x %.1f patch of the %s wall (%.1f wall + %.1f pad)"
               % (w, h, top, port["wall"], t_wall, t_pad))
    log.append("aperture %s, 4 x %.1f on %.2f x %.2f%s"
               % (ap_txt, fp["hole_d"], fp["hole_dx"], fp["hole_dy"],
                  ", hex nut pockets" if P["dt_nut_pockets"] else ""))
    log.append("print this and test-fit a %s before the box" % fp["label"])
    return shape.removeSplitter(), log


_CORNER_SRC = {}


def corner_region(P, D, z0, z1):
    """The block the corner coupons are cut from: the outside corner of the
    lid nearest the guarded switch, reaching in by the derived amount and
    standing proud of the outer faces so the cut keeps the real surfaces."""
    gx, gy = D["guard_xy"]
    sx = 1.0 if gx >= 0 else -1.0
    sy = 1.0 if gy >= 0 else -1.0
    x_out = sx * (D["out_w"] / 2.0 + 5.0)          # past the outer face
    y_out = sy * (D["out_d"] / 2.0 + 5.0)
    x_in = sx * (D["out_w"] / 2.0 - D["corner_w"])
    y_in = sy * (D["out_d"] / 2.0 - D["corner_d"])
    return (box_at(min(x_in, x_out), max(x_in, x_out),
                   min(y_in, y_out), max(y_in, y_out), z0, z1),
            (x_in + sx * D["out_w"] / 2.0) / 2.0,
            (y_in + sy * D["out_d"] / 2.0) / 2.0)


def build_corner_coupon(P, D, which):
    """A corner of the real lid, and the matching top of the real box
    corner, so the pair can be offered up to each other.

    Both are cut from the built parts by intersection - nothing here is
    modelled a second time, so the coupon cannot quietly disagree with the
    part it is testing.  Both are moved by the SAME amount in X and Y, so
    laid on top of each other they still line up."""
    log = []
    if which not in _CORNER_SRC:
        _CORNER_SRC[which] = (build_lid(P, D, "keypad") if which == "lid"
                              else build_box(P, D))[0]
    src = _CORNER_SRC[which]

    if which == "lid":
        z0, z1 = D["lid_z0"] - P["lid_lip_h"] - 2.0, D["lid_z1"] + 2.0
    else:
        z0, z1 = D["box_h"] - P["corner_coupon_box_h"], D["box_h"] + 2.0

    reg, cx, cy = corner_region(P, D, z0, z1)
    shape = src.common(reg)
    if not shape.Solids:
        raise ValueError("corner coupon %r came out empty" % which)
    shape.translate(App.Vector(-cx, -cy, -shape.BoundBox.ZMin))

    bb = shape.BoundBox
    log.append("%.0f x %.0f corner of the %s, cut from the built part"
               % (bb.XLength, bb.YLength,
                  "keypad lid" if which == "lid" else "box"))
    if which == "lid":
        log.append("carries the %s %s button, its %.1f x %.1f recess, both "
                   "guard pin holes, the corner screw, the seal groove and "
                   "the locating lip"
                   % (P["guard_sw"][1], P["guard_sw"][0], P["sw_bezel_d"],
                      P["sw_bezel_depth"]))
        log.append("prints FACE DOWN, same as the lid it came from")
    else:
        log.append("top %.0f mm of the box corner: wall top, cavity, corner "
                   "boss and its %.1f insert hole"
                   % (P["corner_coupon_box_h"], P["boss_hole_d"]))
        log.append("prints as modelled, open side up, same as the box")
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


def lip_gap(P, D, wall, z0):
    """The block that takes the locating lip off one wall.  Cut from the
    lip ring ONLY, before it is fused, so it cannot touch the plate."""
    inset = P["lid_lip_clear"] + P["lid_lip_w"]
    zlo, zhi = z0 - P["lid_lip_h"] - 1.0, z0 + 1.0
    bx = D["cav_w"] / 2.0 + 10.0
    by = D["cav_d"] / 2.0 + 10.0
    if wall in ("+X", "-X"):
        edge = D["cav_w"] / 2.0 - inset
        x0, x1 = (edge, bx) if wall == "+X" else (-bx, -edge)
        return box_at(x0, x1, -by, by, zlo, zhi)
    edge = D["cav_d"] / 2.0 - inset
    y0, y1 = (edge, by) if wall == "+Y" else (-by, -edge)
    return box_at(-bx, bx, y0, y1, zlo, zhi)


def lip_port_gap(P, D, port, z0):
    """Take the lip away just where a DEUTSCH plug comes through, leaving
    the corners of that wall to go on locating the lid."""
    inset = P["lid_lip_clear"] + P["lid_lip_w"]
    zlo, zhi = z0 - P["lid_lip_h"] - 1.0, z0 + 1.0
    half = port["fp"]["flange_w"] / 2.0 + P["lid_lip_port_margin"]
    u0, u1 = port["u"] - half, port["u"] + half
    if port["wall"] in ("+X", "-X"):
        edge, bx = D["cav_w"] / 2.0 - inset, D["cav_w"] / 2.0 + 10.0
        x0, x1 = (edge, bx) if port["wall"] == "+X" else (-bx, -edge)
        return box_at(x0, x1, u0, u1, zlo, zhi)
    edge, by = D["cav_d"] / 2.0 - inset, D["cav_d"] / 2.0 + 10.0
    y0, y1 = (edge, by) if port["wall"] == "+Y" else (-by, -edge)
    return box_at(u0, u1, y0, y1, zlo, zhi)


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
    for wall in D["lip_omit"]:
        lip = lip.cut(lip_gap(P, D, wall, z0))
    for pt in D["lip_port_gaps"]:
        lip = lip.cut(lip_port_gap(P, D, pt, z0))
    shape = shape.fuse(lip)
    log.append("locating lip %.1f wide x %.1f deep, %.1f clearance per side"
               % (P["lid_lip_w"], P["lid_lip_h"], P["lid_lip_clear"]))
    if D["lip_omit"]:
        log.append("lip left OFF the %s wall run(s) entirely"
                   % ", ".join(D["lip_omit"]))
    if D["lip_port_gaps"]:
        log.append("lip cut back clear of %d DEUTSCH plug(s) - %.1f mm of "
                   "lip still runs each side of a port"
                   % (len(D["lip_port_gaps"]), D["lip_run_left"]))

    if P["lid_seal"]:
        # seal groove, centred on the wall below
        gw = D["cav_w"] + P["wall"] + P["seal_groove_w"]
        gd = D["cav_d"] + P["wall"] + P["seal_groove_w"]
        groove = rrect_ring(gw, gd,
                            D["outer_r"] - P["wall"] / 2.0
                            + P["seal_groove_w"] / 2.0,
                            P["seal_groove_w"], P["seal_groove_d"] + 0.001, z0)
        shape = shape.cut(groove)
        log.append("seal groove %.1f x %.1f deep for %.1f cord, %.2f land "
                   "each side" % (P["seal_groove_w"], P["seal_groove_d"],
                                  P["seal_cord_d"], D["seal_land"]))
    else:
        log.append("no seal groove - lid lands flat on the %.1f mm wall top"
                   % P["wall"])

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
        if P["guard_ring"]:
            depth = P["guard_pin_len"] + P["guard_pin_hole_extra"]
            for px, py in D["guard_pins"]:
                shape = shape.cut(cyl(P["guard_pin_d"] + P["guard_pin_clear"],
                                      depth + 1.0, px, py, z1 - depth))
            log.append("2 blind %.1f x %.1f holes for the guard ring pins, "
                       "leaving %.1f mm of lid under them"
                       % (P["guard_pin_d"] + P["guard_pin_clear"], depth,
                          P["lid_t"] - depth))
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


def build_guard_ring(P, D):
    """The start/stop guard: a raised collar around one button so a knee or
    a sleeve cannot start the car.

    Modelled where it SITS when fitted - standing on the lid front face
    with its pins pointing down into it - so box, lid and ring can be
    opened in one document and checked against each other.  It PRINTS the
    other way up: guard end on the bed, bond face and pins last, which is
    the only orientation in which the pins are not overhangs.  The lid
    itself is unchanged apart from the two blind holes."""
    log = []
    gx, gy = D["guard_xy"]
    z0, h = D["lid_z1"], P["guard_h"]

    shape = cyl(P["guard_od"], h, gx, gy, z0)
    shape = shape.cut(cyl(P["guard_id"], h + 2.0, gx, gy, z0 - 1.0))
    log.append("collar %.1f OD x %.1f ID x %.1f high, %.1f wall"
               % (P["guard_od"], P["guard_id"], h,
                  (P["guard_od"] - P["guard_id"]) / 2.0))

    # Break both top edges.  Printed bond-face up these are the FIRST
    # layers off the bed: the outer one widens going up and the bore one
    # closes in going up, both at 45 deg, so neither needs support.
    ci, co = P["guard_chamfer_in"], P["guard_chamfer_out"]
    if ci > 0.0:
        r_in = P["guard_id"] / 2.0
        over = ci + 0.5                     # run the cut past the top face
        shape = shape.cut(Part.makeCone(
            r_in, r_in + over, over, App.Vector(gx, gy, z0 + h - ci),
            App.Vector(0, 0, 1)))
    if co > 0.0:
        r_out = P["guard_od"] / 2.0
        over = co + 0.5
        ring = cyl(P["guard_od"] + 4.0, over, gx, gy, z0 + h - co)
        ring = ring.cut(Part.makeCone(
            r_out, max(r_out - over, 0.01), over,
            App.Vector(gx, gy, z0 + h - co), App.Vector(0, 0, 1)))
        shape = shape.cut(ring)
    if ci > 0.0 or co > 0.0:
        log.append("top edges broken %.1f in / %.1f out at 45 deg, leaving "
                   "%.1f mm flat on top"
                   % (ci, co, (P["guard_od"] - P["guard_id"]) / 2.0 - ci - co))

    for px, py in D["guard_pins"]:
        shape = shape.fuse(cyl(P["guard_pin_d"], P["guard_pin_len"],
                               px, py, z0 - P["guard_pin_len"]))
    log.append("2 locating pins %.1f x %.1f on a %.1f radius at %.0f deg, "
               "into blind holes in the lid"
               % (P["guard_pin_d"], P["guard_pin_len"], P["guard_pin_r"],
                  P["guard_pin_angle"]))
    log.append("sits on the %s %s button at (%.1f, %.1f)"
               % (P["guard_sw"][1], P["guard_sw"][0], gx, gy))
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

    for port in D["dt_ports"]:
        fp, wall, u, z = port["fp"], port["wall"], port["u"], port["z"]
        label = port["label"]
        ap_hw, ap_hh = aperture_half(fp["aperture"])
        n, ax, half = wall_axis(P, D, wall)
        half_w = max(fp["flange_w"], fp["hole_dx"] + fp["hole_d"] + 4) / 2.0
        if not fp["verified"]:
            warn.append("DT port %s uses the UNVERIFIED %s footprint - its "
                        "hole pitch, aperture and body depth are inferred, "
                        "not measured" % (label, fp["label"]))
        if fp["flange_w"] > D["out_d" if wall in ("+X", "-X") else "out_w"] - 8:
            warn.append("DT port %s flange is wider than the wall it is on"
                        % label)
        # against the lid screw bosses
        for bx, by in D["boss_xy"]:
            bu = by if wall in ("+X", "-X") else bx
            bn = bx if wall in ("+X", "-X") else by
            if bn * (1 if wall in ("+X", "+Y") else -1) < 0:
                continue                       # boss is on the far side
            gap = abs(bu - u) - P["boss_od"] / 2.0 - ap_hw
            if gap < 1.0:
                warn.append("DT port %s aperture is %.2f mm from the boss at "
                            "(%.1f, %.1f)" % (label, gap, bx, by))
            flange_gap = abs(bu - u) - P["boss_od"] / 2.0 - fp["flange_w"] / 2.0
            if flange_gap < 0.0:
                warn.append("DT port %s FLANGE overlaps the boss at "
                            "(%.1f, %.1f) by %.2f mm"
                            % (label, bx, by, -flange_gap))
        # against the board and the lid
        low = z - fp["body_d"] / 2.0
        over_board = low - (D["pcb_top_z"] + P["pcb_edge_comp_h"])
        if over_board < 0.5:
            warn.append("DT port %s sits %.2f mm above the board's end-strip "
                        "components" % (label, over_board))
        top = z + fp["flange_h"] / 2.0
        if top > D["box_h"] - 1.0:
            warn.append("DT port %s flange reaches z=%.1f, wall top is %.1f"
                        % (label, top, D["box_h"]))
        # 2.5 not 3: the proven 2-4 way footprint covers its round aperture
        # by 2.975 mm on the short axis, and that one is printed and fitted.
        for du in (-fp["hole_dx"] / 2.0, fp["hole_dx"] / 2.0):
            for dz in (-fp["hole_dy"] / 2.0, fp["hole_dy"] / 2.0):
                edge = aperture_edge_gap(fp["aperture"], du, dz)
                web = edge - fp["hole_d"] / 2.0
                if web < 2.0:
                    warn.append("DT port %s leaves %.2f mm of wall between the "
                                "aperture and the mounting hole at "
                                "(%+.1f, %+.1f)" % (label, web, du, dz))
        cover = min(fp["flange_w"] / 2.0 - ap_hw, fp["flange_h"] / 2.0 - ap_hh)
        if cover < 2.5:
            warn.append("DT port %s flange covers its aperture by only %.2f mm "
                        "- check the cutout" % (label, cover))
        # does the connector body reach past the strip we know to be clear?
        reach = half - fp["body_in"]
        board_edge = (D["pcb_env_w"] if wall in ("+X", "-X")
                      else D["pcb_env_d"]) / 2.0
        overhang = board_edge - reach
        if (overhang > P["pcb_clear_strip"]
                and low < D["pcb_top_z"] + P["pcb_tall_comp_h"]):
            warn.append("DT port %s body overhangs the board by %.1f mm, past "
                        "the %.0f mm strip known to be clear"
                        % (label, overhang, P["pcb_clear_strip"]))

    for fx, fy in P["floor_holes"]:
        r = P["floor_hole_d"] / 2.0
        if (abs(fx) - r < D["pcb_env_w"] / 2.0
                and abs(fy) - r < D["pcb_env_d"] / 2.0):
            warn.append("floor hole at (%.1f, %.1f) is under the PCB (rails "
                        "included) - you could not get a bolt in it"
                        % (fx, fy))
        if (abs(fx) + r > D["cav_w"] / 2.0) or (abs(fy) + r > D["cav_d"] / 2.0):
            warn.append("floor hole at (%.1f, %.1f) breaks into a wall"
                        % (fx, fy))
        for bx, by in D["boss_xy"]:
            gap = math.hypot(fx - bx, fy - by) - r - P["boss_od"] / 2.0
            if gap < 1.0:
                warn.append("floor hole at (%.1f, %.1f) is %.2f mm from the "
                            "boss at (%.1f, %.1f)" % (fx, fy, gap, bx, by))

    # 1.2, not 1.5: the 1.5 was tied to the old 6 mm lid.  What this length
    # does is guide the thread, and the nut is what holds the switch.
    if D["sw_straight_bore"] < 1.2:
        warn.append("only %.2f mm of straight bore left under the bezel "
                    "transition - the switch has nothing to line up in"
                    % D["sw_straight_bore"])
    if D["thread_left"] < 5.0:
        warn.append("only %.1f mm of switch thread left for the nut - the lid "
                    "is too thick" % D["thread_left"])
    if D["lip_run_left"] is not None and D["lip_run_left"] < 8.0:
        warn.append("only %.1f mm of locating lip left beside a DEUTSCH port "
                    "- the lid will not self-align" % D["lip_run_left"])
    if len(D["lip_omit"]) >= 3:
        warn.append("the locating lip is left off %d of 4 walls (%s) - the "
                    "lid no longer self-aligns"
                    % (len(D["lip_omit"]), ", ".join(D["lip_omit"])))
    if set(D["lip_omit"]) >= {"+X", "-X"} or set(D["lip_omit"]) >= {"+Y", "-Y"}:
        warn.append("the lip is off BOTH %s walls, so the lid is free to "
                    "slide along that axis - it is located by the screws "
                    "alone" % ("+/-X" if "+X" in D["lip_omit"] else "+/-Y"))

    if P["lid_seal"]:
        groove_in = (D["cav_w"] + P["wall"] - P["seal_groove_w"]) / 2.0
        for bx, by in D["boss_xy"]:
            if abs(bx) + P["lid_screw_head"] / 2.0 > groove_in:
                warn.append("lid screw at (%.1f, %.1f) breaks into the seal "
                            "groove - reduce boss_wall_bite" % (bx, by))
                break
        if D["seal_land"] < 1.0:
            warn.append("seal land is only %.2f mm - the %.1f groove does not "
                        "fit a %.1f wall. Widen the wall to %.1f, or thicken "
                        "its top from the inside"
                        % (D["seal_land"], P["seal_groove_w"], P["wall"],
                           P["seal_groove_w"] + 2.0))

    # ---- the harness off the long edges ---------------------------
    if D["wire_avail"] < P["wire_clear_h"]:
        warn.append("only %.1f mm above the board's bottom mounting face for "
                    "the harness, %.1f needed - the ceiling is z=%.1f"
                    % (D["wire_avail"], P["wire_clear_h"], D["wire_ceiling_z"]))
    # the plug bodies themselves, against the switch tails over them
    plug_in = P["conn_pin_y"] - P["conn_body_in"]
    for sx, sy in D["sw_xy"]:
        if abs(sy) + P["sw_hole_d"] / 2.0 <= plug_in:
            continue                       # switch body is clear of the plug
        gap = (D["lid_z0"] - D["sw_below_lid"]) - D["plug_top_z"]
        if gap < 1.0:
            warn.append("switch tail at (%.1f, %.1f) comes within %.2f mm of "
                        "the terminal plug bodies" % (sx, sy, gap))
        break

    # ---- the start/stop guard ring --------------------------------
    if P["guard_ring"]:
        gx, gy = D["guard_xy"]
        g_r = P["guard_od"] / 2.0
        if P["guard_id"] < P["sw_bezel_d"] + 1.0:
            warn.append("guard bore %.1f is not 1 mm clear of the %.1f button "
                        "head" % (P["guard_id"], P["sw_bezel_d"]))
        for sx, sy in D["sw_xy"]:
            if (sx, sy) == (gx, gy):
                continue
            gap = math.hypot(sx - gx, sy - gy) - g_r - P["sw_bezel_d"] / 2.0
            if gap < 1.0:
                warn.append("guard ring is %.2f mm from the button at "
                            "(%.1f, %.1f)" % (gap, sx, sy))
        if (abs(gx) + g_r > D["out_w"] / 2.0 - 2.0
                or abs(gy) + g_r > D["out_d"] / 2.0 - 2.0):
            warn.append("guard ring runs off the edge of the lid")
        # the pins have to live inside the ring wall
        pr, pd = P["guard_pin_r"], P["guard_pin_d"]
        if pr - pd / 2.0 < P["guard_id"] / 2.0 or pr + pd / 2.0 > g_r:
            warn.append("guard pins at r=%.1f do not fit the %.1f..%.1f wall"
                        % (pr, P["guard_id"] / 2.0, g_r))
        # and their holes have to miss everything already in the lid
        hr = (P["guard_pin_d"] + P["guard_pin_clear"]) / 2.0
        for px, py in D["guard_pins"]:
            for sx, sy in D["sw_xy"]:
                gap = math.hypot(px - sx, py - sy) - hr - P["sw_bezel_d"] / 2.0
                if gap < 1.0:
                    warn.append("guard pin hole at (%.1f, %.1f) is %.2f mm "
                                "from the button recess at (%.1f, %.1f)"
                                % (px, py, gap, sx, sy))
            for bx, by in D["boss_xy"]:
                gap = math.hypot(px - bx, py - by) - hr - P["lid_screw_head"] / 2.0
                if gap < 1.0:
                    warn.append("guard pin hole at (%.1f, %.1f) is %.2f mm "
                                "from the lid screw at (%.1f, %.1f)"
                                % (px, py, gap, bx, by))
        flat = ((P["guard_od"] - P["guard_id"]) / 2.0
                - P["guard_chamfer_in"] - P["guard_chamfer_out"])
        if flat < 0.8:
            warn.append("guard ring chamfers leave only %.2f mm flat on top - "
                        "they have eaten the %.1f mm wall" % (flat, g_r - P["guard_id"] / 2.0))
        left = P["lid_t"] - P["guard_pin_len"] - P["guard_pin_hole_extra"]
        if left < 1.2:
            warn.append("guard pin holes leave only %.2f mm of lid under "
                        "them - shorten guard_pin_len" % left)
        if D["guard_above_button"] < P["guard_min_above_button"]:
            warn.append("guard stands only %.2f mm above the button face, "
                        "%.1f wanted - the head is %.2f proud, so raise "
                        "guard_h" % (D["guard_above_button"],
                                     P["guard_min_above_button"],
                                     D["sw_head_proud"]))
    return warn


def screw_spans(P, D):
    """Longest run between lid screws, per wall.  With a seal fitted this is
    what drives how well it actually seals; without one it is simply where
    the lid can lift away from the wall top."""
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
    """Write one part.  `name` already carries the variant suffix."""
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
    fmts = P["export_mesh"] or []
    if isinstance(fmts, str):
        fmts = [fmts]
    if fmts:
        import Mesh
        mesh = Mesh.Mesh(shape.tessellate(P["mesh_deflection"]))
        for fmt in fmts:
            p = os.path.join(exp, "%s.%s" % (name, fmt))
            mesh.write(p, fmt.upper(), name)
            print("  wrote %s" % os.path.relpath(p, HERE))
    App.closeDocument(doc.Name)


def report(vname, P, D):
    air = D["box_h"] - D["pcb_top_z"]
    print("")
    print("=" * 72)
    print("rcm enclosure - %s" % vname)
    print("=" * 72)
    f, drv = car_axes(P)
    print("  orientation  %s is the FRONT of the car, +Z up, so the driver's "
          "side (%s) is %sY" % (P["car_front"], "RHD" if P["car_rhd"] else "LHD",
                                "+" if drv > 0 else "-"))
    print("  outside      %.1f x %.1f x %.1f mm  (box %.1f + lid %.1f)"
          % (D["out_w"], D["out_d"], D["box_h"] + P["lid_t"], D["box_h"],
             P["lid_t"]))
    print("  cavity       %.1f x %.1f x %.1f mm, height set by the %s"
          % (D["cav_w"], D["cav_d"], D["inner_h"], D["h_driver"]))
    print("  board        %.0f x %.0f routed, %.0f x %.0f with the rails on"
          % (P["pcb_w"], P["pcb_d"], D["pcb_env_w"], D["pcb_env_d"]))
    print("  board top    z = %.1f mm, %.1f mm of air above it"
          % (D["pcb_top_z"], air))
    print("  switch tail  hangs %.1f mm below the lid, %.1f mm clear of a "
          "%.1f mm component" % (D["sw_below_lid"],
                                 air - D["sw_below_lid"] - P["pcb_tall_comp_h"],
                                 P["pcb_tall_comp_h"]))
    print("  switch nut   %.1f mm of thread left behind a %.1f mm panel"
          % (D["thread_left"], P["lid_t"]))
    print("  switch head  %.1f recess on the top face, head stands %.1f proud"
          % (P["sw_bezel_depth"], D["sw_head_proud"]))
    print("  bezel        %s transition %.2f mm deep, %.2f mm of straight bore "
          "under it" % (P["sw_bezel_transition"], D["bezel_trans_h"],
                        D["sw_straight_bore"]))
    if D["lip_port_gaps"]:
        print("  lid lip      cut back at %d DEUTSCH port(s), %.1f mm of lip "
              "left each side" % (len(D["lip_port_gaps"]), D["lip_run_left"]))
    print("  harness      %.1f mm clear above the board's bottom face, %.1f "
          "needed (%+.1f)" % (D["wire_avail"], P["wire_clear_h"],
                              D["wire_avail"] - P["wire_clear_h"]))
    for port in D["dt_ports"]:
        print("  wire entry   %-4s %-26s %s wall = %s, z=%.1f%s"
              % (port["label"], port["fp"]["label"], port["wall"],
                 car_wall_name(P, port["wall"]), port["z"],
                 " (auto)" if port["auto_z"] else ""))
    if P["guard_ring"]:
        print("  guard ring   %s %s button at (%.1f, %.1f), %.1f OD x %.1f "
              "ID x %.1f high" % (P["guard_sw"][1], P["guard_sw"][0],
                                  D["guard_xy"][0], D["guard_xy"][1],
                                  P["guard_od"], P["guard_id"], P["guard_h"]))
        print("               %.1f mm of that stands above the button face "
              "(%.1f wanted)" % (D["guard_above_button"],
                                 P["guard_min_above_button"]))
    for wall, span in sorted(screw_spans(P, D).items()):
        print("  screw span   %-12s %.0f mm%s" % (wall, span,
              " between screws" if not P["lid_seal"] else " of unsupported seal"))
    print("")


def build_variant(vname, overrides, want_coupon):
    _CORNER_SRC.clear()            # the built parts differ per variant
    P = dict(PARAMS)
    P.update(overrides)
    D = derive(P)
    report(vname, P, D)

    builds = [("Enclosure-Box-%s" % vname, lambda: build_box(P, D))]
    for style in P["lids"]:
        builds.append(("Lid-%s-%s" % (style.capitalize(), vname),
                       lambda s=style: build_lid(P, D, s)))
    if P["guard_ring"] and "keypad" in P["lids"]:
        builds.append(("Guard-Ring-%s" % vname,
                       lambda: build_guard_ring(P, D)))
    # The bezel coupon is a flat test print of the lid geometry, which no
    # variant changes - so it is built once, not once per variant.
    if want_coupon and P["build_coupon"]:
        builds.append(("Test-Coupon", lambda: build_coupon(P, D)))
    if P["corner_coupon"] and P["guard_ring"] and "keypad" in P["lids"]:
        for half in ("lid", "box"):
            builds.append(("Corner-Coupon-%s-%s" % (half.capitalize(), vname),
                           lambda h=half: build_corner_coupon(P, D, h)))
    # A port coupon per UNVERIFIED footprint, so the connector can be
    # test-fitted before anybody prints a box around inferred dimensions.
    for port in D["dt_ports"]:
        if not port["fp"]["verified"]:
            builds.append(("Port-Coupon-%s-%s" % (port["key"], vname),
                           lambda pt=port: build_port_coupon(P, D, pt)))

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
        print("CLEARANCE WARNINGS - %s" % vname)
        for w in warn:
            print("  ! " + w)
    else:
        print("clearance checks: all clear")
    print("")
    return warn


def main():
    import sys
    asked = [a for a in sys.argv[1:] if not a.endswith(".py")]
    for a in asked:
        if a not in VARIANTS:
            raise SystemExit("unknown variant %r - have %s"
                             % (a, ", ".join(VARIANTS)))
    names = asked or list(VARIANTS)

    first = True
    trouble = {}
    for vname in names:
        w = build_variant(vname, VARIANTS[vname], want_coupon=first)
        first = False
        if w:
            trouble[vname] = w

    if len(names) > 1:
        print("=" * 72)
        for vname in names:
            print("  %-12s %s" % (vname, "%d warning(s)" % len(trouble[vname])
                                  if vname in trouble else "all clear"))
        print("")


main()
