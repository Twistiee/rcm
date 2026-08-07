# GND-left-to-the-pour version — APPLIED to `rcm.kicad_pcb`

**This is what is currently on disk.** It is NOT orderable — see "What is still wrong".
The good board is one command away:

```
git checkout rcm.kicad_pcb        # or: cp rcm.kicad_pcb.bak_gnd_routed rcm.kicad_pcb
```

`mfg/` (gerbers, BOM, CPL) was generated from the GOOD board and does **not** match what is
on disk now. Do not upload anything until you restore.

## Comparison

| | good board | this one |
|---|---|---|
| Nets routed | 426 / 426 | 293 / 293 |
| Track segments | 2078 | **1605 (−23%)** |
| Vias | 166 | 165 |
| Route time | 87s | **48s** |
| Electrical DRC violations | 0 | **0** |
| Silkscreen (cosmetic) | 25 | 25 |
| **Unconnected** | **0** | **25** |

Signal routing is visibly freer, and the via count did not blow out — the stitching is
targeted now, so it costs about the same number of holes as before.

## What is still wrong

**8 GND pads cannot reach the pour.** Signal traces box them in on every side, so there is
no legal escape to the bottom pour:

```
U_MCU.12   U_MCU.18   U_MCU.31   U_MCU.47   U_MCU.63     <- five MCU grounds
U_LATCH.3   U_SI2.15   C_SO2.2
```

The rest of the 25 unconnected items are pour-island-to-island, which matters much less.

**Five isolated MCU grounds is the thing to weigh.** An MCU grounded through only some of
its pins usually appears to work and then misbehaves under load or radiates — the sort of
fault that is miserable to find once boards exist.

## What was fixed to get here

The first attempt at this was worse than I reported, because I checked connectivity but not
the full DRC. It had a genuine **short** (a GND rescue trace driven straight through
`SR_SCK`), two more track crossings, and a via 0.077mm from the battery input pad. Causes:

1. The rescue pass checked its trace against **pads but never against tracks**.
2. Hand-rolled clearance maths missed **hole-to-hole** clearance against THT pads.
3. Stitching was a blanket 4mm grid over the whole board — **250 vias**, most joining the
   main pour to itself, achieving nothing while perforating the pour and adding 250 holes
   to the drill file.

`stitch_zone_vias.py` now finds the filled polygons, leaves the largest alone, and stitches
only genuine islands; and every placement is checked by running the **real KiCad DRC**
rather than trusting my own arithmetic.

## The honest recommendation

The technique works and the copper reduction is real. But closing those last 8 pads wants a
**placement** change — opening room around the MCU and the shift registers so the pour can
reach — not more automated patching. Until then, the good board is the one to order.
