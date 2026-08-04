# RCM — Relay Control Module (and Keypad, same board)

> **Status 2026-08-04: architecture agreed, nothing drawn yet.** This document records the
> cost analysis that motivated the pivot, the agreed architecture, and what is still open.

## The pivot

`pdm14-revB` switches 14 loads with Infineon PROFET+2 smart high-side switches on the
board. This board instead **drives relay coils** and lets an external relay/fuse box carry
the load current — turning an ordinary automotive relay/fuse box into a CAN-controlled
unit.

## ONE BOARD, TWO ROLES (agreed 2026-08-04)

The same PCB builds as either the **relay control module** or the **keypad node**
(superseding the separate `../keypad` design). This is driven by the qty-1 economics
below: two designs means two fab runs, two stencils, two assembly setups and 10 boards to
get the 2 that are needed — one design means one setup, 5 boards, 2 used, and the spares
are genuinely sellable *because* they are the flexible variant.

It works because the two roles are already the same circuit:

| | Keypad role | RCM role |
|---|---|---|
| output stage | low-side sink → LED− | low-side sink → coil− |
| input stage | button to GND or +12V | coil-return voltage sense |
| MCU, CAN, buck, IMU, terminals | identical | identical |

### The input stage that unifies them

One series resistor + divider to GND per input, plus an **optional pull-up to +12V**
(DNP by default). That single resistor position covers all three cases:

| Pull-up | Reads | Use |
|---|---|---|
| not fitted | 12V = circuit intact, 0V = open | **RCM coil / fuse sense** |
| **fitted** | 12V = open, 0V = pressed | **keypad, earth-switching buttons** |
| not fitted | 0V = open, 12V = pressed | **keypad, positive-switching buttons** |

So earth-switching vs positive-switching **inputs** are a fit/no-fit choice, per channel or
board-wide. That answers the "whole board mode" question for the input side for pennies.

### Outputs are low-side only — and that is the right answer

You cannot get high-side switching out of a sink array; it would need a P-FET plus gate
drive per channel (~$8 for 20, but ~60 extra placements, which matters when hand-building).

**For the RCM role it is moot** — the board drives *coils*, and the relay's own contacts
provide whatever switching polarity the load needs. Coil-drive polarity is invisible to the
load, so "positive switching" is a wiring decision at the relay, not a board capability.

It would only bite driving LEDs directly in the keypad role, and 12V LED rings are
essentially always common-anode (LED+ to 12V, LED− switched) — i.e. low-side. **So:
low-side board-wide, no P-FETs.**

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
| Per-channel **current sense** (`IS` pin, `kILIS`) | **dropped deliberately** — the fuses do overcurrent. See the coil-circuit sense below, which is a continuity check, not a measurement |
| Open-load detection | replaced by **coil-circuit sense** (below), which detects a broken coil circuit but not a broken *load* |
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

### MCU: STM32 base + an OPTIONAL ESP32-C3 co-processor (decided 2026-08-04)

At qty 1 the spread between all these parts is **~$2 on the whole project**, so cost is a
non-criterion. Two earlier arguments are withdrawn:

- ~~"Pick G431 for commonality with the keypad."~~ **Retracted** — that rested on the keypad
  being fixed, and it is not; the keypad is being folded into this board instead.
- ~~"STM32 is the more performant part."~~ **Not true.** The classic ESP32 is 240MHz
  dual-core with 520KB RAM against an STM32G431's 170MHz single M4 with 32KB.

Where STM32 actually wins is I/O *quality*, not capability: **5V-tolerant pins** (ESP32 has
none), a far better ADC, **mature CAN** (bxCAN/FDCAN vs ESP32's TWAI, which has an errata
history), and deterministic timing that the ESP32's RF stack can disturb. For switching
relays only the CAN maturity and 5V tolerance really matter.

**So do not choose.** Put an STM32 on the board for the reliable CAN/relay job, and add a
footprint + UART header for an optional **`ESP32-C3-MINI-1-N4`** (`C2838502`, $2.79,
16.6×13.2mm, 16k stock) as a WiFi/BLE co-processor. Fitted only when wireless is wanted, a
4-pin header otherwise. Costs almost nothing unpopulated, and it means the wireless
question does not have to be settled before layout.

