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
3. ~~NFC has an iOS problem.~~ **RESOLVED 2026-08-04: the user is on Android**, where host
   card emulation works properly. So **NFC is viable and is the preferred route over BLE** —
   centimetre range makes it inherently resistant to the relay attack in (2), and tapping is
   a deliberate act rather than passive proximity.

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

### Channel plan: 21 UNIVERSAL channels (revised 2026-08-04)

Supersedes an earlier "21 outputs + 16 separate inputs" plan. **Every channel's terminal
serves as either an output or an input**, because the sense node *is* the driver node and a
`TPL7407L` output is open-drain — a channel that is never driven simply floats, and the
terminal behaves as a pure input:

| Build | Pull-up | Terminal behaves as |
|---|---|---|
| RCM coil | not fitted | driven output + coil-circuit sense |
| Keypad LED | not fitted | driven output |
| Keypad button | **fitted** | input; driver never driven for that channel |

This removes ~16 terminal positions and drops the shift registers from 8 to 6. The only
cost is the driver's off-state leakage (~µA) sitting on an input node — negligible. If
firmware ever drives a channel wired as a button, the pull-up simply sources ~12µA to
ground; harmless.

**Final channel plan:**

- **21 universal channels** — 3× `TPL7407L` (7 each)
- **3× `74HC595`** (24 outputs, 21 used)
- **3× `74HC165`** (24 inputs — 21 channels + 3 dedicated board inputs e.g. ignition sense)
- per channel: 2 divider resistors + 1 **optional** pull-up

RCM role uses all 21 as outputs; keypad role uses ~8 as LED outputs and ~8 as button
inputs, from the same 21.

### Three identical 7-channel tiles

The above falls out naturally into **three identical tiles**, each = 1 `TPL7407L` +
1 `74HC595` + 1 `74HC165` + 7 channels' dividers + 7 terminal positions. The tiles
daisy-chain on a 3-wire shift-register bus.

This is the concrete form of the routing argument for keeping shift registers: each tile's
registers sit **directly behind the terminals they serve**, so only 3 signals cross between
tiles instead of 21+ radiating from a central QFP. It is what plausibly keeps the board at
**2 layers**, and it makes layout a copy-paste of one tile — the same approach that worked
on `pdm14-revB`'s channel clusters.

## Assembly: PCBA, with 0805 passives for repairability (decided 2026-08-04)

**JLC PCBA, not hand-build.** ~300 hand joints on a 21-channel board is ~300 chances for a
cold joint to hunt down later, and PCBA makes the `ESP32-C3` module and any fine-pitch part
trivial to fit.

**Passives are 0805, not 0402 — deliberately.** Costs ~8% of component area, accepted by
the user so that a failed part can actually be replaced by hand. Consistent with the
serviceability bias applied throughout this project.

⚠ **The screw terminals and buck header pins are THT**, which JLC surcharges and restricts.
Plan on the realistic split: **JLC places all SMD, the THT terminals are hand-fitted.** That
is a handful of easy joints, not 300 — but do not assume "PCBA" covers the whole board.

**Minimise *unique* part numbers, not part count.** At qty 1 the per-unique-extended-part
fee is a fixed cost, so the divider network should use only 2–3 distinct resistor values
across all 24 inputs, and one terminal family throughout.

### Reconsidered and kept: shift registers over a big MCU

PCBA removes the "fine-pitch is hard to solder" objection, so a single LQFP-100 STM32
(~80 GPIO) could drive all I/O directly and delete the 6 registers — marginally fewer
unique parts and slightly less area. **Kept the registers anyway** for the routing/2-layer
reason above. Revisit for a rev B of this board once the first one is built and the real
routing density is known.

## Floorplan and size estimate

Component area at 0805, roughly:

| | area |
|---|---:|
| ~30 terminal positions (3.5mm pitch × ~10mm deep) | ~1050 mm² |
| Waveshare buck module (33×16 + clearance) | ~600 mm² |
| 6 shift registers + 3 drivers + MCU + CAN + IMU | ~670 mm² |
| optional `ESP32-C3` module | ~220 mm² |
| ~72 divider/pull-up resistors + decoupling @ 0805 | ~460 mm² |
| misc (protection, LDO, crystal, LEDs, SWD) | ~200 mm² |

