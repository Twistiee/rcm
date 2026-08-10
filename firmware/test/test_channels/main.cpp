/*
 * test_channels -- the channel layer against a simulated relay/fuse box.
 *
 * The fuse detection here is not checked against a mock that says "pretend the fuse
 * is blown"; it is checked against a channel model where a blown fuse means the coil
 * cannot pull the node up, which is the actual physics the divider is reading. That
 * matters because the diagnosis is only valid while the driver is OFF, and it is very
 * easy to write a test that never exercises the case that makes it hard.
 */
#include <unity.h>
#include <cstdio>

#include "../stubs/simboard.cpp"
#include "../../src/shiftreg.cpp"

/* channels.cpp expects these to exist; config.cpp is not built here because it drags
 * in the EEPROM, which test_store covers separately. */
#include "config.h"
struct rcm_config_t cfg;
struct rcm_straps_t straps;

#include "../../src/channels.cpp"

#define SETTLE 100
#define CONFIRM 500

/* Set every channel to one mode AND wire the board up healthily to match.
 *
 * The healthy wiring matters more than it looks. An unconnected output terminal reads
 * low forever, which is exactly what a blown fuse looks like -- so a board of
 * outputs with nothing plugged in genuinely does fault on all 21 channels, and a
 * fixture that left them unconnected would drown every fault test in noise. Starting
 * from "all well" makes each test say only what it is actually about. */
static void cfg_for_test(uint8_t mode)
{
    memset(&cfg, 0, sizeof(cfg));
    cfg.input_debounce_ms = 25;
    cfg.output_settle_ms  = SETTLE;
    cfg.fault_confirm_ms  = CONFIRM;
    for (uint8_t i = 0; i < RCM_CHANNELS; i++) {
        cfg.ch[i].mode = mode;
        SIM.wiring[i]  = (mode == CH_OUTPUT) ? SIM_COIL_OK : SIM_BUTTON_OPEN;
    }
}

static void run_ms(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += TICK_MS) {
        SIM.now_ms += TICK_MS;
        ch_tick(SIM.now_ms);
    }
}

void setUp(void)
{
    sim_reset();
    cfg_for_test(CH_OUTPUT);
    sr_begin();
    ch_begin();
    sr_outputs_enable(true);
}
void tearDown(void) {}

/* --- commands -------------------------------------------------------------- */

static void test_command_drives_the_right_channel(void)
{
    for (uint8_t ch = 0; ch < RCM_CHANNELS; ch++) {
        ch_all_off();
        ch_command(ch, true);
        run_ms(TICK_MS * 2);
        for (uint8_t o = 0; o < RCM_CHANNELS; o++) {
            char m[48];
            snprintf(m, sizeof(m), "cmd ch%u -> driver ch%u", ch + 1u, o + 1u);
            TEST_ASSERT_EQUAL_MESSAGE(o == ch, sim_driver_on(o), m);
        }
    }
}

static void test_commands_are_ignored_on_input_channels(void)
{
    /* Safety property: driving a channel wired to a button would short that button's
     * +12V feed to ground through the TPL7407L. */
    cfg.ch[5].mode = CH_INPUT;
    ch_command(5, true);
    run_ms(TICK_MS * 2);
    TEST_ASSERT_FALSE(sim_driver_on(5));
    TEST_ASSERT_EQUAL_HEX32(0, ch_commanded());
}

static void test_commands_are_ignored_on_unused_channels(void)
{
    cfg.ch[9].mode = CH_UNUSED;
    ch_command(9, true);
    run_ms(TICK_MS * 2);
    TEST_ASSERT_FALSE(sim_driver_on(9));
}

static void test_invert_flips_an_output(void)
{
    cfg.ch[2].flags = CH_F_INVERT;
    ch_command(2, false);          /* logical off -> physically ON */
    run_ms(TICK_MS * 2);
    TEST_ASSERT_TRUE(sim_driver_on(2));

    ch_command(2, true);
    run_ms(TICK_MS * 2);
    TEST_ASSERT_FALSE(sim_driver_on(2));
}

static void test_command_mask_touches_only_masked_channels(void)
{
    ch_command(0, true);
    ch_command(1, true);
    run_ms(TICK_MS * 2);
    /* mask covers ch1 only; ch2 must keep its state even though the value bit is 0 */
    ch_command_mask(0x000001, 0x000000);
    run_ms(TICK_MS * 2);
    TEST_ASSERT_FALSE(sim_driver_on(0));
    TEST_ASSERT_TRUE(sim_driver_on(1));
}

static void test_all_off_ignores_mode_and_invert(void)
{
    /* "All off" has to mean nothing is being driven, whatever anything is configured
     * as -- it is what a bus timeout and a shutdown both call. */
    cfg.ch[4].flags = CH_F_INVERT;
    ch_command(4, false);          /* inverted -> driving */
    ch_command(7, true);
    run_ms(TICK_MS * 2);
    TEST_ASSERT_TRUE(sim_driver_on(4));

    ch_all_off();
    run_ms(TICK_MS * 2);
    for (uint8_t ch = 0; ch < RCM_CHANNELS; ch++) TEST_ASSERT_FALSE(sim_driver_on(ch));
    TEST_ASSERT_EQUAL_HEX32(0, ch_commanded());
}

