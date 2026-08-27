#!/usr/bin/env python3
"""rcm_bench -- talk to an RCM board from a PC over CAN.

Built so bring-up is something you RUN rather than something you type. The checklist in
firmware/README.md otherwise means commanding 21 channels one at a time and metering each
terminal; `rcm_bench walk` does that on its own with a pause on each.

Everything decodes through docs/rcm.dbc, so this tool and the dash see identical fields --
if a signal is wrong here it is wrong there too, which is the point.

    pip install python-can cantools

    # a CANable / candleLight in slcan mode, whatever COM port it enumerates as
    rcm_bench.py --interface slcan --channel COM5 monitor
    rcm_bench.py -i slcan -c COM5 set 3 on
    rcm_bench.py -i slcan -c COM5 walk

    # no hardware? --with-sim starts a fake board alongside whatever you asked for
    rcm_bench.py --interface virtual --channel rcm --with-sim walk

Interfaces are python-can's, so socketcan / pcan / kvaser / ixxat all work the same way.

Note that python-can's `virtual` bus only connects buses in the SAME PROCESS, which is
why --with-sim exists rather than telling you to open two terminals.
"""
import argparse
import os
import sys
import threading
import time

try:
    import can
    import cantools
except ImportError:
    sys.exit("needs python-can and cantools:  pip install python-can cantools")

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
DBC = os.path.join(REPO, "docs", "rcm.dbc")

CHANNELS = 21
STRIDE = 0x10
GLOBAL_OFFSET = 0x80

F_OUTPUTS, F_INPUTS, F_FAULTS, F_STATUS = 0x0, 0x1, 0x2, 0x3
F_CFG_REPLY = 0x4
F_CMD_SET, F_CMD_CTL = 0x8, 0x9

OP = {
    "alloff":       0x01,
    "enable":       0x02,
    "clearfaults":  0x03,
    "save":         0x04,
    "defaults":     0x05,
    "reboot":       0x06,
    "chmode":       0x10,
    "bitrate":      0x11,
    "baseid":       0x12,
    "timing":       0x13,
    "failsafe":     0x14,
    # mode(0=maintained 1=momentary), then brake / starter / run-signal input channels
    # and the RUN-position output, each 0-based or 255 for "not configured".
    "ignition":     0x15,
    # hold-to-stop ms, crank max ms, shutdown hold ms -- each a 16-bit LE pair.
    "igntimes":     0x16,
    # ECU RPM frame id (0 = none) then the rpm at or above which the engine counts as
    # running. rusEFI publishes RPM at base+1, so 0x201 for a stock base. This is what
    # makes hold-to-stop work: with no run source the board cannot tell a running
    # engine from a stopped one and treats every press as "switch off".
    "runsrc":       0x17,
    # peer node (0-7, or 255 for none), then the channels that follow that node, then
    # of those the ones where a PRESS TOGGLES rather than follows. Mirroring is strictly
    # channel-for-channel: peer channel N drives channel N here.
    "peer":         0x18,
    # bind an input channel to a TunerStudio command sent to the ECU over CAN.
    #   ecucmd <slot 0-5> <channel|none> <preset | subsystem index>
    "ecucmd":       0x19,
    # drive one of our channels from a bit in the ECU's broadcast.
    #   ecufollow <slot 0-5> <channel|none> <preset | frame-id bit>
    "ecufollow":    0x1C,
    # channel function label + behaviour + param.
    #   chfunc <channel> <func> <behaviour> [param-ms]
    # behaviour names depend on the channel's mode: outputs take
    # steady/flash/pulse/delayoff, inputs take momentary/toggle/holdarm.
    "chfunc":       0x1B,
}
MODES = {"unused": 0, "out": 1, "in": 2}
# The behaviour byte means whichever list applies to the channel's mode.
OUT_BEH = {"steady": 0, "flash": 1, "pulse": 2, "delayoff": 3}
IN_BEH = {"momentary": 0, "toggle": 1, "holdarm": 2}

# Only the four the FIRMWARE acts on are named here. The other ~47 labels are display
# text that lives in the firmware's chnames.cpp, and duplicating them would just create
# something to drift. These four are derived from the ignition block, never set by hand.
IGN_FUNCS = {1: "IGNITION", 2: "STARTER", 128: "IN_BRAKE", 129: "IN_ENGINE_RUN"}
# The roles the firmware LOOKS UP. Labelling a channel with one is how it is given that
# job -- there is no separate channel-number setting to keep in step.
FUNC_NAMES = {"ignition": 1, "starter": 2, "brake": 128, "enginerun": 129, "none": 0}

