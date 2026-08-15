/*
 * RCM firmware -- main.
 *
 * ===========================================================================
 * BOOT ORDER IS A SAFETY PROPERTY, NOT A STYLE CHOICE
 * ===========================================================================
 * Two things must happen before anything else, in this order:
 *
 *   1. LATCH_HOLD high. Until it is, the board is alive only for as long as the
 *      ignition signal is, and it will cut out mid-boot the moment you switch off.
 *      Nothing is debuggable before this works.
 *
 *   2. The shift registers get a zeroed frame shifted out and LATCHED, with
 *      SR_OE_N still high. Only then may the outputs be enabled. R_OE holds the
 *      595s in high-impedance from the instant power arrives; that is what stops
 *      21 relays clacking on during boot, and it is trivially easy to defeat by
 *      enabling the outputs one line too early.
 *
 * Everything after that can fail without taking the board down. A missing EEPROM
 * gives you defaults, a missing IMU gives you no IMU, and a CAN bitrate that cannot
 * be produced gives you a board that blinks an error but still runs its channels.
 *
 * ===========================================================================
 * SCHEDULING
 * ===========================================================================
 * A plain cooperative loop with millis() deadlines. No RTOS: the work is a 5ms I/O
 * tick, a 50ms broadcast and a 20ms IMU read, none of it is long, and a scheduler
 * that can be understood by reading forty lines is worth more here than one that
 * can preempt.
 *
 * The one hard rule is that ch_tick() must not be starved -- it is what feeds the
 * watchdog, and it is what turns relays on and off.
 */
#include <Arduino.h>
#include <IWatchdog.h>
#include "app.h"
#include "board.h"
#include "canbus.h"
#include "channels.h"
#include "config.h"
#include "ignition.h"
#include "imu.h"
#include "protocol.h"
#include "shiftreg.h"
#include "store.h"

#define TICK_MS      5      /* must match channels.cpp */
#define WDG_US  200000      /* 200ms; 40 missed ticks */

/* Ignition sense: J_IGN -> 1M/270k -> PA0. The divider ratio is 270/1270, so the
 * pin sees 21.26% of the ignition feed and the ADC saturates at about 15.5V. That
 * is a deliberate limitation -- the question being asked is "is the ignition on",
 * not "what exactly is the battery doing" -- but it does mean a reading pinned at
 * 15.5V means "at least 15.5V" and nothing more precise. */
#define IGN_NUM      1270L
#define IGN_DEN       270L
#define IGN_ON_MV    6000   /* below this the ignition input is considered open */

static bool     outputs_live;
static bool     eeprom_ok;
static bool     can_up;
static uint16_t ign_mv;

static uint32_t next_tick, next_bcast, next_imu, next_slow;

/* --- app.h ---------------------------------------------------------------- */

bool app_outputs_live(void) { return outputs_live; }

void app_set_outputs_live(bool live)
{
    /* Going live is only ever allowed to happen with a known state already latched
     * into the 595s -- which sr_begin() guarantees before this can first be called. */
    outputs_live = live;
    sr_outputs_enable(live);
}

bool     app_eeprom_ok(void)   { return eeprom_ok; }
uint16_t app_ignition_mv(void) { return ign_mv; }
bool     app_ignition_on(void) { return ign_mv >= IGN_ON_MV; }

/* --- LEDs ------------------------------------------------------------------
 * LED1 (green) is the heartbeat and carries the coarse health of the board.
 * LED2 (red) is faults. Between them you can tell what a board is doing from
 * across a workshop with nothing plugged into it, which turns out to matter far
 * more than it sounds like it should.
 */
static void leds(uint32_t now)
{
    const bool fault = ch_fault_open() || ch_fault_short();

    uint16_t period;
    if (!can_up)                 period = 150;   /* frantic: CAN never came up  */
    else if (proto_failsafe())   period = 300;   /* fast: the bus has gone quiet */
    else if (straps.keypad)      period = 1000;
    else                         period = 2000;  /* slow idle: all well          */

    digitalWrite(PIN_LED1, ((now % period) < (period / 2)) ? HIGH : LOW);
    digitalWrite(PIN_LED2, fault ? (((now % 400) < 200) ? HIGH : LOW) : LOW);
}

/* Below this the channel sense divider cannot tell a healthy coil circuit from an open
 * one -- see ch_inhibit_diag(). 11.5V leaves a little margin over the ~11.0V where the
 * 74HC165 stops seeing a HIGH. The ignition feed is the only supply the board can
 * measure, but it comes off the same battery as the coils, so it tracks. */
#define DIAG_MIN_MV 11500

static void read_ignition(void)
{
    const uint32_t raw = analogRead(PIN_IGN_SENSE);           /* 12-bit */
    const uint32_t mv  = (raw * 3300UL) / 4095UL;
    ign_mv = (uint16_t)((mv * IGN_NUM) / IGN_DEN);

    /* Cranking drags the battery down far enough to make every un-driven output look
     * like a blown fuse. Stop diagnosing rather than report a boardful of faults. */
    ch_inhibit_diag(ign_mv < DIAG_MIN_MV);
}

/* --- shutdown --------------------------------------------------------------
 * The whole reason the board holds its own power on: when the ignition goes away
 * we get to put the outputs down deliberately and save anything that needs saving
 * before dropping LATCH_HOLD and switching ourselves off.
 */
