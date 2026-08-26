/*
 * test_ignition -- the ignition state machine, and above all its safety invariants.
 *
 * This is the module most worth testing hard: it is the only one that can turn a
 * starter motor, and the only one whose job includes stopping an engine. The tests are
 * arranged around the asymmetry in ignition.h -- starting is conditional, stopping is
 * never conditional -- and try to break it in both directions.
 */
#include <unity.h>
#include <cstdio>

#include "../stubs/simboard.cpp"
#include "../../src/shiftreg.cpp"

#include "config.h"
struct rcm_config_t cfg;
struct rcm_straps_t straps;

#include "../../src/channels.cpp"
#include "../../src/ignition.cpp"

#define BRAKE_CH 10
#define START_CH 4
#define RUN_CH   11
#define RUNOUT_CH 6
#define HOLD_MS  1000
#define CRANK_MS 8000

static bool sw;                    /* the ignition input, true = +12V present */

static void tick(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += TICK_MS) {
        SIM.now_ms += TICK_MS;
        ch_tick(SIM.now_ms);
        ign_tick(SIM.now_ms, sw);
    }
}

static void press(uint32_t hold_ms)
{
    sw = true;  tick(hold_ms);
    sw = false; tick(TICK_MS * 4);
}

static void set_brake(bool on)
{
    SIM.wiring[BRAKE_CH] = on ? SIM_BUTTON_PRESSED : SIM_BUTTON_OPEN;
    tick(cfg.input_debounce_ms + TICK_MS * 4);
}

static void set_running(bool on)
{
    SIM.wiring[RUN_CH] = on ? SIM_BUTTON_PRESSED : SIM_BUTTON_OPEN;
    tick(cfg.input_debounce_ms + TICK_MS * 4);
}

/* A board configured the way a push-button start install would be, already awake and
 * with the wake press released. */
static void momentary_setup(bool with_run_channel)
{
    memset(&cfg, 0, sizeof(cfg));
    cfg.input_debounce_ms = 25;
    cfg.output_settle_ms  = 100;
    cfg.fault_confirm_ms  = 500;
    cfg.ign_mode          = IGN_MOMENTARY;
    cfg.ign_brake_ch      = BRAKE_CH;
    cfg.ign_start_ch      = START_CH;
    cfg.ign_run_ch        = with_run_channel ? RUN_CH : IGN_CH_NONE;
    cfg.ign_run_out_ch    = RUNOUT_CH;
    cfg.ign_shutdown_ms   = 3000;
    cfg.ign_idle_timeout_s = 0;          /* off unless a test asks for it */
    cfg.ecu_rpm_can_id     = 0;
    cfg.ign_run_rpm        = 400;
    cfg.ign_hold_stop_ms  = HOLD_MS;
    cfg.ign_crank_max_ms  = CRANK_MS;
    cfg.ign_off_hold_ms   = 2000;

    for (uint8_t i = 0; i < RCM_CHANNELS; i++) {
        cfg.ch[i].mode = CH_OUTPUT;
        SIM.wiring[i]  = SIM_COIL_OK;
    }
    cfg.ch[BRAKE_CH].mode = CH_INPUT; SIM.wiring[BRAKE_CH] = SIM_BUTTON_OPEN;
    cfg.ch[RUN_CH].mode   = CH_INPUT; SIM.wiring[RUN_CH]   = SIM_BUTTON_OPEN;

    ch_begin();
    sr_outputs_enable(true);

    sw = true;                      /* the press that woke the board */
    ign_begin(true);
    tick(50);
    sw = false;                     /* released -- now armed */
    tick(50);
}

void setUp(void)    { sim_reset(); sr_begin(); sw = false; }
void tearDown(void) {}

/* --- maintained mode -------------------------------------------------------- */

static void test_maintained_shuts_down_after_the_level_goes(void)
{
    memset(&cfg, 0, sizeof(cfg));
    cfg.ign_mode = IGN_MAINTAINED;
    cfg.ign_off_hold_ms = 2000;
    cfg.ign_brake_ch = cfg.ign_start_ch = cfg.ign_run_ch = IGN_CH_NONE;
    ch_begin();
    sw = true;
    ign_begin(true);

    tick(5000);
    TEST_ASSERT_FALSE_MESSAGE(ign_wants_shutdown(), "shut down while ignition was on");

    sw = false;
    tick(1500);
    TEST_ASSERT_FALSE_MESSAGE(ign_wants_shutdown(), "shut down before the hold expired");
    tick(1000);
    TEST_ASSERT_TRUE(ign_wants_shutdown());
}

