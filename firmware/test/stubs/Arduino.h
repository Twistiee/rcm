/*
 * Arduino.h -- host stub, for the native unit tests only.
 *
 * Just enough of the Arduino API for the firmware's own .cpp files to compile and run
 * on a PC. Every pin operation is routed out to sim_* hooks that the test provides, so
 * a test can watch what the firmware does to the hardware and answer back as the
 * hardware would.
 *
 * This file is NOT on the include path for a real build -- see platformio.ini.
 */
#ifndef RCM_TEST_ARDUINO_H
#define RCM_TEST_ARDUINO_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

/* Pin names. Values are arbitrary but must be distinct; the tests refer to them by
 * the same PIN_* macros the firmware does. */
enum {
    PA0 = 0,  PA1,  PA2,  PA3,  PA4,  PA5,  PA6,  PA7,  PA8,  PA9,
    PA10, PA11, PA12, PA13, PA14, PA15,
    PB0 = 20, PB1,  PB2,  PB3,  PB4,  PB5,  PB6,  PB7,  PB8,  PB9,
    PB10, PB11, PB12, PB13, PB14, PB15,
    PC0 = 40, PC1,  PC2,  PC3,  PC4,  PC5,
};

#define LOW           0
#define HIGH          1
#define INPUT         0
#define OUTPUT        1
#define INPUT_PULLUP  2
#define INPUT_ANALOG  3
#define MSBFIRST      1
#define LSBFIRST      0

/* Provided by the test. */
extern "C" {
uint32_t sim_millis(void);
void     sim_advance(uint32_t ms);
void     sim_pin_mode(uint32_t pin, uint32_t mode);
void     sim_digital_write(uint32_t pin, uint32_t val);
int      sim_digital_read(uint32_t pin);
int      sim_analog_read(uint32_t pin);
uint8_t  sim_spi_transfer(uint8_t out);
}

static inline uint32_t millis(void)                  { return sim_millis(); }
/* delay() in a test advances simulated time rather than actually sleeping -- a suite
 * that really slept through the firmware's delays would take minutes. */
static inline void delay(uint32_t ms)                { sim_advance(ms); }
static inline void delayMicroseconds(uint32_t us)    { (void)us; }
static inline void pinMode(uint32_t p, uint32_t m)   { sim_pin_mode(p, m); }
static inline void digitalWrite(uint32_t p, uint32_t v) { sim_digital_write(p, v); }
static inline int  digitalRead(uint32_t p)           { return sim_digital_read(p); }
static inline int  analogRead(uint32_t p)            { return sim_analog_read(p); }
static inline void analogReadResolution(int b)       { (void)b; }

#endif /* RCM_TEST_ARDUINO_H */
