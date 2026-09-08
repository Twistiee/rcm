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
static float acc_r[3];   /* sensor axes, g     -- auto-level solves from these; the  */
static float gyr_r[3];   /* sensor axes, deg/s -- remap is what everything else reads */
static uint8_t level_res  = RCM_IMU_LEVEL_NONE;
static uint8_t level_tilt = 90;
static uint32_t level_at;
static void rebuild_basis(void);

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

    rebuild_basis();
    ready = true;
    return true;
}

bool imu_ok(void) { return ready; }

/* --- read ------------------------------------------------------------------ */

/* The rotation is rebuilt whenever the stored vectors change rather than every sample:
 * it is the same answer each time, and imu_tick() runs at the sensor rate. */
static float basis[3][3];
static bool  basis_ok;

static void rebuild_basis(void)
{
    basis_ok = imu_basis(cfg.imu_up, cfg.imu_fwd, basis);
    if (!basis_ok) {
        /* Whatever is stored cannot define a frame -- a corrupt record, or vectors that
         * ended up parallel. Fall back to identity rather than publishing noise: wrong
         * but coherent beats a NaN going out on the bus as the car's motion. */
        float u[3], f[3];
        imu_identity(u, f);
        (void)imu_basis(u, f, basis);
    }
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

    for (uint8_t i = 0; i < 3; i++) { acc_r[i] = a[i]; gyr_r[i] = g[i]; }
    for (uint8_t i = 0; i < 3; i++) {
        acc_v[i] = basis[i][0] * a[0] + basis[i][1] * a[1] + basis[i][2] * a[2];
        gyr_v[i] = basis[i][0] * g[0] + basis[i][1] * g[1] + basis[i][2] * g[2];
    }
}

float imu_accel(uint8_t axis)     { return axis < 3 ? acc_v[axis] : 0.0f; }
float imu_gyro(uint8_t axis)      { return axis < 3 ? gyr_v[axis] : 0.0f; }
float imu_accel_raw(uint8_t axis)  { return axis < 3 ? acc_r[axis] : 0.0f; }
float imu_gyro_raw(uint8_t axis)   { return axis < 3 ? gyr_r[axis] : 0.0f; }

uint8_t imu_level_result(void)   { return level_res; }
uint8_t imu_level_tilt_deg(void) { return level_tilt; }
uint32_t imu_level_when(void)    { return level_at; }

/* Shared by both calibration steps: take a fresh sample, insist the board is actually
 * still, and hand back the gravity direction. Whether that direction means "up" or
 * "forward" is the caller's business -- it is the same measurement either way, which is
 * why one switch can level a board and another can aim it. */
static uint8_t measure(float out[3])
{
    if (!ready) return RCM_IMU_LEVEL_NO_IMU;
    imu_tick();                     /* describe the board NOW, not a second ago */
    return imu_solve_gravity(acc_r, gyr_r, out);
}

static void stamp(uint8_t r, float off_deg)
{
    level_at   = millis() ? millis() : 1;
    level_res  = r;
    level_tilt = (uint8_t)(off_deg < 0.0f ? 0 : (off_deg > 90.0f ? 90 : (uint8_t)(off_deg + 0.5f)));
}

/* UP, from gravity, keeping whatever forward direction is already stored. Preserving
 * forward is the whole point: gravity cannot see yaw, so re-levelling a board must not
 * be allowed to guess at a direction somebody already established. */
uint8_t imu_autolevel(void)
{
    float up[3];
    const uint8_t r = measure(up);
    if (r != RCM_IMU_LEVEL_OK) { stamp(r, 90.0f); return r; }

    /* If the new up is parallel to the stored forward, that forward is meaningless for
     * this mounting -- the board has been turned onto a different face. Fall back to a
     * right-handed guess rather than refuse, because UP is the half actually measured
     * and is worth keeping; the installer then aims it with the forward switch. */
    float fwd[3] = { cfg.imu_fwd[0], cfg.imu_fwd[1], cfg.imu_fwd[2] };
    if (!imu_separated_enough(up, fwd)) {
        static const float cand[3][3] = { {1,0,0}, {0,1,0}, {0,0,1} };
        for (uint8_t i = 0; i < 3; i++)
            if (imu_separated_enough(up, cand[i])) {
                fwd[0] = cand[i][0]; fwd[1] = cand[i][1]; fwd[2] = cand[i][2];
                break;
            }
    }

    float R[3][3];
    if (!imu_basis(up, fwd, R)) { stamp(RCM_IMU_LEVEL_TILTED, 90.0f); return RCM_IMU_LEVEL_TILTED; }

    memcpy(cfg.imu_up,  up,  sizeof up);
    memcpy(cfg.imu_fwd, fwd, sizeof fwd);
    rebuild_basis();
    imu_tick();                     /* so the next reader sees the new axes, not a
                                     * stale sample in the old ones */

    uint8_t m[3]; float off = 0.0f;
    imu_nearest_map(basis, m, &off);
    stamp(RCM_IMU_LEVEL_OK, off);   /* reported as "how far off square", not a refusal --
                                     * a leaning mount is now corrected, not rejected */
    return RCM_IMU_LEVEL_OK;
}

/* FORWARD. Hold the board so the edge you want at the REAR of the car points at the
 * ground: the axis reading +1 g is then the one that will face forward. Keeps up. */
uint8_t imu_set_forward(void)
{
    float fwd[3];
    const uint8_t r = measure(fwd);
    if (r != RCM_IMU_LEVEL_OK) { stamp(r, 90.0f); return r; }

    /* Held flat instead of on edge: the answer would be the board normal, which is not a
     * direction a car travels in. Refuse rather than store it. */
    if (!imu_separated_enough(cfg.imu_up, fwd)) {
        stamp(RCM_IMU_LEVEL_TILTED, imu_angle_deg(cfg.imu_up, fwd));
        return RCM_IMU_LEVEL_TILTED;
    }

    float R[3][3];
    if (!imu_basis(cfg.imu_up, fwd, R)) { stamp(RCM_IMU_LEVEL_TILTED, 90.0f); return RCM_IMU_LEVEL_TILTED; }

    memcpy(cfg.imu_fwd, fwd, sizeof fwd);
    rebuild_basis();
    imu_tick();

    uint8_t m[3]; float off = 0.0f;
    imu_nearest_map(basis, m, &off);
    stamp(RCM_IMU_LEVEL_OK, off);
    return RCM_IMU_LEVEL_OK;
}

/* The live rotation, for whoever has to report it. */
void imu_current_basis(float R[3][3]) { memcpy(R, basis, sizeof basis); }

/* Adopt vectors that someone else wrote straight into cfg. */
void imu_reload_basis(void) { rebuild_basis(); }

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
