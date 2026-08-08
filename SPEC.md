# RCM — schematic specification

> Block-by-block spec for review **before** any KiCad work. Decisions and their rationale
> live in `DESIGN.md`; this file is the buildable description.
>
> **2026-08-05: SIM7600 and microSD DROPPED** on the user's call. Consequences worked
> through below — the biggest is that the LDO **stays** (see block 3), reversing an earlier
> claim that deleting SIM7600 would delete it too.

Target: **140 × 70mm**, **2 layers**, 1oz, HASL, PCBA (SMD) + hand-fitted THT terminals.
Passives **0805** throughout for hand-rework.

## Verified before drawing (2026-08-05)

| Question | Answer |
|---|---|
| Does `TPL7407L` define its inputs when the 595s are Hi-Z? | **Yes — internal 1M pull-down per channel**, explicitly to allow tri-stated drivers. Datasheet: *"When a high-impedance driver is connected to a channel input the TPL7407L detects the channel input as a low level input and remains in the OFF position."* **No external pulldowns needed; no relay clack at boot.** |
| `TPL7407L` ratings | 600mA/channel, 40V outputs; TI targets *"relay drivers with currents less than 250 mA per channel"*. Also a 50k input series resistor forming an RC snubber for noise immunity |
| Is the KiCad `BMI160` symbol valid for a **BMI270**? | **Yes — 14/14 pins identical** (verified against BST-BMI270-DS000 Table 22 / §7.1). revB's substitution was correct |
| Symbols available in KiCad 10 standard libs | `MCU_ST_STM32F4:STM32F446RETx`, `74xx:74HC595`, `74xx:74HC165`, `Interface_CAN_LIN:SN65HVD230`, `Transistor_Array:TPL7407LAPW`, `Sensor_Motion:BMI160` — **all present** |
| Custom symbol needed | Latch PROFET — reuse `BTS70xx-1E` from `../pdm/pdm14-revB/lib/PDM14.kicad_sym` |

✅ **LQFP-64 pinout VERIFIED** against ST's CubeMX AF table for the F446R variant
(`framework-arduinoststm32/variants/STM32F4xx/F446R(C-E)T/PeripheralPins.c`):
**CAN1 on PB8/PB9 (AF9)** and **USB_OTG_FS_DM/DP on PA11/PA12 (AF10)** — available
simultaneously. SDIO is moot now microSD is dropped. No need for the F446VET6 fallback.

⚠ **PB11 does not exist on LQFP-64** — the pin list jumps PB10=29 → PB12=33.

---

## Nets — top level

| Net | Source | Notes |
|---|---|---|
| `+12V_IN` | `J_PWR` | raw battery |
| `+12V_P` | after F1/D1/D2 | permanent, protected. Also the `TPL7407L` `COM` flyback rail |
| `+12V_SW` | latch OUT | switched — feeds the buck only |
| `+5V` | buck module OUT | module default; feeds the LDO only |
| `+3V3` | `U_LDO` | logic rail |
| `CH1..CH21` | tile outputs | universal channel nodes |
| `SNS1..SNS21` | dividers | to `74HC165` |

---

## Block 1 — Power input and protection

| Ref | Part | Notes |
|---|---|---|
| `J_PWR` | KF2EDGVM-3.5-**2P** | `+12V_IN`, `GND` |
| `F1` | PPTC 1A hold, 1812 | board draws ~150–250mA |
| `D1` | SS34 (SMA) | series reverse-polarity |
| `D2` | SMAJ33A (SMA) | load-dump clamp |

## Block 2 — Ignition latch (revB `U19` verbatim)

Proven circuit — **do not re-derive**. A *resistive* OR into `IN`, not a diode-OR.

| Ref | Part | Connection |
|---|---|---|
| `U_LATCH` | **BTS7040-1EPA** (TSDSO-14, `BTS70xx-1E` symbol) | VS(15)←`+12V_P`; OUT(8,9,10,12,13,14)→`+12V_SW`; GND(1)→`R_LG`→GND; DEN(3)→GND; **IS(4) open**; IN(2)=`LATCH_IN` |
| `R_LG` | 47R | RGND network — **required** for ReverseON |
| `R_LPD` | 22k | `LATCH_IN`→GND. Default **off** |
| `R_LIGN` | 47k | `LATCH_IGN`→`LATCH_IN` |
| `R_LHOLD` | 1k | `LATCH_HOLD` (MCU GPIO)→`LATCH_IN` |
| `C_LVS`/`C_LSW` | 100nF | VS decoupling / output snubber |

Ignition 12V → `IN` ≈3.8V; MCU 3.3V → `IN` ≈3.2V. Either turns it on alone.
Dumb switch: DEN low, IS open.

⚠ `LATCH_HOLD` **must be a direct MCU GPIO, never a shift-register output** — bootstrapping
hazard. `J_IGN` feeds `LATCH_IGN` plus a 1M/270k divider to an MCU input for ignition sense.

## Block 3 — Buck + 3V3 — **the LDO STAYS**

`U_BUCK` = **Waveshare DC5-36-TO-DC3V3-5**, socketed (footprints exist in
`../pdm/pdm14-revB/footprints/pdm14.pretty/`): IN 4 holes @3.50mm (`+12V_SW`/GND),
OUT 6 holes @2.54mm (top three `+5V`, bottom three GND).

`U_LDO` = LD1117S33 (SOT-223), `+5V`→`+3V3`.

**Reversing the earlier "delete the LDO" claim.** The module **ships set to 5V**; getting
3V3 out of it requires a per-module pad rework — the same trap already documented on the
keypad (*"its 3V3 pad must be bridged per module or it will cook the STM32"*). Forgetting it
once destroys the board. Keeping a $0.20 LDO removes a destructive manual step, lets the
module stay at its default, and costs little now that SIM7600 is gone: the 3V3 load is
~130mA, about 0.22W in SOT-223 — comfortable without a large copper pad.

