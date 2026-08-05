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
PITCH = 3.5              # terminal pole pitch
RPITCH = 6.0             # channel column pitch -- resistors spread wider than the
                         # terminal poles so the rows breathe and leave routing room
TILE_PITCH = 44.0        # x distance between tile anchors
TILE_X0 = 8.8            # x of tile 1 column 1 (~7.9mm margin each end)
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
# Members are (dx, dy, rot, ref-template) tuples so the offset and the ref it belongs
# to can never drift apart. expand_placement pairs members[i] with refs[i] positionally,
# and keeping those as two separate lists silently swapped parts into each other's slots
# twice during this layout -- both times it only surfaced because the two happened to
# collide. A reorder that landed somewhere legal would have passed every check.
TILE = [
    # terminal, centred under the 36mm-wide spread of resistor columns above it
    (7.5, 0.0, 0, "J_CH%d"),
]
for _k in range(7):
    _dx = _k * RPITCH
    TILE.append((_dx, -8.0, 90, "R_SH{ch%d}" % _k))
    TILE.append((_dx, -12.5, 90, "R_SL{ch%d}" % _k))
    TILE.append((_dx, -17.0, 90, "R_PU{ch%d}" % _k))
# Driver and both shift registers in a row NORTH of the resistors (user, 2026-08-05),
# spread across the same 36mm the resistor columns now occupy.
TILE += [
    (3.0, -25.0, 0, "U_DRV%d"),
    (14.0, -25.0, 90, "U_SO%d"),
    (20.5, -25.0, 90, "C_SO%d"),
    (28.0, -25.0, 90, "U_SI%d"),
    (34.5, -25.0, 90, "C_SI%d"),
]

members = [(m[0], m[1], m[2]) for m in TILE]
refs_for = [[], [], []]
for t in range(3):
    for m in TILE:
        tmpl = m[3]
        if "{ch" in tmpl:
            k = int(tmpl.split("{ch")[1].split("}")[0])
            refs_for[t].append(tmpl.split("{")[0] + str(t * 7 + k + 1))
        else:
            refs_for[t].append(tmpl % (t + 1))

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
# North-edge terminals, centred as a group on that edge (rot 180, so the body runs
# from anchor-5.25 for a 2P / anchor-8.75 for a 3P, out to anchor+1.75).
P("J_PWR", 36.5, 5.0, 180)
P("J_IGN", 51.5, 5.0, 180)
P("J_AUX", 70.0, 5.0, 180)
P("J_CAN1", 88.5, 5.0, 180)
P("J_CAN2", 107.0, 5.0, 180)

# --- north-west: protection + latch, under the free corner ---
P("F1", 6.0, 14.0, 90)
P("D1", 10.0, 14.0, 90)
P("D2", 14.0, 14.0, 90)
P("C_BULK", 18.0, 14.0, 90)
P("U_LATCH", 12.0, 22.0, 0)
P("R_IGH", 5.0, 29.0, 0)
P("R_LG", 9.0, 29.0, 0)
P("R_LPD", 13.0, 29.0, 0)
P("R_LIGN", 17.0, 29.0, 0)
P("R_IGL", 5.0, 34.5, 0)
P("R_LHOLD", 9.0, 34.5, 0)
P("C_LVS", 13.0, 34.5, 0)
P("C_LSW", 17.0, 34.5, 0)

# --- buck: columns 27.80mm apart in X, offset 1.90mm in Y. Body x 26..59, y 12..28 ---
BUCK_X, BUCK_Y = 29.6, 15.6
P("JB1", BUCK_X, BUCK_Y, 0)
P("JB2", BUCK_X + 27.8, BUCK_Y - 1.9, 0)
P("U_LDO", 28.0, 31.0, 0)
P("C_5V", 35.0, 31.0, 90)
P("C_3V3I", 39.0, 31.0, 90)
P("C_3V3O", 43.0, 31.0, 90)
P("SW_RST", 50.0, 32.0, 0)
P("D_LED1", 23.0, 5.0, 0)    # beside PWR_IN, per user
P("R_LED1", 23.0, 9.0, 0)
P("D_LED2", 28.0, 5.0, 0)
P("R_LED2", 28.0, 9.0, 0)

# --- MCU, centre-east under the terminals ---
MX, MY = 80.0, 19.0
P("U_MCU", MX, MY, 0)
P("Y1", MX - 16.0, MY - 4.0, 0)
P("C_Y1A", MX - 16.0, MY - 8.5, 0)
P("C_Y1B", MX - 16.0, MY + 0.5, 0)
P("Y2", MX + 16.0, MY - 4.0, 0)
P("C_Y2A", MX + 16.0, MY - 8.5, 0)
P("C_Y2B", MX + 16.0, MY + 0.5, 0)
for i, ref in enumerate(["C_M1", "C_M2", "C_M3", "C_M4", "C_M5"]):
    P(ref, MX - 8.0 + i * 4.0, MY - 8.5, 0)   # above the MCU, under the terminals
P("C_MB", 96.0, 29.0, 0)
P("C_VCAP", MX - 10.5, MY - 2.0, 90)
P("C_VDDA", MX - 10.5, MY + 2.0, 90)
P("C_NRST", MX + 10.5, MY - 2.0, 90)
P("R_BOOT", MX + 10.5, MY + 2.0, 90)
P("J_SWD", 60.0, 32.0, 90)
P("J_BOOT", 62.0, 28.0, 90)

# --- far east: CAN, IMU, EEPROM, USB, aux dividers ---
P("R_AH1", 99.0, 12.0, 90)
P("R_AH2", 103.0, 12.0, 90)
P("R_AH3", 107.0, 12.0, 90)
P("R_AL1", 99.0, 16.0, 90)
P("R_AL2", 103.0, 16.0, 90)
P("R_AL3", 107.0, 16.0, 90)

P("U_CAN", 116.0, 13.0, 0)
P("C_CAN", 122.0, 13.0, 90)
P("R_TERM", 114.0, 19.0, 0)
P("R_TJ", 118.0, 19.0, 0)
P("R_RS", 122.0, 19.0, 0)
P("D_CAN", 110.0, 19.0, 0)

P("U_IMU", 116.0, 26.0, 0)
P("C_IMU1", 122.0, 25.0, 90)
P("C_IMU2", 122.0, 28.5, 90)
P("R_ADDR", 126.0, 25.0, 90)
P("R_ADDR_ALT", 126.0, 28.5, 90)
P("R_SDA", 130.0, 25.0, 90)
P("R_SCL", 130.0, 28.5, 90)

P("U_EEP", 107.0, 31.0, 0)
P("C_EEP", 113.0, 31.0, 90)
P("R_OE", 136.0, 30.0, 90)

P("J_USB", 133.0, 17.0, 0)
P("R_CC1", 128.0, 24.0, 90)
P("R_CC2", 132.0, 24.0, 90)

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
            [[5.0, 5.0, 0], [135.0, 5.0, 0], [5.0, 65.0, 0], [135.0, 65.0, 0]], 1)
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
