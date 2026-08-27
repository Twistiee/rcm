/*
 * protocol.cpp -- what goes on the bus, and what to do with what comes off it.
 */
#include <Arduino.h>
#include <string.h>
#include "board.h"
#include "canbus.h"
#include "channels.h"
#include "config.h"
#include "ignition.h"
#include "imu.h"
#include "protocol.h"
#include "app.h"

static uint8_t  seq;
static void     install_filters(void);
/* Set when a control frame changes something the hardware filters depend on. Applied
 * after the receive loop has finished draining, never inside it: reconfiguring filter
 * banks while walking the FIFO is asking for the frames still in it to be discarded. */
static bool     filters_dirty;
/* Previous debounced state of each ECU-command button, one bit per slot, so the ECU is
 * asked once per press rather than continuously while a button is held. */
static uint8_t  ecu_prev;

/* Same test ignition.cpp uses: 0xFF (IGN_CH_NONE) and anything past the channel count
 * both mean "not configured". Named differently because the host tests compile these
 * translation units together. */
static inline bool input_ch_ok(uint8_t ch) { return ch < RCM_CHANNELS; }

/* Every TunerStudio command is this shape: the magic byte, then subsystem and index as
 * 16-bit little-endian words, in an EXTENDED frame. See processCanUserControl() in
 * rusEFI's bench_test.cpp. */
void proto_send_ecu_cmd(uint16_t subsystem, uint16_t index)
{
    const uint8_t d[8] = { RCM_ECU_CMD_MAGIC, 0,
                           (uint8_t)(subsystem & 0xFF), (uint8_t)(subsystem >> 8),
                           (uint8_t)(index     & 0xFF), (uint8_t)(index     >> 8), 0, 0 };
    can_send_ext(RCM_ECU_CMD_ID, d, 8);
}

static void ecu_seed(void)
{
    ecu_prev = 0;
    for (uint8_t i = 0; i < RCM_ECU_CMDS; i++) {
        const uint8_t ch = cfg.ecu_cmd[i].ch;
        if (input_ch_ok(ch) && ((ch_inputs() >> ch) & 1u)) ecu_prev |= (uint8_t)(1u << i);
    }
}
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

/* Bit for the starter channel, or 0 if none is configured. */
static inline uint32_t starter_mask(void)
{
    return cfg.ign_start_ch < RCM_CHANNELS ? (1ul << cfg.ign_start_ch) : 0ul;
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
    filters_dirty = false;
    /* Seed from the CURRENT button states, not from zero. Otherwise a board that boots
     * with a start button held -- or shorted -- sees a rising edge on its first tick and
     * asks the ECU to crank before anyone touched anything. */
    ecu_seed();
    peer_prev = 0;
    peer_seen = false;

    /* Hardware filters, not software ones. bxCAN has 28 banks and a running engine
     * bus is busy enough that letting every frame through would mean waking up for
     * traffic we have no interest in.
     *
     * The node block is 16 consecutive ids starting at a 16-aligned base, so one
     * mask filter covers the lot -- provided can_base_id really is 16-aligned,
     * which is why proto_sanitise_base() forces it. */
    install_filters();
}

/* Separated from proto_begin() because the filter set is no longer fixed for the life
 * of the board: SET_RUN_SRC changes which ECU frame we listen to. Always rebuilt from
 * bank 0 rather than appended to -- see can_filters_reset(). */
