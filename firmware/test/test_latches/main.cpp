/*
 * test_latches -- toggle inputs that remember their state across a power cycle.
 *
 * Runs the real channel layer, the real EEPROM driver and the real latch store against
 * the simulated M95640, and "power cycles" by re-running the boot sequence over the same
 * EEPROM contents. That is the only honest test of "comes back on after a restart".
 */
#include <unity.h>
#include <cstdio>

#include "../stubs/simboard.cpp"
#include "../../src/shiftreg.cpp"

#include "../../src/store.cpp"
#include "../../src/imu_level.cpp"   /* config.cpp's defaults need imu_identity() */
#include "../../src/config.cpp"
#include "../../src/channels.cpp"
#include "../../src/latches.cpp"

#define AUTO_HL  6      /* ch 7 -- R2, auto headlights */
#define FAN_OVR 10      /* ch 11 -- R3, a toggle that is NOT remembered */

static void run_ms(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += TICK_MS) {
        SIM.now_ms += TICK_MS;
        ch_tick(SIM.now_ms);
        latches_tick(SIM.now_ms);
    }
}

static void setup_cfg(void)
{
    memset(&cfg, 0, sizeof(cfg));
    cfg.input_debounce_ms = 25;
    cfg.output_settle_ms  = 100;
    cfg.fault_confirm_ms  = 500;
    for (uint8_t i = 0; i < RCM_CHANNELS; i++) { cfg.ch[i].mode = CH_OUTPUT; SIM.wiring[i] = SIM_COIL_OK; }
    cfg.ch[AUTO_HL].mode = CH_INPUT; cfg.ch[AUTO_HL].behaviour = IN_TOGGLE;
    cfg.ch[AUTO_HL].flags = CH_F_REMEMBER;
    cfg.ch[FAN_OVR].mode = CH_INPUT; cfg.ch[FAN_OVR].behaviour = IN_TOGGLE;
    SIM.wiring[AUTO_HL] = SIM_BUTTON_OPEN;
    SIM.wiring[FAN_OVR] = SIM_BUTTON_OPEN;
}

/* The board's boot, minus the radio: storage, channels, then the remembered latches. */
static void boot(void)
{
    store_begin();
    sr_begin();
    ch_begin();
    latches_restore();
    sr_outputs_enable(true);
}

/* Power cycle: RAM is gone, the EEPROM is not. */
static void power_cycle(void)
{
    uint8_t keep[sizeof SIM.eep];
    memcpy(keep, SIM.eep, sizeof keep);
    sim_reset();
    memcpy(SIM.eep, keep, sizeof keep);
    setup_cfg();
    boot();
}

static void press(uint8_t ch)
{
    SIM.wiring[ch] = SIM_BUTTON_PRESSED; run_ms(cfg.input_debounce_ms + 30);
    SIM.wiring[ch] = SIM_BUTTON_OPEN;    run_ms(cfg.input_debounce_ms + 30);
}

static bool reported(uint8_t ch) { return (ch_inputs() >> ch) & 1u; }

void setUp(void) { sim_reset(); setup_cfg(); boot(); run_ms(100); }
void tearDown(void) {}

static void test_auto_headlights_left_on_come_back_on(void)
{
    press(AUTO_HL);
    TEST_ASSERT_TRUE(reported(AUTO_HL));
    run_ms(700);                                  /* let it settle and be written */
    power_cycle();
    run_ms(100);
    TEST_ASSERT_TRUE_MESSAGE(reported(AUTO_HL), "auto headlights did not come back on");
}

static void test_left_off_stays_off(void)
{
    press(AUTO_HL); run_ms(700);
    press(AUTO_HL); run_ms(700);                  /* on, then off again */
    power_cycle(); run_ms(100);
    TEST_ASSERT_FALSE(reported(AUTO_HL));
}

