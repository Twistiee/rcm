/*
 * test_protocol -- the CAN application layer, wired to the real channel stack.
 *
 * Only the transceiver and the IMU are faked. Commands arriving on the simulated bus
 * go all the way through to a modelled relay coil, and broadcasts are decoded back out
 * of the actual bytes rather than from the firmware's internal state -- so a packing
 * mistake shows up here rather than on a dash.
 */
#include <unity.h>
#include <cstdio>
#include <vector>

#include "../stubs/simboard.cpp"
#include "../../src/shiftreg.cpp"

/* config.cpp owns the cfg and straps globals, so it has to come before anything that
 * reads them -- and the test must NOT declare its own copies, or the config functions
 * would end up updating an object nobody else can see. */
#include "../../src/store.cpp"
#include "../../src/config.cpp"
#include "../../src/channels.cpp"
#include "../../src/ignition.cpp"

/* --- fakes ----------------------------------------------------------------- */

#include "canbus.h"
#include "app.h"
#include "imu.h"

static std::vector<can_frame_t> TX;
static std::vector<can_frame_t> RX;
static std::vector<uint16_t>    FILTERS;
static bool   FAKE_OUTPUTS_LIVE = true;
static int    RESETS = 0;

/* Extended frames are modelled separately because can_frame_t cannot hold a 29-bit id
 * -- which is the point: forgetting the extended flag has to be a visible failure, not
 * a silently truncated id that happens to look plausible. */
struct ext_frame_t { uint32_t id; bool ext; uint8_t len; uint8_t data[8]; };
static std::vector<ext_frame_t> EXT_TX;

bool can_send_frame(uint32_t id, bool ext, const uint8_t *d, uint8_t len)
{
    ext_frame_t e = {}; e.id = id; e.ext = ext; e.len = len;
    for (uint8_t i = 0; i < len && i < 8; i++) e.data[i] = d[i];
    EXT_TX.push_back(e);
    if (!ext) {
        can_frame_t f = {}; f.id = (uint16_t)id; f.len = len;
        for (uint8_t i = 0; i < len && i < 8; i++) f.data[i] = d[i];
        TX.push_back(f);
    }
    return true;
}
bool can_send(uint16_t id, const uint8_t *d, uint8_t len)
{
    return can_send_frame(id, false, d, len);
}
bool can_send_ext(uint32_t id, const uint8_t *d, uint8_t len)
{
    return can_send_frame(id, true, d, len);
}
bool can_recv(can_frame_t *f)
{
    if (RX.empty()) return false;
    *f = RX.front();
    RX.erase(RX.begin());
    return true;
}
/* The real allocator hands out one of 28 hardware banks per call and can be rewound;
 * the model is a list that can be cleared. Modelling the RESET is the point -- it is
 * what stops a changed RPM id from stacking a second filter on top of the old one. */
void can_filters_reset(void)                   { FILTERS.clear(); }
void can_filter_block(uint16_t base, uint16_t) { FILTERS.push_back(base); }
void can_filter_id(uint16_t id)                { FILTERS.push_back(id); }
bool     can_bus_off(void)   { return false; }
uint8_t  can_rx_errors(void) { return 0; }
uint8_t  can_tx_errors(void) { return 0; }

bool imu_ok(void) { return false; }

bool     app_outputs_live(void) { return FAKE_OUTPUTS_LIVE; }
void     app_set_outputs_live(bool v) { FAKE_OUTPUTS_LIVE = v; sr_outputs_enable(v); }
bool     app_eeprom_ok(void)    { return true; }
bool     app_ignition_on(void)  { return true; }
uint16_t app_ignition_mv(void)  { return 12700; }

static void NVIC_SystemReset(void) { RESETS++; }

#include "../../src/protocol.cpp"

/* --- helpers --------------------------------------------------------------- */

#define NODE_BASE (RCM_CAN_BASE_DEFAULT + 0 * RCM_CAN_NODE_STRIDE)
#define PEER_ID   (RCM_CAN_BASE_DEFAULT + 4 * RCM_CAN_NODE_STRIDE + RCM_F_INPUTS)

static void inject(uint16_t id, std::initializer_list<uint8_t> bytes)
{
    can_frame_t f = {}; f.id = id; f.len = (uint8_t)bytes.size();
    uint8_t i = 0;
    for (uint8_t b : bytes) f.data[i++] = b;
    RX.push_back(f);
}

static const can_frame_t *sent(uint16_t id)
{
    for (size_t i = TX.size(); i-- > 0; ) if (TX[i].id == id) return &TX[i];
    return nullptr;
}

static uint32_t get21(const uint8_t *d)
{
    return (uint32_t)d[0] | ((uint32_t)d[1] << 8) | (((uint32_t)d[2] & 0x1F) << 16);
}

static void run_ms(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += TICK_MS) {
        SIM.now_ms += TICK_MS;
        ch_tick(SIM.now_ms);
        proto_poll(SIM.now_ms);
    }
}

void setUp(void)
{
    sim_reset();
    TX.clear(); RX.clear(); FILTERS.clear();
    RESETS = 0;
    FAKE_OUTPUTS_LIVE = true;

    memset(&straps, 0, sizeof(straps));
    cfg_defaults(&cfg);
    cfg.can_timeout_ms = 1000;
    for (uint8_t i = 0; i < RCM_CHANNELS; i++) {
        cfg.ch[i].mode = CH_OUTPUT;
        SIM.wiring[i]  = SIM_COIL_OK;
    }
    sr_begin();
    ch_begin();
    sr_outputs_enable(true);
    proto_begin();
}
void tearDown(void) {}

/* --- addressing ------------------------------------------------------------ */

static void test_ids_are_where_protocol_h_says(void)
{
    TEST_ASSERT_EQUAL_HEX16(0x300, proto_node_base());
    TEST_ASSERT_EQUAL_HEX16(0x300, proto_id(RCM_F_OUTPUTS));
    TEST_ASSERT_EQUAL_HEX16(0x308, proto_id(RCM_F_CMD_SET));
    TEST_ASSERT_EQUAL_HEX16(0x380, proto_global_id());

    straps.node = 7;                       /* keypad at address 3 */
    TEST_ASSERT_EQUAL_HEX16(0x370, proto_node_base());
    /* The whole allocation must stay inside 11-bit space and clear of rusEFI's
     * 0x200..0x20B broadcast, 0x100/0x102, 0x190 and 0x174/0x178/0x17C. */
    TEST_ASSERT_LESS_THAN_UINT16(0x400, proto_global_id());
    TEST_ASSERT_GREATER_THAN_UINT16(0x20B, proto_node_base());
}

static void test_base_id_is_forced_somewhere_workable(void)
{
    /* The node block is matched with one mask filter, which only works if the base
     * is 16-aligned. A bad SET_BASE_ID would otherwise leave a node deaf, and you
     * cannot fix a deaf node over the bus. */
    cfg.can_base_id = 0x305;
    proto_sanitise_base();
    TEST_ASSERT_EQUAL_HEX16(0x300, cfg.can_base_id);

    cfg.can_base_id = 0x7F0;               /* global block would run past 0x7FF */
    proto_sanitise_base();
    TEST_ASSERT_EQUAL_HEX16(RCM_CAN_BASE_DEFAULT, cfg.can_base_id);

    cfg.can_base_id = 0;
    proto_sanitise_base();
    TEST_ASSERT_EQUAL_HEX16(RCM_CAN_BASE_DEFAULT, cfg.can_base_id);
}