static void test_maintained_ignores_a_brief_dropout(void)
{
    memset(&cfg, 0, sizeof(cfg));
    cfg.ign_mode = IGN_MAINTAINED;
    cfg.ign_off_hold_ms = 2000;
    cfg.ign_brake_ch = cfg.ign_start_ch = cfg.ign_run_ch = IGN_CH_NONE;
    ch_begin();
    sw = true;
    ign_begin(true);
    tick(500);

    for (int i = 0; i < 5; i++) { sw = false; tick(200); sw = true; tick(500); }
    TEST_ASSERT_FALSE_MESSAGE(ign_wants_shutdown(), "a bouncing key shut the board down");
}

/* --- momentary: the wake press ---------------------------------------------- */

static void test_the_wake_press_is_consumed(void)
{
    /* The press that woke the board through the hardware latch is still happening when
     * firmware starts. If it counted as a command, waking with the brake held would go
     * straight to cranking -- which is the single worst bug this module could have. */
    memset(&cfg, 0, sizeof(cfg));
    cfg.input_debounce_ms = 25;
    cfg.ign_mode = IGN_MOMENTARY;
    cfg.ign_brake_ch = BRAKE_CH; cfg.ign_start_ch = START_CH;
    cfg.ign_run_ch = IGN_CH_NONE;
    cfg.ign_hold_stop_ms = HOLD_MS; cfg.ign_crank_max_ms = CRANK_MS;
    for (uint8_t i = 0; i < RCM_CHANNELS; i++) cfg.ch[i].mode = CH_OUTPUT;
    cfg.ch[BRAKE_CH].mode = CH_INPUT;
    SIM.wiring[BRAKE_CH] = SIM_BUTTON_PRESSED;      /* brake held during the wake */
    ch_begin();
    sr_outputs_enable(true);

    sw = true;
    ign_begin(true);
    tick(400);

    TEST_ASSERT_FALSE_MESSAGE(sim_driver_on(START_CH), "cranked on the wake press");
    TEST_ASSERT_FALSE(ign_wants_shutdown());
    TEST_ASSERT_EQUAL(IGN_ST_IGNITION, ign_state());
}

static void test_holding_the_wake_press_does_not_shut_the_board_down(void)
{
    /* THE case the `armed` flag exists for, and the one the test above does not reach.
     *
     * People hold a start button rather than tapping it. The board boots while it is
     * still held, and press_ms starts at zero -- so without the guard the hold-to-stop
     * gesture fires about a second later and switches the car straight back off. That
     * presents as "it won't start", and you would look at the hardware first.
     *
     * Note the other guard, sw_prev being seeded from the boot state, already stops the
     * wake press LOOKING like a rising edge. This is the half it does not cover. */
    memset(&cfg, 0, sizeof(cfg));
    cfg.input_debounce_ms = 25;
    cfg.ign_mode = IGN_MOMENTARY;
    cfg.ign_brake_ch = IGN_CH_NONE; cfg.ign_start_ch = IGN_CH_NONE;
    cfg.ign_run_ch = IGN_CH_NONE;
    cfg.ign_hold_stop_ms = HOLD_MS; cfg.ign_crank_max_ms = CRANK_MS;
    ch_begin();

    sw = true;
    ign_begin(true);
    tick(HOLD_MS * 3);
    TEST_ASSERT_FALSE_MESSAGE(ign_wants_shutdown(),
                              "holding the wake button shut the board down again");

    /* And once it has been released, a deliberate hold still works. */
    sw = false; tick(100);
    sw = true;  tick(HOLD_MS + 100);
    TEST_ASSERT_TRUE_MESSAGE(ign_wants_shutdown(), "hold-to-stop stopped working");
}

