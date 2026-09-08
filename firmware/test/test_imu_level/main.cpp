/*
 * test_imu_level -- where the IMU thinks up and forward are.
 *
 * The point of this suite is the case the old axis-map version could not represent at
 * all: a mounting that is NOT square. A signed permutation snaps to 90 degrees, so a
 * 10 degree lean was accepted and then ignored, putting 0.17g of gravity into the
 * longitudinal reading permanently. Several tests below exist purely to prove that a
 * leaning mount is now CORRECTED rather than tolerated.
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

static void apply(const float R[3][3], const float in[3], float out[3])
{
    for (int i = 0; i < 3; i++)
        out[i] = R[i][0] * in[0] + R[i][1] * in[1] + R[i][2] * in[2];
}

static float det(const float R[3][3])
{
    return R[0][0] * (R[1][1]*R[2][2] - R[1][2]*R[2][1])
         - R[0][1] * (R[1][0]*R[2][2] - R[1][2]*R[2][0])
         + R[0][2] * (R[1][0]*R[2][1] - R[1][1]*R[2][0]);
}

static void check_orthonormal(const float R[3][3], const char *msg)
{
    for (int i = 0; i < 3; i++) {
        float len = 0;
        for (int j = 0; j < 3; j++) len += R[i][j] * R[i][j];
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f, 1.0f, sqrtf(len), msg);
    }
    for (int i = 0; i < 3; i++)
        for (int k = i + 1; k < 3; k++) {
            float d = 0;
            for (int j = 0; j < 3; j++) d += R[i][j] * R[k][j];
            TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f, 0.0f, d, msg);
        }
    /* +1 not -1: a left-handed frame reads plausibly at a standstill and only shows up
     * as the car yawing the wrong way through a corner. */
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f, 1.0f, det(R), msg);
}

/* --- the whole reason this was rewritten ----------------------------------- */

static void test_a_leaning_mount_is_CORRECTED_not_merely_tolerated(void)
{
    /* Board mounted 10 degrees off flat. Under the old axis map this was accepted and
     * ignored, leaving sin(10) = 0.174g of gravity in the longitudinal channel forever.
     * Levelling against that same gravity must now null it completely. */
    const float lean = 10.0f / 57.29578f;
    const float g_at_rest[3] = { sinf(lean), 0.0f, cosf(lean) };

    float up[3];
    TEST_ASSERT_EQUAL_UINT8(RCM_IMU_LEVEL_OK, imu_solve_gravity(g_at_rest, STILL, up));

    float fwd[3] = { 1.0f, 0.0f, 0.0f };
    float R[3][3];
    TEST_ASSERT_TRUE(imu_basis(up, fwd, R));

    float v[3];
    apply(R, g_at_rest, v);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.001f, 1.0f, v[2], "vertical should read a clean 1g");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.001f, 0.0f, v[0], "gravity leaked into LONGITUDINAL");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.001f, 0.0f, v[1], "gravity leaked into LATERAL");
}

static void test_the_lean_this_is_about_really_is_that_big(void)
{
    /* Pins the error being removed: an axis snap leaves this much gravity in the
     * longitudinal channel, which is most of a moderate braking event. */
    const float lean = 10.0f / 57.29578f;
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.005f, 0.174f, sinf(lean),
        "the lean this test is about is not 0.17g");
}

static void test_a_raked_dash_is_now_just_another_mounting(void)
{
    /* 30 degrees. The old solver REFUSED this outright, because no axis map could say
     * it. There is no tilt limit any more. */
    const float a[3] = { 0.5f, 0.0f, 0.8660254f };
    float up[3];
    TEST_ASSERT_EQUAL_UINT8(RCM_IMU_LEVEL_OK, imu_solve_gravity(a, STILL, up));

    float fwd[3] = { 1.0f, 0.0f, 0.0f };
    float R[3][3];
    TEST_ASSERT_TRUE(imu_basis(up, fwd, R));
    float v[3];
    apply(R, a, v);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, v[2]);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, v[0]);
}

