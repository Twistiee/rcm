/*
 * channels.cpp -- debouncing, and reading meaning into the sense bits.
 *
 * ===========================================================================
 * WHAT A SENSE BIT MEANS  (DESIGN.md has the measured divider voltages)
 * ===========================================================================
 * Every channel has the same three resistors fitted: 1M/270k sense divider and a
 * 10k pull-down. No hardware differs between an input channel and an output one.
 *
 *   OUTPUT, driver OFF, coil circuit intact   2.530 V   HIGH
 *   OUTPUT, driver OFF, fuse blown / open     0.000 V   LOW
 *   OUTPUT, driver ON                         ~0 V      LOW   (we are pulling it down)
 *   INPUT,  button open                       0.000 V   LOW
 *   INPUT,  button closed to +12V             2.551 V   HIGH
 *
 * So the diagnosis for an output only exists while it is OFF. That is not a
 * limitation worth engineering around -- a relay that is commanded on and drawing
 * current will announce a broken circuit the moment it is next switched off.
 *
 * Two timers keep this honest:
 *   output_settle_ms   after ANY commanded change, ignore that channel's sense.
 *                      A relay coil's flyback current takes real time to collapse
 *                      through the TPL7407L's clamp, and the node sits low until it
 *                      has. Diagnose during that window and every switch-off looks
 *                      like a blown fuse.
 *   fault_confirm_ms   the condition then has to hold continuously. A car is a
 *                      terrible electrical environment and one bad sample is noise.
 */
#include <Arduino.h>
#include <string.h>
#include "board.h"
#include "channels.h"
#include "config.h"
#include "shiftreg.h"

#define TICK_MS 5   /* must match the rate main() calls ch_tick() at */

/* `requested` is what somebody ASKED for; `commanded` is what the driver is actually
 * doing. For OUT_STEADY -- the default and almost every channel -- they are identical.
 * They diverge for flash, pulse and delay-off, and keeping them apart is what stops a
 * flashing indicator confusing the toggle logic or the failsafe. */
static uint32_t requested;
static uint32_t commanded;         /* what the drivers are actually doing   */
static uint32_t beh_timer[RCM_CHANNELS];  /* pulse start / delayed-off start */
static uint32_t stable_sense;      /* debounced raw sense, all 21 channels  */
static uint32_t last_raw;
static uint8_t  debounce_ct[RCM_CHANNELS];
static uint32_t change_ms[RCM_CHANNELS];   /* when this channel last switched */

static uint8_t  aux_stable;
static uint8_t  aux_last_raw;
static uint8_t  aux_ct[RCM_AUX_INPUTS];

static uint16_t open_ms[RCM_CHANNELS];
static uint16_t short_ms[RCM_CHANNELS];
static uint32_t fault_open;
static uint32_t fault_short;
static bool     diag_inhibited;

void ch_inhibit_diag(bool inhibit) { diag_inhibited = inhibit; }
bool ch_diag_inhibited(void)       { return diag_inhibited; }

/* Shared phase, so every flashing channel blinks together rather than each running its
 * own timer from whenever it happened to be switched on. Hazards look wrong otherwise. */
static bool flash_on(uint32_t now)
{
    const uint16_t p = cfg.flash_period_ms ? cfg.flash_period_ms : 800;
    return (now % p) < (uint32_t)(p / 2);
}

/* Turn a request into what the driver should actually do this instant. */
static void apply_behaviour(uint8_t ch, uint32_t now)
{
    bool on = (requested >> ch) & 1u;

    if (on || cfg.ch[ch].behaviour == OUT_DELAY_OFF) {
        switch (cfg.ch[ch].behaviour) {
        case OUT_FLASH:
            on = on && flash_on(now);
            break;
        case OUT_PULSE:
            /* One shot per press. Holding the button does not re-trigger; releasing
             * and pressing again does. */
            on = on && (now - beh_timer[ch]) < cfg.ch[ch].param;
            break;
        case OUT_DELAY_OFF:
            if (!on && beh_timer[ch] && (now - beh_timer[ch]) < cfg.ch[ch].param) on = true;
            break;
        default:
            break;
        }
    }

    if (on) commanded |=  (1ul << ch);
    else    commanded &= ~(1ul << ch);
    sr_set(ch, on);
}