static void test_a_wake_press_the_boot_sample_missed_is_still_consumed(void)
{
    /* ign_begin() samples the ignition ONCE. That sample is not trustworthy: a short
     * press, or 12V still climbing through the 6V threshold at that instant, reads LOW
     * while the button is in fact still down.
     *
     * Arming on it made the wake press itself look like a fresh rising edge on an armed
     * button. Brake up means shut down, so the board woke, immediately requested a
     * shutdown, and died as soon as the button was released. It presents as a board that
     * will not wake at all -- and you go looking at the latch, the supply and the
     * BTS7040 before you suspect the state machine. Found on the bench doing exactly
     * that. */
    memset(&cfg, 0, sizeof(cfg));
    cfg.input_debounce_ms = 25;
    cfg.ign_mode = IGN_MOMENTARY;
    cfg.ign_brake_ch = IGN_CH_NONE; cfg.ign_start_ch = IGN_CH_NONE;
    cfg.ign_run_ch = IGN_CH_NONE;
    cfg.ign_hold_stop_ms = HOLD_MS; cfg.ign_crank_max_ms = CRANK_MS;
    ch_begin();

    sw = false;                 /* what the boot sample wrongly reads */
    ign_begin(false);
    sw = true;                  /* the truth: the button is still down */
    tick(200);
    TEST_ASSERT_FALSE_MESSAGE(ign_wants_shutdown(),
                              "a mis-sampled wake press switched the board off");

    sw = false; tick(100);      /* the release is what arms it */
    sw = true;  tick(100);      /* and now a real press means something */
    TEST_ASSERT_TRUE_MESSAGE(ign_wants_shutdown(),
                             "the button never armed after the wake press ended");
}

static void test_a_press_that_ended_before_boot_still_arms(void)
{
    /* The other half, and the reason arming watches the LEVEL rather than a falling
     * edge. A press brief enough to latch the board and be over before firmware runs
     * never produces an edge for anyone to see. Waiting for one would leave the button
     * dead forever -- the board would come up and then ignore you. */
    memset(&cfg, 0, sizeof(cfg));
    cfg.input_debounce_ms = 25;
    cfg.ign_mode = IGN_MOMENTARY;
    cfg.ign_brake_ch = IGN_CH_NONE; cfg.ign_start_ch = IGN_CH_NONE;
    cfg.ign_run_ch = IGN_CH_NONE;
    cfg.ign_hold_stop_ms = HOLD_MS; cfg.ign_crank_max_ms = CRANK_MS;
    ch_begin();

    sw = false;                 /* already over by the time we run */
    ign_begin(false);
    tick(100);                  /* no falling edge will ever arrive */

    sw = true; tick(100);
    TEST_ASSERT_TRUE_MESSAGE(ign_wants_shutdown(),
                             "the button never armed, so the board ignores it forever");
}

/* --- momentary: starting ---------------------------------------------------- */

static void test_press_with_brake_cranks(void)
{
    momentary_setup(true);
    set_brake(true);

    sw = true;
    tick(TICK_MS * 4);
    TEST_ASSERT_EQUAL(IGN_ST_CRANKING, ign_state());
    TEST_ASSERT_TRUE(sim_driver_on(START_CH));

    sw = false; tick(TICK_MS * 4);
    TEST_ASSERT_TRUE_MESSAGE(sim_driver_on(START_CH),
                             "with a run signal, cranking should continue after release");

    set_running(true);
    TEST_ASSERT_EQUAL(IGN_ST_RUNNING, ign_state());
    TEST_ASSERT_FALSE_MESSAGE(sim_driver_on(START_CH), "starter still engaged when running");
}

static void test_press_without_brake_shuts_down(void)
{
    momentary_setup(true);
    TEST_ASSERT_FALSE(ign_wants_shutdown());

    press(TICK_MS * 4);
    TEST_ASSERT_TRUE(ign_wants_shutdown());
    TEST_ASSERT_FALSE_MESSAGE(sim_driver_on(START_CH), "cranked without the brake");
}

static void test_cranking_gives_up_on_timeout(void)
{
    momentary_setup(true);
    set_brake(true);
    sw = true; tick(TICK_MS * 4); sw = false; tick(TICK_MS * 4);
    TEST_ASSERT_TRUE(sim_driver_on(START_CH));

    tick(CRANK_MS + 200);
    TEST_ASSERT_FALSE_MESSAGE(sim_driver_on(START_CH), "starter left engaged past timeout");
    TEST_ASSERT_EQUAL(IGN_ST_IGNITION, ign_state());
}