static void test_filters_cover_our_block_and_the_global_id(void)
{
    TEST_ASSERT_EQUAL_UINT32(2, FILTERS.size());
    TEST_ASSERT_EQUAL_HEX16(0x300, FILTERS[0]);
    TEST_ASSERT_EQUAL_HEX16(0x380, FILTERS[1]);
}

/* --- broadcast ------------------------------------------------------------- */

static void test_broadcast_sends_all_four_frames(void)
{
    proto_broadcast(SIM.now_ms);
    TEST_ASSERT_EQUAL_UINT32(4, TX.size());
    TEST_ASSERT_NOT_NULL(sent(NODE_BASE + RCM_F_OUTPUTS));
    TEST_ASSERT_NOT_NULL(sent(NODE_BASE + RCM_F_INPUTS));
    TEST_ASSERT_NOT_NULL(sent(NODE_BASE + RCM_F_FAULTS));
    TEST_ASSERT_NOT_NULL(sent(NODE_BASE + RCM_F_STATUS));
}

static void test_output_bits_land_in_the_right_places(void)
{
    /* Decoded out of the raw bytes, not read back from the firmware's own state --
     * channel 1 must be byte 0 bit 0 and channel 21 byte 2 bit 4. */
    ch_command(0, true);
    ch_command(20, true);
    run_ms(TICK_MS * 2);
    TX.clear();
    proto_broadcast(SIM.now_ms);

    const can_frame_t *f = sent(NODE_BASE + RCM_F_OUTPUTS);
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_HEX8(0x01, f->data[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00, f->data[1]);
    TEST_ASSERT_EQUAL_HEX8(0x10, f->data[2]);
    TEST_ASSERT_EQUAL_HEX32((1ul << 0) | (1ul << 20), get21(f->data));
}

static void test_status_flags_and_seq(void)
{
    proto_broadcast(SIM.now_ms);
    const can_frame_t *f = sent(NODE_BASE + RCM_F_OUTPUTS);
    TEST_ASSERT_TRUE(f->data[3] & RCM_ST_OUT_ENABLED);
    TEST_ASSERT_TRUE(f->data[3] & RCM_ST_EEPROM_OK);
    TEST_ASSERT_TRUE(f->data[3] & RCM_ST_IGN_ON);
    TEST_ASSERT_FALSE(f->data[3] & RCM_ST_ROLE_KEYPAD);
    TEST_ASSERT_FALSE(f->data[3] & RCM_ST_ANY_FAULT);
    TEST_ASSERT_EQUAL_UINT8(0, f->data[6]);          /* node id */
    const uint8_t s0 = f->data[7];

    TX.clear();
    proto_broadcast(SIM.now_ms);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(s0 + 1), sent(NODE_BASE + RCM_F_OUTPUTS)->data[7]);
}

static void test_faults_reach_the_bus(void)
{
    SIM.wiring[9] = SIM_COIL_OPEN;
    run_ms(cfg.output_settle_ms + cfg.fault_confirm_ms + 200);
    TX.clear();
    proto_broadcast(SIM.now_ms);

    TEST_ASSERT_EQUAL_HEX32(1ul << 9, get21(sent(NODE_BASE + RCM_F_FAULTS)->data));
    TEST_ASSERT_TRUE(sent(NODE_BASE + RCM_F_OUTPUTS)->data[3] & RCM_ST_ANY_FAULT);
}

static void test_raw_sense_is_published_alongside_logical_inputs(void)
{
    /* Every channel is an output here, so ch_inputs() is empty -- but the raw sense
     * bits still go out, which is what makes a miswired board diagnosable without
     * agreeing with the firmware about what each channel is for. */
    run_ms(100);
    TX.clear();
    proto_broadcast(SIM.now_ms);
    const can_frame_t *f = sent(NODE_BASE + RCM_F_INPUTS);
    TEST_ASSERT_EQUAL_HEX32(0, get21(f->data));
    TEST_ASSERT_EQUAL_HEX32(0x1FFFFF, get21(&f->data[4]));
}

/* --- commands -------------------------------------------------------------- */

static void test_cmd_set_applies_only_masked_channels(void)
{
    ch_command(1, true);
    run_ms(TICK_MS * 2);

    inject(NODE_BASE + RCM_F_CMD_SET, { 0x05, 0x00, 0x00,     /* mask: ch1 and ch3 */
                                        0x04, 0x00, 0x00 });  /* value: ch3 only   */
    run_ms(TICK_MS * 4);

    TEST_ASSERT_FALSE(sim_driver_on(0));
    TEST_ASSERT_TRUE_MESSAGE(sim_driver_on(1), "unmasked channel was disturbed");
    TEST_ASSERT_TRUE(sim_driver_on(2));
}

static void test_cmd_set_reaches_channel_21(void)
{
    inject(NODE_BASE + RCM_F_CMD_SET, { 0x00, 0x00, 0x10, 0x00, 0x00, 0x10 });
    run_ms(TICK_MS * 4);
    TEST_ASSERT_TRUE(sim_driver_on(20));
}

static void test_short_cmd_set_is_ignored(void)
{
    inject(NODE_BASE + RCM_F_CMD_SET, { 0xFF, 0xFF, 0x1F });   /* mask but no values */
    run_ms(TICK_MS * 4);
    TEST_ASSERT_EQUAL_HEX32(0, ch_commanded());
}

static void test_all_off(void)
{
    ch_command_mask(0x1FFFFF, 0x1FFFFF);
    run_ms(TICK_MS * 2);
    inject(NODE_BASE + RCM_F_CMD_CTL, { RCM_OP_ALL_OFF });
    run_ms(TICK_MS * 4);
    TEST_ASSERT_EQUAL_HEX32(0, ch_commanded());
}

static void test_global_ctl_is_obeyed(void)
{
    ch_command_mask(0x1FFFFF, 0x1FFFFF);
    run_ms(TICK_MS * 2);
    inject(proto_global_id(), { RCM_OP_ALL_OFF });
    run_ms(TICK_MS * 4);
    TEST_ASSERT_EQUAL_HEX32(0, ch_commanded());
}

static void test_outputs_can_be_parked_hiz(void)
{
    inject(NODE_BASE + RCM_F_CMD_CTL, { RCM_OP_OUTPUTS_ENABLE, 0 });
    run_ms(TICK_MS * 4);
    TEST_ASSERT_FALSE(FAKE_OUTPUTS_LIVE);
    TEST_ASSERT_FALSE(SIM.oe_low);
}

