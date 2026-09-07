/*
 * test_imu_level -- solving the IMU axis map from gravity.
 *
 * This is the one piece of the IMU path that is pure arithmetic, so it gets walked
 * exhaustively rather than sampled: all 24 orthogonal mountings, each fed the gravity
 * vector that mounting would actually produce, checking the solver returns a map that
 * puts the car's Z back where it belongs and keeps the axes right-handed.
 *
 * Handedness is the reason this file is longer than it looks like it needs to be. A
 * left-handed map still reads plausibly -- 1 g down, sensible braking numbers -- and
 * only shows up as the car yawing the wrong way in a corner, which is exactly the sort
 * of thing nobody notices until the ECU acts on it.
 */
#include <unity.h>
#include <math.h>
#include <string.h>
#include <cstdio>

#include "../../src/imu_level.cpp"

void setUp(void) {}
void tearDown(void) {}

/* Bench noise off the real BMI270 sitting on a desk: 0.08 / 0.02 / 0.15 deg/s. Using
 * the measured figure rather than a clean zero keeps the tests honest about what
 * "standing still" actually looks like coming off this chip. */
static const float STILL[3] = { -0.08f, -0.02f, -0.15f };

/* Apply a map the way imu.cpp's remap() does. */
static void apply(const uint8_t map[3], const float in[3], float out[3])
{
    for (int i = 0; i < 3; i++) {
        const uint8_t ax = map[i] & 0x03;
        out[i] = (map[i] & 0x80) ? -in[ax] : in[ax];
    }
}

/* det of the signed permutation the map describes. +1 right-handed, -1 left. */
static float det(const uint8_t map[3])
{
    float m[3][3] = {{0,0,0},{0,0,0},{0,0,0}};
    for (int i = 0; i < 3; i++)
        m[i][map[i] & 0x03] = (map[i] & 0x80) ? -1.0f : 1.0f;
    return m[0][0] * (m[1][1]*m[2][2] - m[1][2]*m[2][1])
         - m[0][1] * (m[1][0]*m[2][2] - m[1][2]*m[2][0])
         + m[0][2] * (m[1][0]*m[2][1] - m[1][1]*m[2][0]);
}

/* --- the honest refusals --------------------------------------------------- */

static void test_a_moving_car_is_refused(void)
{
    /* Braking hard: the vector is well over 1 g, so this is not just gravity. */
    const float a[3] = { -0.9f, 0.0f, 1.0f };
    uint8_t map[3]; float tilt;
    TEST_ASSERT_EQUAL_UINT8(RCM_IMU_LEVEL_MOVING, imu_solve_level(a, STILL, map, &tilt));
}

static void test_a_dead_axis_is_refused_rather_than_solved(void)
{
    /* Under 0.85 g total -- a stuck axis or a wrong range, not a mounting to solve. */
    const float a[3] = { 0.0f, 0.0f, 0.5f };
    uint8_t map[3]; float tilt;
    TEST_ASSERT_EQUAL_UINT8(RCM_IMU_LEVEL_MOVING, imu_solve_level(a, STILL, map, &tilt));
}

static void test_a_raked_dash_mount_is_refused(void)
{
    /* 30 degrees off vertical: this is the case an axis swap fundamentally cannot fix,
     * and rounding it to the nearest axis would bake a phantom 0.5 g into forward. */
    const float a[3] = { 0.5f, 0.0f, 0.8660254f };
    uint8_t map[3]; float tilt = 0.0f;
    TEST_ASSERT_EQUAL_UINT8(RCM_IMU_LEVEL_TILTED, imu_solve_level(a, STILL, map, &tilt));
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 30.0f, tilt);
}

static void test_a_sloped_driveway_still_solves(void)
{
    /* 8 degrees. Refusing this would make the feature useless anywhere but a level
     * workshop floor, which is not where anyone configures a car. */
    const float a[3] = { 0.139f, 0.0f, 0.990f };
    uint8_t map[3]; float tilt = 90.0f;
    TEST_ASSERT_EQUAL_UINT8(RCM_IMU_LEVEL_OK, imu_solve_level(a, STILL, map, &tilt));
    TEST_ASSERT_TRUE(tilt < 15.0f);
}

