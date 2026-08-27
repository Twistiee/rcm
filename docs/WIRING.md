# Wiring an Exocet with two RCM boards

Road-registered, all-LED lighting, **sealed rear clusters with a shared ground per
cluster**. Those three facts decide almost everything below, so re-read this section
before changing anything.

## The constraint that decides the architecture

**Every channel is low-side.** The load takes a permanent +12V feed and the board sinks
its ground. Two consequences, and they are not firmware-fixable:

1. **Anything driven directly needs its ground routed back to the board.** That is an
   extra thin wire per function, into the cabin.
2. **A shared-ground cluster cannot be switched per function.** Breaking the shared
   ground kills tail, indicator and brake together. The only way to switch those
   individually is high-side — a relay, or a solid-state high-side switch.

The second point is why this car's rear lighting still needs relays. It is a property of
the lamps, not of the board.

**The board is also a 600 mA device in the cabin.** It drives coils, not loads. Anything
above roughly half an amp stays a relay out at the fuse panel rather than dragging heavy
cable in and back out again.

## Relay count

| Circuit | Why it must be a relay |
|---|---|
| Main (ECU feed, injectors, coils) | current |
| Fuel pump | current |
| Fan | current |
| Starter | current; driven by the ECU, not this board |
| Headlight low | current |
| Headlight high | current |
| Horn | current |
| Tail / park | shared cluster ground — high-side only |

**Eight.** Add a ninth for a rear fog if one is fitted.

### Who drives which coil

**The ECU owns main, starter and fuel pump.** rusEFI has the sensor data those three
depend on and does not need a bus to act, so they are wired to its outputs directly.
The relay module drives the remaining five: fan, tail, both headlight circuits and the
horn.

The consequence is worth stating plainly: **fuelling and ignition do not depend on this
board's outputs, or on CAN, at all.** The relay module decides whether the car is on. It
has no say in whether the engine keeps running once it is.

### Telling the ECU the ignition is on

With a momentary start button there is no key-on wire, so something has to give the ECU a
steady "ignition on". **Feed it from `+12V_SW`, not from a channel.**

A channel is the obvious choice and it is the wrong one. Every channel goes
high-impedance on any reset and comes back about 22 ms later, and an ECU reading that as
key-off mid-drive is exactly the failure this architecture exists to avoid. `+12V_SW` is
the BTS7040's output rather than a shift-register output, and it does not drop: a reset
was measured on revA with the ignition released and the board came straight back, because
the rail capacitance bridges the few milliseconds before firmware re-asserts
`LATCH_HOLD`.

It needs no fuse. The rail comes off the BTS7040, a smart high-side switch, so it is
already e-fused -- current limit, short-circuit survival and thermal shutdown are in the
part. The budget is the switch's **4.5 A**, less the board's own draw, and today that
rail feeds nothing but the buck.

### VIGN is a power input, and it is diode-isolated

Checked against the **super-uaEFI rev b schematic**, power sheet (`Module-power_12and5V`).

`IN_VIGN` (the vehicle pin) goes through **D417, a B5819W Schottky**, onto the same
internal rail that `V12_RAW` reaches through **D416**, another B5819W. That rail runs
into the input filter and then straight into the **LMR14020 buck**. So the two supplies
are **diode-OR'd into the ECU's main converter**: key-on power really can run the whole
ECU, which matches rusEFI's general wiring guidance.

Two consequences, and they point in opposite directions:

**Size it as a supply, not a signal.** Worst case is the buck's whole draw -- order of an
amp, less whenever the permanent feed is also present and sharing. This is not a pin to
hang off `JB1`'s 0.1 inch header; it wants the `+12V_SW` terminal revision B should add.

**Nothing can flow back out of it.** D417's cathode is on the internal rail and its anode
on the pin, so current enters the ECU and cannot leave. **No blocking diode is needed in
the feed from `+12V_SW`** -- the ECU already has one, and a second would only cost another
few hundred millivolts.