static void test_set_ch_mode_turns_the_channel_off_first(void)
{
    /* Changing a channel to an input while it is energised would leave the driver on
     * with nothing willing to switch it off again. */
    ch_command(4, true);
    run_ms(TICK_MS * 2);
    TEST_ASSERT_TRUE(sim_driver_on(4));

    inject(NODE_BASE + RCM_F_CMD_CTL, { RCM_OP_SET_CH_MODE, 4, CH_INPUT, CH_F_INVERT });
    run_ms(TICK_MS * 4);

    TEST_ASSERT_EQUAL(CH_INPUT, cfg.ch[4].mode);
    TEST_ASSERT_EQUAL_HEX8(CH_F_INVERT, cfg.ch[4].flags);
    TEST_ASSERT_FALSE_MESSAGE(sim_driver_on(4), "left energised after becoming an input");
}

static void test_set_ch_mode_rejects_nonsense(void)
{
    inject(NODE_BASE + RCM_F_CMD_CTL, { RCM_OP_SET_CH_MODE, 99, CH_INPUT, 0 });
    inject(NODE_BASE + RCM_F_CMD_CTL, { RCM_OP_SET_CH_MODE, 3, 77, 0 });
    run_ms(TICK_MS * 4);
    TEST_ASSERT_EQUAL(CH_OUTPUT, cfg.ch[3].mode);
}

static void test_set_ignition_rejects_a_mode_that_does_not_exist(void)
{
    /* SET_IGNITION carries the mode and the ECU flags, and nothing else. Which channel
     * does which job is said once, by that channel's function label. */
    cfg.ign_mode = IGN_MAINTAINED;
    inject(NODE_BASE + RCM_F_CMD_CTL, { RCM_OP_SET_IGNITION, 7 });
    run_ms(TICK_MS * 4);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(IGN_MAINTAINED, cfg.ign_mode, "a bad mode was applied");

    inject(NODE_BASE + RCM_F_CMD_CTL, { RCM_OP_SET_IGNITION, IGN_MOMENTARY, 0x03 });
    run_ms(TICK_MS * 4);
    TEST_ASSERT_EQUAL_UINT8(IGN_MOMENTARY, cfg.ign_mode);
    TEST_ASSERT_EQUAL_UINT8(0x03, cfg.ign_ecu_flags);
}

static void test_set_ign_times_clamps(void)
{
    /* A zero hold means the lightest touch stops the engine; an unbounded crank cooks
     * the starter. Both are refused rather than clamped silently. */
    cfg.ign_hold_stop_ms = 1000;
    cfg.ign_crank_max_ms = 8000;
    inject(NODE_BASE + RCM_F_CMD_CTL, { RCM_OP_SET_IGN_TIMES, 0, 0, 0, 0 });
    run_ms(TICK_MS * 4);
    TEST_ASSERT_EQUAL_UINT16(1000, cfg.ign_hold_stop_ms);
    TEST_ASSERT_EQUAL_UINT16(8000, cfg.ign_crank_max_ms);

    inject(NODE_BASE + RCM_F_CMD_CTL, { RCM_OP_SET_IGN_TIMES, 0xE8, 0x03, 0xB8, 0x0B });
    run_ms(TICK_MS * 4);
    TEST_ASSERT_EQUAL_UINT16(1000, cfg.ign_hold_stop_ms);
    TEST_ASSERT_EQUAL_UINT16(3000, cfg.ign_crank_max_ms);
}

/* --- the ECU run source ------------------------------------------------------
 * This opcode is what makes "hold to stop a RUNNING engine" mean anything. Without a
 * run source engine_running() is permanently false, the state machine never reaches
 * IGN_ST_RUNNING, and every press -- including an accidental brush -- falls through to
 * the instant-shutdown branch. The field existed and was read in two places; nothing
 * could ever write it. */

static void test_set_run_src_takes_id_and_threshold(void)
{
    cfg.ecu_rpm_can_id = 0;
    cfg.ign_run_rpm    = 400;
    /* 0x201, 500 rpm */
    inject(NODE_BASE + RCM_F_CMD_CTL, { RCM_OP_SET_RUN_SRC, 0x01, 0x02, 0xF4, 0x01 });
    run_ms(TICK_MS * 4);
    TEST_ASSERT_EQUAL_HEX16(0x201, cfg.ecu_rpm_can_id);
    TEST_ASSERT_EQUAL_UINT16(500, cfg.ign_run_rpm);
}

static void test_set_run_src_installs_a_filter_for_the_id(void)
{
    /* An id with no filter behind it is decoration: bxCAN drops the frame before any
     * of this code sees it, and the board stays blind to a running engine while
     * claiming to be configured for one. */
    inject(NODE_BASE + RCM_F_CMD_CTL, { RCM_OP_SET_RUN_SRC, 0x01, 0x02, 0x90, 0x01 });
    run_ms(TICK_MS * 4);
    bool found = false;
    for (size_t i = 0; i < FILTERS.size(); i++) if (FILTERS[i] == 0x201) found = true;
    TEST_ASSERT_TRUE_MESSAGE(found, "no receive filter was installed for the RPM id");
}

static void test_changing_the_run_src_does_not_grow_the_filter_set(void)
{
    /* The banks are a finite resource (28) and they are ORed, so appending rather than
     * rebuilding would both leave the OLD id still accepted and eventually exhaust the
     * table -- at which point new filters are dropped silently and the board goes deaf
     * to the frame it was just told to listen for. */
    inject(NODE_BASE + RCM_F_CMD_CTL, { RCM_OP_SET_RUN_SRC, 0x01, 0x02, 0x90, 0x01 });
    run_ms(TICK_MS * 4);
    const size_t after_first = FILTERS.size();

    for (int i = 0; i < 5; i++) {
        inject(NODE_BASE + RCM_F_CMD_CTL, { RCM_OP_SET_RUN_SRC, 0x11, 0x03, 0x90, 0x01 });
        run_ms(TICK_MS * 4);
    }
    TEST_ASSERT_EQUAL_HEX16(0x311, cfg.ecu_rpm_can_id);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)after_first, (uint32_t)FILTERS.size());

    bool stale = false;
    for (size_t i = 0; i < FILTERS.size(); i++) if (FILTERS[i] == 0x201) stale = true;
    TEST_ASSERT_FALSE_MESSAGE(stale, "the old RPM id is still being accepted");
}

static void test_set_run_src_refuses_a_non_standard_id(void)
{
    /* 0x800 and up is not an 11-bit id. Masking it would quietly point the ignition at
     * somebody else's traffic, so it is refused outright and the old value kept. */
    inject(NODE_BASE + RCM_F_CMD_CTL, { RCM_OP_SET_RUN_SRC, 0x01, 0x02, 0x90, 0x01 });
    run_ms(TICK_MS * 4);
    inject(NODE_BASE + RCM_F_CMD_CTL, { RCM_OP_SET_RUN_SRC, 0x00, 0x08, 0x90, 0x01 });
    run_ms(TICK_MS * 4);
    TEST_ASSERT_EQUAL_HEX16(0x201, cfg.ecu_rpm_can_id);
}