≈ **32 cm²** of parts; at a typical 2–3× for routing and spacing that lands around
**70–100 cm², i.e. roughly 100 × 80mm** — against revB's 216 × 148mm = 320 cm², a ~4×
reduction. Note this comes mostly from losing the high-current copper, not from PCBA.

Proposed edge allocation:

| Edge | Contents | positions |
|---|---|---:|
| South | 3 channel tiles × 7 | 21 |
| West | power in (2) + commons +12V/GND for keypad LED+/switch returns (4) | 6 |
| East | CAN in/out (2× 3P, daisy-chain) + 3 dedicated inputs | 9 |
| North | buck module + `ESP32-C3`, no terminals | — |

~36 positions ≈ 126mm of terminal edge against ~360mm of perimeter — comfortable, which is
why spreading across edges matters more for grouping than for fit.

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

## MCU: STM32F446RET6, LQFP-64 (decided 2026-08-05)

Same silicon as revB, quarter the area of its ZET6. The user asked whether reusing revB's
chip was worth it "if more capable and roughly the same cost" — both true, but **the cost
that matters here is area, not dollars**: LQFP-144 is 20×20mm = 400mm², ~5% of an ~80cm²
board, plus ~11 VDD decoupling caps and heavy fanout, and it buys nothing because the shift
registers already absorb the I/O. The RET6 is 10×10mm at $4.93.

Why the F446 family over the G431 otherwise preferred:

- **SDIO.** Confirmed present on the die (CMSIS header: `CAN1`, `CAN2`, `USB_OTG_FS`,
  `SDIO`). G431 has none — SD would be SPI-mode only. Keeps microSD cheap while it survives.
- **USB and CAN coexist.** This rules out the F103 floated earlier at $1.04: on F103 they
  **share the same SRAM buffer and are mutually exclusive**, and USB-C is on the keep-list.
- Butchering joesbox's firmware becomes a pin remap rather than a port.

⚠ Verify against DS10693 before drawing that LQFP-64 breaks out USB OTG FS, CAN *and* SDIO
without conflict; fallback is **F446VET6** (LQFP-100, 14×14mm).

## Latch part: BTS7040, not BTS7008 (asked and checked 2026-08-05)

User suggested a BTS7008 on the grounds that tiny current means no heating and therefore no
heavy pours. The goal is right; the swap is not needed, and the part ordering runs the other
way.

**The digits are roughly Rds(on) in mΩ** — BTS7002 = 2mΩ/21A, BTS7008 = 8mΩ/11A,
BTS7040 = 40mΩ/4.5A. Lower number = beefier. 7040 → 7008 is moving *up*.

The latch feeds only the buck (coil current comes from the fuse box), so it carries
~65–165mA:

| | dissipation @165mA |
|---|---:|
| BTS7040 (~40mΩ, ~60mΩ hot) | **1.6 mW** |
| BTS7008 (~8mΩ) | 0.3 mW |

1.6mW in a TSDSO-14 is a couple of degrees — **no pour needed either way, 2-layer is safe**.
Prices are a wash ($0.867 vs $0.924). Keeping the 7040 because revB proved this exact
circuit with that part's `IN` thresholds and divider values; swapping means re-verifying
them for zero gain.

## Feature priority (user, 2026-08-05)

Keep-order: **IMU (non-negotiable) > ignition latch > USB-C > EEPROM**. microSD and SIM7600
are lower value — but the user's call is to **design them in now and delete later if space
demands**, on the principle that simplifying is easier than adding.

Endorsed, with one practical condition: the droppable blocks go in a **physically separable
corner** so deletion does not ripple through the floorplan. Note microSD is not free even
when fitted — it drags back the no-RTC logging problem, since there is no backup cell.

The latch **cannot** be built from a `TPL7407L` channel, as asked: the latch switches the
+12V feed to the buck, which is high-side, and a sink can only pull a P-FET gate — which is
exactly revB's deleted `Q3` bug (source on +12V_P, gate to ground = full rail across Vgs,
−14.4V against ±12V abs max). Use the PROFET; its `IN` is ground-referenced so the problem
does not exist.

