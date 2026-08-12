#!/usr/bin/env python3
"""Self-test for rcm_bench.py. No hardware, no CAN adapter.

Two jobs:

  1. Exercise the tool end to end against its own simulator, so `walk` and friends are
     known to work before there is a board to point them at.

  2. Cross-check the tool's byte packing against docs/rcm.dbc -- which gen_dbc.py builds
     from protocol.h. That closes the loop: bench tool <-> DBC <-> firmware. If any one
     of the three moves, this fails.

Run: python firmware/tools/test_rcm_bench.py
"""
import os
import sys
import threading
import time

import can
import cantools

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import rcm_bench as R  # noqa: E402

DBC = os.path.join(os.path.dirname(os.path.dirname(HERE)), "docs", "rcm.dbc")

fails = []


def check(name, cond, detail=""):
    print("  %-58s %s" % (name, "ok" if cond else "FAIL"))
    if not cond:
        fails.append("%s %s" % (name, detail))


class Args:
    base, node = 0x300, 0


# --- 1. packing agrees with the DBC ------------------------------------------

def test_against_dbc():
    print("packing vs docs/rcm.dbc (generated from protocol.h)")
    db = cantools.database.load_file(DBC)
    out = db.get_message_by_name("RM0_Outputs")
    check("Outputs frame id matches the tool's node base", out.frame_id == 0x300,
          "dbc says 0x%03X" % out.frame_id)

    # Every channel, one at a time, through pack21 and back out via the DBC.
    bad = []
    for ch in range(1, R.CHANNELS + 1):
        raw = R.pack21(1 << (ch - 1)) + bytes(5)
        sig = out.decode(raw)
        on = [n for n, v in sig.items() if n.endswith("_Out") and v]
        if on != ["RM0_CH%02d_Out" % ch]:
            bad.append((ch, on))
    check("all 21 channels pack to the signal the DBC expects", not bad, str(bad[:3]))

    # And the reverse: the DBC's own encoder must agree with unpack21.
    enc = out.encode({s.name: 0 for s in out.signals}
                     | {"RM0_CH01_Out": 1, "RM0_CH21_Out": 1})
    check("unpack21 reads the DBC's own encoding",
          R.unpack21(enc) == (1 << 0) | (1 << 20), hex(R.unpack21(enc)))

    inp = db.get_message_by_name("RM0_Inputs")
    check("Inputs raw-sense block is at byte 4",
          inp.get_signal_by_name("RM0_CH01_Sense").start == 32)

    imu = db.get_message_by_name("MM5_10_YawRate_AccY")
    check("IMU frame is at the id rusEFI listens on (0x174)", imu.frame_id == 0x174)
    yaw = imu.decode(imu.encode({"YawRate": 30.0, "AccelLateral": 0.5}))
    check("IMU yaw round-trips through the MM5.10 quantisation",
          abs(yaw["YawRate"] - 30.0) < 0.01, str(yaw["YawRate"]))


# --- 2. the tool against its own simulator -----------------------------------

def with_sim(fn):
    bus = can.Bus(interface="virtual", channel="selftest")
    sim = can.Bus(interface="virtual", channel="selftest")
    stop = threading.Event()
    th = threading.Thread(target=R.cmd_sim, args=(sim, Args, stop), daemon=True)
    th.start()
    time.sleep(0.2)
    try:
        return fn(bus)
    finally:
        stop.set()
        th.join(timeout=1.0)
        sim.shutdown()
        bus.shutdown()


def test_commands():
    print("\ncommands, against the simulated node")

    def body(bus):
        rcm = R.Rcm(bus, 0x300, 0)

        st = rcm.state()
        check("all four broadcast frames arrive", sorted(st) ==
              ["faults", "inputs", "outputs", "status"], str(sorted(st)))
        check("status flags decode", st["outputs"]["flags"] & 0x01, "outputs-live")

        rcm.set_channel(7, True)
        time.sleep(0.15)
        check("set one channel", rcm.state()["outputs"]["bits"] == 1 << 6)

        rcm.set_channel(21, True)
        time.sleep(0.15)
        check("set a second, without disturbing the first",
              rcm.state()["outputs"]["bits"] == (1 << 6) | (1 << 20))

        # A mask that covers only channel 7 must leave channel 21 alone.
        rcm.set_mask(1 << 6, 0)
        time.sleep(0.15)
        check("a masked clear leaves other channels alone",
              rcm.state()["outputs"]["bits"] == 1 << 20)

        rcm.ctl(R.OP["alloff"])
        time.sleep(0.15)
        check("all-off", rcm.state()["outputs"]["bits"] == 0)

        rcm.set_mask((1 << 21) - 1, (1 << 21) - 1)
        time.sleep(0.15)
        check("set all 21", rcm.state()["outputs"]["bits"] == 0x1FFFFF)

        rcm.global_ctl(R.OP["alloff"])
        time.sleep(0.15)
        check("global all-off reaches the node", rcm.state()["outputs"]["bits"] == 0)

        # A channel switched to input mode must stop accepting drive commands.
        rcm.ctl(R.OP["chmode"], 4, 2, 0)
        time.sleep(0.1)
        rcm.set_channel(5, True)
        time.sleep(0.15)
        check("an input-mode channel refuses to be driven",
              rcm.state()["outputs"]["bits"] == 0)

        # Sense: with every coil intact, an undriven output channel reads HIGH.
        rcm.ctl(R.OP["chmode"], 4, 1, 0)
        time.sleep(0.1)
        rcm.set_channel(2, True)
        time.sleep(0.15)
        st = rcm.state()
        check("a driven channel drops out of the raw sense bits",
              not (st["inputs"]["raw"] >> 1 & 1))
        check("undriven channels still sense high",
              st["inputs"]["raw"] >> 0 & 1 and st["inputs"]["raw"] >> 20 & 1)

    with_sim(body)


def test_walk():
    print("\nwalk (the bring-up bit-order check)")

    class A2(Args):
        dwell = 0.06

    def body(bus):
        rc = R.cmd_walk(bus, A2)
        check("walk reports every channel as itself", rc == 0)

    with_sim(body)


def test_stale_frames():
    print("\nthe staleness trap")

    def body(bus):
        rcm = R.Rcm(bus, 0x300, 0)
        rcm.set_channel(9, True)
        time.sleep(0.6)                      # let a big backlog build up
        # Reading WITHOUT flushing must see the old state; that is the bug this guards.
        stale = rcm.state(fresh=False)["outputs"]["bits"]
        check("un-flushed read returns a pre-command frame", stale == 0, hex(stale))
        check("flushed read returns the truth",
              rcm.state()["outputs"]["bits"] == 1 << 8)

    with_sim(body)


if __name__ == "__main__":
    test_against_dbc()
    test_commands()
    test_walk()
    test_stale_frames()
    print()
    if fails:
        print("%d FAILED:" % len(fails))
        for f in fails:
            print("  -", f)
        sys.exit(1)
    print("all checks passed")
