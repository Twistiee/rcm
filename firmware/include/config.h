/*
 * config.h -- what the board is, and what each channel is for.
 *
 * Two quite different things live here and it is worth keeping them apart:
 *
 *   STRAPS  come from the DIP switch and can only change with the power off. Role,
 *           node address, IMU enable, and the 500k recovery override. These are the
 *           settings you cannot fix over the bus once they are wrong, which is
 *           exactly why they are on a switch and not in EEPROM.
 *
 *   CONFIG  comes from EEPROM and can be changed at runtime over CAN. Bitrate,
 *           message IDs, and the channel mode table -- the in/out decision that this
 *           board deliberately made a software matter rather than a hardware one
 *           (see DESIGN.md, "Channel mode became a software table").
 */
#ifndef RCM_CONFIG_H
#define RCM_CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include "board.h"
#include "ignition.h"

/* ---- channel mode table --------------------------------------------------- */
enum ch_mode_t {
    CH_UNUSED = 0,   /* never driven, never diagnosed, not reported as a fault    */
    CH_OUTPUT = 1,   /* drives a relay coil low-side; sense = coil circuit health */
    CH_INPUT  = 2,   /* never driven; sense = a button switching +12V            */
};

/* Channel flags. INVERT applies to the logical value in BOTH directions: an inverted
 * output is energised when commanded off, an inverted input reads pressed when the
 * line is low. NO_DIAG suppresses fault reporting on a channel whose load legitimately
 * looks broken -- an output feeding something other than a relay coil, typically. */
#define CH_F_INVERT   0x01
#define CH_F_NO_DIAG  0x02

#define PEER_NONE     0xFFu

/* HOW a channel behaves when commanded on. Orthogonal to what it is FOR -- an indicator
 * and a rain light are different functions with the same behaviour, and a horn and a
 * washer are different functions that both want a pulse. */
enum ch_behaviour_t {
    OUT_STEADY    = 0,  /* on means on. The default, and what most loads want.       */
    OUT_FLASH     = 1,  /* on means flash, at the shared cfg.flash_period_ms so that
                         * every flashing channel stays in phase -- which is what
                         * makes hazards look right rather than like two indicators
                         * that happen to be on.                                     */
    OUT_PULSE     = 2,  /* on fires a single pulse of `param` ms and then stops, until
                         * released and pressed again. Horn chirp, washer, prime.    */
    OUT_DELAY_OFF = 3,  /* on immediately; off is deferred by `param` ms. Courtesy
                         * lights, fan run-on, anything that should linger.          */
};

/* HOW an INPUT behaves. Shares the `behaviour` byte with ch_behaviour_t, which is
 * outputs-only, so a channel's one behaviour byte means whichever applies to its mode.
 * IN_MOMENTARY is 0, the same as OUT_STEADY, so the default is right for both.
 *
 * These change what the channel REPORTS, not what some output does. That is the useful
 * place for it: a latched keypad button then carries its latched state in the INPUTS
 * frame, so a relay module mirroring that keypad just follows it, the ECU-command
 * binding sees one edge per intent, and a dash shows the real state. Doing it at the
 * destination instead -- which is what peer_toggle_mask does -- only works for the one
 * consumer that implements it. */
enum ch_in_behaviour_t {
    IN_MOMENTARY = 0,  /* reports the switch, as it is. The default.               */
    IN_TOGGLE    = 1,  /* each PRESS flips the reported state and it stays there.
                        * A momentary button driving a latching load.              */
    IN_HOLD_ARM  = 2,  /* reports true only once the switch has been held for
                        * `param` ms, and false the instant it is released. For
                        * things a knock must not arm -- launch control being the
                        * case this exists for.                                    */
};

/* WHAT a channel is for. Mostly a label -- it names the channel for the bench tool and
 * the self-test console, and it is what a dash would show instead of "CH07".
 *
 * Four of them mean something to the firmware as well, because it has to be able to
 * FIND those channels: FN_IGNITION, FN_STARTER, FN_IN_BRAKE and FN_IN_ENGINE_RUN. The
 * rest are purely descriptive and can be extended freely. */
