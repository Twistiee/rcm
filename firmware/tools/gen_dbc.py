"""Emit docs/rcm.dbc -- the CAN database uaDASH and TunerStudio load to render this board.

Generated rather than hand-written for the same reason the schematic is: 8 nodes x 6
frames x ~25 signals is 1200 lines nobody will keep in step by hand, and a DBC that has
drifted from the firmware is worse than no DBC at all -- it displays confident, wrong
numbers.

The IDs here must match include/protocol.h. That is asserted at the bottom of this file
by parsing the header rather than by restating the constants.

Run: python firmware/tools/gen_dbc.py   ->  docs/rcm.dbc
"""
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.dirname(HERE)
REPO = os.path.dirname(FW)

CHANNELS = 21
NODES = 8            # 4 addresses x 2 roles
STRIDE = 0x10
GLOBAL_OFFSET = 0x80

# --- read the ids out of protocol.h so the two cannot drift -------------------
_hdr = open(os.path.join(FW, "include", "protocol.h"), encoding="utf-8").read()


def _define(name):
    m = re.search(r"#define\s+%s\s+(0x[0-9A-Fa-f]+|\d+)" % name, _hdr)
    if not m:
        raise SystemExit("protocol.h has no #define %s" % name)
    return int(m.group(1), 0)


BASE = _define("RCM_CAN_BASE_DEFAULT")
F_OUTPUTS = _define("RCM_F_OUTPUTS")
F_INPUTS = _define("RCM_F_INPUTS")
F_FAULTS = _define("RCM_F_FAULTS")
F_STATUS = _define("RCM_F_STATUS")
F_CMD_SET = _define("RCM_F_CMD_SET")
F_CMD_CTL = _define("RCM_F_CMD_CTL")
assert _define("RCM_CAN_NODE_STRIDE") == STRIDE
assert _define("RCM_CAN_GLOBAL_OFFSET") == GLOBAL_OFFSET

MM5_YAW_Y = _define("MM5_10_ID_YAW_Y")
MM5_ROLL_X = _define("MM5_10_ID_ROLL_X")
MM5_Z = _define("MM5_10_ID_Z")

out = []


def W(s=""):
    out.append(s)


def sig(name, start, length, signed=False, factor=1, offset=0,
         lo=0, hi=1, unit="", rx="Vector__XXX"):
    """One SG_ line. @1 is Intel (little-endian) byte order throughout -- the firmware
    packs everything little-endian and mixing the two in one file is a classic way to
    end up with a dash showing byte-swapped garbage."""
    return (' SG_ %s : %d|%d@1%s (%g,%g) [%g|%g] "%s" %s'
            % (name, start, length, "-" if signed else "+", factor, offset, lo, hi, unit, rx))


def msg(mid, name, dlc, sender, signals):
    W("BO_ %d %s: %d %s" % (mid, name, dlc, sender))
    for s in signals:
        W(s)
    W()


W('VERSION "RCM"')
W()
W("NS_ :")
W("    CM_")
W("    BA_DEF_")
W("    BA_")
W("    VAL_")
W("    BA_DEF_DEF_")
W()
W("BS_:")
W()
W("BU_: RCM ECU DASH")   # the colon is part of the keyword; without it no parser will
                         # touch the file
W()

comments = []