static void test_cranking_survives_the_brake_going_away(void)
{
    /* The brake gates the START, not the continuation -- and this is not a preference,
     * it is forced by the hardware. The brake is a digital channel needing >10.87V at
     * the terminal, and a starter drags the battery to 9-10V, so the brake input goes
     * unreadable the instant cranking begins. An abort-on-brake-release would abort
     * EVERY start the moment the starter loaded the battery, and the car would never
     * fire. A key does not make you hold the brake through a crank either. */
    momentary_setup(true);
    set_brake(true);
    sw = true; tick(TICK_MS * 4); sw = false; tick(TICK_MS * 4);
    TEST_ASSERT_TRUE(sim_driver_on(START_CH));

    set_brake(false);                      /* what a sagging battery looks like */
    tick(500);
    TEST_ASSERT_TRUE_MESSAGE(sim_driver_on(START_CH),
                             "cranking aborted when the brake input dropped");

    set_running(true);                     /* and it still ends properly when it fires */
    TEST_ASSERT_FALSE(sim_driver_on(START_CH));
    TEST_ASSERT_EQUAL(IGN_ST_RUNNING, ign_state());
}

static void test_a_stale_state_cannot_crank_a_running_engine(void)
{
    /* A watchdog reset with the engine running comes back in IGN_ST_IGNITION, because
     * ign_begin() cannot know. If the no-crank-while-running guard looked at the STATE
     * it would be looking at a lie, and the next press with the brake down would throw
     * a starter pinion at a spinning ring gear. It looks at the run SIGNAL instead. */
    momentary_setup(true);
    set_running(true);                     /* engine is running... */
    ign_begin(false);                      /* ...and the board resets underneath it */
    tick(TICK_MS * 4);

    set_brake(true);
    press(TICK_MS * 4);
    TEST_ASSERT_FALSE_MESSAGE(sim_driver_on(START_CH),
                              "cranked a running engine after a reset");

    /* It should also have picked the running state back up on its own. */
    TEST_ASSERT_EQUAL_MESSAGE(IGN_ST_RUNNING, ign_state(),
                              "never noticed the engine was running");
}

static void test_without_a_run_signal_cranking_follows_the_button(void)
{
    /* No way to know when it caught, so the button behaves like a key's spring-return
     * START position. */
    momentary_setup(false);
    set_brake(true);

    sw = true; tick(TICK_MS * 4);
    TEST_ASSERT_TRUE(sim_driver_on(START_CH));
    tick(300);
    TEST_ASSERT_TRUE(sim_driver_on(START_CH));

    sw = false; tick(TICK_MS * 4);
    TEST_ASSERT_FALSE_MESSAGE(sim_driver_on(START_CH), "starter stayed on after release");
}

static void test_cranking_is_refused_when_half_configured(void)
{
    /* A starter channel with no brake channel must not crank. The dangerous capability
     * needs two deliberate settings, not one. */
    momentary_setup(true);
    cfg.ign_brake_ch = IGN_CH_NONE;
    set_brake(true);

    press(TICK_MS * 4);
    TEST_ASSERT_FALSE_MESSAGE(sim_driver_on(START_CH), "cranked with no brake configured");
    TEST_ASSERT_TRUE_MESSAGE(ign_wants_shutdown(),
                             "an unconfigured board should fall through to shutdown");
}

static void test_no_crank_from_running(void)
{
    momentary_setup(true);
    set_brake(true);
    sw = true; tick(TICK_MS * 4); sw = false; tick(TICK_MS * 4);
    set_running(true);
    TEST_ASSERT_EQUAL(IGN_ST_RUNNING, ign_state());

    press(TICK_MS * 4);                       /* short press, brake still held */
    TEST_ASSERT_FALSE_MESSAGE(sim_driver_on(START_CH),
                              "engaged the starter with the engine running");
}

/* --- the ECU owns the starter ----------------------------------------------- */

static void test_brake_held_press_never_shuts_down_when_the_ecu_cranks(void)
{
    /* The arrangement this car actually uses: rusEFI drives the starter relay, so
     * ign_start_ch is unconfigured and the ECU watches the same button.
     *
     * A press with the brake down is then somebody ELSE's start command. If this board
     * treated it as "turn off" it would drop the RUN output and kill the ECU in the
     * middle of its own crank -- and the car would be unstartable in a way that looked
     * like an ECU fault. */
    momentary_setup(true);
    cfg.ign_start_ch = IGN_CH_NONE;        /* the ECU cranks, not us */
    set_brake(true);

    press(TICK_MS * 4);
    TEST_ASSERT_FALSE_MESSAGE(ign_wants_shutdown(),
                              "a start attempt shut the car down instead");

    /* Brake up, though, still means off. */
    set_brake(false);
    press(TICK_MS * 4);
    TEST_ASSERT_TRUE_MESSAGE(ign_wants_shutdown(), "press without brake should stop it");
}

