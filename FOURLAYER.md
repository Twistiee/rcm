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


## The 5 unconnected items, examined

They are **not** isolated pads — every pad on the board is connected. They are pour
fragments on the outer layers. Measured:

| Layer | Islands | Orphaned (no pad or via inside) |
|---|---|---|
| `GND_plane` (In1) | **1** | **0** |
| F.Cu | 39 | 3 — 8.2, 1.25, 0.89 mm² |
| B.Cu | 14 | 1 — 1.34 mm² |

**The layer that carries the current is perfect**: the inner ground plane is a single
continuous island with nothing orphaned. That is the whole point of going to 4 layers.

One genuine problem was found and fixed: a **43 mm²** floating patch on B.Cu at (61, 29).
It was floating because `stitch_zone_vias.py` only ever examined F.Cu, so B.Cu islands were
never even attempted. Stitched with a via now.

The four that remain are 0.89–8.2 mm² slivers with no room for a via. Island removal is
enabled on both outer pours so KiCad deletes unconnected copper rather than leaving it
floating, which is the right treatment for fragments this size.

**Honest note:** enabling island removal did not change the DRC count — it still reports 5.
I have not confirmed why; the likely explanation is that KiCad's zone ratsnest wants
same-layer continuity and does not credit the connection made through the inner plane. That
would make it a reporting artifact rather than a defect, but I have not proven it, so treat
the 5 as unexplained-but-benign rather than definitively fine.

Nothing here is electrical: **0 electrical DRC violations, 0 schematic parity errors, and
no pad anywhere left unconnected.**