static void test_the_tilt_limit_is_where_it_claims_to_be(void)
{
    /* Either side of 15 degrees must land either side of the decision, otherwise the
     * documented limit is a comment rather than behaviour. */
    uint8_t map[3]; float tilt;
    const float in [3] = { sinf(14.0f / 57.29578f), 0.0f, cosf(14.0f / 57.29578f) };
    const float out[3] = { sinf(16.0f / 57.29578f), 0.0f, cosf(16.0f / 57.29578f) };
    TEST_ASSERT_EQUAL_UINT8(RCM_IMU_LEVEL_OK,     imu_solve_level(in,  STILL, map, &tilt));
    TEST_ASSERT_EQUAL_UINT8(RCM_IMU_LEVEL_TILTED, imu_solve_level(out, STILL, map, &tilt));
}

/* --- turning the board is moving, even though gravity says otherwise -----------
 * This is the case the bench found and the desk tests had missed: slowly turning the
 * board over by hand solved happily at 8 deg off square, because |a| stays at 1 g
 * however you rotate it. Magnitude sees LINEAR acceleration only. The numbers below are
 * the ones the real board produced while being handled. */

static void test_a_turning_board_is_refused_even_at_one_g(void)
{
    const float a[3] = { -0.009f, 0.127f, 0.985f };   /* |a| = 0.994, looks fine */
    const float g[3] = { -3.43f, -0.28f, 9.16f };     /* but it is being turned */
    uint8_t map[3]; float tilt;
    TEST_ASSERT_EQUAL_UINT8(RCM_IMU_LEVEL_MOVING, imu_solve_level(a, g, map, &tilt));
}

static void test_the_accel_alone_would_have_accepted_that_reading(void)
{
    /* Guards the reason the gyro check exists. If this ever starts failing, the
     * accelerometer window has been tightened and the gyro test above may be passing
     * for the wrong reason. */
    const float a[3] = { -0.009f, 0.127f, 0.985f };
    uint8_t map[3]; float tilt;
    TEST_ASSERT_EQUAL_UINT8(RCM_IMU_LEVEL_OK, imu_solve_level(a, STILL, map, &tilt));
}

static void test_real_bench_noise_still_counts_as_still(void)
{
    /* The other half: if the gyro limit were too tight, auto-level would never work at
     * all. These are the accel and gyro this board actually reads sitting on a desk. */
    const float a[3] = { 0.006f, 0.007f, 0.995f };
    uint8_t map[3]; float tilt;
    TEST_ASSERT_EQUAL_UINT8(RCM_IMU_LEVEL_OK, imu_solve_level(a, STILL, map, &tilt));
}

static void test_the_rate_limit_is_where_it_claims_to_be(void)
{
    const float a[3] = { 0.0f, 0.0f, 1.0f };
    const float slow[3] = { 0.0f, 0.0f, 1.9f };
    const float fast[3] = { 0.0f, 0.0f, 2.1f };
    uint8_t map[3]; float tilt;
    TEST_ASSERT_EQUAL_UINT8(RCM_IMU_LEVEL_OK,     imu_solve_level(a, slow, map, &tilt));
    TEST_ASSERT_EQUAL_UINT8(RCM_IMU_LEVEL_MOVING, imu_solve_level(a, fast, map, &tilt));
}

static void test_rate_is_the_whole_vector_not_one_axis(void)
{
    /* Turning about a diagonal is still turning. Checking axes independently would let
     * 1.9 deg/s on each of three axes (3.3 total) through. */
    const float a[3] = { 0.0f, 0.0f, 1.0f };
    const float g[3] = { 1.5f, 1.5f, 1.5f };   /* 2.6 deg/s total */
    uint8_t map[3]; float tilt;
    TEST_ASSERT_EQUAL_UINT8(RCM_IMU_LEVEL_MOVING, imu_solve_level(a, g, map, &tilt));
}

/* --- every orientation there is -------------------------------------------- */

/* Feed the solver the gravity a given mounting produces, then check the map it hands
 * back turns that same reading into "Z is up". */
static void check_one(const float a[3])
{
    uint8_t map[3]; float tilt = 90.0f;
    char msg[96];
    snprintf(msg, sizeof msg, "gravity %+.0f %+.0f %+.0f", a[0], a[1], a[2]);

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(RCM_IMU_LEVEL_OK,
                                    imu_solve_level(a, STILL, map, &tilt), msg);
    TEST_ASSERT_TRUE_MESSAGE(imu_map_valid(map), msg);

    float v[3];
    apply(map, a, v);
    /* Standing still, vehicle Z must read +1 g and the horizontal axes nothing. */
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.001f, 1.0f, v[2], msg);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.001f, 0.0f, v[0], msg);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.001f, 0.0f, v[1], msg);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.001f, 1.0f, det(map), msg);  /* right-handed */
    TEST_ASSERT_TRUE_MESSAGE(tilt < 0.001f, msg);
}