static void test_hold_still_stops_it_when_the_ecu_cranks(void)
{
    /* Deferring the press must not cost the stop gesture. */
    momentary_setup(true);
    cfg.ign_start_ch = IGN_CH_NONE;
    set_brake(true);

    sw = true; tick(HOLD_MS + 100);
    TEST_ASSERT_TRUE_MESSAGE(ign_wants_shutdown(), "hold-to-stop stopped working");
}

/* --- momentary: stopping ---------------------------------------------------- */

static void test_a_short_press_while_running_does_nothing(void)
{
    /* Brushing the button at speed must not cut the engine. */
    momentary_setup(true);
    set_brake(true);
    sw = true; tick(TICK_MS * 4); sw = false; tick(TICK_MS * 4);
    set_running(true);
    set_brake(false);

    press(HOLD_MS / 4);
    TEST_ASSERT_FALSE_MESSAGE(ign_wants_shutdown(), "a short press stopped the engine");
    TEST_ASSERT_EQUAL(IGN_ST_RUNNING, ign_state());
}

static void test_a_hold_while_running_stops_the_engine(void)
{
    momentary_setup(true);
    set_brake(true);
    sw = true; tick(TICK_MS * 4); sw = false; tick(TICK_MS * 4);
    set_running(true);
    set_brake(false);

    sw = true; tick(HOLD_MS + 100);
    TEST_ASSERT_TRUE(ign_wants_shutdown());
}

static void test_stopping_is_never_conditional(void)
{
    /* The core safety property. A hold must stop the engine whatever the sensors say --
     * run channel reading nonsense, brake held, mid-crank, all of it. Every one of
     * those is a state something could be stuck in when you most need to stop. */
    momentary_setup(true);
    set_brake(true);
    sw = true; tick(TICK_MS * 4); sw = false; tick(TICK_MS * 4);
    TEST_ASSERT_EQUAL(IGN_ST_CRANKING, ign_state());

    sw = true; tick(HOLD_MS + 100);
    TEST_ASSERT_TRUE_MESSAGE(ign_wants_shutdown(), "could not stop while cranking");
    TEST_ASSERT_FALSE_MESSAGE(sim_driver_on(START_CH),
                              "starter left engaged through a shutdown");
}

static void test_stopping_works_with_no_run_channel_at_all(void)
{
    momentary_setup(false);
    sw = true; tick(HOLD_MS + 100);
    TEST_ASSERT_TRUE(ign_wants_shutdown());
}

/* --- engine-running from CAN RPM -------------------------------------------- */

static void test_can_rpm_ends_a_crank(void)
{
    /* No wired run signal at all -- the engine-running verdict comes from rusEFI's
     * broadcast. 0x201 low 16 bits, 1 rpm per count. */
    momentary_setup(false);
    cfg.ecu_rpm_can_id = 0x201;
    cfg.ign_run_rpm    = 400;

    set_brake(true);
    sw = true; tick(TICK_MS * 4); sw = false; tick(TICK_MS * 4);
    TEST_ASSERT_TRUE(sim_driver_on(START_CH));

    ign_note_rpm(250, SIM.now_ms);          /* cranking speed, not running */
    tick(TICK_MS * 4);
    TEST_ASSERT_TRUE_MESSAGE(sim_driver_on(START_CH), "gave up at cranking speed");

    ign_note_rpm(900, SIM.now_ms);          /* caught */
    tick(TICK_MS * 4);
    TEST_ASSERT_FALSE_MESSAGE(sim_driver_on(START_CH), "starter held on after it fired");
    TEST_ASSERT_EQUAL(IGN_ST_RUNNING, ign_state());
}

static void test_can_rpm_blocks_cranking_a_running_engine(void)
{
    momentary_setup(false);
    cfg.ecu_rpm_can_id = 0x201;
    cfg.ign_run_rpm    = 400;

    ign_note_rpm(2000, SIM.now_ms);
    tick(TICK_MS * 4);
    TEST_ASSERT_EQUAL(IGN_ST_RUNNING, ign_state());

    set_brake(true);
    press(TICK_MS * 4);
    TEST_ASSERT_FALSE_MESSAGE(sim_driver_on(START_CH), "cranked against a running engine");
}

