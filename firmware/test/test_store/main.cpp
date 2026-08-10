/*
 * test_store -- the EEPROM driver and the config record on top of it.
 *
 * The model underneath is a real M95640, page wrap included. The most valuable test
 * in here is the one that writes across a page boundary: if store_write() stopped
 * splitting bursts, the model would wrap the tail back over the head exactly as the
 * chip does, and the data would come back corrupt rather than the write failing.
 */
#include <unity.h>
#include <cstdio>

#include "../stubs/simboard.cpp"
#include "../../src/store.cpp"
#include "../../src/config.cpp"

void setUp(void)
{
    sim_reset();
    store_begin();
    memset(&straps, 0, sizeof(straps));
    straps.node = 0;
}
void tearDown(void) {}

/* --- the driver ------------------------------------------------------------ */

static void test_probe_finds_the_part(void)
{
    TEST_ASSERT_TRUE(store_present());
}

static void test_probe_reports_a_missing_part(void)
{
    /* With nothing fitted MISO floats and reads as 0xFF, which would look like a
     * status register with every bit set -- including WEL. The probe toggles WEL and
     * insists on seeing it go both ways, so a float cannot fake it. */
    sim_reset();
    SIM.eep_fitted = false;
    store_begin();
    TEST_ASSERT_FALSE(store_present());
    uint8_t b = 0;
    TEST_ASSERT_FALSE(store_read(0, &b, 1));
    TEST_ASSERT_FALSE(store_write(0, &b, 1));
}

static void test_small_round_trip(void)
{
    const uint8_t src[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
    uint8_t dst[4] = { 0 };
    TEST_ASSERT_TRUE(store_write(0x100, src, 4));
    TEST_ASSERT_TRUE(store_read(0x100, dst, 4));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(src, dst, 4);
}

static void test_write_across_a_page_boundary(void)
{
    /* Start 8 bytes before a page boundary and write 40 bytes, so the burst spans
     * three pages. Unsplit, the chip would wrap and the data would be scrambled. */
    uint8_t src[40], dst[40];
    for (int i = 0; i < 40; i++) src[i] = (uint8_t)(i + 1);

    TEST_ASSERT_TRUE(store_write(0x0018, src, sizeof(src)));
    TEST_ASSERT_TRUE(store_read(0x0018, dst, sizeof(dst)));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(src, dst, sizeof(src));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, SIM.eep_page_wraps,
                                     "a burst was allowed to run off the end of a page");
}

static void test_write_does_not_disturb_neighbouring_pages(void)
{
    uint8_t pad[8];
    memset(pad, 0x5A, sizeof(pad));
    TEST_ASSERT_TRUE(store_write(0x0010, pad, sizeof(pad)));   /* before */
    TEST_ASSERT_TRUE(store_write(0x0060, pad, sizeof(pad)));   /* after  */

    uint8_t src[32];
    memset(src, 0xC3, sizeof(src));
    TEST_ASSERT_TRUE(store_write(0x0020, src, sizeof(src)));

    uint8_t chk[8];
    TEST_ASSERT_TRUE(store_read(0x0010, chk, sizeof(chk)));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(pad, chk, sizeof(pad));
    TEST_ASSERT_TRUE(store_read(0x0060, chk, sizeof(chk)));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(pad, chk, sizeof(pad));
}

static void test_out_of_range_is_refused(void)
{
    uint8_t b[4] = { 0 };
    TEST_ASSERT_FALSE(store_write(EEP_SIZE - 2, b, 4));
    TEST_ASSERT_FALSE(store_read(EEP_SIZE - 2, b, 4));
}

/* --- CRC ------------------------------------------------------------------- */

static void test_crc16_known_vector(void)
{
    /* CRC-16/CCITT-FALSE of "123456789" is 0x29B1. Checked against the published
     * vector rather than against our own output, or the test would just be
     * re-asserting whatever the code happens to do. */
    TEST_ASSERT_EQUAL_HEX16(0x29B1, cfg_crc16("123456789", 9));
}

static void test_crc16_catches_a_single_bit_flip(void)
{
    uint8_t buf[64];
    for (int i = 0; i < 64; i++) buf[i] = (uint8_t)i;
    const uint16_t good = cfg_crc16(buf, sizeof(buf));
    buf[37] ^= 0x08;
    TEST_ASSERT_NOT_EQUAL(good, cfg_crc16(buf, sizeof(buf)));
}

/* --- config ---------------------------------------------------------------- */