static void shutdown_check(uint32_t now)
{
    /* WHEN to shut down is ignition.cpp's decision now -- a level going away, or a
     * button gesture. This function only knows HOW. */
    if (!ign_wants_shutdown()) return;

    /* The RUN output has already gone; ignition.cpp drops it the moment a stop is
     * requested. Hold everything ELSE up for a moment so the ECU can see ignition-off
     * and finish its own shutdown -- park the throttle, run the fan down, write back
     * whatever it has learned. Yanking the rail instantly would be the equivalent of
     * pulling the battery lead, every single time you switch the car off. */
    if ((now - ign_shutdown_since()) < cfg.ign_shutdown_ms) return;

    ch_all_off();
    sr_exchange();                     /* make sure the zeros actually reach the pins */
    sr_outputs_enable(false);
    digitalWrite(PIN_LED1, LOW);
    digitalWrite(PIN_LED2, LOW);

    digitalWrite(PIN_LATCH_HOLD, LOW); /* goodnight */

    /* Wait for the rail to collapse. The watchdog has to be fed while we do -- it is
     * a 200ms window and this wait is 250ms, so a plain delay() would reset the board
     * instead of letting it die. Feeding it here is not defeating the watchdog: if
     * the power really is going away we never finish this loop anyway. */
    const uint32_t t0 = millis();
    while (millis() - t0 < 250) IWatchdog.reload();

    /* Still here: the latch had nothing to cut, which is the normal case on a bench
     * supply. Come back up rather than spinning, and re-arm the ignition logic so the
     * board stays usable. */
    digitalWrite(PIN_LATCH_HOLD, HIGH);
    ign_begin(app_ignition_on());
}

/* --- setup ----------------------------------------------------------------- */

void setup(void)
{
    /* 1. HOLD OUR OWN POWER ON. First statement, for the reason in the header. */
    pinMode(PIN_LATCH_HOLD, OUTPUT);
    digitalWrite(PIN_LATCH_HOLD, HIGH);

    /* 2. Shift registers: zeroed, latched, still high-impedance. */
    sr_begin();

    pinMode(PIN_LED1, OUTPUT);
    pinMode(PIN_LED2, OUTPUT);
    pinMode(PIN_IGN_SENSE, INPUT_ANALOG);
    analogReadResolution(12);

    cfg_read_straps();

    store_begin();
    eeprom_ok = store_present();
    cfg_load();                  /* falls back to defaults if the EEPROM is absent */
    proto_sanitise_base();

    ch_begin();

    const bool wdg_reset = IWatchdog.isReset(true);

    /* 3. Adopt the configured resting state, THEN go live.
     *
     * `failsafe_state` means "what should be on when nobody has told us otherwise", and
     * that is as true at power-up as it is after the bus goes quiet. It used to be
     * applied only on a CAN timeout, which meant a board came up with every channel off
     * and did not reach its configured state until can_timeout_ms had elapsed -- so a
     * main relay behind this board closed a second and a half after the key rather than
     * at boot.
     *
     * The boot-order invariant is unchanged and still the important thing: a KNOWN state
     * has to be latched into the 595s before SR_OE_N is allowed to drop. sr_begin()
     * latched zeros; this latches the configured state; only then do the outputs go
     * live. What has gone is the idea that they must go live LAST.
     *
     * Both reset paths do this identically, so the outputs are back within a few
     * milliseconds either way -- on a watchdog reset that is the difference between a
     * glitch and a stall, and on a cold start it is the difference between the engine
     * being ready to crank and waiting on a timeout. */
    read_ignition();
    ign_begin(app_ignition_on());

    ch_apply_failsafe();
    ch_tick(millis());
    app_set_outputs_live(true);

    can_up = can_begin(cfg_effective_bitrate());
    if (can_up) proto_begin();

    if (!wdg_reset) {
        /* Cold boot only: blink the node address on the red LED. N+1 flashes = node N,
         * which confirms the DIP is being read with nothing attached. Worth two seconds
         * once; not worth it on every watchdog reset, and no longer in the way of the
         * outputs coming up. */
        for (uint8_t i = 0; i <= straps.node; i++) {
            digitalWrite(PIN_LED2, HIGH); delay(120);
            digitalWrite(PIN_LED2, LOW);  delay(180);
        }
    }

    /* Last, because the BMI270 config upload is slow and nothing depends on it. */
    if (straps.publish_imu) imu_begin();

    IWatchdog.begin(WDG_US);

    const uint32_t now = millis();
    next_tick = next_bcast = next_imu = next_slow = now;
}

/* --- loop ------------------------------------------------------------------ */

void loop(void)
{
    const uint32_t now = millis();

    /* CAN is polled every time round, not on a deadline. The receive FIFO is three
     * frames deep and commands should not wait for a tick boundary; the transmit
     * queue likewise only moves when something pumps it. */
    if (can_up) {
        can_tx_pump();
        proto_poll(now);
    }

    if ((int32_t)(now - next_tick) >= 0) {
        next_tick += TICK_MS;
        /* If we ever fall far enough behind that the deadline has already passed
         * again, resynchronise instead of trying to catch up with a burst of ticks
         * -- the debounce counters assume a steady interval. */
        if ((int32_t)(now - next_tick) >= 0) next_tick = now + TICK_MS;

        ch_tick(now);
        read_ignition();
        ign_tick(now, app_ignition_on());
        IWatchdog.reload();
    }

    if (can_up && (int32_t)(now - next_bcast) >= 0) {
        next_bcast = now + cfg.broadcast_ms;
        proto_broadcast(now);
    }

    if (can_up && straps.publish_imu && imu_ok() && (int32_t)(now - next_imu) >= 0) {
        next_imu = now + cfg.imu_rate_ms;
        imu_tick();
        imu_broadcast();
    }

    if ((int32_t)(now - next_slow) >= 0) {
        next_slow = now + 100;
        shutdown_check(now);
    }

    leds(now);
}