static void test_set_run_src_zero_disables_the_can_source(void)
{
    inject(NODE_BASE + RCM_F_CMD_CTL, { RCM_OP_SET_RUN_SRC, 0x01, 0x02, 0x90, 0x01 });
    run_ms(TICK_MS * 4);
    inject(NODE_BASE + RCM_F_CMD_CTL, { RCM_OP_SET_RUN_SRC, 0x00, 0x00, 0x00, 0x00 });
    run_ms(TICK_MS * 4);
    TEST_ASSERT_EQUAL_HEX16(0, cfg.ecu_rpm_can_id);
    /* A zero threshold means "leave it alone" -- 0 rpm would make a stopped engine
     * read as running the moment any frame arrived. */
    TEST_ASSERT_EQUAL_UINT16(400, cfg.ign_run_rpm);
}

/* --- configuring the peer over the bus ---------------------------------------
 * peer_node / peer_mask / peer_toggle_mask were read by the firmware and written by
 * nothing, so a keypad-to-relay-module pair could not be set up without a recompile.
 * peer_toggle_mask is the one that matters most: it is what turns a momentary button
 * into a latching load. */

static void set_peer_over_can(uint8_t node, uint32_t mask, uint32_t toggle)
{
    inject(NODE_BASE + RCM_F_CMD_CTL,
           { RCM_OP_SET_PEER, node,
             (uint8_t)mask,   (uint8_t)(mask >> 8),   (uint8_t)(mask >> 16),
             (uint8_t)toggle, (uint8_t)(toggle >> 8), (uint8_t)(toggle >> 16) });
    run_ms(TICK_MS * 4);
}

static void test_set_peer_takes_node_and_both_masks(void)
{
    cfg.peer_node = PEER_NONE;
    set_peer_over_can(4, 0x000103, 0x000002);
    TEST_ASSERT_EQUAL_UINT8(4, cfg.peer_node);
    TEST_ASSERT_EQUAL_HEX32(0x000103, cfg.peer_mask);
    TEST_ASSERT_EQUAL_HEX32(0x000002, cfg.peer_toggle_mask);
}

static void test_set_peer_installs_a_filter_for_the_peers_inputs(void)
{
    /* Without a filter the keypad's frames are dropped by bxCAN before any of this code
     * runs, and the board sits there configured and deaf. */
    set_peer_over_can(4, 0x1FFFFF, 0);
    bool found = false;
    for (size_t i = 0; i < FILTERS.size(); i++) if (FILTERS[i] == PEER_ID) found = true;
    TEST_ASSERT_TRUE_MESSAGE(found, "no receive filter for the peer's INPUTS frame");
}

static void test_set_peer_none_disables_and_drops_the_filter(void)
{
    set_peer_over_can(4, 0x1FFFFF, 0);
    set_peer_over_can(PEER_NONE, 0, 0);
    TEST_ASSERT_EQUAL_UINT8(PEER_NONE, cfg.peer_node);
    for (size_t i = 0; i < FILTERS.size(); i++)
        TEST_ASSERT_NOT_EQUAL_MESSAGE(PEER_ID, FILTERS[i],
                                      "still listening to a peer that was switched off");
}

static void test_set_peer_rejects_a_node_that_cannot_exist(void)
{
    set_peer_over_can(4, 0x000001, 0);
    inject(NODE_BASE + RCM_F_CMD_CTL,
           { RCM_OP_SET_PEER, 9, 0xFF, 0xFF, 0x1F, 0, 0, 0 });
    run_ms(TICK_MS * 4);
    TEST_ASSERT_EQUAL_UINT8(4, cfg.peer_node);
    TEST_ASSERT_EQUAL_HEX32(0x000001, cfg.peer_mask);
}

static void test_changing_the_peer_re_baselines_before_acting(void)
{
    /* THE ONE THAT BITES. A toggle fires on a rising edge, so the remembered previous
     * frame has to be thrown away when the peer changes. Keep it, and the new peer's
     * very first frame is compared against whatever the OLD one last sent -- every
     * toggle channel whose bit differs fires the moment the keypad is plugged in. */
    set_peer_over_can(4, 0x000001, 0x000001);
    inject(PEER_ID, { 0x00, 0, 0 });            /* baseline: button not pressed */
    run_ms(TICK_MS * 2);
    TEST_ASSERT_FALSE(sim_driver_on(0));

    set_peer_over_can(4, 0x000001, 0x000001);   /* reconfigured */

    inject(PEER_ID, { 0x01, 0, 0 });            /* first frame after: baseline ONLY */
    run_ms(TICK_MS * 2);
    TEST_ASSERT_FALSE_MESSAGE(sim_driver_on(0),
                              "a toggle fired on the first frame from a new peer");

    inject(PEER_ID, { 0x00, 0, 0 });
    run_ms(TICK_MS * 2);
    inject(PEER_ID, { 0x01, 0, 0 });            /* a real press now works */
    run_ms(TICK_MS * 2);
    TEST_ASSERT_TRUE(sim_driver_on(0));
}

/* --- asking the ECU to start or stop ----------------------------------------
 * A press on a configured input sends rusEFI's bench-test user-control frame, which
 * lands on the same startStopButtonToggle() its own physical button uses. This board
 * asks; the ECU decides whether that means crank or stop. */

static void arm_ecu_cmd(uint8_t slot, uint8_t ch, uint16_t sub, uint16_t idx)
{
    cfg.ch[ch].mode = CH_INPUT;
    SIM.wiring[ch]  = SIM_BUTTON_OPEN;
    ch_begin();
    inject(NODE_BASE + RCM_F_CMD_CTL,
           { RCM_OP_SET_ECU_CMD, slot, ch,
             (uint8_t)sub, (uint8_t)(sub >> 8),
             (uint8_t)idx, (uint8_t)(idx >> 8) });
    run_ms(TICK_MS * 4);
}

static void arm_ecu_start(uint8_t ch)
{
    arm_ecu_cmd(0, ch, RCM_ECU_SUB_X14, RCM_ECU_IDX_STARTSTOP);
}

static int count_ecu_cmds(void)
{
    int n = 0;
    for (size_t i = 0; i < EXT_TX.size(); i++)
        if (EXT_TX[i].id == RCM_ECU_CMD_ID) n++;
    return n;
}

static void test_ecu_start_sends_the_documented_frame(void)
{
    arm_ecu_start(6);
    EXT_TX.clear();
    SIM.wiring[6] = SIM_BUTTON_PRESSED;
    run_ms(cfg.input_debounce_ms + TICK_MS * 6);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, count_ecu_cmds(), "expected exactly one command");
    const ext_frame_t &f = EXT_TX[0];
    TEST_ASSERT_TRUE_MESSAGE(f.ext, "rusEFI's command block is EXTENDED, not standard");
    TEST_ASSERT_EQUAL_HEX32(0x77000Cu, f.id);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x66, f.data[0], "BENCH_HEADER magic");
    TEST_ASSERT_EQUAL_HEX8(0x14, f.data[2]);   /* TS_X14, LE */
    TEST_ASSERT_EQUAL_HEX8(0x00, f.data[3]);
    TEST_ASSERT_EQUAL_HEX8(0x09, f.data[4]);   /* TS_START_STOP_ENGINE, LE */
    TEST_ASSERT_EQUAL_HEX8(0x00, f.data[5]);
}