/* --- inputs ---------------------------------------------------------------- */

static void test_input_needs_the_full_debounce(void)
{
    cfg_for_test(CH_INPUT);
    ch_begin();
    SIM.wiring[3] = SIM_BUTTON_PRESSED;

    run_ms(TICK_MS * 2);                       /* well short of 25ms */
    TEST_ASSERT_EQUAL_HEX32(0, ch_inputs());

    run_ms(cfg.input_debounce_ms + TICK_MS * 2);
    TEST_ASSERT_EQUAL_HEX32(1ul << 3, ch_inputs());
}

static void test_input_chatter_never_settles(void)
{
    cfg_for_test(CH_INPUT);
    ch_begin();
    for (int i = 0; i < 40; i++) {
        SIM.wiring[3] = (i & 1) ? SIM_BUTTON_PRESSED : SIM_BUTTON_OPEN;
        run_ms(TICK_MS);
    }
    TEST_ASSERT_EQUAL_HEX32(0, ch_inputs());
}

static void test_input_invert(void)
{
    cfg_for_test(CH_INPUT);
    cfg.ch[6].flags = CH_F_INVERT;
    ch_begin();
    SIM.wiring[6] = SIM_BUTTON_OPEN;           /* line low -> inverted reads pressed */
    run_ms(100);
    TEST_ASSERT_EQUAL_HEX32(1ul << 6, ch_inputs());
}

static void test_inputs_only_reports_input_channels(void)
{
    /* An OUTPUT channel sitting at +12V because its coil circuit is fine must not
     * appear as a pressed button. */
    for (uint8_t i = 0; i < RCM_CHANNELS; i++) SIM.wiring[i] = SIM_COIL_OK;
    cfg.ch[8].mode = CH_INPUT;
    SIM.wiring[8] = SIM_BUTTON_PRESSED;
    run_ms(100);
    TEST_ASSERT_EQUAL_HEX32(1ul << 8, ch_inputs());
    TEST_ASSERT_EQUAL_HEX32(0x1FFFFF, ch_sense_raw());   /* raw still shows everything */
}

static void test_aux_debounce(void)
{
    cfg_for_test(CH_INPUT);
    ch_begin();
    SIM.aux[1] = 1;
    run_ms(TICK_MS);
    TEST_ASSERT_EQUAL_HEX8(0, ch_aux());
    run_ms(100);
    TEST_ASSERT_EQUAL_HEX8(0x02, ch_aux());
}

/* --- coil circuit diagnosis ------------------------------------------------ */

static void test_open_circuit_is_detected_when_the_driver_is_off(void)
{
    SIM.wiring[0] = SIM_COIL_OK;
    SIM.wiring[1] = SIM_COIL_OPEN;             /* blown fuse on channel 2 */
    run_ms(SETTLE + CONFIRM + 100);
    TEST_ASSERT_EQUAL_HEX32(1ul << 1, ch_fault_open());
    TEST_ASSERT_EQUAL_HEX32(0, ch_fault_short());
}

static void test_no_fault_during_the_settle_window(void)
{
    /* Switching a relay off leaves flyback current collapsing through the clamp and
     * the node sits low for a while. Diagnose in that window and every switch-off
     * looks like a blown fuse. */
    SIM.wiring[0] = SIM_COIL_OK;
    run_ms(SETTLE + CONFIRM + 100);
    TEST_ASSERT_EQUAL_HEX32(0, ch_fault_open());

    ch_command(0, true);
    run_ms(SETTLE - TICK_MS * 2);
    TEST_ASSERT_EQUAL_HEX32(0, ch_fault_open());
    TEST_ASSERT_EQUAL_HEX32(0, ch_fault_short());
}

static void test_fault_needs_the_full_confirm_time(void)
{
    SIM.wiring[2] = SIM_COIL_OPEN;
    run_ms(SETTLE + CONFIRM - TICK_MS * 4);
    TEST_ASSERT_EQUAL_HEX32(0, ch_fault_open());
    run_ms(TICK_MS * 8);
    TEST_ASSERT_EQUAL_HEX32(1ul << 2, ch_fault_open());
}

static void test_fault_clears_when_the_fuse_is_replaced(void)
{
    SIM.wiring[4] = SIM_COIL_OPEN;
    run_ms(SETTLE + CONFIRM + 100);
    TEST_ASSERT_EQUAL_HEX32(1ul << 4, ch_fault_open());

    SIM.wiring[4] = SIM_COIL_OK;
    run_ms(100);
    TEST_ASSERT_EQUAL_HEX32(0, ch_fault_open());
}

