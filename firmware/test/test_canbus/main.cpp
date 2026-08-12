/*
 * test_canbus -- the real canbus.cpp against a bxCAN shim.
 *
 * Until now this was the only module with no host coverage, which was the wrong module
 * to leave uncovered: it holds the newest code (the transmit queue, added after the
 * discovery that four broadcast frames do not fit in three mailboxes) and the classic
 * bug magnet (filter packing, where an 11-bit id lives in bits 31..21 of a bank).
 *
 * What this still cannot test is whether the peripheral behaves as ST document it. That
 * needs silicon, and the self-test build's internal-loopback check is what covers it.
 */
#include <unity.h>
#include <cstdio>

#include "../stubs/stm32_hal_shim.cpp"
#include "../../src/canbus.cpp"

void setUp(void)    { hal_sim_reset(45000000); }   /* 180MHz SYSCLK / 4 */
void tearDown(void) {}

/* --- bit timing ------------------------------------------------------------- */

static void check_rate(uint32_t pclk, uint32_t rate)
{
    char m[64];
    hal_sim_reset(pclk);
    snprintf(m, sizeof(m), "%luMHz %lu baud", (unsigned long)pclk / 1000000UL,
             (unsigned long)rate);
    TEST_ASSERT_TRUE_MESSAGE(can_begin(rate), m);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(rate, can_actual_bitrate(), m);
    /* Everything else on an automotive bus samples near 87.5%; drift far from that and
     * long cable runs start producing errors that look like noise. */
    TEST_ASSERT_TRUE_MESSAGE(can_sample_point_permille() >= 800
                             && can_sample_point_permille() <= 900, m);
}

static void test_the_usual_bitrates_come_out_exact(void)
{
    const uint32_t rates[] = { 125000, 250000, 500000, 1000000 };
    for (uint32_t r : rates) { check_rate(45000000, r); check_rate(42000000, r); }
}

static void test_odd_but_achievable_bitrates(void)
{
    check_rate(45000000, 100000);
    check_rate(45000000, 200000);
    check_rate(42000000, 125000);
}

static void test_unachievable_bitrates_fail_rather_than_approximate(void)
{
    /* 45e6/800e3 = 56.25 -- no integer prescaler and quantum count reach it. Returning
     * "close enough" would give a bus that works on the bench and fails in the rain. */
    hal_sim_reset(45000000);
    TEST_ASSERT_FALSE_MESSAGE(can_begin(800000), "800k must fail on a 45MHz APB1");
    TEST_ASSERT_FALSE(can_send(0x100, (const uint8_t *)"x", 1));

    hal_sim_reset(45000000);
    TEST_ASSERT_FALSE(can_begin(7));            /* absurdly low */
    hal_sim_reset(45000000);
    TEST_ASSERT_FALSE(can_begin(4000000));      /* above anything bxCAN can produce */
}

static void test_800k_works_where_it_divides(void)
{
    check_rate(40000000, 800000);               /* 40e6/800e3 = 50 = 5 x 10 */
}

static void test_register_fields_are_the_hal_encoding(void)
{
    /* HAL wants (n-1) shifted into place, not n. Off by one here is a bitrate that is
     * wrong by a whole quantum and nobody notices until two nodes disagree. */
    hal_sim_reset(45000000);
    TEST_ASSERT_TRUE(can_begin(500000));
    const uint32_t ts1 = (hcan.Init.TimeSeg1 >> CAN_BTR_TS1_Pos) + 1;
    const uint32_t ts2 = (hcan.Init.TimeSeg2 >> CAN_BTR_TS2_Pos) + 1;
    TEST_ASSERT_EQUAL_UINT32(45000000UL / (hcan.Init.Prescaler * (1 + ts1 + ts2)), 500000UL);
    TEST_ASSERT_TRUE(ts1 >= 1 && ts1 <= 16);
    TEST_ASSERT_TRUE(ts2 >= 1 && ts2 <= 8);
    TEST_ASSERT_EQUAL_UINT32(CAN_MODE_NORMAL, hcan.Init.Mode);
    /* A node that goes permanently silent because something brushed the bus once is
     * not acceptable in a car. */
    TEST_ASSERT_EQUAL(ENABLE, hcan.Init.AutoBusOff);
}