## Commons come from the loom (user, 2026-08-05)

Confirmed: the board is an **earth-switching and sensing mechanism**. Coil+ comes from the
fused terminal in the box; keypad LED+ comes from the loom at the button. No +12V
distribution terminals.

**One exception: GND stays on the CAN connector.** That is a signal reference, not power —
CAN transceivers' common-mode range depends on nodes sharing a ground. Spare GND positions
for switch returns are optional: at 12µA through a 1M divider, a 0.5V chassis-ground offset
maps to ~0.09V at the logic pin, so chassis-grounding switches is fine.

## Open decisions

1. **`TPL7407L` input pulldowns** — whether the part defines its inputs internally while the
   `74HC595` outputs are Hi-Z at power-on. Determines whether 21 pulldowns are needed to stop
   relays clacking at boot. See `SPEC.md` block 7; verify against the datasheet.
2. **LQFP-64 pinout** — USB/CAN/SDIO coexistence, above.

### Settled

- **One board, two roles** — RCM and keypad build variants of the same PCB.
- **No load current sensing** — the fuses do that job. Coil-circuit sense is *not* a
  current measurement.
- **Driver: TPL7407L** ×3 (21 channels), not ULN2803 — dissipation *and* 5V-logic inputs.
- **Outputs low-side only**; switching polarity comes from the relay contacts.
- **Inputs switchable** earth/positive via an optional pull-up resistor.
- **Channel plan: 21 UNIVERSAL channels**, each terminal an output or an input.
- **Three identical 7-channel tiles** (driver + 2 registers + 7 channels each).
- **I/O via `74HC595`/`74HC165` shift registers**, not a resistor ladder — kept over a
  big-pin-count MCU for routing/2-layer reasons; revisit for a rev B.