## Block 4 — MCU

`U_MCU` = **STM32F446RET6**, LQFP-64 (`MCU_ST_STM32F4:STM32F446RETx`).

- Decoupling per VDD pair + VCAP 2.2µF ×2 (F4 requirement)
- `Y1` 8MHz HSE + 2× 20pF — matches joesbox's stock `RCC_HSE_ON`, PLLM=8
- `Y2` 32.768kHz LSE + 2× 12pF. **No backup cell** (`VBAT`→`+3V3`) — time lost each park.
  With microSD gone this no longer gates logging, so it is now harmless
- BOOT0 10k pulldown + **`J_BOOT` 1×03 select header** (3V3 / BOOT0 / GND) so USB DFU is
  reachable without cutting a track; NRST 100nF + reset switch
- `J_SWD` 1×05 (3V3, SWDIO, SWCLK, NRST, GND); 2× status LED

## Block 5 — CAN

| Ref | Part |
|---|---|
| `U_CAN` | SN65HVD230 (SOIC-8) |
| `D_CAN` | SZNUP2105L (SOT-23) ESD |
| `R_TERM`/`R_TJ` | **120R populated + 0R DNP in series** — project convention, do not invert |
| `J_CAN1`,`J_CAN2` | KF2EDGVM-3.5-**3P**: `CANH`, `CANL`, **`GND`** |

GND on CAN is a **signal reference** (transceiver common-mode range), not power.

## Block 6 — IMU

`U_IMU` = **BMI270**, symbol `Sensor_Motion:BMI160` (pinout verified identical), LGA-14,
I2C, decoupling on VDD and VDDIO.