void ch_begin(void)
{
    requested = 0;
    commanded = 0;
    memset(beh_timer, 0, sizeof(beh_timer));
    stable_sense = last_raw = 0;
    fault_open = fault_short = 0;
    memset(debounce_ct, 0, sizeof(debounce_ct));
    memset(open_ms, 0, sizeof(open_ms));
    memset(short_ms, 0, sizeof(short_ms));
    memset(aux_ct, 0, sizeof(aux_ct));
    aux_stable = aux_last_raw = 0;
    diag_inhibited = false;

    uint32_t now = millis();
    for (uint8_t i = 0; i < RCM_CHANNELS; i++) change_ms[i] = now;

    sr_set_all(0);
}

void ch_command(uint8_t ch, bool on)
{
    if (ch >= RCM_CHANNELS) return;
    if (cfg.ch[ch].mode != CH_OUTPUT) return;      /* see the header comment */

    if (cfg.ch[ch].flags & CH_F_INVERT) on = !on;

    if (((requested >> ch) & 1u) != (uint32_t)on) {
        const uint32_t now = millis();
        change_ms[ch] = now;
        /* Edge into the behaviour timers: a rising edge starts a pulse, a falling edge
         * starts a delayed-off. */
        beh_timer[ch] = now ? now : 1;
        /* A channel that has just been switched has no opinion about its own health
         * yet, and any fault it was showing belonged to the previous state. */
        open_ms[ch] = short_ms[ch] = 0;
        fault_open  &= ~(1ul << ch);
        fault_short &= ~(1ul << ch);
    }
    if (on) requested |=  (1ul << ch);
    else    requested &= ~(1ul << ch);
    apply_behaviour(ch, millis());
}

void ch_command_mask(uint32_t mask, uint32_t values)
{
    for (uint8_t ch = 0; ch < RCM_CHANNELS; ch++)
        if ((mask >> ch) & 1u) ch_command(ch, (values >> ch) & 1u);
}

void ch_all_off(void)
{
    /* Deliberately bypasses the invert flag and the mode check. "All off" has to
     * mean no channel is being driven, whatever anything is configured as -- it is
     * what gets called on a bus timeout and on the way to shutting down. */
    for (uint8_t ch = 0; ch < RCM_CHANNELS; ch++) {
        if ((requested >> ch) & 1u) change_ms[ch] = millis();
        open_ms[ch] = short_ms[ch] = 0;
        /* Clear the behaviour timers too. Leaving them set would let a delayed-off
         * channel keep lingering through a shutdown. */
        beh_timer[ch] = 0;
    }
    /* BOTH have to go. Clearing only `commanded` leaves the request standing, and the
     * very next tick re-applies it -- so a flashing indicator would carry on blinking
     * straight through a bus timeout and through the board powering itself down. */
    requested = 0;
    commanded = 0;
    fault_open = fault_short = 0;
    sr_set_all(0);
}

void ch_apply_failsafe(void)
{
    for (uint8_t ch = 0; ch < RCM_CHANNELS; ch++) {
        if (cfg.ch[ch].mode != CH_OUTPUT) continue;
        /* The starter is never part of a resting state, whatever failsafe_state says.
         * failsafe_state is applied at power-up and whenever the bus goes quiet, so a
         * stray bit here would mean a board that cranks the engine on boot, or every
         * time CAN hiccups. Only the ignition state machine turns a starter. */
        if (ch == cfg.ign_start_ch) { ch_command(ch, false); continue; }
        /* Likewise the RUN output: the ignition state machine owns it, and a failsafe
         * that switched the ECU's ignition feed would be deciding to stop the engine. */
        if (ch == cfg.ign_run_out_ch) continue;
        bool want = (cfg.failsafe_state >> ch) & 1u;
        /* failsafe_state is the PHYSICAL state wanted, so undo the invert that
         * ch_command would apply -- a failsafe table you have to mentally invert is
         * a failsafe table someone will get wrong at 2am. */
        if (cfg.ch[ch].flags & CH_F_INVERT) want = !want;
        ch_command(ch, want);
    }
}