static void test_ecu_start_fires_once_per_press_not_while_held(void)
{
    /* The ECU treats each command as a toggle. Repeating it while the button is held
     * would start the engine and then immediately stop it again. */
    arm_ecu_start(6);
    EXT_TX.clear();
    SIM.wiring[6] = SIM_BUTTON_PRESSED;
    run_ms(cfg.input_debounce_ms + TICK_MS * 20);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, count_ecu_cmds(), "repeated while held");

    SIM.wiring[6] = SIM_BUTTON_OPEN;
    run_ms(cfg.input_debounce_ms + TICK_MS * 6);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, count_ecu_cmds(), "release should not command");

    SIM.wiring[6] = SIM_BUTTON_PRESSED;
    run_ms(cfg.input_debounce_ms + TICK_MS * 6);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, count_ecu_cmds(), "second press should command");
}

static void test_no_ecu_command_when_unconfigured(void)
{
    for (uint8_t i = 0; i < RCM_ECU_CMDS; i++) cfg.ecu_cmd[i].ch = IGN_CH_NONE;
    cfg.ch[6].mode = CH_INPUT;
    ch_begin();
    EXT_TX.clear();
    SIM.wiring[6] = SIM_BUTTON_PRESSED;
    run_ms(cfg.input_debounce_ms + TICK_MS * 8);
    TEST_ASSERT_EQUAL_INT(0, count_ecu_cmds());
}

static void test_a_button_already_held_at_boot_does_not_command(void)
{
    /* Seeding the edge detector from false would make a board that comes up with the
     * button held ask the ECU to crank before anyone touched anything -- and on a car
     * that wakes with a jammed or shorted button, repeatedly. */
    cfg.ch[6].mode = CH_INPUT;
    SIM.wiring[6]  = SIM_BUTTON_PRESSED;
    ch_begin();
    run_ms(cfg.input_debounce_ms + TICK_MS * 4);
    cfg.ecu_cmd[0].ch        = 6;
    cfg.ecu_cmd[0].subsystem = RCM_ECU_SUB_X14;
    cfg.ecu_cmd[0].index     = RCM_ECU_IDX_STARTSTOP;
    proto_begin();
    EXT_TX.clear();
    run_ms(TICK_MS * 8);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_ecu_cmds(), "commanded on a held button");
}

static void test_a_second_slot_sends_its_own_command(void)
{
    /* The whole point of storing a (subsystem, index) pair rather than a list of named
     * commands: a Lua command is just different numbers, and rusEFI has no fixed command
     * for traction control or launch control -- you bind a button to LUA_COMMAND_n and
     * watch the counter from a Lua script. */
    arm_ecu_cmd(0, 6, RCM_ECU_SUB_X14,   RCM_ECU_IDX_STARTSTOP);
    arm_ecu_cmd(1, 7, RCM_ECU_SUB_BENCH, RCM_ECU_IDX_LUA1);
    EXT_TX.clear();

    SIM.wiring[7] = SIM_BUTTON_PRESSED;
    run_ms(cfg.input_debounce_ms + TICK_MS * 6);

    TEST_ASSERT_EQUAL_INT(1, count_ecu_cmds());
    TEST_ASSERT_EQUAL_HEX8(0x16, EXT_TX[0].data[2]);   /* TS_BENCH_CATEGORY */
    TEST_ASSERT_EQUAL_HEX8(0x21, EXT_TX[0].data[4]);   /* LUA_COMMAND_1 = 33 */

    /* and the other slot's button is still its own command */
    EXT_TX.clear();
    SIM.wiring[6] = SIM_BUTTON_PRESSED;
    run_ms(cfg.input_debounce_ms + TICK_MS * 6);
    TEST_ASSERT_EQUAL_INT(1, count_ecu_cmds());
    TEST_ASSERT_EQUAL_HEX8(0x14, EXT_TX[0].data[2]);
    TEST_ASSERT_EQUAL_HEX8(0x09, EXT_TX[0].data[4]);
}

/* --- reading configuration back ---------------------------------------------
 * Every other opcode is a setter. Without this a tool can configure a board and never
 * ask what it is configured as, so every check has to be behavioural -- which is exactly
 * how bench sessions end up drawing conclusions from stale frames. */

static const can_frame_t *cfg_reply(void)
{
    for (size_t i = TX.size(); i-- > 0; )
        if (TX[i].id == NODE_BASE + RCM_F_CFG_REPLY) return &TX[i];
    return nullptr;
}

static const can_frame_t *ask(uint8_t sel, uint8_t idx = 0)
{
    TX.clear();
    inject(NODE_BASE + RCM_F_CMD_CTL, { RCM_OP_GET_CFG, sel, idx });
    run_ms(TICK_MS * 4);
    return cfg_reply();
}

static void test_config_read_back_returns_what_was_set(void)
{
    inject(NODE_BASE + RCM_F_CMD_CTL, { RCM_OP_SET_IGNITION, IGN_MOMENTARY });
    inject(NODE_BASE + RCM_F_CMD_CTL, { RCM_OP_SET_CH_FUNC, 10, FN_IN_BRAKE, 0, 0, 0 });
    inject(NODE_BASE + RCM_F_CMD_CTL, { RCM_OP_SET_CH_FUNC, 4,  FN_STARTER,  0, 0, 0 });
    inject(NODE_BASE + RCM_F_CMD_CTL, { RCM_OP_SET_RUN_SRC, 0x01, 0x02, 0xF4, 0x01 });
    run_ms(TICK_MS * 4);

    const can_frame_t *r = ask(RCM_CFG_SEL_IGN);
    TEST_ASSERT_NOT_NULL_MESSAGE(r, "no reply to a config request");
    TEST_ASSERT_EQUAL_HEX8(RCM_CFG_SEL_IGN, r->data[0]);
    TEST_ASSERT_EQUAL_UINT8(IGN_MOMENTARY, r->data[2]);
    /* The channels come back RESOLVED from the labels -- which is what makes
     * "so which channel is the brake?" answerable at all. */
    TEST_ASSERT_EQUAL_UINT8(10, r->data[3]);
    TEST_ASSERT_EQUAL_UINT8(4,  r->data[4]);

    r = ask(RCM_CFG_SEL_RUNSRC);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_HEX16(0x201, (uint16_t)(r->data[2] | (r->data[3] << 8)));
    TEST_ASSERT_EQUAL_UINT16(500,  (uint16_t)(r->data[4] | (r->data[5] << 8)));
}

static void test_config_read_back_is_indexed_for_tables(void)
{
    inject(NODE_BASE + RCM_F_CMD_CTL, { RCM_OP_SET_ECU_CMD, 2, 7, 0x16, 0x00, 0x21, 0x00 });
    run_ms(TICK_MS * 4);

    const can_frame_t *r = ask(RCM_CFG_SEL_ECUCMD, 2);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(2, r->data[1], "the reply must say which slot it is");
    TEST_ASSERT_EQUAL_UINT8(7, r->data[2]);
    TEST_ASSERT_EQUAL_HEX16(0x0016, (uint16_t)(r->data[3] | (r->data[4] << 8)));
    TEST_ASSERT_EQUAL_HEX16(0x0021, (uint16_t)(r->data[5] | (r->data[6] << 8)));

    /* a different slot is genuinely a different answer, not the same one echoed */
    r = ask(RCM_CFG_SEL_ECUCMD, 3);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_UINT8(3, r->data[1]);
    TEST_ASSERT_EQUAL_UINT8(IGN_CH_NONE, r->data[2]);
}