static void test_short_to_12v_is_detected_while_driving(void)
{
    SIM.wiring[6] = SIM_SHORT_12V;             /* driver cannot pull it down */
    ch_command(6, true);
    run_ms(SETTLE + CONFIRM + 100);
    TEST_ASSERT_EQUAL_HEX32(1ul << 6, ch_fault_short());
    TEST_ASSERT_EQUAL_HEX32(0, ch_fault_open());
}

static void test_a_healthy_energised_channel_reports_nothing(void)
{
    for (uint8_t i = 0; i < RCM_CHANNELS; i++) SIM.wiring[i] = SIM_COIL_OK;
    ch_command_mask(0x1FFFFF, 0x1FFFFF);
    run_ms(SETTLE + CONFIRM + 200);
    TEST_ASSERT_EQUAL_HEX32(0, ch_fault_open());
    TEST_ASSERT_EQUAL_HEX32(0, ch_fault_short());
}

static void test_no_diag_suppresses_reporting(void)
{
    SIM.wiring[5] = SIM_COIL_OPEN;
    cfg.ch[5].flags = CH_F_NO_DIAG;
    run_ms(SETTLE + CONFIRM + 100);
    TEST_ASSERT_EQUAL_HEX32(0, ch_fault_open());
}

static void test_unused_and_input_channels_never_fault(void)
{
    /* An unconnected terminal reads low forever, which is indistinguishable from a
     * blown fuse. Only channels someone has declared to be outputs get diagnosed. */
    cfg.ch[10].mode = CH_UNUSED;
    cfg.ch[11].mode = CH_INPUT;
    SIM.wiring[10] = SIM_UNCONNECTED;
    SIM.wiring[11] = SIM_BUTTON_OPEN;
    run_ms(SETTLE + CONFIRM + 100);
    TEST_ASSERT_EQUAL_HEX32(0, ch_fault_open() & ((1ul << 10) | (1ul << 11)));
}

static void test_hiz_outputs_are_not_diagnosed(void)
{
    /* With the 595s disabled every channel looks off, so a real fault and a healthy
     * board are indistinguishable. Manufacturing faults out of that would light the
     * red LED on every bench power-up. */
    sr_outputs_enable(false);
    for (uint8_t i = 0; i < RCM_CHANNELS; i++) SIM.wiring[i] = SIM_COIL_OPEN;
    run_ms(SETTLE + CONFIRM + 200);
    TEST_ASSERT_EQUAL_HEX32(0, ch_fault_open());
}

static void test_switching_a_channel_drops_its_stale_fault(void)
{
    SIM.wiring[7] = SIM_COIL_OPEN;
    run_ms(SETTLE + CONFIRM + 100);
    TEST_ASSERT_EQUAL_HEX32(1ul << 7, ch_fault_open());

    ch_command(7, true);                       /* the old verdict no longer applies */
    TEST_ASSERT_EQUAL_HEX32(0, ch_fault_open());
}

/* --- failsafe -------------------------------------------------------------- */

static void test_failsafe_applies_physical_states(void)
{
    /* failsafe_state is what should physically happen, so an inverted channel has to
     * be un-inverted on the way in. A failsafe table you have to mentally invert is
     * one somebody will get wrong. */
    cfg.ch[1].flags = CH_F_INVERT;
    cfg.failsafe_state = (1ul << 1) | (1ul << 3);

    ch_all_off();
    ch_apply_failsafe();
    run_ms(TICK_MS * 2);

    TEST_ASSERT_TRUE_MESSAGE(sim_driver_on(1), "inverted failsafe channel");
    TEST_ASSERT_TRUE(sim_driver_on(3));
    TEST_ASSERT_FALSE(sim_driver_on(0));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_command_drives_the_right_channel);
    RUN_TEST(test_commands_are_ignored_on_input_channels);
    RUN_TEST(test_commands_are_ignored_on_unused_channels);
    RUN_TEST(test_invert_flips_an_output);
    RUN_TEST(test_command_mask_touches_only_masked_channels);
    RUN_TEST(test_all_off_ignores_mode_and_invert);
    RUN_TEST(test_input_needs_the_full_debounce);
    RUN_TEST(test_input_chatter_never_settles);
    RUN_TEST(test_input_invert);
    RUN_TEST(test_inputs_only_reports_input_channels);
    RUN_TEST(test_aux_debounce);
    RUN_TEST(test_open_circuit_is_detected_when_the_driver_is_off);
    RUN_TEST(test_no_fault_during_the_settle_window);
    RUN_TEST(test_fault_needs_the_full_confirm_time);
    RUN_TEST(test_fault_clears_when_the_fuse_is_replaced);
    RUN_TEST(test_short_to_12v_is_detected_while_driving);
    RUN_TEST(test_a_healthy_energised_channel_reports_nothing);
    RUN_TEST(test_no_diag_suppresses_reporting);
    RUN_TEST(test_unused_and_input_channels_never_fault);
    RUN_TEST(test_hiz_outputs_are_not_diagnosed);
    RUN_TEST(test_switching_a_channel_drops_its_stale_fault);
    RUN_TEST(test_failsafe_applies_physical_states);
    return UNITY_END();
}