for node in range(NODES):
    nb = BASE + node * STRIDE
    role = "KP" if node & 4 else "RM"      # top bit of the node number is the role
    addr = node & 3
    p = "%s%d" % (role, addr)

    # --- outputs ------------------------------------------------------------
    s = [sig("%s_CH%02d_Out" % (p, c + 1), c, 1) for c in range(CHANNELS)]
    s += [
        sig("%s_OutputsEnabled" % p, 24, 1),
        sig("%s_Failsafe" % p, 25, 1),
        sig("%s_AnyFault" % p, 26, 1),
        sig("%s_ImuOk" % p, 27, 1),
        sig("%s_EepromOk" % p, 28, 1),
        sig("%s_RoleKeypad" % p, 29, 1),
        sig("%s_IgnitionOn" % p, 30, 1),
        sig("%s_Forced500k" % p, 31, 1),
        sig("%s_UptimeSec" % p, 32, 16, hi=65535, unit="s"),
        sig("%s_NodeId" % p, 48, 8, hi=7),
        sig("%s_Seq" % p, 56, 8, hi=255),
    ]
    msg(nb + F_OUTPUTS, "%s_Outputs" % p, 8, "RCM", s)

    # --- inputs -------------------------------------------------------------
    s = [sig("%s_CH%02d_In" % (p, c + 1), c, 1) for c in range(CHANNELS)]
    s += [sig("%s_Aux%d" % (p, a + 1), 24 + a, 1) for a in range(3)]
    s += [sig("%s_CH%02d_Sense" % (p, c + 1), 32 + c, 1) for c in range(CHANNELS)]
    s += [sig("%s_InSeq" % p, 56, 8, hi=255)]
    msg(nb + F_INPUTS, "%s_Inputs" % p, 8, "RCM", s)

    # --- faults -------------------------------------------------------------
    s = [sig("%s_CH%02d_Open" % (p, c + 1), c, 1) for c in range(CHANNELS)]
    s += [sig("%s_CH%02d_Short" % (p, c + 1), 24 + c, 1) for c in range(CHANNELS)]
    s += [sig("%s_FaultSeq" % p, 56, 8, hi=255)]
    msg(nb + F_FAULTS, "%s_Faults" % p, 8, "RCM", s)

    # --- status -------------------------------------------------------------
    s = [
        sig("%s_FwMajor" % p, 0, 8, hi=255),
        sig("%s_FwMinor" % p, 8, 8, hi=255),
        sig("%s_FwPatch" % p, 16, 8, hi=255),
        sig("%s_CanBusOff" % p, 24, 1),
        sig("%s_IgnitionMv" % p, 32, 16, hi=65535, unit="mV"),
        sig("%s_CanRxErr" % p, 48, 8, hi=255),
        sig("%s_CanTxErr" % p, 56, 8, hi=255),
    ]
    msg(nb + F_STATUS, "%s_Status" % p, 8, "RCM", s)

    # --- commands (received by us; here so a sender can be built from the DBC) --
    s = [sig("%s_SetMask%02d" % (p, c + 1), c, 1) for c in range(CHANNELS)]
    s += [sig("%s_SetVal%02d" % (p, c + 1), 24 + c, 1) for c in range(CHANNELS)]
    msg(nb + F_CMD_SET, "%s_CmdSet" % p, 6, "ECU", s)

    s = [
        sig("%s_CtlOpcode" % p, 0, 8, hi=255),
        sig("%s_CtlArg1" % p, 8, 8, hi=255),
        sig("%s_CtlArg2" % p, 16, 8, hi=255),
        sig("%s_CtlArg3" % p, 24, 8, hi=255),
        sig("%s_CtlArg4" % p, 32, 8, hi=255),
    ]
    msg(nb + F_CMD_CTL, "%s_CmdCtl" % p, 5, "ECU", s)

    comments.append('CM_ BO_ %d "RCM node %d (%s address %d) commanded output states";'
                    % (nb + F_OUTPUTS, node,
                       "keypad" if role == "KP" else "relay module", addr))

# --- global control ----------------------------------------------------------
gid = BASE + GLOBAL_OFFSET
msg(gid, "RCM_GlobalCtl", 5, "ECU", [
    sig("G_CtlOpcode", 0, 8, hi=255),
    sig("G_CtlArg1", 8, 8, hi=255),
    sig("G_CtlArg2", 16, 8, hi=255),
    sig("G_CtlArg3", 24, 8, hi=255),
    sig("G_CtlArg4", 32, 8, hi=255),
])

# --- Bosch MM5.10 emulation --------------------------------------------------
# Physical units come out directly here: rusEFI's own decode is (raw - 0x8000) * quant,
# which is exactly factor/offset in DBC terms.
RQ, AQ = 0.005, 0.0001274
msg(MM5_YAW_Y, "MM5_10_YawRate_AccY", 8, "RCM", [
    sig("YawRate", 0, 16, factor=RQ, offset=-0x8000 * RQ,
        lo=-163.84, hi=163.83, unit="deg/s", rx="ECU"),
    sig("AccelLateral", 32, 16, factor=AQ, offset=-0x8000 * AQ,
        lo=-4.17, hi=4.17, unit="g", rx="ECU"),
])
msg(MM5_ROLL_X, "MM5_10_RollRate_AccX", 8, "RCM", [
    sig("AccelLongitudinal", 32, 16, factor=AQ, offset=-0x8000 * AQ,
        lo=-4.17, hi=4.17, unit="g", rx="ECU"),
])
msg(MM5_Z, "MM5_10_AccZ", 8, "RCM", [
    sig("AccelVertical", 32, 16, factor=AQ, offset=-0x8000 * AQ,
        lo=-4.17, hi=4.17, unit="g", rx="ECU"),
])

comments.append('CM_ BO_ %d "Bosch MM5.10 emulation -- set imuType = IMU_MM5_10 in '
                'rusEFI and it decodes these with no further configuration";' % MM5_YAW_Y)
comments.append('CM_ BU_ RCM "CAN relay control module / keypad. '
                'Node id = DIP address, plus 4 when the role switch says keypad.";')

for c in comments:
    W(c)
W()

docs = os.path.join(REPO, "docs")
os.makedirs(docs, exist_ok=True)
path = os.path.join(docs, "rcm.dbc")
with open(path, "w", encoding="utf-8") as f:
    f.write("\n".join(out))

nmsg = sum(1 for line in out if line.startswith("BO_ "))
nsig = sum(1 for line in out if line.startswith(" SG_ "))
print("wrote %s" % path)
print("  base id   : 0x%03X  (nodes 0x%03X..0x%03X, global 0x%03X)"
      % (BASE, BASE, BASE + (NODES - 1) * STRIDE + 0xF, gid))
print("  messages  : %d" % nmsg)
print("  signals   : %d" % nsig)
