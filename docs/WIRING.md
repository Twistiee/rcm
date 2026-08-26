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
| 1 | `FN_MAIN_RELAY` | set `failsafe_state` bit |
| 2 | `FN_FUEL_PUMP` | `failsafe_state` on = primes at wake |
| 3 | `FN_FAN_1` | |
| 4 | `FN_TAIL` | |
| 5 | `FN_HEADLIGHT_LOW` | |
| 6 | `FN_HEADLIGHT_HIGH` | |
| 7 | `FN_HORN` | `OUT_PULSE` if you want a chirp |
| 8 | `FN_RAIN_LIGHT` | direct if separately grounded |
| 9-10 | spare outputs | |
| 11 | `FN_IN_BRAKE` (input) | from the hardwired brake switch |
| 12 | spare input | |

Twelve of twenty-one. The starter is deliberately absent: the ECU owns it, and the
firmware masks it out of every CAN and peer path so a stray frame cannot crank.

**Keypad** (node 4-7, channels default to inputs): headlights, high beam, horn, rain
light, fan override, pump prime, traction/launch/map-select, plus tell-tale inputs. Ten
or so of twenty-one.

Buttons switch **+12V into the channel** — inputs sense high, and need more than 10.87 V
at the terminal to read as pressed.

## Ground budget

Every channel's return, coil and direct load alike, comes back through **one 3.5 mm pole
on `J_PWR`** rated about 8 A. Eight 80 Ω coils at 14.4 V is 8 x 180 mA = **1.44 A**, so
there is plenty of margin here — but it is the first thing to check before adding
directly-driven loads, and it is the known limit a revision should fix with a second
ground pole.

## Before this works: peer mirroring is not configurable

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
