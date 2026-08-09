# RCM firmware

**Status: bring-up milestone 1.** Pin map complete, project skeleton in place.

## Approach: incremental, but not in the obvious order

The instinct is to get CAN working first. That's the wrong order here, because of one
hardware fact:

> **`LATCH_HOLD` must be driven high within the first few milliseconds of `main()`,
> or the board switches its own power off when the ignition signal drops.**

Nothing is debuggable until that works. So the bring-up order is driven by what makes the
*next* step testable, and each milestone is independently verifiable with a multimeter
before any bus is involved.

| # | Milestone | Proves | How you check it |
|---|---|---|---|
| **1** | Latch + LED + config read | power, clock, GPIO, the board stays alive | LED blinks; board stays on with ignition off |
| **2** | Shift register loop | all 21 channels, both directions | drive a channel, meter the terminal; short it, see the sense bit |
| **3** | CAN up at the configured rate | bus timing, the config path | ECU sees the node |
| **4** | Channel commands + state broadcast | the actual product | relay clicks from a CAN frame |
| **5** | Fuse detection | the diagnostic that comes free | pull a fuse, watch the bit change |
| **6** | IMU | the optional extra | orientation on the bus |

Milestones 1 and 2 need **no CAN at all**, which matters — you can bring the board up on a
bench supply before it ever sees the car.

## Safety rules that are not negotiable

1. **Assert `LATCH_HOLD` first.** Before clocks, before peripherals, before anything.
2. **Leave `SR_OE_N` high until the shift registers hold a known state.** It has a 10k
   pull-up so the 595 outputs are high-impedance from power-on. That is what stops every
   relay clacking on during boot — do not defeat it by enabling outputs early.
3. **Shift out a zeroed frame, latch it, *then* drop `SR_OE_N`.**

## CAN bitrate: 500k default, and configurable

uaDASH is limited to 500 kbps, so that is the default and almost certainly what you want.
But the rate is not hard-coded:

- **Stored in EEPROM** as an arbitrary value — 125k, 250k, 500k, 800k, 1M — so other
  installs are not stuck with our two.
- **`CFG_BAUD` closed forces 500 kbps**, ignoring the stored value.

That second part is deliberately a *recovery* mechanism rather than a selector. If you
configure a rate the bus doesn't run at, the node goes silent — and you cannot fix a bad
CAN setting over CAN. Flipping one DIP switch gets you back to a known-good 500k without a
programmer.

## rusEFI integration

Target bus: **uaEFI SUPER** ECU + **uaDASH** display.

The message IDs are **not decided yet and must not be invented.** rusEFI publishes its CAN
broadcast format; the right move is to read that and follow it, so uaDASH can render channel
state without custom display code. Two things to settle from their docs:

- Which base ID range is safe for a third-party node
- Whether their dash protocol has a generic channel/switch concept we can map onto, or
  whether we need our own IDs alongside

Until that's read, the CAN layer is written so IDs come from one header and can be changed
in a single place.

## Toolchain

**PlatformIO + the STM32duino core.** Arduino-simple where it doesn't matter, but with real
dependency management, and it drops to STM32 HAL where it does. Debugging over `J_SWD` with
an ST-Link works out of the box.

USB DFU (jumper `J_BOOT` 1-2) flashes without a programmer, but is flashing only -- no
breakpoints. Get an ST-Link for development.

## Layout

```
include/board.h    pin map, taken from gen_spec.py's PINMAP -- do not hand-edit
src/main.cpp       milestone 1
platformio.ini     env + upload config
```