/* --- still refusing what is genuinely not gravity -------------------------- */

static void test_a_moving_car_is_refused(void)
{
    const float a[3] = { -0.9f, 0.0f, 1.0f };      /* braking: well over 1g */
    float out[3];
    TEST_ASSERT_EQUAL_UINT8(RCM_IMU_LEVEL_MOVING, imu_solve_gravity(a, STILL, out));
}

static void test_a_turning_board_is_refused_even_at_one_g(void)
{
    /* Found on the bench: turning a board keeps |a| at 1g, because rotation does not
     * change the magnitude of gravity. Measured numbers from that session. */
    const float a[3] = { -0.009f, 0.127f, 0.985f };   /* |a| = 0.994, looks perfect */
    const float g[3] = { -3.43f, -0.28f, 9.16f };     /* but it is being turned */
    float out[3];
    TEST_ASSERT_EQUAL_UINT8(RCM_IMU_LEVEL_MOVING, imu_solve_gravity(a, g, out));
}

static void test_the_accel_alone_would_have_accepted_that_reading(void)
{
    const float a[3] = { -0.009f, 0.127f, 0.985f };
    float out[3];
    TEST_ASSERT_EQUAL_UINT8(RCM_IMU_LEVEL_OK, imu_solve_gravity(a, STILL, out));
}

static void test_real_bench_noise_still_counts_as_still(void)
{
    const float a[3] = { 0.006f, 0.007f, 0.995f };
    float out[3];
    TEST_ASSERT_EQUAL_UINT8(RCM_IMU_LEVEL_OK, imu_solve_gravity(a, STILL, out));
}

static void test_a_dead_axis_is_refused(void)
{
    const float a[3] = { 0.0f, 0.0f, 0.5f };
    float out[3];
    TEST_ASSERT_EQUAL_UINT8(RCM_IMU_LEVEL_MOVING, imu_solve_gravity(a, STILL, out));
}

static void test_the_rate_limit_is_where_it_claims_to_be(void)
{
    const float a[3] = { 0.0f, 0.0f, 1.0f };
    const float slow[3] = { 0.0f, 0.0f, 1.9f };
    const float fast[3] = { 0.0f, 0.0f, 2.1f };
    float out[3];
    TEST_ASSERT_EQUAL_UINT8(RCM_IMU_LEVEL_OK,     imu_solve_gravity(a, slow, out));
    TEST_ASSERT_EQUAL_UINT8(RCM_IMU_LEVEL_MOVING, imu_solve_gravity(a, fast, out));
}

static void test_rate_is_the_whole_vector_not_one_axis(void)
{
    const float a[3] = { 0.0f, 0.0f, 1.0f };
    const float g[3] = { 1.5f, 1.5f, 1.5f };   /* 2.6 deg/s total */
    float out[3];
    TEST_ASSERT_EQUAL_UINT8(RCM_IMU_LEVEL_MOVING, imu_solve_gravity(a, g, out));
}

/* --- building a frame ------------------------------------------------------ */

static void test_identity_is_identity(void)
{
    float up[3], fwd[3], R[3][3];
    imu_identity(up, fwd);
    TEST_ASSERT_TRUE(imu_basis(up, fwd, R));
    check_orthonormal(R, "identity");
    const float in[3] = { 0.3f, -0.4f, 0.9f };
    float out[3];
    apply(R, in, out);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, in[0], out[0]);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, in[1], out[1]);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, in[2], out[2]);
}

