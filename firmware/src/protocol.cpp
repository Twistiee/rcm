/*
 * protocol.cpp -- what goes on the bus, and what to do with what comes off it.
 */
#include <Arduino.h>
#include <string.h>
#include "board.h"
#include "canbus.h"
#include "channels.h"
#include "config.h"
#include "imu.h"
#include "protocol.h"
#include "app.h"

static uint8_t  seq;
static uint32_t last_rx_ms;
static bool     failsafe_active;
static bool     reboot_pending;

/* Peer mirroring keeps an edge memory so toggle channels fire once per press
 * rather than continuously while the button is held. */
static uint32_t peer_prev;
static bool     peer_seen;

uint16_t proto_node_base(void)
{
    return (uint16_t)(cfg.can_base_id + straps.node * RCM_CAN_NODE_STRIDE);
}

uint16_t proto_id(uint8_t frame_offset)
{
    return (uint16_t)(proto_node_base() + frame_offset);
}

uint16_t proto_global_id(void)
{
    return (uint16_t)(cfg.can_base_id + RCM_CAN_GLOBAL_OFFSET);
}

/* 21 channel bits into 3 little-endian bytes: channel 1 is byte 0 bit 0. */
static inline void pack21(uint8_t *d, uint32_t v)
{
    d[0] = (uint8_t)(v);
    d[1] = (uint8_t)(v >> 8);
    d[2] = (uint8_t)(v >> 16) & 0x1F;
}

static inline uint32_t unpack21(const uint8_t *d)
{
    return ((uint32_t)d[0]) | ((uint32_t)d[1] << 8) | (((uint32_t)d[2] & 0x1F) << 16);
}

void proto_begin(void)
{
    seq = 0;
    last_rx_ms = millis();
    failsafe_active = false;
    reboot_pending = false;
    peer_prev = 0;
    peer_seen = false;

    /* Hardware filters, not software ones. bxCAN has 28 banks and a running engine
     * bus is busy enough that letting every frame through would mean waking up for
     * traffic we have no interest in.
     *
     * The node block is 16 consecutive ids starting at a 16-aligned base, so one
     * mask filter covers the lot -- provided can_base_id really is 16-aligned,
     * which is why proto_sanitise_base() forces it. */
    can_filter_block(proto_node_base(), 0x7F0);
    can_filter_id(proto_global_id());
    if (cfg.peer_node != PEER_NONE)
        can_filter_id((uint16_t)(cfg.can_base_id + cfg.peer_node * RCM_CAN_NODE_STRIDE
                                 + RCM_F_INPUTS));
}

void proto_sanitise_base(void)
{
    /* A base id that is not 16-aligned would make the block filter above match the
     * wrong range, and 11-bit ids stop at 0x7FF. Both are things a fat-fingered
     * SET_BASE_ID could do, and the failure mode -- a node that hears nothing --
     * is not one you can undo over the bus. */
    if (cfg.can_base_id & 0x0F) cfg.can_base_id &= (uint16_t)~0x0Fu;
    if (cfg.can_base_id < 0x010 || (cfg.can_base_id + RCM_CAN_GLOBAL_OFFSET) > 0x7FF)
        cfg.can_base_id = RCM_CAN_BASE_DEFAULT;
}

/* --- transmit -------------------------------------------------------------- */

static uint8_t status_flags(void)
{
    uint8_t f = 0;
    if (app_outputs_live())                     f |= RCM_ST_OUT_ENABLED;
    if (failsafe_active)                        f |= RCM_ST_FAILSAFE;
    if (ch_fault_open() || ch_fault_short())    f |= RCM_ST_ANY_FAULT;
    if (imu_ok())                               f |= RCM_ST_IMU_OK;
    if (app_eeprom_ok())                        f |= RCM_ST_EEPROM_OK;
    if (straps.keypad)                          f |= RCM_ST_ROLE_KEYPAD;
    if (app_ignition_on())                      f |= RCM_ST_IGN_ON;
    if (straps.force_500k)                      f |= RCM_ST_FORCED_500K;
    return f;
}