- **`CSB`(12) → `+3V3`.** Tying it to GND selects SPI mode (BST-BMI270 Table 22).
- **Address strap, mirroring revB:** `R_ADDR` 0R fitted (SDO→GND) = **0x68**;
  `R_ADDR_ALT` 0R DNP (SDO→+3V3) = 0x69. Do not leave both off — SDO has no internal
  pull (the datasheet's 75–140k internal pull-up is on `CSB`, not SDO).
- **`R_SDA`/`R_SCL` 4.7k pull-ups to `+3V3` are required.** The datasheet's "no external
  pull-ups needed" line refers to the *auxiliary* interface only.

## Block 7 — Shift-register chains

| Ref | Part | Role |
|---|---|---|
| `U_SO1..3` | 74HC595 (SOP-16) | 24 outputs, 21 used → `TPL7407L` inputs |
| `U_SI1..3` | 74HC165 (SOP-16) | 24 inputs — 21 channels + 3 dedicated |

Bus: `SCK`, `MOSI`→595, `MISO`←165, `RCLK`, `PL`, `OE_N`. One 100nF per chip.

**Boot state — RESOLVED, no extra parts.** `OE_N` is pulled high (outputs Hi-Z) at power-on;
the `TPL7407L`'s internal 1M pull-downs then hold every channel OFF. MCU clocks in zeros,
pulses `RCLK`, then drives `OE_N` low.

## Block 8 — Channel tiles ×3 (7 channels each)

Three **identical** tiles; lay out one and replicate.

| Ref | Part | Notes |
|---|---|---|
| `U_DRVn` | **TPL7407L** (`Transistor_Array:TPL7407LAPW`, TSSOP-16) | 7ch sink, 600mA/ch. IN1-7←595; OUT1-7→`CHx`; **COM→`+12V_P`**; GND |
| `J_CHn` | KF2EDGVM-3.5-**7P** | 7 channel terminals |

Per channel (×21):

| Ref | Value | Function |
|---|---|---|
| `R_SHn` | **1M** | `CHx` → `SNSx` |
| `R_SLn` | **270k** | `SNSx` → GND; also the pulldown defining 0V on a dead feed |
| `R_PUn` | **10k**, DNP | `CHx` → `+12V_P`. Fit only for keypad button channels — see wetting current below |

⚠ **This was 1M/220k and it FAILS at rest voltage — corrected to 1M/270k.** 74HC `VIH(min)`
is **0.7 × Vcc = 2.31V** at 3V3, not the ~1.65V typical switching point:

| Divider | @11V | @12.0V | @14.4V |
|---|---:|---:|---:|
| 1M/220k (was) | 1.98V ✗ | **2.16V ✗** | 2.60V ✓ |
| **1M/270k (now)** | 2.34V ✓ | **2.55V ✓** | 3.06V ✓ |

At 220k every channel would have read "circuit broken" whenever the battery sat at rest with
the engine off. 270k clears `VIH` from ~10.9V up and still peaks below 3V3 at charging
voltage, so the input protection diodes never conduct. Draw 9.4µA/channel, ~200µA over 21.

⚠ That ~200µA flows **from the fuse box through each relay coil**, not from the latched rail
— it is *not* killed by the ignition latch. Harmless (a coil needs ~150mA) but it is
permanent parked drain.

**`COM` → `+12V_P`:** flyback energy dumps into our protected rail rather than the fuse
box's. Both are battery-derived and our rail has bulk capacitance and a TVS to absorb it;
this is what the keypad already did. `COM` is upstream of the latch so it is always live.

## Block 9 — Optional blocks (physically separable corner)

| Block | Parts | Status |
|---|---|---|
| USB-C | HRO TYPE-C-31-M-12, 2× 5.1k CC, ESD | keep |
| EEPROM | 25LC640 (SOIC-8), SPI | keep |
| ~~ESP32-C3 co-proc~~ | — | **DROPPED 2026-08-05.** PA9/PA10 are ordinary spare GPIO |
| ~~microSD~~ | — | **DROPPED 2026-08-05** |
| ~~SIM7600 header~~ | — | **DROPPED 2026-08-05** |

---

## Connector summary

| Ref | Poles | Edge |
|---|---:|---|
| `J_CH1..3` | 7 each = **21** | South |
| `J_PWR`, `J_IGN` | 2 + 2 | West |
| `J_CAN1`, `J_CAN2`, `J_AUX` | 3 + 3 + 3 | East |
| | **34 positions** ≈ 119mm | |

## Build pipeline

Uses the shared scripted-board tooling, frozen into `tools/` per project convention:

```
gen_spec.py → spec.json → tools/sch_gen.py --verify → rcm.kicad_sch   [DONE]
            → board_plan.json → tools/plan_lint.py → tools/netlist_to_board.py
            → tools/route_board.py → tools/copper_builder.py → tools/gen_jlc_bom_cpl.py
```

**Never hand-edit `rcm.kicad_sch`** — it is regenerated from scratch by `gen_spec.py`.
Fold changes into the generator.

## Build state — schematic DONE (2026-08-05)

`python gen_spec.py && python tools/sch_gen.py spec.json --verify`

| | |
|---|---|
| Components | **154** |
| Nets | **123** |
| ERC | **0 errors** |
| Netlist round-trip | **MATCH** |

### Bugs caught during the build (none of which ERC would have found)

1. **MCU pins 31/47/63 are VSS but typed `passive`** in the KiCad symbol, so they were
   being silently auto-NC'd — the board would have shipped with three MCU grounds
   floating. Now explicitly on `GND`.
2. **PB11 does not exist on LQFP-64** (the pin list jumps PB10=29 → PB12=33). `IMU_INT1`
   was assigned to it; moved to PA1.
3. **Status LEDs were wired wrong** — the resistor went MCU→GND with the LED dangling on a
   single-pin net. Now MCU → R → anode(2), cathode(1) → GND. A `gen_spec.py` lint now
   **fails the build on any single-pin net**, since that is almost always this class of bug.
4. **IMU `CSB` was tied to GND, which selects SPI mode.** BST-BMI270-DS000 Table 22 requires
   `CSB → VDDIO` for I²C. Now on `+3V3`.

ERC-clean and round-trip MATCH only prove the netlist is self-consistent, not correct —
all four of the above passed both before being found by hand-review against datasheets.

### Second review pass, before PCB (2026-08-05)

Three more issues found by reviewing the generated netlist against datasheets — again, all
of them ERC-clean:

5. **No I²C pull-ups at all.** `R_SDA`/`R_SCL` 4.7k added. revB has these (R9/R10); this
   schematic simply lacked them, and the IMU would never have responded.
6. **Sense divider failed `VIH` at rest voltage** — see block 8. 220k → 270k.
7. **No way to reach USB DFU** — BOOT0 was pulled down with no jumper. `J_BOOT` added.

**revB was checked for the same IMU faults and is CORRECT on both counts**: `CSB` → `+3V3`,
and R57 (0R, fitted) pulls SDO to GND for 0x68 with R58 (DNP) as the 0x69 alternate.

### Deferred / accepted

- **`USB_VBUS` is not sensed** — fine for a self-powered device doing DFU, but there is no
  VBUS-detect path.
- **`R_RS` = 10k** puts the CAN transceiver in slope-control mode; change to 0R for
  full-speed edges if EMI turns out not to matter.
- Terminal body dimensions for `KF2EDG-3.5` came from catalogue figures, not a dimensioned
  drawing — they affect silk/courtyard only, never pads, but check against a real part.

## Build state — placement DONE (2026-08-05)

`python gen_plan.py && python tools/plan_lint.py board_plan.json`
then `"C:\Program Files\KiCad\10.0\bin\python.exe" tools/netlist_to_board.py board_plan.json`

| | |
|---|---|
| Board | **140 × 70mm**, 154 components placed |
| Placement lint | **0 errors, 0 warnings** |
| Board DRC | **0 errors** (111 silkscreen warnings, handled in the silk pass at the end) |
| Unconnected | 414 — expected, nothing is routed yet |

**Never hand-edit `rcm.kicad_pcb`** — `netlist_to_board.py` rewrites it from scratch.
Fold changes into `gen_plan.py`.

### Layout

**Outline is set by the keypad button panel** (user, 2026-08-05): 8 momentary switches in
2 rows of 4 at 35mm each = **140 × 70mm**. That is 98cm² against the earlier 100 × 100mm
draft's 100cm² — the same area reshaped, not a resize, so nothing was given up for it.

- **South half**: the three identical 7-channel tiles, anchored at x = 8.8 / 52.8 / 96.8,
  terminals on the south edge at y = 65 and **centred as a group on that edge**. Each tile is defined **once** as a group of 27
  members and instantiated three times — 81 of the 154 parts come from that one definition.
  The driver and both shift registers sit in a row **north of** the resistors. The channel
  columns are spread at **6.0mm pitch, wider than the terminal's 3.5mm poles**, so the
  resistor rows and the registers above them spread evenly along the board's length rather
  than bunching into tight clusters with dead space between — that spacing is also routing
  room. ~8mm of margin is left at each end.
- **North half** (y 0–36): power entry + latch on the left, buck module centre-left,
  MCU centre-east, CAN / IMU / EEPROM / USB / aux far east.
- All terminals sit on an edge with wire entry pointing **off** the board — south at rot 0,
  north at rot 180. **No 90° terminal rotations anywhere**, which is where orientation
  mistakes come from.

### ⚠ The buck module trap, and why no checker catches it

`JB1` (IN) and `JB2` (OUT) are separate footprints whose courtyards never touch, so nothing
verifies they line up. The columns must be **27.80mm apart in X and offset 1.90mm in Y** —
on the Waveshare drawing the IN column's first hole is 3.60mm from the module's top edge and
the OUT column's is 1.70mm, so OUT leads IN by 1.90mm. They were initially placed level,
which would have meant the module physically did not fit.

The resulting module body occupies **x 42.4–75.4, y 10.4–26.4** and that area must stay
clear: the body is drawn on `F.Fab` only, deliberately without a courtyard, so `plan_lint`
cannot see it. **`gen_plan.py` now hard-asserts** nothing is placed inside it — that assert
immediately caught three parts (both status LEDs and a resistor) sitting under the module
after the 140 × 70 reshape, which the placement lint had passed as clean.

### ⚠ Group members and refs are paired by POSITION — now structurally prevented

`expand_placement` pairs `members[i]` with `refs[i]`, so reordering the offsets without
reordering the ref list silently moves parts into each other's slots. That happened **twice**
during this layout (once putting `U_SI` in `C_SO`'s position on all three tiles), and both
times it only surfaced because the two parts happened to collide — a reorder landing
somewhere legal would pass every check and still be wrong.

`gen_plan.py` now defines the tile as a list of `(dx, dy, rot, ref-template)` tuples and
derives both lists from it, so the offset and its ref cannot drift apart.

### Status LEDs and mounting holes — both settled 2026-08-05

Status LEDs sit **beside `J_PWR`** on the north edge (user's call), on the understanding
they will probably end up covered anyway.

Mounting holes stay in the corners: **the button panel is being 3D-printed after this board
and will be made to suit it**, so the board drives the panel's fixing pattern rather than
the other way round. No enclosure constraint to wait on.

### Connectors

Both groups of terminals are **centred on their edge**, and the four mounting holes are at
the **corners** (5,5 / 135,5 / 5,65 / 135,65) — the centred connectors are what freed the
corners up. Holes clear the board edge by 5mm, enough for an M3 washer.

### Next: routing

Draw the copper tracks, pour the ground fill, then re-check clearances. Expect this to raise
at least one "these two things want the same space" question worth a decision.

## Build state — ROUTED (round 2, 2026-08-05)

`"C:\Program Files\KiCad\10.0\bin\python.exe" tools/route_board.py rcm.kicad_pcb --zone GND:F.Cu --zone GND:B.Cu`

| | round 1 | round 2 |
|---|---|---|
| Nets routed | 413 / 414 | **414 / 414** |
| Unconnected | 1 | **0** |
| Track segments + vias | 2143 | 2225 |
| GND pours | 2 (F.Cu + B.Cu) | 2 |
| Router time | 63s | 48s |

**The layout needed no congestion changes** — the spread-out placement routed first time.
Only two real issues came out of round 1:

1. **Starved thermals** (5 in round 1, 9 in round 2). A ground pad reached the pour through
   one narrow neck instead of several. On reflow-soldered parts the thermal relief buys
   nothing — its purpose is heat isolation for *hand* soldering — so the fix is a solid
   connection, which is also lower inductance and more copper. `tools/fix_starved_thermals.py`
   reads the DRC report rather than hard-coding pads, so it stays correct across re-routes.
   **Which pads get starved changes every re-route — always re-run it after routing.**
2. **`USB_CC1` unroutable in round 1.** The CC pin sits in a 0.5mm-pitch pad row with no
   escape gap, and the resistors were placed on the side where the second pad row blocks the
   way. Moving them to the connector's open side fixed it.

### Round 3 (2026-08-05) — USB-C rotated, headers and resistors nudged

- **`J_USB` rotated 0° → +90°**, opening out of the east edge instead of the north.
- **`J_SWD` / `J_BOOT` moved right and down**, clearing the buck module better.
- **All southern resistor rows moved down 2mm**, closing the gap to their own terminals.

⚠ **Rotating the USB-C moves its CC pads, and the whole pad row is on ONE line** — the 16
signal pads all share `local y = -3.745`, so there is no asymmetry in the pad data to
predict orientation from. At +90 the row runs vertically at **x ≈ 129.26** (CC1 y 21.25,
CC2 y 18.25). Those numbers were **read back off the placed board**, not predicted — local
pad coordinates do not change under rotation, only the footprint angle does. The CC
resistors, I²C pull-ups and CAN slope resistor all moved as a knock-on.

⚠ **Still unverified: which way the opening actually faces.** The body sits east of the pad
row, which implies the cable entry is on the east edge as intended, but that is inference.
Confirm visually; if it faces inward the fix is 270° instead of 90°.

| | round 1 | round 2 | round 3 |
|---|---|---|---|
| Nets routed | 413/414 | 414/414 | **414/414** |
| Unconnected | 1 | 0 | **0** |
| Segments + vias | 2143 | 2225 | **2346** |
| Errors after thermal fix | 0 | 2 (pending USB) | **0** |

Current DRC: **0 errors**, 111 silkscreen warnings, 0 unconnected.


## ROUTED AND CLEAN — round 5 (2026-08-05)

| | |
|---|---|
| Nets routed | **414 / 414** |
| Unconnected | **0** |
| Track segments + vias | 2214 |
| GND pours | 2 (F.Cu + B.Cu) |
| **DRC errors** | **0** |
| Warnings | 114, all silkscreen text |

### ⚠ USB-C opening was 2.8mm INSIDE the board edge

Rotating `J_USB` to +90 pointed the opening east correctly, but left the connector's front
face at x=137.18 on a 140mm board — a plug would have fouled the PCB before seating.
Moved to x=135.8 so the face is flush with the edge. `plan_lint` now warns `NEAR EDGE` for
`J_USB`, which is the **correct** result for an edge connector, not a problem.

Derived from the placed board, not predicted: at rot 90 the front face is the courtyard's
local `+y` extreme (`4.18`), which maps to `+x`.

### ⚠ BOOT0: identical output across runs means it is NOT stochastic

`BOOT0` (`U_MCU` pad 60, at 78.25/13.325 on the 0.5mm-pitch north edge) failed to route on
three consecutive runs with a **byte-identical 2.0617mm stub and identical score**. It was
initially dismissed as a freerouting artifact worth retrying — wrong. Freerouting genuinely
is stochastic, so "run it again" is sometimes right, but **identical results are the tell
that it is deterministic and therefore a real geometric problem.** Compare the numbers
before retrying.

The cause: the five MCU decoupling caps sat in a row directly north of the QFP, leaving a
**0.6mm slot** between two of them as the only escape from pad 60. Fixed by leaving a
deliberate gap in that row at x=78 and putting `R_BOOT` directly north of the pad, turning a
QFP escape into a short hop.

### ⚠ `fix_starved_thermals.py` misses THROUGH-HOLE pads

Its DRC-report parser matches `Pad N [NET] of REF` but the report writes **`PTH pad N ...`**
for through-hole, so it silently reports "nothing to do" while those violations remain.
`J_SWD` pad 5 needed `(zone_connect 2)` set by hand. Worth fixing in the shared tool.

**Which pads get starved changes on every re-route — always re-run the thermal pass after
routing, then re-run DRC to confirm it actually cleared.**

### Still outstanding

- **3D models** are missing on the locally-generated footprints: the five KF2EDG terminals
  and the buck IN column (`JB1`). Cosmetic for fab — models never reach the gerbers, BOM or
  CPL — but they matter here because the button panel is being 3D-printed to suit the board,
  and the terminals are the tallest parts on it (~9mm proud). Attach before designing the panel.
- **Silkscreen pass** (114 warnings) and then the manufacturing package.


## Momentary buttons — yes, and they are the right choice (2026-08-05)

Asked whether the input channels work with momentary push-buttons acting as latching
"switches" in software. **They do, and momentary is better than a real latching switch here.**

The hardware only ever reports "contact closed" or "contact open"; the latching lives
entirely in firmware. Press -> toggle state -> CAN -> relay switches -> module reports actual
state back -> the LED follows *that*. A mechanical latching switch can be **wrong**: if
something else turns a circuit off (another node, a fault, the module refusing) the switch is
physically stuck in the "on" position and needs two clicks to resync. A momentary button has
no position to be wrong about, so the LED is always free to show the truth.

### ⚠ Pull-up changed 100k -> 10k for switch WETTING CURRENT

At 12V a 100k pull-up passes only ~120µA through a closed contact. That is below the
~1mA most mechanical contacts need to punch through the oxide/sulphide film that forms on
the contact faces. Gold-plated contacts are fine at microamps; ordinary automotive buttons
are not, and the failure mode is the bad kind — **perfect on the bench, intermittent months
later in a damp vibrating car.**

10k gives 1.2mA on press and does not move the logic levels at all (sense still reads
~2.53V released, 0V pressed). It draws that 1.2mA only while a finger is on the button.

Applied to the schematic *and* patched into the placed board in-place — a value-only change,
so the board was NOT regenerated, which would have wiped the routing.

### Firmware notes

- **Debounce is mandatory.** Contacts bounce 5–20ms; a shift-register poll will read a bounce
  as a press. Poll ~10ms and require two consistent reads, or single presses will sometimes
  toggle twice.
- **Each button costs TWO channels** — one input for the contact, one output for its LED. So
  8 buttons uses 16 of the 21, leaving 5 spare.


## Crystal placement corrected (2026-08-06)

Both crystals were on the wrong part of the board and nothing flagged it — they were
legally placed and fully routed the whole time.

`U_MCU` pin positions, read off the placed board: **PC14/PC15 (LSE) at (82.53, 22.09/23.36)
and PH0/PH1 (HSE) at (82.53, 24.64/25.91)** — all four on the chip's WEST edge.

| | was | now |
|---|---|---|
| `Y1` 8MHz HSE | x=69, **14mm** from its pins | (76, 27), **6.8mm** |
| `Y2` 32.768kHz LSE | x=99 — **the wrong side of the chip**, 20mm | (76, 21), **6.8mm** |

`C_VCAP`/`C_VDDA` moved south of the MCU to free that edge. The last blocker was `J_BOOT`,
a 3-pin jumper with no placement constraints at all — several attempts went into fitting a
capacitor around it before moving the jumper solved it instantly. **Check whether the thing
in the way is the thing that has to stay.**

## Freerouting: stochastic or deterministic? COMPARE, do not assume

Both happen, and they need opposite responses:

| Net | Symptom | Verdict |
|---|---|---|
| `BOOT0` | 3 runs, **byte-identical** stub and score | **deterministic** — real geometry (0.6mm escape slot). Retrying was useless |
| `LATCH_GND` | 2 runs, **different** result and score | **stochastic** — second run routed it |

The test is one re-run and a comparison of the numbers. Assuming either way has already cost
time in both directions on this board.

## Final routed state

| | |
|---|---|
| Nets routed | **414 / 414** |
| Unconnected | **0** |
| DRC errors | **0** |
| Warnings | 103, all silkscreen |

Placement is DONE. Remaining: silkscreen pass, 3D models for the locally-generated
footprints (needed before the panel is designed), then the manufacturing package.

## Silkscreen pass + manufacturing files (2026-08-06)

### Silkscreen: 103 → 27 findings

Two levers, in this order:

1. `tools/relocate_refs.py` against a fresh DRC report, run repeatedly (103 → 47). It
   converges when it runs out of free space — a second pass moved 1 label and a third moved 3.
2. **Shrinking the reference text to 0.8mm / 0.15 stroke** (JLC fab minimum) did more than
   relocation: 47 → 30 on its own, then a final relocate → **27**. Only the `Reference`
   property is shrunk; `Value` and user text are untouched.

The remaining 27 (14 text-over-text, 13 silk-over-pad) are cosmetic. Fabs clip silk off
pads automatically. revB's comparable floor was 19 on a much less dense board.

### Gerbers — DONE, PCB pricing is unblocked

`mfg/rcm_gerbers.zip`, 14 files. Verified:

| Check | Result |
|---|---|
| Outline | **exactly 140.00 × 70.00mm** |
| `G36` filled regions in Edge.Cuts | **0** — no phantom internal cutouts |
| PTH drills | 0.3 / 0.6 / 1.0 / 1.2mm — all standard |
| NPTH | 0.65mm, 3.2mm (M3 mounting holes) |
| Layers | standard KiCad names, `--no-protel-ext` |

### ⚠ BOM/CPL still needs 49 LCSC part numbers

`tools/gen_jlc_bom_cpl.py` is deliberately driven by a parts map rather than reading values
off the board, because a value like "100nF" is not a purchasable part — JLC's uploader will
silently auto-match it to *something*, and the failure mode is a ferrite bead matched to a
resistor, or a 16V cap on a 12V rail.

**`jlc_parts.json` does not exist yet for this board.** 49 distinct values need an explicit
LCSC number. Known from this session's sourcing work:

| Part | LCSC |
|---|---|
| TPL7407L | `C2149827` |
| 74HC165 | `C22384789` |
| BMI270 | `C2836813` |
| SN65HVD230 | `C12084` |
| STM32F446RET6 | `C69336` |
| BTS7040-1EPA | `C534837` |
| KF2EDGV-3.5 header | `C441321` 2P / `C441322` 3P / `C441326` 7P (+ screw plugs separately) |

**SOURCING COMPLETE (2026-08-06)** — 44 of 48 lines carry an LCSC number, and the four
that do not are the 2.54mm headers fitted by hand, excluded from BOM and CPL through
`exclude_refs` in `jlc_parts.json`.

The full map lives in `jlc_parts.json`, keyed **`value|footprint`** — that composite key is
what `gen_jlc_bom_cpl.py` looks up, and keyed by bare value it silently matches nothing.
`gen_spec.py` now reads the same file and stamps an `LCSC` field onto every symbol, so a
part number is edited in exactly one place and flows into the schematic from there.

Four substitutions came out of sourcing, all folded back into `gen_spec.py`: **AMS1117-3.3**
for the LD1117S33, **M95640** for the 25LC640, a **60V** PPTC over a 30V one, and **20pF**
crystal load caps replacing 12pF — that last one a genuine error, not a cost change, worth
about a minute a month of RTC drift. Rationale for each is in `DESIGN.md`.

Two lines still want a human eye before the order goes in: the **USB-C land** wants
confirming against the HCTL footprint, and the **TL3342 reset switch** is $1.13 with ~455
in stock, which is a poor line but needs a different footprint to improve.


## Build state — ROUTED, SILKED, MANUFACTURING FILES OUT (2026-08-06)

Round 3. Resistor rows re-ordered per the user, four part substitutions folded in, and the
full manufacturing set generated.

| | |
|---|---|
| Routing | **414 / 414 nets, 0 unconnected** (40s, score 994.78, 2164 segments) |
| Copper pour | **GND on F.Cu and B.Cu**, both filled, 33 polygons |
| DRC | **0 unconnected, 0 schematic parity** |
| Violations | 26 silkscreen + **1 clearance, 1.3 microns short** (see below) |
| Gerbers | `mfg/rcm_gerbers.zip`, 14 files, 140.00 x 70.00mm, 0 G36 regions |
| BOM | `mfg/bom_jlc.csv`, 39 lines, **every line has an LCSC number** |
| CPL | `mfg/cpl_jlc.csv`, 127 placements, centroid, Y negated |

### Resistor rows (user, 2026-08-06)

Row *centres* keep the original 6.0 / 10.5 / 15.0mm spacing from the terminal; what changed
is which resistor sits in which row and how they lie:

| Row | Was | Now | Rotation |
|---|---|---|---|
| South (nearest terminal) | `R_SH` | **`R_PU`** | 0 |
| Middle | `R_SL` | **`R_SH`** | 0 |
| North | `R_PU` | **`R_SL`** | 180 |

Laying them flat helped the silkscreen materially — silk violations in the south tiles all
but vanished, and the remaining crowding is entirely in the dense north cluster.

### The one clearance violation is understood and accepted

A `+5V` track passes `U_LDO` pad 1 (GND) at **0.1987mm against a 0.2000mm rule — 1.3
microns short**, about 1/40th of a normal fab tolerance, where JLC's actual capability is
0.127mm. It will fabricate without incident.

It is **not** a random router artifact: the identical segment at identical coordinates
recurred across independent routing runs, so it is deterministic geometry. Three fixes were
tried and rejected:

1. **Narrowing the segment** (`tools/fix_marginal_clearance.py`) works geometrically but
   this board's minimum track width is also 0.2mm, so it just trades a clearance error for
   a track-width error.
2. **Moving the segment** risks breaking connectivity at the junctions either side.
3. **Routing against a 0.25mm clearance** and verifying at 0.20mm — the principled fix —
   makes the board **unroutable**: 60-63 nets failed after 7 minutes per pass, against 71
   seconds and a complete route at 0.20mm. This board is too dense for that margin.

Left as-is deliberately. Giving `U_LDO` more elbow room in `gen_plan.py` is the real fix if
it ever needs to be formally clean.

### Silkscreen: 27 -> 26, on a board that routed differently

`tools/shrink_refs.py` (new) drops crowded reference text to 0.8mm/0.15 stroke — JLC's
minimum — before `relocate_refs.py` runs, which is what lets the relocator converge instead
of reporting "no free spot". Overlapped text is unreadable; small text is not, and this
board is built for hand-rework.

### Gerber trap: do NOT pass `--subtract-soldermask`

The first export used it and put **590 front / 62 back pad-shaped flashes in clear polarity
(`%LPC*%`) into the silkscreen gerbers** — KiCad faithfully knocking silk out from over
pads. Negative-polarity silkscreen is handled inconsistently by fabs, and they clip silk off
pads as standard practice anyway. Exporting without the flag gives **0 flashes, 0 clear
blocks**.

This is the same signature flagged on pdm14-revB. Whether revB's was this flag or a genuine
fault was not re-checked here.


### The ground pour went missing, and no check caught it

Round 3 was first routed **without `--zone`**, which defaults to empty in `route_board.py`.
The result was a fully routed, 0-unconnected, DRC-clean board with **no copper pour on
either layer** — and nothing in any report said so, because a missing zone breaks no rule.
The user spotted it by eye off the 3D view.

Re-routed as `--zone GND:F.Cu --zone GND:B.Cu`, then `fix_starved_thermals.py` set three
pads (`J_USB.A12`, `J_USB.B1`, `U_MCU.47`) to solid zone connection where thermal spokes
came up short.

Cheap ways to confirm the pour is actually there, since the tooling will not tell you:

- Count **top-level** zone blocks (`
	(zone
`). Grepping for `(zone` matches the
  `zone_connect` token inside every pad and reports dozens of false hits.
- The copper gerbers should carry `G36` filled regions — F.Cu ~27, B.Cu ~6 here.
- The zipped gerber set is ~120kB with no pour and ~280kB with it.


## Latch part changed on a stock-out: BTS7040-1EPA -> -1EPZ (2026-08-06)

JLC's quote came back with **`C534837` (BTS7040-1EPA) at zero stock** — 38 of 39 BOM lines
priced, the latch missing. Replaced with **`C534838`, BTS7040-1EPZ**.

Why this one rather than a different PROFET:

- **Same base part number.** The `Z` is a temperature/variant grade, not a different die,
  so pin identity is near-certain rather than resting on family convention.
- Same `PG-TSDSO-14-22` package, so the layout is untouched.
- Better on both specs that matter here: **19mOhm** against 36, and **-40..+175C**
  against +150.
- Cheaper: $1.07 against the -1EPA's $0.79... marginally dearer per piece, but irrelevant
  against being unbuyable.

**The generic symbol is what made this cheap.** `RCM:BTS70xx-1E` was built as a family part
(pins 1 GND, 2 IN, 3 DEN, 4 IS, 8-14 OUT, 15 pad VS), so absorbing a stock-out in the
PROFET+2 1-channel line costs a value string, not a re-layout.

**Watch the stock: only ~10 pieces.** Enough for this run of 5 with spares, but if it goes,
the fallback is **`C534825` BTS7004-1EPP, ~20k in stock, $1.85** — 4.4mOhm/15A, wildly
overkill for a 500mA latch but harmless, same package and family pinout. That one relies on
family convention for the pinout rather than being the same base part, so it would be worth
a datasheet check first.

### The 1.3 micron clearance miss is gone

The re-route that came with this part change happens to avoid it. The board is now **clean
of every non-silkscreen violation**: 0 unconnected, 0 schematic parity, 26 cosmetic
silkscreen warnings and nothing else. Worth noting it was route-dependent after all, so the
earlier "deterministic geometry" reading was only true across the routes tried at the time.

## Quote received (2026-08-06) — the pivot worked

**~$250 for 5 boards, against revB's ~$500.** PCB $31.80 + PCBA $217.93.

The line that validates the whole basic-parts strategy is **Feeders Loading fee: $53.55** —
about half the $110.98 component cost, charged per unique part. That is exactly the fixed
per-part fee the sourcing pass was optimised against, and it is why a basic 74HC595 at
$0.171 beat an extended one at $0.044.

`X-Ray Inspection $8.20` is the BMI270 LGA-14 — bottom-terminated, no visible joints. It is
also the reason ENIG is worth taking over HASL.


## D1 WAS WIRED BACKWARDS (found by the user, 2026-08-08)

`Diode:SS34` follows KiCad's convention of **pin 1 = K, pin 2 = A** — not the pin-1-is-anode
you would assume. The netlist had `D1.1` (cathode) on `+12V_FUSED` and `D1.2` (anode) on
`+12V_P`, i.e. the reverse-polarity diode **facing the wrong way**. It would have blocked the
battery feed outright and the board would never have powered up.

Caught by looking at the schematic symbol and noticing the cathode bar pointed at the fuse.

**Nothing automated caught this, and nothing was going to.** ERC passes either orientation,
because a diode is a valid two-pin connection both ways round. DRC, the netlist round-trip
and the BOM are all equally blind to it. This is the same class of fault as the four found in
the first schematic review — correct-looking connectivity, wrong circuit.

### The other polarised parts were checked and are correct

| Part | Symbol convention | Wiring | Verdict |
|---|---|---|---|
| `D_LED1/2` | `LED` pin 1 = K, 2 = A | anode(2) to resistor, cathode(1) to GND | correct |
| `D2` SMAJ33A | `D_TVS` pins A1/A2 | pad 1 to `+12V_P`, pad 2 to GND | correct |
| `F1` | Polyfuse, unpolarised | — | n/a |

`D2` deserves a note: `Device:D_TVS` is drawn as a **bidirectional** TVS (both pins named
"anode"), but SMAJ33A is **unidirectional**. The schematic symbol therefore cannot express
the polarity. It comes out right because the `D_SMA` footprint's pad 1 is the cathode, and
pad 1 is on `+12V_P` — but the symbol is misleading and worth swapping for a directional one
if this is ever revised.

### Everything downstream is now STALE

The fix changes the netlist, so both boards and both manufacturing sets were built from the
wrong circuit:

- `rcm.kicad_pcb` (2-layer) — **stale, do not order**
- `rcm4.kicad_pcb` (4-layer) — **stale, do not order**
- `mfg/` and `mfg4/` — **stale**

Both need `netlist_to_board.py`, a re-route, the thermal and silk passes, and fresh
manufacturing files.


# ORDERED — 2026-08-08

Order placed with JLCPCB. The set uploaded was **`mfg4/`** (4-layer, `rcm4.kicad_pcb`).

Final state at order time:

| | |
|---|---|
| DRC | **0 violations** — electrical and silkscreen |
| Unconnected | **0** |
| Schematic parity | **0** |
| Footprints | **all verified against manufacturer drawings** |
| Stackup | 4-layer 1.6mm, leaded HASL |
| BOM | 43 lines, every one with an LCSC number |
| CPL | 152 placements |

Order-side settings (confirm against the actual JLC order if it matters later): Standard
PCBA — required for the BMI270 — which adds ~$25 setup and panelises to 140x80mm with two
5mm breakaway rails. Leaded HASL over ENIG, on the grounds that the X-ray inspection already
in the quote covers the LGA-14's hidden joints.

## What still needs doing

- **3D models** for the five KF2EDG terminal footprints and `JB1`. Deferred throughout, but
  now on the critical path: the button panel is designed to suit the board, and it cannot be
  modelled accurately without them.
- **Firmware.** Target is the rusEFI bus at 500kbps — see the CAN peers section in
  `DESIGN.md`. `CFG_ROLE`, `CFG_ADDR0/1`, `CFG_BAUD` and `CFG_IMU_EN` are read on PC0-PC4.
- **`JB1` hand-fitting.** 4 pins at 3.50mm pitch, excluded from assembly because no header
  exists at that pitch. Respacing it to 2.54mm is the obvious fix for any future revision.

## Three faults caught late, worth remembering how

- **No ground pour** — `route_board.py --zone` defaults to empty; a fully routed, DRC-clean,
  0-unconnected board came back with no copper pour and nothing flagged it.
- **D1 backwards** — `Diode:SS34` is pin 1 = K. ERC passes either orientation.
- **Stale manufacturing files** — regenerated from an older board state.

Two of the three were found by the user looking at the board, not by any check in this
pipeline. Every automated gate was green throughout.


## Mating plugs — buy separately from LCSC (2026-08-08)

JLC fits only the **board-side headers**. The screw plugs that mate with them are ordinary
parts you buy yourself; they are not on the BOM and never were.

`KF2EDGK-3.5-xP` is the plug half of the `KF2EDGV-3.5-xP` headers on the board:

| Plug | LCSC | Mates with | Per board | 5 boards | Unit |
|---|---|---|---|---|---|
| 2P | `C440847` | `J_PWR`, `J_IGN` | 2 | 10 | $0.21 |
| 3P | `C440848` | `J_AUX`, `J_CAN1`, `J_CAN2` | 3 | 15 | $0.28 |
| 7P | `C440852` | `J_CH1`, `J_CH2`, `J_CH3` | 3 | 15 | $0.64 |

About **$16** for all five boards. Worth buying the full set rather than just for the two
being used — spares matter when making up looms.

**Watch the 7P: only ~513 in stock** against 15 needed, the thinnest of the three.


# 4-layer build notes (was FOURLAYER.md)

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


## Standard PCBA required, and the edge-rail problem (2026-08-08)

JLC will not place the **BMI270** under Economic PCBA — it needs **Standard PCBA**. There is
no way around it: LGA-14 is bottom-terminated with no reachable joints, so "do not place"
would mean hand-soldering something that cannot be hand-soldered.

Consequences:

- **+$25 setup per assembly side.** Everything here is top-side, so paid once.
- **Board is panelised to 140 x 80mm** — two 5mm breakaway rails added on the short
  dimension. The delivered board is still 140 x 70; the rails snap off. PCB is priced on the
  larger area, roughly +14%.

### The thing to check before ordering

**Every terminal block sits 0.55mm from the top or bottom edge** — measured, not estimated:

```
J_CH1, J_CH2, J_CH3          0.55 mm from the BOTTOM edge
J_PWR, J_IGN, J_AUX,
J_CAN1, J_CAN2               0.56 mm from the TOP edge
```

Those are exactly the edges the rails attach to. JLC normally wants several mm of component
clearance from a breakaway rail because de-panelling stresses that edge, V-scoring more than
mouse bites.

**Ask them to confirm the de-panelling method clears the terminals** — put it in the PCB
remarks rather than discovering it after the fact. `Confirm Production File` is already
enabled, so it will be reviewed either way.

In our favour: these are chunky THT plastic blocks rather than fragile SMD parts, and mouse
bites need far less clearance than V-scoring. But it is a genuine question and the answer
costs nothing to get.

If it ever comes back as a problem, the fix is to inset the terminal rows a few mm from both
edges — a layout change and a re-route, so worth knowing now rather than later.
