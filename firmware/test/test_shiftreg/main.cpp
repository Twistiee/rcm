/*
 * test_shiftreg -- the real shiftreg.cpp driving a bit-accurate model of the chain.
 *
 * This is the test that matters most in the suite. The driver maps a channel to
 * (SPI byte, bit); the model maps a channel to a physical flip-flop and then shifts
 * bits through 24 stages the way the hardware does. Those are two independent
 * derivations from the same netlist, and they only agree if both are right.
 *
 * A mirrored byte order -- the single most likely mistake here, because the two
 * chains genuinely do run in opposite directions -- shows up as channel 1 lighting
 * up channel 15.
 */
#include <unity.h>
#include <cstdio>

#include "../stubs/simboard.cpp"
#include "../../src/shiftreg.cpp"

void setUp(void)    { sim_reset(); sr_begin(); }
void tearDown(void) {}

/* --- boot safety ----------------------------------------------------------- */

static void test_begin_leaves_outputs_hiz(void)
{
    TEST_ASSERT_FALSE_MESSAGE(SIM.oe_low, "SR_OE_N must still be high after sr_begin");
    TEST_ASSERT_FALSE(sr_outputs_enabled());
}

static void test_begin_latches_zero_before_anything_can_enable(void)
{
    /* sr_begin() must have completed a full exchange, so the 595 storage registers
     * hold zeros rather than power-on garbage by the time anyone drops OE. */
    TEST_ASSERT_EQUAL_UINT32(1, SIM.exchanges);
    TEST_ASSERT_EQUAL_UINT32(0, SIM.latch595);

    sr_outputs_enable(true);
    for (uint8_t ch = 0; ch < RCM_CHANNELS; ch++)
        TEST_ASSERT_FALSE_MESSAGE(sim_driver_on(ch), "a channel came up energised");
}

/* --- outputs --------------------------------------------------------------- */

static void test_every_channel_drives_itself_and_nothing_else(void)
{
    sr_outputs_enable(true);
    for (uint8_t ch = 0; ch < RCM_CHANNELS; ch++) {
        sr_set_all(0);
        sr_set(ch, true);
        sr_exchange();
        for (uint8_t o = 0; o < RCM_CHANNELS; o++) {
            char msg[64];
            snprintf(msg, sizeof(msg), "set ch%u -> ch%u", ch + 1u, o + 1u);
            TEST_ASSERT_EQUAL_MESSAGE(o == ch, sim_driver_on(o), msg);
        }
    }
}

static void test_output_byte_order_is_not_mirrored(void)
{
    /* Nailed down explicitly, because a mirrored driver would still pass a test that
     * only checked "one channel on at a time" if the model were mirrored too.
     * Channel 1 lives in U_SO1, which is the FAR end of the chain: chain position 16.
     * Channel 15 lives in U_SO3, nearest MOSI: position 0. */
    TEST_ASSERT_EQUAL_UINT8(16, sim_pos595(0));
    TEST_ASSERT_EQUAL_UINT8(0,  sim_pos595(14));

    sr_outputs_enable(true);
    sr_set_all(0);
    sr_set(0, true);              /* channel 1 */
    sr_exchange();
    TEST_ASSERT_EQUAL_UINT32(1ul << 16, SIM.latch595);

    sr_set_all(0);
    sr_set(14, true);             /* channel 15 */
    sr_exchange();
    TEST_ASSERT_EQUAL_UINT32(1ul << 0, SIM.latch595);
}

static void test_qh_is_left_alone(void)
{
    /* Each 595's 8th output is unconnected. Nothing we do should ever set it -- if it
     * did, the mapping has slipped by one and some channel is being driven by the
     * wrong bit. */
    sr_outputs_enable(true);
    sr_set_all(0x1FFFFF);         /* all 21 channels on */
    sr_exchange();
    const uint32_t qh = (1ul << 7) | (1ul << 15) | (1ul << 23);
    TEST_ASSERT_EQUAL_HEX32(0, SIM.latch595 & qh);
    TEST_ASSERT_EQUAL_HEX32(0x7F7F7F, SIM.latch595);
}

static void test_outputs_are_hiz_until_enabled(void)
{
    sr_set_all(0x1FFFFF);
    sr_exchange();
    TEST_ASSERT_EQUAL_UINT32(0x7F7F7F, SIM.latch595);   /* latched... */
    for (uint8_t ch = 0; ch < RCM_CHANNELS; ch++)
        TEST_ASSERT_FALSE(sim_driver_on(ch));           /* ...but not driving */
}

/* --- inputs ---------------------------------------------------------------- */

static void test_every_channel_senses_itself_and_nothing_else(void)
{
    for (uint8_t ch = 0; ch < RCM_CHANNELS; ch++) {
        for (uint8_t i = 0; i < RCM_CHANNELS; i++) SIM.wiring[i] = SIM_UNCONNECTED;
        SIM.wiring[ch] = SIM_BUTTON_PRESSED;
        sr_exchange();
        for (uint8_t o = 0; o < RCM_CHANNELS; o++) {
            char msg[64];
            snprintf(msg, sizeof(msg), "high on ch%u -> read ch%u", ch + 1u, o + 1u);
            TEST_ASSERT_EQUAL_MESSAGE(o == ch, sr_sense(o), msg);
        }
    }
}

