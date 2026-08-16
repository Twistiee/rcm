/*
 * test_behaviour -- flash, pulse and delay-off, and the traps they set.
 *
 * These exist mainly so indicators are possible at all, but the interesting part is not
 * the blinking. It is that a channel whose physical state now moves on its own breaks
 * anything that assumed "what was asked for" and "what the driver is doing" were the
 * same number. Toggling and fault diagnosis both made that assumption.
 */
#include <unity.h>
#include <cstdio>

#include "../stubs/simboard.cpp"
#include "../../src/shiftreg.cpp"

#include "config.h"
struct rcm_config_t cfg;
struct rcm_straps_t straps;

#include "../../src/channels.cpp"

#define FLASH_MS 800

static void run_ms(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += TICK_MS) {
        SIM.now_ms += TICK_MS;
        ch_tick(SIM.now_ms);
    }
}

static void base_cfg(void)
{
    memset(&cfg, 0, sizeof(cfg));
    cfg.input_debounce_ms = 25;
    cfg.output_settle_ms  = 100;
    cfg.fault_confirm_ms  = 500;
    cfg.flash_period_ms   = FLASH_MS;
    for (uint8_t i = 0; i < RCM_CHANNELS; i++) {
        cfg.ch[i].mode = CH_OUTPUT;
        SIM.wiring[i]  = SIM_COIL_OK;
    }
}

/* How many times did this channel change state over the window? */
static int count_edges(uint8_t ch, uint32_t ms)
{
    int edges = 0;
    bool prev = sim_driver_on(ch);
    for (uint32_t t = 0; t < ms; t += TICK_MS) {
        SIM.now_ms += TICK_MS;
        ch_tick(SIM.now_ms);
        const bool now = sim_driver_on(ch);
        if (now != prev) edges++;
        prev = now;
    }
    return edges;
}

void setUp(void)
{
    sim_reset();
    base_cfg();
    sr_begin();
    ch_begin();
    sr_outputs_enable(true);
}
void tearDown(void) {}

/* --- steady is untouched ---------------------------------------------------- */

static void test_steady_is_still_just_on(void)
{
    ch_command(2, true);
    run_ms(TICK_MS * 2);          /* let the command reach the pins first -- otherwise
                                   * count_edges sees the initial off->on and reports it
                                   * as the channel moving by itself */
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_edges(2, FLASH_MS * 3),
                                  "a steady channel moved on its own");
    TEST_ASSERT_TRUE(sim_driver_on(2));
}

/* --- flash ------------------------------------------------------------------ */

static void test_flash_blinks_at_the_configured_rate(void)
{
    cfg.ch[5].behaviour = OUT_FLASH;
    ch_command(5, true);
    /* Four periods is eight edges, give or take one at the boundaries. */
    const int edges = count_edges(5, FLASH_MS * 4);
    TEST_ASSERT_INT_WITHIN_MESSAGE(1, 8, edges, "wrong flash rate");
}

static void test_flash_stops_dead_when_released(void)
{
    cfg.ch[5].behaviour = OUT_FLASH;
    ch_command(5, true);
    run_ms(FLASH_MS * 2);
    ch_command(5, false);
    run_ms(TICK_MS * 2);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_edges(5, FLASH_MS * 3), "kept flashing");
    TEST_ASSERT_FALSE(sim_driver_on(5));
}

static void test_flashing_channels_stay_in_phase(void)
{
    /* Hazards. Two indicators switched on at different moments must still blink
     * together, which is why the period is shared config rather than a per-channel
     * timer started from whenever that channel happened to come on. */
    cfg.ch[5].behaviour = OUT_FLASH;
    cfg.ch[6].behaviour = OUT_FLASH;

    ch_command(5, true);
    run_ms(FLASH_MS / 3);                  /* deliberately not on a boundary */
    ch_command(6, true);
    run_ms(FLASH_MS);

    for (int i = 0; i < 40; i++) {
        run_ms(TICK_MS * 3);
        TEST_ASSERT_EQUAL_MESSAGE(sim_driver_on(5), sim_driver_on(6),
                                  "indicators drifted out of phase");
    }
}

/* --- pulse ------------------------------------------------------------------ */

