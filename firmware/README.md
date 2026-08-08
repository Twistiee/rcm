# RCM firmware

**Status: started — `include/board.h` only.** Nothing runs yet.

## What exists

`include/board.h` — the pin map, **generated from `gen_spec.py`'s PINMAP**, which is the same
table that produced the schematic and therefore the copper. It is not typed by hand and
should not be edited by hand; regenerate it if the hardware changes. A firmware pin map that
has quietly drifted from the netlist is a miserable afternoon.

Every pin resolved against the real netlist — no placeholders.

## The shape of the problem

This board does very little, and the firmware should reflect that:

1. Assert `LATCH_HOLD` **early in boot**, or the board cuts its own power when the ignition
   drops. This is the one thing that must happen before anything else.
2. Read the config DIP once at startup — role, address, bitrate, IMU enable.
3. Bring up CAN at the selected bitrate.
4. Loop: shift 24 bits out to the drivers, shift 24 bits back from the sense chain, publish
   state, apply commands.
5. Publish IMU frames, but only if `CFG_IMU_EN` says this is the board that does.

## Decisions already made in hardware

- **Bus is rusEFI** (uaEFI SUPER + uaDASH), default **500 kbps**. `CFG_BAUD` selects
  500k/1M because a wrong bitrate makes the node silent and unfixable over the bus.
- **Channel mode is a software table.** The hardware is identical on all 21 channels; whether
  one is an output or an input is firmware's decision, stored in the EEPROM. There is no way
  to read it back from the pins — see the note below.
- **Config bits are active-low.** Internal pull-ups, switch shorts to ground.

## The thing to be careful about

**A channel's mode cannot be inferred from its sense bit.** Input-with-button-pressed and
output-with-blown-fuse read *identically*, as do input-idle and output-with-healthy-fuse. The
mode has to come from configuration, and if the stored config disagrees with how the loom is
actually wired, the board will report plausible nonsense — a pressed button as a blown fuse.

Worth designing the config-report message early so a mismatch is visible rather than
mysterious.

## Not yet decided

- Toolchain. Arduino-on-STM32 vs bare HAL vs butchering the SynapsePDM firmware. The scope
  here is small enough that Arduino is defensible.
- CAN message IDs. Should follow rusEFI conventions so uaDASH can render channel state
  without custom display code, rather than inventing a private format.
- Whether the EEPROM config is written over CAN, over USB, or both.
