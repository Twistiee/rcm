# RCM — schematic specification

> Block-by-block spec for review **before** any KiCad work. Decisions and their rationale
> live in `DESIGN.md`; this file is the buildable description. Nothing here is drawn yet.

Target: ~100 × 80mm, **2 layers**, 1oz, HASL, PCBA (SMD) + hand-fitted THT terminals.
Passives **0805** throughout for hand-rework.

---

## Nets — top level

| Net | Source | Notes |
|---|---|---|
| `+12V_IN` | `J_PWR` | raw battery |
| `+12V_P` | after F1/D1/D2 | permanent, protected |
| `+12V_SW` | latch `U_LATCH` OUT | switched — feeds the buck only |
| `+5V` | buck module OUT | 5V; only needed if SIM7600 survives |
| `+3V3` | `U_LDO` | logic rail |
| `LATCH_IGN` / `LATCH_HOLD` / `LATCH_IN` | latch OR network | see block 2 |
| `CH1..CH21` | tile outputs | universal channel nodes |
| `SNS1..SNS21` | dividers | to `74HC165` |

---

## Block 1 — Power input and protection

| Ref | Part | Notes |
|---|---|---|
| `J_PWR` | KF2EDGVM-3.5-**2P** | `+12V_IN`, `GND` |
| `F1` | PPTC 1A hold, 1812 | board draws ~150–250mA; 1A is ample |
| `D1` | SS34 (SMA) | series reverse-polarity. 0.4V drop × 0.2A = 80mW, fine |
| `D2` | SMAJ33A (SMA) | load-dump clamp, `+12V_P` → GND |

## Block 2 — Ignition latch (lifted verbatim from revB `U19`)

Proven circuit — **do not re-derive**. It is a *resistive* OR into `IN`, not a diode-OR.

| Ref | Part | Connection |
|---|---|---|
| `U_LATCH` | **BTS7040-1EPA** (TSDSO-14) | VS(15)←`+12V_P`; OUT(8,9,10,12,13,14)→`+12V_SW`; GND(1)→`R_LG`→GND; DEN(3)→GND; **IS(4) open**; IN(2)=`LATCH_IN` |
| `R_LG` | 47R | the RGND network — **required**, ReverseON needs a resistive GND path |
| `R_LPD` | 22k | `LATCH_IN`→GND. Default **off** |
| `R_LIGN` | 47k | `LATCH_IGN`→`LATCH_IN` |
| `R_LHOLD` | 1k | `LATCH_HOLD`(MCU GPIO)→`LATCH_IN` |
| `C_LVS` / `C_LSW` | 100nF ea | VS decoupling / output snubber |

Ignition 12V → `IN` ≈ 3.8V (4.6V at 14.4V charging). MCU 3.3V → `IN` ≈ 3.2V. Either turns
it on alone; both absent, `R_LPD` holds it off.

Run as a **dumb switch**: DEN low, IS open — no RS/RP/DZ/CS chain.

⚠ `LATCH_HOLD` **must be a direct MCU GPIO, never a shift-register output** — the registers
are in an undefined state at power-on and gating your own supply through them is a
bootstrapping hazard.

`J_IGN` (KF2EDGVM-3.5-**2P**: `IGN`, spare) feeds `LATCH_IGN`, plus a 1M/220k divider to an
MCU input so firmware can sense ignition state.

## Block 3 — Buck + 3V3

`U_BUCK` = **Waveshare DC5-36-TO-DC3V3-5**, socketed on header pins (geometry in
`../pdm/pdm14-revB/DESIGN.md`, footprints in `../pdm/pdm14-revB/footprints/pdm14.pretty/`):
IN column 4 holes @ 3.50mm (`+12V_SW`/GND), OUT column 6 holes @ 2.54mm (top three `+5V`,
bottom three GND). **Ships set to 5V — no pad rework.**

`U_LDO` = LD1117S33 (SOT-223), `+5V`→`+3V3`.

> **Simplification path if SIM7600 is deleted:** nothing else needs 5V. Set the Waveshare to
> 3V3 and **delete the LDO entirely**. Until then the LDO drops 1.7V × ~0.3A ≈ 0.5W in
> SOT-223, which needs a reasonable copper pad even on a 2-layer board.

## Block 4 — MCU

`U_MCU` = **STM32F446RET6**, LQFP-64 10×10mm.

⚠ **Verify against DS10693 before drawing:** that LQFP-64 exposes USB OTG FS, CAN *and*
SDIO on non-conflicting pins simultaneously. Confirmed present on the die (CMSIS header);
the 64-pin package may not break all three out. Fallback: **F446VET6**, LQFP-100 14×14mm.

- Decoupling: 1× 4.7µF + 1× 100nF per VDD pair; VCAP 2.2µF ×2 (F4 requirement)
- `Y1` 8MHz HSE + 2× 20pF — matches joesbox's stock `RCC_HSE_ON`, PLLM=8
- `Y2` 32.768kHz LSE + 2× 12pF — RTC. **No backup cell** (`VBAT`→`+3V3`), time is lost each
  park; see the logging consequences in the pdm review
