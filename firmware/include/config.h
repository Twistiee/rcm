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

struct ch_cfg_t {
    uint8_t mode;
    uint8_t flags;
} __attribute__((packed));

/* ---- persisted configuration ---------------------------------------------- */
#define RCM_CFG_MAGIC    0x314D4352UL   /* "RCM1" little-endian */
/* v2 added the ignition block. A stored v1 record is rejected by cfg_valid() and the
 * board falls back to defaults rather than misreading it -- which is the whole reason
 * `version` and `size` are in there. */
#define RCM_CFG_VERSION  2

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
    uint16_t ign_idle_timeout_s;

    uint8_t  reserved[3];
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
void     cfg_load(void);            /* EEPROM, falling back to defaults */
bool     cfg_save(void);
uint16_t cfg_crc16(const void *data, uint32_t len);
bool     cfg_valid(const struct rcm_config_t *c);

/* The bitrate actually in force, after the CFG_BAUD recovery strap is applied. */
uint32_t cfg_effective_bitrate(void);

#endif /* RCM_CONFIG_H */
