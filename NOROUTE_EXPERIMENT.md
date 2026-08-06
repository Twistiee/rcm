# `rcm_noroute.kicad_pcb` — GND left to the pour

**This is a side-by-side experiment, not the board to order.** Open it next to
`rcm.kicad_pcb`, which is unchanged and remains the good one.

Built with `route_board.py --no-route GND`, which strips GND from the DSN so freerouting
never routes it, then 250 stitching vias on a 4mm grid and an automated rescue pass.

## The comparison

| | `rcm.kicad_pcb` (good) | `rcm_noroute.kicad_pcb` |
|---|---|---|
| Nets routed | 426 / 426 | 293 / 293 |
| Track segments | 2244 | **1726 (−23%)** |
| Route time | 87s | **51s** |
| Stitching vias | — | 250 |
| **Unconnected** | **0** | **36** |

The signal routing really is cleaner and there is visibly more room — that part of your
hunch was right, and it shows up plainly on screen.

## What the 36 are

- **22 are pour-island-to-island.** Cosmetic. Signal traces slice the top pour into
  patches; most matter little because the bottom pour carries the current.
- **11 are genuinely isolated pads** — these are the real problem:

```
U_MCU.12  U_MCU.18  U_MCU.31  U_MCU.47  U_MCU.63     <- five MCU grounds
U_IMU.6   U_IMU.7                                     <- IMU grounds
U_CAN.2   U_LATCH.3   U_SI1.15   U_SI2.15
```

The automated rescue could not place a via beside any of them — signal traces box them in
completely, so there is no escape route to the bottom pour. They need a trace threaded out
by hand, or a small placement change to open a gap.

## Worth knowing before you judge it

Five isolated MCU grounds is not a cosmetic problem. An MCU with grounds connected only
through some of its pins will often *work*, then behave badly under load or emit noise —
the sort of fault that is miserable to diagnose after the boards are built.

If you want to pursue this, the honest path is fixing the **placement** so the pour can
reach those pins, rather than hand-threading eleven traces into the tightest parts of the
board. The MCU and the two shift registers are the recurring offenders.

## Files

- `rcm_noroute.kicad_pcb` — this experiment
- `noroute.drc.json` — full DRC, every unconnected item listed
- `rcm.kicad_pcb` — untouched, 0 unconnected, matches the gerbers in `mfg/`