static void test_an_unremembered_toggle_still_starts_off(void)
{
    press(FAN_OVR); run_ms(700);
    TEST_ASSERT_TRUE(reported(FAN_OVR));
    power_cycle(); run_ms(100);
    TEST_ASSERT_FALSE_MESSAGE(reported(FAN_OVR), "a toggle without CH_F_REMEMBER came back on");
}

static void test_a_change_just_before_power_off_is_lost_not_garbled(void)
{
    /* Inside the settle window nothing has been written. That costs the last change,
     * which is the right failure: the previous state, intact. */
    press(AUTO_HL); run_ms(100);
    power_cycle(); run_ms(100);
    TEST_ASSERT_FALSE(reported(AUTO_HL));
}

static void test_a_blank_eeprom_restores_nothing(void)
{
    memset(SIM.eep, 0xFF, sizeof SIM.eep);
    power_cycle(); run_ms(100);
    TEST_ASSERT_FALSE(reported(AUTO_HL));
}

static void test_a_corrupt_record_restores_nothing(void)
{
    /* Corrupt the CHECKSUM, leaving the data saying "on". A believed record would then
     * turn auto headlights on -- so this can only pass if the CRC is actually checked.
     * (The first version flipped the data bit itself, which turned it OFF, the answer
     * expected anyway: removing the CRC check left that test passing.) */
    press(AUTO_HL); run_ms(700);
    SIM.eep[EEP_ADDR_LATCH + 4] ^= 0x5A;
    power_cycle(); run_ms(100);
    TEST_ASSERT_FALSE_MESSAGE(reported(AUTO_HL), "a corrupt record was believed");
}

static void test_a_reassigned_channel_does_not_inherit_the_latch(void)
{
    press(AUTO_HL); run_ms(700);
    uint8_t keep[sizeof SIM.eep];
    memcpy(keep, SIM.eep, sizeof keep);
    sim_reset(); memcpy(SIM.eep, keep, sizeof keep);
    setup_cfg();
    cfg.ch[AUTO_HL].flags = 0;                    /* no longer remembered */
    boot(); run_ms(100);
    TEST_ASSERT_FALSE(reported(AUTO_HL));
}

static void test_a_restore_is_not_a_press(void)
{
    /* Coming back on must not look like a fresh button press to anything downstream --
     * no edge, just the state. A held button at boot still does not toggle it off. */
    press(AUTO_HL); run_ms(700);
    uint8_t keep[sizeof SIM.eep];
    memcpy(keep, SIM.eep, sizeof keep);
    sim_reset(); memcpy(SIM.eep, keep, sizeof keep);
    setup_cfg();
    SIM.wiring[AUTO_HL] = SIM_BUTTON_PRESSED;     /* button stuck down at power-up */
    boot(); run_ms(200);
    TEST_ASSERT_TRUE_MESSAGE(reported(AUTO_HL), "a held button at boot toggled the restored latch");
}

static void test_the_latch_record_does_not_touch_the_config(void)
{
    /* It lives in its own page. If it overlapped a config copy, every button press would
     * corrupt the configuration. */
    TEST_ASSERT_TRUE(EEP_ADDR_LATCH % EEP_PAGE == 0);
    TEST_ASSERT_TRUE(EEP_ADDR_LATCH >= EEP_ADDR_CFG_B + sizeof(struct rcm_config_t) + 2);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_auto_headlights_left_on_come_back_on);
    RUN_TEST(test_left_off_stays_off);
    RUN_TEST(test_an_unremembered_toggle_still_starts_off);
    RUN_TEST(test_a_change_just_before_power_off_is_lost_not_garbled);
    RUN_TEST(test_a_blank_eeprom_restores_nothing);
    RUN_TEST(test_a_corrupt_record_restores_nothing);
    RUN_TEST(test_a_reassigned_channel_does_not_inherit_the_latch);
    RUN_TEST(test_a_restore_is_not_a_press);
    RUN_TEST(test_the_latch_record_does_not_touch_the_config);
    return UNITY_END();
}
