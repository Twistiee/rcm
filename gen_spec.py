"""Emit the RCM schematic spec (spec.json) for the shared sch_gen.py.

Encodes SPEC.md: 21 universal low-side channels (3x TPL7407L, 7 each) behind
74HC595/74HC165 shift-register chains, an STM32F446RET6 (LQFP-64), SN65HVD230 CAN,
BMI270 IMU, a Waveshare buck on an ignition-latched 12V rail, USB-C and an SPI EEPROM.

The board builds as either the relay control module or the keypad node -- the
difference is which channels get their optional pull-up fitted (see R_PU*, all DNP
here) and how the loom is wired. Nothing on the board changes.

Run: python gen_spec.py  ->  spec.json
Then: python tools/sch_gen.py spec.json --verify
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))

# ---------------------------------------------------------------------------
# Footprints
# ---------------------------------------------------------------------------
R0805 = "Resistor_SMD:R_0805_2012Metric"
C0805 = "Capacitor_SMD:C_0805_2012Metric"
C1210 = "Capacitor_SMD:C_1210_3225Metric"
SOIC8 = "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm"
SOIC16 = "Package_SO:SOIC-16_3.9x9.9mm_P1.27mm"
TSSOP16 = "Package_SO:TSSOP-16_4.4x5mm_P0.65mm"
LQFP64 = "Package_QFP:LQFP-64_10x10mm_P0.5mm"
SOT23 = "Package_TO_SOT_SMD:SOT-23"
SOT223 = "Package_TO_SOT_SMD:SOT-223-3_TabPin2"
SMA = "Diode_SMD:D_SMA"
TSDSO = "Package_SO:Infineon_PG-TSDSO-14-22"
LGA14 = "Package_LGA:Bosch_LGA-14_3x2.5mm_P0.5mm"
F1812 = "Resistor_SMD:R_1812_4532Metric"
XTAL5032 = "Crystal:Crystal_SMD_5032-2Pin_5.0x3.2mm"
XTAL3215 = "Crystal:Crystal_SMD_3215-2Pin_3.2x1.5mm"
USBC = "Connector_USB:USB_C_Receptacle_HCTL_HC-TYPE-C-16P-01A"
SWPUSH = "Button_Switch_SMD:SW_SPST_TL3342"
PINHDR5 = "Connector_PinHeader_2.54mm:PinHeader_1x05_P2.54mm_Vertical"
PINHDR3 = "Connector_PinHeader_2.54mm:PinHeader_1x03_P2.54mm_Vertical"
BUCK_IN = "rcm:WaveshareBuck_IN_2x2_P3.50mm"
BUCK_OUT = "rcm:WaveshareBuck_OUT_3x3_P2.54mm"
KF2P = "rcm:TerminalBlock_KF2EDG-3.5-2P_1x02_P3.50mm_Horizontal"
KF3P = "rcm:TerminalBlock_KF2EDG-3.5-3P_1x03_P3.50mm_Horizontal"
KF7P = "rcm:TerminalBlock_KF2EDG-3.5-7P_1x07_P3.50mm_Horizontal"

comps = []
nets = {}
no_connects = []
NCH = 21          # universal channels
NTILE = 3         # tiles
PER = 7           # channels per tile


def C(ref, lib, value, fp, dnp=False, **kw):
    d = {"ref": ref, "lib": lib, "value": value, "footprint": fp}
    if dnp:
        d["dnp"] = True
    d.update(kw)
    comps.append(d)


def N(name, *pins):
    nets.setdefault(name, []).extend(pins)


# ---------------------------------------------------------------------------
# Block 1 -- power input and protection
# ---------------------------------------------------------------------------
C("J_PWR", "Connector_Generic:Conn_01x02", "PWR_IN", KF2P)
C("F1", "Device:Polyfuse", "PPTC_1A", F1812)
C("D1", "Diode:SS34", "SS34", SMA)
C("D2", "Device:D_TVS", "SMAJ33A", SMA)
C("C_BULK", "Device:C", "10uF_50V", C1210)

N("+12V_IN", "J_PWR.1", "F1.1")
N("+12V_FUSED", "F1.2", "D1.1")
N("+12V_P", "D1.2", "D2.1", "C_BULK.1")
N("GND", "J_PWR.2", "D2.2", "C_BULK.2")

# ---------------------------------------------------------------------------
# Block 2 -- ignition latch (revB U19 verbatim: resistive OR into IN)
# ---------------------------------------------------------------------------
# BTS7040-1EPZ, not the -1EPA this started as: JLC went to ZERO stock on the -1EPA
# (C534837) between quoting and ordering. Same base part number, so the Z is a
# temperature/variant grade rather than a different die -- same PG-TSDSO-14-22, same
# pinout, and better on both counts that matter (19mOhm vs 36, -40..+175 vs +150).
# The RCM:BTS70xx-1E symbol is deliberately generic across the 1-channel family, which
# is precisely so a stock-out can be absorbed without touching the layout.
# Fallback if the -1EPZ's thin stock goes too: BTS7004-1EPP, C534825, ~20k in stock.
C("U_LATCH", "RCM:BTS70xx-1E", "BTS7040-1EPZ", TSDSO)
C("R_LG", "Device:R", "47R", R0805)
C("R_LPD", "Device:R", "22k", R0805)
C("R_LIGN", "Device:R", "47k", R0805)
C("R_LHOLD", "Device:R", "1k", R0805)
C("C_LVS", "Device:C", "100nF", C0805)
C("C_LSW", "Device:C", "100nF", C0805)
C("J_IGN", "Connector_Generic:Conn_01x02", "IGN_IN", KF2P)
C("R_IGH", "Device:R", "1M", R0805)
C("R_IGL", "Device:R", "270k", R0805)

N("+12V_P", "U_LATCH.15", "C_LVS.1")
N("+12V_SW", "U_LATCH.8", "U_LATCH.9", "U_LATCH.10",
  "U_LATCH.12", "U_LATCH.13", "U_LATCH.14", "C_LSW.1")
N("GND", "C_LVS.2", "C_LSW.2", "R_LG.2", "R_LPD.2", "U_LATCH.3", "J_IGN.2", "R_IGL.2")
N("LATCH_GND", "U_LATCH.1", "R_LG.1")
N("LATCH_IN", "U_LATCH.2", "R_LPD.1", "R_LIGN.2", "R_LHOLD.2")
N("LATCH_IGN", "J_IGN.1", "R_LIGN.1", "R_IGH.1")
N("LATCH_HOLD", "R_LHOLD.1")            # -> MCU PB10, direct GPIO
N("IGN_SENSE", "R_IGH.2", "R_IGL.1")    # -> MCU PA0
no_connects += ["U_LATCH.4", "U_LATCH.5", "U_LATCH.6", "U_LATCH.7", "U_LATCH.11"]

# ---------------------------------------------------------------------------
# Block 3 -- buck module (socketed) + 3V3 LDO
# ---------------------------------------------------------------------------
C("JB1", "Connector_Generic:Conn_01x02", "WAVESHARE-IN", BUCK_IN)
C("JB2", "Connector_Generic:Conn_01x02", "WAVESHARE-OUT", BUCK_OUT)
# AMS1117-3.3, not the LD1117S33 this started as: the AMS is a JLC *basic* part and at
# qty 1 the per-unique-extended-part fee dwarfs the unit price. Safe swap -- both symbols
# `extends "AP1117-15"` in Regulator_Linear.kicad_sym, so they are pin-identical by
# construction, and both are SOT-223 with the tab on pin 2 (VOUT). 5V in leaves 1.7V
# headroom, well over the ~1.3V dropout.
C("U_LDO", "Regulator_Linear:AMS1117-3.3", "AMS1117-3.3", SOT223)
C("C_5V", "Device:C", "10uF_25V", C0805)
C("C_3V3I", "Device:C", "10uF_25V", C0805)
C("C_3V3O", "Device:C", "100nF", C0805)

N("+12V_SW", "JB1.1")
N("GND", "JB1.2", "JB2.2", "C_5V.2", "U_LDO.1", "C_3V3I.2", "C_3V3O.2")
N("+5V", "JB2.1", "C_5V.1", "U_LDO.3")
N("+3V3", "U_LDO.2", "C_3V3I.1", "C_3V3O.1")

# ---------------------------------------------------------------------------
# Block 4 -- MCU
# ---------------------------------------------------------------------------
C("U_MCU", "MCU_ST_STM32F4:STM32F446RETx", "STM32F446RET6", LQFP64)
for i, ref in enumerate(["C_M1", "C_M2", "C_M3", "C_M4", "C_M5"]):
    C(ref, "Device:C", "100nF", C0805)
C("C_MB", "Device:C", "4.7uF", C0805)
C("C_VCAP", "Device:C", "2.2uF", C0805)
C("C_VDDA", "Device:C", "100nF", C0805)
C("C_NRST", "Device:C", "100nF", C0805)
C("R_BOOT", "Device:R", "10k", R0805)
C("J_BOOT", "Connector_Generic:Conn_01x03", "BOOT_SEL", PINHDR3)
C("SW_RST", "Switch:SW_Push", "RESET", SWPUSH)
C("Y1", "Device:Crystal", "8MHz", XTAL5032)
C("C_Y1A", "Device:C", "20pF", C0805)
C("C_Y1B", "Device:C", "20pF", C0805)
C("Y2", "Device:Crystal", "32.768kHz", XTAL3215)
# 20pF, NOT the 12pF this started as. Load caps follow C = 2 x (CL - Cstray); the chosen
# crystal (C32346, Epson Q13FC13500004) is CL=12.5pF, so with ~3pF of stray it wants
# ~19pF, and 12pF would have left the loop ~3.5pF light -- about 22ppm FAST, or a minute
# a month on the one oscillator whose whole job is keeping time.
#
# Reusing the 8MHz value also deletes a BOM line, which at qty 1 is a saved per-part fee.
# The 8MHz keeps 20pF against its own 20pF CL: that runs ~60ppm fast, which is nothing
# beside CAN's ~1% and USB FS's 2500ppm budgets, and light caps only help startup margin.
# 33pF would be exact there if it ever matters -- at the cost of another BOM line.
C("C_Y2A", "Device:C", "20pF", C0805)
C("C_Y2B", "Device:C", "20pF", C0805)
C("J_SWD", "Connector_Generic:Conn_01x05", "SWD", PINHDR5)
C("D_LED1", "Device:LED", "GRN", "LED_SMD:LED_0805_2012Metric")
C("R_LED1", "Device:R", "1k", R0805)
C("D_LED2", "Device:LED", "RED", "LED_SMD:LED_0805_2012Metric")
C("R_LED2", "Device:R", "1k", R0805)

N("+3V3", "U_MCU.19", "U_MCU.32", "U_MCU.48", "U_MCU.64", "U_MCU.13", "U_MCU.1",
  "C_M1.1", "C_M2.1", "C_M3.1", "C_M4.1", "C_M5.1", "C_MB.1", "C_VDDA.1", "J_SWD.1")
# VSS: pin 18 is power_in, but 31/47/63 are typed *passive* in the KiCad symbol.
# They are still real ground pins -- without this they get silently auto-NC'd and
# the MCU ships with three grounds floating.
N("GND", "U_MCU.18", "U_MCU.31", "U_MCU.47", "U_MCU.63",
  "U_MCU.12", "C_M1.2", "C_M2.2", "C_M3.2", "C_M4.2", "C_M5.2",
  "C_MB.2", "C_VDDA.2", "C_VCAP.2", "C_NRST.2", "R_BOOT.2", "SW_RST.2",
  "C_Y1A.2", "C_Y1B.2", "C_Y2A.2", "C_Y2B.2", "J_SWD.5")
N("VCAP1", "U_MCU.30", "C_VCAP.1")
N("NRST", "U_MCU.7", "C_NRST.1", "SW_RST.1", "J_SWD.4")
N("BOOT0", "U_MCU.60", "R_BOOT.1", "J_BOOT.2")
N("+3V3", "J_BOOT.1")
N("GND", "J_BOOT.3")
N("HSE_IN", "U_MCU.5", "Y1.1", "C_Y1A.1")
N("HSE_OUT", "U_MCU.6", "Y1.2", "C_Y1B.1")
N("LSE_IN", "U_MCU.3", "Y2.1", "C_Y2A.1")
N("LSE_OUT", "U_MCU.4", "Y2.2", "C_Y2B.1")
N("SWDIO", "U_MCU.46", "J_SWD.2")
N("SWCLK", "U_MCU.49", "J_SWD.3")
# Status LEDs: MCU pin -> resistor -> LED anode(2), LED cathode(1) -> GND.
# (Device:LED pin 1 = K, pin 2 = A.)
N("LED1", "U_MCU.41", "R_LED1.1")       # PA8
N("LED2", "U_MCU.57", "R_LED2.1")       # PB5
N("LED1_A", "R_LED1.2", "D_LED1.2")
N("LED2_A", "R_LED2.2", "D_LED2.2")
N("GND", "D_LED1.1", "D_LED2.1")

# ---------------------------------------------------------------------------
# Block 5 -- CAN
# ---------------------------------------------------------------------------
C("U_CAN", "Interface_CAN_LIN:SN65HVD230", "SN65HVD230", SOIC8)
C("C_CAN", "Device:C", "100nF", C0805)
C("R_RS", "Device:R", "10k", R0805)
C("R_TERM", "Device:R", "120R", R0805)
C("R_TJ", "Device:R", "0R", R0805, dnp=True)
C("D_CAN", "Power_Protection:SZNUP2105L", "SZNUP2105L", SOT23)
C("J_CAN1", "Connector_Generic:Conn_01x03", "CAN_A", KF3P)
C("J_CAN2", "Connector_Generic:Conn_01x03", "CAN_B", KF3P)

N("+3V3", "U_CAN.3", "C_CAN.1")
N("GND", "U_CAN.2", "C_CAN.2", "R_RS.2", "D_CAN.3",
  "J_CAN1.3", "J_CAN2.3")
N("CAN_TX", "U_CAN.1")                  # <- MCU PB9
N("CAN_RX", "U_CAN.4")                  # -> MCU PB8
N("CAN_RS", "U_CAN.8", "R_RS.1")
N("CANH", "U_CAN.7", "J_CAN1.1", "J_CAN2.1", "D_CAN.1", "R_TERM.1")
N("CANL", "U_CAN.6", "J_CAN1.2", "J_CAN2.2", "D_CAN.2", "R_TJ.2")
N("CAN_TERM", "R_TERM.2", "R_TJ.1", "SW_CFG.1")

# ---------------------------------------------------------------------------
# Config DIP switch (8-way, 1.27mm half-pitch, top-right corner)
#
# SW_DIP_x08 pairs switch k with pins k and (17-k) -- verified off the symbol's
# own pin geometry, not assumed.
#
# Position 1 is PASSIVE: it parallels R_TJ, so closing the DIP terminates CAN, and
# soldering the 0R does the same thing permanently. Both fitted means you get a
# fingernail setting for the bench and a vibration-proof one for the car.
#
# Positions 2-8 are read by the MCU with its INTERNAL pull-ups enabled, so each
# switch just shorts a GPIO to ground -- no external resistors at all. Closed = 0.
#
# These carry the settings you cannot fix over the bus once they are wrong:
# termination, node identity, and bitrate all have to be right BEFORE CAN works.
# IMU_EN is the exception -- it is here because only one board in a car should be
# publishing orientation, and a keypad bolted into a door card is not that board.
# ---------------------------------------------------------------------------
C("SW_CFG", "Switch:SW_DIP_x08", "CFG_DIP",
  "Button_Switch_SMD:SW_DIP_SPSTx08_Slide_KingTek_DSHP08TS_W7.62mm_P1.27mm")
N("CANL", "SW_CFG.16")                   # pos 1 -> CAN termination
N("GND", *["SW_CFG.%d" % n for n in range(11, 16)])  # common side of pos 2-6
# Positions 7 and 8 are left UNCONNECTED. They were spare bits, and routing two more
# nets from the top-right corner across the CAN/IMU corridor to the MCU is what tipped
# this board from routable to not -- 3 nets failed, then 1. The switch positions still
# physically exist if a future revision wants them; only the copper is gone.
no_connects += ["SW_CFG.7", "SW_CFG.8", "SW_CFG.9", "SW_CFG.10"]
for _p, _net in ((2, "CFG_ROLE"), (3, "CFG_ADDR0"), (4, "CFG_ADDR1"), (5, "CFG_BAUD"),
                 (6, "CFG_IMU_EN")):
    N(_net, "SW_CFG.%d" % _p)
no_connects += ["U_CAN.5"]

# ---------------------------------------------------------------------------
# Block 6 -- IMU (BMI270; the KiCad BMI160 symbol is pin-identical, verified)
# ---------------------------------------------------------------------------
C("U_IMU", "Sensor_Motion:BMI160", "BMI270", LGA14)
C("C_IMU1", "Device:C", "100nF", C0805)
C("C_IMU2", "Device:C", "100nF", C0805)
C("R_ADDR", "Device:R", "0R", R0805)
C("R_ADDR_ALT", "Device:R", "0R", R0805, dnp=True)
C("R_SDA", "Device:R", "4.7k", R0805)
C("R_SCL", "Device:R", "4.7k", R0805)

# CSB(12) -> VDDIO selects I2C. Tying it to GND would select SPI mode instead
# (BST-BMI270-DS000 Table 22, "Connect to ... in I2C" column).
N("+3V3", "U_IMU.8", "U_IMU.5", "U_IMU.12", "C_IMU1.1", "C_IMU2.1")
N("GND", "U_IMU.7", "U_IMU.6", "C_IMU1.2", "C_IMU2.2", "R_ADDR.2")
N("IMU_SDA", "U_IMU.14", "R_SDA.2")     # -> MCU PB7
N("IMU_SCL", "U_IMU.13", "R_SCL.2")     # -> MCU PB6
N("IMU_INT1", "U_IMU.4")                # -> MCU PB11
N("IMU_SDO", "U_IMU.1", "R_ADDR.1", "R_ADDR_ALT.1")  # R_ADDR->GND = 0x68
N("+3V3", "R_SDA.1", "R_SCL.1", "R_ADDR_ALT.2")      # ALT fitted = 0x69
no_connects += ["U_IMU.2", "U_IMU.3", "U_IMU.9", "U_IMU.10", "U_IMU.11"]

# ---------------------------------------------------------------------------
# Block 7 -- shift-register chains
# ---------------------------------------------------------------------------
# 74HC595 outputs QA..QH = pins 15,1,2,3,4,5,6,7
Q595 = ["15", "1", "2", "3", "4", "5", "6", "7"]
# 74HC165 inputs D0..D7 = pins 11,12,13,14,3,4,5,6
D165 = ["11", "12", "13", "14", "3", "4", "5", "6"]

C("R_OE", "Device:R", "10k", R0805)      # pull OE_N high at boot -> outputs Hi-Z
N("+3V3", "R_OE.1")
N("SR_OE_N", "R_OE.2")                   # -> MCU PB2

for t in range(1, NTILE + 1):
    so, si = "U_SO%d" % t, "U_SI%d" % t
    C(so, "74xx:74HC595", "74HC595", SOIC16)
    C(si, "74xx:74HC165", "74HC165", SOIC16)
    C("C_SO%d" % t, "Device:C", "100nF", C0805)
    C("C_SI%d" % t, "Device:C", "100nF", C0805)
    N("+3V3", so + ".16", si + ".16", "C_SO%d.1" % t, "C_SI%d.1" % t, so + ".10")
    N("GND", so + ".8", si + ".8", "C_SO%d.2" % t, "C_SI%d.2" % t, si + ".15")
    N("SR_SCK", so + ".11", si + ".2")
    N("SR_RCLK", so + ".12")
    N("SR_OE_N", so + ".13")
    N("SR_PL", si + ".1")
    no_connects.append(si + ".7")        # ~Q7 unused

# 595 daisy chain, EAST to WEST: MOSI enters at U_SO3, the tile physically nearest
# the MCU, and ripples away from it. Chaining the other way made MOSI a ~74mm haul
# across the whole board to the far tile before doubling back.
# Firmware note: this sets the bit order -- the FIRST byte shifted out ends up in
# U_SO1 (channels 1-7), the last in U_SO3 (channels 15-21).
N("SR_MOSI", "U_SO3.14")
N("SR_CH32", "U_SO3.9", "U_SO2.14")
N("SR_CH21", "U_SO2.9", "U_SO1.14")
no_connects.append("U_SO1.9")
# 165 daisy chain: SI1.DS=GND, SI1.Q7->SI2.DS, ..., SI3.Q7 -> MISO
N("GND", "U_SI1.10")
N("SR_DI12", "U_SI1.9", "U_SI2.10")
N("SR_DI23", "U_SI2.9", "U_SI3.10")
N("SR_MISO", "U_SI3.9")

# ---------------------------------------------------------------------------
# Block 8 -- 3 identical tiles, 7 universal channels each
# ---------------------------------------------------------------------------
for t in range(1, NTILE + 1):
    drv = "U_DRV%d" % t
    C(drv, "Transistor_Array:TPL7407LAPW", "TPL7407L", TSSOP16)
    C("J_CH%d" % t, "Connector_Generic:Conn_01x07", "CH%d-%d" % ((t - 1) * PER + 1, t * PER), KF7P)
    N("GND", drv + ".8")
    N("+12V_P", drv + ".9")              # COM = flyback clamp rail
    for k in range(1, PER + 1):
        ch = (t - 1) * PER + k
        # 595 QA..QG drive IN1..IN7
        N("DRV%d" % ch, "U_SO%d.%s" % (t, Q595[k - 1]), "%s.%d" % (drv, k))
        # TPL7407L OUT1..OUT7 = pins 16,15,14,13,12,11,10
        N("CH%d" % ch, "%s.%d" % (drv, 17 - k), "J_CH%d.%d" % (t, k),
          "R_SH%d.1" % ch, "R_PU%d.1" % ch)
        N("SNS%d" % ch, "R_SH%d.2" % ch, "R_SL%d.1" % ch,
          "U_SI%d.%s" % (t, D165[k - 1]))
        N("GND", "R_SL%d.2" % ch)
        N("GND", "R_PU%d.2" % ch)
        C("R_SH%d" % ch, "Device:R", "1M", R0805)
        C("R_SL%d" % ch, "Device:R", "270k", R0805)
        C("R_PU%d" % ch, "Device:R", "10k", R0805)
    # 595 QH (8th output) unused on every tile
    no_connects.append("U_SO%d.%s" % (t, Q595[7]))

# 3 dedicated aux inputs on the spare 165 bit of each tile
C("J_AUX", "Connector_Generic:Conn_01x03", "AUX_IN", KF3P)
for a in range(1, 4):
    C("R_AH%d" % a, "Device:R", "1M", R0805)
    C("R_AL%d" % a, "Device:R", "270k", R0805)
    N("AUX%d" % a, "J_AUX.%d" % a, "R_AH%d.1" % a)
    N("ASNS%d" % a, "R_AH%d.2" % a, "R_AL%d.1" % a, "U_SI%d.%s" % (a, D165[7]))
    N("GND", "R_AL%d.2" % a)

# ---------------------------------------------------------------------------
# Block 9 -- USB-C + SPI EEPROM
# ---------------------------------------------------------------------------
C("J_USB", "Connector:USB_C_Receptacle_USB2.0_16P", "USB-C", USBC)
C("R_CC1", "Device:R", "5.1k", R0805)
C("R_CC2", "Device:R", "5.1k", R0805)
N("GND", "J_USB.A1", "J_USB.B1", "J_USB.A12", "J_USB.B12", "J_USB.SH", "R_CC1.2", "R_CC2.2")
N("USB_VBUS", "J_USB.A4", "J_USB.B4", "J_USB.A9", "J_USB.B9")
N("USB_DM", "J_USB.A7", "J_USB.B7")     # -> MCU PA11
N("USB_DP", "J_USB.A6", "J_USB.B6")     # -> MCU PA12
N("USB_CC1", "J_USB.A5", "R_CC1.1")
N("USB_CC2", "J_USB.B5", "R_CC2.1")
no_connects += ["J_USB.A8", "J_USB.B8"]

# M95640, not the 25LC640 this started as: JLC lists the 25LC640 at $0.73-1.42 with ~3k
# stock, against $0.33 for the ST part. Both are 64Kbit SPI EEPROMs on the industry-standard
# 25-series pinout (1=CS 2=SO 3=WP 4=GND 5=SI 6=SCK 7=HOLD 8=Vcc), which is what the generic
# Memory_EEPROM:25LCxxx symbol already describes -- so the symbol and every net stay put.
C("U_EEP", "Memory_EEPROM:25LCxxx", "M95640", SOIC8)
C("C_EEP", "Device:C", "100nF", C0805)
N("+3V3", "U_EEP.8", "C_EEP.1", "U_EEP.3", "U_EEP.7")
N("GND", "U_EEP.4", "C_EEP.2")
N("EEP_CS", "U_EEP.1")                  # -> MCU PB12
N("EEP_MISO", "U_EEP.2")                # -> MCU PB14
N("EEP_MOSI", "U_EEP.5")                # -> MCU PB15
N("EEP_SCK", "U_EEP.6")                 # -> MCU PB13

# ---------------------------------------------------------------------------
# MCU pin map. sch_gen resolves pins by NUMBER only, so this is (number, name).
#
# Deliberately unused: PA15(50), PB3(55), PB4(56) are the JTAG pins (JTDI, JTDO,
# NJTRST) and default to JTAG at reset -- unsafe for anything that must be driven
# early, which is why LATCH_HOLD is on PB10 instead.
#
# NOTE: PB11 does NOT exist on LQFP-64 (pin list jumps PB10=29 -> PB12=33).
# ---------------------------------------------------------------------------
PINMAP = {
    "14": ("PA0", "IGN_SENSE"),
    "15": ("PA1", "IMU_INT1"),
    "21": ("PA5", "SR_SCK"),
    "22": ("PA6", "SR_MISO"),
    "23": ("PA7", "SR_MOSI"),
    "41": ("PA8", "LED1"),
    "44": ("PA11", "USB_DM"),
    "45": ("PA12", "USB_DP"),
    "46": ("PA13", "SWDIO"),
    "49": ("PA14", "SWCLK"),
    "26": ("PB0", "SR_RCLK"),
    "27": ("PB1", "SR_PL"),
    "28": ("PB2", "SR_OE_N"),
    "29": ("PB10", "LATCH_HOLD"),   # direct GPIO, never a shift-register output
    "57": ("PB5", "LED2"),
    "58": ("PB6", "IMU_SCL"),
    "59": ("PB7", "IMU_SDA"),
    "61": ("PB8", "CAN_RX"),
    "62": ("PB9", "CAN_TX"),
    "33": ("PB12", "EEP_CS"),
    "34": ("PB13", "EEP_SCK"),
    "35": ("PB14", "EEP_MISO"),
    "36": ("PB15", "EEP_MOSI"),
    # Config DIP positions 2-8. Plain GPIOs on port C, deliberately avoiding the
    # JTAG pins and PC13 (weak drive / RTC tamper). Internal pull-up, switch to GND.
    "8":  ("PC0", "CFG_ROLE"),    # RCM or keypad -- decides CAN IDs and behaviour
    "9":  ("PC1", "CFG_ADDR0"),
    "10": ("PC2", "CFG_ADDR1"),   # ADDR0+1 -> 4 nodes per role
    "11": ("PC3", "CFG_BAUD"),    # 500k / 1M -- wrong means totally mute on the bus
    "24": ("PC4", "CFG_IMU_EN"),  # publish IMU data, so only one board in the car does
}
for num, (_port, net) in PINMAP.items():
    N(net, "U_MCU.%s" % num)

# PA9(42)/PA10(43): the ESP32-C3 co-processor was DROPPED (user, 2026-08-05).
# They are ordinary spare GPIO (USART1-capable if ever wanted).
no_connects += ["U_MCU.42", "U_MCU.43"]

single = [k for k, v in nets.items() if len(set(v)) < 2]
if single:
    raise SystemExit("SINGLE-PIN NETS (dangling, almost always a wiring bug): %s"
                     % ", ".join(sorted(single)))

# ---------------------------------------------------------------------------
# Stamp the chosen LCSC part onto every component as a hidden schematic field, so the
# part choice lives in the design itself rather than only in a side file. jlc_parts.json
# stays the single place a number is edited; this just propagates it.
#
# Keyed "value|footprint" -- the same composite key gen_jlc_bom_cpl.py uses, because the
# package is part of the choice (a TSSOP 74HC595 will not fit these SOIC-16 lands).
# ---------------------------------------------------------------------------
with open(os.path.join(HERE, "jlc_parts.json"), encoding="utf-8") as f:
    _parts = json.load(f)["parts"]

_stamped = 0
for _c in comps:
    _key = "%s|%s" % (_c.get("value", ""), _c.get("footprint", ""))
    if _key not in _parts:
        raise SystemExit("no jlc_parts.json line for %s (%s) -- rerun the rekey" % (_c["ref"], _key))
    _lcsc = _parts[_key].get("lcsc")
    if _lcsc:
        _c.setdefault("fields", {})["LCSC"] = _lcsc
        _stamped += 1

spec = {
    "schematic": os.path.join(HERE, "rcm.kicad_sch"),
    "project": "rcm",
    "title": "RCM - Relay Control Module / Keypad (21 universal channels)",
    "rev": "A",
    "symbol_dirs": [
        r"C:/Program Files/KiCad/10.0/share/kicad/symbols",
        os.path.join(HERE, "lib"),
    ],
    "components": comps,
    "nets": [{"name": k, "pins": sorted(set(v))} for k, v in sorted(nets.items())],
    # +3V3 is driven by the LDO (power_out) and +12V_SW by the PROFET OUT pins, so
    # flagging them would be a second power source on the same net -- ERC error.
    "power_flags": ["GND", "+5V", "+12V_P", "+12V_IN"],
    "no_connects": sorted(set(no_connects)),
}

with open(os.path.join(HERE, "spec.json"), "w", encoding="utf-8") as f:
    json.dump(spec, f, indent=1)

print("components : %d" % len(comps))
print("nets       : %d" % len(spec["nets"]))
print("no_connects: %d" % len(spec["no_connects"]))
print("channels   : %d in %d tiles" % (NCH, NTILE))
print("LCSC fields: %d of %d components stamped (%d lines still unsourced)"
      % (_stamped, len(comps), sum(1 for v in _parts.values() if not v.get("lcsc"))))
