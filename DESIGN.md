# RCM — Relay Control Module

> **Status 2026-08-04: repo just created. Architecture is NOT fixed.** This document
> records the cost analysis that motivated the pivot, what carries over, and the decisions
> still open. Nothing here has been drawn yet.

## The pivot

`pdm14-revB` switches 14 loads with Infineon PROFET+2 smart high-side switches on the
board. This board instead **drives relay coils** and lets an external relay/fuse box carry
the load current — turning an ordinary automotive relay/fuse box into a CAN-controlled
unit.

## Where the $500 goes (measured, 2026-08-04)

The revB quote is ~$500 USD for 5 boards. Component prices below are live LCSC/JLC unit
prices pulled 2026-08-04; the fab/assembly figure is the residual.

| | per board | ×5 | share |
|---|---:|---:|---:|
| 15× PROFET (2×BTS7002 @2.88, 4×BTS7008 @0.92, 9×BTS7040 @0.87) | $17.25 | $86 | ~17% |
| STM32F446ZET6 | $5.40 | $27 | ~5% |
| BMI270 IMU | $1.55 | $8 | ~2% |
| microSD socket (DM3AT-SF-PEJM5) | $1.23 | $6 | ~1% |
| SN65HVD230 CAN | $0.48 | $2 | <1% |
| 14× KF301-5.0-2P screw terminals | $0.56 | $3 | <1% |
| everything else (~200 passives, USB-C, EEPROM, LDO, TVS, crystals, terminals, headers) | ~$6 | ~$30 | ~6% |
| **components subtotal** | **~$32** | **~$160** | **~32%** |
| **PCB fab + assembly + setup (residual)** | **~$68** | **~$340** | **~68%** |

### What this means for cost reduction

**The MCU is not the lever.** At $5.40 it is ~5% of the quote. Swapping to an ESP32 saves
maybe $3–4/board — about $20 across the run — while discarding a vehicle-proven firmware
stack and its PC config app. If the MCU changes, the reason should be a *smaller STM32*
(fewer pins, cheaper package, less board area, cheaper to place) rather than a different
architecture.

**The board is the lever.** ~68% of the cost is fab + assembly, driven by:

| Driver | revB | why relays change it |
|---|---|---|
| **Area** | 216 × 148mm = **320 cm²** | no 7mm/2oz output trunks, no EP thermal-via arrays, no M5 stud columns, no 14 large screw terminals |
| **Copper weight** | 2oz outer (required for 21A trunks) | a relay coil is ~150–200mA — 1oz is ample |
| **Layers** | 4 | plausibly 2 once the high-current pours are gone |
| **Surface finish** | ENIG (required for the bolted M5 stud joints) | no studs ⇒ HASL is fine |
| **Placements** | **315 per board** (1575 for the run) | a PROFET channel carries RS/RP/DZ/CS/RG/RIP/CO/CV; a relay channel is a low-side FET + gate resistor + flyback, or one octal driver per 8 channels |

Attacking area, layer count, finish and placement count together is where the real
reduction is. The PROFET removal ($17.25/board) is a genuine saving but is second-order
next to the board itself.

## What is being given up, and what has to be replaced

Relays are not drop-in equivalents to smart high-side switches. Going in with this
explicit:

| PROFET feature | Relay equivalent |
|---|---|
| Per-channel **current sense** (`IS` pin, `kILIS`) | **none inherently** — needs added shunt+amp or hall sensors, or drop the feature. This is the biggest open decision |
| Open-load detection | none inherently (revB used the RPD/ROL/T1 tap) |
| **PWM / soft-start** | not possible on a mechanical relay |
| Overcurrent / overtemp / short-circuit shutdown | delegated to the **fuse box's fuses** |
| Reverse-polarity, ~35V clamp per channel | delegated to input protection |
| Silent, unlimited switching cycles | relays have finite contact life and audible click |

The fuse box handling overcurrent is arguably the point: fuses are cheaper, field
replaceable, and understood by anyone working on the car.

## Firmware scope (user, 2026-08-04) — deliberately much smaller than SynapsePDM

The functional requirement is **"turn things on and off via CAN, plus put IMU data on
CAN"**. That is it. Route is undecided between **butchering joesbox's firmware down to
that** and **writing something simple in the Arduino IDE**.