static void test_a_global_config_request_is_never_answered(void)
{
    /* THE one that matters on a real bus. Eight nodes replying to one broadcast would
     * collide, and the asker could not tell whose answer it got anyway. */
    TX.clear();
    inject(proto_global_id(), { RCM_OP_GET_CFG, RCM_CFG_SEL_IGN, 0 });
    run_ms(TICK_MS * 4);
    TEST_ASSERT_NULL_MESSAGE(cfg_reply(), "answered a broadcast config request");

    /* and the same request addressed to this node still works */
    TEST_ASSERT_NOT_NULL(ask(RCM_CFG_SEL_IGN));
}

static void test_out_of_range_requests_say_nothing(void)
{
    TEST_ASSERT_NULL_MESSAGE(ask(0x7F), "answered an unknown selector");
    TEST_ASSERT_NULL_MESSAGE(ask(RCM_CFG_SEL_ECUCMD, RCM_ECU_CMDS), "answered a bad slot");
    TEST_ASSERT_NULL_MESSAGE(ask(RCM_CFG_SEL_CHANNEL, RCM_CHANNELS), "answered a bad channel");
}

static void test_set_ch_func_validates_behaviour_against_mode(void)
{
    /* The behaviour byte means different things for an input and an output. Letting an
     * output behaviour onto an input would leave a channel claiming to flash while
     * reporting a latch, which nothing downstream could make sense of. */
    inject(NODE_BASE + RCM_F_CMD_CTL, { RCM_OP_SET_CH_MODE, 4, CH_INPUT, 0 });
    run_ms(TICK_MS * 4);

    inject(NODE_BASE + RCM_F_CMD_CTL,
           { RCM_OP_SET_CH_FUNC, 4, 40, IN_TOGGLE, 0x00, 0x00 });
    run_ms(TICK_MS * 4);
    TEST_ASSERT_EQUAL_UINT8(IN_TOGGLE, cfg.ch[4].behaviour);
    TEST_ASSERT_EQUAL_UINT8(40, cfg.ch[4].func);

    /* OUT_DELAY_OFF is 3, past IN_HOLD_ARM -- refused, old value kept */
    inject(NODE_BASE + RCM_F_CMD_CTL,
           { RCM_OP_SET_CH_FUNC, 4, 41, OUT_DELAY_OFF, 0x00, 0x00 });
    run_ms(TICK_MS * 4);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(IN_TOGGLE, cfg.ch[4].behaviour,
        "an output behaviour was accepted on an input channel");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(40, cfg.ch[4].func, "the label changed anyway");
}

static void test_set_ch_func_carries_the_param(void)
{
    inject(NODE_BASE + RCM_F_CMD_CTL, { RCM_OP_SET_CH_MODE, 5, CH_INPUT, 0 });
    inject(NODE_BASE + RCM_F_CMD_CTL,
           { RCM_OP_SET_CH_FUNC, 5, 40, IN_HOLD_ARM, 0xE8, 0x03 });
    run_ms(TICK_MS * 4);
    TEST_ASSERT_EQUAL_UINT8(IN_HOLD_ARM, cfg.ch[5].behaviour);
    TEST_ASSERT_EQUAL_UINT16(1000, cfg.ch[5].param);
}

static void test_a_role_lives_in_exactly_one_place(void)
{
    /* The point of the whole arrangement: move the label, and the role moves with it.
     * There is no second setting left behind pointing at the old channel. */
    inject(NODE_BASE + RCM_F_CMD_CTL, { RCM_OP_SET_CH_FUNC, 11, FN_IN_BRAKE, 0, 0, 0 });
    run_ms(TICK_MS * 4);
    TEST_ASSERT_EQUAL_UINT8(11, cfg_ch_for(FN_IN_BRAKE));

    /* relabel the old one away, and label a new one */
    inject(NODE_BASE + RCM_F_CMD_CTL, { RCM_OP_SET_CH_FUNC, 11, FN_NONE,     0, 0, 0 });
    inject(NODE_BASE + RCM_F_CMD_CTL, { RCM_OP_SET_CH_FUNC, 3,  FN_IN_BRAKE, 0, 0, 0 });
    run_ms(TICK_MS * 4);
    TEST_ASSERT_EQUAL_UINT8(3, cfg_ch_for(FN_IN_BRAKE));
}

static void test_an_unlabelled_role_reads_as_unconfigured(void)
{
    for (uint8_t ch = 0; ch < RCM_CHANNELS; ch++) cfg.ch[ch].func = FN_NONE;
    TEST_ASSERT_EQUAL_UINT8(IGN_CH_NONE, cfg_ch_for(FN_IN_BRAKE));
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(IGN_CH_NONE, cfg_ch_for(FN_STARTER),
        "an unlabelled starter must read as unconfigured, not as channel 0");
}

static void test_set_failsafe_and_bitrate(void)
{
    inject(NODE_BASE + RCM_F_CMD_CTL, { RCM_OP_SET_FAILSAFE, 0x03, 0x00, 0x10 });
    inject(NODE_BASE + RCM_F_CMD_CTL, { RCM_OP_SET_BITRATE, 0x40, 0x42, 0x0F, 0x00 });
    run_ms(TICK_MS * 4);
    TEST_ASSERT_EQUAL_HEX32(0x100003, cfg.failsafe_state);
    TEST_ASSERT_EQUAL_UINT32(1000000, cfg.can_bitrate);
}

static void test_reboot_needs_the_magic_byte(void)
{
    /* A stray frame that rebooted the relay module would drop every load on the car. */
    inject(NODE_BASE + RCM_F_CMD_CTL, { RCM_OP_REBOOT });
    inject(NODE_BASE + RCM_F_CMD_CTL, { RCM_OP_REBOOT, 0x00 });
    run_ms(TICK_MS * 4);
    TEST_ASSERT_EQUAL_INT(0, RESETS);

    inject(NODE_BASE + RCM_F_CMD_CTL, { RCM_OP_REBOOT, 0xA5 });
    run_ms(TICK_MS * 4);
    TEST_ASSERT_EQUAL_INT(1, RESETS);
}

static void test_config_can_be_saved_and_comes_back(void)
{
    store_begin();
    inject(NODE_BASE + RCM_F_CMD_CTL, { RCM_OP_SET_BITRATE, 0x90, 0xD0, 0x03, 0x00 });
    inject(NODE_BASE + RCM_F_CMD_CTL, { RCM_OP_SAVE_CONFIG });
    run_ms(TICK_MS * 4);

    memset(&cfg, 0, sizeof(cfg));
    cfg_load();
    TEST_ASSERT_EQUAL_UINT32(250000, cfg.can_bitrate);
}