static void test_loopback_mode_is_requested_when_asked(void)
{
    hal_sim_reset(45000000);
    TEST_ASSERT_TRUE(can_begin_ex(500000, true));
    TEST_ASSERT_EQUAL_UINT32(CAN_MODE_LOOPBACK, hcan.Init.Mode);
}

static void test_clocks_and_start(void)
{
    hal_sim_reset(45000000);
    TEST_ASSERT_TRUE(can_begin(500000));
    TEST_ASSERT_TRUE(hal_sim.can1_clocked);
    TEST_ASSERT_TRUE(hal_sim.gpiob_clocked);
    TEST_ASSERT_TRUE(hal_sim.started);
}

static void test_a_failed_init_leaves_the_driver_down(void)
{
    hal_sim_reset(45000000);
    hal_sim.init_result = HAL_ERROR;
    TEST_ASSERT_FALSE(can_begin(500000));
    TEST_ASSERT_FALSE(can_send(0x300, (const uint8_t *)"12345678", 8));
    struct can_frame_t f;
    TEST_ASSERT_FALSE(can_recv(&f));
}

/* --- filters ---------------------------------------------------------------- */

static void test_filter_packs_the_id_into_the_top_bits(void)
{
    hal_sim_reset(45000000);
    can_begin(500000);
    can_filter_block(0x300, 0x7F0);
    const CAN_FilterTypeDef *b = &hal_sim.banks[0];

    /* A standard 11-bit id sits in bits 31..21 of the 32-bit bank, which in the HAL's
     * split representation means the high half holds id << 5. */
    TEST_ASSERT_EQUAL_HEX32(0x300u << 5, b->FilterIdHigh);
    TEST_ASSERT_EQUAL_HEX32(0x7F0u << 5, b->FilterMaskIdHigh);
    TEST_ASSERT_EQUAL_UINT32(CAN_FILTERMODE_IDMASK, b->FilterMode);
    TEST_ASSERT_EQUAL_UINT32(CAN_FILTERSCALE_32BIT, b->FilterScale);
    TEST_ASSERT_EQUAL(ENABLE, b->FilterActivation);
    /* Standard data frames only, so a bench-test frame at an extended id cannot alias
     * onto our block. */
    TEST_ASSERT_EQUAL_HEX32(0x0006, b->FilterMaskIdLow);
}

static void test_each_filter_gets_its_own_bank(void)
{
    hal_sim_reset(45000000);
    can_begin(500000);
    can_filter_block(0x300, 0x7F0);
    can_filter_id(0x380);
    can_filter_id(0x341);
    TEST_ASSERT_EQUAL_INT(3, hal_sim.bank_count);
    for (int i = 0; i < 3; i++)
        TEST_ASSERT_EQUAL_UINT32((uint32_t)i, hal_sim.banks[i].FilterBank);
    TEST_ASSERT_EQUAL_HEX32(0x7FFu << 5, hal_sim.banks[1].FilterMaskIdHigh);
}

static void test_the_first_real_filter_replaces_the_accept_all(void)
{
    /* can_begin() parks an accept-all in bank 0 so a caller that installs nothing is
     * noisy rather than deaf. Banks are ORed, so if that one survived, every filter
     * added afterwards would be decorative -- the whole bus would keep arriving and a
     * 3-deep FIFO can overrun with other nodes' traffic, dropping our own commands. */
    hal_sim_reset(45000000);
    can_begin(500000);
    TEST_ASSERT_TRUE_MESSAGE(hal_sim_accepts(0x200),
                             "with no filters installed we should hear everything");

    can_filter_block(0x300, 0x7F0);
    can_filter_id(0x380);

    TEST_ASSERT_TRUE(hal_sim_accepts(0x300));         /* our block */
    TEST_ASSERT_TRUE(hal_sim_accepts(0x308));
    TEST_ASSERT_TRUE(hal_sim_accepts(0x30F));
    TEST_ASSERT_TRUE(hal_sim_accepts(0x380));         /* global */
    TEST_ASSERT_FALSE_MESSAGE(hal_sim_accepts(0x200),
                              "rusEFI's broadcast still gets in -- accept-all survived");
    TEST_ASSERT_FALSE(hal_sim_accepts(0x310));        /* the next node's block */
    TEST_ASSERT_FALSE(hal_sim_accepts(0x174));        /* our own IMU frames */
}

