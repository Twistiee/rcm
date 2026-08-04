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

### MCU decision at qty 1: pick G431 for commonality (2026-08-04)

Given the quantity reframe below, the spread between these parts is **~$2 on the whole
project**. That makes cost a non-criterion and leaves ecosystem consistency: the
`../keypad` node is already **STM32G431CBT6** (LQFP-48, `C529355`, $2.85, 67k stock) — same
family, same FDCAN peripheral, one toolchain, and CAN/config code shared between the two
nodes on the same bus. **Use the G431 unless something else forces a change.** The F103's
$1.04 is no longer worth a second ecosystem, and the ESP32 is a WiFi/BT decision only.

## Quantity: these are ONE-OFFS (user, 2026-08-04)

**Not production, not for sale — ideally one of each board.** This changes the objective
function, and it retires several earlier concerns.

Per-unit BOM cost is now nearly irrelevant. At qty 1 the cost is dominated by:

1. **Board area** — fab is priced by area, so shrinking the outline is still the top lever.
2. **Layer count, copper weight, surface finish** — 2-layer / 1oz / HASL vs revB's
   4-layer / 2oz / ENIG.
3. **Number of unique *extended* parts** — JLC charges a per-unique-extended-part handling
   fee, which is a fixed cost paid once no matter how few boards you build. **revB has 33
   extended lines out of 59.** Consolidating values and preferring Basic/Preferred parts is
   a real lever at qty 1 that did not matter at volume. (Check JLC's current fee schedule —
   the terms change — but the structure holds.)
4. **Placement count and fixed setup/stencil fees.**

⚠ **You cannot actually order one.** JLC's PCB minimum is typically 5 pcs (sometimes 2) and
PCBA minimum around 2. The practical shape is: fab ~5, **assemble 1–2, keep the rest bare
as spares** — you only pay components and placement for what is assembled.

