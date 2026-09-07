/*
 * imu_level.cpp -- work out imu_map from a single gravity reading.
 *
 * Split out of imu.cpp deliberately: imu.cpp cannot be built on a PC because it drags
 * in Bosch's BMI270 driver, and this is the only part worth testing exhaustively. It
 * touches no hardware and no config -- it is handed three numbers and returns three
 * bytes, so the host suite can walk every mounting orientation there is.
 *
 * WHAT IT CAN AND CANNOT DO
 * Standing still, the only acceleration is gravity, so the accelerometer points at the
 * sky. That fixes ONE vehicle axis: Z. It says nothing whatever about the other two --
 * spin the board on a level bench and gravity never changes -- so forward and left
 * cannot be solved this way and are not guessed at. They are filled in with a
 * right-handed pair and left for the installer to correct, because a wrong-handed set
 * would report the car yawing the wrong way, which is far worse than a swapped X/Y.
 */
#include <math.h>
#include "imu.h"

/* Tilt beyond this and we refuse. An axis swap can only ever describe a square mount,
 * so a board on a raked dash cannot be corrected here -- reporting that is the honest
 * answer, and silently rounding to the nearest axis would bake gravity into the
 * forward reading as a permanent phantom acceleration. 15 degrees is loose enough to
 * survive a sloped driveway and tight enough that a real dash rake fails it. */
#define LEVEL_MAX_TILT_DEG   15.0f

/* Standing still, |a| is 1 g. Anything else means the reading is not just gravity:
 * engine shaking the car, someone leaning on it, or a scaling fault. */
#define LEVEL_G_MIN          0.85f
#define LEVEL_G_MAX          1.15f

/* ...but |a| alone does NOT mean "standing still", and assuming it did was a real bug
 * found on the bench: a board being slowly turned over by hand solved happily at 8 deg
 * off square, because gravity still totals 1 g whichever way you point it. Magnitude
 * only sees LINEAR acceleration; rotation is invisible to it, and rotation is exactly
 * what someone does to a board while fitting it.
 *
 * The gyro says it outright. Sitting on the bench this board reads about 0.1-0.3 deg/s
 * of noise; being handled it read 9 deg/s. 2 deg/s sits an order of magnitude above the
 * noise and well below anything a person does, so it separates the two cleanly. */
#define LEVEL_MAX_RATE_DPS   2.0f

uint8_t imu_solve_level(const float a[3], const float g[3], uint8_t map_out[3],
                        float *tilt_deg)
{
    const float mag = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    if (tilt_deg) *tilt_deg = 90.0f;

    if (mag < LEVEL_G_MIN || mag > LEVEL_G_MAX) return RCM_IMU_LEVEL_MOVING;

    /* Turning counts as moving even when gravity still adds up to 1 g. */
    const float rate = sqrtf(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
    if (rate > LEVEL_MAX_RATE_DPS) return RCM_IMU_LEVEL_MOVING;

    /* Whichever sensor axis carries most of gravity is the vertical one. */
    uint8_t k = 0;
    for (uint8_t i = 1; i < 3; i++)
        if (fabsf(a[i]) > fabsf(a[k])) k = i;

    /* How far off vertical that axis actually is. acosf is clamped because rounding can
     * push the ratio a hair past 1.0 and acosf(1.0000001) is NaN, which would compare
     * false against the limit and let a bad mount through. */
    float c = fabsf(a[k]) / mag;
    if (c > 1.0f) c = 1.0f;
    const float tilt = acosf(c) * 57.29578f;
    if (tilt_deg) *tilt_deg = tilt;
    if (tilt > LEVEL_MAX_TILT_DEG) return RCM_IMU_LEVEL_TILTED;

    /* At rest an accelerometer reads +1 g along whichever axis points UP, so vehicle Z
     * (up) is +k when a[k] is positive and -k when it is negative. */
    const bool neg_z = (a[k] < 0.0f);
    map_out[2] = (uint8_t)(k | (neg_z ? 0x80 : 0x00));

    /* X and Y are unknowable from gravity. Taking them in cyclic order after k makes
     * the bare permutation even, so the whole set is right-handed exactly when Z was
     * not negated -- and when it was, negating Y (which is already a guess) restores
     * it without touching the one axis we actually solved. */
    map_out[0] = (uint8_t)((k + 1) % 3);
    map_out[1] = (uint8_t)((k + 2) % 3);
    if (neg_z) map_out[1] |= 0x80;

    return RCM_IMU_LEVEL_OK;
}

bool imu_map_valid(const uint8_t map[3])
{
    uint8_t seen = 0;
    for (uint8_t i = 0; i < 3; i++) {
        const uint8_t ax = map[i] & 0x7F;
        if (ax > 2) return false;          /* names an axis that does not exist */
        seen = (uint8_t)(seen | (1u << ax));
    }
    return seen == 0x07;                   /* all three, each exactly once */
}
