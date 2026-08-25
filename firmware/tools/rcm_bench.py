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
}
MODES = {"unused": 0, "out": 1, "in": 2}

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


def cmd_ctl(bus, args):
    rcm = Rcm(bus, args.base, args.node)
    op = OP[args.op]
    extra = [int(a, 0) for a in args.args]
    if args.op == "reboot":
        extra = [0xA5]
    if args.op == "enable":
        extra = [1 if not extra else extra[0]]
    if args.op == "chmode":
        if len(args.args) < 2:
            sys.exit("chmode <channel> <out|in|unused> [flags]")
        ch = int(args.args[0])
        extra = [ch - 1, MODES[args.args[1]], int(args.args[2], 0) if len(args.args) > 2 else 0]
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

    sim_bus, stop, thread = None, None, None
    if args.with_sim:
        sim_bus = can.Bus(**kw)
        stop = threading.Event()
        thread = threading.Thread(target=cmd_sim, args=(sim_bus, args, stop), daemon=True)
        thread.start()
        time.sleep(0.2)               # let it get a first broadcast out

    fn = {"scan": cmd_scan, "monitor": cmd_monitor, "set": cmd_set, "walk": cmd_walk,
          "faults": cmd_faults, "ctl": cmd_ctl, "dump": cmd_dump, "sim": cmd_sim}[args.cmd]
    try:
        return fn(bus, args)
    finally:
        if stop:
            stop.set()
            thread.join(timeout=1.0)
            sim_bus.shutdown()
        bus.shutdown()


if __name__ == "__main__":
    sys.exit(main())