static void test_defaults_follow_the_role_strap(void)
{
    straps.keypad = false;
    cfg_defaults(&cfg);
    for (uint8_t i = 0; i < RCM_CHANNELS; i++) TEST_ASSERT_EQUAL(CH_OUTPUT, cfg.ch[i].mode);
    TEST_ASSERT_EQUAL_HEX16(RCM_CAN_BASE_DEFAULT, cfg.can_base_id);
    TEST_ASSERT_EQUAL_UINT32(RCM_BAUD_DEFAULT, cfg.can_bitrate);
    TEST_ASSERT_EQUAL_UINT8(PEER_NONE, cfg.peer_node);
    TEST_ASSERT_EQUAL_HEX32(0, cfg.failsafe_state);   /* quiet bus -> everything off */

    straps.keypad = true;
    cfg_defaults(&cfg);
    for (uint8_t i = 0; i < RCM_CHANNELS; i++) TEST_ASSERT_EQUAL(CH_INPUT, cfg.ch[i].mode);
}

static void test_save_and_load_round_trip(void)
{
    cfg_defaults(&cfg);
    cfg.can_bitrate  = 250000;
    cfg.ch[4].mode   = CH_INPUT;
    cfg.ch[4].flags  = CH_F_INVERT;
    cfg.peer_node    = 5;
    cfg.peer_mask    = 0x0000FF;
    cfg.imu_map[0]   = 0x81;                 /* vehicle X = -sensor Y */
    TEST_ASSERT_TRUE(cfg_save());

    memset(&cfg, 0, sizeof(cfg));
    cfg_load();

    TEST_ASSERT_EQUAL_UINT32(250000, cfg.can_bitrate);
    TEST_ASSERT_EQUAL(CH_INPUT, cfg.ch[4].mode);
    TEST_ASSERT_EQUAL_HEX8(CH_F_INVERT, cfg.ch[4].flags);
    TEST_ASSERT_EQUAL_UINT8(5, cfg.peer_node);
    TEST_ASSERT_EQUAL_HEX32(0x0000FF, cfg.peer_mask);
    TEST_ASSERT_EQUAL_HEX8(0x81, cfg.imu_map[0]);
}

static void test_a_blank_eeprom_gives_defaults(void)
{
    straps.keypad = false;
    memset(&cfg, 0xAA, sizeof(cfg));
    cfg_load();
    TEST_ASSERT_TRUE(cfg_valid(&cfg));
    TEST_ASSERT_EQUAL_UINT32(RCM_BAUD_DEFAULT, cfg.can_bitrate);
}

static void test_a_corrupt_primary_falls_back_to_the_backup_and_repairs_it(void)
{
    cfg_defaults(&cfg);
    cfg.can_bitrate = 125000;
    TEST_ASSERT_TRUE(cfg_save());

    /* Scribble on the primary the way a half-finished write would. */
    uint8_t junk[16];
    memset(junk, 0x00, sizeof(junk));
    TEST_ASSERT_TRUE(store_write(EEP_ADDR_CFG_A + 4, junk, sizeof(junk)));

    memset(&cfg, 0, sizeof(cfg));
    cfg_load();
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(125000, cfg.can_bitrate, "did not fall back to backup");

    /* And the primary must have been rewritten, so the next boot does not have to
     * make the same discovery. */
    struct rcm_config_t a;
    TEST_ASSERT_TRUE(store_read(EEP_ADDR_CFG_A, &a, sizeof(a)));
    TEST_ASSERT_TRUE_MESSAGE(cfg_valid(&a), "primary was not repaired");
    TEST_ASSERT_EQUAL_UINT32(125000, a.can_bitrate);
}

static void test_both_copies_corrupt_gives_defaults(void)
{
    cfg_defaults(&cfg);
    cfg.can_bitrate = 125000;
    TEST_ASSERT_TRUE(cfg_save());

    uint8_t junk[16];
    memset(junk, 0x00, sizeof(junk));
    store_write(EEP_ADDR_CFG_A + 4, junk, sizeof(junk));
    store_write(EEP_ADDR_CFG_B + 4, junk, sizeof(junk));

    memset(&cfg, 0, sizeof(cfg));
    cfg_load();
    TEST_ASSERT_EQUAL_UINT32(RCM_BAUD_DEFAULT, cfg.can_bitrate);
}