static void test_input_byte_order_is_not_mirrored(void)
{
    /* The 165 chain runs the OTHER way to the 595 chain: U_SI3 holds MISO, so tile 3
     * arrives in the FIRST byte. The driver's rx byte index must therefore be the
     * mirror of its tx byte index, and this is where that gets pinned down. */
    for (uint8_t i = 0; i < RCM_CHANNELS; i++) SIM.wiring[i] = SIM_UNCONNECTED;
    SIM.wiring[0] = SIM_BUTTON_PRESSED;      /* channel 1, tile 1 */
    sr_exchange();
    TEST_ASSERT_TRUE(sr_sense(0));
    TEST_ASSERT_FALSE_MESSAGE(sr_sense(14), "channel 1 read as channel 15 -- byte order");

    for (uint8_t i = 0; i < RCM_CHANNELS; i++) SIM.wiring[i] = SIM_UNCONNECTED;
    SIM.wiring[14] = SIM_BUTTON_PRESSED;     /* channel 15, tile 3 */
    sr_exchange();
    TEST_ASSERT_TRUE(sr_sense(14));
    TEST_ASSERT_FALSE(sr_sense(0));
}

static void test_aux_inputs(void)
{
    for (uint8_t a = 0; a < RCM_AUX_INPUTS; a++) {
        memset(SIM.aux, 0, sizeof(SIM.aux));
        SIM.aux[a] = 1;
        sr_exchange();
        for (uint8_t o = 0; o < RCM_AUX_INPUTS; o++)
            TEST_ASSERT_EQUAL(o == a, sr_aux(o));
        /* An aux bit must never leak into a channel: they share the 165 with the
         * seven channels of their tile, on the spare D7. */
        TEST_ASSERT_EQUAL_UINT32(0, sr_sense_all());
    }
}

static void test_full_pattern_round_trips(void)
{
    /* Every other channel high, all three aux high. Catches an off-by-one that a
     * single-bit test would walk straight past. */
    uint32_t expect = 0;
    for (uint8_t ch = 0; ch < RCM_CHANNELS; ch++) {
        SIM.wiring[ch] = (ch % 2) ? SIM_BUTTON_PRESSED : SIM_UNCONNECTED;
        if (ch % 2) expect |= 1ul << ch;
    }
    for (uint8_t a = 0; a < RCM_AUX_INPUTS; a++) SIM.aux[a] = 1;
    sr_exchange();
    TEST_ASSERT_EQUAL_HEX32(expect, sr_sense_all());
    TEST_ASSERT_EQUAL_HEX8(0x07, sr_aux_all());
}

static void test_sense_lags_a_commanded_change_by_one_exchange(void)
{
    /* A real property of the hardware, not a quirk of the model. sr_exchange() loads
     * the 165s FIRST and pulses RCLK LAST, so within any one cycle the inputs were
     * sampled against the PREVIOUS output state. Commanding a channel on therefore
     * shows up in its own sense bit on the exchange after next.
     *
     * Nothing depends on this -- cfg.output_settle_ms holds fault diagnosis off for
     * twenty ticks after a switch -- but anyone writing a tighter loop against these
     * bits needs to know, and a test is a better place to say so than a comment. */
    sr_outputs_enable(true);
    for (uint8_t i = 0; i < RCM_CHANNELS; i++) SIM.wiring[i] = SIM_COIL_OK;

    sr_exchange();
    TEST_ASSERT_EQUAL_HEX32(0x1FFFFF, sr_sense_all());   /* all coils intact, all high */

    sr_set(3, true);
    sr_exchange();                                       /* publishes it, too late to read */
    TEST_ASSERT_TRUE_MESSAGE(sr_sense(3), "sense should still show the pre-switch state");
    TEST_ASSERT_TRUE(sim_driver_on(3));                  /* but it IS being driven now */

    sr_exchange();
    TEST_ASSERT_FALSE_MESSAGE(sr_sense(3), "a driven channel must read low by now");
    TEST_ASSERT_EQUAL_HEX32(0x1FFFFF & ~(1ul << 3), sr_sense_all());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_begin_leaves_outputs_hiz);
    RUN_TEST(test_begin_latches_zero_before_anything_can_enable);
    RUN_TEST(test_every_channel_drives_itself_and_nothing_else);
    RUN_TEST(test_output_byte_order_is_not_mirrored);
    RUN_TEST(test_qh_is_left_alone);
    RUN_TEST(test_outputs_are_hiz_until_enabled);
    RUN_TEST(test_every_channel_senses_itself_and_nothing_else);
    RUN_TEST(test_input_byte_order_is_not_mirrored);
    RUN_TEST(test_aux_inputs);
    RUN_TEST(test_full_pattern_round_trips);
    RUN_TEST(test_sense_lags_a_commanded_change_by_one_exchange);
    return UNITY_END();
}