static void test_stale_can_rpm_is_not_running(void)
{
    /* The weakness worth knowing about. If the bus stops while the engine turns, this
     * goes stale and the board believes the engine has stopped -- which is exactly why
     * a wired run signal is the one to use if this board turns the starter. The test
     * pins the behaviour so nobody is surprised by it. */
    momentary_setup(false);
    cfg.ecu_rpm_can_id = 0x201;
    cfg.ign_run_rpm    = 400;

    ign_note_rpm(2000, SIM.now_ms);
    tick(TICK_MS * 4);
    TEST_ASSERT_EQUAL(IGN_ST_RUNNING, ign_state());

    tick(1000);                              /* bus goes quiet */
    TEST_ASSERT_EQUAL_MESSAGE(IGN_ST_IGNITION, ign_state(),
                              "stale RPM should not still read as running");
}

static void test_a_wired_run_signal_still_wins(void)
{
    /* Wired and CAN are ORed, so a wired signal keeps saying "running" through a bus
     * outage -- which is the whole reason to fit one. */
    momentary_setup(true);
    cfg.ecu_rpm_can_id = 0x201;
    cfg.ign_run_rpm    = 400;
    set_running(true);
    tick(TICK_MS * 4);
    TEST_ASSERT_EQUAL(IGN_ST_RUNNING, ign_state());

    tick(2000);                              /* no RPM frames ever arrive */
    TEST_ASSERT_EQUAL_MESSAGE(IGN_ST_RUNNING, ign_state(),
                              "a wired run signal was lost to CAN staleness");
}

/* --- idle timeout ----------------------------------------------------------- */

static void test_idle_timeout_shuts_an_unattended_board_down(void)
{
    /* Wake the car, wander off without starting it. The board draws ~100mA plus
     * whatever channels are on -- that is a flat battery by morning. Real keyless cars
     * drop out of accessory after a few minutes for the same reason. */
    momentary_setup(true);
    cfg.ign_idle_timeout_s = 10;

    tick(9000);
    TEST_ASSERT_FALSE_MESSAGE(ign_wants_shutdown(), "timed out early");
    tick(2000);
    TEST_ASSERT_TRUE_MESSAGE(ign_wants_shutdown(), "never timed out");
}

static void test_a_running_engine_is_never_idle(void)
{
    /* The obvious way to get this wrong is to switch the car off in the middle of a
     * drive because nobody touched the button for half an hour. */
    momentary_setup(true);
    cfg.ign_idle_timeout_s = 10;
    set_brake(true);
    sw = true; tick(TICK_MS * 4); sw = false; tick(TICK_MS * 4);
    set_running(true);

    tick(30000);
    TEST_ASSERT_FALSE_MESSAGE(ign_wants_shutdown(), "shut down a running engine");
}

static void test_activity_defers_the_idle_timeout(void)
{
    momentary_setup(true);
    cfg.ign_idle_timeout_s = 10;

    for (int i = 0; i < 5; i++) {          /* a CAN frame every 6s */
        tick(6000);
        ign_note_activity(SIM.now_ms);
    }
    TEST_ASSERT_FALSE_MESSAGE(ign_wants_shutdown(), "timed out while in use");

    tick(11000);
    TEST_ASSERT_TRUE(ign_wants_shutdown());
}

static void test_no_idle_timeout_without_a_run_signal(void)
{
    /* THE dangerous case. With no run channel the board cannot tell an unattended
     * driveway from half an hour of driving -- `running` is false either way. Letting
     * the timeout fire there would switch the car off at speed. It stays inert instead:
     * a flat battery is a far better failure than an engine cut on a motorway. */
    momentary_setup(false);                /* no run channel */
    cfg.ign_idle_timeout_s = 10;

    tick(60000);                           /* six timeouts' worth */
    TEST_ASSERT_FALSE_MESSAGE(ign_wants_shutdown(),
                              "timed out with no way to know the engine was running");
}

static void test_no_idle_timeout_when_the_bus_dies(void)
{
    /* The same hazard as having no run source at all, reached a different way. CAN RPM
     * is CONFIGURED, so the old guard was satisfied -- but the bus has stopped, so RPM
     * reads stale, which is indistinguishable from a stopped engine. Letting the
     * timeout run there would shut the car down mid-drive because the bus dropped. */
    momentary_setup(false);
    cfg.ecu_rpm_can_id     = 0x201;
    cfg.ign_run_rpm        = 400;
    cfg.ign_idle_timeout_s = 10;

    ign_note_rpm(2500, SIM.now_ms);        /* engine was running... */
    tick(60000);                           /* ...and then the bus went quiet */
    TEST_ASSERT_FALSE_MESSAGE(ign_wants_shutdown(),
                              "a dead bus shut the car down");
}

