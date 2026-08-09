/*
 * RCM firmware -- milestone 1: latch, config read, heartbeat.
 *
 * Deliberately does nothing with CAN or the channels yet. What this proves, in order:
 * the board powers up, the clock is right, GPIO works, the config DIP reads, and above
 * all that the board KEEPS ITSELF ALIVE. Nothing else is debuggable until that is true.
 */
#include <Arduino.h>
#include "board.h"

struct Config {
    bool     keypad;      /* role: keypad vs relay module */
    uint8_t  address;     /* 0..3 from ADDR0/ADDR1        */
    bool     force_500k;  /* CFG_BAUD closed -> recovery  */
    bool     publish_imu;
};

static Config cfg;

/* DIP positions are active LOW: internal pull-up, switch shorts to ground. */
static inline bool dip_closed(uint32_t pin) { return digitalRead(pin) == LOW; }

static void read_config(void)
{
    const uint32_t pins[] = { PIN_CFG_ROLE, PIN_CFG_ADDR0, PIN_CFG_ADDR1,
                              PIN_CFG_BAUD, PIN_CFG_IMU_EN };
    for (uint32_t p : pins) pinMode(p, INPUT_PULLUP);
    delayMicroseconds(50);            /* let the pull-ups settle before sampling */

    cfg.keypad      = dip_closed(PIN_CFG_ROLE);
    cfg.address     = (dip_closed(PIN_CFG_ADDR0) ? 1 : 0)
                    | (dip_closed(PIN_CFG_ADDR1) ? 2 : 0);
    cfg.force_500k  = dip_closed(PIN_CFG_BAUD);
    cfg.publish_imu = dip_closed(PIN_CFG_IMU_EN);
}

void setup(void)
{
    /* 1. HOLD OUR OWN POWER ON. First statement for a reason: until this is high the
     *    board is alive only while the ignition signal is, and it will cut out mid-boot
     *    the moment you switch off. Everything else can wait. */
    pinMode(PIN_LATCH_HOLD, OUTPUT);
    digitalWrite(PIN_LATCH_HOLD, HIGH);

    /* 2. Keep the channel outputs disabled. R_OE (10k pull-up) already holds the 595s
     *    in high-impedance from power-on -- this just makes sure we do not release them
     *    by accident. They stay off until milestone 2 has shifted a known state in. */
    pinMode(PIN_SR_OE_N, OUTPUT);
    digitalWrite(PIN_SR_OE_N, HIGH);

    pinMode(PIN_LED1, OUTPUT);
    pinMode(PIN_LED2, OUTPUT);

    read_config();

    /* Blink the address back on the red LED at boot, so you can confirm the DIP is
     * being read without attaching anything: N flashes = node address N. */
    for (uint8_t i = 0; i <= cfg.address; i++) {
        digitalWrite(PIN_LED2, HIGH); delay(120);
        digitalWrite(PIN_LED2, LOW);  delay(180);
    }
}

void loop(void)
{
    /* Heartbeat. Slow = relay module, fast = keypad, so the role is visible across a
     * workshop without a debugger attached. */
    const uint16_t period = cfg.keypad ? 200 : 800;
    digitalWrite(PIN_LED1, HIGH); delay(period / 2);
    digitalWrite(PIN_LED1, LOW);  delay(period / 2);
}