/* --- boot state ------------------------------------------------------------- */

static void test_a_default_board_boots_with_nothing_energised(void)
{
    /* setup() adopts failsafe_state and enables the outputs as its first act, so the
     * obvious worry is that this turns things on at boot. It does not: failsafe_state
     * defaults to zero, and an unconfigured board comes up with all 21 channels off --
     * exactly as it did when failsafe was only applied on a bus timeout. */
    cfg_defaults(&cfg);
    for (uint8_t i = 0; i < RCM_CHANNELS; i++) cfg.ch[i].mode = CH_OUTPUT;
    ch_begin();

    TEST_ASSERT_EQUAL_HEX32_MESSAGE(0, cfg.failsafe_state, "default must be all-off");

    ch_apply_failsafe();          /* what setup() does */
    run_ms(TICK_MS * 2);

    for (uint8_t ch = 0; ch < RCM_CHANNELS; ch++)
        TEST_ASSERT_FALSE_MESSAGE(sim_driver_on(ch), "a channel came up energised");
    TEST_ASSERT_EQUAL_HEX32(0, ch_commanded());
}

static void test_boot_state_honours_an_inverted_channel(void)
{
    /* failsafe_state is the PHYSICAL state wanted, so a zero bit must mean "not
     * energised" even on a channel whose logic is inverted. Getting this backwards
     * would energise every inverted channel at boot -- the exact failure the question
     * was about. */
    cfg_defaults(&cfg);
    for (uint8_t i = 0; i < RCM_CHANNELS; i++) cfg.ch[i].mode = CH_OUTPUT;
    cfg.ch[3].flags = CH_F_INVERT;
    ch_begin();

    ch_apply_failsafe();
    run_ms(TICK_MS * 2);
    TEST_ASSERT_FALSE_MESSAGE(sim_driver_on(3), "inverted channel energised itself");

    /* And with the bit set it really does come up on. */
    cfg.failsafe_state = (1ul << 3);
    ch_apply_failsafe();
    run_ms(TICK_MS * 2);
    TEST_ASSERT_TRUE(sim_driver_on(3));
}

static void test_only_configured_channels_come_up(void)
{
    cfg_defaults(&cfg);
    for (uint8_t i = 0; i < RCM_CHANNELS; i++) cfg.ch[i].mode = CH_OUTPUT;
    cfg.ch[9].mode = CH_INPUT;          /* an input must never be driven */
    cfg.failsafe_state = 0x1FFFFF;      /* ask for everything, deliberately */
    ch_begin();

    ch_apply_failsafe();
    run_ms(TICK_MS * 2);
    TEST_ASSERT_FALSE_MESSAGE(sim_driver_on(9), "an input channel was energised");
    TEST_ASSERT_TRUE(sim_driver_on(8));
    TEST_ASSERT_TRUE(sim_driver_on(10));
}

static void test_the_starter_cannot_be_commanded_over_can(void)
{
    /* Only the ignition state machine turns a starter, and it does so with the brake
     * held and the engine confirmed stopped -- conditions a remote frame knows nothing
     * about. A stray, replayed or mistaken CMD_SET must not crank the engine. */
    cfg.ch[4].func = FN_STARTER;

    inject(NODE_BASE + RCM_F_CMD_SET, { 0xFF, 0xFF, 0x1F, 0xFF, 0xFF, 0x1F });
    run_ms(TICK_MS * 6);

    TEST_ASSERT_FALSE_MESSAGE(sim_driver_on(4), "CAN commanded the starter");
    TEST_ASSERT_TRUE_MESSAGE(sim_driver_on(3), "other channels should still obey");
    TEST_ASSERT_TRUE(sim_driver_on(5));
}

static void test_a_peer_keypad_cannot_command_the_starter(void)
{
    /* Same rule by a different route: a button on a mirrored keypad must not be one
     * press away from the starter motor. */
    cfg.ch[4].func = FN_STARTER;
    cfg.peer_node        = 4;
    cfg.peer_mask        = 0x1FFFFF;
    cfg.peer_toggle_mask = 0;
    proto_begin();

    inject(PEER_ID, { 0x00, 0x00, 0x00 });
    run_ms(TICK_MS * 4);
    inject(PEER_ID, { 0xFF, 0xFF, 0x1F });
    run_ms(TICK_MS * 4);

    TEST_ASSERT_FALSE_MESSAGE(sim_driver_on(4), "a peer keypad reached the starter");
    TEST_ASSERT_TRUE(sim_driver_on(3));
}

/* --- bus timeout ----------------------------------------------------------- */

static void test_silence_triggers_the_failsafe(void)
{
    cfg.failsafe_state = (1ul << 2);
    ch_command_mask(0x1FFFFF, 0x1FFFFF);
    run_ms(TICK_MS * 2);
    TEST_ASSERT_FALSE(proto_failsafe());

    run_ms(cfg.can_timeout_ms + 100);
    TEST_ASSERT_TRUE(proto_failsafe());
    TEST_ASSERT_EQUAL_HEX32(1ul << 2, ch_commanded());
    proto_broadcast(SIM.now_ms);
    TEST_ASSERT_TRUE(sent(NODE_BASE + RCM_F_OUTPUTS)->data[3] & RCM_ST_FAILSAFE);
}

static void test_traffic_clears_the_failsafe(void)
{
    run_ms(cfg.can_timeout_ms + 100);
    TEST_ASSERT_TRUE(proto_failsafe());

    inject(NODE_BASE + RCM_F_CMD_SET, { 0x01, 0, 0, 0x01, 0, 0 });
    run_ms(TICK_MS * 4);
    TEST_ASSERT_FALSE(proto_failsafe());
    TEST_ASSERT_TRUE(sim_driver_on(0));
}

static void test_traffic_for_someone_else_does_not_count(void)
{
    /* Hearing the ECU chatter to the dash says nothing about whether anyone is still
     * commanding THIS board. */
    for (uint32_t t = 0; t < cfg.can_timeout_ms + 100; t += 50) {
        inject(0x200, { 1, 2, 3, 4 });         /* rusEFI's own broadcast */
        inject(0x341, { 1, 2, 3, 4 });         /* another RCM node's inputs */
        run_ms(50);
    }
    TEST_ASSERT_TRUE(proto_failsafe());
}

static void test_a_zero_timeout_disables_the_failsafe(void)
{
    cfg.can_timeout_ms = 0;
    ch_command_mask(0x1FFFFF, 0x1FFFFF);
    run_ms(5000);
    TEST_ASSERT_FALSE(proto_failsafe());
    TEST_ASSERT_EQUAL_HEX32(0x1FFFFF, ch_commanded());
}

/* --- peer mirroring -------------------------------------------------------- */

static void setup_peer(uint32_t mask, uint32_t toggle)
{
    cfg.peer_node        = 4;              /* keypad at address 0 */
    cfg.peer_mask        = mask;
    cfg.peer_toggle_mask = toggle;
    FILTERS.clear();
    proto_begin();
}

