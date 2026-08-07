# `rcm4.kicad_pcb` — 4-layer variant

A parallel variant, built the ordinary way: **GND is routed normally**, and the inner
layers are planes. No `--no-route` tricks — that whole line of experimentation is dropped.

## Stackup

```
F.Cu        signal
GND_plane   (In1.Cu)  solid ground   <- continuous reference under every trace
PWR_plane   (In2.Cu)  +3V3
B.Cu        signal + GND pour
```

## Comparison

| | 2-layer (`rcm.kicad_pcb`) | 4-layer (`rcm4.kicad_pcb`) |
|---|---|---|
| Nets routed | 426 / 426 | 426 / 426 |
| Unconnected | 0 | **0** |
| Schematic parity | 0 | **0** |
| Electrical DRC | 0 | **0** |
| Silkscreen (cosmetic) | 25 | 26 |
| Segments | 2078 | 2148 |
| Vias | 166 | 164 |

Both are clean. The 4-layer board is not *smaller* in copper — it is better in the way that
matters: every signal has a continuous ground plane directly beneath it instead of whatever
pour fragment happens to be nearby. For CAN, USB and an 8MHz crystal on a board that
switches inductive relay loads, that is the real argument, not trace count.

It also routed first time with no fighting, which the 2-layer board did not.

## Before ordering this one

- **Set a stackup** in Board Setup. Neither board carries a `(stackup)` block, and on a
  4-layer board the fab needs to know the dielectric heights — this is exactly the issue
  that held up pdm14-revB.
- **Re-quote.** 4-layer changes the PCB price; the assembly side is unchanged.
- Regenerate gerbers — `mfg/` is from the **2-layer** board and has no inner layers in it.
- The build pipeline does not yet generate this variant; the layer count and plane zones
  were applied to a fresh `netlist_to_board.py` output by hand. If the schematic changes,
  that has to be redone (worth folding into `gen_plan.py` if 4-layer becomes the choice).
