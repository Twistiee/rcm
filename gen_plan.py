"""Emit board_plan.json -- where every part physically sits on the RCM board.

Layout intent (see SPEC.md "Floorplan"):
  * South half  = the three IDENTICAL 7-channel tiles. Each tile is one 7-pole
    terminal on the south edge, its 21 divider resistors stacked directly above
    their own terminal pin, then the driver and the two shift registers behind.
    Defined ONCE as a group and instantiated three times -- that repetition is
    the whole reason the shift-register architecture was chosen.
  * North half  = power entry + latch + buck on the left, MCU centre,
    CAN/IMU/EEPROM/USB right.

All terminals sit on an edge with their wire entry pointing OFF the board:
south-edge terminals at rot 0 (entry +Y), north-edge at rot 180 (entry -Y).
No 90-degree terminal rotations anywhere -- that is where orientation mistakes
come from.

Run: python gen_plan.py  ->  board_plan.json
Then: python tools/plan_lint.py board_plan.json
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

BOARD_W, BOARD_H = 140.0, 70.0   # = the button panel: 4 x 2 cells of 35mm
PITCH = 3.5              # terminal pole pitch, and the channel column pitch
TILE_PITCH = 44.0        # x distance between tile anchors
TILE_X0 = 7.0            # x of tile 1 pin 1
TILE_Y = 65.0            # y of the channel terminal pin row (south edge)

placement = {}
groups = []


def P(ref, x, y, rot=0):
    placement[ref] = [round(x, 3), round(y, 3), rot]


# ---------------------------------------------------------------------------
# South -- three identical channel tiles
#
# Offsets are relative to the tile anchor (= J_CH pin 1). Negative Y is toward
# the middle of the board, since the terminals are on the south edge.
# ---------------------------------------------------------------------------
members = []
refs_for = [[], [], []]

members.append((0.0, 0.0, 0))                     # J_CH
for t in range(3):
    refs_for[t].append("J_CH%d" % (t + 1))

for k in range(7):
    dx = k * PITCH
    for row, prefix in ((-8.0, "R_SH"), (-12.5, "R_SL"), (-17.0, "R_PU")):
        members.append((dx, row, 90))             # 0805 stood vertical: 1.85mm
        for t in range(3):                        # wide, so it fits the 3.5mm
            ch = t * 7 + k + 1                    # column pitch with margin
            refs_for[t].append("%s%d" % (prefix, ch))

# Driver and both shift registers side by side in one row. This is what makes the
# tile fit a 70mm-tall board: 34mm tall instead of 46mm, using width we now have.
# Driver, both shift registers and their decoupling all in ONE row. This is what
# makes the tile fit a 70mm-tall board: 31mm tall instead of 46mm, spending the
# width the 140mm board now has.
members.append((3.0, -25.0, 0))                   # U_DRV (TSSOP-16)
members.append((13.0, -25.0, 90))                 # U_SO  (SOIC-16 side-on, 10x6)
members.append((19.5, -25.0, 90))                 # C_SO decoupling
members.append((26.0, -25.0, 90))                 # U_SI
members.append((32.8, -25.0, 90))                 # C_SI decoupling
for t in range(3):
    n = t + 1
    refs_for[t] += ["U_DRV%d" % n, "U_SO%d" % n, "C_SO%d" % n,
                    "U_SI%d" % n, "C_SI%d" % n]

groups.append({
    "members": [list(m) for m in members],
    "instances": [
        {"at": [TILE_X0 + t * TILE_PITCH, TILE_Y], "refs": refs_for[t]}
        for t in range(3)
    ],
})

# ---------------------------------------------------------------------------
# North-west -- power entry, protection, latch, buck, LDO
#
# The Waveshare buck's BODY is 33 x 16mm spanning JB1..JB2 and is drawn on F.Fab.
# Nothing may sit under it, so the LDO row goes BELOW y=44, not beside the buck.
# ---------------------------------------------------------------------------
P("J_PWR", 10.0, 5.0, 180)
P("J_IGN", 22.0, 5.0, 180)
P("F1", 6.0, 13.0, 90)
P("D1", 10.0, 13.0, 90)
P("D2", 14.0, 13.0, 90)
P("C_BULK", 18.0, 13.0, 90)

P("U_LATCH", 30.0, 13.0, 0)
P("R_IGH", 22.0, 20.0, 0)
P("R_LG", 26.0, 20.0, 0)
P("R_LPD", 30.0, 20.0, 0)
P("R_LIGN", 34.0, 20.0, 0)
P("R_IGL", 22.0, 24.0, 0)
P("R_LHOLD", 26.0, 24.0, 0)
P("C_LVS", 30.0, 24.0, 0)
P("C_LSW", 34.0, 24.0, 0)

# Waveshare buck: columns 27.80mm apart in X, offset 1.90mm in Y (IN's first hole
# is 3.60mm from the module edge, OUT's is 1.70mm). Body lands x 42.4..75.4,
# y 10.4..26.4 and must stay clear -- it is F.Fab only, invisible to plan_lint.
BUCK_X, BUCK_Y = 46.0, 14.0
P("JB1", BUCK_X, BUCK_Y, 0)
P("JB2", BUCK_X + 27.8, BUCK_Y - 1.9, 0)
P("U_LDO", 46.0, 32.0, 0)
P("C_5V", 53.0, 32.0, 90)
P("C_3V3I", 57.0, 32.0, 90)
P("C_3V3O", 61.0, 32.0, 90)

# --- MCU, centre-east ---
MX, MY = 94.0, 24.0
P("U_MCU", MX, MY, 0)
P("Y1", MX - 6.0, 12.0, 0)
P("C_Y1A", MX - 12.0, 10.5, 90)
P("C_Y1B", MX - 12.0, 14.0, 90)
P("Y2", MX + 6.0, 12.0, 0)
P("C_Y2A", MX + 12.0, 10.5, 90)
P("C_Y2B", MX + 12.0, 14.0, 90)
for i, ref in enumerate(["C_M1", "C_M2", "C_M3", "C_M4", "C_M5"]):
    P(ref, MX - 8.0 + i * 4.0, MY + 10.0, 0)
P("C_MB", MX - 12.0, MY + 10.0, 0)
P("C_VCAP", MX - 10.0, MY - 2.0, 90)
P("C_VDDA", MX - 10.0, MY + 2.0, 90)
P("C_NRST", MX + 10.0, MY - 2.0, 90)
P("R_BOOT", MX + 10.0, MY + 2.0, 90)

P("J_SWD", 66.0, 34.0, 90)
P("J_BOOT", 66.0, 28.0, 90)
P("SW_RST", 14.0, 32.0, 0)
P("D_LED1", 22.0, 32.0, 0)
P("R_LED1", 26.0, 32.0, 0)
P("D_LED2", 31.0, 32.0, 0)
P("R_LED2", 35.0, 32.0, 0)

# --- CAN / IMU / EEPROM / USB / aux, far east ---
P("J_AUX", 104.0, 5.0, 180)
P("J_CAN1", 117.0, 5.0, 180)
P("J_CAN2", 130.0, 5.0, 180)

P("R_AH1", 108.0, 11.0, 90)
P("R_AH2", 112.0, 11.0, 90)
P("R_AH3", 116.0, 11.0, 90)
P("R_AL1", 108.0, 15.0, 90)
P("R_AL2", 112.0, 15.0, 90)
P("R_AL3", 116.0, 15.0, 90)

P("U_CAN", 124.0, 12.0, 0)
P("C_CAN", 130.0, 12.0, 90)
P("R_TERM", 122.0, 17.0, 0)
P("R_TJ", 126.0, 17.0, 0)
P("R_RS", 130.0, 17.0, 0)
P("D_CAN", 134.0, 17.0, 0)

P("U_IMU", 112.0, 23.0, 0)
P("C_IMU1", 118.0, 22.0, 90)
P("C_IMU2", 118.0, 25.5, 90)
P("R_ADDR", 122.0, 22.0, 90)
P("R_ADDR_ALT", 122.0, 25.5, 90)
P("R_SDA", 126.0, 22.0, 90)
P("R_SCL", 126.0, 25.5, 90)

P("U_EEP", 112.0, 32.0, 0)
P("C_EEP", 118.0, 32.0, 90)
P("R_OE", 122.0, 32.0, 90)

P("J_USB", 133.0, 30.0, 0)
P("R_CC1", 126.0, 30.0, 90)
P("R_CC2", 126.0, 33.5, 90)

# ---------------------------------------------------------------------------
plan = {
    "netlist": os.path.join(HERE, "rcm.kicad_sch.net"),
    "board": os.path.join(HERE, "rcm.kicad_pcb"),
    "outline_mm": [0.0, 0.0, BOARD_W, BOARD_H],
    "title": "RCM - Relay Control Module / Keypad",
    "rev": "A",
    "footprint_dirs": [
        r"C:/Program Files/KiCad/10.0/share/kicad/footprints",
        os.path.join(HERE, "footprints"),
    ],
    "placement": placement,
    "groups": groups,
    "extra_footprints": [
        {"ref": "H%d" % i, "lib": "MountingHole",
         "name": "MountingHole_3.2mm_M3", "at": at, "exclude_bom": True}
        for i, at in enumerate(
            [[5.0, 33.0, 0], [135.0, 45.0, 0], [43.5, 62.5, 0], [87.5, 62.5, 0]], 1)
    ],
    "grid_start": [5.0, 55.0],
}

# The buck module body has NO courtyard (F.Fab only, deliberately), so plan_lint is
# blind to anything placed underneath it. Assert it here instead.
_sys_path = os.path.join(HERE, "tools")
sys.path.insert(0, _sys_path)
from boardlib import expand_placement  # noqa: E402
_body = (BUCK_X - 3.6, BUCK_Y - 3.6, BUCK_X - 3.6 + 33.0, BUCK_Y - 3.6 + 16.0)
_under = [r for r, q in expand_placement(plan).items()
          if r not in ("JB1", "JB2")
          and _body[0] <= q[0] <= _body[2] and _body[1] <= q[1] <= _body[3]]
if _under:
    raise SystemExit("parts under the buck module body %s: %s" % (_body, _under))
print("buck keep-out : clear (x %.1f-%.1f, y %.1f-%.1f)"
      % (_body[0], _body[2], _body[1], _body[3]))

with open(os.path.join(HERE, "board_plan.json"), "w", encoding="utf-8") as f:
    json.dump(plan, f, indent=1)

n = len(placement) + sum(len(i["refs"]) for g in groups for i in g["instances"])
print("placed        : %d refs" % n)
print("  explicit    : %d" % len(placement))
print("  via tiles   : %d (3 x %d)" % (n - len(placement), len(members)))
print("board         : %.0f x %.0f mm" % (BOARD_W, BOARD_H))