The voltage the ECU *reads* is a separate path: `IN_VIGN` also feeds an R411 33k / R413
6.8k divider with a 100n cap and 1N4148 clamps to `VBAT`, and that node -- confusingly
also called `VIGN` -- is the ADC input on **PA5**. On its own that divider plus the
indicator LED draws well under a milliamp; the current that matters is the buck's.

Worth noting for anyone reading rusEFI's guide and worrying about the main relay feeding
back into VIGN: on this board `+12V_FROM_MAIN_RELAY` and `+12V_FROM_KEY` are separate
nets, bridged only by **R9, which is marked DNP**. They are not joined unless somebody
fits that resistor.

Schematic revisions differ -- the same repository carries v5 and v9 mechanical files -- so
re-check against the revision actually in the car before relying on this.

Because the ECU owns the fuel pump, there is no reason to set a `failsafe_state` bit for
priming — the ECU primes on its own ignition input.

Two that look like they need one and do not:

- **Reverse light** — the gearbox switch already switches +12V. Leave it hardwired. The
  board gains nothing by being in that circuit.
- **Brake lights** — hardwired, deliberately. A mechanical switch is simpler and more
  reliable than anything running on a microcontroller, and there is no upside to
  inserting one. Tap the switched feed into a spare input if the board should *know*
  about the brake (the ignition state machine needs this), but keep it out of the lamp
  circuit.

### Indicators and hazards: keep the flasher

Use a conventional flasher can, indicator stalk and hazard switch, entirely outside both
boards. Three reasons:

1. **The clusters are shared-ground**, so the board would need a relay per side anyway.
2. **A relay flashed continuously wears out.** At roughly 1.5 Hz, an hour on hazards is
   about 5000 operations. Relays are rated in the hundreds of thousands of cycles, so it
   is not immediate — but it is pointless wear for no gain.
3. The same argument as the brake lights: it already works, standalone, without power,
   firmware or a bus.

**This is a real loss.** `OUT_FLASH` exists precisely so several channels flash on one
shared period and stay in phase, which is what makes hazards look right instead of like
two indicators that happen to be on. With separately-grounded LED clusters, that feature
would have removed the flasher can *and* both relays, and given per-channel open-circuit
detection instead of bulb-failure fast-flash. Sealed clusters cost you all of that.

If a future set of rear lamps can be grounded per function, revisit this.

## What can be driven directly

Only loads with their own ground, under ~600 mA:

- rain light, if it is a separate unit rather than part of a cluster
- shift light, warning buzzers
- boost solenoid, line lock
- anything you add later with a dedicated earth

Everything with a shared ground is a relay, whatever its current.

## Channel budget

**Relay module** (node 0-3, channels default to outputs):

| Ch | Function | Notes |
|---|---|---|
| 1 | `FN_FAN_1` | relay |
| 2 | `FN_TAIL` | relay — shared cluster ground |
| 3 | `FN_HEADLIGHT_LOW` | relay |
| 4 | `FN_HEADLIGHT_HIGH` | relay |
| 5 | `FN_HORN` | relay; `OUT_PULSE` if you want a chirp |
| 6 | `FN_RAIN_LIGHT` | direct, if it is a separate unit with its own earth |
| 7-9 | spare outputs | |
| 10 | `FN_IN_BRAKE` (input) | from the hardwired brake switch |
| 11 | spare input | |

Eleven of twenty-one, with plenty in hand. Main, starter and fuel pump are absent because
the ECU drives them. The starter would be absent regardless: the firmware masks it out of
every CAN and peer path so a stray or replayed frame cannot crank the engine.

**Keypad** (node 4-7, channels default to inputs): headlights, high beam, horn, rain
light, fan override, pump prime, traction/launch/map-select, plus tell-tale inputs. Ten
or so of twenty-one.

Buttons switch **+12V into the channel** — inputs sense high, and need more than 10.87 V
at the terminal to read as pressed.

## Two buttons: ignition, and start/stop

With buttons to spare, split the two jobs rather than overloading one.

| Button | Goes to | Does |
|---|---|---|
| **Ignition** | `J_IGN` on the relay module | press to wake, hold to shut the car down |
| **Start/stop** | the ECU's `startStopButtonPin`, **directly** | cranks, and stops a running engine |