# Named (subsystem, index) pairs for the ECU command table. EVERY TunerStudio command is
# that shape -- see the cmd_* lines in rusefi's tunerstudio.template.ini -- so the
# firmware stores the pair and the names live here, where adding one costs nothing.
# Values read out of rusEFI's engine_types.h: ts_command_e for the subsystem,
# ts_14_command / bench_mode_e for the index.
ECU_CMDS = {
    # crank if stopped, stop if running. Lands on the same startStopButtonToggle() as
    # rusEFI's own physical start button, so the ECU applies its own interlocks.
    "startstop":  (20, 9),
    # unconditional stop, NOT gated on the ECU's view of engine speed.
    "stopengine": (36, 0),
    # bump a counter a rusEFI Lua script can watch. This is how you get traction control,
    # launch control, a map switch -- anything rusEFI has no fixed command for. Only 1-4
    # are handled by the firmware, despite the enum going further.
    "lua1":       (22, 33),
    "lua2":       (22, 34),
    "lua3":       (22, 35),
    "lua4":       (22, 36),
    # cancel any running bench test
    "benchcancel": (22, 15),
}

# (frame id, bit) for the ECU broadcast bits worth following. Straight out of rusEFI's
# rusEFI_CAN_verbose.dbc -- all the relay-shaped ones live in frame 0x200.
ECU_BITS = {
    "mainrelay":     (0x200, 33),
    "fuelpump":      (0x200, 34),
    "cel":           (0x200, 35),
    "egoheat":       (0x200, 36),
    "lambdaprotect": (0x200, 37),
    "fan":           (0x200, 38),
    "fan2":          (0x200, 39),
    "revlimit":      (0x200, 32),   # shift light
    "brakepedal":    (0x20B, 0),
}

MM5_IDS = {0x174: "IMU yaw+latG", 0x178: "IMU lonG", 0x17C: "IMU vertG"}


# --- packing -----------------------------------------------------------------

def pack21(v):
    """21 channel bits into 3 little-endian bytes, channel 1 = byte 0 bit 0."""
    return bytes([v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0x1F])


def unpack21(d, off=0):
    return d[off] | (d[off + 1] << 8) | ((d[off + 2] & 0x1F) << 16)


def bits_to_channels(v):
    return [i + 1 for i in range(CHANNELS) if v >> i & 1]


def fmt_bits(v):
    """21 channels as three groups of seven, matching the three physical tiles."""
    s = "".join("1" if v >> i & 1 else "." for i in range(CHANNELS))
    return " ".join(s[i:i + 7] for i in (0, 7, 14))


# --- the board ---------------------------------------------------------------

