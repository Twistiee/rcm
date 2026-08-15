/*
 * ignition.h -- what the ignition input means, and what to do about it.
 *
 * Two modes, selected by cfg.ign_mode:
 *
 *   IGN_MAINTAINED  the original behaviour and the default. J_IGN is a LEVEL. Closed
 *                   means ignition on; open for ign_off_hold_ms means shut down. A key,
 *                   or a maintained panel switch, behaves exactly like this.
 *
 *   IGN_MOMENTARY   J_IGN is a BUTTON, and this becomes a state machine roughly like a
 *                   VW start button:
 *                     press, engine off, brake NOT held  ->  shut the car down
 *                     press, engine off, brake held      ->  crank
 *                     press and HOLD while running       ->  shut the car down
 *                   The very first press is the one that woke the board through the
 *                   hardware latch, and is deliberately consumed.
 *
 * ===========================================================================
 * THE SAFETY ASYMMETRY, WHICH IS THE WHOLE DESIGN
 * ===========================================================================
 * **Starting is conditional. Stopping never is.**
 *
 * Cranking requires a configured brake input, requires it to be pressed, requires the
 * engine not to be running already, and gives up after a timeout. If any of that is
 * missing or unclear, the starter does not turn.
 *
 * Stopping asks no questions. It is not gated on RPM, on a running signal, or on any
 * sensor at all -- because every one of those is a thing that can fail in the exact
 * situation where you need the engine to stop. A hold on the button always shuts the
 * board down.
 *
 * A short press while running does nothing, deliberately: brushing the button at speed
 * must not cut the engine. Stopping needs a deliberate hold.
 */
#ifndef RCM_IGNITION_H
#define RCM_IGNITION_H

#include <stdint.h>
#include <stdbool.h>

enum ign_mode_t {
    IGN_MAINTAINED = 0,
    IGN_MOMENTARY  = 1,
};

enum ign_state_t {
    IGN_ST_IGNITION = 0,   /* awake, engine not running   */
    IGN_ST_CRANKING = 1,   /* starter channel energised   */
    IGN_ST_RUNNING  = 2,   /* engine confirmed running    */
    IGN_ST_SHUTDOWN = 3,   /* main() should power us down */
};

#define IGN_CH_NONE 0xFFu

void ign_begin(bool sw_closed_at_boot);

/* Call every tick with the debounced state of the ignition input.
 * `sw_closed` is true when J_IGN is at +12V. */
void ign_tick(uint32_t now_ms, bool sw_closed);

enum ign_state_t ign_state(void);
bool ign_wants_shutdown(void);
bool ign_cranking(void);
bool ign_engine_running(void);

#endif /* RCM_IGNITION_H */