void proto_broadcast(uint32_t now_ms)
{
    uint8_t d[8];
    const uint16_t up_s = (uint16_t)(now_ms / 1000UL);

    memset(d, 0, sizeof(d));
    pack21(d, ch_commanded());
    d[3] = status_flags();
    d[4] = (uint8_t)up_s;
    d[5] = (uint8_t)(up_s >> 8);
    d[6] = straps.node;
    d[7] = seq;
    can_send(proto_id(RCM_F_OUTPUTS), d, 8);

    memset(d, 0, sizeof(d));
    pack21(d, ch_inputs());
    d[3] = ch_aux();
    /* Bytes 4-6 are the RAW sense bits for every channel, not just the ones
     * configured as inputs. It costs nothing and it is the single most useful thing
     * to have on the bus when something is wired wrong -- you can see what the pin
     * is doing without agreeing with the firmware about what the pin is for. */
    d[4] = (uint8_t)(ch_sense_raw());
    d[5] = (uint8_t)(ch_sense_raw() >> 8);
    d[6] = (uint8_t)((ch_sense_raw() >> 16) & 0x1F);
    d[7] = seq;
    can_send(proto_id(RCM_F_INPUTS), d, 8);

    memset(d, 0, sizeof(d));
    pack21(d, ch_fault_open());
    d[3] = (uint8_t)(ch_fault_short());
    d[4] = (uint8_t)(ch_fault_short() >> 8);
    d[5] = (uint8_t)((ch_fault_short() >> 16) & 0x1F);
    d[6] = 0;
    d[7] = seq;
    can_send(proto_id(RCM_F_FAULTS), d, 8);

    memset(d, 0, sizeof(d));
    d[0] = RCM_FW_MAJOR;
    d[1] = RCM_FW_MINOR;
    d[2] = RCM_FW_PATCH;
    d[3] = (uint8_t)(can_bus_off() ? 1 : 0);
    const uint16_t ign_mv = app_ignition_mv();
    d[4] = (uint8_t)ign_mv;
    d[5] = (uint8_t)(ign_mv >> 8);
    d[6] = can_rx_errors();
    d[7] = can_tx_errors();
    can_send(proto_id(RCM_F_STATUS), d, 8);

    seq++;
}

/* --- receive --------------------------------------------------------------- */

static void handle_ctl(const struct can_frame_t *f)
{
    if (f->len < 1) return;

    switch (f->data[0]) {
    case RCM_OP_ALL_OFF:
        ch_all_off();
        break;

    case RCM_OP_OUTPUTS_ENABLE:
        if (f->len >= 2) app_set_outputs_live(f->data[1] != 0);
        break;

    case RCM_OP_CLEAR_FAULTS:
        ch_clear_faults();
        break;

    case RCM_OP_SAVE_CONFIG:
        cfg_save();
        break;

    case RCM_OP_LOAD_DEFAULTS:
        /* RAM only. Somebody has to send SAVE_CONFIG as a second, deliberate act
         * before this survives a power cycle. */
        cfg_defaults(&cfg);
        break;

    case RCM_OP_REBOOT:
        /* The magic byte is not paranoia -- a stray frame that rebooted the relay
         * module would drop every load on the car. */
        if (f->len >= 2 && f->data[1] == 0xA5) reboot_pending = true;
        break;

    case RCM_OP_SET_CH_MODE:
        if (f->len >= 4 && f->data[1] < RCM_CHANNELS && f->data[2] <= CH_INPUT) {
            const uint8_t ch = f->data[1];
            /* Changing a channel's mode with it energised would leave the driver on
             * with nothing willing to turn it off again. */
            ch_command(ch, false);
            cfg.ch[ch].mode  = f->data[2];
            cfg.ch[ch].flags = f->data[3];
        }
        break;

    case RCM_OP_SET_BITRATE:
        if (f->len >= 5) {
            cfg.can_bitrate = ((uint32_t)f->data[1]) | ((uint32_t)f->data[2] << 8)
                            | ((uint32_t)f->data[3] << 16) | ((uint32_t)f->data[4] << 24);
        }
        break;

    case RCM_OP_SET_BASE_ID:
        if (f->len >= 3) {
            cfg.can_base_id = (uint16_t)(f->data[1] | (f->data[2] << 8));
            proto_sanitise_base();
        }
        break;

    case RCM_OP_SET_TIMING:
        if (f->len >= 5) {
            uint16_t b = (uint16_t)(f->data[1] | (f->data[2] << 8));
            uint16_t t = (uint16_t)(f->data[3] | (f->data[4] << 8));
            if (b >= 10 && b <= 5000) cfg.broadcast_ms = b;
            cfg.can_timeout_ms = t;          /* 0 legitimately means "never" */
        }
        break;

    case RCM_OP_SET_FAILSAFE:
        if (f->len >= 4) cfg.failsafe_state = unpack21(&f->data[1]);
        break;

    default:
        break;
    }
}

