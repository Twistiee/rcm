/*
 * imu.h -- BMI270 on I2C1, published as Bosch MM5.10 CAN frames.
 *
 * The BMI270 is not a chip you can talk to with a handful of register writes: it
 * needs an 8KB configuration image uploaded after every power-on before it will
 * produce data. That image and the init sequence around it are Bosch's, vendored
 * under lib/bmi270 rather than reimplemented.
 *
 * Everything published is optional and gated on the CFG_IMU_EN strap, because only
 * one board in a car should be claiming to be the accelerometer.
 */
#ifndef RCM_IMU_H
#define RCM_IMU_H

#include <stdint.h>
#include <stdbool.h>

#include "protocol.h"   /* RCM_IMU_LEVEL_* */

bool imu_begin(void);        /* false if the chip does not answer or init fails */
bool imu_ok(void);
void imu_tick(void);         /* reads the sensor; call at roughly imu_rate_ms */
void imu_broadcast(void);    /* emits the three MM5.10 frames */

/* Latest sample, in vehicle axes after the imu_map remap.
 * accel in g, gyro in deg/s. */
float imu_accel(uint8_t axis);
float imu_gyro(uint8_t axis);

/* Latest sample in SENSOR axes, before the remap. Only auto-levelling wants this --
 * everything else should be reading vehicle axes. */
float imu_accel_raw(uint8_t axis);
float imu_gyro_raw(uint8_t axis);

/* --- orientation maths (imu_level.cpp, no hardware, host-tested) --------------
 * The orientation is two MEASURED unit vectors in SENSOR axes -- where up is and where
 * forward is -- not an axis permutation. A permutation can only snap to 90 degrees, and
 * a real bracket is never square: a 10 degree lean puts 0.17 g of gravity into the
 * longitudinal reading permanently. Vectors describe any angle exactly. */

/* Build vehicle-from-sensor rotation R (rows = vehicle X, Y, Z). False if the two
 * vectors cannot define a frame -- zero length, or forward parallel to up. */
bool imu_basis(const float up[3], const float fwd[3], float R[3][3]);

/* The default: board flat, +X edge pointing down the car. */
void imu_identity(float up[3], float fwd[3]);

/* One gravity reading -> unit vector, or an RCM_IMU_LEVEL_* refusal. There is no tilt
 * limit: any mounting angle is representable now. Still refuses if the board is moving
 * or turning, because then the reading is not gravity. */
uint8_t imu_solve_gravity(const float a[3], const float g[3], float out[3]);

/* Angle between two unit vectors, and whether they are far enough apart to define a
 * frame. The separation test is what catches "held the board flat during the FORWARD
 * step", whose answer would otherwise be the board normal. */
float imu_angle_deg(const float a[3], const float b[3]);
bool  imu_separated_enough(const float a[3], const float b[3]);

/* Reporting only: the nearest SQUARE mounting, and how far off square it really is. */
void imu_nearest_map(const float R[3][3], uint8_t map_out[3], float *off_deg);

/* Exact +/-1 vector for an axis-map byte, so the byte-oriented SET_IMU_MAP still works
 * and a square mounting stays exact. */
bool imu_vec_from_axis(uint8_t b, float out[3]);

/* True if a map names each of the three sensor axes exactly once. */
bool imu_map_valid(const uint8_t map[3]);

/* Re-measure UP from gravity, keeping the forward direction. Applied to cfg in RAM
 * only -- never auto-saved. */
uint8_t imu_autolevel(void);

/* Re-measure FORWARD: hold the board so the edge you want at the REAR of the car points
 * at the ground. The axis reading +1 g is then the one that will face forward. Keeps the
 * up direction. */
uint8_t imu_set_forward(void);

/* The live vehicle-from-sensor rotation, for reporting. */
void imu_current_basis(float R[3][3]);

/* Rebuild the rotation after cfg.imu_up / cfg.imu_fwd are written directly. */
void imu_reload_basis(void);
uint8_t imu_level_result(void);   /* last imu_autolevel() code, NONE before any */
uint8_t imu_level_tilt_deg(void); /* saturating, 90 if never attempted */
uint32_t imu_level_when(void);    /* millis() of the last attempt, 0 if never -- the
                                   * status LEDs use it to show the result briefly */

#endif /* RCM_IMU_H */
