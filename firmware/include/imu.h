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

/* Solve a map from one gravity reading. Returns an RCM_IMU_LEVEL_* code; map_out and
 * tilt_deg are only meaningful on RCM_IMU_LEVEL_OK. Pure maths, no hardware -- see
 * imu_level.cpp for what it can and cannot determine. */
uint8_t imu_solve_level(const float a[3], const float g[3], uint8_t map_out[3],
                        float *tilt_deg);

/* True if a map names each of the three sensor axes exactly once. */
bool imu_map_valid(const uint8_t map[3]);

/* Auto-level from the current reading and apply the result to cfg (RAM only, like
 * every other setter -- SAVE_CONFIG to keep it). Returns an RCM_IMU_LEVEL_* code and
 * stores it, with the measured tilt, for RCM_CFG_SEL_IMU to report. */
uint8_t imu_autolevel(void);
uint8_t imu_level_result(void);   /* last imu_autolevel() code, NONE before any */
uint8_t imu_level_tilt_deg(void); /* saturating, 90 if never attempted */

#endif /* RCM_IMU_H */
