/*
 * test_lights -- headlights driven by this board from their own switches.
 *
 * The switches on this car switch GROUND -- their common is shared with park and cannot
 * be changed -- so they come in on J_AUX as active-low inputs through a 10 k pull-up.
 * On the sim, a pin at +12 V is SIM.aux = 1, so "switch closed" here is aux = 0.
 */
#include <unity.h>
#include <cstdio>

#include "../stubs/simboard.cpp"
#include "../../src/shiftreg.cpp"

#include "config.h"
struct rcm_config_t cfg;
struct rcm_straps_t straps;

#include "../../src/channels.cpp"

/* lights.cpp asks two other modules whether a start is in progress. Faked here. */
static bool PEER_STARTING = false;
static bool SELF_STARTING = false;
bool proto_peer_starting(uint32_t) { return PEER_STARTING; }
bool ign_starting(void)            { return SELF_STARTING; }
static uint32_t FOLLOW_REQ = 0;     /* what follow slots are asking for, per channel */
bool proto_follow_requests(uint8_t ch) { return (FOLLOW_REQ >> ch) & 1u; }

#include "../../src/lights.cpp"

#define LOW_OUT   2      /* ch 3 */
#define HIGH_OUT  3      /* ch 4 */
#define PIN_LOW   0      /* J_AUX 1 */
#define PIN_HIGH  1      /* J_AUX 2 */

static void run_ms(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += TICK_MS) {
        SIM.now_ms += TICK_MS;
        ch_tick(SIM.now_ms);
        lights_tick(SIM.now_ms);
    }
}

/* the light switch: active-low, so closed = pulled to ground */
static void low_switch(bool closed)  { SIM.aux[PIN_LOW]  = closed ? 0 : 1; }
static void high_switch(bool closed) { SIM.aux[PIN_HIGH] = closed ? 0 : 1; }

#define SETTLE (cfg.input_debounce_ms + TICK_MS * 6)

void setUp(void)
{
    sim_reset();
    memset(&cfg, 0, sizeof(cfg));
    cfg.input_debounce_ms = 25;
    cfg.output_settle_ms  = 100;
    cfg.fault_confirm_ms  = 500;
    for (uint8_t i = 0; i < RCM_CHANNELS; i++) {
        cfg.ch[i].mode = CH_OUTPUT;
        SIM.wiring[i]  = SIM_COIL_OK;
    }
    cfg.ch[LOW_OUT].func  = FN_HEADLIGHT_LOW;
    cfg.ch[HIGH_OUT].func = FN_HEADLIGHT_HIGH;
    cfg.aux_func[PIN_LOW]  = FN_IN_HEADLIGHT;
    cfg.aux_func[PIN_HIGH] = FN_IN_HIGH_BEAM;
    cfg.aux_invert = (1u << PIN_LOW) | (1u << PIN_HIGH);
    PEER_STARTING = SELF_STARTING = false;
    FOLLOW_REQ = 0;
    low_switch(false);
    high_switch(false);

    sr_begin();
    ch_begin();
    sr_outputs_enable(true);
}
void tearDown(void) {}

/* --- the switch drives the relay ------------------------------------------- */

static void test_low_beam_follows_its_switch(void)
{
    run_ms(SETTLE);
    TEST_ASSERT_FALSE(sim_driver_on(LOW_OUT));
    low_switch(true);
    run_ms(SETTLE);
    TEST_ASSERT_TRUE_MESSAGE(sim_driver_on(LOW_OUT), "switch closed, low beam not on");
    low_switch(false);
    run_ms(SETTLE);
    TEST_ASSERT_FALSE(sim_driver_on(LOW_OUT));
}

static void test_high_beam_is_its_own_switch(void)
{
    high_switch(true);
    run_ms(SETTLE);
    TEST_ASSERT_TRUE(sim_driver_on(HIGH_OUT));
    TEST_ASSERT_FALSE_MESSAGE(sim_driver_on(LOW_OUT), "high-beam switch lit the low beam");
}

/* --- shed for the crank ------------------------------------------------------ */

static void test_headlights_drop_while_the_keypad_is_starting(void)
{
    /* The whole point: the start button is on the keypad, which publishes "starting",
     * and this board drops the headlights for the crank even with the switch on. */
    low_switch(true);
    high_switch(true);
    run_ms(SETTLE);
    TEST_ASSERT_TRUE(sim_driver_on(LOW_OUT));

    PEER_STARTING = true;
    run_ms(TICK_MS * 2);
    TEST_ASSERT_FALSE_MESSAGE(sim_driver_on(LOW_OUT),  "low beam stayed on during the crank");
    TEST_ASSERT_FALSE_MESSAGE(sim_driver_on(HIGH_OUT), "high beam stayed on during the crank");

    PEER_STARTING = false;                       /* caught, or the keypad gave up */
    run_ms(TICK_MS * 2);
    TEST_ASSERT_TRUE_MESSAGE(sim_driver_on(LOW_OUT), "headlights did not come back");
    TEST_ASSERT_TRUE(sim_driver_on(HIGH_OUT));
}

static void test_headlights_also_drop_if_this_board_is_starting(void)
{
    low_switch(true);
    run_ms(SETTLE);
    SELF_STARTING = true;
    run_ms(TICK_MS * 2);
    TEST_ASSERT_FALSE(sim_driver_on(LOW_OUT));
}

