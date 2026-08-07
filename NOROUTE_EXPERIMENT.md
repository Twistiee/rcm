# GND to the pour, except the MCU — APPLIED to `rcm.kicad_pcb`

Currently on disk. **Still not orderable** — 3 GND pads remain isolated. Restore with:

```
cp rcm.kicad_pcb.bak_gnd_routed rcm.kicad_pcb
```

`mfg/` was generated from the good board and does **not** match what is on disk.

## Where keeping the MCU grounds routed got us

`route_board.py --no-route GND --no-route-keep U_MCU` keeps the MCU's 5 ground pins in the
netlist so freerouting wires them properly, and hands the other 131 GND pins to the pour.

| | good board | GND fully to pour | **GND except MCU** |
|---|---|---|---|
| Track segments | 2078 | 1605 | **1574 (−24%)** |
| Vias | 166 | 165 | 208 |
| Route time | 87s | 48s | **40s** |
| Electrical DRC | 0 | 0 | **0** |
| Unconnected | 0 | 25 | **24** |
| **Isolated pads** | **0** | **8** (5 MCU grounds) | **3** (no MCU grounds) |

That was the right instinct. The MCU's ground pins are boxed in by its own escape routing,
so they were never going to reach the pour — but they are trivial for the router to wire
once they are back in the netlist. Five isolated MCU grounds became zero, and it cost
nothing in copper: 1574 segments against 1605.

## What is left

Three pads still cannot reach the pour:

```
U_LATCH.3    U_SI1.15    U_SI2.15
```

`U_SI1`/`U_SI2` are shift registers whose ground pin sits behind their own fanout — the
same failure mode as the MCU, one level down. The obvious next step is
`--no-route-keep U_MCU --no-route-keep U_SI --no-route-keep U_LATCH`, which should close
them the same way, at the cost of a few more traces.

Vias are up (208 vs 166) because island stitching is doing more work now that the pour is
carrying nearly all of GND. Worth an eye on drill count if this ever ships.

## Health warning on the tooling

The stitch/rescue pass validates against the real KiCad DRC and backs out anything that
makes things worse, which is why electrical violations are 0. But getting here took several
rounds, and one intermediate cleanup removed GND tracks by matching their *length*, which
also deleted innocent tracks of the same length. If you rebuild this, rebuild it from the
good board rather than iterating on top.