/* --- transmit --------------------------------------------------------------- */

static const uint8_t P[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

static void test_frames_go_straight_out_while_mailboxes_are_free(void)
{
    hal_sim_reset(45000000);
    can_begin(500000);
    for (int i = 0; i < 3; i++) TEST_ASSERT_TRUE(can_send(0x300 + i, P, 8));
    TEST_ASSERT_EQUAL_INT(3, hal_sim.sent_count);
    TEST_ASSERT_EQUAL_UINT8(0, can_tx_pending());
    TEST_ASSERT_EQUAL_HEX32(0x301, hal_sim.sent[1].id);
    TEST_ASSERT_EQUAL_UINT32(CAN_ID_STD, hal_sim.sent[0].ide);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(P, hal_sim.sent[0].data, 8);
}

static void test_the_fourth_broadcast_frame_is_queued_not_lost(void)
{
    /* The bug this queue exists for: one broadcast cycle emits four frames and bxCAN
     * has three mailboxes, so the STATUS frame was being dropped every single time --
     * silently, and always the same one. */
    hal_sim_reset(45000000);
    can_begin(500000);
    for (int i = 0; i < 4; i++) TEST_ASSERT_TRUE(can_send(0x300 + i, P, 8));

    TEST_ASSERT_EQUAL_INT(3, hal_sim.sent_count);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, can_tx_pending(), "4th frame was dropped");

    hal_sim.free_mailboxes = 1;                 /* one drained onto the wire */
    can_tx_pump();
    TEST_ASSERT_EQUAL_INT(4, hal_sim.sent_count);
    TEST_ASSERT_EQUAL_HEX32(0x303, hal_sim.sent[3].id);
    TEST_ASSERT_EQUAL_UINT8(0, can_tx_pending());
}

static void test_queue_preserves_order(void)
{
    /* A state frame overtaking the command that produced it would be worse than a
     * dropped frame. */
    hal_sim_reset(45000000);
    can_begin(500000);
    hal_sim.free_mailboxes = 0;
    for (int i = 0; i < 6; i++) TEST_ASSERT_TRUE(can_send((uint16_t)(0x400 + i), P, 8));
    TEST_ASSERT_EQUAL_UINT8(6, can_tx_pending());

    hal_sim.free_mailboxes = 6;
    can_tx_pump();
    TEST_ASSERT_EQUAL_INT(6, hal_sim.sent_count);
    for (int i = 0; i < 6; i++)
        TEST_ASSERT_EQUAL_HEX32((uint32_t)(0x400 + i), hal_sim.sent[i].id);
}

static void test_a_full_queue_refuses_rather_than_corrupts(void)
{
    hal_sim_reset(45000000);
    can_begin(500000);
    hal_sim.free_mailboxes = 0;
    int accepted = 0;
    for (int i = 0; i < 20; i++) if (can_send((uint16_t)(0x400 + i), P, 8)) accepted++;
    TEST_ASSERT_EQUAL_INT_MESSAGE(8, accepted, "queue depth should be 8");
    TEST_ASSERT_EQUAL_UINT8(8, can_tx_pending());

    /* What survived must be the FIRST eight, in order -- not a scrambled ring. */
    hal_sim.free_mailboxes = 8;
    can_tx_pump();
    for (int i = 0; i < 8; i++)
        TEST_ASSERT_EQUAL_HEX32((uint32_t)(0x400 + i), hal_sim.sent[i].id);
}