static void test_peer_follow_mode(void)
{
    setup_peer(0x00000F, 0);
    inject(PEER_ID, { 0x00, 0, 0 });        /* baseline */
    run_ms(TICK_MS * 2);
    inject(PEER_ID, { 0x05, 0, 0 });        /* buttons 1 and 3 held */
    run_ms(TICK_MS * 2);
    TEST_ASSERT_TRUE(sim_driver_on(0));
    TEST_ASSERT_TRUE(sim_driver_on(2));

    inject(PEER_ID, { 0x00, 0, 0 });        /* released */
    run_ms(TICK_MS * 2);
    TEST_ASSERT_FALSE(sim_driver_on(0));
}

static void test_peer_toggle_mode_fires_once_per_press(void)
{
    setup_peer(0x000001, 0x000001);
    inject(PEER_ID, { 0x00, 0, 0 });
    run_ms(TICK_MS * 2);

    inject(PEER_ID, { 0x01, 0, 0 });        /* press */
    run_ms(TICK_MS * 2);
    TEST_ASSERT_TRUE(sim_driver_on(0));

    for (int i = 0; i < 5; i++) {           /* still held -- must not chatter */
        inject(PEER_ID, { 0x01, 0, 0 });
        run_ms(TICK_MS * 2);
    }
    TEST_ASSERT_TRUE(sim_driver_on(0));

    inject(PEER_ID, { 0x00, 0, 0 });        /* release does nothing */
    run_ms(TICK_MS * 2);
    TEST_ASSERT_TRUE(sim_driver_on(0));

    inject(PEER_ID, { 0x01, 0, 0 });        /* second press turns it off */
    run_ms(TICK_MS * 2);
    TEST_ASSERT_FALSE(sim_driver_on(0));
}

static void test_first_peer_frame_only_sets_a_baseline(void)
{
    /* Otherwise a board coming up while a button happens to be held would fire every
     * toggle channel the instant it joined the bus. */
    setup_peer(0x000001, 0x000001);
    inject(PEER_ID, { 0x01, 0, 0 });
    run_ms(TICK_MS * 2);
    TEST_ASSERT_FALSE_MESSAGE(sim_driver_on(0), "acted on the very first peer frame");
}

static void test_peer_mask_bounds_what_it_can_touch(void)
{
    setup_peer(0x000001, 0);
    ch_command(1, true);
    run_ms(TICK_MS * 2);
    inject(PEER_ID, { 0x00, 0, 0 });
    run_ms(TICK_MS * 2);
    inject(PEER_ID, { 0x03, 0, 0 });        /* peer says ch1 AND ch2 pressed */
    run_ms(TICK_MS * 2);
    TEST_ASSERT_TRUE(sim_driver_on(0));
    TEST_ASSERT_TRUE_MESSAGE(sim_driver_on(1), "peer reached outside its mask");
}

static void test_peer_adds_a_filter(void)
{
    setup_peer(0x1FFFFF, 0);
    TEST_ASSERT_EQUAL_UINT32(3, FILTERS.size());
    TEST_ASSERT_EQUAL_HEX16(PEER_ID, FILTERS[2]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ids_are_where_protocol_h_says);
    RUN_TEST(test_base_id_is_forced_somewhere_workable);
    RUN_TEST(test_filters_cover_our_block_and_the_global_id);
    RUN_TEST(test_broadcast_sends_all_four_frames);
    RUN_TEST(test_output_bits_land_in_the_right_places);
    RUN_TEST(test_status_flags_and_seq);
    RUN_TEST(test_faults_reach_the_bus);
    RUN_TEST(test_raw_sense_is_published_alongside_logical_inputs);
    RUN_TEST(test_cmd_set_applies_only_masked_channels);
    RUN_TEST(test_cmd_set_reaches_channel_21);
    RUN_TEST(test_short_cmd_set_is_ignored);
    RUN_TEST(test_all_off);
    RUN_TEST(test_global_ctl_is_obeyed);
    RUN_TEST(test_outputs_can_be_parked_hiz);
    RUN_TEST(test_set_ch_mode_turns_the_channel_off_first);
    RUN_TEST(test_set_ch_mode_rejects_nonsense);
    RUN_TEST(test_set_failsafe_and_bitrate);
    RUN_TEST(test_set_ignition_rejects_a_mode_that_does_not_exist);
    RUN_TEST(test_set_ign_times_clamps);
    RUN_TEST(test_set_run_src_takes_id_and_threshold);
    RUN_TEST(test_set_run_src_installs_a_filter_for_the_id);
    RUN_TEST(test_changing_the_run_src_does_not_grow_the_filter_set);
    RUN_TEST(test_set_run_src_refuses_a_non_standard_id);
    RUN_TEST(test_set_run_src_zero_disables_the_can_source);
    RUN_TEST(test_reboot_needs_the_magic_byte);
    RUN_TEST(test_config_can_be_saved_and_comes_back);
    RUN_TEST(test_a_default_board_boots_with_nothing_energised);
    RUN_TEST(test_boot_state_honours_an_inverted_channel);
    RUN_TEST(test_only_configured_channels_come_up);
    RUN_TEST(test_the_starter_cannot_be_commanded_over_can);
    RUN_TEST(test_a_peer_keypad_cannot_command_the_starter);
    RUN_TEST(test_silence_triggers_the_failsafe);
    RUN_TEST(test_traffic_clears_the_failsafe);
    RUN_TEST(test_traffic_for_someone_else_does_not_count);
    RUN_TEST(test_a_zero_timeout_disables_the_failsafe);
    RUN_TEST(test_peer_follow_mode);
    RUN_TEST(test_peer_toggle_mode_fires_once_per_press);
    RUN_TEST(test_first_peer_frame_only_sets_a_baseline);
    RUN_TEST(test_peer_mask_bounds_what_it_can_touch);
    RUN_TEST(test_peer_adds_a_filter);
    RUN_TEST(test_set_peer_takes_node_and_both_masks);
    RUN_TEST(test_set_peer_installs_a_filter_for_the_peers_inputs);
    RUN_TEST(test_set_peer_none_disables_and_drops_the_filter);
    RUN_TEST(test_set_peer_rejects_a_node_that_cannot_exist);
    RUN_TEST(test_changing_the_peer_re_baselines_before_acting);
    RUN_TEST(test_ecu_start_sends_the_documented_frame);
    RUN_TEST(test_ecu_start_fires_once_per_press_not_while_held);
    RUN_TEST(test_no_ecu_command_when_unconfigured);
    RUN_TEST(test_a_button_already_held_at_boot_does_not_command);
    RUN_TEST(test_a_second_slot_sends_its_own_command);
    RUN_TEST(test_config_read_back_returns_what_was_set);
    RUN_TEST(test_config_read_back_is_indexed_for_tables);
    RUN_TEST(test_a_global_config_request_is_never_answered);
    RUN_TEST(test_out_of_range_requests_say_nothing);
    RUN_TEST(test_set_ch_func_validates_behaviour_against_mode);
    RUN_TEST(test_set_ch_func_carries_the_param);
    RUN_TEST(test_a_role_lives_in_exactly_one_place);
    RUN_TEST(test_an_unlabelled_role_reads_as_unconfigured);
    return UNITY_END();
}
