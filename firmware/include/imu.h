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

bool imu_begin(void);        /* false if the chip does not answer or init fails */
bool imu_ok(void);
void imu_tick(void);         /* reads the sensor; call at roughly imu_rate_ms */
void imu_broadcast(void);    /* emits the three MM5.10 frames */

/* Latest sample, in vehicle axes after the imu_map remap.
 * accel in g, gyro in deg/s. */
float imu_accel(uint8_t axis);
float imu_gyro(uint8_t axis);

#endif /* RCM_IMU_H */
