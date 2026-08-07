"""One-shot autoroute: DSN export -> Freerouting -> SES import -> zones -> DRC.

Run with KiCad's bundled python. Encodes the hard-won Freerouting v2 lessons:
  - SES is only written after the optimizer finishes; --router.improvement_threshold
    high makes it exit right after routing instead of optimizing ~forever.
  - Never leave fine-pitch IC nets in a wide net class (freerouting reports them
    as unroutable) — that's a project-file concern, checked here only by result.

Usage:
  python route_board.py <board.kicad_pcb> [--zone GND:F.Cu --zone GND:B.Cu]
      [--freerouting <exe>] [--timeout 1800] [--passes 100] [--threshold 0.99]
      [--kicad-cli <path>]

Exit 0 only if freerouting completed, the SES applied, and DRC reports
0 violations / 0 unconnected.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time

import pcbnew

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from apply_ses_and_zones import add_zones, fill_zones  # noqa: E402

DEFAULT_FREEROUTING = os.path.expandvars(r"%LOCALAPPDATA%\Freerouting\freerouting.exe")
DEFAULT_KICAD_CLI = r"C:\Program Files\KiCad\10.0\bin\kicad-cli.exe"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("board")
    ap.add_argument("--zone", action="append", default=[])
    ap.add_argument("--no-route", action="append", default=[],
                    help="net to STRIP from the DSN so freerouting never routes it. "
                         "For a net that is a full copper pour, every trace the router "
                         "lays is wasted congestion -- it steals paths from signals to "
                         "reach a pad the zone fill was going to connect anyway. Pass "
                         "--no-route GND together with --zone GND:F.Cu --zone GND:B.Cu.")
    ap.add_argument("--no-route-keep", action="append", default=[],
                    help="reference prefix whose pins STAY in the --no-route net, e.g. "
                         "U_MCU. Everything else on that net is left to the pour. Use this "
                         "for parts the pour cannot reach -- a QFP's ground pins get boxed "
                         "in by its own escape routing, and an MCU grounded through only "
                         "some of its pins works on the bench then misbehaves under load.")
    ap.add_argument("--power-layer", action="append", default=[],
                    help="mark this copper layer (type power) in the DSN so "
                         "freerouting routes no wires on it (vias still pass); "
                         "full-layer keepouts on it are dropped from the DSN "
                         "(plain DSN keepouts would block vias board-wide)")
    ap.add_argument("--freerouting", default=DEFAULT_FREEROUTING)
    ap.add_argument("--kicad-cli", default=DEFAULT_KICAD_CLI)
    ap.add_argument("--timeout", type=int, default=1800)
    ap.add_argument("--passes", type=int, default=100)
    ap.add_argument("--threshold", default="0.99")
    ap.add_argument("--fr-arg", action="append", default=[],
                    help="extra argument passed through to freerouting verbatim")
    args = ap.parse_args()

    board_path = os.path.abspath(args.board)
    routing_dir = os.path.join(os.path.dirname(board_path), "output", "routing")
    os.makedirs(routing_dir, exist_ok=True)
    dsn = os.path.join(routing_dir, "board.dsn")
    ses = os.path.join(routing_dir, "board.ses")
    log = os.path.join(routing_dir, "freerouting.log")

    print("== DSN export ==")
    board = pcbnew.LoadBoard(board_path)
    # strip existing routing/zones so reruns are idempotent (freerouting would
    # otherwise grind its optimizer over the already-routed copper)
    removed = 0
    for t in list(board.GetTracks()):
        board.RemoveNative(t)  # Remove() trips a swig ownership quirk on zones
        removed += 1
    for z in list(board.Zones()):
        if z.GetIsRuleArea():
            continue  # keepouts are design intent, not routing output
        board.RemoveNative(z)
        removed += 1
    if removed:
        print(f"  cleared {removed} existing tracks/vias/zones")
        pcbnew.SaveBoard(board_path, board)
    if not pcbnew.ExportSpecctraDSN(board, dsn):
        raise SystemExit("DSN export failed")
    if args.power_layer:
        txt = open(dsn, encoding="utf-8").read()
        for lay in args.power_layer:
            esc = re.escape(lay)
            txt, n = re.subn(r"(\(layer %s\s*\(type )signal" % esc,
                             r"\1power", txt)
            if n != 1:
                raise SystemExit(f"--power-layer {lay}: layer not found in DSN")
            txt, n = re.subn(r'\(keepout "" \(polygon %s [^()]*\)\)\s*' % esc,
                             "", txt, flags=re.S)
            print(f"  power layer {lay} (dropped {n} DSN keepout(s) on it)")
        open(dsn, "w", encoding="utf-8").write(txt)

    # Strip pour nets from the DSN. Freerouting has no concept of "this net is a plane,
    # leave it alone" -- given GND pins it will dutifully route them, and on a dense board
    # those traces crowd out real signals to reach pads the pour connects for free.
    # Removing the net from the netlist is the whole trick; the zone fill does the work.
    if args.no_route:
        txt = open(dsn, encoding="utf-8").read()
        for net in args.no_route:
            # DSN writes names UNQUOTED: "(net GND". The lookahead keeps LATCH_GND,
            # CAN_TERM etc. from matching -- a substring match here would silently
            # delete the wrong net and the board would route fine but wired wrong.
            esc = re.escape(net)
            m = re.search(r"\(net\s+\"?%s\"?(?=[\s(])" % esc, txt)
            if not m:
                raise SystemExit("--no-route %s: net not found in DSN" % net)
            depth = 0
            for j in range(m.start(), len(txt)):
                if txt[j] == "(":
                    depth += 1
                elif txt[j] == ")":
                    depth -= 1
                    if depth == 0:
                        break
            block = txt[m.start():j + 1]
            allpins = re.findall(r"[A-Za-z_][\w.]*-[\w]+", block)
            keep = [p for p in allpins
                    if any(p.split("-")[0].startswith(k)
                           for k in args.no_route_keep)]
            if keep:
                # Keep a subset routed: rewrite the net with only those pins. Freerouting
                # wires them together; the pour picks the cluster up from there.
                txt = (txt[:m.start()]
                       + '(net %s\n      (pins %s)\n    )' % (net, " ".join(keep))
                       + txt[j + 1:])
                print("  no-route %s: %d pins to the pour, %d kept routed (%s)"
                      % (net, len(allpins) - len(keep), len(keep),
                         ", ".join(args.no_route_keep)))
            else:
                txt = txt[:m.start()] + txt[j + 1:]
                # the class list names every net; a dangling name upsets the parse
                txt = re.sub(r"(?<=[\s])%s(?=[\s])" % esc, " ", txt, count=1)
                print("  no-route %s (%d pins left to the pour)" % (net, len(allpins)))
        open(dsn, "w", encoding="utf-8").write(txt)

    print("== Freerouting ==")
    if os.path.exists(ses):
        os.remove(ses)
    cmd = [args.freerouting, "-de", dsn, "-do", ses, "-da",
           f"--router.optimizer.improvement_threshold={args.threshold}",
           f"--router.max_passes={args.passes}"] + args.fr_arg
    with open(log, "w", encoding="utf-8") as lf:
        proc = subprocess.Popen(cmd, stdout=lf, stderr=subprocess.STDOUT)
        t0 = time.time()
        while proc.poll() is None:
            if time.time() - t0 > args.timeout:
                proc.kill()
                raise SystemExit(f"freerouting timed out after {args.timeout}s (log: {log})")
            time.sleep(5)
    if not os.path.exists(ses):
        raise SystemExit(f"freerouting exited without writing SES (log: {log})")
    for line in open(log, encoding="utf-8", errors="replace"):
        if "session completed" in line:
            print("  " + line.strip().split("INFO")[-1].strip())

    print("== SES import + zones ==")
    board = pcbnew.LoadBoard(board_path)
    if not pcbnew.ImportSpecctraSES(board, ses):
        raise SystemExit("SES import failed")
    print(f"  {len(board.GetTracks())} track segments/vias")
    if args.zone:
        add_zones(board, args.zone)
        fill_zones(board)
        print(f"  zones: {len(board.Zones())}")
    pcbnew.SaveBoard(board_path, board)

    def run_drc():
        drc_json = os.path.join(routing_dir, "drc.json")
        r = subprocess.run([args.kicad_cli, "pcb", "drc", "--format", "json",
                            "--refill-zones", "--save-board", "-o", drc_json, board_path],
                           capture_output=True)
        if r.returncode != 0:
            # kicad-cli can crash during refill on large boards; zones were
            # already filled in-process, so retry DRC without refill
            subprocess.run([args.kicad_cli, "pcb", "drc", "--format", "json",
                            "-o", drc_json, board_path], check=True, capture_output=True)
        return json.load(open(drc_json, encoding="utf-8"))

    print("== DRC (refill zones, save) ==")
    d = run_drc()

    # Auto-fix: starved thermal reliefs -> solid zone connection on just that
    # pad. Refilling can starve OTHER pads, so iterate until stable.
    import re as _re
    for _round in range(5):
        starved = [v for v in d.get("violations", []) if v["type"] == "starved_thermal"]
        if not starved or not all(v["type"] == "starved_thermal"
                                  for v in d.get("violations", [])):
            break
        board = pcbnew.LoadBoard(board_path)
        fixed = 0
        for v in starved:
            for it in v.get("items", []):
                m = _re.match(r".*pad (\S+) \[[^\]]*\] of (\S+)", it["description"])
                if not m:
                    continue
                padnum, ref = m.group(1), m.group(2)
                fp = board.FindFootprintByReference(ref)
                if fp:
                    for pad in fp.Pads():
                        if pad.GetNumber() == padnum:
                            pad.SetLocalZoneConnection(pcbnew.ZONE_CONNECTION_FULL)
                            fixed += 1
        if not fixed:
            break
        print(f"  auto-fix round {_round + 1}: {fixed} starved pad(s) -> solid; re-running")
        pcbnew.ZONE_FILLER(board).Fill(board.Zones())
        pcbnew.SaveBoard(board_path, board)
        d = run_drc()

    nviol = len(d.get("violations", []))
    nunc = len(d.get("unconnected_items", []))
    print(f"  violations: {nviol}, unconnected: {nunc}")
    for v in d.get("violations", [])[:10]:
        print("   -", v["type"], ":", v["description"][:70])
    for u in d.get("unconnected_items", [])[:10]:
        its = u.get("items", [])
        print("   - unconnected:", " <-> ".join(i["description"][:45] for i in its[:2]))
    sys.exit(0 if (nviol == 0 and nunc == 0) else 1)


if __name__ == "__main__":
    main()
