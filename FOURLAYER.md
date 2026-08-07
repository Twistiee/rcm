# `rcm4.kicad_pcb` — 4-layer, GND to the pours

Parallel variant. `rcm.kicad_pcb` (2-layer) is untouched and still matches `mfg/`.

## Stackup and pours

```
F.Cu        signal + GND pour
GND_plane   (In1.Cu)  solid ground   <- continuous reference under every trace
PWR_plane   (In2.Cu)  +3V3
B.Cu        signal + GND pour
```

GND is left to the copper — 117 of its 136 pins are never routed as traces. The IC ground
pins **are** kept in the netlist (`U_MCU`, `U_SI`, `U_IMU`, `U_LATCH`, `U_CAN`, `U_DRV`),
because those get boxed in by their own escape routing and the pour can never reach them.
That was the lesson from the 2-layer attempts: leave the easy 90% to copper, route the
handful that copper cannot get to.

## Comparison

| | 2-layer | 4-layer, GND routed | **4-layer, GND to pours** |
|---|---|---|---|
| Segments | 2078 | 2148 | **1766 (−15%)** |
| Vias | 166 | 164 | 161 |
| Route time | 87s | 88s | **86s** |
| Electrical DRC | 0 | 0 | **0** |
| Schematic parity | 0 | 0 | **0** |
| Silkscreen (cosmetic) | 25 | 26 | **25** |
| **Isolated pads** | **0** | **0** | **0** |
| Unconnected items | 0 | 0 | 5 |

The 5 remaining unconnected items are all **pour-island-to-island** — no pad is isolated.
Small patches of top-side pour fenced off by signal traces, with the solid inner plane
carrying the current regardless. Cosmetic; KiCad will keep listing them.

This is the best of the three: fewest traces, a continuous ground reference under every
signal, and every pad connected.

## Before ordering this one

- **Set a stackup** in Board Setup — dielectric heights for a 4-layer build. Neither board
  carries a `(stackup)` block, which is exactly what held up pdm14-revB.
- **Re-quote**; 4-layer changes the PCB price. Assembly is unchanged.
- **Regenerate gerbers** — `mfg/` is the 2-layer set and has no inner layers.
- Layer count and plane zones are applied by hand to a fresh `netlist_to_board.py` output,
  not generated. A schematic change means redoing that; worth folding into the pipeline if
  4-layer becomes the choice.
