# RCM — Relay Control Module

![RCM board, top view](docs/board-top.png)

A CAN-controlled relay driver for a car. **21 universal channels**, each of which is
simultaneously a low-side output *and* an input — so the same board builds as a relay
controller or as a switch panel, decided in firmware rather than by a different PCB.

Designed for a [rusEFI](https://rusefi.com/) bus (uaEFI SUPER ECU + uaDASH display).

**Status: ordered 2026-08-08.** Boards are in fabrication; firmware has not been written yet.

## What it does

| | |
|---|---|
| **Channels** | 21, low-side switched, ~7 per driver |
| **Drivers** | 3 × TPL7407L (MOSFET, not Darlington — see below) |
| **I/O expansion** | 74HC595 out / 74HC165 in, daisy-chained |
| **MCU** | STM32F446RET6, LQFP-64 |
| **Bus** | CAN, 3.3V transceiver, switchable termination |
| **Sensing** | every channel is read back continuously, including while driving |
| **IMU** | Bosch BMI270, 6-axis |
| **Board** | 140 × 70 mm, 4-layer, solid ground plane |

## The channel trick

Each channel's terminal connects to three things at once — a driver output, a permanently
connected 1M/270k sense divider, and a fitted 10k pull-down:

- **As an output** it sinks a relay coil to ground.
- **As an input** a button wired to +12V pulls it high; the pull-down gives 1.2mA of wetting
  current, enough to keep switch contacts from going oxide-flaky.
- **As a diagnostic**, because the divider never disconnects, an idle channel reads battery
  voltage through a healthy relay coil and 0V through a blown fuse. **Fuse detection falls
  out of the same wiring** — no extra parts.

Getting all three from one resistor took a few goes. A pull-*up* would have been the obvious
choice and it silently breaks fuse sensing: blown and healthy both read high, 20mV apart.

## Configuration

An 8-way DIP sets what can't be fixed over the bus once it's wrong:

| Pos | Function |
|---|---|
| 1 | CAN termination (passive, parallels a solder jumper) |
| 2 | Role — relay module or keypad |
| 3–4 | Node address |
| 5 | CAN bitrate |
| 6 | Publish IMU data — only one board per car should |

## Repository layout

```
gen_spec.py      -> spec.json      circuit definition, the source of truth
gen_plan.py      -> board_plan.json  every part's physical position
jlc_parts.json                     LCSC part per line; stamped into the schematic
tools/                             scripted-board pipeline (schematic, place, route, export)
mfg/                               gerbers, BOM, CPL as ordered
SPEC.md / DESIGN.md                what was built, and why
```

The schematic and PCB are **generated, not drawn** — `gen_spec.py` and `gen_plan.py` are the
real design files. Editing `rcm.kicad_sch` by hand gets overwritten.

## Firmware

Not started. Targets the rusEFI CAN protocol at 500 kbps. Config bits are read on PC0–PC4.

## Honest notes

Three faults were caught *after* the board looked finished and every automated check was
green: a missing ground pour, a reverse-polarity diode wired backwards, and stale
manufacturing files. Two were spotted by eye, not by tooling. ERC passes a diode in either
orientation, and DRC says nothing at all about an absent copper pour.

`JB1` (the buck's input pins, 4 holes at 3.50mm pitch) is hand-fit — no header is made at
that pitch. Respacing it to 2.54mm is the obvious fix for a revision.

## Licence

Not yet chosen.