enum ch_func_t {
    FN_NONE = 0,
    /* --- engine and drivetrain, outputs --- */
    FN_IGNITION,        /* the key's RUN position, feeding the ECU ignition input */
    FN_STARTER,
    FN_MAIN_RELAY,
    FN_FUEL_PUMP,
    FN_FUEL_PUMP_2,
    FN_FAN_1,
    FN_FAN_2,
    FN_WATER_PUMP,
    FN_INTERCOOLER_PUMP,
    FN_BOOST_SOLENOID,
    FN_NITROUS,
    /* --- lighting, outputs --- */
    FN_HEADLIGHT_LOW = 32,
    FN_HEADLIGHT_HIGH,
    FN_TAIL,
    FN_BRAKE_LIGHT,
    FN_REVERSE_LIGHT,
    FN_INDICATOR_L,
    FN_INDICATOR_R,
    FN_FOG_FRONT,
    FN_FOG_REAR,
    FN_RAIN_LIGHT,
    FN_INTERIOR_LIGHT,
    FN_WORK_LIGHT,
    /* --- body, outputs --- */
    FN_HORN = 64,
    FN_WIPER,
    FN_WIPER_FAST,
    FN_WASHER,
    FN_HEATED_SCREEN,
    FN_HEATED_SEAT,
    FN_AC_CLUTCH,
    FN_LINE_LOCK,
    /* --- inputs --- */
    FN_IN_BRAKE = 128,  /* brake pedal -- the crank interlock reads this */
    FN_IN_ENGINE_RUN,   /* alternator D+, oil pressure, anything that says "turning" */
    FN_IN_CLUTCH,
    FN_IN_HANDBRAKE,
    FN_IN_REVERSE,
    FN_IN_DOOR,
    FN_IN_BONNET,
    FN_IN_TRACTION_CTL,
    FN_IN_LAUNCH_ARM,
    FN_IN_PIT_LIMITER,
    FN_IN_MAP_SELECT,
    FN_IN_HORN,
    FN_IN_HEADLIGHT,
    FN_IN_INDICATOR_L,
    FN_IN_INDICATOR_R,
    FN_IN_HAZARD,
    FN_IN_WIPER,
    FN_IN_WASHER,
    FN_IN_USER = 250,   /* unlabelled button */
};

struct ch_cfg_t {
    uint8_t  mode;       /* ch_mode_t   */
    uint8_t  flags;      /* CH_F_*      */
    uint8_t  func;       /* ch_func_t -- a label, plus four the firmware looks up */
    uint8_t  behaviour;  /* ch_behaviour_t, outputs only */
    uint16_t param;      /* ms, for OUT_PULSE and OUT_DELAY_OFF */
} __attribute__((packed));

/* ---- persisted configuration ---------------------------------------------- */
#define RCM_CFG_MAGIC    0x314D4352UL   /* "RCM1" little-endian */
/* v2 added the ignition block, v5 the ECU command table. A stored older record is
 * rejected by cfg_valid() and the board falls back to defaults rather than misreading
 * it -- which is the whole reason `version` and `size` are in there. Note `size` alone
 * would catch a struct that GREW; the version is what catches one that changed meaning
 * without changing length. */
#define RCM_CFG_VERSION  5

#define RCM_ECU_CMDS 6

struct ecu_cmd_t {
    uint8_t  ch;          /* input channel whose PRESS sends it; IGN_CH_NONE = unused */
    uint8_t  _pad;
    uint16_t subsystem;   /* ts_command_e   -- e.g. 20 TS_X14, 22 TS_BENCH_CATEGORY */
    uint16_t index;       /* ts_14_command / bench_mode_e, depending on the subsystem */
};

struct rcm_config_t {
    uint32_t magic;
    uint16_t version;
    uint16_t size;                 /* sizeof(rcm_config_t), so a grown struct is
                                    * detectable rather than silently misread */