static void test_every_square_mounting_builds_a_right_handed_frame(void)
{
    /* All 24: six choices of which axis is up, four of which is forward. */
    int built = 0;
    for (int uk = 0; uk < 3; uk++) for (int us = -1; us <= 1; us += 2)
    for (int fk = 0; fk < 3; fk++) for (int fs = -1; fs <= 1; fs += 2) {
        if (uk == fk) continue;
        float up[3] = {0,0,0}, fwd[3] = {0,0,0}, R[3][3];
        up[uk] = (float)us; fwd[fk] = (float)fs;
        char msg[64];
        snprintf(msg, sizeof msg, "up=%d%d fwd=%d%d", uk, us, fk, fs);
        TEST_ASSERT_TRUE_MESSAGE(imu_basis(up, fwd, R), msg);
        check_orthonormal(R, msg);
        built++;
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(24, built, "expected all 24 orthogonal mountings");
}

static void test_a_sloppy_forward_is_squared_up_against_gravity(void)
{
    /* Nobody holds a board exactly perpendicular. The component of forward along up is
     * the error and is removed, so a hand-held aim still yields a proper frame. */
    float up[3]  = { 0.0f, 0.0f, 1.0f };
    float fwd[3] = { 1.0f, 0.0f, 0.30f };        /* about 17 degrees out */
    float R[3][3];
    TEST_ASSERT_TRUE(imu_basis(up, fwd, R));
    check_orthonormal(R, "sloppy forward");
    /* Up is measured against gravity and is trusted; forward is the one adjusted. */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, R[0][2]);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, R[2][2]);
}

static void test_forward_parallel_to_up_cannot_build_a_frame(void)
{
    float up[3]  = { 0.0f, 0.0f, 1.0f };
    float fwd[3] = { 0.0f, 0.0f, 1.0f };
    float R[3][3];
    TEST_ASSERT_FALSE(imu_basis(up, fwd, R));
}

static void test_a_zero_vector_cannot_build_a_frame(void)
{
    float up[3]  = { 0.0f, 0.0f, 0.0f };
    float fwd[3] = { 1.0f, 0.0f, 0.0f };
    float R[3][3];
    TEST_ASSERT_FALSE(imu_basis(up, fwd, R));
}

/* --- the separation guard -------------------------------------------------- */

static void test_holding_it_flat_during_the_forward_step_is_rejected(void)
{
    /* The answer would be the board normal, which is not a direction a car travels in. */
    const float up[3]   = { 0.0f, 0.0f, 1.0f };
    const float flat[3] = { 0.0f, 0.0f, 1.0f };
    TEST_ASSERT_FALSE(imu_separated_enough(up, flat));
}

static void test_upside_down_is_also_too_close_to_parallel(void)
{
    const float up[3]    = { 0.0f, 0.0f, 1.0f };
    const float under[3] = { 0.0f, 0.0f, -1.0f };
    TEST_ASSERT_FALSE_MESSAGE(imu_separated_enough(up, under),
        "180 degrees apart is just as unusable as 0 -- still no cross product");
}

static void test_a_hand_held_vertical_is_comfortably_accepted(void)
{
    /* The real ask: hold a board on edge by hand. 20 degrees of slop is allowed, so it
     * does not demand precision from someone lying under a dash. */
    const float up[3]   = { 0.0f, 0.0f, 1.0f };
    const float held[3] = { 0.94f, 0.0f, 0.34f };    /* about 20 deg off perpendicular */
    TEST_ASSERT_TRUE(imu_separated_enough(up, held));
}

/* --- reporting -------------------------------------------------------------- */

static void test_the_nearest_square_mounting_is_reported_with_its_error(void)
{
    const float lean = 10.0f / 57.29578f;
    float up[3]  = { sinf(lean), 0.0f, cosf(lean) };
    float fwd[3] = { 1.0f, 0.0f, 0.0f };
    float R[3][3];
    TEST_ASSERT_TRUE(imu_basis(up, fwd, R));

    uint8_t m[3]; float off = 0.0f;
    imu_nearest_map(R, m, &off);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(2, m[2], "nearest up axis should still be sensor Z");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1.0f, 10.0f, off,
        "the report must state HOW FAR off square, not hide it");
}

static void test_a_square_mounting_reports_zero_error(void)
{
    float up[3], fwd[3], R[3][3];
    imu_identity(up, fwd);
    TEST_ASSERT_TRUE(imu_basis(up, fwd, R));
    uint8_t m[3]; float off = 99.0f;
    imu_nearest_map(R, m, &off);
    TEST_ASSERT_EQUAL_HEX8(0, m[0]);
    TEST_ASSERT_EQUAL_HEX8(1, m[1]);
    TEST_ASSERT_EQUAL_HEX8(2, m[2]);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, off);
}