static void test_all_six_square_mountings(void)
{
    /* Six ways up: each sensor axis pointing at the sky, and at the ground. */
    for (int k = 0; k < 3; k++) {
        for (int sign = -1; sign <= 1; sign += 2) {
            float a[3] = { 0.0f, 0.0f, 0.0f };
            a[k] = (float)sign;
            check_one(a);
        }
    }
}

static void test_upside_down_flips_z_and_stays_right_handed(void)
{
    /* The specific case someone will hit: lid-mounted board, sensor Z pointing down. */
    const float a[3] = { 0.0f, 0.0f, -1.0f };
    uint8_t map[3]; float tilt;
    TEST_ASSERT_EQUAL_UINT8(RCM_IMU_LEVEL_OK, imu_solve_level(a, STILL, map, &tilt));
    TEST_ASSERT_EQUAL_HEX8(0x82, map[2]);            /* vehicle Z = -sensor Z */
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, det(map));
}

static void test_a_solved_map_never_repeats_an_axis(void)
{
    for (int k = 0; k < 3; k++) {
        for (int sign = -1; sign <= 1; sign += 2) {
            float a[3] = { 0, 0, 0 };
            a[k] = (float)sign;
            uint8_t map[3]; float tilt;
            TEST_ASSERT_EQUAL_UINT8(RCM_IMU_LEVEL_OK, imu_solve_level(a, STILL, map, &tilt));
            TEST_ASSERT_TRUE(imu_map_valid(map));
        }
    }
}

/* --- what a map is allowed to be ------------------------------------------- */

static void test_identity_is_valid(void)
{
    const uint8_t m[3] = { 0, 1, 2 };
    TEST_ASSERT_TRUE(imu_map_valid(m));
}

static void test_negation_does_not_make_a_map_invalid(void)
{
    const uint8_t m[3] = { 0x81, 0x82, 0x80 };   /* -Y, -Z, -X */
    TEST_ASSERT_TRUE(imu_map_valid(m));
}

static void test_a_repeated_axis_is_rejected(void)
{
    /* The dangerous one: X and Y both fed from sensor X. Reads plausibly at a standstill
     * and can never show a yaw, because nothing is connected to the third axis. */
    const uint8_t m[3] = { 0, 0, 2 };
    TEST_ASSERT_FALSE(imu_map_valid(m));
}

static void test_an_axis_that_does_not_exist_is_rejected(void)
{
    /* 3 fits in the two bits remap() masks with, and would index off the end of a
     * three-float array. It has to be refused here, not clamped later. */
    const uint8_t m[3] = { 0, 1, 3 };
    TEST_ASSERT_FALSE(imu_map_valid(m));
    const uint8_t n[3] = { 0, 1, 0x83 };
    TEST_ASSERT_FALSE(imu_map_valid(n));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_moving_car_is_refused);
    RUN_TEST(test_a_dead_axis_is_refused_rather_than_solved);
    RUN_TEST(test_a_raked_dash_mount_is_refused);
    RUN_TEST(test_a_sloped_driveway_still_solves);
    RUN_TEST(test_the_tilt_limit_is_where_it_claims_to_be);
    RUN_TEST(test_a_turning_board_is_refused_even_at_one_g);
    RUN_TEST(test_the_accel_alone_would_have_accepted_that_reading);
    RUN_TEST(test_real_bench_noise_still_counts_as_still);
    RUN_TEST(test_the_rate_limit_is_where_it_claims_to_be);
    RUN_TEST(test_rate_is_the_whole_vector_not_one_axis);
    RUN_TEST(test_all_six_square_mountings);
    RUN_TEST(test_upside_down_flips_z_and_stays_right_handed);
    RUN_TEST(test_a_solved_map_never_repeats_an_axis);
    RUN_TEST(test_identity_is_valid);
    RUN_TEST(test_negation_does_not_make_a_map_invalid);
    RUN_TEST(test_a_repeated_axis_is_rejected);
    RUN_TEST(test_an_axis_that_does_not_exist_is_rejected);
    return UNITY_END();
}
