/*
 * ignition.cpp -- the ignition input, as a level or as a button.
 *
 * Read ignition.h first; the safety asymmetry described there is what this file is
 * shaped around. Starting is hedged about with conditions. Stopping is not.
 */
#include "ignition.h"
#include "channels.h"
#include "config.h"

static enum ign_state_t state;
static bool     want_shutdown;
static bool     sw_prev;
static bool     armed;          /* the wake press has been released */
static uint32_t press_ms;       /* when the current press began */
static bool     hold_fired;     /* the hold action already happened this press */
static uint32_t low_since;      /* maintained mode: when the level went away */
static uint32_t crank_ms;       /* when cranking began */

static inline bool ch_configured(uint8_t ch) { return ch < RCM_CHANNELS; }

static bool read_ch(uint8_t ch)
{
    return ch_configured(ch) && ((ch_inputs() >> ch) & 1u);
}

static void set_starter(bool on)
{
    if (ch_configured(cfg.ign_start_ch)) ch_command(cfg.ign_start_ch, on);
}

/* Cranking needs BOTH channels configured. Requiring the brake input as well as the
 * starter output means the dangerous capability takes two deliberate settings, and a
 * half-configured board simply will not turn a starter. */
static bool crank_allowed(void)
{
    return ch_configured(cfg.ign_start_ch) && ch_configured(cfg.ign_brake_ch);
}

void ign_begin(bool sw_closed_at_boot)
{
    state = IGN_ST_IGNITION;
    want_shutdown = false;
    hold_fired = false;
    press_ms = low_since = crank_ms = 0;
    sw_prev = sw_closed_at_boot;
    /* In momentary mode the press that woke us through the hardware latch is still
     * happening. Do not let it count as a command -- otherwise a wake with the brake
     * held would go straight to cranking. Wait for a release first. */
    armed = (cfg.ign_mode != IGN_MOMENTARY) || !sw_closed_at_boot;
}

/* --- maintained: J_IGN is a level ------------------------------------------- */

static void tick_maintained(uint32_t now, bool sw)
{
    if (sw) { low_since = 0; return; }
    if (low_since == 0) { low_since = now ? now : 1; return; }
    if ((now - low_since) >= cfg.ign_off_hold_ms) want_shutdown = true;
}

/* --- momentary: J_IGN is a button ------------------------------------------- */

static void tick_momentary(uint32_t now, bool sw)
{
    /* With no run channel configured there is no running signal, so "running" is simply
     * never true and cranking falls back to following the button. */
    const bool running = read_ch(cfg.ign_run_ch);
    const bool rising  = sw && !sw_prev;
    const bool falling = !sw && sw_prev;

    if (rising) { press_ms = now; hold_fired = false; }
    if (!armed) {
        /* Still waiting for the wake press to end. */
        if (falling) armed = true;
        return;
    }

    /* A hold is the stop gesture, and it works from ANY state. Checked before anything
     * else and gated on nothing -- no RPM, no running signal, no sensor that could be
     * broken at the moment you most need the engine to stop. */
    if (sw && !hold_fired && (now - press_ms) >= cfg.ign_hold_stop_ms) {
        hold_fired = true;
        set_starter(false);
        want_shutdown = true;
        return;
    }

    switch (state) {
    case IGN_ST_IGNITION:
        if (rising) {
            if (read_ch(cfg.ign_brake_ch) && crank_allowed()) {
                state = IGN_ST_CRANKING;
                crank_ms = now;
                set_starter(true);
            } else {
                /* Engine off, brake not held: this press means "turn the car off".
                 * Also the outcome when cranking is not configured, which is the right
                 * way for an unconfigured board to fail. */
                want_shutdown = true;
            }
        }
        break;

    case IGN_ST_CRANKING:
        if (running) {                       /* caught -- let go of the starter */
            set_starter(false);
            state = IGN_ST_RUNNING;
        } else if ((now - crank_ms) >= cfg.ign_crank_max_ms) {
            set_starter(false);              /* give up rather than cook the starter */
            state = IGN_ST_IGNITION;
        } else if (falling && !ch_configured(cfg.ign_run_ch)) {
            /* With no running signal there is nothing to tell us when to stop, so the
             * button behaves like a key's spring-return START position: crank while
             * held, release to stop. */
            set_starter(false);
            state = IGN_ST_IGNITION;
        } else if (!read_ch(cfg.ign_brake_ch)) {
            set_starter(false);              /* brake released mid-crank */
            state = IGN_ST_IGNITION;
        }
        break;

    case IGN_ST_RUNNING:
        /* A short press does nothing on purpose. Brushing the button at speed must not
         * cut the engine; stopping takes a deliberate hold, handled above. */
        if (ch_configured(cfg.ign_run_ch) && !running) state = IGN_ST_IGNITION;
        break;

    case IGN_ST_SHUTDOWN:
    default:
        break;
    }
}

/* --- entry point ------------------------------------------------------------ */

void ign_tick(uint32_t now, bool sw)
{
    if (want_shutdown) return;               /* latched; main() is powering us down */

    if (cfg.ign_mode == IGN_MOMENTARY) tick_momentary(now, sw);
    else                               tick_maintained(now, sw);

    /* Track the engine state in maintained mode too, so the CAN status is meaningful
     * whichever way the ignition is wired. */
    if (cfg.ign_mode != IGN_MOMENTARY && ch_configured(cfg.ign_run_ch))
        state = read_ch(cfg.ign_run_ch) ? IGN_ST_RUNNING : IGN_ST_IGNITION;

    sw_prev = sw;
}

enum ign_state_t ign_state(void)   { return want_shutdown ? IGN_ST_SHUTDOWN : state; }
bool ign_wants_shutdown(void)      { return want_shutdown; }
bool ign_cranking(void)            { return state == IGN_ST_CRANKING; }
bool ign_engine_running(void)      { return state == IGN_ST_RUNNING; }