void ch_tick(uint32_t now_ms)
{
    /* Re-evaluate anything whose output depends on time before publishing. STEADY
     * channels are already correct and are left alone. */
    for (uint8_t ch = 0; ch < RCM_CHANNELS; ch++)
        if (cfg.ch[ch].behaviour != OUT_STEADY) apply_behaviour(ch, now_ms);

    sr_exchange();

    const uint32_t raw = sr_sense_all();
    const uint8_t  need = cfg.input_debounce_ms / TICK_MS ? cfg.input_debounce_ms / TICK_MS : 1;

    for (uint8_t ch = 0; ch < RCM_CHANNELS; ch++) {
        const bool r = (raw >> ch) & 1u;
        if (r == (bool)((last_raw >> ch) & 1u)) {
            if (debounce_ct[ch] < 255) debounce_ct[ch]++;
        } else {
            debounce_ct[ch] = 0;
        }
        if (debounce_ct[ch] >= need) {
            if (r) stable_sense |=  (1ul << ch);
            else   stable_sense &= ~(1ul << ch);
        }
    }
    last_raw = raw;

    const uint8_t araw = sr_aux_all();
    for (uint8_t a = 0; a < RCM_AUX_INPUTS; a++) {
        const bool r = (araw >> a) & 1u;
        if (r == (bool)((aux_last_raw >> a) & 1u)) { if (aux_ct[a] < 255) aux_ct[a]++; }
        else                                       { aux_ct[a] = 0; }
        if (aux_ct[a] >= need) {
            if (r) aux_stable |=  (uint8_t)(1u << a);
            else   aux_stable &= (uint8_t)~(1u << a);
        }
    }
    aux_last_raw = araw;

    /* --- diagnosis, outputs only ------------------------------------------- */
    for (uint8_t ch = 0; ch < RCM_CHANNELS; ch++) {
        if (cfg.ch[ch].mode != CH_OUTPUT || (cfg.ch[ch].flags & CH_F_NO_DIAG)) {
            open_ms[ch] = short_ms[ch] = 0;
            fault_open  &= ~(1ul << ch);
            fault_short &= ~(1ul << ch);
            continue;
        }
        /* Hi-Z outputs tell you nothing about the coil circuit -- with the 595s
         * disabled the TPL7407L inputs are floating low and every channel reads as
         * if it were simply off. Do not manufacture faults out of that.
         *
         * Nor when the supply has sagged: the sense divider needs ~10.9V at the
         * channel node to read HIGH, so below roughly 11V of battery a healthy coil
         * circuit is indistinguishable from an open one. Cranking does exactly that,
         * and without this every un-driven channel would report a fault on every
         * start. */
        if (!sr_outputs_enabled() || diag_inhibited
            || (now_ms - change_ms[ch]) < cfg.output_settle_ms) {
            open_ms[ch] = short_ms[ch] = 0;
            continue;
        }

        const bool on = (commanded >> ch) & 1u;
        const bool hi = (stable_sense >> ch) & 1u;

        /* driver off, node not pulled up by the coil -> the circuit is broken */
        const bool open_now  = !on && !hi;
        /* driver on, node still up -> the low side is not sinking */
        const bool short_now =  on &&  hi;

        if (open_now) { if (open_ms[ch] < 60000) open_ms[ch] += TICK_MS; }
        else            open_ms[ch] = 0;
        if (short_now) { if (short_ms[ch] < 60000) short_ms[ch] += TICK_MS; }
        else             short_ms[ch] = 0;

        if (open_ms[ch]  >= cfg.fault_confirm_ms) fault_open  |=  (1ul << ch);
        else if (!open_now)                       fault_open  &= ~(1ul << ch);
        if (short_ms[ch] >= cfg.fault_confirm_ms) fault_short |=  (1ul << ch);
        else if (!short_now)                      fault_short &= ~(1ul << ch);
    }
}

uint32_t ch_commanded(void) { return commanded; }
uint32_t ch_requested(void) { return requested; }
uint32_t ch_sense_raw(void) { return stable_sense; }
uint8_t  ch_aux(void)       { return aux_stable; }
uint32_t ch_fault_open(void)  { return fault_open; }
uint32_t ch_fault_short(void) { return fault_short; }

void ch_clear_faults(void)
{
    fault_open = fault_short = 0;
    memset(open_ms, 0, sizeof(open_ms));
    memset(short_ms, 0, sizeof(short_ms));
}

uint32_t ch_inputs(void)
{
    uint32_t v = 0;
    for (uint8_t ch = 0; ch < RCM_CHANNELS; ch++) {
        if (cfg.ch[ch].mode != CH_INPUT) continue;
        bool on = (stable_sense >> ch) & 1u;
        if (cfg.ch[ch].flags & CH_F_INVERT) on = !on;
        if (on) v |= 1ul << ch;
    }
    return v;
}
