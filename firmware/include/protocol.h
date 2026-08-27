/*
 * protocol.h -- every CAN identifier this board uses, in one place.
 *
 * ===========================================================================
 * WHY THESE IDs (checked against rusEFI's own source, 2026-08-09)
 * ===========================================================================
 * The bus this board has to share is a rusEFI uaEFI SUPER plus a uaDASH. What
 * rusEFI actually occupies:
 *
 *   0x100, 0x102        TunerStudio over CAN
 *   0x130, 0x131        VAG yaw/lateral-G IMU input
 *   0x150, 0x151        Mercedes A0065422618 IMU input
 *   0x174, 0x178, 0x17C Bosch MM5.10 IMU input          <-- see below, we USE these
 *   0x190               rusEFI wideband O2, two-way
 *   0x200 .. 0x20B      rusEFI verbose gauge broadcast (base configurable)
 *   0x667, 0x7E1        OpenBLT bootloader
 *   0x7DF, 0x7E0..0x7E8 OBD2
 *   0x770000 +          bench-test protocol, EXTENDED ids -- no clash with 11-bit
 *
 * 0x300 is clear of all of it, with room above and below, so that is the default
 * base. It is not baked in: can_base_id lives in EEPROM and can be moved over the
 * bus if some other device on your loom wants that space.
 *
 * ID MAP
 *   base + node*0x10 + f   f = 0..15, one 16-ID block per node
 *   base + 0x80            global control, every node listens
 *
 * With 8 nodes (4 addresses x 2 roles) the whole thing lives in 0x300..0x380.
 *
 * ===========================================================================
 * uaDASH INTEGRATION
 * ===========================================================================
 * uaDASH renders any third-party broadcast for which it has a DBC. So rather than
 * imitating rusEFI's frame layout -- which would have meant squeezing 21 channels
 * into a shape designed for RPM and coolant temp -- this board broadcasts its own
 * clean frames and ships docs/rcm.dbc alongside. Keep the two in step: if you edit
 * a frame here, edit the DBC.
 *
 * The IMU is the exception, and the happy one. rusEFI already decodes Bosch MM5.10
 * accelerometer frames natively, and our IMU is a Bosch BMI270. Emitting MM5.10 at
 * 0x174/0x178/0x17C means the ECU picks up yaw rate and lateral/longitudinal/vertical
 * G with nothing to configure but imuType = IMU_MM5_10 in TunerStudio.
 */
#ifndef RCM_PROTOCOL_H
#define RCM_PROTOCOL_H

#include <stdint.h>

#define RCM_CAN_BASE_DEFAULT   0x300u
#define RCM_CAN_NODE_STRIDE    0x10u
#define RCM_CAN_GLOBAL_OFFSET  0x80u

/* --- frame offsets within a node's block ----------------------------------- */
/* transmitted by us */
#define RCM_F_OUTPUTS   0x0
#define RCM_F_INPUTS    0x1
#define RCM_F_FAULTS    0x2
#define RCM_F_STATUS    0x3
/* received by us */
#define RCM_F_CMD_SET   0x8
#define RCM_F_CMD_CTL   0x9

/* --- Bosch MM5.10 emulation (rusEFI decodes these with no custom code) ------ */
#define MM5_10_ID_YAW_Y  0x174u   /* [0:2] yaw rate, [4:6] lateral (Y) accel      */
#define MM5_10_ID_ROLL_X 0x178u   /*                [4:6] longitudinal (X) accel  */
#define MM5_10_ID_Z      0x17Cu   /*                [4:6] vertical (Z) accel      */
/* Both fields are 16-bit little-endian offset binary, biased by 0x8000. rusEFI
 * multiplies by these to get physical units, so we must divide by them. */
#define MM5_10_RATE_QUANT  0.005f     /* deg/s per LSB */
#define MM5_10_ACC_QUANT   0.0001274f /* g per LSB     */

/* --- CMD_CTL opcodes (byte 0) ---------------------------------------------- */
#define RCM_OP_ALL_OFF        0x01
#define RCM_OP_OUTPUTS_ENABLE 0x02   /* b1: 0 = Hi-Z, 1 = live */
#define RCM_OP_CLEAR_FAULTS   0x03
#define RCM_OP_SAVE_CONFIG    0x04
#define RCM_OP_LOAD_DEFAULTS  0x05   /* in RAM only, until SAVE_CONFIG */
#define RCM_OP_REBOOT         0x06   /* b1 must be 0xA5 */
#define RCM_OP_SET_CH_MODE    0x10   /* b1 ch(0-based), b2 mode, b3 flags */
#define RCM_OP_SET_BITRATE    0x11   /* b1..b4 LE Hz; needs SAVE + REBOOT */
#define RCM_OP_SET_BASE_ID    0x12   /* b1..b2 LE;    needs SAVE + REBOOT */
#define RCM_OP_SET_TIMING     0x13   /* b1..b2 broadcast_ms, b3..b4 can_timeout_ms */
#define RCM_OP_SET_FAILSAFE   0x14   /* b1..b3 channel bits */
#define RCM_OP_SET_IGNITION   0x15   /* b1 mode, b2 brake ch, b3 start ch, b4 run ch,
                                      * b5 RUN-position output ch (optional);
                                      * 0xFF on any channel means "not configured" */
