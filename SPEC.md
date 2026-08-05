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
| `R_PUn` | 100k, **DNP** | `CHx` → `+12V_P`. Fit only for keypad button channels |

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

- **South half**: the three identical 7-channel tiles, anchored at x = 7 / 51 / 95,
  terminals on the south edge at y = 65. Each tile is defined **once** as a group of 27
  members and instantiated three times — 81 of the 154 parts come from that one definition.
  To fit 70mm the driver, both shift registers and their decoupling sit in **one row**
  rather than a column: 31mm of tile height instead of 46mm, spending width the wider
  board now has.
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

### ⚠ Group members and refs are paired by POSITION

`expand_placement` pairs `members[i]` with `refs[i]`. Reordering the member offsets without
reordering the ref list silently moves parts to each other's slots — doing exactly that put
`U_SI` in `C_SO`'s position on all three tiles. It surfaced as a courtyard overlap, but only
because the two happened to collide; a reorder that lands parts somewhere legal would pass
every check and be wrong.

### Next: routing

Draw the copper tracks, pour the ground fill, then re-check clearances. Expect this to raise
at least one "these two things want the same space" question worth a decision.
