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
static uint32_t shutdown_at;    /* when the stop was requested */
static uint32_t last_activity;  /* for the idle timeout */

static inline bool ch_configured(uint8_t ch) { return ch < RCM_CHANNELS; }

static bool read_ch(uint8_t ch)
{
    return ch_configured(ch) && ((ch_inputs() >> ch) & 1u);
}

static void set_starter(bool on)
{
    if (ch_configured(cfg.ign_start_ch)) ch_command(cfg.ign_start_ch, on);
}

/* The key's RUN position. Held for as long as the board is awake and dropped the
 * instant a stop is requested, so whatever feeds the ECU's ignition input sees a
 * clean ignition-off and rusEFI can park itself the way it would after a key. */
static void set_run_out(bool on)
{
    if (ch_configured(cfg.ign_run_out_ch)) ch_command(cfg.ign_run_out_ch, on);
}

static void request_shutdown(uint32_t now)
{
    if (want_shutdown) return;
    want_shutdown = true;
    shutdown_at = now ? now : 1;
    set_starter(false);
    set_run_out(false);         /* first thing to go, before anything else */
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
    press_ms = low_since = crank_ms = shutdown_at = last_activity = 0;
    sw_prev = sw_closed_at_boot;
    /* In momentary mode the press that woke us through the hardware latch is still
     * happening. Do not let it count as a command -- otherwise a wake with the brake
     * held would go straight to cranking. Wait for a release first. */
    armed = (cfg.ign_mode != IGN_MOMENTARY) || !sw_closed_at_boot;

    /* Assert RUN here rather than waiting for the first ign_tick, so it is part of the
     * same shift-register frame that brings the outputs live. Otherwise a watchdog
     * reset leaves the ECU looking at ignition-off for an extra tick on top of the
     * reset itself -- a blip it has no reason to see. */
    set_run_out(true);
}

/* --- maintained: J_IGN is a level ------------------------------------------- */

static void tick_maintained(uint32_t now, bool sw)
{
    if (sw) { low_since = 0; return; }
    if (low_since == 0) { low_since = now ? now : 1; return; }
    if ((now - low_since) >= cfg.ign_off_hold_ms) request_shutdown(now);
}

/* --- momentary: J_IGN is a button ------------------------------------------- */

static void tick_momentary(uint32_t now, bool sw)
{
    /* With no run channel configured there is no running signal, so "running" is simply
     * never true and cranking falls back to following the button. */
    const bool running = read_ch(cfg.ign_run_ch);
    const bool rising  = sw && !sw_prev;
    const bool falling = !sw && sw_prev;

    if (rising || falling) last_activity = now;
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
        request_shutdown(now);
        return;
    }

    /* Adopt the run signal wherever we see it, not only on the way out of CRANKING.
     *
     * Without this a watchdog reset with the engine running comes back in IGNITION and
     * STAYS there, because nothing else moves it. The state would then say "not
     * running" while the engine turned -- and the no-crank-while-running guard below
     * would be looking at a lie. */
    if (running) last_activity = now;
    if (running && state == IGN_ST_IGNITION) state = IGN_ST_RUNNING;

    switch (state) {
    case IGN_ST_IGNITION:
        if (rising) {
            /* Gated on the run SIGNAL, not on the state. State can be stale after a
             * reset; the signal is what is true right now. Engaging a starter against
             * a turning engine wrecks the pinion and the ring gear. */
            if (read_ch(cfg.ign_brake_ch) && crank_allowed() && !running) {
                state = IGN_ST_CRANKING;
                crank_ms = now;
                set_starter(true);
            } else if (running) {
                /* Engine running but the state machine had lost track. Nothing to do
                 * except stop pretending -- a hold still stops the car. */
            } else {
                /* Engine off, brake not held: this press means "turn the car off".
                 * Also the outcome when cranking is not configured, which is the right
                 * way for an unconfigured board to fail. */
                request_shutdown(now);
            }
        }
        break;

    case IGN_ST_CRANKING:
        /* NOTE what is NOT here: releasing the brake does not stop cranking.
         *
         * The brake gates the START, not the continuation, for two reasons. A key does
         * the same -- nothing makes you hold the brake through a crank. And more
         * importantly the brake input CANNOT BE READ while cranking: it is a digital
         * channel needing >10.87V at the terminal, and a starter drags the battery to
         * 9-10V. Aborting on "brake released" would therefore abort every single start
         * the instant the starter loaded the battery, and the car would never fire.
         * (The button itself survives, because IGN_SENSE is an ADC with a 6V threshold
         * rather than a logic input.) */
        if (running) {                       /* caught -- let go of the starter */
            set_starter(false);
            state = IGN_ST_RUNNING;
        } else if ((now - crank_ms) >= cfg.ign_crank_max_ms) {
            set_starter(false);              /* give up rather than cook the starter */
            state = IGN_ST_IGNITION;
        } else if (falling && !ch_configured(cfg.ign_run_ch)) {
            /* With no running signal there is nothing to tell us when it caught, so the
             * button behaves like a key's spring-return START position: crank while
             * held, release to stop. */
            set_starter(false);
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

    /* Idle timeout -- and note the FIRST condition, which is the important one.
     *
     * Without a run channel there is no way to tell "sitting in the driveway with the
     * ignition on" from "half an hour into a drive". `running` would simply be false
     * forever, the state would never reach IGN_ST_RUNNING, and this would switch the
     * car off mid-drive. So the timeout is inert unless the board can actually see
     * whether the engine is turning. A flat battery is a far better failure than an
     * engine cut at speed. */
    if (cfg.ign_idle_timeout_s && ch_configured(cfg.ign_run_ch)
        && state != IGN_ST_RUNNING
        && (now - last_activity) >= (uint32_t)cfg.ign_idle_timeout_s * 1000UL) {
        request_shutdown(now);
    }
}

void ign_note_activity(uint32_t now) { last_activity = now; }

/* --- entry point ------------------------------------------------------------ */

void ign_tick(uint32_t now, bool sw)
{
    if (want_shutdown) {
        /* The decision is latched and main() is powering us down -- but keep tracking
         * the button, because ign_may_cut_power() needs to see it released before the
         * latch can actually be dropped. Returning without this leaves sw_prev stuck
         * at "held" forever and the board never switches off. */
        sw_prev = sw;
        return;
    }

    if (cfg.ign_mode == IGN_MOMENTARY) tick_momentary(now, sw);
    else                               tick_maintained(now, sw);

    /* Reassert RUN every tick rather than setting it once. ch_apply_failsafe() runs at
     * boot and on every bus timeout, and this is the cheapest way to make sure neither
     * can leave the ECU's ignition feed in whatever state failsafe_state happened to
     * ask for. */
    if (!want_shutdown) set_run_out(true);

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
uint32_t ign_shutdown_since(void)  { return shutdown_at; }

bool ign_may_cut_power(uint32_t now)
{
    if (!want_shutdown) return false;
    if ((now - shutdown_at) < cfg.ign_shutdown_ms) return false;
    return !sw_prev;                 /* see the note in ignition.h */
}