class Rcm:
    def __init__(self, bus, base, node, db=None):
        self.bus, self.base, self.node, self.db = bus, base, node, db

    @property
    def nb(self):
        return self.base + self.node * STRIDE

    def send(self, offset, data):
        self.bus.send(can.Message(arbitration_id=self.nb + offset,
                                  data=data, is_extended_id=False))

    def set_mask(self, mask, values):
        self.send(F_CMD_SET, pack21(mask) + pack21(values))

    def set_channel(self, ch, on):
        self.set_mask(1 << (ch - 1), (1 << (ch - 1)) if on else 0)

    def ctl(self, op, *args):
        self.send(F_CMD_CTL, bytes([op, *args]))

    def global_ctl(self, op, *args):
        self.bus.send(can.Message(arbitration_id=self.base + GLOBAL_OFFSET,
                                  data=bytes([op, *args]), is_extended_id=False))

    def flush(self):
        """Throw away everything already queued.

        Not optional before reading state back after a command. The board broadcasts
        every 50ms and nothing drains the receive queue between calls, so without this
        `state()` cheerfully returns frames captured BEFORE the command was sent -- and
        every read-back looks like the command did nothing. True of a real slcan
        adapter as much as of the simulator; both buffer."""
        while self.bus.recv(timeout=0) is not None:
            pass

    def recv(self, timeout=1.0):
        """Next frame from OUR node, decoded into a dict, or None on timeout."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            m = self.bus.recv(timeout=max(0.01, deadline - time.time()))
            if m is None:
                continue
            off = m.arbitration_id - self.nb
            if 0 <= off <= 3:
                return decode(off, m.data)
        return None

    def state(self, timeout=1.5, fresh=True):
        """Collect one of each broadcast frame. `fresh` discards the backlog first."""
        if fresh:
            self.flush()
        out = {}
        deadline = time.time() + timeout
        while time.time() < deadline and len(out) < 4:
            f = self.recv(timeout=max(0.01, deadline - time.time()))
            if f:
                out[f["frame"]] = f
        return out


STATUS_FLAGS = [
    (0x01, "outputs-live"), (0x02, "FAILSAFE"), (0x04, "FAULT"), (0x08, "imu"),
    (0x10, "eeprom"), (0x20, "keypad"), (0x40, "ign"), (0x80, "forced-500k"),
]


def decode(off, d):
    if off == F_OUTPUTS:
        return {"frame": "outputs", "bits": unpack21(d), "flags": d[3],
                "uptime": d[4] | d[5] << 8, "node": d[6], "seq": d[7]}
    if off == F_INPUTS:
        return {"frame": "inputs", "bits": unpack21(d), "aux": d[3] & 0x07,
                "raw": unpack21(d, 4), "seq": d[7]}
    if off == F_FAULTS:
        return {"frame": "faults", "open": unpack21(d), "short": unpack21(d, 3),
                "seq": d[7]}
    return {"frame": "status", "fw": "%d.%d.%d" % (d[0], d[1], d[2]),
            "busoff": bool(d[3] & 1), "ign_mv": d[4] | d[5] << 8,
            "rxerr": d[6], "txerr": d[7]}


def flag_str(f):
    return ",".join(n for b, n in STATUS_FLAGS if f & b) or "-"


# --- subcommands -------------------------------------------------------------

def cmd_scan(bus, args):
    """Listen for a few seconds and report every RCM node heard."""
    print("listening %.1fs for nodes on base 0x%03X ..." % (args.seconds, args.base))
    seen = {}
    deadline = time.time() + args.seconds
    while time.time() < deadline:
        m = bus.recv(timeout=max(0.01, deadline - time.time()))
        if m is None or m.is_extended_id:
            continue
        off = m.arbitration_id - args.base
        if 0 <= off < 8 * STRIDE and off % STRIDE == F_OUTPUTS:
            node = off // STRIDE
            f = decode(F_OUTPUTS, m.data)
            seen[node] = f
        elif m.arbitration_id in MM5_IDS:
            seen.setdefault("imu", m.arbitration_id)

    if not any(isinstance(k, int) for k in seen):
        print("  nothing. check the bitrate, the termination, and that the board is awake.")
        return 1
    for node, f in sorted((k, v) for k, v in seen.items() if isinstance(k, int)):
        role = "keypad" if node & 4 else "relay "
        print("  node %d  %s addr %d  id 0x%03X  up %ds  [%s]"
              % (node, role, node & 3, args.base + node * STRIDE,
                 f["uptime"], flag_str(f["flags"])))
    if "imu" in seen:
        print("  IMU frames present (Bosch MM5.10 emulation)")
    return 0


def cmd_monitor(bus, args):
    rcm = Rcm(bus, args.base, args.node)
    print("node %d, id 0x%03X. ctrl-c to stop.\n" % (args.node, rcm.nb))
    print("        %-25s %-25s" % ("1234567 8901234 5678901", ""))
    state = {}
    last = 0
    try:
        while True:
            m = bus.recv(timeout=0.5)
            if m is not None and not m.is_extended_id:
                off = m.arbitration_id - rcm.nb
                if 0 <= off <= 3:
                    f = decode(off, m.data)
                    state[f["frame"]] = f
            if time.time() - last < 0.25:
                continue
            last = time.time()
            o, i, fl, st = (state.get(k) for k in ("outputs", "inputs", "faults", "status"))
            if not o:
                continue
            print("out     %s   %s" % (fmt_bits(o["bits"]), flag_str(o["flags"])))
            if i:
                print("in      %s   aux %s" % (fmt_bits(i["bits"]),
                                               format(i["aux"], "03b")))
                print("sense   %s" % fmt_bits(i["raw"]))
            if fl and (fl["open"] or fl["short"]):
                if fl["open"]:
                    print("OPEN    %s   <- blown fuse / missing relay / broken wire"
                          % fmt_bits(fl["open"]))
                if fl["short"]:
                    print("SHORT   %s   <- low side not pulling down" % fmt_bits(fl["short"]))
            if st:
                print("fw %s  ign %.1fV  canerr rx%d tx%d%s  up %ds"
                      % (st["fw"], st["ign_mv"] / 1000.0, st["rxerr"], st["txerr"],
                         "  BUS-OFF" if st["busoff"] else "", o["uptime"]))
            print()
    except KeyboardInterrupt:
        return 0


def cmd_set(bus, args):
    rcm = Rcm(bus, args.base, args.node)
    on = args.state in ("on", "1", "true")
    if args.ch == "all":
        rcm.set_mask((1 << CHANNELS) - 1, (1 << CHANNELS) - 1 if on else 0)
        print("all channels %s" % ("ON" if on else "off"))
    else:
        ch = int(args.ch)
        if not 1 <= ch <= CHANNELS:
            sys.exit("channel must be 1..%d" % CHANNELS)
        rcm.set_channel(ch, on)
        print("channel %d %s" % (ch, "ON" if on else "off"))
    return 0


def cmd_walk(bus, args):
    """Bring-up step 5. One channel at a time, so a multimeter can follow along.

    This is the check worth doing by hand even though the unit tests cover the bit
    order: the tests verify the firmware against my reading of the netlist, and this
    verifies the netlist against the actual board."""
    rcm = Rcm(bus, args.base, args.node)
    print("walking %d channels, %.1fs each. meter each terminal in turn.\n"
          % (CHANNELS, args.dwell))
    rcm.set_mask((1 << CHANNELS) - 1, 0)
    time.sleep(0.2)
    bad = []
    for ch in range(1, CHANNELS + 1):
        rcm.set_channel(ch, True)
        time.sleep(args.dwell)
        st = rcm.state(timeout=0.6)
        o = st.get("outputs")
        got = o["bits"] if o else -1
        want = 1 << (ch - 1)
        ok = got == want
        if not ok:
            bad.append((ch, got))
        print("  ch %2d  %s  %s" % (ch, fmt_bits(got if got >= 0 else 0),
                                    "ok" if ok else "MISMATCH (reported %s)"
                                    % (bits_to_channels(got) if got >= 0 else "nothing")))
        rcm.set_channel(ch, False)
    rcm.ctl(OP["alloff"])
    if bad:
        print("\n%d channel(s) did not read back as themselves. If the pattern looks like"
              "\na constant offset of 14, the shift-register byte order is mirrored." % len(bad))
        return 1
    print("\nall %d channels reported themselves." % CHANNELS)
    return 0


def cmd_faults(bus, args):
    rcm = Rcm(bus, args.base, args.node)
    st = rcm.state()
    if "faults" not in st:
        print("no frames from node %d" % args.node)
        return 1
    f = st["faults"]
    if not f["open"] and not f["short"]:
        print("no faults.")
        return 0
    if f["open"]:
        print("open circuit : channels %s" % bits_to_channels(f["open"]))
        print("               blown fuse, missing relay, open coil or broken wire")
    if f["short"]:
        print("short to 12V : channels %s" % bits_to_channels(f["short"]))
        print("               the low side is not pulling down")
    return 0


# selector -> (name, index count, decoder). Mirrors RCM_OP_GET_CFG in protocol.h.
def _u16(d, i): return d[i] | (d[i + 1] << 8)


CFG_SEL = {
    0x00: ("ids", 1, lambda d: "base id 0x%03X, bitrate %d"
           % (_u16(d, 2), d[4] | (d[5] << 8) | (d[6] << 16) | (d[7] << 24))),
    0x01: ("timing", 1, lambda d: "broadcast %dms, can timeout %dms, debounce %dms"
           % (_u16(d, 2), _u16(d, 4), _u16(d, 6))),
    0x02: ("failsafe", 1, lambda d: "channels %s"
           % (_chlist(unpack21(d[2:5])) or "none")),
    0x03: ("ignition", 1, lambda d: "mode %s, brake %s, starter %s, run %s, RUN out %s, flags 0x%02X"
           % ("momentary" if d[2] else "maintained", _ch(d[3]), _ch(d[4]), _ch(d[5]),
              _ch(d[6]), d[7])),
    0x04: ("igntimes", 1, lambda d: "hold-to-stop %dms, crank max %dms, shutdown hold %dms"
           % (_u16(d, 2), _u16(d, 4), _u16(d, 6))),
    0x05: ("igntimes2", 1, lambda d: "ign-off hold %dms, idle timeout %ds, wake-start %dms"
           % (_u16(d, 2), _u16(d, 4), _u16(d, 6))),
    # NOTE the peer NODE is a node number 0-7, not a channel -- do not run it through
    # _ch(), which is 1-based. Read-back caught this reporting node 4 as "node 5".
    0x06: ("peer", 2, lambda d: ("node %s, follows %s"
                                 % ("none" if d[2] == 0xFF else d[2],
                                 _chlist(unpack21(d[3:6])) or "nothing"))
           if d[1] == 0 else "toggles %s" % (_chlist(unpack21(d[2:5])) or "nothing")),
    0x07: ("runsrc", 1, lambda d: "ECU rpm id %s, running at %d rpm"
           % (("0x%03X" % _u16(d, 2)) if _u16(d, 2) else "none", _u16(d, 4))),
    0x08: ("ecucmd", 6, lambda d: "channel %s -> subsystem %d index %d%s"
           % (_ch(d[2]), _u16(d, 3), _u16(d, 5), _cmd_name(_u16(d, 3), _u16(d, 5)))),
    0x0A: ("follow", 6, lambda d: "channel %s <- frame 0x%03X bit %d%s"
           % (_ch(d[2]), _u16(d, 4), d[3], _bit_name(_u16(d, 4), d[3]))),
    0x09: ("channel", CHANNELS, lambda d: "%-6s %-9s flags 0x%02X func %-14s param %d"
           % (["unused", "out", "in"][d[2]] if d[2] < 3 else "?",
              _beh_name(d[2], d[5]), d[3], _func_name(d[4]), _u16(d, 6))),
}


def _bit_name(cid, bit):
    for n, v in ECU_BITS.items():
        if v == (cid, bit):
            return "  (%s)" % n
    return ""


def _func_name(f):
    if f == 0:
        return "-"
    return IGN_FUNCS.get(f, str(f))


def _beh_name(mode, beh):
    table = IN_BEH if mode == 2 else OUT_BEH
    for n, v in table.items():
        if v == beh:
            return n
    return "?%d" % beh


def _chan_arg(a):
    """A channel as the user says it: 1-based, or 'none'."""
    if a in ("none", "off", "unassigned"):
        return 0xFF
    n = int(a, 0)
    if not 1 <= n <= CHANNELS:
        sys.exit("channel must be 1..%d, or 'none'" % CHANNELS)
    return n - 1


def _chlist(v):
    return ",".join(str(c) for c in bits_to_channels(v))


def _ch(v, none="unassigned"):
    return none if v == 0xFF else str(v + 1)


def _cmd_name(sub, idx):
    for n, (s_, i_) in ECU_CMDS.items():
        if (s_, i_) == (sub, idx):
            return "  (%s)" % n
    return ""


def cmd_get(bus, args):
    """Ask the board what it is configured as. Every other opcode is a setter, so
    without this the only way to check a board is to watch what it does."""
    rcm = Rcm(bus, args.base, args.node)
    wanted = [k for k, v in CFG_SEL.items()
              if args.what in ("all", v[0])]
    if not wanted:
        sys.exit("unknown section %r -- try: all, %s"
                 % (args.what, ", ".join(v[0] for v in CFG_SEL.values())))
    reply_id = args.base + args.node * STRIDE + F_CFG_REPLY
    for sel in sorted(wanted):
        name, count, decode = CFG_SEL[sel]
        for idx in range(count):
            rcm.flush()
            rcm.ctl(0x1A, sel, idx)
            d = None
            t0 = time.time()
            while time.time() - t0 < 0.5:
                m = bus.recv(timeout=0.1)
                if m and m.arbitration_id == reply_id and m.data[0] == sel                         and m.data[1] == idx:
                    d = bytes(m.data)
                    break
            if d is None:
                if count == 1:
                    print("  %-10s no reply" % name)
                continue
            # channels are 1-based everywhere else in this tool and on the board's
            # terminals; ecucmd slots are genuinely 0-based in the protocol.
            shown = idx + 1 if sel == 0x09 else idx
            label = name if count == 1 else "%s[%d]" % (name, shown)
            print("  %-12s %s" % (label, decode(d)))
    return 0


def cmd_ctl(bus, args):
    rcm = Rcm(bus, args.base, args.node)
    op = OP[args.op]
    # Parse per-opcode, NOT up front. The generic int() pass used to run first and blew
    # up on `chmode 1 in` -- the one opcode whose arguments are deliberately words --
    # before the branch that knows how to read them was ever reached.
    if args.op == "reboot":
        extra = [0xA5]
    elif args.op == "enable":
        extra = [int(args.args[0], 0)] if args.args else [1]
    elif args.op == "peer":
        # Masks are given as channel lists, not as three little-endian bytes: "peer 4
        # 1,2,5 5" is a great deal harder to get wrong than six hex numbers, and getting
        # the toggle mask wrong means a button latches when it should follow.
        if len(args.args) < 1:
            sys.exit("peer <node|none> [follow-channels] [toggle-channels] -- "
                     "channels are comma-separated, 1-21, 'all' or 'none'")
        node = 0xFF if args.args[0] in ("none", "off") else int(args.args[0], 0)
        if node != 0xFF and not 0 <= node <= 7:
            sys.exit("peer node must be 0..7, or 'none'")

        def chlist(spec):
            if spec in ("none", "", "0"):
                return 0
            if spec == "all":
                return (1 << CHANNELS) - 1
            v = 0
            for part in spec.split(","):
                ch = int(part, 0)
                if not 1 <= ch <= CHANNELS:
                    sys.exit("channel must be 1..%d" % CHANNELS)
                v |= 1 << (ch - 1)
            return v

        follow = chlist(args.args[1]) if len(args.args) > 1 else 0
        toggle = chlist(args.args[2]) if len(args.args) > 2 else 0
        if toggle & ~follow:
            sys.exit("toggle channels must also be follow channels: %s are not"
                     % ",".join(str(c + 1) for c in range(CHANNELS)
                                if (toggle & ~follow) >> c & 1))
        extra = [node, *pack21(follow), *pack21(toggle)]
    elif args.op == "ignition":
        # Mode and flags only. WHICH channel does which job is said once, by labelling
        # that channel -- `ctl chfunc <ch> brake ...`. There is no second setting here
        # to disagree with the label.
        if not args.args:
            sys.exit("ignition <maintained|momentary> [ecu-flags]  -- assign roles with "
                     "`ctl chfunc <channel> <brake|starter|enginerun|ignition> ...`")
        mode = {"maintained": 0, "momentary": 1}.get(args.args[0])
        if mode is None:
            mode = int(args.args[0], 0)
        flags = int(args.args[1], 0) if len(args.args) > 1 else 0
        extra = [mode, flags]
    elif args.op == "runsrc":
        # <can id> <rpm>, not four raw bytes.
        if len(args.args) < 1:
            sys.exit("runsrc <ecu-rpm-can-id|none> [running-rpm]")
        cid = 0 if args.args[0] in ("none", "off") else int(args.args[0], 0)
        if cid > 0x7FF:
            sys.exit("an 11-bit standard id is 0..0x7FF")
        rpm = int(args.args[1], 0) if len(args.args) > 1 else 0
        extra = [cid & 0xFF, cid >> 8, rpm & 0xFF, rpm >> 8]
    elif args.op == "ecufollow":
        if len(args.args) < 2:
            sys.exit("ecufollow <slot 0-5> <channel|none> <%s | frame-id bit>"
                     % "|".join(sorted(ECU_BITS)))
        slot = int(args.args[0], 0)
        ch = _chan_arg(args.args[1])
        if len(args.args) >= 3 and args.args[2] in ECU_BITS:
            cid, bit = ECU_BITS[args.args[2]]
        elif len(args.args) >= 4:
            cid, bit = int(args.args[2], 0), int(args.args[3], 0)
        elif ch == 0xFF:
            cid, bit = 0, 0
        else:
            sys.exit("give a preset (%s) or a frame id and bit"
                     % ", ".join(sorted(ECU_BITS)))
        extra = [slot, ch, bit, cid & 0xFF, cid >> 8]
    elif args.op == "chfunc":
        if len(args.args) < 3:
            sys.exit("chfunc <channel> <%s|number> <%s|%s> [param-ms]"
                     % ("|".join(FUNC_NAMES), "|".join(OUT_BEH), "|".join(IN_BEH)))
        ch = _chan_arg(args.args[0])
        if ch == 0xFF:
            sys.exit("chfunc needs a real channel")
        fn = args.args[1]
        func = FUNC_NAMES.get(fn)
        if func is None:
            func = int(fn, 0)
        beh = args.args[2]
        if beh in OUT_BEH:   bval = OUT_BEH[beh]
        elif beh in IN_BEH:  bval = IN_BEH[beh]
        else:                bval = int(beh, 0)
        param = int(args.args[3], 0) if len(args.args) > 3 else 0
        extra = [ch, func, bval, param & 0xFF, param >> 8]
    elif args.op == "ecucmd":
        if len(args.args) < 2:
            sys.exit("ecucmd <slot 0-%d> <channel|none> <%s | subsystem index>"
                     % (5, "|".join(sorted(ECU_CMDS))))
        slot = int(args.args[0], 0)
        ch = 0xFF if args.args[1] in ("none", "off") else int(args.args[1], 0) - 1
        if ch != 0xFF and not 0 <= ch < CHANNELS:
            sys.exit("channel must be 1..%d, or 'none'" % CHANNELS)
        if len(args.args) >= 3 and args.args[2] in ECU_CMDS:
            sub, idx = ECU_CMDS[args.args[2]]
        elif len(args.args) >= 4:
            sub, idx = int(args.args[2], 0), int(args.args[3], 0)
        elif ch == 0xFF:
            sub, idx = 0, 0          # clearing a slot needs no command
        else:
            sys.exit("give a preset name (%s) or a raw subsystem and index"
                     % ", ".join(sorted(ECU_CMDS)))
        extra = [slot, ch, sub & 0xFF, sub >> 8, idx & 0xFF, idx >> 8]
    elif args.op == "chmode":
        if len(args.args) < 2:
            sys.exit("chmode <channel> <out|in|unused> [flags]")
        if args.args[1] not in MODES:
            sys.exit("mode must be one of: %s" % ", ".join(MODES))
        ch = int(args.args[0], 0)
        if not 1 <= ch <= CHANNELS:
            sys.exit("channel must be 1..%d" % CHANNELS)
        flags = int(args.args[2], 0) if len(args.args) > 2 else 0
        extra = [ch - 1, MODES[args.args[1]], flags]
    else:
        extra = [int(a, 0) for a in args.args]
    (rcm.global_ctl if args.glob else rcm.ctl)(op, *extra)
    print("%s%s %s" % ("global " if args.glob else "", args.op,
                       " ".join(str(e) for e in extra)))
    return 0


def cmd_dump(bus, args):
    """Every RCM-ish frame, raw and decoded, for when something is not adding up."""
    db = cantools.database.load_file(DBC) if os.path.exists(DBC) else None
    print("raw dump, ctrl-c to stop")
    try:
        while True:
            m = bus.recv(timeout=1.0)
            if m is None:
                continue
            name = ""
            if db:
                try:
                    msg = db.get_message_by_frame_id(m.arbitration_id)
                    name = msg.name
                except KeyError:
                    name = MM5_IDS.get(m.arbitration_id, "")
            print("0x%03X  %-22s %s" % (m.arbitration_id, name, m.data.hex(" ")))
    except KeyboardInterrupt:
        return 0


# --- simulator ---------------------------------------------------------------

def cmd_sim(bus, args, stop=None):
    """A fake RCM node, so the tool above can be exercised with no hardware.

    Not a second implementation of the firmware -- it is deliberately shallow, and it
    exists to prove the FRAME LAYOUTS in this file agree with the ones in protocol.cpp.
    A real board will disagree with it about timing and diagnosis, and should.
    """
    base, node = args.base, args.node
    nb = base + node * STRIDE
    out = 0
    modes = [1] * CHANNELS
    print("simulating node %d at 0x%03X. ctrl-c to stop." % (node, nb))
    t0 = time.time()
    seq = 0
    nxt = 0.0
    try:
        while stop is None or not stop.is_set():
            m = bus.recv(timeout=0.02)
            if m is not None and not m.is_extended_id:
                off = m.arbitration_id - nb
                if off == F_CMD_SET and len(m.data) >= 6:
                    mask, val = unpack21(m.data), unpack21(m.data, 3)
                    for i in range(CHANNELS):
                        if mask >> i & 1 and modes[i] == 1:
                            out = (out | 1 << i) if val >> i & 1 else (out & ~(1 << i))
                elif off == F_CMD_CTL or m.arbitration_id == base + GLOBAL_OFFSET:
                    op = m.data[0]
                    if op == OP["alloff"]:
                        out = 0
                    elif op == OP["chmode"] and len(m.data) >= 3:
                        modes[m.data[1]] = m.data[2]
                        out &= ~(1 << m.data[1])

            if time.time() < nxt:
                continue
            nxt = time.time() + 0.05
            up = int(time.time() - t0)
            # A healthy board: every output channel's coil circuit intact, so an
            # un-driven channel senses HIGH and a driven one senses LOW.
            raw = 0
            for i in range(CHANNELS):
                if modes[i] == 1 and not (out >> i & 1):
                    raw |= 1 << i
            flags = 0x01 | 0x10 | 0x40           # outputs live, eeprom ok, ignition on
            bus.send(can.Message(arbitration_id=nb + F_OUTPUTS, is_extended_id=False,
                                 data=pack21(out) + bytes([flags, up & 0xFF, up >> 8,
                                                           node, seq])))
            bus.send(can.Message(arbitration_id=nb + F_INPUTS, is_extended_id=False,
                                 data=pack21(0) + bytes([0]) + pack21(raw) + bytes([seq])))
            bus.send(can.Message(arbitration_id=nb + F_FAULTS, is_extended_id=False,
                                 data=pack21(0) + pack21(0) + bytes([0, seq])))
            bus.send(can.Message(arbitration_id=nb + F_STATUS, is_extended_id=False,
                                 data=bytes([0, 2, 0, 0, 0x8C, 0x31, 0, 0])))
            seq = (seq + 1) & 0xFF
    except KeyboardInterrupt:
        return 0


# --- main --------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-i", "--interface", default="slcan",
                    help="python-can interface: slcan, socketcan, pcan, virtual ...")
    ap.add_argument("-c", "--channel", default="COM5",
                    help="port or channel name for that interface")
    ap.add_argument("-b", "--bitrate", type=int, default=500000)
    ap.add_argument("-n", "--node", type=int, default=0, help="0..7")
    ap.add_argument("--base", type=lambda s: int(s, 0), default=0x300)
    ap.add_argument("--with-sim", action="store_true",
                    help="run a fake board in a background thread on the same bus, so "
                         "the tool can be exercised with no hardware. Only makes sense "
                         "with --interface virtual.")

    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("scan", help="find nodes on the bus").add_argument(
        "--seconds", type=float, default=3.0)
    sub.add_parser("monitor", help="live state, inputs, sense and faults")
    sub.add_parser("faults", help="one-shot fault report")
    sub.add_parser("dump", help="raw frames, decoded through the DBC")
    sub.add_parser("sim", help="pretend to be a board, for testing this tool")

    p = sub.add_parser("set", help="drive a channel")
    # dest MUST NOT be "channel": that is the global --channel, which names the CAN
    # adapter's port. argparse lets a positional silently overwrite it, and the failure
    # lands somewhere unrelated -- "could not open slcan:21" while trying to open the bus.
    p.add_argument("ch", metavar="channel", help="1..21, or 'all'")
    p.add_argument("state", choices=["on", "off", "1", "0", "true", "false"])

    p = sub.add_parser("walk", help="bring-up: drive each channel in turn")
    p.add_argument("--dwell", type=float, default=1.0)

    p = sub.add_parser("get", help="read configuration back off the board")
    p.add_argument("what", nargs="?", default="all",
                   help="all, or one section: ids timing failsafe ignition igntimes "
                        "igntimes2 peer runsrc ecucmd channel")

    p = sub.add_parser("ctl", help="control opcode")
    p.add_argument("op", choices=sorted(OP))
    p.add_argument("args", nargs="*")
    p.add_argument("-g", "--global", dest="glob", action="store_true",
                   help="send to the global id, so every node acts on it")

    args = ap.parse_args()

    kw = {"interface": args.interface, "channel": args.channel}
    if args.interface == "gs_usb":
        # candleLight firmware. python-can addresses these by DEVICE INDEX, not by a
        # port name, and it needs `pip install "python-can[gs-usb]"` plus a WinUSB
        # driver (Zadig) on Windows. Prefer slcan unless you have a reason not to.
        kw["channel"] = int(args.channel) if str(args.channel).isdigit() else 0
        kw["index"] = 0
        kw["bitrate"] = args.bitrate
    elif args.interface != "virtual":
        kw["bitrate"] = args.bitrate
    try:
        bus = can.Bus(**kw)
    except Exception as e:
        sys.exit("could not open %s:%s -- %s" % (args.interface, args.channel, e))

    # Same reasoning at the other end: an slcan adapter has only just been sent
    # C / S<rate> / O, and a frame written into that gap can be dropped.
    if args.interface not in ("virtual",):
        time.sleep(0.15)

    sim_bus, stop, thread = None, None, None
    if args.with_sim:
        sim_bus = can.Bus(**kw)
        stop = threading.Event()
        thread = threading.Thread(target=cmd_sim, args=(sim_bus, args, stop), daemon=True)
        thread.start()
        time.sleep(0.2)               # let it get a first broadcast out

    fn = {"scan": cmd_scan, "monitor": cmd_monitor, "set": cmd_set, "walk": cmd_walk,
          "faults": cmd_faults, "ctl": cmd_ctl, "get": cmd_get, "dump": cmd_dump, "sim": cmd_sim}[args.cmd]
    try:
        return fn(bus, args)
    finally:
        if stop:
            stop.set()
            thread.join(timeout=1.0)
            sim_bus.shutdown()
        # Let anything just transmitted actually reach the wire before tearing the bus
        # down. slcan's shutdown() closes the CAN channel with a `C` command, and a frame still
        # queued in the adapter dies with it -- so a one-shot `set` or `ctl` would print
        # a cheerful confirmation and change nothing at all. Cost 150ms on commands that
        # exit immediately; the streaming ones were never affected.
        time.sleep(0.15)
        bus.shutdown()


if __name__ == "__main__":
    sys.exit(main())