static void test_idle_timeout_still_works_on_a_live_bus(void)
{
    /* And the gate must not disable the feature outright: with RPM arriving and reading
     * stopped, an unattended board should still time out. */
    momentary_setup(false);
    cfg.ecu_rpm_can_id     = 0x201;
    cfg.ign_run_rpm        = 400;
    cfg.ign_idle_timeout_s = 10;

    for (int i = 0; i < 60; i++) {         /* 12s of "engine stopped" at 5Hz */
        ign_note_rpm(0, SIM.now_ms);
        tick(200);
    }
    TEST_ASSERT_TRUE_MESSAGE(ign_wants_shutdown(), "never timed out on a live bus");
}

static void test_idle_timeout_is_momentary_only(void)
{
    /* In maintained mode the switch is physically closed, so a shutdown could not
     * complete anyway -- dropping LATCH_HOLD just power-cycles. The board would sit
     * awake with every channel off, which is worse than leaving it alone. */
    memset(&cfg, 0, sizeof(cfg));
    cfg.ign_mode = IGN_MAINTAINED;
    cfg.ign_off_hold_ms = 2000;
    cfg.ign_idle_timeout_s = 5;
    cfg.ign_brake_ch = cfg.ign_start_ch = cfg.ign_run_ch = IGN_CH_NONE;
    cfg.ign_run_out_ch = IGN_CH_NONE;
    ch_begin();
    sw = true;
    ign_begin(true);

    tick(20000);
    TEST_ASSERT_FALSE_MESSAGE(ign_wants_shutdown(),
                              "idle timeout fired with the key still on");
}

/* --- the RUN position output ------------------------------------------------ */

static void test_run_output_is_held_while_awake(void)
{
    /* This is what feeds the ECU's ignition input -- the key's RUN position. It must
     * be on the whole time the board is awake. */
    momentary_setup(true);
    tick(100);
    TEST_ASSERT_TRUE_MESSAGE(sim_driver_on(RUNOUT_CH), "RUN output was not asserted");
}

static void test_run_output_survives_a_failsafe(void)
{
    /* failsafe_state is applied at boot and on every bus timeout. If it could clear the
     * RUN output, a moment of CAN silence would switch the ignition off and stop the
     * engine -- so the ignition state machine owns that channel outright. */
    momentary_setup(true);
    cfg.failsafe_state = 0;                   /* "everything off" */
    ch_apply_failsafe();
    tick(TICK_MS * 4);
    TEST_ASSERT_TRUE_MESSAGE(sim_driver_on(RUNOUT_CH),
                             "a failsafe switched the ECU's ignition feed off");
}

static void test_stopping_drops_run_first_and_waits(void)
{
    /* Order matters: RUN goes immediately so the ECU sees ignition-off, and the board
     * stays powered for ign_shutdown_ms afterwards so it can finish. Cutting the rail
     * straight away would be pulling the battery lead on every switch-off. */
    momentary_setup(true);
    tick(100);
    TEST_ASSERT_TRUE(sim_driver_on(RUNOUT_CH));

    sw = true; tick(HOLD_MS + 100);
    TEST_ASSERT_TRUE(ign_wants_shutdown());
    TEST_ASSERT_FALSE_MESSAGE(sim_driver_on(RUNOUT_CH), "RUN was not dropped on stop");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, ign_shutdown_since(), "shutdown time not recorded");
}

static void test_power_cannot_be_cut_while_the_button_is_held(void)
{
    /* Electrical, not cosmetic. LATCH_HOLD reaches the BTS7040 through 1k while the
     * switch reaches it through 47k, so with the switch closed, dropping LATCH_HOLD
     * pulls LATCH_IN to ~0.24V -- the latch releases, the MCU dies, its pin goes
     * high-impedance, and R_LIGN/R_LPD immediately put LATCH_IN back to 3.83V and
     * switch the board on again. A power CYCLE, not a power off.
     *
     * So the board must wait for the release. Anything else is a reboot loop for as
     * long as somebody leans on the button. */
    momentary_setup(true);

    sw = true; tick(HOLD_MS + 100);              /* hold to stop, and keep holding */
    TEST_ASSERT_TRUE(ign_wants_shutdown());

    tick(cfg.ign_shutdown_ms + 500);             /* well past the ECU window */
    TEST_ASSERT_FALSE_MESSAGE(ign_may_cut_power(SIM.now_ms),
                              "would have cut power with the button still held");

    sw = false; tick(TICK_MS * 4);
    TEST_ASSERT_TRUE_MESSAGE(ign_may_cut_power(SIM.now_ms),
                             "did not power down after the button was released");
}