static void handle_peer_inputs(const struct can_frame_t *f)
{
    if (f->len < 3) return;
    const uint32_t in = unpack21(f->data) & cfg.peer_mask;

    if (!peer_seen) {
        /* First frame from the peer only establishes a baseline. Acting on it would
         * fire every toggle channel whose button happened to be held at the moment
         * this board came up. */
        peer_prev = in;
        peer_seen = true;
        return;
    }

    const uint32_t rising = in & ~peer_prev;
    for (uint8_t ch = 0; ch < RCM_CHANNELS; ch++) {
        const uint32_t bit = 1ul << ch;
        if (!(cfg.peer_mask & bit)) continue;
        if (cfg.peer_toggle_mask & bit) {
            if (rising & bit) ch_command(ch, !((ch_commanded() >> ch) & 1u));
        } else {
            ch_command(ch, (in & bit) != 0);
        }
    }
    peer_prev = in;
}

void proto_poll(uint32_t now_ms)
{
    struct can_frame_t f;
    const uint16_t base   = proto_node_base();
    const uint16_t global = proto_global_id();
    const uint16_t peer   = (cfg.peer_node == PEER_NONE) ? 0xFFFF
                          : (uint16_t)(cfg.can_base_id + cfg.peer_node * RCM_CAN_NODE_STRIDE
                                       + RCM_F_INPUTS);

    /* Drain the whole FIFO. It is only three deep, and leaving frames in it means
     * commands arrive a tick late for no reason. */
    while (can_recv(&f)) {
        bool ours = true;

        if (f.id == peer) {
            handle_peer_inputs(&f);
        } else if (f.id == global) {
            handle_ctl(&f);
        } else if (f.id == (uint16_t)(base + RCM_F_CMD_SET)) {
            if (f.len >= 6) ch_command_mask(unpack21(&f.data[0]), unpack21(&f.data[3]));
        } else if (f.id == (uint16_t)(base + RCM_F_CMD_CTL)) {
            handle_ctl(&f);
        } else {
            ours = false;
        }

        /* Only traffic addressed to us counts as the bus being alive. Hearing the
         * ECU chatter to the dash says nothing about whether anyone is still
         * commanding this board. */
        if (ours) {
            last_rx_ms = now_ms;
            failsafe_active = false;
        }
    }

    /* --- bus timeout ---
     * Only outputs that are actually commanded by CAN need a failsafe. A board doing
     * nothing but peer mirroring, or one deliberately configured with no timeout,
     * is left alone. */
    if (cfg.can_timeout_ms && !failsafe_active
        && (now_ms - last_rx_ms) > cfg.can_timeout_ms) {
        failsafe_active = true;
        ch_apply_failsafe();
    }

    if (reboot_pending) {
        /* Cleared before the reset, not after. NVIC_SystemReset() does not return on
         * real hardware, so this looks pointless -- but if it ever did (a debugger
         * holding the core, a future soft-reset path) a latched flag would put us in
         * a reset loop with every channel dropping each time round. */
        reboot_pending = false;
        ch_all_off();
        delay(20);            /* let the last shift-register frame get latched out */
        NVIC_SystemReset();
    }
}

bool proto_failsafe(void)   { return failsafe_active; }
uint32_t proto_last_rx(void) { return last_rx_ms; }
