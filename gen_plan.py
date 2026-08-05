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

HERE = os.path.dirname(os.path.abspath(__file__))

BOARD_W, BOARD_H = 100.0, 100.0
PITCH = 3.5              # terminal pole pitch, and the channel column pitch
TILE_PITCH = 30.0        # x distance between tile anchors
TILE_X0 = 8.0            # x of tile 1 pin 1
TILE_Y = 95.0            # y of the channel terminal pin row (south edge)

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

members.append((10.5, -25.0, 0))                  # U_DRV  (TSSOP-16)
members.append((10.5, -33.0, 90))                 # U_SO (SOIC-16, laid
members.append((10.5, -41.0, 90))                 # U_SI  side-on: 10mm
members.append((2.0, -37.0, 90))                  # C_SO   wide, 6mm tall)
members.append((19.0, -37.0, 90))                 # C_SI decoupling
for t in range(3):
    n = t + 1
    refs_for[t] += ["U_DRV%d" % n, "U_SO%d" % n, "U_SI%d" % n,
                    "C_SO%d" % n, "C_SI%d" % n]

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
P("J_PWR", 6.0, 5.0, 180)
P("J_IGN", 16.0, 5.0, 180)
P("F1", 6.0, 13.0, 90)
P("D1", 10.0, 13.0, 90)
P("D2", 14.0, 13.0, 90)
P("C_BULK", 18.0, 13.0, 90)

P("U_LATCH", 30.0, 10.0, 0)
P("R_LG", 26.0, 16.0, 0)
P("R_LPD", 30.0, 16.0, 0)
P("R_LIGN", 34.0, 16.0, 0)
P("R_LHOLD", 26.0, 20.0, 0)
P("C_LVS", 30.0, 20.0, 0)
P("C_LSW", 34.0, 20.0, 0)
P("R_IGH", 22.0, 16.0, 0)
P("R_IGL", 22.0, 20.0, 0)

# Waveshare buck. The two columns are 27.80mm apart in X and offset 1.90mm in Y:
# on the module drawing the IN column's first hole is 3.60mm from the top edge and
# the OUT column's is 1.70mm, so OUT leads IN by 1.90mm. Getting this wrong means
# the module physically does not fit, and no checker catches it -- the two
# footprints are separate parts whose courtyards never touch.
BUCK_X, BUCK_Y = 8.0, 28.0
P("JB1", BUCK_X, BUCK_Y, 0)                       # IN  column, 4 holes @3.50
P("JB2", BUCK_X + 27.8, BUCK_Y - 1.9, 0)          # OUT column, 6 holes @2.54
# Resulting module body: x 4.4..37.4, y 24.4..40.4. KEEP THIS AREA CLEAR -- the
# body is drawn on F.Fab only (no courtyard), so plan_lint cannot see it.
P("U_LDO", 8.0, 45.0, 0)
P("C_5V", 15.0, 45.0, 90)
P("C_3V3I", 19.0, 45.0, 90)
P("C_3V3O", 23.0, 45.0, 90)

# ---------------------------------------------------------------------------
# North-centre -- MCU and its support
# ---------------------------------------------------------------------------
MX, MY = 58.0, 26.0
P("U_MCU", MX, MY, 0)
for i, ref in enumerate(["C_M1", "C_M2", "C_M3", "C_M4", "C_M5"]):
    P(ref, MX - 8.0 + i * 4.0, MY - 9.5, 0)
P("C_MB", MX, MY - 13.5, 0)
P("C_VCAP", MX - 9.5, MY + 4.0, 90)
P("C_VDDA", MX - 9.5, MY, 90)
P("C_NRST", MX + 9.5, MY, 90)
P("R_BOOT", MX + 9.5, MY + 4.0, 90)

P("Y1", MX - 5.0, MY + 10.0, 0)
P("C_Y1A", MX - 9.5, MY + 8.5, 90)
P("C_Y1B", MX - 9.5, MY + 12.0, 90)
P("Y2", MX + 5.0, MY + 10.0, 0)
P("C_Y2A", MX + 9.5, MY + 8.5, 90)
P("C_Y2B", MX + 9.5, MY + 12.0, 90)

P("SW_RST", MX - 16.0, MY + 11.0, 0)
P("J_SWD", MX - 6.0, MY + 17.0, 90)
P("J_BOOT", MX + 9.0, MY + 17.0, 90)
P("D_LED1", MX - 16.0, MY + 4.0, 0)
P("R_LED1", MX - 16.0, MY + 0.5, 0)
P("D_LED2", MX - 16.0, MY - 3.0, 0)
P("R_LED2", MX - 16.0, MY - 6.5, 0)

# ---------------------------------------------------------------------------
# North-east -- CAN, IMU, EEPROM, USB, aux inputs
# ---------------------------------------------------------------------------
P("J_CAN1", 73.0, 5.0, 180)
P("J_CAN2", 85.0, 5.0, 180)
P("J_AUX", 97.0, 5.0, 180)

P("U_CAN", 75.0, 14.0, 0)
P("C_CAN", 81.0, 14.0, 90)
P("R_TERM", 73.0, 20.0, 0)
P("R_TJ", 77.0, 20.0, 0)
P("R_RS", 81.0, 20.0, 0)
P("D_CAN", 85.0, 20.0, 0)

# aux-input dividers, directly under their own J_AUX poles
P("R_AH1", 89.0, 12.0, 90)
P("R_AH2", 93.0, 12.0, 90)
P("R_AH3", 97.0, 12.0, 90)
P("R_AL1", 89.0, 16.0, 90)
P("R_AL2", 93.0, 16.0, 90)
P("R_AL3", 97.0, 16.0, 90)

P("U_IMU", 75.0, 28.0, 0)
P("C_IMU1", 81.0, 27.0, 90)
P("C_IMU2", 81.0, 30.5, 90)
P("R_ADDR", 85.0, 27.0, 90)
P("R_ADDR_ALT", 85.0, 30.5, 90)
P("R_SDA", 89.0, 27.0, 90)
P("R_SCL", 89.0, 30.5, 90)

P("U_EEP", 75.0, 38.0, 0)
P("C_EEP", 81.0, 38.0, 90)
P("R_OE", 85.0, 38.0, 90)

P("J_USB", 92.0, 40.0, 0)
P("R_CC1", 84.0, 42.0, 90)
P("R_CC2", 84.0, 45.5, 90)

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
            [[4.0, 60.0, 0], [96.0, 60.0, 0], [32.0, 45.0, 0], [96.0, 96.0, 0]], 1)
    ],
    "grid_start": [5.0, 55.0],
}

with open(os.path.join(HERE, "board_plan.json"), "w", encoding="utf-8") as f:
    json.dump(plan, f, indent=1)

n = len(placement) + sum(len(i["refs"]) for g in groups for i in g["instances"])
print("placed        : %d refs" % n)
print("  explicit    : %d" % len(placement))
print("  via tiles   : %d (3 x %d)" % (n - len(placement), len(members)))
print("board         : %.0f x %.0f mm" % (BOARD_W, BOARD_H))