static void test_power_is_not_cut_before_the_ecu_window(void)
{
    momentary_setup(true);
    sw = true; tick(HOLD_MS + 100);
    sw = false; tick(TICK_MS * 4);
    TEST_ASSERT_FALSE_MESSAGE(ign_may_cut_power(SIM.now_ms),
                              "cut power before the ECU had its shutdown window");
    tick(cfg.ign_shutdown_ms);
    TEST_ASSERT_TRUE(ign_may_cut_power(SIM.now_ms));
}

/* --- the starter must never be a resting state ------------------------------ */

static void test_failsafe_can_never_engage_the_starter(void)
{
    /* failsafe_state is applied at power-up and whenever the bus goes quiet. A stray
     * bit for the starter channel would mean a board that cranks on boot, and cranks
     * again every time CAN hiccups. ch_apply_failsafe() forces it off regardless. */
    momentary_setup(true);
    cfg.failsafe_state = 0x1FFFFF;            /* ask for absolutely everything */

    ch_apply_failsafe();
    tick(TICK_MS * 4);

    TEST_ASSERT_FALSE_MESSAGE(sim_driver_on(START_CH),
                              "failsafe_state engaged the starter motor");
    TEST_ASSERT_TRUE_MESSAGE(sim_driver_on(0), "other channels should still apply");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_maintained_shuts_down_after_the_level_goes);
    RUN_TEST(test_maintained_ignores_a_brief_dropout);
    RUN_TEST(test_the_wake_press_is_consumed);
    RUN_TEST(test_holding_the_wake_press_does_not_shut_the_board_down);
    RUN_TEST(test_a_wake_press_the_boot_sample_missed_is_still_consumed);
    RUN_TEST(test_a_press_that_ended_before_boot_still_arms);
    RUN_TEST(test_press_with_brake_cranks);
    RUN_TEST(test_press_without_brake_shuts_down);
    RUN_TEST(test_cranking_gives_up_on_timeout);
    RUN_TEST(test_cranking_survives_the_brake_going_away);
    RUN_TEST(test_a_stale_state_cannot_crank_a_running_engine);
    RUN_TEST(test_without_a_run_signal_cranking_follows_the_button);
    RUN_TEST(test_cranking_is_refused_when_half_configured);
    RUN_TEST(test_no_crank_from_running);
    RUN_TEST(test_brake_held_press_never_shuts_down_when_the_ecu_cranks);
    RUN_TEST(test_hold_still_stops_it_when_the_ecu_cranks);
    RUN_TEST(test_a_short_press_while_running_does_nothing);
    RUN_TEST(test_a_hold_while_running_stops_the_engine);
    RUN_TEST(test_stopping_is_never_conditional);
    RUN_TEST(test_stopping_works_with_no_run_channel_at_all);
    RUN_TEST(test_can_rpm_ends_a_crank);
    RUN_TEST(test_can_rpm_blocks_cranking_a_running_engine);
    RUN_TEST(test_stale_can_rpm_is_not_running);
    RUN_TEST(test_a_wired_run_signal_still_wins);
    RUN_TEST(test_idle_timeout_shuts_an_unattended_board_down);
    RUN_TEST(test_a_running_engine_is_never_idle);
    RUN_TEST(test_activity_defers_the_idle_timeout);
    RUN_TEST(test_no_idle_timeout_without_a_run_signal);
    RUN_TEST(test_no_idle_timeout_when_the_bus_dies);
    RUN_TEST(test_idle_timeout_still_works_on_a_live_bus);
    RUN_TEST(test_idle_timeout_is_momentary_only);
    RUN_TEST(test_power_cannot_be_cut_while_the_button_is_held);
    RUN_TEST(test_power_is_not_cut_before_the_ecu_window);
    RUN_TEST(test_run_output_is_held_while_awake);
    RUN_TEST(test_run_output_survives_a_failsafe);
    RUN_TEST(test_stopping_drops_run_first_and_waits);
    RUN_TEST(test_failsafe_can_never_engage_the_starter);
    return UNITY_END();
}