static void test_park_is_never_touched(void)
{
    /* Park stays wired direct because it must work with the car off. This module must
     * not reach for it, even mid-crank. */
    cfg.ch[1].func = FN_TAIL;
    ch_command(1, true);
    PEER_STARTING = true;
    run_ms(SETTLE);
    TEST_ASSERT_TRUE_MESSAGE(sim_driver_on(1), "park was shed with the headlights");
}

/* --- leaving alone what it does not own -------------------------------------- */

static void test_without_a_switch_label_the_output_is_left_alone(void)
{
    /* A board not using this feature must be unaffected -- an output commanded some
     * other way must not be pinned off by a module with nothing to follow. */
    cfg.aux_func[PIN_LOW] = FN_NONE;
    ch_command(LOW_OUT, true);
    run_ms(SETTLE);
    TEST_ASSERT_TRUE(sim_driver_on(LOW_OUT));
}

static void test_a_switch_on_a_channel_works_too(void)
{
    /* Not this car, but the label can live on an ordinary active-high channel. */
    cfg.aux_func[PIN_LOW] = FN_NONE;
    cfg.ch[9].mode = CH_INPUT;
    cfg.ch[9].func = FN_IN_HEADLIGHT;
    SIM.wiring[9]  = SIM_BUTTON_PRESSED;
    run_ms(SETTLE);
    TEST_ASSERT_TRUE(sim_driver_on(LOW_OUT));
}

/* --- active-low J_AUX, and the boot blip ------------------------------------- */

static void test_an_active_low_pin_reads_pressed_when_grounded(void)
{
    low_switch(true);
    run_ms(SETTLE);
    TEST_ASSERT_TRUE(((ch_aux() >> PIN_LOW) & 1u) != 0);
    low_switch(false);
    run_ms(SETTLE);
    TEST_ASSERT_TRUE(((ch_aux() >> PIN_LOW) & 1u) == 0);
}

static void test_a_plain_pin_is_still_active_high(void)
{
    cfg.aux_invert = 0;
    SIM.aux[PIN_LOW] = 1;
    run_ms(SETTLE);
    TEST_ASSERT_TRUE(((ch_aux() >> PIN_LOW) & 1u) != 0);
}

static void test_no_headlight_blip_at_boot(void)
{
    /* The debounced state starts at zero, and on an inverted pin zero means PRESSED. If
     * that leaked out, every boot would flick the headlight relay on for the first
     * debounce period with the switch OFF. */
    low_switch(false);
    for (uint32_t t = 0; t < (uint32_t)cfg.input_debounce_ms + 40; t += TICK_MS) {
        run_ms(TICK_MS);
        TEST_ASSERT_FALSE_MESSAGE(sim_driver_on(LOW_OUT),
            "headlight relay energised during boot with the switch off");
    }
}

/* --- wired-OR with a follow slot: auto headlights from a keypad button ----------- */

static void test_auto_headlights_light_the_low_beam_with_the_switch_off(void)
{
    run_ms(SETTLE);
    FOLLOW_REQ = 1u << LOW_OUT;                  /* R2 on the keypad */
    run_ms(TICK_MS * 2);
    TEST_ASSERT_TRUE_MESSAGE(sim_driver_on(LOW_OUT), "auto headlights did not light the low beam");
}

static void test_the_switch_still_works_with_auto_headlights_off(void)
{
    low_switch(true);
    FOLLOW_REQ = 0;
    run_ms(SETTLE);
    TEST_ASSERT_TRUE(sim_driver_on(LOW_OUT));
}

static void test_a_crank_cuts_auto_headlights_too(void)
{
    FOLLOW_REQ = 1u << LOW_OUT;
    run_ms(SETTLE);
    PEER_STARTING = true;
    run_ms(TICK_MS * 2);
    TEST_ASSERT_FALSE_MESSAGE(sim_driver_on(LOW_OUT), "auto headlights stayed on during the crank");
}

static void test_it_only_claims_the_headlights_it_has_a_switch_for(void)
{
    /* Ownership is what tells the follow handling to stand back. It must cover exactly
     * the paired outputs, or a follow slot on some other channel would go dead. */
    TEST_ASSERT_TRUE(lights_owns(LOW_OUT));
    TEST_ASSERT_TRUE(lights_owns(HIGH_OUT));
    TEST_ASSERT_FALSE(lights_owns(0));
    cfg.aux_func[PIN_HIGH] = FN_NONE;
    TEST_ASSERT_FALSE_MESSAGE(lights_owns(HIGH_OUT), "claimed a headlight with no switch labelled");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_low_beam_follows_its_switch);
    RUN_TEST(test_high_beam_is_its_own_switch);
    RUN_TEST(test_headlights_drop_while_the_keypad_is_starting);
    RUN_TEST(test_headlights_also_drop_if_this_board_is_starting);
    RUN_TEST(test_park_is_never_touched);
    RUN_TEST(test_without_a_switch_label_the_output_is_left_alone);
    RUN_TEST(test_a_switch_on_a_channel_works_too);
    RUN_TEST(test_an_active_low_pin_reads_pressed_when_grounded);
    RUN_TEST(test_a_plain_pin_is_still_active_high);
    RUN_TEST(test_no_headlight_blip_at_boot);
    RUN_TEST(test_auto_headlights_light_the_low_beam_with_the_switch_off);
    RUN_TEST(test_the_switch_still_works_with_auto_headlights_off);
    RUN_TEST(test_a_crank_cuts_auto_headlights_too);
    RUN_TEST(test_it_only_claims_the_headlights_it_has_a_switch_for);
    return UNITY_END();
}