    uint32_t can_bitrate;          /* Hz. Arbitrary -- the bit timing is solved for
                                    * at runtime, not looked up in a table. */
    uint16_t can_base_id;          /* start of this board's 16-ID block; the node
                                    * address is added on top. See protocol.h. */
    uint16_t broadcast_ms;         /* how often state/input frames go out          */
    uint16_t can_timeout_ms;       /* silence before failsafe; 0 disables          */
    uint16_t input_debounce_ms;
    uint16_t output_settle_ms;     /* ignore sense this long after switching       */
    uint16_t fault_confirm_ms;     /* how long a fault must persist to be reported */
    uint16_t imu_rate_ms;

    struct ch_cfg_t ch[RCM_CHANNELS];
    uint32_t failsafe_state;       /* channel bits to apply when the bus goes quiet */

    /* Peer mirroring: the shortest path from a keypad to a relay without an ECU in
     * the middle. A relay module set to follow node N applies that node's debounced
     * input bits straight to its own outputs, channel for channel. Anything outside
     * peer_mask is left alone, so a board can mirror some channels and still take
     * CAN commands for the rest. */
    uint8_t  peer_node;            /* 0..7, or PEER_NONE to disable */
    uint8_t  _pad;
    uint32_t peer_mask;            /* channels that follow the peer at all */
    uint32_t peer_toggle_mask;     /* of those, ones where a PRESS toggles rather
                                    * than follows -- momentary button, latching load */

    /* IMU axis remap. imu_map[i] says which SENSOR axis feeds vehicle axis i
     * (0=X 1=Y 2=Z), with bit 7 set to negate it. Vehicle axes are the automotive
     * convention: X forward, Y left, Z up. This exists because the board does not
     * always get to be mounted flat and forward -- a keypad in a door card is the
     * obvious case. Default is identity, which is only right if the board is lying
     * flat with its +X edge pointing down the car. */
    uint8_t  imu_map[3];

    /* --- ignition (see ignition.h) ---
     * ign_mode picks between a level (a key, or a maintained switch) and a push
     * button. The three channel numbers are only consulted in momentary mode and are
     * IGN_CH_NONE when unused.
     *
     * ign_start_ch is the one to think twice about: it drives a starter solenoid, and
     * an unintended crank is dangerous. Cranking is refused outright unless
     * ign_brake_ch is also configured, so the dangerous action needs two deliberate
     * settings rather than one. */
    uint8_t  ign_mode;
    uint8_t  ign_brake_ch;      /* CH_INPUT channel, brake pressed = high */
    uint8_t  ign_start_ch;      /* CH_OUTPUT channel driving the starter relay */
    uint8_t  ign_run_ch;        /* CH_INPUT channel, engine running (alternator D+,
                                 * oil pressure switch...). NONE = no running signal,
                                 * in which case cranking follows the button. */
    uint16_t ign_hold_stop_ms;  /* how long to hold the button to stop the engine */
    uint16_t ign_crank_max_ms;  /* give up cranking after this */
    uint16_t ign_off_hold_ms;   /* maintained mode: ignition low this long -> shut down */

    /* The key's RUN position, as an output. Whatever feeds the ECU's ignition input
     * goes here; it is energised the whole time the board is awake and dropped first
     * on shutdown, so the ECU sees ignition-off and can park itself properly. */
    uint8_t  ign_run_out_ch;
    uint16_t ign_shutdown_ms;   /* how long to stay powered after dropping RUN, so the
                                 * ECU can finish its own shutdown before the rail goes */