#define RCM_OP_SET_IGN_TIMES  0x16   /* b1..b2 hold-to-stop ms, b3..b4 crank max ms,
                                      * b5..b6 shutdown hold ms (optional) */
#define RCM_OP_SET_RUN_SRC    0x17   /* b1..b2 LE ECU RPM frame id (0 = no CAN source),
                                      * b3..b4 LE rpm at or above which the engine counts
                                      * as running. Installs a receive filter for the id,
                                      * so this is what makes "hold to stop a RUNNING
                                      * engine" work at all -- without a run source the
                                      * board cannot tell a running engine from a stopped
                                      * one, and every press is treated as "switch off". */
#define RCM_OP_SET_PEER       0x18   /* b1 peer node 0..7 or 0xFF for none,
                                      * b2..b4 channels that follow the peer at all,
                                      * b5..b7 of those, the ones where a PRESS TOGGLES
                                      * rather than follows -- momentary button, latching
                                      * load. Mirroring is strictly channel-for-channel:
                                      * peer channel N drives channel N here. */
#define RCM_OP_SET_ECU_CMD    0x19   /* b1 slot 0..RCM_ECU_CMDS-1, b2 input channel or
                                      * 0xFF for unused, b3..b4 LE subsystem,
                                      * b5..b6 LE index */

/* --- asking rusEFI to start or stop the engine ------------------------------
 * An EXTENDED frame from rusEFI's bench-test command block. Verified against rusEFI's
 * own source (bench_test.cpp processCanUserControl, and the ts_command_e / ts_14_command
 * enums in engine_types.h), not guessed:
 *
 *   id 0x77000C ext, data 66 00 14 00 09 00 00 00
 *      66     BENCH_HEADER magic, checked first by processCanEcuControl()
 *      14 00  subsystem, LE = TS_X14 (20)
 *      09 00  index,     LE = TS_START_STOP_ENGINE (9)
 *
 * It lands on startStopButtonToggle() -- the SAME entry point as rusEFI's own physical
 * start/stop button. So this asks; the ECU decides. Cranking stays subject to rusEFI's
 * interlocks, its RPM off the crank sensor, and startCrankingDuration. This board does
 * not know or care whether the result was a start or a stop.
 */
#define RCM_ECU_CMD_ID        0x77000Cu   /* extended */
#define RCM_ECU_CMD_MAGIC     0x66u

/* Subsystems (ts_command_e) and the indices worth naming. Values read out of rusEFI's
 * engine_types.h, not guessed. */
#define RCM_ECU_SUB_X14       0x0014u   /* TS_X14 */
#define RCM_ECU_SUB_BENCH     0x0016u   /* TS_BENCH_CATEGORY */
#define RCM_ECU_SUB_STOP      0x0024u   /* TS_STOP_ENGINE -- index ignored */
#define RCM_ECU_IDX_STARTSTOP 0x0009u   /* TS_START_STOP_ENGINE, under TS_X14 */
#define RCM_ECU_IDX_LUA1      0x0021u   /* LUA_COMMAND_1, under TS_BENCH_CATEGORY */

/* Send one TunerStudio command to the ECU. */
void proto_send_ecu_cmd(uint16_t subsystem, uint16_t index);

/* --- OUTPUTS frame, byte 3 status flags ------------------------------------ */
#define RCM_ST_OUT_ENABLED  0x01
#define RCM_ST_FAILSAFE     0x02   /* the bus went quiet and we fell back */
#define RCM_ST_ANY_FAULT    0x04
#define RCM_ST_IMU_OK       0x08
#define RCM_ST_EEPROM_OK    0x10
#define RCM_ST_ROLE_KEYPAD  0x20
#define RCM_ST_IGN_ON       0x40
#define RCM_ST_FORCED_500K  0x80   /* the CFG_BAUD recovery strap is closed */

/* --- firmware version, as broadcast in the STATUS frame -------------------- */
#define RCM_FW_MAJOR 0
#define RCM_FW_MINOR 2
#define RCM_FW_PATCH 0

uint16_t proto_node_base(void);           /* base + node*stride */
uint16_t proto_id(uint8_t frame_offset);  /* our node's id for that frame */
uint16_t proto_global_id(void);

void     proto_begin(void);               /* installs the hardware filters */
void     proto_sanitise_base(void);       /* clamp can_base_id to something workable */
void     proto_broadcast(uint32_t now_ms);
void     proto_poll(uint32_t now_ms);
bool     proto_failsafe(void);
uint32_t proto_last_rx(void);

#endif /* RCM_PROTOCOL_H */
