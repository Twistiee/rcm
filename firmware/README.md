# RCM firmware

STM32F446RET6, PlatformIO + STM32duino. Drives 21 universal channels behind shift
registers, talks CAN to a rusEFI bus, and diagnoses its own relay circuits.

**Status:** feature-complete for bench bring-up, **never run on hardware** — the boards
were ordered 2026-08-08 and have not arrived. Everything below is written, compiling, and
unit-tested against a model of the board. None of it has seen a relay.

```
pio run                     build for the board
pio run -t upload           flash over J_SWD with an ST-Link
pio test -e native          79 host unit tests
python tools/gen_dbc.py     regenerate ../docs/rcm.dbc
```

---

## What is here

| | |
|---|---|
| `include/board.h` | pin map, taken from `gen_spec.py`'s `PINMAP` — do not hand-edit |
| `src/shiftreg.cpp` | 3× 74HC595 out + 3× 74HC165 in, one shared SPI1 transfer |
| `src/channels.cpp` | channel modes, debounce, coil-circuit diagnosis |
| `src/canbus.cpp` | bxCAN on the HAL, bit timing solved for any bitrate |
| `src/protocol.cpp` | frame layouts, commands, failsafe, peer mirroring |
| `src/store.cpp` | M95640 EEPROM driver |
| `src/config.cpp` | straps from the DIP, configuration from EEPROM |
| `src/imu.cpp` | BMI270 → Bosch MM5.10 CAN frames |
| `src/main.cpp` | boot order, scheduler, ignition shutdown, watchdog |
| `lib/bmi270/` | Bosch's own BMI270 driver, vendored (BSD-3) |
| `test/` | host unit tests + a model of the board |

---

## Boot order is a safety property

Three things happen in this order and the order is not negotiable:

1. **`LATCH_HOLD` high**, as the first statement in `setup()`. Until it is, the board is
   alive only while the ignition signal is, and it cuts out mid-boot the moment you
   switch off. Nothing is debuggable before this works.
2. **A zeroed frame shifted out and latched**, with `SR_OE_N` still high. `R_OE` holds the
   595s in high-impedance from the instant power arrives; that is what stops 21 relays
   clacking on during boot. Until a known state is latched, dropping `SR_OE_N` would put
   power-on garbage onto the coils.
3. **Only then** may the outputs go live.

`test_shiftreg` asserts all three.

---

## Channel modes are software, and only software

Every channel has the same three resistors fitted, and every channel is simultaneously an
output and an input in hardware — the sense divider never disconnects. What a channel *is*
comes from `cfg.ch[n].mode`:

| Mode | Behaviour |
|---|---|
| `CH_OUTPUT` | drives a relay coil low-side; sense reports coil-circuit health |
| `CH_INPUT` | never driven; sense is a button switching +12V |
| `CH_UNUSED` | never driven, never diagnosed |

Commands to a channel that is not `CH_OUTPUT` are **ignored**. That is a safety property,
not tidiness: driving a channel wired to a button would short that button's +12V feed to
ground through the TPL7407L.

### What a sense bit means

| | Reads |
|---|---|
| Output, driver off, coil circuit intact | HIGH |
| Output, driver off, fuse blown / open circuit | LOW |
| Output, driver on | LOW |
| Input, button open | LOW |
| Input, button closed to +12V | HIGH |

So **an output can only be diagnosed while it is off.** That is not worth engineering
around — a relay that is commanded on and drawing current announces a broken circuit the
moment it is next switched off.

Two timers keep it honest. `output_settle_ms` (100ms) ignores a channel's sense after any
commanded change, because a coil's flyback takes real time to collapse and diagnosing
inside that window makes every switch-off look like a blown fuse. `fault_confirm_ms`
(500ms) then requires the condition to hold — a car is a terrible electrical environment
and one bad sample is noise.

---

## CAN

### Where the IDs came from

Checked against rusEFI's own source rather than guessed. What rusEFI occupies:

| | |
|---|---|
| `0x100`, `0x102` | TunerStudio over CAN |
| `0x130`, `0x131` | VAG yaw/lateral-G IMU input |
| `0x150`, `0x151` | Mercedes A0065422618 IMU input |
| `0x174`, `0x178`, `0x17C` | Bosch MM5.10 IMU input |
| `0x190` | rusEFI wideband O2 |
| `0x200`–`0x20B` | rusEFI verbose gauge broadcast |
| `0x667`, `0x7E1` | OpenBLT bootloader |
| `0x7DF`, `0x7E0`–`0x7E8` | OBD2 |
| `0x770000`+ | bench-test protocol (extended IDs — no clash with 11-bit) |

**`0x300` is clear of all of it**, so that is the default base. It is not baked in:
`can_base_id` lives in EEPROM and can be moved over the bus.

```
base + node*0x10 + f    f = 0..15, one 16-ID block per node
base + 0x80             global control, every node listens
```

Eight nodes (4 DIP addresses × 2 roles) fit in `0x300`–`0x380`.

### Frames

| Offset | Name | Direction |
|---|---|---|
| `+0` | `OUTPUTS` — commanded states, status flags, uptime, node id, seq | TX |
| `+1` | `INPUTS` — debounced inputs, aux, **raw sense for every channel** | TX |
| `+2` | `FAULTS` — open-circuit bits, short-to-12V bits | TX |
| `+3` | `STATUS` — firmware version, bus-off, ignition mV, CAN error counters | TX |
| `+8` | `CMD_SET` — 21-bit mask + 21-bit values | RX |
| `+9` | `CMD_CTL` — opcode + up to 4 argument bytes | RX |

`INPUTS` carries the raw sense bits alongside the logical ones deliberately. It costs
nothing and it is the most useful thing on the bus when something is miswired — you can
see what a pin is actually doing without having to agree with the firmware about what
that pin is *for*.

### uaDASH: use the DBC, don't imitate rusEFI

uaDASH renders any third-party broadcast it has a DBC for. So rather than squeezing 21
channels into a frame layout designed for RPM and coolant temp, the board broadcasts its
own clean frames and ships **`docs/rcm.dbc`** alongside — 52 messages, 1409 signals,
generated by `tools/gen_dbc.py` and validated with `cantools`.

**Keep them in step.** The generator reads the IDs straight out of `protocol.h` so those
cannot drift, but signal *layouts* are written in both places.

### The IMU is the exception, and the happy one

rusEFI already decodes Bosch MM5.10 accelerometer frames natively. Our IMU is a Bosch
BMI270. So the board emits MM5.10 at `0x174`/`0x178`/`0x17C`, and the ECU picks up yaw
rate and lateral/longitudinal/vertical G with **nothing to configure but
`imuType = IMU_MM5_10`** in TunerStudio.

Two honest caveats:

- We emit only the fields rusEFI reads (bytes 0–1 and 4–5). A real MM5.10 also puts a
  status nibble, a rolling counter and a CRC in the gaps. Anything that validates those
  will reject these frames.
- The MM5.10 encoding saturates at ±163.8 °/s of yaw. That is most of the way through a
  spin, and the encoders clamp rather than wrap — a wrapped yaw rate would tell the ECU
  the car had suddenly turned the other way.

### Bitrate

Solved for at runtime from the actual APB1 clock, targeting an 87.5% sample point, and
**exact rates only** — if no `(BRP, TS1, TS2)` divides out exactly, `can_begin()` fails
rather than settling for close. A 1% bitrate error works on the bench and fails in the
rain; a hard failure at boot is a much better outcome. (Note this rules out 800k on a
45MHz APB1, which is asserted at compile time.)

`CFG_BAUD` closed forces 500k regardless of what is stored. That is a *recovery*
mechanism, not a selector: you cannot fix a bad CAN setting over CAN, so one switch has to
beat any configuration.

### Peer mirroring

The shortest path from a keypad to a relay module without an ECU in the middle. Set
`peer_node` and a relay module applies that node's debounced input bits straight to its
own outputs, channel for channel. `peer_mask` bounds what it may touch;
`peer_toggle_mask` picks channels where a *press* toggles rather than follows — momentary
button, latching load.

The first frame from a peer only establishes a baseline, so a board joining the bus while
a button happens to be held does not fire every toggle channel at once.

---

## Configuration