    /* Momentary mode only: shut down after this long awake with the engine not
     * running and nothing happening. Real keyless cars drop out of accessory after a
     * few minutes for exactly this reason -- a board left awake draws ~100mA plus
     * whatever channels are on, which is a flat battery by morning. 0 disables.
     *
     * Deliberately NOT applied in maintained mode: there the switch is physically
     * closed, so a shutdown could not complete anyway (dropping LATCH_HOLD with the
     * switch on just power-cycles) and the board would sit awake with every channel
     * off, which is worse than leaving it alone. */
    /* Buttons that ask the ECU to do something, over CAN.
     *
     * Deliberately a generic (subsystem, index) pair rather than a list of named
     * commands: EVERY TunerStudio command is that shape -- see the cmd_* lines in
     * rusefi's tunerstudio.template.ini, all of which are TS_IO_TEST_COMMAND followed
     * by two 16-bit words. Storing the pair means a new command needs a config change,
     * not a firmware release, and it reaches things this board has never heard of.
     * The friendly names live in rcm_bench.py, where they cost nothing.
     *
     * LUA_COMMAND_1..4 (subsystem 22, index 33..36) are the interesting ones for a car:
     * they bump a counter a rusEFI Lua script can watch, which is how you get traction
     * control, launch control, a map switch or anything else rusEFI has no fixed
     * command for. */
    struct ecu_cmd_t ecu_cmd[RCM_ECU_CMDS];

    /* What the IGNITION button itself asks the ECU for, on top of powering the board.
     *
     * Set both for a one-button car, the VW arrangement: the same button wakes it,
     * starts it and stops it. Set NEITHER for a two-button car, where the ignition
     * button only ever powers the board and a separate button in ecu_cmd[] does the
     * starting -- which is the simpler thing to reason about, and the default. */
#define IGN_ECU_START_ON_BRAKE  0x01  /* press with the brake held -> ask ECU to start */
#define IGN_ECU_STOP_ON_HOLD    0x02  /* hold while running -> ask ECU to stop, then
                                       * power down after ign_shutdown_ms as usual */
    uint8_t  ign_ecu_flags;
    uint8_t  _pad_ign_ecu;

    /* How long the WAKE press must be held, with the brake down, before it also asks the
     * ECU to start. Only consulted when IGN_ECU_START_ON_BRAKE is set. Note rusEFI has
     * its own startButtonSuppressOnStartUpMs, which exists for exactly this arrangement
     * -- a start button combined with an ECU power-source button -- so set that shorter
     * than this or the request lands while the ECU is still ignoring them. */
    uint16_t ign_wake_start_ms;
    uint16_t ign_idle_timeout_s;

    /* Engine-running from the bus instead of (or as well as) a wired signal.
     * rusEFI's verbose broadcast puts RPM in the low 16 bits of base+1, so 0x201 with
     * the stock base. 0 disables.
     *
     * Read the note in ignition.h before relying on this for the crank interlock: a
     * wired signal keeps working when CAN does not, and the interlock is the one place
     * that matters. */
    uint16_t ecu_rpm_can_id;
    uint16_t ign_run_rpm;       /* at or above this, the engine is running */

    /* Shared so every flashing channel is in phase. 800ms is 75 flashes/minute, inside
     * the 60-120 that indicator regulations ask for. */
    uint16_t flash_period_ms;

    uint8_t  reserved[2];
    uint16_t crc;                  /* CRC-16/CCITT over every byte before this */
} __attribute__((packed));

/* ---- straps (read once, at boot) ------------------------------------------ */
struct rcm_straps_t {
    bool    keypad;        /* CFG_ROLE closed  */
    uint8_t address;       /* 0..3 from ADDR0/ADDR1 */
    bool    force_500k;    /* CFG_BAUD closed -> ignore the stored bitrate */
    bool    publish_imu;   /* CFG_IMU_EN closed */
    uint8_t node;          /* 0..7 -- address, with the role as the top bit */
};

extern struct rcm_config_t cfg;
extern struct rcm_straps_t straps;

void     cfg_read_straps(void);
void     cfg_defaults(struct rcm_config_t *c);
void cfg_sync_ign_labels(struct rcm_config_t *c);
bool cfg_ign_owns_channel(const struct rcm_config_t *c, uint8_t ch);
void     cfg_load(void);            /* EEPROM, falling back to defaults */
bool     cfg_save(void);
uint16_t cfg_crc16(const void *data, uint32_t len);
bool     cfg_valid(const struct rcm_config_t *c);

/* The bitrate actually in force, after the CFG_BAUD recovery strap is applied. */
uint32_t cfg_effective_bitrate(void);

#endif /* RCM_CONFIG_H */