static void test_short_frames_are_zero_padded(void)
{
    hal_sim_reset(45000000);
    can_begin(500000);
    const uint8_t three[3] = { 0xAA, 0xBB, 0xCC };
    TEST_ASSERT_TRUE(can_send(0x309, three, 3));
    TEST_ASSERT_EQUAL_UINT8(3, hal_sim.sent[0].len);
    TEST_ASSERT_EQUAL_HEX8(0xCC, hal_sim.sent[0].data[2]);
    TEST_ASSERT_EQUAL_HEX8(0x00, hal_sim.sent[0].data[3]);
}

static void test_oversized_frames_are_refused(void)
{
    hal_sim_reset(45000000);
    can_begin(500000);
    TEST_ASSERT_FALSE(can_send(0x300, P, 9));
    TEST_ASSERT_EQUAL_INT(0, hal_sim.sent_count);
}

/* --- receive ---------------------------------------------------------------- */

static void test_receive(void)
{
    hal_sim_reset(45000000);
    can_begin(500000);
    struct can_frame_t f;
    TEST_ASSERT_FALSE(can_recv(&f));

    hal_sim_rx_push(0x308, CAN_ID_STD, P, 6);
    TEST_ASSERT_TRUE(can_recv(&f));
    TEST_ASSERT_EQUAL_HEX16(0x308, f.id);
    TEST_ASSERT_EQUAL_UINT8(6, f.len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(P, f.data, 6);
    TEST_ASSERT_FALSE(can_recv(&f));
}

static void test_extended_frames_are_dropped(void)
{
    /* rusEFI's bench-test protocol lives at 0x770000 on extended ids. Letting one of
     * those through with its low bits aliasing onto a command id would be very bad. */
    hal_sim_reset(45000000);
    can_begin(500000);
    hal_sim_rx_push(0x770008, CAN_ID_EXT, P, 8);
    struct can_frame_t f;
    TEST_ASSERT_FALSE(can_recv(&f));
}

/* --- error reporting -------------------------------------------------------- */

static void test_error_counters_and_bus_off(void)
{
    hal_sim_reset(45000000);
    can_begin(500000);
    TEST_ASSERT_FALSE(can_bus_off());
    TEST_ASSERT_EQUAL_UINT8(0, can_rx_errors());

    CAN1->ESR = (37u << CAN_ESR_REC_Pos) | (91u << CAN_ESR_TEC_Pos) | CAN_ESR_BOFF;
    TEST_ASSERT_EQUAL_UINT8(37, can_rx_errors());
    TEST_ASSERT_EQUAL_UINT8(91, can_tx_errors());
    TEST_ASSERT_TRUE(can_bus_off());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_usual_bitrates_come_out_exact);
    RUN_TEST(test_odd_but_achievable_bitrates);
    RUN_TEST(test_unachievable_bitrates_fail_rather_than_approximate);
    RUN_TEST(test_800k_works_where_it_divides);
    RUN_TEST(test_register_fields_are_the_hal_encoding);
    RUN_TEST(test_loopback_mode_is_requested_when_asked);
    RUN_TEST(test_clocks_and_start);
    RUN_TEST(test_a_failed_init_leaves_the_driver_down);
    RUN_TEST(test_filter_packs_the_id_into_the_top_bits);
    RUN_TEST(test_each_filter_gets_its_own_bank);
    RUN_TEST(test_the_first_real_filter_replaces_the_accept_all);
    RUN_TEST(test_frames_go_straight_out_while_mailboxes_are_free);
    RUN_TEST(test_the_fourth_broadcast_frame_is_queued_not_lost);
    RUN_TEST(test_queue_preserves_order);
    RUN_TEST(test_a_full_queue_refuses_rather_than_corrupts);
    RUN_TEST(test_short_frames_are_zero_padded);
    RUN_TEST(test_oversized_frames_are_refused);
    RUN_TEST(test_receive);
    RUN_TEST(test_extended_frames_are_dropped);
    RUN_TEST(test_error_counters_and_bus_off);
    return UNITY_END();
}