- **PCBA (not hand-build), 0805 passives** for repairability; THT terminals hand-fitted.
- **Phone-as-key uses NFC, not BLE** (user is on Android) — still a separate node.
- **Connectors: `KF2EDG` 3.5mm pluggable**, multiple edges.
- **Buck: Waveshare DC5-36-TO-DC3V3-5** from revB (not the keypad's WeAct), socketed.
- **MCU: STM32F446RET6** (LQFP-64), plus an optional `ESP32-C3-MINI-1` co-processor.
- **Latch: one `BTS7040-1EPA`**, revB's circuit verbatim, driven by a DIRECT MCU GPIO.
- **Commons from the loom**; only CAN keeps a GND pin.
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

## Part sourcing — the four substitutions (2026-08-06)

Sourcing the BOM against JLC's live catalogue forced four changes to what the design first
called for. All four are folded back into `gen_spec.py`, so the schematic, the netlist and
the BOM agree — nothing lives only in `jlc_parts.json`.

**The governing economics:** at qty 1 (min order 5) the per-unique-*extended*-part fee is a
fixed cost, and this BOM has ~48 lines. Those fees, not unit prices, dominate the build. So
a JLC **Basic** part is worth taking even when it costs several times more per piece — the
74HC595 is the clean example: basic SOIC-16 at $0.171 against $0.044 for the cheapest
extended SOP-16, which is 39 cents more across three chips against a fee many times that.

| Was | Now | Why |
|---|---|---|
| LD1117S33 | **AMS1117-3.3** (`C6186`) | Basic part. Both KiCad symbols `extends "AP1117-15"`, so pin-identical by construction; same SOT-223, tab on pin 2. 5V in leaves 1.7V over a ~1.3V dropout. |
| 25LC640 | **M95640** (`C283461`) | $0.33 against $0.73–1.42, and better stocked. Same industry-standard 25-series SPI pinout, which is exactly what the generic `Memory_EEPROM:25LCxxx` symbol describes — symbol and nets unchanged. |
| 12pF crystal load caps | **20pF** (`C1798`, basic) | Not a cost change — the 12pF was **wrong**. See below. |
| 30V PPTC | **60V PPTC** (`C19078719`) | The SMAJ33A clamps near 53V. A 30V fuse sits under its own clamp. |

### The crystal load caps were wrong, and it was the RTC that suffered

Load caps follow `C = 2 x (CL - Cstray)`. The chosen 32.768kHz part (`C32346`, Epson
Q13FC13500004) is a **CL = 12.5pF** crystal, so with ~3pF of stray it wants **~19pF** —
not the 12pF the design had. 12pF leaves the loop about 3.5pF light, which pulls the
oscillator roughly **22 ppm fast: about a minute a month**, on the one oscillator whose
entire job is keeping time. Given how much of the revB review turned on RTC behaviour,
that was worth catching before ordering rather than after.

20pF fixes it to within a few ppm **and deletes a BOM line**, since the 8MHz already used
20pF — one fewer unique part fee.

The 8MHz keeps 20pF against its own 20pF CL, which runs it ~60ppm fast. That is nothing
beside CAN's ~1% and USB FS's 2500ppm budgets, and light load caps only *improve* startup
margin. 33pF would be exact if it ever matters, at the cost of a BOM line.

### The package trap the composite key caught

`jlc_parts.json` is keyed `value|footprint`, not by value. That is what the BOM script
looks up — keyed by bare value it silently matches nothing and every line comes out
unsourced, which is how the map was first written and would have produced an empty BOM.

The key also guards the package, and it earned that immediately: the cheapest **basic**
8MHz crystal (`C12674`) is **HC-49S-SMD**, which would not have fitted the 5032 lands on
this board. The 5032 basic part (`C115962`) costs more and is the correct one.

### Two lines worth knowing about

- **`RESET` switch (`C2886894`, TL3342) is $1.13 with only ~455 in stock** — easily the
  weakest line in the BOM. A cheaper tactile switch needs a different land pattern, so it
  is a next-revision change rather than something to force now. The button is not required
  to program the board: NRST is on the SWD header.
- **`USB-C` (`C165948`, TYPE-C-31-M-12) needs its land confirmed** against the
  `USB_C_Receptacle_HCTL_HC-TYPE-C-16P-01A` footprint. The two are widely treated as
  interchangeable, but that is received wisdom, not something checked against a drawing.

### Status LEDs run dim, deliberately

Green LEDs are InGaN with a Vf around 3.0–3.2V at rated current, so on a 3.3V GPIO through
1k there is very little headroom — perhaps 0.7mA once Vf falls at low current. Visible, but
dim. Dropping `R_LED1`/`R_LED2` to 470R (or reusing the 120R already in the BOM) would fix
it; 120R costs no extra BOM line but pulls ~11mA on the red. Left at 1k because the
indicators sit under the button panel anyway — worth revisiting if they end up exposed.

### 270k stays extended

There is no basic 270k in 0805, and none at 300k or 330k either, so there was no basic part
to move the sense divider *to*. The verified 1M/270k ratio stands rather than being bent
toward a part that does not exist.


## Channel mode became a software table, not hardware (2026-08-06)

The plan to make each channel switchable in hardware — 21 DIP switches, a readback, a
south-half re-layout — was **abandoned, and the board got simpler instead.**

### What I had wrong

The problem was only ever the pull-**up**. `R_PU` fitted as a 10k pull-up to +12V makes a
blown fuse read 2.531V and a good one 2.551V — both HIGH, 20mV apart. Fuse sensing dead.

A 10k pull-**down** does not do that, because the ~85 ohm relay coil completely swamps it:

| Channel state | Divider | Reads |
|---|---|---|
| Output, driver off, fuse GOOD | 2.530 V | HIGH |
| Output, driver off, fuse BLOWN | 0.000 V | LOW |
| Input, button open | 0.000 V | LOW |
| Input, button closed to +12V | 2.551 V | HIGH |

All four correct **with the same resistor fitted on every channel**. Nothing is
mode-dependent, so there is nothing for hardware to select. `R_PU` is now a fitted 10k to
GND on all 21 channels, and in/out is purely a firmware table.

That deleted the DIP array, the readback, the P-FET rails and the re-layout in one go.

### The trade: buttons are positive-switched

Wired to +12V rather than to ground. Two consequences, both improvements:

- **Wetting current 1.20mA**, against 0.0094mA if the sense divider alone were the
  pull-down. That is the difference between contacts that stay good and contacts that go
  intermittent.
- **Chafe is fail-safe.** A button wire rubbing to chassis reads *not pressed*. Earth
  switching reads *pressed* — a phantom activation. The feed to the buttons wants its own
  small fuse.

Standing draw is 25mA with all 21 idle-high; the 1.19mA through an idle coil is far under
the ~100mA a relay needs to pick up.

## Config DIP switch (`SW_CFG`)

One 8-way 1.27mm half-pitch switch (`C6386921`) in the top-right corner — the only free
10x10mm pocket on the board, and conveniently next to `J_CAN2` where the termination pole
has to reach. Switch *k* pairs pins *k* and *(17-k)*, verified off the symbol's own pin
geometry rather than assumed.

| Pos | Function | How |
|---|---|---|
| 1 | **CAN termination** | Passive, parallels `R_TJ` |
| 2 | **Role** — RCM or keypad | PC0 |
| 3 | **Address bit 0** | PC1 |
| 4 | **Address bit 1** | PC2 |
| 5 | **CAN bitrate** 500k/1M | PC3 |
| 6 | **IMU publish enable** | PC4 |
| 7-8 | unconnected | — |

Positions 2-6 use the STM32's **internal** pull-ups, so each switch just shorts a GPIO to
ground — no external resistors. Closed = 0.

**Why these settings and not others:** everything except the IMU bit is something you cannot
fix over the bus once it is wrong — termination, node identity and bitrate all have to be
right *before* CAN talks at all. The IMU bit is there because only one board in a car should
publish orientation, and a keypad bolted into a door card is not that board.

Termination keeps `R_TJ` in parallel deliberately: the DIP for the bench, a soldered 0R for
the car, where a mechanical contact carrying ~17mA could chatter under vibration.

### Positions 7 and 8 are unconnected on purpose

Routing two more nets from the top-right corner across the CAN/IMU corridor to the MCU is
what tipped this board from routable to not — first 3 nets failed, then 1. Dropping the two
spare bits fixed it outright. The switch positions still physically exist for a future
revision; only the copper is gone.

**Verify the DIP land before ordering.** The footprint is the standard 1.27mm 8-position
gull-wing (7.62mm row spacing); `C6386921` was not checked against a dimensioned drawing.
Same caveat as the USB-C.


## Temperature audit — the board is a +70C board (2026-08-08)

Prompted by the question: if one part is rated lower than the rest, that part sets the
board's limit. Correct, and worth doing properly rather than spot-checking. Every line
audited against its JLC datasheet range:

| Limit | Parts |
|---|---|
| **-20 .. +70C** | `SW_RST` TL3342 tactile switch |
| **-30 .. +80C** | `J_USB` TYPE-C receptacle |
| **-40 .. +85C** | `U_MCU`, `U_IMU`, `U_CAN`, `U_EEP`, `U_SI1-3`, `Y1`, `Y2`, `F1` |
| -30 .. +85C | `D_LED1`, `D_LED2`, `SW_CFG` |
| -40 .. +105C | all three terminal blocks |
| -40 .. +125C | `U_DRV1-3`, `U_SO1-3`, `U_LDO`, `D1` |
| -55 .. +155C | every resistor |
| -40 .. +175C | `U_LATCH` |

Capacitors are not stated in JLC's description but follow their dielectric: **X5R is +85C**
(`C_3V3I`, `C_5V`, `C_MB`, `C_VCAP`), **X7R and C0G are +125C** (`C_3V3O` and the other
100nF, `C_BULK`, the crystal load caps).

**So the board is -20 .. +70C, set by the reset switch**, and -30 .. +80C once that is
discounted. A cabin in direct sun can reach 60-80C, so this is not academic.

### What that does and does not mean

These are *guaranteed operating* ranges, not destruction limits — a +70C part does not fail
at 71C, it leaves spec. And the two lowest-rated parts are both **bench items**: the reset
switch and the USB port are used for programming, not while driving. A reset switch that
drifts out of spec on a hot day is an inconvenience; the MCU doing so is a dead board.

That reframes the practical limit as the **+85C cluster** — MCU, IMU, CAN transceiver,
EEPROM, shift registers and both crystals. Raising *that* is not a part swap, it is a
different BOM: industrial-grade STM32, a different IMU, different crystals.

### Consequences

- **+85C is the practical ceiling** for this design. Fine for a cabin; do not mount it in an
  engine bay, which was already the intent (see the mounting note in `CLAUDE.md`).
- **The `C_VCAP` X7R -> X5R swap costs nothing.** It dropped that part from +125C to +85C,
  but the board was already pinned at +85C by parts that cannot be swapped. Worth recording,
  because it turns a hedge into a confirmed non-issue.
- If the +70C reset switch bothers you, **not fitting it is free** — NRST is on the SWD
  header, so the button is redundant. That alone lifts the board to -30 .. +80C, and it is
  also the worst-value line in the BOM ($1.13 each, 455 in stock).


## What is actually on the CAN bus (user, 2026-08-08)

- **rusEFI uaEFI SUPER** — the ECU
- **rusEFI uaDASH** — dash display, Waveshare screen/board
- **RCM** (this board), and a second one built as the keypad

The Haltech in this repo is the **fuse/relay box** and nothing else — it is a passive panel,
not a CAN node. Do not infer an ECU from it. (I made exactly that mistake once and reasoned
about bitrates from it.)

### Why this matters

**Bitrate.** rusEFI's default is **500 kbps**, set in TunerStudio. Worth confirming against
the actual tune rather than assuming, but 500k is the sensible default for the firmware and
for `CFG_BAUD` (config DIP position 5). The hardware covers both rates either way, which is
the point of putting it on a switch — get it wrong and the node is silent, and you cannot
fix that over the bus.

**The protocol is open.** rusEFI is open source with a documented CAN broadcast format, so
integration is reading a spec rather than reverse-engineering a proprietary one. That is a
material difference to how the firmware work will go.

**The dash is a consumer.** uaDASH already renders CAN data, so switch states and IMU output
from this board can appear there without writing any display code — worth designing the
message IDs with that in mind rather than inventing a private format.

## Next revision: one-piece screw terminals instead of two-piece? (user, 2026-08-12)

Raised after JLC refused to ship the mating plugs — they only ship parts the assembly
process solders down, so the `KF2EDGK` plugs bought into the JLC parts library are stuck
there and had to be re-bought from AliExpress. A one-piece screw terminal (`KF128-3.5`,
`DG128`, and similar) would arrive soldered to the board and delete that whole problem.

**The case for it is stronger than it first looks, because of the enclosure.** The
original reason for a two-piece terminal was that you can unplug the loom and lift the
board out. But the enclosure now terminates the loom at DEUTSCH DT bulkhead connectors on
the wall of the box — so the real disconnect point has moved off the PCB entirely. The
loom already unplugs at the enclosure; the board's terminals only ever see short internal
pigtails that stay with the box.

**What is still lost.** Removing the board from its own enclosure goes from unplugging
three connector bodies to undoing up to 27 screw terminals and keeping track of which
wire came from where. That is the difference between a five-minute swap and a careful
afternoon, and it is felt every time the board comes out — which, for a board still under
development, is often.

**Costs, roughly.** Two-piece is header (~$0.30) + plug (~$0.35); one-piece is ~$0.25.
Around $2–3 a board plus a separate order and its shipping.

**Not a factor either way:** wire gauge, current rating and vibration resistance are the
same. Both are 3.5mm-pitch screw clamps and both will need re-torquing in a car.

**Suggested resolution for a revision:** mixed, not all-or-nothing. The three 7-way
channel terminals are the ones with 21 wires and the ones you most want to unplug — keep
those pluggable. `J_PWR`, `J_IGN`, `J_AUX` and the two CAN terminals are 13 wires between
them and are the ones most likely to be landed once and forgotten — those can be
one-piece. That keeps the servicing benefit where it matters and removes most of the
loose-plug purchase.

Bundle this with the other known revision item: `JB1` (the buck's input pins, 4 holes at
3.50mm pitch) is hand-fit because no header is made at that pitch — respace it to 2.54mm.