**Worth evaluating for this board specifically: skip PCBA and hand-build.** The RCM should
land near ~100–150 placements (against revB's 315), which is a long evening by hand and
removes the assembly and per-part fees entirely. A middle option is JLC placing the
fine-pitch SMD and hand-fitting the THT connectors, which JLC surcharges anyway.

### Concerns this retires

All the stock-depth caps flagged earlier were volume constraints and are now moot:
BTS7008 (10 boards), BTS7002 (36), and **TPL7407L's thin 32–48 unit stock** — needing 3
chips for one board makes it a non-issue.

## Relay drive: the saving is larger than the PROFET line suggests

An octal Darlington/MOSFET sink array replaces both the switch **and** the flyback diodes:

| | revB | RCM |
|---|---|---|
| switching | 15× PROFET, **$17.25** | 2× **ULN2803A** (16 ch), `C845537`, $0.14 ea = **$0.29** |
| flyback | n/a | **integral** — the ULN2803's `COM` pin clamps all 8 channels, so 14 discrete diodes disappear |
| per-channel support parts | RS/RP/DZ/CS/RG/RIP/CO/CV | none |

### DECIDED: TPL7407L, not ULN2803 — the Darlington cannot hold all channels on

The user flagged this and was right. Relay coils are ~150–200mA each, unlike the keypad's
LEDs, and that is where the Darlington falls over:

| | ULN2803A (**8**ch, SOP-18) | **TPL7407L** (**7**ch, TSSOP-16) |
|---|---|---|
| drop @ 200mA | 1.1–1.6V `Vce(sat)` | ~0.1–0.25V (MOSFET) |
| per channel | ~0.26W | ~0.05W |
| **all channels on** | **~2.1W ⇒ ~200°C rise** — not survivable | ~0.35W — fine |
| 3.3V logic drive | input resistors sized for 5V, needs checking | explicitly 3.3V-friendly |

`TPL7407LAPWR` = `C2149827`, $0.39, TSSOP-16. Note it is **7-channel** (a ULN2003
replacement, not ULN2803), so **3 chips give 21 channels** for $1.17. Stock is thin
(32–48 units per variant) but irrelevant at qty 1.

**The keypad keeps its ULN2803A** — see `../keypad/CLAUDE.md`. It drives 12V LED rings
totalling ~0.2–0.4A, nowhere near the dissipation limit, and the part is socketed DIP-18
against the user's own chip stock; TPL7407L has no DIP package. The MOSFET part is better
for *coils*, not for that load.

## Channel count: target 18–20 (user, 2026-08-04)

"Aim for 18–20 outputs; pull back if something doesn't work." With `TPL7407L` being a
**7-channel** part, 3 chips = **21 channels**, which covers the target with one spare.

**I/O expansion makes the count cheap to change.** 20 outputs + 20 fuse-sense inputs = 40
I/O, which no LQFP-48 provides alongside CAN and a crystal. Use shift registers —
`74HC595` for outputs, `74HC165` for sense inputs (`C22384789`, $0.065, 3.7k stock) — and
channel count decouples from MCU pins entirely. Scaling 20 → 16 then becomes a board-space
decision, not a re-architecture, which is exactly the flexibility the target asks for.

## Fuse/coil-circuit detection — user's idea, adopted (2026-08-04)

**The idea:** feed the relay coil from the *fused* terminal and have the board switch the
coil's ground, so a blown fuse is detectable.

**It works, and needs no extra wires.** Sense the low-side switch drain — the coil-return
wire already present. With the channel OFF no current flows, so there is no drop across the
coil and the node sits at rail voltage; the sense divider's lower leg doubles as the
pulldown that defines 0V when the feed is dead. **Two resistors per channel plus one
`74HC165` input, ~$0.15 for all 20.**

Four constraints:

1. **Off-state diagnostic only.** With the channel on, our own switch holds the node near
   0V. Every *off* channel can be monitored continuously; a channel cannot be tested while
   driving.
2. **It detects any coil-circuit break** — blown fuse, missing relay, open coil, broken
   wire. Report it as *coil circuit integrity*, not "fuse blown".
3. **⚠ Topology is the real risk — verify against the actual box before committing.** This
   requires the fuse **upstream**: battery → fuse → relay pin 30, coil fed from that same
   fused node. Many relay/fuse boxes instead run relay pin 87 → fuse → load; there the coil
   could never get power to close the relay in the first place (chicken-and-egg) and the
   scheme does not work.
4. **Parked drain.** A 100k/22k divider is ~0.1mA/channel, ~2mA over 20 channels,
   permanently. The revB ignition latch exists precisely to kill that, so use ~1M/220k
   (~0.24mA total) or feed the dividers from the switched rail.

Side effect to be aware of: with the coil on the fused node, a blown fuse drops its own
relay out immediately. Probably a safety plus, but it is a behaviour change.

## Open decisions

1. **Relay box** — which box, and its internal topology (see the fuse-sense constraint
   above). This is now the gating unknown: it drives channel count, connector and pinout.
2. **Coil supply routing** — does the module feed coil power, or only sink the coil return?
   Low-side sink is assumed throughout.
3. **Which revB features survive** — microSD logging, USB-C, EEPROM config store, SIM7600
   COMMS header, ignition latch, analogue/digital inputs. At this firmware scope most are
   candidates to drop, and at qty 1 each one also carries unique-part fees.

### Settled

- **No load current sensing** — the fuses do that job. Coil-circuit sense above is *not* a
  current measurement.
- **MCU: STM32G431CBT6**, for commonality with the keypad node.
- **Driver: TPL7407L** ×3 (21 channels), not ULN2803 — see the dissipation numbers above.
- **Channel target: 18–20.**

## Parked

- `5.0SMDJ20A` (`C2990361`), the revB load-dump TVS on `D15`/`D16`, went to **0 stock** at
  JLC on 2026-08-04. Not urgent — the revB order is on hold pending this board — but it
  needs a substitute before that BOM is ever uploaded.

## Carried over

See `CLAUDE.md`: BMI270 IMU, the Waveshare DC5-36-TO-DC3V3-5 buck module and its pin
arrangement (socketed), CAN as the control bus, and the SynapsePDM firmware lineage.