static void test_a_record_from_a_future_version_is_rejected(void)
{
    cfg_defaults(&cfg);
    cfg.can_bitrate = 125000;
    cfg_save();

    struct rcm_config_t a;
    store_read(EEP_ADDR_CFG_A, &a, sizeof(a));
    a.version = RCM_CFG_VERSION + 1;
    a.crc = cfg_crc16(&a, sizeof(a) - sizeof(uint16_t));   /* validly CRC'd, wrong version */
    store_write(EEP_ADDR_CFG_A, &a, sizeof(a));
    store_read(EEP_ADDR_CFG_B, &a, sizeof(a));
    a.version = RCM_CFG_VERSION + 1;
    a.crc = cfg_crc16(&a, sizeof(a) - sizeof(uint16_t));
    store_write(EEP_ADDR_CFG_B, &a, sizeof(a));

    cfg_load();
    TEST_ASSERT_EQUAL_UINT32(RCM_BAUD_DEFAULT, cfg.can_bitrate);
}

static void test_config_record_fits_comfortably(void)
{
    /* Two copies plus room to grow. If this ever trips, move the backup address
     * rather than shrinking the record. */
    TEST_ASSERT_LESS_THAN_UINT32(EEP_ADDR_CFG_B - EEP_ADDR_CFG_A,
                                 (uint32_t)sizeof(struct rcm_config_t));
}

/* --- bitrate policy -------------------------------------------------------- */

static void test_recovery_strap_overrides_a_stored_bitrate(void)
{
    cfg_defaults(&cfg);
    cfg.can_bitrate = 125000;
    straps.force_500k = false;
    TEST_ASSERT_EQUAL_UINT32(125000, cfg_effective_bitrate());

    /* The point of the strap: you cannot fix a wrong CAN setting over CAN, so one
     * switch has to beat anything the configuration says. */
    straps.force_500k = true;
    TEST_ASSERT_EQUAL_UINT32(RCM_BAUD_RECOVERY, cfg_effective_bitrate());
}

static void test_a_nonsense_stored_bitrate_falls_back(void)
{
    cfg_defaults(&cfg);
    straps.force_500k = false;
    cfg.can_bitrate = 0;
    TEST_ASSERT_EQUAL_UINT32(RCM_BAUD_DEFAULT, cfg_effective_bitrate());
    cfg.can_bitrate = 99000000;
    TEST_ASSERT_EQUAL_UINT32(RCM_BAUD_DEFAULT, cfg_effective_bitrate());
}

static void test_node_number_combines_role_and_address(void)
{
    /* A keypad and a relay module can share a DIP address without colliding, because
     * the role is the top bit. */
    SIM.dip_closed[1] = 1;                 /* ADDR0 */
    SIM.dip_closed[2] = 1;                 /* ADDR1 */
    SIM.dip_closed[0] = 0;                 /* role = relay module */
    cfg_read_straps();
    TEST_ASSERT_EQUAL_UINT8(3, straps.address);
    TEST_ASSERT_EQUAL_UINT8(3, straps.node);

    SIM.dip_closed[0] = 1;                 /* role = keypad */
    cfg_read_straps();
    TEST_ASSERT_EQUAL_UINT8(3, straps.address);
    TEST_ASSERT_EQUAL_UINT8(7, straps.node);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_probe_finds_the_part);
    RUN_TEST(test_probe_reports_a_missing_part);
    RUN_TEST(test_small_round_trip);
    RUN_TEST(test_write_across_a_page_boundary);
    RUN_TEST(test_write_does_not_disturb_neighbouring_pages);
    RUN_TEST(test_out_of_range_is_refused);
    RUN_TEST(test_crc16_known_vector);
    RUN_TEST(test_crc16_catches_a_single_bit_flip);
    RUN_TEST(test_defaults_follow_the_role_strap);
    RUN_TEST(test_save_and_load_round_trip);
    RUN_TEST(test_a_blank_eeprom_gives_defaults);
    RUN_TEST(test_a_corrupt_primary_falls_back_to_the_backup_and_repairs_it);
    RUN_TEST(test_both_copies_corrupt_gives_defaults);
    RUN_TEST(test_a_record_from_a_future_version_is_rejected);
    RUN_TEST(test_config_record_fits_comfortably);
    RUN_TEST(test_recovery_strap_overrides_a_stored_bitrate);
    RUN_TEST(test_a_nonsense_stored_bitrate_falls_back);
    RUN_TEST(test_node_number_combines_role_and_address);
    return UNITY_END();
}
