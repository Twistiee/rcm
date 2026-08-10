/*
 * imu.cpp -- BMI270 -> Bosch MM5.10 frames.
 *
 * ===========================================================================
 * WHY MM5.10 AND NOT OUR OWN FRAME
 * ===========================================================================
 * rusEFI already decodes Bosch MM5.10 accelerometer frames -- see can_rx.cpp in
 * their firmware, which handles 0x174 / 0x178 / 0x17C and drops the values straight
 * into engine->sensors.accelerometer. Our sensor is a Bosch BMI270. Emitting the
 * frames the ECU is already listening for means yaw rate and lateral/longitudinal/
 * vertical G land in rusEFI with nothing to set up but imuType = IMU_MM5_10.
 *
 * We emit only the fields rusEFI reads: bytes 0-1 and 4-5. A real MM5.10 also puts
 * a status nibble, a rolling counter and a CRC in the gaps. If you ever put this on
 * a bus with something that validates those, it will reject these frames -- that is
 * a deliberate trade, not an oversight.
 *
 * ===========================================================================
 * RANGES
 * ===========================================================================
 * The MM5.10 encoding is fixed: 16-bit offset binary biased by 0x8000, with
 * 0.0001274 g and 0.005 deg/s per LSB. That gives +-4.17g and +-163.8 deg/s of
 * encodable range, so the sensor is configured to match rather than exceed it:
 *   accel +-4g   -- fits inside the encoding with a little to spare
 *   gyro  +-250 deg/s -- wider than the encoding, so yaw CLIPS at 163.8 deg/s.
 * That clip is fine for a car (163 deg/s is most of the way through a spin) and the
 * alternative, a narrower gyro range, costs resolution everywhere for a number
 * nobody reads. Both encoders saturate rather than wrap; a wrapped yaw rate would
 * tell the ECU the car had suddenly turned the other way.
 */
#include <Arduino.h>
#include <Wire.h>
#include "board.h"
#include "canbus.h"
#include "config.h"
#include "imu.h"
#include "protocol.h"

extern "C" {
#include "bmi270.h"
}

#define ACC_RANGE_G    4.0f
#define GYR_RANGE_DPS  250.0f

static TwoWire imu_wire(PIN_IMU_SDA, PIN_IMU_SCL);
static struct bmi2_dev dev;
static uint8_t dev_addr = IMU_I2C_ADDR;
static bool ready;

static float acc_v[3];   /* vehicle axes, g     */
static float gyr_v[3];   /* vehicle axes, deg/s */

/* --- Bosch API interface shims --------------------------------------------- */

static BMI2_INTF_RETURN_TYPE i2c_read(uint8_t reg, uint8_t *data, uint32_t len, void *intf)
{
    const uint8_t addr = *(uint8_t *)intf;
    imu_wire.beginTransmission(addr);
    imu_wire.write(reg);
    if (imu_wire.endTransmission(false) != 0) return BMI2_E_COM_FAIL;   /* repeated start */

    uint32_t got = imu_wire.requestFrom((int)addr, (int)len);
    if (got != len) return BMI2_E_COM_FAIL;
    for (uint32_t i = 0; i < len; i++) data[i] = (uint8_t)imu_wire.read();
    return BMI2_INTF_RET_SUCCESS;
}

static BMI2_INTF_RETURN_TYPE i2c_write(uint8_t reg, const uint8_t *data, uint32_t len, void *intf)
{
    const uint8_t addr = *(uint8_t *)intf;
    imu_wire.beginTransmission(addr);
    imu_wire.write(reg);
    for (uint32_t i = 0; i < len; i++) imu_wire.write(data[i]);
    return imu_wire.endTransmission() == 0 ? BMI2_INTF_RET_SUCCESS : BMI2_E_COM_FAIL;
}

static void delay_us(uint32_t period, void *intf)
{
    (void)intf;
    delayMicroseconds(period);
}

/* --- init ------------------------------------------------------------------ */

