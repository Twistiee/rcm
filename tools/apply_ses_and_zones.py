"""Apply a Freerouting SES to a board, add full-board copper zones, fill, save.

Run with KiCad's bundled python.

Usage:
  python apply_ses_and_zones.py <board.kicad_pcb> [--ses board.ses]
      [--zone NET:LAYER ...] [--margin 0.5]

Example:
  python apply_ses_and_zones.py keypad.kicad_pcb --ses output/routing/board.ses \
      --zone GND:F.Cu --zone GND:B.Cu
"""
import argparse

import pcbnew


def add_zones(board, zone_specs, margin_mm=0.5):
    """Add full-board zones. zone_specs: ["NET:LAYER", ...]. Returns count."""
    bbox = board.GetBoardEdgesBoundingBox()
    m = pcbnew.FromMM(margin_mm)
    x0, y0 = bbox.GetLeft() + m, bbox.GetTop() + m
    x1, y1 = bbox.GetRight() - m, bbox.GetBottom() - m
    for spec in zone_specs:
        netname, layername = spec.split(":", 1)
        net = board.FindNet(netname)
        if net is None:
            raise SystemExit(f"net not found: {netname}")
        layer = board.GetLayerID(layername)
        zone = pcbnew.ZONE(board)
        zone.SetLayer(layer)
        zone.SetNetCode(net.GetNetCode())
        zone.SetPadConnection(pcbnew.ZONE_CONNECTION_THERMAL)
        zone.SetLocalClearance(pcbnew.FromMM(0.3))
        zone.SetMinThickness(pcbnew.FromMM(0.25))
        zone.SetThermalReliefGap(pcbnew.FromMM(0.4))
        zone.SetThermalReliefSpokeWidth(pcbnew.FromMM(0.4))
        ol = zone.Outline()
        ol.NewOutline()
        for x, y in ((x0, y0), (x1, y0), (x1, y1), (x0, y1)):
            ol.Append(x, y)
        board.Add(zone)
    return len(zone_specs)


def fill_zones(board):
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("board")
    ap.add_argument("--ses")
    ap.add_argument("--zone", action="append", default=[],
                    help="NET:LAYER full-board zone, e.g. GND:B.Cu")
    ap.add_argument("--margin", type=float, default=0.5,
                    help="zone pullback from board bbox edge, mm")
    args = ap.parse_args()

    board = pcbnew.LoadBoard(args.board)
    if args.ses:
        if not pcbnew.ImportSpecctraSES(board, args.ses):
            raise SystemExit("SES import failed")
        print(f"SES applied: {len(board.GetTracks())} track segments/vias now on board")

    if args.zone:
        n = add_zones(board, args.zone, args.margin)
        print(f"zones added: {n}")

    fill_zones(board)
    print(f"zones filled: {len(board.Zones())}")
    pcbnew.SaveBoard(args.board, board)
    print("saved", args.board)


if __name__ == "__main__":
    main()