**Straps** come from the DIP and can only change with the power off — role, node address,
IMU enable, and the 500k override. These are exactly the settings you cannot fix over the
bus once they are wrong, which is why they are on a switch.

**Config** lives in EEPROM and can be changed over CAN: bitrate, base ID, timings, the
channel mode table, failsafe states, peer mirroring, IMU axis remap.

Two CRC'd copies at `0x0000` and `0x0400`. The backup is written **first**, so a power
loss between the two writes always leaves one valid record. On boot, a bad primary with a
good backup is repaired immediately rather than rediscovered next time. A blank or
doubly-corrupt EEPROM gives defaults, and the defaults follow the role strap — a fresh
board already does the obviously right thing for what it is.

`RCM_OP_LOAD_DEFAULTS` only touches RAM. Someone has to send `SAVE_CONFIG` as a second,
deliberate act.

---

## Testing

79 host unit tests, run with `pio test -e native`. They compile the firmware's **own**
`.cpp` files against a model of the board in `test/stubs/`, so they test the code that
ships rather than a transcription of it.

`test/stubs/simboard.cpp` models:

- the 595/165 chains bit by bit, in both directions
- each channel's electrical behaviour, so a "blown fuse" means the coil cannot pull the
  node up rather than a mock asserting the answer
- the M95640 **including its page-wrap misbehaviour** — a write burst that runs off the
  end of a 32-byte page comes back round and overwrites what it just wrote

That last one matters: `store_write()` splitting at page boundaries is the only thing
between the config record and quiet corruption, and a model that did not reproduce the
misbehaviour would prove nothing.

### Two things the tests already caught

- **The 165 chain mapping.** The two chains run in opposite directions, and the first
  draft of the *model* copied the 595 formula. The driver and the model disagreed
  immediately. That is the whole point — two independent derivations from the netlist only
  agree if both are right, and this is a bug you cannot see by reading either side alone.
- **A reboot latch** in `proto_poll()` that was never cleared. Harmless on real hardware,
  where `NVIC_SystemReset()` does not return, but a reset loop anywhere it did.

### What is NOT covered

**`canbus.cpp` has no host test** — it needs the STM32 HAL and will not compile on a PC.
The bit-timing solver is `constexpr` and asserted at compile time for 125k/250k/500k/1M on
both 45MHz and 42MHz APB1, but the register setup, the filter banks and the transmit queue
are unexercised until real hardware arrives. Same for `imu.cpp` and the parts of
`main.cpp` that touch peripherals.

---

## Bring-up order, when boards arrive

Each step is verifiable with a multimeter before any bus is involved.

| # | Check | How |
|---|---|---|
| 1 | Board stays alive | LED1 blinks; board stays on with the ignition input pulled low |
| 2 | Node address | LED2 flashes N+1 times at boot |
| 3 | Channels | command one over CAN, meter the terminal |
| 4 | Sense | short a terminal to +12V, watch the raw sense bit in the `INPUTS` frame |
| 5 | **Bit order** | drive channel 1 and channel 21 and confirm it is *those* terminals |
| 6 | Fuse detection | pull a fuse, watch the `FAULTS` frame |
| 7 | IMU | close DIP 6, watch `0x174` |

Step 5 is worth doing explicitly even though the tests cover it. The model is only as good
as my reading of the netlist, and the netlist is only as good as the board.

---

## Toolchain notes

- **`-D HAL_CAN_MODULE_ENABLED` is required.** STM32duino leaves bxCAN out of the HAL
  build by default — it is in the "unused modules" list in
  `cores/arduino/stm32/stm32yyxx_hal_conf.h`. Without the flag `stm32f4xx_hal_can.c`
  compiles to nothing and `CAN_HandleTypeDef` does not exist.
- **Clean builds can hit a GCC internal compiler error.** `arm-none-eabi-g++ 12.3.1`
  segfaults intermittently while compiling the STM32duino core under parallel jobs. It is
  not our code and it is not deterministic: re-run the build, or use `pio run -j 1` (26s
  instead of 3s, but it has never failed).
- **Host tests need a host gcc on PATH.** MinGW-w64 (WinLibs) on Windows.
- `HSE_VALUE=8000000U` must match Y1. A wrong value silently breaks CAN timing.