/* --- the byte-oriented setter still works ---------------------------------- */

static void test_axis_bytes_become_exact_unit_vectors(void)
{
    float v[3];
    TEST_ASSERT_TRUE(imu_vec_from_axis(0x81, v));      /* -Y */
    TEST_ASSERT_FLOAT_WITHIN(1e-6f,  0.0f, v[0]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, -1.0f, v[1]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f,  0.0f, v[2]);
}

static void test_a_fourth_axis_has_no_vector(void)
{
    float v[3];
    TEST_ASSERT_FALSE(imu_vec_from_axis(3, v));
}

static void test_the_keypad_own_mounting_round_trips(void)
{
    /* ny x z -- terminal edge to the right of the car. Set by hand, so it must survive
     * being expressed as vectors and read back as the same square mounting. */
    float fwd[3], up[3], R[3][3];
    TEST_ASSERT_TRUE(imu_vec_from_axis(0x81, fwd));    /* X = -sensor Y */
    TEST_ASSERT_TRUE(imu_vec_from_axis(0x02, up));     /* Z = +sensor Z */
    TEST_ASSERT_TRUE(imu_basis(up, fwd, R));
    check_orthonormal(R, "ny x z");

    uint8_t m[3]; float off = 99.0f;
    imu_nearest_map(R, m, &off);
    TEST_ASSERT_EQUAL_HEX8(0x81, m[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00, m[1]);
    TEST_ASSERT_EQUAL_HEX8(0x02, m[2]);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, off);
}

static void test_map_validation_still_rejects_a_repeated_axis(void)
{
    const uint8_t m[3] = { 0, 0, 2 };
    TEST_ASSERT_FALSE(imu_map_valid(m));
    const uint8_t n[3] = { 0, 1, 3 };
    TEST_ASSERT_FALSE(imu_map_valid(n));
    const uint8_t ok[3] = { 0x81, 0x00, 0x02 };
    TEST_ASSERT_TRUE(imu_map_valid(ok));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_leaning_mount_is_CORRECTED_not_merely_tolerated);
    RUN_TEST(test_the_lean_this_is_about_really_is_that_big);
    RUN_TEST(test_a_raked_dash_is_now_just_another_mounting);
    RUN_TEST(test_a_moving_car_is_refused);
    RUN_TEST(test_a_turning_board_is_refused_even_at_one_g);
    RUN_TEST(test_the_accel_alone_would_have_accepted_that_reading);
    RUN_TEST(test_real_bench_noise_still_counts_as_still);
    RUN_TEST(test_a_dead_axis_is_refused);
    RUN_TEST(test_the_rate_limit_is_where_it_claims_to_be);
    RUN_TEST(test_rate_is_the_whole_vector_not_one_axis);
    RUN_TEST(test_identity_is_identity);
    RUN_TEST(test_every_square_mounting_builds_a_right_handed_frame);
    RUN_TEST(test_a_sloppy_forward_is_squared_up_against_gravity);
    RUN_TEST(test_forward_parallel_to_up_cannot_build_a_frame);
    RUN_TEST(test_a_zero_vector_cannot_build_a_frame);
    RUN_TEST(test_holding_it_flat_during_the_forward_step_is_rejected);
    RUN_TEST(test_upside_down_is_also_too_close_to_parallel);
    RUN_TEST(test_a_hand_held_vertical_is_comfortably_accepted);
    RUN_TEST(test_the_nearest_square_mounting_is_reported_with_its_error);
    RUN_TEST(test_a_square_mounting_reports_zero_error);
    RUN_TEST(test_axis_bytes_become_exact_unit_vectors);
    RUN_TEST(test_a_fourth_axis_has_no_vector);
    RUN_TEST(test_the_keypad_own_mounting_round_trips);
    RUN_TEST(test_map_validation_still_rejects_a_repeated_axis);
    return UNITY_END();
}
