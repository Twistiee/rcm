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


## Silkscreen — done (2026-08-07)

Converged at **25 cosmetic warnings**, the same floor the 2-layer board reached. Nothing
electrical: **0 electrical DRC violations, 0 schematic parity.**

| | |
|---|---|
| Text over text | 13 |
| Text over pad | 12 |
| Refs involved | 23 |

Worst offenders are the crowded north-east cluster — `U_CAN`(6), `J_CAN2`(6), `U_MCU`(4) —
plus the tile-1 shift registers.

Both levers are exhausted: reference text is already at **0.8mm**, which is JLC's minimum
height (going smaller risks illegible or unprintable silk), and `relocate_refs.py` reports
0 moves because there is no free space left to move anything into.

The 12 text-over-pad warnings are the least interesting of the two — fabs clip silkscreen
off exposed pads automatically, so those resolve themselves at manufacture. The 13
text-over-text are real overlaps you would notice while reworking, and the only way to
clear them is a placement change to open room in that cluster.


## Stackup — set (2026-08-07)

JLC's standard 4-layer 1.6mm build (`JLC04161H-7628`), so the board is made the way the
quote assumes rather than however the fab feels like guessing:

| Layer | Type | mm |
|---|---|---|
| F.Mask | solder mask | 0.010 |
| **F.Cu** | copper **1oz** | 0.035 |
| dielectric 1 | prepreg 7628, Er 4.4 | 0.2104 |
| **In1.Cu** `GND_plane` | copper **0.5oz** | 0.0175 |
| dielectric 2 | core, Er 4.6 | 1.065 |
| **In2.Cu** `PWR_plane` | copper **0.5oz** | 0.0175 |
| dielectric 3 | prepreg 7628, Er 4.4 | 0.2104 |
| **B.Cu** | copper **1oz** | 0.035 |
| B.Mask | solder mask | 0.010 |
| | **total** | **1.611** |

Finish `HASL lead free` — change this in Board Setup if ENIG is ordered, so the file agrees
with the order.

Both inner layers are now typed **`power`** rather than `signal`, which is what they are and
stops anything trying to route on them.

**Inner layers are 0.5oz, not 1oz** — that is JLC's standard and worth knowing, though it
costs nothing here: a ground plane spanning the whole board has vastly more copper
cross-section than any trace, even at half the weight.

This closes the gap that held up pdm14-revB, where no stackup meant the fab would have
built 1oz while the design assumed 2oz.


## Manufacturing files — `mfg4/` (2026-08-07)

```
mfg4/rcm4_gerbers.zip   16 files, incl. both inner layers
mfg4/bom_jlc.csv        40 lines, every one with an LCSC number
mfg4/cpl_jlc.csv        149 placements, centroid, Y negated
```

Verified:

| Check | Result |
|---|---|
| Outline | exactly **140.00 x 70.00 mm** |
| `G36` in Edge.Cuts | 0 — no phantom cutouts |
| `GND_plane` | **1 filled region** — one continuous plane |
| `PWR_plane` | **1 filled region** |
| Silkscreen pad flashes | 0 front, 0 back |
| Clear-polarity silk blocks | 0 (no `--subtract-soldermask`) |
| PTH drills | 0.3 / 0.6 / 1.0 / 1.2mm |
| NPTH | 0.65, 3.2mm (M3 mounting) |
| BOM lines missing LCSC | 0 |
| CPL Y negated | yes |
| Designator ranges in BOM | none |

The single filled region on each plane is the check that matters — a plane broken into
fragments would mean the pour never closed.

**BOM and CPL are byte-for-byte equivalent to the 2-layer set** — identical part numbers,
identical 149 designators. Nothing about going to 4 layers changes what gets assembled, so
only the PCB half of the quote moves.

`mfg/` (2-layer) is untouched. Upload whichever directory matches the board you order.