static void test_pulse_fires_once_and_stops(void)
{
    cfg.ch[7].behaviour = OUT_PULSE;
    cfg.ch[7].param     = 300;             /* a horn chirp */

    ch_command(7, true);
    run_ms(100);
    TEST_ASSERT_TRUE(sim_driver_on(7));
    run_ms(400);
    TEST_ASSERT_FALSE_MESSAGE(sim_driver_on(7), "pulse never ended");

    /* Still held, so it must not fire again. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_edges(7, 2000), "pulse re-triggered while held");
}

static void test_pulse_rearms_on_the_next_press(void)
{
    cfg.ch[7].behaviour = OUT_PULSE;
    cfg.ch[7].param     = 300;

    ch_command(7, true);  run_ms(500);
    ch_command(7, false); run_ms(TICK_MS * 2);
    ch_command(7, true);  run_ms(100);
    TEST_ASSERT_TRUE_MESSAGE(sim_driver_on(7), "second press did nothing");
}

/* --- delay off -------------------------------------------------------------- */

static void test_delay_off_lingers_then_goes(void)
{
    cfg.ch[8].behaviour = OUT_DELAY_OFF;
    cfg.ch[8].param     = 1000;            /* courtesy light */

    ch_command(8, true);
    run_ms(200);
    TEST_ASSERT_TRUE(sim_driver_on(8));

    ch_command(8, false);
    run_ms(500);
    TEST_ASSERT_TRUE_MESSAGE(sim_driver_on(8), "went out immediately");
    run_ms(700);
    TEST_ASSERT_FALSE_MESSAGE(sim_driver_on(8), "never went out");
}

static void test_all_off_beats_a_delayed_off(void)
{
    /* ch_all_off() is what a bus timeout and a shutdown call. A channel that lingered
     * through it would still be lit as the board cut its own power. */
    cfg.ch[8].behaviour = OUT_DELAY_OFF;
    cfg.ch[8].param     = 5000;
    ch_command(8, true);
    run_ms(100);

    ch_all_off();
    run_ms(TICK_MS * 2);
    TEST_ASSERT_FALSE_MESSAGE(sim_driver_on(8), "lingered through an all-off");
}

/* --- the traps -------------------------------------------------------------- */

static void test_requested_is_stable_while_the_output_flashes(void)
{
    /* Toggle logic reads ch_requested(). Reading ch_commanded() instead would sample a
     * flashing indicator mid-blink, and the answer would depend on when you asked. */
    cfg.ch[5].behaviour = OUT_FLASH;
    ch_command(5, true);
    for (int i = 0; i < 40; i++) {
        run_ms(TICK_MS * 5);
        TEST_ASSERT_EQUAL_HEX32_MESSAGE(1ul << 5, ch_requested() & (1ul << 5),
                                        "request wobbled while flashing");
    }
}

static void test_a_flashing_channel_is_not_diagnosed_as_faulty(void)
{
    /* The coil-circuit check compares the commanded state against the sense line. A
     * channel that switches itself twice a second spends much of its life inside
     * output_settle_ms, and must not accumulate a fault from a perfectly good circuit. */
    cfg.ch[5].behaviour = OUT_FLASH;
    SIM.wiring[5] = SIM_COIL_OK;
    ch_command(5, true);
    run_ms(cfg.output_settle_ms + cfg.fault_confirm_ms + FLASH_MS * 6);
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(0, ch_fault_open() & (1ul << 5),
                                    "flashing channel reported an open circuit");
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(0, ch_fault_short() & (1ul << 5),
                                    "flashing channel reported a short");
}

static void test_behaviours_are_ignored_on_input_channels(void)
{
    cfg.ch[9].mode      = CH_INPUT;
    cfg.ch[9].behaviour = OUT_FLASH;
    ch_command(9, true);
    run_ms(FLASH_MS * 2);
    TEST_ASSERT_FALSE_MESSAGE(sim_driver_on(9), "an input channel was driven");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_steady_is_still_just_on);
    RUN_TEST(test_flash_blinks_at_the_configured_rate);
    RUN_TEST(test_flash_stops_dead_when_released);
    RUN_TEST(test_flashing_channels_stay_in_phase);
    RUN_TEST(test_pulse_fires_once_and_stops);
    RUN_TEST(test_pulse_rearms_on_the_next_press);
    RUN_TEST(test_delay_off_lingers_then_goes);
    RUN_TEST(test_all_off_beats_a_delayed_off);
    RUN_TEST(test_requested_is_stable_while_the_output_flashes);
    RUN_TEST(test_a_flashing_channel_is_not_diagnosed_as_faulty);
    RUN_TEST(test_behaviours_are_ignored_on_input_channels);
    return UNITY_END();
}