static void install_filters(void)
{
    can_filters_reset();
    can_filter_block(proto_node_base(), 0x7F0);
    can_filter_id(proto_global_id());
    if (cfg.peer_node != PEER_NONE)
        can_filter_id((uint16_t)(cfg.can_base_id + cfg.peer_node * RCM_CAN_NODE_STRIDE
                                 + RCM_F_INPUTS));
    /* rusEFI's verbose broadcast, for engine speed. */
    if (cfg.ecu_rpm_can_id) can_filter_id(cfg.ecu_rpm_can_id);
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

static void send_cfg_reply(uint8_t sel, uint8_t idx)
{
    uint8_t d[8] = { sel, idx, 0, 0, 0, 0, 0, 0 };
    uint8_t *p = &d[2];

    switch (sel) {
    case RCM_CFG_SEL_IDS:
        p[0] = (uint8_t)cfg.can_base_id;       p[1] = (uint8_t)(cfg.can_base_id >> 8);
        p[2] = (uint8_t)cfg.can_bitrate;       p[3] = (uint8_t)(cfg.can_bitrate >> 8);
        p[4] = (uint8_t)(cfg.can_bitrate >> 16); p[5] = (uint8_t)(cfg.can_bitrate >> 24);
        break;
    case RCM_CFG_SEL_TIMING:
        p[0] = (uint8_t)cfg.broadcast_ms;      p[1] = (uint8_t)(cfg.broadcast_ms >> 8);
        p[2] = (uint8_t)cfg.can_timeout_ms;    p[3] = (uint8_t)(cfg.can_timeout_ms >> 8);
        p[4] = (uint8_t)cfg.input_debounce_ms; p[5] = (uint8_t)(cfg.input_debounce_ms >> 8);
        break;
    case RCM_CFG_SEL_FAILSAFE:
        pack21(p, cfg.failsafe_state);
        break;
    case RCM_CFG_SEL_IGN:
        p[0] = cfg.ign_mode;    p[1] = cfg.ign_brake_ch; p[2] = cfg.ign_start_ch;
        p[3] = cfg.ign_run_ch;  p[4] = cfg.ign_run_out_ch; p[5] = cfg.ign_ecu_flags;
        break;
    case RCM_CFG_SEL_IGNTIME:
        p[0] = (uint8_t)cfg.ign_hold_stop_ms;  p[1] = (uint8_t)(cfg.ign_hold_stop_ms >> 8);
        p[2] = (uint8_t)cfg.ign_crank_max_ms;  p[3] = (uint8_t)(cfg.ign_crank_max_ms >> 8);
        p[4] = (uint8_t)cfg.ign_shutdown_ms;   p[5] = (uint8_t)(cfg.ign_shutdown_ms >> 8);
        break;
    case RCM_CFG_SEL_IGNTIME2:
        p[0] = (uint8_t)cfg.ign_off_hold_ms;   p[1] = (uint8_t)(cfg.ign_off_hold_ms >> 8);
        p[2] = (uint8_t)cfg.ign_idle_timeout_s;p[3] = (uint8_t)(cfg.ign_idle_timeout_s >> 8);
        p[4] = (uint8_t)cfg.ign_wake_start_ms; p[5] = (uint8_t)(cfg.ign_wake_start_ms >> 8);
        break;
    case RCM_CFG_SEL_PEER:
        if (idx == 0) { p[0] = cfg.peer_node; pack21(&p[1], cfg.peer_mask); }
        else          { pack21(p, cfg.peer_toggle_mask); }
        break;
    case RCM_CFG_SEL_RUNSRC:
        p[0] = (uint8_t)cfg.ecu_rpm_can_id;    p[1] = (uint8_t)(cfg.ecu_rpm_can_id >> 8);
        p[2] = (uint8_t)cfg.ign_run_rpm;       p[3] = (uint8_t)(cfg.ign_run_rpm >> 8);
        break;
    case RCM_CFG_SEL_ECUCMD:
        if (idx >= RCM_ECU_CMDS) return;
        p[0] = cfg.ecu_cmd[idx].ch;
        p[1] = (uint8_t)cfg.ecu_cmd[idx].subsystem;
        p[2] = (uint8_t)(cfg.ecu_cmd[idx].subsystem >> 8);
        p[3] = (uint8_t)cfg.ecu_cmd[idx].index;
        p[4] = (uint8_t)(cfg.ecu_cmd[idx].index >> 8);
        break;
    case RCM_CFG_SEL_CHANNEL:
        if (idx >= RCM_CHANNELS) return;
        p[0] = cfg.ch[idx].mode;      p[1] = cfg.ch[idx].flags;
        p[2] = cfg.ch[idx].func;      p[3] = cfg.ch[idx].behaviour;
        p[4] = (uint8_t)cfg.ch[idx].param; p[5] = (uint8_t)(cfg.ch[idx].param >> 8);
        break;
    default:
        return;                       /* unknown selector: say nothing at all */
    }
    can_send(proto_id(RCM_F_CFG_REPLY), d, 8);
}

static void handle_ctl(const struct can_frame_t *f, bool global)
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

    case RCM_OP_SET_CH_FUNC:
        if (f->len >= 4 && f->data[1] < RCM_CHANNELS) {
            const uint8_t ch  = f->data[1];
            const uint8_t beh = f->data[3];
            /* The behaviour byte means different things for an input and an output, so
             * it is validated against the channel's MODE. Accepting an output behaviour
             * on an input would leave a channel claiming to flash while reporting a
             * latch, which nothing downstream could make sense of. */
            const uint8_t max = (cfg.ch[ch].mode == CH_INPUT) ? IN_HOLD_ARM
                                                              : OUT_DELAY_OFF;
            if (beh <= max) {
                cfg.ch[ch].func      = f->data[2];
                cfg.ch[ch].behaviour = beh;
                if (f->len >= 6)
                    cfg.ch[ch].param = (uint16_t)(f->data[4] | (f->data[5] << 8));
            }
        }
        break;

    case RCM_OP_GET_CFG:
        /* Never answer a GLOBAL request. Eight nodes replying at once would collide,
         * and the asker cannot tell whose answer it got anyway. */
        if (!global && f->len >= 2) send_cfg_reply(f->data[1], f->len >= 3 ? f->data[2] : 0);
        break;

    case RCM_OP_SET_ECU_CMD:
        if (f->len >= 7 && f->data[1] < RCM_ECU_CMDS
            && (f->data[2] < RCM_CHANNELS || f->data[2] == IGN_CH_NONE)) {
            struct ecu_cmd_t *c = &cfg.ecu_cmd[f->data[1]];
            c->ch        = f->data[2];
            c->subsystem = (uint16_t)(f->data[3] | (f->data[4] << 8));
            c->index     = (uint16_t)(f->data[5] | (f->data[6] << 8));
            ecu_seed();          /* re-seed, for the same reason proto_begin() does */
        }
        break;

    case RCM_OP_SET_PEER:
        /* 0xFF disables mirroring; anything else must be a real node. A typo that
         * pointed this at a node which does not exist would leave the board waiting
         * for a keypad that never speaks, which looks exactly like a wiring fault. */
        if (f->len >= 8 && (f->data[1] < 8 || f->data[1] == PEER_NONE)) {
            cfg.peer_node        = f->data[1];
            cfg.peer_mask        = unpack21(&f->data[2]);
            cfg.peer_toggle_mask = unpack21(&f->data[5]);
            /* Forget the old peer's button state. Carrying it across would compare the
             * new peer's first frame against whatever the old one last sent, and every
             * toggle channel whose bit differs would fire on arrival. */
            peer_prev = 0;
            peer_seen = false;
            filters_dirty = true;          /* the peer's INPUTS id needs a filter */
        }
        break;

    case RCM_OP_SET_RUN_SRC:
        /* An id of 0 legitimately means "no CAN run source". Anything at or above
         * 0x800 is not a standard 11-bit id and would be silently truncated into
         * somebody else's traffic, so it is refused rather than masked. */
        if (f->len >= 5) {
            const uint16_t id  = (uint16_t)(f->data[1] | (f->data[2] << 8));
            const uint16_t rpm = (uint16_t)(f->data[3] | (f->data[4] << 8));
            if (id < 0x800) {
                cfg.ecu_rpm_can_id = id;
                if (rpm) cfg.ign_run_rpm = rpm;
                /* The id is useless without a filter to let it through. */
                filters_dirty = true;
            }
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

    case RCM_OP_SET_IGNITION:
        if (f->len >= 5 && f->data[1] <= IGN_MOMENTARY) {
            /* Channel numbers are only accepted if they are real channels or the
             * explicit "none" -- a typo must not silently point the starter at
             * whatever channel happens to share the low bits. */
            const uint8_t b = f->data[2], s = f->data[3], r = f->data[4];
            if ((b < RCM_CHANNELS || b == IGN_CH_NONE)
             && (s < RCM_CHANNELS || s == IGN_CH_NONE)
             && (r < RCM_CHANNELS || r == IGN_CH_NONE)) {
                cfg.ign_mode     = f->data[1];
                cfg.ign_brake_ch = b;
                cfg.ign_start_ch = s;
                cfg.ign_run_ch   = r;
                if (f->len >= 6) {
                    const uint8_t o = f->data[5];
                    if (o < RCM_CHANNELS || o == IGN_CH_NONE) cfg.ign_run_out_ch = o;
                }
                /* Reconsider the ignition from scratch under the new settings rather
                 * than carrying a state that was reached under the old ones. */
                ign_begin(app_ignition_on());
            }
        }
        break;

    case RCM_OP_SET_IGN_TIMES:
        if (f->len >= 5) {
            const uint16_t hold  = (uint16_t)(f->data[1] | (f->data[2] << 8));
            const uint16_t crank = (uint16_t)(f->data[3] | (f->data[4] << 8));
            /* A zero hold time would mean the lightest touch stops the engine, and an
             * unbounded crank cooks the starter. */
            if (hold >= 200 && hold <= 10000)  cfg.ign_hold_stop_ms = hold;
            if (crank >= 500 && crank <= 30000) cfg.ign_crank_max_ms = crank;
            if (f->len >= 7) {
                const uint16_t sd = (uint16_t)(f->data[5] | (f->data[6] << 8));
                if (sd <= 30000) cfg.ign_shutdown_ms = sd;   /* 0 = cut immediately */
            }
        }
        break;

    default:
        break;
    }
}

