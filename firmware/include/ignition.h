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

/* True once a stop has been requested. main() drops the RUN output immediately and then
 * holds the board up for cfg.ign_shutdown_ms before cutting its own power, so the ECU
 * gets the same graceful ignition-off it would get from a key. */
uint32_t ign_shutdown_since(void);

/* True only when the board can ACTUALLY power itself down: a stop has been requested,
 * the ECU has had its shutdown window, and the ignition input has gone low.
 *
 * That last condition is not politeness, it is electrical. LATCH_HOLD reaches the
 * BTS7040 through R_LHOLD (1k) while the switch reaches it through R_LIGN (47k). With
 * the switch CLOSED, driving LATCH_HOLD low pulls LATCH_IN to only ~0.24V, so the latch
 * does turn off -- and then the MCU dies, its pin goes high-impedance, and R_LIGN/R_LPD
 * put LATCH_IN straight back to 3.83V and switch the board on again. A power cycle, not
 * a power off. Waiting for the release is the only thing that works. */
bool ign_may_cut_power(uint32_t now_ms);

/* Something happened that means somebody is still using the board -- a CAN frame
 * addressed to us, typically. Restarts the idle timeout. */
void ign_note_activity(uint32_t now_ms);

/* Feed engine speed in from the bus. rusEFI broadcasts it at base+1 (0x201 stock), and
 * ign_run_rpm decides what counts as running.
 *
 * WEAKER THAN A WIRED SIGNAL, and only for the crank interlock does that matter. If CAN
 * stops while the engine is turning, this goes stale and the board would believe the
 * engine had stopped -- so a press with the brake down could crank against a spinning
 * ring gear. A wired alternator-D+ or oil-pressure input keeps working when the bus does
 * not. Use CAN RPM for convenience; wire a run signal if this board turns the starter. */
void ign_note_rpm(uint16_t rpm, uint32_t now_ms);
bool ign_has_run_source(void);

#endif /* RCM_IGNITION_H */