bool imu_begin(void)
{
    ready = false;

    imu_wire.begin();
    imu_wire.setClock(400000);

    dev.intf     = BMI2_I2C_INTF;
    dev.read     = i2c_read;
    dev.write    = i2c_write;
    dev.delay_us = delay_us;
    dev.intf_ptr = &dev_addr;
    /* The 8KB config image is uploaded in chunks of this size. The Arduino Wire
     * buffer on STM32 is 32 bytes by default, and asking for more silently
     * truncates the burst -- which shows up as a config upload that "succeeds" and
     * a sensor that never reports ready. */
    dev.read_write_len   = 32;
    dev.config_file_ptr  = NULL;     /* use the image built into the Bosch driver */

    if (bmi270_init(&dev) != BMI2_OK) return false;

    uint8_t sens[2] = { BMI2_ACCEL, BMI2_GYRO };
    if (bmi2_sensor_enable(sens, 2, &dev) != BMI2_OK) return false;

    struct bmi2_sens_config sc[2];
    sc[0].type = BMI2_ACCEL;
    sc[1].type = BMI2_GYRO;
    if (bmi2_get_sensor_config(sc, 2, &dev) != BMI2_OK) return false;

    sc[0].cfg.acc.odr         = BMI2_ACC_ODR_100HZ;   /* 2x our 50Hz publish rate */
    sc[0].cfg.acc.range       = BMI2_ACC_RANGE_4G;
    sc[0].cfg.acc.bwp         = BMI2_ACC_NORMAL_AVG4;
    sc[0].cfg.acc.filter_perf = BMI2_PERF_OPT_MODE;

    sc[1].cfg.gyr.odr         = BMI2_GYR_ODR_100HZ;
    sc[1].cfg.gyr.range       = BMI2_GYR_RANGE_250;
    sc[1].cfg.gyr.bwp         = BMI2_GYR_NORMAL_MODE;
    sc[1].cfg.gyr.noise_perf  = BMI2_PERF_OPT_MODE;
    sc[1].cfg.gyr.filter_perf = BMI2_PERF_OPT_MODE;
    sc[1].cfg.gyr.ois_range   = BMI2_GYR_OIS_250;

    if (bmi2_set_sensor_config(sc, 2, &dev) != BMI2_OK) return false;

    ready = true;
    return true;
}

bool imu_ok(void) { return ready; }

/* --- read ------------------------------------------------------------------ */

/* imu_map[i] = source axis for vehicle axis i, bit 7 = negate. */
static inline float remap(const float *src, uint8_t axis)
{
    const uint8_t m = cfg.imu_map[axis];
    const uint8_t s = m & 0x03;
    return (m & 0x80) ? -src[s] : src[s];
}

void imu_tick(void)
{
    if (!ready) return;

    struct bmi2_sens_data d;
    if (bmi2_get_sensor_data(&d, &dev) != BMI2_OK) return;

    const float a[3] = { (float)d.acc.x * ACC_RANGE_G   / 32768.0f,
                         (float)d.acc.y * ACC_RANGE_G   / 32768.0f,
                         (float)d.acc.z * ACC_RANGE_G   / 32768.0f };
    const float g[3] = { (float)d.gyr.x * GYR_RANGE_DPS / 32768.0f,
                         (float)d.gyr.y * GYR_RANGE_DPS / 32768.0f,
                         (float)d.gyr.z * GYR_RANGE_DPS / 32768.0f };

    for (uint8_t i = 0; i < 3; i++) {
        acc_v[i] = remap(a, i);
        gyr_v[i] = remap(g, i);
    }
}

float imu_accel(uint8_t axis) { return axis < 3 ? acc_v[axis] : 0.0f; }
float imu_gyro(uint8_t axis)  { return axis < 3 ? gyr_v[axis] : 0.0f; }

/* --- publish --------------------------------------------------------------- */

/* Offset binary, saturating. rusEFI does (value - 0x8000) * quant, so this is
 * exactly that arithmetic run backwards. */
static uint16_t mm5_encode(float physical, float quant)
{
    float lsb = physical / quant;
    if (lsb >  32767.0f) lsb =  32767.0f;
    if (lsb < -32768.0f) lsb = -32768.0f;
    return (uint16_t)((int32_t)lroundf(lsb) + 0x8000);
}

static inline void put16(uint8_t *d, uint16_t v) { d[0] = (uint8_t)v; d[1] = (uint8_t)(v >> 8); }

void imu_broadcast(void)
{
    if (!ready) return;
    uint8_t d[8];

    /* 0x174: yaw rate at [0:2], lateral (Y) accel at [4:6].
     * Yaw is rotation about the vehicle's vertical axis, so it is gyro Z. */
    memset(d, 0, sizeof(d));
    put16(&d[0], mm5_encode(gyr_v[2], MM5_10_RATE_QUANT));
    put16(&d[4], mm5_encode(acc_v[1], MM5_10_ACC_QUANT));
    can_send(MM5_10_ID_YAW_Y, d, 8);

    /* 0x178: longitudinal (X) accel at [4:6]. rusEFI ignores [0:2] on this one. */
    memset(d, 0, sizeof(d));
    put16(&d[4], mm5_encode(acc_v[0], MM5_10_ACC_QUANT));
    can_send(MM5_10_ID_ROLL_X, d, 8);

    /* 0x17C: vertical (Z) accel at [4:6]. */
    memset(d, 0, sizeof(d));
    put16(&d[4], mm5_encode(acc_v[2], MM5_10_ACC_QUANT));
    can_send(MM5_10_ID_Z, d, 8);
}