Note the specific STM32 is still open — with shift registers absorbing the I/O, the part
only needs CAN, ~6 pins for the register chains, I2C/SPI for the IMU, and SWD. Almost
anything qualifies, so pick on toolchain preference.

### Phone-as-key — deliberately a SEPARATE future node, not this board

Feasible and wanted, but kept off the RCM for three reasons:

1. **It is a security function on a body controller.** The RCM should stay a dumb, reliable
   actuator. Far better for an auth node to publish an authenticated "unlocked" message on
   CAN and let something else gate ignition.
2. **BLE proximity is relay-attackable** — the well-known passive-keyless-entry attack.
   Bare RSSI proximity is not enough; it needs challenge-response with a shared secret, and
   ideally an explicit app action rather than "phone is near".
3. **⚠ NFC has an iOS problem.** Host card emulation is heavily restricted on iPhone and
   only recently opened with conditions; on Android it is straightforward. **Which phone is
   used largely decides whether NFC is viable at all** — if iPhone, BLE is the realistic
   route. Settle this before spending effort.

**The real blocker is power, not the radio.** Phone-as-key needs something permanently
awake, which fights the parked-off architecture. An ESP32-C3 duty-cycling BLE is ~10–100µA
— negligible against a ~50Ah battery — but the **buck module's quiescent draw will dominate
and is likely milliamps**, i.e. ~1–2Ah over a fortnight. The always-on portion needs its own
low-Iq rail. That is a further argument for making it a small separate node.

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

A MOSFET sink array replaces both the switch **and** the flyback diodes:

| | revB | RCM |
|---|---|---|
| switching | 15× PROFET, **$17.25** | 3× **TPL7407L** (21 ch), `C2149827`, $0.39 ea = **$1.17** |
| flyback | n/a | **integral** — the array's `COM` pin clamps every channel, so ~20 discrete diodes disappear |
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

**The keypad role uses the same TPL7407L** (superseding the earlier note that the standalone
`../keypad` board keeps its ULN2803A — that board is being folded into this one). Two
reasons beyond dissipation: the **ULN2803's input resistors are sized for 5V logic**, so
3.3V drive is marginal at coil currents, and the socketed-DIP rationale no longer applies
now that a specific package is not required. One driver part covers both roles.

## Channel count: target 18–20 (user, 2026-08-04)

"Aim for 18–20 outputs; pull back if something doesn't work." With `TPL7407L` being a
**7-channel** part, 3 chips = **21 channels**, which covers the target with one spare.

**I/O expansion makes the count cheap to change.** 20 outputs + 20 fuse-sense inputs = 40
I/O, which no LQFP-48 provides alongside CAN and a crystal. Use shift registers —
`74HC595` for outputs, `74HC165` for sense inputs (`C22384789`, $0.065, 3.7k stock) — and
channel count decouples from MCU pins entirely. Scaling 20 → 16 then becomes a board-space
decision, not a re-architecture, which is exactly the flexibility the target asks for.

### Proposed channel plan

- **21 low-side outputs** — 3× `TPL7407L`
- **21 internal sense taps** on the output nodes (coil-circuit integrity)
- **16 external inputs** with optional pull-ups (buttons in the keypad role)

Sense inputs total 37, so 5× `74HC165` ($0.33) covers it with spare bits. At that price
each channel gets *both* an internal tap and, where relevant, an external terminal, and
firmware decides what a given board is. **Keypad role uses 8 outputs + 8 external inputs;
RCM role uses all 21 outputs.**

### Why shift registers and not a resistor ladder

Asked and worth recording. **The decisive reason is simultaneity, not level shifting.** A
ladder encodes N switches as N voltage levels on one ADC pin, so two closed at once gives an
ambiguous middle voltage. Both roles need all states at once — a keypad must handle
multi-button presses, and the RCM must continuously read 21 independent coil states. A
ladder fundamentally cannot do that.

