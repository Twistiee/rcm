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

static void test_releasing_the_brake_stops_cranking(void)
{
    momentary_setup(true);
    set_brake(true);
    sw = true; tick(TICK_MS * 4); sw = false; tick(TICK_MS * 4);
    TEST_ASSERT_TRUE(sim_driver_on(START_CH));

    set_brake(false);
    TEST_ASSERT_FALSE(sim_driver_on(START_CH));
    TEST_ASSERT_EQUAL(IGN_ST_IGNITION, ign_state());
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
    RUN_TEST(test_press_with_brake_cranks);
    RUN_TEST(test_press_without_brake_shuts_down);
    RUN_TEST(test_cranking_gives_up_on_timeout);
    RUN_TEST(test_releasing_the_brake_stops_cranking);
    RUN_TEST(test_without_a_run_signal_cranking_follows_the_button);
    RUN_TEST(test_cranking_is_refused_when_half_configured);
    RUN_TEST(test_no_crank_from_running);
    RUN_TEST(test_a_short_press_while_running_does_nothing);
    RUN_TEST(test_a_hold_while_running_stops_the_engine);
    RUN_TEST(test_stopping_is_never_conditional);
    RUN_TEST(test_stopping_works_with_no_run_channel_at_all);
    RUN_TEST(test_run_output_is_held_while_awake);
    RUN_TEST(test_run_output_survives_a_failsafe);
    RUN_TEST(test_stopping_drops_run_first_and_waits);
    RUN_TEST(test_failsafe_can_never_engage_the_starter);
    return UNITY_END();
}