- BOOT0 10k pulldown + jumper; NRST 100nF; reset tactile switch
- `J_SWD` 1×05 (3V3, SWDIO, SWCLK, NRST, GND)
- 2× status LED + resistors

## Block 5 — CAN

| Ref | Part |
|---|---|
| `U_CAN` | SN65HVD230 (SOIC-8) |
| `D_CAN` | SZNUP2105L (SOT-23) ESD |
| `R_TERM` / `R_TJ` | **120R populated + 0R DNP in series** — project convention, do not invert |
| `J_CAN1`,`J_CAN2` | KF2EDGVM-3.5-**3P** each: `CANH`, `CANL`, **`GND`** |

GND on the CAN connector is **required** — transceiver common-mode range depends on nodes
sharing a ground reference. It is a signal reference, not power distribution.

## Block 6 — IMU

`U_IMU` = **BMI270** (LGA-14), I2C, address-select 0R jumper, 100nF + 100nF decoupling.
Non-negotiable per user.

## Block 7 — Shift-register chains

| Ref | Part | Role |
|---|---|---|
| `U_SO1..3` | 74HC595 (SOP-16) | 24 outputs, 21 used → TPL7407L inputs |
| `U_SI1..3` | 74HC165 (SOP-16) | 24 inputs — 21 channels + 3 dedicated |

Shared bus: `SCK`, `MOSI`→595, `MISO`←165, `RCLK` (595 latch), `PL` (165 load), `OE_N`.
One 100nF per chip.

⚠ **Boot-state hazard — resolve before layout.** At power-on the 595 output latches are
random, so relays could clack on. Plan: `OE_N` pulled **high** (outputs Hi-Z) by a resistor,
MCU clocks in zeros, pulses `RCLK`, then drives `OE_N` low. **This only works if the
TPL7407L inputs are defined while the 595 outputs are Hi-Z — verify whether TPL7407L has
internal input pulldowns.** If it does not, either add 21 pulldowns or drive `OE_N`/`SRCLR_N`
from a power-on RC. Do not leave this to chance.

## Block 8 — Channel tiles ×3 (7 channels each)

Three **identical** tiles; lay out one and replicate.

Per tile:

| Ref | Part | Notes |
|---|---|---|
| `U_DRVn` | **TPL7407L** (TSSOP-16) | 7ch low-side sink. IN1-7 ← 595; OUT1-7 → `CHx`; **COM → `+12V_P`** (integral flyback clamp); GND |
| `J_CHn` | KF2EDGVM-3.5-**7P** | 7 channel terminals |

Per channel (×7 per tile, ×21 total):

| Ref | Value | Function |
|---|---|---|
| `R_SHn` | **1M** | `CHx` → `SNSx` |
| `R_SLn` | **220k** | `SNSx` → GND (also the pulldown that defines 0V on a dead feed) |
| `R_PUn` | 100k, **DNP** | `CHx` → `+12V_P`. **Fit only for keypad button channels** |
| `C_Fn` | 100nF, DNP-able | optional input filter |

Divider check: 14.4V → `SNS` 2.60V; 12V → 2.16V. Both comfortably above the 74HC threshold
(~1.65V at 3V3 Vcc). Draw is 12µA/channel, ~250µA over 21.

⚠ That 250µA flows **from the fuse box through each relay coil**, not from our latched rail
— so it is *not* killed by the ignition latch. Harmless (a coil needs ~150mA to pull in) but
it is permanent parked drain and belongs in the budget.

## Block 9 — Optional blocks (put in a physically separable corner)

Design in now, delete later if space demands — deletion must not ripple through the
floorplan.

| Block | Parts | Priority |
|---|---|---|
| USB-C | HRO TYPE-C-31-M-12, 2× 5.1k CC, ESD | keep |
| EEPROM | 25LC640 (SOIC-8), SPI | keep |
| ESP32-C3 co-proc | ESP32-C3-MINI-1-N4 + UART + strapping | optional |
| microSD | DM3AT-SF-PEJM5, SDIO | droppable |
| SIM7600 header | 6P + `Q1`/`Q2` 5V switch | droppable — **its removal also deletes the LDO** |

---

## Connector summary

| Ref | Poles | Edge |
|---|---:|---|
| `J_CH1..3` | 7 each = **21** | South |
| `J_PWR` | 2 | West |
| `J_IGN` | 2 | West |
| `J_CAN1`,`J_CAN2` | 3 + 3 | East |
| `J_AUX` | 3 dedicated inputs | East |
| | **34 positions** ≈ 119mm | |

## Unique-part budget

At qty 1 the per-unique-extended-part fee is a fixed cost, so this is a real target.
Estimate ~45–50 distinct part numbers against revB's 59.

**Consolidate aggressively:** resistors should land on ~8 values (0R, 47R, 120R, 1k, 10k,
22k/47k, 100k, 220k, 1M) and caps on ~6 (20pF, 12pF, 100nF, 2.2µF, 4.7µF, 10µF). Use one
terminal family (KF2EDG) in as few pole counts as the layout allows.