Secondary: with a 3.3V ADC and 16 levels each step is ~200mV, which is uncomfortably tight
against resistor tolerance and automotive noise on long wires, whereas a digital threshold
just has to be crossed. And a ladder needs precision resistors anyway, so it is not even
cheaper than a $0.065 register. The per-input divider is needed either way — the register
replaces the *discrimination* scheme, not the level shift.

⚠ `74HC165` inputs are **not Schmitt-triggered**, so heavily RC-filtered inputs give slow
edges. Fine in practice here (transitions are infrequent and firmware debounces anyway),
but if bench testing shows chatter, a `74HC14` Schmitt buffer ahead of it is the fix — do
not blame the wiring first.

## Connectors: 3.5mm pluggable, spread over multiple edges (2026-08-04)

User: smallest sensible edge terminals, **not** revB's power-handling blocks, and spreading
across 2–4 edges to shrink the board is fine.

**Family: `KF2EDG` / `15EDG` 3.5mm pluggable** — a board header plus a removable screw plug:

| Part | LCSC | Price |
|---|---|---|
| `KF2EDGVM-3.5-2P` board header | `C441407` | $0.12 |
| `KF2EDGK-3.5-2P` screw plug | `C440847` | $0.18 |

3.5mm pitch, ~8–10A, 16–24AWG — right for coil, LED and button wires, and far smaller than
revB's 5.0mm KF301 power blocks. **The pluggable part matters more than the size**: the
whole loom unplugs from the board for service, consistent with how the rest of this project
is built.

Density is not the constraint — 3.5mm × ~40 positions is ~140mm of edge against ~360mm of
perimeter on a 100×80mm board. **Spread across 2–3 edges anyway** and group by function
(outputs one edge, inputs another, power/CAN a third) rather than purely to save space.

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

1. **Which STM32.** Needs CAN, ~6 pins for the register chains, I2C/SPI for the IMU, SWD.
   Almost anything qualifies — pick on toolchain preference.
2. **Which phone** (iPhone vs Android) — decides whether NFC is viable for the future
   auth node at all. Does not block this board.
3. **Which revB features survive** — microSD logging, USB-C, EEPROM config store, SIM7600
   COMMS header, ignition latch. At this firmware scope most are candidates to drop, and at
   qty 1 each one also carries unique-part fees.
4. **Exact terminal grouping** — how the 37 signal positions split across edges, settled at
   layout once channel count is final.

### Settled

- **One board, two roles** — RCM and keypad build variants of the same PCB.
- **No load current sensing** — the fuses do that job. Coil-circuit sense is *not* a
  current measurement.
- **Driver: TPL7407L** ×3 (21 channels), not ULN2803 — dissipation *and* 5V-logic inputs.
- **Outputs low-side only**; switching polarity comes from the relay contacts.
- **Inputs switchable** earth/positive via an optional pull-up resistor.
- **Channel plan: 21 out / 21 internal sense / 16 external in.**
- **I/O via `74HC595`/`74HC165` shift registers**, not a resistor ladder.
- **Connectors: `KF2EDG` 3.5mm pluggable**, multiple edges.
- **Buck: Waveshare DC5-36-TO-DC3V3-5** from revB (not the keypad's WeAct), socketed.
- **MCU: an STM32, plus an optional `ESP32-C3-MINI-1` co-processor footprint.**
- **Phone-as-key: separate future node**, not this board.

### Relay box — no longer a gating unknown

The fuse-sense topology risk is resolved: the box is **fully re-pinnable** (currently a
Haltech, possibly replaced or self-built), so the required arrangement — battery → fuse →
relay pin 30, coil fed from that same fused node — can simply be wired that way.

## Parked

- `5.0SMDJ20A` (`C2990361`), the revB load-dump TVS on `D15`/`D16`, went to **0 stock** at
  JLC on 2026-08-04. Not urgent — the revB order is on hold pending this board — but it
  needs a substitute before that BOM is ever uploaded.

## Carried over

See `CLAUDE.md`: BMI270 IMU, the Waveshare DC5-36-TO-DC3V3-5 buck module and its pin
arrangement (socketed), CAN as the control bus, and the SynapsePDM firmware lineage.
