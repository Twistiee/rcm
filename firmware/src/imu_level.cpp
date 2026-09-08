/*
 * imu_level.cpp -- where the IMU thinks "up" and "forward" are.
 *
 * Split out of imu.cpp deliberately: imu.cpp cannot be built on a PC because it drags
 * in Bosch's BMI270 driver, and this is the part worth testing exhaustively. It touches
 * no hardware and no config -- handed vectors, it returns vectors -- so the host suite
 * can walk orientations no one would bother to build on a bench.
 *
 * WHY VECTORS AND NOT AN AXIS MAP
 * The first version stored a signed axis permutation: each vehicle axis was +/-1 times a
 * sensor axis. That is exact and cheap, and it cannot describe a mounting that is not
 * square. A real bracket in a real car is never square, and the error does not announce
 * itself -- a 10 degree lean silently puts 0.17g of gravity into the longitudinal
 * reading, permanently, which is most of a moderate braking event. The board reports it
 * as confident, plausible, wrong motion forever.
 *
 * So the orientation is two MEASURED unit vectors in sensor axes -- where UP is, and
 * where FORWARD is -- orthonormalised into a rotation matrix. Any angle is representable,
 * there is no tolerance to fall outside, and the axis-aligned case still comes out exact
 * because the vectors are then exactly +/-1.
 *
 * Vehicle axes are the automotive convention: X forward, Y left, Z up.
 */
#include <math.h>
#include <string.h>
#include "imu.h"

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

/* Two directions that are nearly the same direction cannot define a frame: the cross
 * product collapses and the maths turns to noise. This is what catches "held the board
 * flat while doing the FORWARD step" -- the answer would otherwise be the board normal,
 * which is not a direction the car can travel in. 20 degrees is far outside anything a
 * person aiming for perpendicular would produce. */
#define BASIS_MIN_SEP_DEG    20.0f

static float dot3(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void cross3(const float a[3], const float b[3], float out[3])
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static bool norm3(float v[3])
{
    const float m = sqrtf(dot3(v, v));
    if (m < 1e-4f) return false;              /* degenerate: no direction at all */
    v[0] /= m; v[1] /= m; v[2] /= m;
    return true;
}

/* Rows of R are the vehicle axes expressed in sensor axes, so vehicle = R * sensor.
 *
 * FORWARD is orthogonalised against UP rather than the other way round: up is measured
 * against gravity and is the more trustworthy of the two, while forward is whatever
 * angle someone managed to hold a board at. Any component of forward along up is
 * therefore the error, and is removed. */
bool imu_basis(const float up[3], const float fwd[3], float R[3][3])
{
    float u[3] = { up[0], up[1], up[2] };
    float f[3] = { fwd[0], fwd[1], fwd[2] };
    if (!norm3(u) || !norm3(f)) return false;

    const float along = dot3(f, u);
    f[0] -= along * u[0];
    f[1] -= along * u[1];
    f[2] -= along * u[2];
    if (!norm3(f)) return false;              /* forward was parallel to up */

    float y[3];
    cross3(u, f, y);                          /* Y = Z x X, which is what makes it
                                               * right-handed rather than a mirror */
    if (!norm3(y)) return false;

    R[0][0] = f[0]; R[0][1] = f[1]; R[0][2] = f[2];   /* vehicle X = forward */
    R[1][0] = y[0]; R[1][1] = y[1]; R[1][2] = y[2];   /* vehicle Y = left    */
    R[2][0] = u[0]; R[2][1] = u[1]; R[2][2] = u[2];   /* vehicle Z = up      */
    return true;
}

void imu_identity(float up[3], float fwd[3])
{
    up[0]  = 0.0f; up[1]  = 0.0f; up[2]  = 1.0f;
    fwd[0] = 1.0f; fwd[1] = 0.0f; fwd[2] = 0.0f;
}

/* Turn one gravity reading into a unit vector, or say why not.
 *
 * There is deliberately NO tilt limit any more. The old solver refused past 15 degrees
 * because an axis map could not express the remainder; a rotation matrix can express
 * any angle, so a raked dash is now just another mounting rather than a refusal. */
uint8_t imu_solve_gravity(const float a[3], const float g[3], float out[3])
{
    const float mag = sqrtf(dot3(a, a));
    if (mag < LEVEL_G_MIN || mag > LEVEL_G_MAX) return RCM_IMU_LEVEL_MOVING;

    /* Turning counts as moving even when gravity still adds up to 1 g. */
    if (sqrtf(dot3(g, g)) > LEVEL_MAX_RATE_DPS) return RCM_IMU_LEVEL_MOVING;

    out[0] = a[0]; out[1] = a[1]; out[2] = a[2];
    if (!norm3(out)) return RCM_IMU_LEVEL_MOVING;
    return RCM_IMU_LEVEL_OK;
}

/* Angle between two unit vectors, degrees. */
float imu_angle_deg(const float a[3], const float b[3])
{
    float c = dot3(a, b);
    if (c >  1.0f) c =  1.0f;                 /* rounding can push acosf to NaN, and a
                                               * NaN compares false against every limit */
    if (c < -1.0f) c = -1.0f;
    return acosf(c) * 57.29578f;
}

bool imu_separated_enough(const float a[3], const float b[3])
{
    const float d = imu_angle_deg(a, b);
    return d > BASIS_MIN_SEP_DEG && d < (180.0f - BASIS_MIN_SEP_DEG);
}

/* Describe an orientation the way a person reads it: the nearest square mounting, plus
 * how far off square it actually is. Purely for reporting -- nothing steers by it. */
void imu_nearest_map(const float R[3][3], uint8_t map_out[3], float *off_deg)
{
    float worst = 0.0f;
    for (uint8_t i = 0; i < 3; i++) {
        uint8_t k = 0;
        for (uint8_t j = 1; j < 3; j++)
            if (fabsf(R[i][j]) > fabsf(R[i][k])) k = j;
        map_out[i] = (uint8_t)(k | ((R[i][k] < 0.0f) ? 0x80 : 0x00));

        float c = fabsf(R[i][k]);
        if (c > 1.0f) c = 1.0f;
        const float d = acosf(c) * 57.29578f;
        if (d > worst) worst = d;
    }
    if (off_deg) *off_deg = worst;
}

/* Exact unit vectors for an axis map byte (axis in the low bits, bit 7 negates). Lets
 * the byte-oriented SET_IMU_MAP keep working against a vector store, and it lands on
 * exactly +/-1 so a square mounting stays exact. */
bool imu_vec_from_axis(uint8_t b, float out[3])
{
    const uint8_t ax = b & 0x7F;
    if (ax > 2) return false;
    out[0] = out[1] = out[2] = 0.0f;
    out[ax] = (b & 0x80) ? -1.0f : 1.0f;
    return true;
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
