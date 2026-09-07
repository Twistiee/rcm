# RCM firmware

STM32F446RET6, PlatformIO + STM32duino. Drives 21 universal channels behind shift
registers, talks CAN to a rusEFI bus, and diagnoses its own relay circuits.

**Status:** feature-complete for bench bring-up, **never run on hardware** — the boards
were ordered 2026-08-08 and have not arrived. Everything below is written, compiling, and
unit-tested against a model of the board. None of it has seen a relay.

```
pio run                          build for the board
pio run -t upload                flash over J_SWD with an ST-Link
pio run -e selftest -t upload    bring-up console on the USB-C port
pio test -e native               237 host unit tests
python tools/gen_dbc.py          regenerate ../docs/rcm.dbc
python tools/test_rcm_bench.py   self-test the bench tool, no hardware needed
python tools/rcm_bench.py --help talk to a board over CAN
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
| `src/selftest_main.cpp` | bring-up console over USB CDC, its own build environment |
| `lib/bmi270/` | Bosch's own BMI270 driver, vendored (BSD-3) |
| `test/` | host unit tests + a model of the board |
| `tools/rcm_bench.py` | PC-side CAN tool: monitor, command, channel walk |
| `tools/gen_dbc.py` | generates `docs/rcm.dbc` from `protocol.h` |

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

### Which way up the board is mounted

The IMU does not have to face any particular direction, but the board has to be **told**
which way it is facing, because a wrong axis map reports confident, plausible, wrong
motion to an ECU that may act on it. `cfg.imu_map` names, for each vehicle axis, the
sensor axis that feeds it and whether to negate it. Vehicle axes are the automotive
convention: **X forward, Y left, Z up**.

```
rcm_bench.py ctl imumap x y z       # board flat, +X edge pointing down the car
rcm_bench.py ctl imumap ny x z      # the same board rotated 90 deg clockwise
rcm_bench.py ctl imumap x ny nz     # board mounted upside down
rcm_bench.py ctl imulevel           # or solve it from gravity, standing still
rcm_bench.py get imu                # read back what it settled on
rcm_bench.py ctl save
```

Write a negated axis as `ny`, not `-y` — a leading dash is taken for a command-line
option before the tool ever sees it.

**Auto-level solves one axis, not three.** Standing still, the only acceleration is
gravity, so the reading fixes which way is **up** and nothing else — spin the board on a
level bench and gravity never changes. Forward and left are not guessable this way, so
they are filled in as a right-handed pair and left for you to confirm on a drive: braking
should read **negative vehicle X**. Handedness is preserved rather than left to chance
because a left-handed map reads perfectly plausibly at a standstill and only shows up as
the car yawing the wrong way in a corner.

**It refuses more often than it succeeds, on purpose.** `imu_solve_level()` returns
`MOVING` if the vector is not ~1 g (engine running, someone leaning on the car), `MOVING`
again if the **gyro** shows more than 2 °/s, and `TILTED` if no axis is within 15° of
vertical.

The gyro check is there because the accelerometer alone cannot see rotation, and that
was a real bug caught on the bench rather than a theoretical one. Magnitude only responds
to *linear* acceleration: turn the board slowly and gravity still totals exactly 1 g, it
just points somewhere else. Spinning the board flat on a desk measured `|a|` = 0.991,
1.005 and 0.995 g — perfect, square, and completely wrong — while the gyro read 46–68 °/s.
The first version solved happily all three times. Sitting still the same board reads
0.17 °/s, so 2 °/s sits an order of magnitude above the noise and an order below anything
a person does. That second one is the important refusal:
an axis map can only ever describe a **square** mount, so a board on a raked dash cannot
be corrected by any map at all. Rounding it to the nearest axis would bake a component of
gravity into the forward reading as a permanent phantom acceleration. If you need the
board at an angle, that needs real angle correction, which this firmware does not have.

All of this is hardware-verified over both transports (2026-09-07): solving flat and on
its side, refusing a 34 deg tilt, refusing a flat spin, setting and reading a map over
CAN, and refusing a repeated axis, a fourth axis, a short frame and a missing magic byte
sent as raw frames — so the guards are the firmware's, not just the tool's.

Mount it **rigidly** either way — on a compliant bracket you measure the bracket
resonating, not the car.

### What the status LEDs mean

**Green is SOLID when all is well.** It only flashes to tell you something, so a glance
separates "fine" from "look at me" without counting blink rates.

| Green | Meaning |
|---|---|
| **Solid** | Running normally |
| 1 Hz | Was being commanded over CAN, and the master went away |
| 6.7 Hz | The CAN controller never started at all — wrong bitrate, or worse |

**Red is off unless an output that has a job is faulty.**

Both rules exist because the first version cried wolf, and an indicator that is always on
tells you nothing:

- Green went frantic whenever the bus was quiet. But **a keypad is never commanded** — it
  *reports* button presses — so a quiet bus is its normal condition and it flashed fast
  forever. `proto_ever_addressed()` is what separates "nobody has ever commanded this
  board" from "this board lost its master", and only the second is a fault. Note this is
  independent of the CAN timeout: the failsafe still fires and is still reported over the
  bus, the LED just stops treating a keypad's normal life as an emergency.
- Red lit for an open circuit on **any** channel, including the eighteen nobody has wired
  yet, so every board on a bench sat with a fault lamp on.

The fault rule lives in **one** function, `ch_fault_actionable()`, used by both the LED and
the `RCM_ST_ANY_FAULT` status flag, with a test asserting the two agree — a dark lamp while
`rcm_bench scan` reports `FAULT` is exactly the sort of contradiction that costs an
evening. The per-channel `FAULTS` frame is **not** filtered: that detail is what you want
while diagnosing, and only the summary is a lamp.

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
`peer_toggle_mask` picks channels where a *press* toggles rather than follows.

Prefer setting the **input behaviour** to `IN_TOGGLE` on the keypad instead, where
available: the latched state then travels in the keypad's own INPUTS frame, so mirroring
just follows it, a dash sees the real state, and any ECU command bound to that button gets
one edge per intent. `peer_toggle_mask` un-picks it at the destination, which only ever
works for the one consumer that implements it.

The first frame from a peer only establishes a baseline, so a board joining the bus while
a button happens to be held does not fire every toggle channel at once.

---

## Configuration

**Straps** come from the DIP and can only change with the power off — role, node address,
IMU enable, and the 500k override. These are exactly the settings you cannot fix over the
bus once they are wrong, which is why they are on a switch.

**Everything else lives in EEPROM and is settable over CAN** — bitrate, base ID, timings,
channel modes and behaviours, failsafe states, peer mirroring, IMU axis remap, the ECU
command and follow tables.

**And readable back over CAN.** `RCM_OP_GET_CFG` takes a selector and an index and answers
on its own frame. That is not a convenience: without it every check has to be behavioural,
which is how a bench session ends up concluding something from a stale broadcast frame.
`rcm_bench get` dumps the lot.

### A channel's role is its label, and nothing else

The ignition machinery needs to find four channels: the start pedal, the starter, the
engine-running signal, and the RUN output that feeds an ECU's ignition input. Those are
named by the channel's **function label** and nowhere else.

There used to be a second copy — `ign_brake_ch` and friends — with nothing tying the two
together. Label channel 12 "Brake pedal", point `ign_brake_ch` at channel 5, and the board
reads the brake from 5 while every display says 12. Pressing start with a foot on the
brake then does nothing, the configuration looks correct, and you go and suspect the
switch. One record of a role, so nothing can disagree with it.

### Which pedal starts the car

**The clutch if a channel is labelled `FN_IN_CLUTCH`, the brake otherwise.** There is no
setting for this — the label *is* the setting, for exactly the reason above.

A manual starts on the clutch: that is the interlock that proves the engine is not about
to drive the wheels, and it is what stops a car being cranked in gear. An automatic has no
clutch and starts on the brake. So labelling the pedal you actually wired chooses the
behaviour, and when **both** are labelled — the normal case, since the brake is wanted
anyway for brake lights and the ECU — the clutch wins and the brake takes no part in
ignition at all.

The pedal does not merely gate cranking, it decides what a press *means*: pedal down is
always an attempt to start, pedal up is always "switch the car off". So swapping pedals
swaps both halves together, which is why they are one lookup rather than two settings.

`GET_CFG`'s ignition selector reports the pedal that **actually gates**, not the brake —
a read-back naming a channel with no say in starting would be a confident wrong answer.
`rcm_bench get ignition` prints it as `start pedal`.

Everything the brake-only version documented about cranking still applies unchanged: the
pedal gates the *start*, not the continuation, because a starter drags the rail to 9–10 V
and a 10.87 V digital input cannot be read through that. Aborting on "pedal released"
would abort every start the instant the starter loaded the battery.

### Storage

Two CRC'd copies at `0x0000` and `0x0400`. The backup is written **first**, so a power
loss between the two writes always leaves one valid record. On boot, a bad primary with a
good backup is repaired immediately rather than rediscovered next time. A blank or
doubly-corrupt EEPROM gives defaults, and the defaults follow the role strap — a fresh
board already does the obviously right thing for what it is.

`RCM_OP_LOAD_DEFAULTS` only touches RAM. Someone has to send `SAVE_CONFIG` as a second,
deliberate act.

---

## Talking to a rusEFI ECU

Two directions, both optional, both configured by table so neither side needs new firmware
when rusEFI adds something.

**The ECU decides, this board switches.** rusEFI broadcasts the state it wants each relay
in — `MainRelayAct`, `FuelPumpAct`, `Fan`, `Fan2`, `EGOHeatAct`, `CELAct`, `RevLimAct` —
as single bits in one frame, re-asserted every broadcast period rather than sent on
change. Bind a bit to a channel and the channel follows it. rusEFI has far fewer outputs
than this board has channels, so this is how the split usually wants to go.

Two details that matter:

- a followed channel whose frame stops arriving **falls back to its failsafe bit**.
  Holding the last value would leave a fuel pump running on the strength of a frame that
  arrived before the ECU died.
- that staleness timeout is **separate from the bus timeout**, because rusEFI multiplies
  its own broadcast period by **five** while TunerStudio is connected over CAN. Sharing
  one value would drop fans and pumps while tuning, and read as a fault in this board.
  Allow at least 5× the ECU's configured period.

**A button asks the ECU to act.** Any input channel can be bound to a TunerStudio command,
sent as an extended frame. Start/stop the engine lands on the same entry point as rusEFI's
own physical start button, so the ECU keeps its own interlocks and decides whether that
means crank or stop. `LUA_COMMAND_1`..`4` bump counters a rusEFI Lua script can watch,
which is how traction control, launch control or a map switch get done — rusEFI has no
fixed command for any of them.

Commands are stored as a raw (subsystem, index) pair rather than a list of names: every
TunerStudio command is that shape, so a new one is a config change rather than a release.
The friendly names live in `tools/rcm_bench.py`, where they cost nothing.

---

## Two ways to bring a board up

### `pio run -e selftest -t upload` — needs nothing but the USB-C cable

An interactive console on the USB-C port. This exists because the first hour with a new
board is exactly when you least want a CAN adapter as a second unknown. It can prove, with
no other hardware at all:

- the board holds its own power on, and the DIP reads
- the EEPROM answers and survives a 40-byte write **across a page boundary**
- the CAN controller, its bit timing and its filter banks, via **internal loopback** —
  a lone CAN node normally cannot transmit at all, because nothing is there to ACK it
- the IMU initialises and reports about 1 g of gravity whichever way up the board is
- every channel drives, and what its sense line reads back

`w` walks all 21 channels with a 2 s dwell, which is bring-up step 5 done for you.
`W` deliberately hangs so the watchdog fires, then reports how long the outputs were
down coming back — the number the top-level README quotes for a reset dropout, measured
rather than estimated.

The one thing it cannot prove is that the transceiver reaches another node.

### `tools/rcm_bench.py` — over CAN, needs an adapter

**What an adapter is:** a PC has no CAN port. A USB-CAN adapter is a dongle with USB on
one end and CANH/CANL on the other, letting the PC join the bus as another node.

**Get one with `slcan` firmware.** It enumerates as an ordinary virtual COM port on
Windows — no driver wrangling — and it is what this tool defaults to. A
[CANable 2.0](https://canable.io/) ships that way, and so does the
[WeAct Studio USB2CANFD](https://github.com/WeActStudio/WeActStudio.USB2CANFDV1)
(STM32G0B1, CAN-FD, works with Cangaroo — this is the one in use here, about NZ$17 on
AliExpress).

The `candleLight`/`gs_usb` firmware is also fine and slightly faster, but on Windows it
needs `pip install "python-can[gs-usb]"` and a WinUSB driver swapped in with Zadig, and
then the tool wants `-i gs_usb -c 0` — a device index rather than a port name.

**Avoid the generic blue/black "USB-CAN Analyzer" dongles** (Waveshare USB-CAN-A and the
many AliExpress lookalikes). They use a vendor-specific serial protocol, not slcan;
python-can only reaches some of them through its `seeedstudio` interface and it is a
fight. Buying on price here costs an evening.

**Wiring and termination.** CANH and CANL to `J_CAN1` or `J_CAN2`, and tie the grounds
together. A CAN bus wants exactly **two** 120Ω terminators, one at each physical end:

- *bench, adapter plus board only* — enable both. `SW_CFG` position 1 on the board, and
  the adapter's own termination jumper.
- *on the car's bus* — the ECU and dash are already terminated. Turn both of yours off,
  or you will load the bus down to four terminators and errors will start appearing.

```
rcm_bench.py -i slcan -c COM5 scan          find nodes
rcm_bench.py -i slcan -c COM5 monitor       live state, sense and faults
rcm_bench.py -i slcan -c COM5 set 3 on
rcm_bench.py -i slcan -c COM5 walk          bit-order check
rcm_bench.py -i slcan -c COM5 faults
```

Any python-can interface works (slcan, socketcan, pcan, kvaser). Decoding goes through
`docs/rcm.dbc`, so the tool and the dash see identical fields.

`--with-sim` runs a fake board in a background thread, which is how the tool was
developed and tested with no hardware. `tools/test_rcm_bench.py` drives that end to end
and also cross-checks the tool's byte packing against the DBC — so bench tool, DBC and
`protocol.h` are pinned to each other.

---

## Testing

237 host unit tests, run with `pio test -e native`. They compile the firmware's **own**
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

`canbus.cpp` gets the same treatment through a small bxCAN shim in
`test/stubs/stm32_hal_shim.*` — three transmit mailboxes with controllable availability,
a receive FIFO, and filter banks indexed the way the hardware indexes them.

### What the tests have caught so far

- **The 165 chain mapping.** The two chains run in opposite directions, and the first
  draft of the *model* copied the 595 formula. The driver and the model disagreed
  immediately. That is the whole point — two independent derivations from the netlist only
  agree if both are right, and this is a bug you cannot see by reading either side alone.
- **A surviving accept-all CAN filter.** `can_begin()` parks an accept-all in bank 0 so a
  caller that installs nothing is noisy rather than deaf — but banks are ORed, so it made
  every later filter decorative and left a 3-deep FIFO exposed to the whole bus. The first
  real filter now overwrites bank 0. Found only because the shim indexes banks by number
  rather than appending them.
- **Four broadcast frames into three mailboxes.** STATUS was being dropped every cycle,
  silently and always the same frame. Hence the transmit queue.
- **A reboot latch** in `proto_poll()` that was never cleared. Harmless on real hardware,
  where `NVIC_SystemReset()` does not return, but a reset loop anywhere it did.
- **An auto-level that could not see rotation.** The first version gated only on the
  accelerometer reading ~1 g, which is true of a board being slowly turned over — gravity
  does not change magnitude when you rotate it. Spun flat on a desk it read a perfect
  0.991 g and solved a confident, wrong map. Only found by turning a real board while the
  command ran; every host test had fed it accelerometer vectors alone. It now gates on the
  gyro too, and `test_imu_level` carries the measured numbers from that session.
- **Stale receive frames** in the bench tool — read-back after a command returned frames
  captured *before* it, so every command looked like it had done nothing. True of a real
  slcan adapter as much as of the simulator.

### What is NOT covered

- **`imu.cpp`** — the BMI270 needs an 8KB config upload over I2C to a real chip. Only the
  MM5.10 encoding is checked, via the DBC. The axis solving is split into `imu_level.cpp`
  precisely so it *can* be tested without the chip: `test_imu_level` walks all six square
  mountings, feeds each the gravity vector it would really produce, and checks the map
  that comes back puts Z upright and keeps the axes right-handed.
- **The peripherals themselves.** The HAL shim proves this firmware drives bxCAN the way
  ST document it, not that the silicon then behaves. The self-test build's loopback check
  is what covers that, on hardware.
- **`main.cpp`'s scheduler and shutdown path.**

---

## Bring-up order, when boards arrive

Each step is verifiable with a multimeter before any bus is involved.

### Before you apply power at all

**The board will look completely dead with only `J_PWR` connected.** `+12V_SW` — the
buck's input, and therefore the whole board's supply — is the BTS7040 latch's *output*,
and the latch is held on by `J_IGN`. No ignition signal, no rail, no LEDs, nothing. This
is by design and it is the first thing that will make you think the board is faulty.

**USB cannot power it either.** `USB_VBUS` goes to the connector and stops there; it is
not connected to any rail. USB is for data only.

Three checks with the power off, meter on diode range:

| Check | Expect |
|---|---|
| `J_PWR` pin 1 → pin 2 (12V to GND), both polarities | no short. A few hundred kΩ or open |
| `J_PWR` pin 1 → `C_BULK` +12V pad, red lead on `J_PWR` | ~0.3–0.4 V forward, open reversed |
| 3V3 to GND | no short |

The middle one is worth doing properly: it tests `F1` and `D1` together, and confirms
`D1` is round the right way. That diode was wired backwards in the design once and caught
before the order — `Diode:SS34` has **pin 1 = cathode**, which is not the convention most
people assume. Backwards, it blocks the battery feed and the board never powers up.

### First power-up, staged

Use a bench supply with the **current limit set to about 200 mA**. The board draws
roughly 100 mA. If an assembly short exists, the limit is what stops it becoming a
replacement board.

1. **Buck NOT fitted.** 12 V to `J_PWR`, then 12 V to `J_IGN`. Measure `JB1` pin 1: it
   should sit at about **11.5 V** (12 V less the SS34 drop). Remove `J_IGN` and it should
   collapse. That single test proves the fuse, `D1`, the latch and its divider, with
   nothing downstream at risk.
2. **Fit the buck.** Check it is still set to **5 V out** — it ships that way and must
   stay that way. Confirm 5 V at `C_5V`.
3. **Confirm 3V3** at `C_3V3O`.
4. With a blank MCU the board should stay on only while `J_IGN` is held, because nothing
   is driving `LATCH_HOLD` yet. That is the correct behaviour and a useful signal that
   the latch is doing its job.
5. Only then flash the selftest build.

Nothing connected to any channel terminal for any of this — bare board and buck.

Flash the **selftest** build first — steps 1 to 6 need only the USB cable.

| # | Check | How |
|---|---|---|
| 1 | Board stays alive | LED1 solid (or blinking, see below); board stays on with the ignition input pulled low |
| 2 | Node address | LED2 flashes N+1 times at boot; then `d` in the console |
| 3 | EEPROM | `e` |
| 4 | CAN controller | `c` — internal loopback, no other node needed |
| 5 | IMU | `i` — should read about 1 g total, sitting still; `L` to auto-level |
| 6 | **Bit order** | `w` — walks all 21 channels, meter each terminal as it goes |
| 7 | Fuse detection | wire one relay, pull its fuse, `s` |
| 8 | On the real bus | flash the normal build, then `rcm_bench.py scan` |

Step 6 is worth doing by hand even though the tests cover it. The tests verify the
firmware against **my reading of the netlist**; this verifies the netlist against the
board you are holding. If a terminal lights up that is not the one named, look for an
offset of 14 — that is a mirrored shift-register byte order.

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