static void handle_peer_inputs(const struct can_frame_t *f)
{
    if (f->len < 3) return;
    const uint32_t in = unpack21(f->data) & cfg.peer_mask & ~starter_mask();

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
            if (rising & bit) ch_command(ch, !((ch_requested() >> ch) & 1u));
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

        if (cfg.ecu_rpm_can_id && f.id == cfg.ecu_rpm_can_id) {
            /* rusEFI base+1: RPM is the low 16 bits, little-endian, 1 rpm per count.
             * Deliberately NOT counted as traffic addressed to us -- the ECU shouting
             * at the dash says nothing about whether anyone is commanding this board,
             * so it must not hold off the failsafe. */
            if (f.len >= 2) ign_note_rpm((uint16_t)(f.data[0] | (f.data[1] << 8)), now_ms);
            continue;
        }

        if (f.id == peer) {
            handle_peer_inputs(&f);
        } else if (f.id == global) {
            handle_ctl(&f, true);
        } else if (f.id == (uint16_t)(base + RCM_F_CMD_SET)) {
            /* The starter is masked out of anything arriving over the bus. Only the
             * ignition state machine turns it, and it does so with the brake held and
             * the engine confirmed stopped -- conditions a remote frame cannot know
             * about. A stray or replayed CMD_SET must never crank the engine. */
            if (f.len >= 6) ch_command_mask(unpack21(&f.data[0]) & ~starter_mask(),
                                            unpack21(&f.data[3]));
        } else if (f.id == (uint16_t)(base + RCM_F_CMD_CTL)) {
            handle_ctl(&f, false);
        } else {
            ours = false;
        }

        /* Only traffic addressed to us counts as the bus being alive. Hearing the
         * ECU chatter to the dash says nothing about whether anyone is still
         * commanding this board. */
        if (ours) {
            last_rx_ms = now_ms;
            failsafe_active = false;
            /* Somebody is still talking to this board, so it is not idle. */
            ign_note_activity(now_ms);
        }
    }

    /* Ask the ECU to do things, once per press.
     *
     * Deliberately ungated. rusEFI owns these decisions: for start/stop it has RPM
     * straight off the crank sensor, its own crank timeout and its own interlocks, and
     * it uses the very same entry point as its physical start/stop button. A second
     * opinion here -- about whether the engine is running, or whether the brake is down
     * -- could only ever disagree with the half that can actually see the engine. */
    for (uint8_t i = 0; i < RCM_ECU_CMDS; i++) {
        const struct ecu_cmd_t *c = &cfg.ecu_cmd[i];
        if (!input_ch_ok(c->ch)) { ecu_prev &= (uint8_t)~(1u << i); continue; }
        const bool pressed = (ch_inputs() >> c->ch) & 1u;
        if (pressed && !((ecu_prev >> i) & 1u)) proto_send_ecu_cmd(c->subsystem, c->index);
        if (pressed) ecu_prev |= (uint8_t)(1u << i);
        else         ecu_prev &= (uint8_t)~(1u << i);
    }

    if (filters_dirty) {
        filters_dirty = false;
        install_filters();
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