**This is the decision that reframes the MCU choice.** The earlier argument for staying on
an STM32F4 was firmware reuse — if the firmware is a few hundred lines either way, reuse
stops being the constraint and the MCU should be chosen on cost, size and CAN support.

What is still worth preserving regardless of chip or language: **CAN message compatibility**
with the existing DBC (`../SynapsePDM/SynapsePDM/CAN DB/`) and the Cortex PC app, so the
`../keypad` node and any existing tooling keep working. Message layout is cheaper to keep
than to re-invent, even in fresh firmware.

### MCU options at this scope (live prices 2026-08-04)

| Part | Price | Package | Notes |
|---|---:|---|---|
| **STM32F103C8T6** | **$1.04** | LQFP-48 7×7 | bxCAN on-chip, Arduino via STM32duino, 214k stock. 64KB flash is ample for this scope. Cheapest and smallest |
| STM32F446RET6 | $4.93 | LQFP-64 10×10 | only worth it if joesbox's firmware is kept substantially intact |
| STM32F446ZET6 | $5.40 | LQFP-144 20×20 | revB's part. No reason to carry 144 pins here |
| ESP32-WROOM-32E-N4 | $3.34 | module 25.5×18mm | **more expensive than the F103, and physically much larger.** One placement, native Arduino, TWAI CAN, and WiFi/BT if wanted |

**The ESP32 is not the cost win it looks like** — it is 3× the F103's price and takes far
more board area, which is the thing actually driving cost here. Pick it for WiFi/BT as a
feature, not to save money. Both still need an external CAN transceiver.

## Relay drive: the saving is larger than the PROFET line suggests

An octal Darlington/MOSFET sink array replaces both the switch **and** the flyback diodes:

| | revB | RCM |
|---|---|---|
| switching | 15× PROFET, **$17.25** | 2× **ULN2803A** (16 ch), `C845537`, $0.14 ea = **$0.29** |
| flyback | n/a | **integral** — the ULN2803's `COM` pin clamps all 8 channels, so 14 discrete diodes disappear |
| per-channel support parts | RS/RP/DZ/CS/RG/RIP/CO/CV | none |

Two caveats to check when this is drawn:

- **Darlington drop.** ULN2803 `Vce(sat)` is ~1.1–1.6V at 200mA, so the coil sees ~10.5V of
  a 12V supply, and 8 channels on at once dissipates ~2W in a SOP-18 — fine for typical
  duty, not for all-on-continuous. **`TPL7407LAPWR`** (`C2149827`, $0.39, TSSOP-16) is the
  modern MOSFET-based equivalent with ~0.1V drop and near-zero dissipation, and is the
  better part *if stock recovers* — it was at only 32 units on 2026-08-04.
- **3.3V drive.** ULN2803 input resistors are sized for 5V logic; verify drive margin at
  3.3V for the chosen coil current. The TPL7407L is explicitly 3.3V-friendly.

## Open decisions

Nothing below is settled — these change the architecture, not just values.

1. **Channel count**, and whether channels are uniform (relays are, unlike revB's 3 tiers).
2. **Current sensing** — keep per-channel, sense only a few channels, or drop entirely and
   rely on fuses? Biggest remaining cost/area fork.
3. **MCU** — see table above. Leaning F103C8T6 now that firmware reuse is not a constraint.
4. **Relay box interface** — connector and pinout, and whether the module feeds coil power
   or only sinks the coil return (low-side sink is assumed above).
5. **Which revB features survive** — microSD logging, USB-C, EEPROM config store, SIM7600
   COMMS header, ignition latch, analogue/digital inputs. At this firmware scope most are
   candidates to drop.

## Parked

- `5.0SMDJ20A` (`C2990361`), the revB load-dump TVS on `D15`/`D16`, went to **0 stock** at
  JLC on 2026-08-04. Not urgent — the revB order is on hold pending this board — but it
  needs a substitute before that BOM is ever uploaded.

## Carried over

See `CLAUDE.md`: BMI270 IMU, the Waveshare DC5-36-TO-DC3V3-5 buck module and its pin
arrangement (socketed), CAN as the control bus, and the SynapsePDM firmware lineage.