**Wire start/stop straight to the ECU, not through this board and not over CAN.** rusEFI
already owns `starterControlPin` and `startCrankingDuration`, has RPM off the crank
sensor rather than through a divider that goes unreadable during a crank, and applies its
own interlocks. A wire beats a bus for that, the same way the brake switch does.

uaDASH's on-screen start button sends `ECU_CAN_BUS_USER_CONTROL`, **extended id
`0x77000C`**, from rusEFI's bench-test protocol. This board could send it too. It should
not: that would make starting the car depend on this board being alive and the bus being
up, to replace a wire that does the job unconditionally.

### What splitting them buys

**The brake dependency leaves this board completely.** `ign_brake_ch` stays
`IGN_CH_NONE`, and the whole "the brake decides what this press means" branch becomes
dead code for this install -- the ignition button can no longer be mistaken for a start
attempt, because starting is not its job. The interlock still exists, in the ECU, where
the RPM signal actually is.

Starting also stops depending on this board at all. Ignition and start become
independent: killing ignition cannot be misread as a start, and a start press cannot end
in a shutdown.

The sequence is then exactly a modern car:

1. press **ignition** -- relay module latches, `+12V_SW` comes up, ECU boots
2. press **start/stop** -- ECU cranks, on its own timeout and interlocks
3. press **start/stop** again -- ECU stops the engine; the board stays awake
4. hold **ignition** -- board powers down, `+12V_SW` drops, ECU goes with it

### If the board should also SEE the start button

Only worth wiring for a tell-tale or logging -- nothing in the firmware needs it.

**Check the polarity before assuming one button can feed both.** rusEFI switch inputs are
generally pulled up and switched to ground, while every input on this board is
active-high and wants +12 V at the terminal. If that is the case here they are opposite
by nature, and a single-pole button cannot drive both. A two-pole momentary button solves
it -- one pole grounds the ECU pin, the other feeds +12 V to a `J_AUX` input or a keypad
channel.

## Asking the ECU to do things

Any input channel can be bound to a TunerStudio command, sent to the ECU as an extended
frame. Six slots:

    rcm_bench ctl ecucmd 0 5 startstop     # ch5 press: crank if stopped, stop if running
    rcm_bench ctl ecucmd 1 7 lua1          # ch7 press: bump rusEFI Lua counter 1
    rcm_bench ctl ecucmd 2 8 stopengine    # ch8 press: unconditional stop
    rcm_bench ctl ecucmd 3 9 22 43         # or any raw subsystem and index
    rcm_bench ctl ecucmd 0 none            # clear a slot

**`lua1`..`lua4` are the interesting ones for a car.** rusEFI has no fixed command for
traction control, launch control or a map switch -- what it has is four counters that a
Lua script can watch. Bind a button to one, write the behaviour in Lua, and none of it
needs a firmware change here.

Stored as a raw (subsystem, index) pair rather than a list of named commands, because
every TunerStudio command is that shape. Adding one is a config change, not a release.

Verified on revA: a touch of +12 V to a bound channel put
`T 0077000C 8 6600140009000000` on the wire -- a `T` record, so genuinely extended --
once per press, with real switch bounce.

### One button or two

`ign_ecu_flags` decides whether the IGNITION button also talks to the ECU. **Both bits
default off, which is the two-button car** -- the ignition button only powers the board,
and a separate button in `ecu_cmd[]` starts the engine. That is the simpler thing to
reason about and the recommended arrangement.

Set both bits for the one-button VW arrangement:

| Gesture | Result |
|---|---|
| single press from off | wakes the board, nothing else |
| wake press **held** with the brake down | wakes the board, then asks the ECU to start |
| press with the brake down, engine off | asks the ECU to start |
| press with the brake **up** | shuts the car down |
| **hold** while running | asks the ECU to stop, then powers down after `ign_shutdown_ms` |

A short press while running does nothing on purpose: stopping takes a deliberate hold, so
a hand catching the dash cannot cut the engine. `ign_hold_stop_ms` defaults to 1000 ms.

Two details that matter when setting this up:

- **The stop gesture sends `TS_STOP_ENGINE`, not the start/stop toggle.** The toggle is
  gated on the ECU's own view of engine speed; a stop must not be. The power-down runs on
  its own timer either way, so a shutdown proceeds identically if the ECU never answers.
- **Set rusEFI's `startButtonSuppressOnStartUpMs` shorter than `ign_wake_start_ms`**
  (2000 ms). rusEFI ignores start requests for a period after it gets ignition power, and
  its own source says that exists for exactly this arrangement -- a start button combined
  with an ECU power-source button. Leave it long and the wake-and-start gesture lands
  while the ECU is still ignoring requests.

## The three `J_AUX` inputs

`J_AUX` gives three more 12 V inputs without spending a channel -- they use the spare
eighth bit of each tile's 74HC165. Debounced like any input, published as byte 3 of the
INPUTS frame, shown on the `s` line of the console as `aux`.

**Input only**, and there is no ground pole on the connector: the return is via `J_PWR`.

They suit switches the board should KNOW about but must not control -- the hardwired
brake feed, a clutch, handbrake or reverse switch. That is what keeps the brake lights on
a mechanical switch while the ignition state machine still sees the pedal.

Like every input here they are **active-high**: the switch feeds +12 V, and above 10.87 V
at the terminal reads as pressed. The one thing to add is a **10 k pull-down from each
used pin to ground**. Channels have one fitted; `J_AUX` does not, so an open switch is
held down only through the 1.27 M divider. Fine at DC, but a high source impedance for a
long run near ignition coils.

## Ground budget

Every channel's return, coil and direct load alike, comes back through **one 3.5 mm pole
on `J_PWR`** rated about 8 A. Eight 80 Ω coils at 14.4 V is 8 x 180 mA = **1.44 A**, so
there is plenty of margin here — but it is the first thing to check before adding
directly-driven loads, and it is the known limit a revision should fix with a second
ground pole.

## Pairing the two boards

    rcm_bench ctl peer 4 1,2,5 5     # follow node 4 on channels 1,2,5; ch5 latches
    rcm_bench ctl peer none 0        # disable mirroring
    rcm_bench ctl save               # keep it across a power cycle

`RCM_OP_SET_PEER` (0x18) sets the node and both masks and installs the receive filter
for the peer's INPUTS frame. Verified on hardware with a PC impersonating a keypad, so
**a second board is not needed to test this** -- a keypad is only a node broadcasting an
INPUTS frame at `base + node * 0x10 + 1`, which anything on the bus can fake.

**Mirroring is strictly channel-for-channel.** Keypad channel N drives relay module
channel N, so the two channel maps have to be planned together and one button cannot
drive two outputs. That is the other reason the hazard switch stays conventional:
`FN_IN_HAZARD` is a label with no logic behind it.

Two things to know when watching this on a bus:

- **The board's transmit queue runs a couple of broadcast cycles deep**, so the first
  OUTPUTS frame after a command can still predate it. Flushing the host's receive buffer
  is not enough. Settle for a few hundred ms, or read for a while and take the last
  frame, before believing what a channel is doing.
- **Peer frames count as traffic addressed to this board**, so a mirrored channel is not
  reverted by the CAN timeout. Stop the keypad talking entirely and `failsafe_state`
  takes over as usual.

## Historical: peer mirroring was not configurable

The keypad-to-relay-module path is `peer_node`, `peer_mask` and `peer_toggle_mask` —
`peer_toggle_mask` being the one that turns a momentary button into a latching load.
All three are **read by the firmware and written by nothing**. There is no CAN opcode,
so a two-board setup currently needs a recompile to configure.

This is the same gap `ecu_rpm_can_id` had before `RCM_OP_SET_RUN_SRC`, and it needs the
same fix. Note that changing `peer_node` also changes a receive filter, so whatever sets
it must reinstall the filter set — see `install_filters()`.

**Peer mirroring is strictly channel-for-channel.** Keypad channel N drives relay module
channel N, so the two channel maps have to be planned together, and one button cannot
drive two outputs. That is the other reason the hazard switch stays conventional:
`FN_IN_HAZARD` is a label with no logic behind it.
