/*
 * lights.cpp -- see lights.h.
 */
#include "lights.h"
#include "channels.h"
#include "config.h"
#include "ignition.h"
#include "protocol.h"

/* Is the input carrying `func` pressed? Channels first, then J_AUX. `found` says
 * whether any input carries the label at all, which is what decides whether this
 * module touches the paired output. */
static bool read_input(uint8_t func, bool *found)
{
    const uint8_t ch = cfg_ch_for(func);
    if (ch < RCM_CHANNELS && cfg.ch[ch].mode == CH_INPUT) {
        *found = true;
        return (ch_inputs() >> ch) & 1u;
    }
    for (uint8_t a = 0; a < RCM_AUX_INPUTS; a++)
        if (cfg.aux_func[a] == func) { *found = true; return (ch_aux() >> a) & 1u; }
    *found = false;
    return false;
}

/* The output this module drives for an input label, or RCM_CHANNELS if it does not
 * drive one -- no switch labelled, or no output labelled to pair it with. */
static uint8_t owned_output(uint8_t in_func, uint8_t out_func)
{
    bool found;
    (void)read_input(in_func, &found);
    if (!found) return RCM_CHANNELS;
    const uint8_t ch = cfg_ch_for(out_func);
    if (ch >= RCM_CHANNELS || cfg.ch[ch].mode != CH_OUTPUT) return RCM_CHANNELS;
    return ch;
}

/* Channels an auto-headlight follow slot drives. */
static uint32_t auto_targets(void)
{
    uint32_t m = 0;
    for (uint8_t i = 0; i < RCM_ECU_FOLLOWS; i++) {
        const struct ecu_follow_t *fl = &cfg.ecu_follow[i];
        if (!((cfg.auto_light_slots >> i) & 1u)) continue;
        if (!fl->can_id || fl->ch >= RCM_CHANNELS) continue;
        if (cfg.ch[fl->ch].mode != CH_OUTPUT) continue;
        m |= 1u << fl->ch;
    }
    return m;
}

bool lights_owns(uint8_t ch)
{
    return ch < RCM_CHANNELS
        && (ch == owned_output(FN_IN_HEADLIGHT, FN_HEADLIGHT_LOW)
         || ch == owned_output(FN_IN_HIGH_BEAM, FN_HEADLIGHT_HIGH)
         || ((auto_targets() >> ch) & 1u));
}

/* Dark enough for auto headlights. With no sensor labelled it is always dark, so the
 * button alone decides -- which is also what an unplugged sensor on a pulled-up pin
 * reads, so losing the sensor fails to lights ON. */
static bool dark(void)
{
    bool found;
    const bool active = read_input(FN_IN_LIGHT_SENSOR, &found);
    return !found || active;
}

void lights_tick(uint32_t now_ms)
{
    /* Shed while ANY board is starting the car -- the keypad, which owns the start
     * button, or this board if it ever owns one itself. Both answers already time out
     * on their own (crank window, stale STATUS), so the headlights always come back. */
    const bool shed = proto_peer_starting(now_ms) || ign_starting();
    /* Auto lights run with the ENGINE, not the ignition: on at the start of a drive, off
     * when it stops, never draining the battery with the engine off, and dropped for a
     * crank. Headlights also wait for dark; anything else on an auto slot -- park --
     * does not, which makes it a daytime running light. The manual switches are not
     * gated by any of this -- they work with the ignition alone. */
    const bool auto_run  = ign_engine_running() && !shed;
    const bool auto_dark = dark();

    const uint8_t low  = owned_output(FN_IN_HEADLIGHT, FN_HEADLIGHT_LOW);
    const uint8_t high = owned_output(FN_IN_HIGH_BEAM, FN_HEADLIGHT_HIGH);
    const uint32_t targets = auto_targets();
    bool found;

    for (uint8_t ch = 0; ch < RCM_CHANNELS; ch++) {
        bool on;
        if (ch == low)       on = read_input(FN_IN_HEADLIGHT, &found);
        else if (ch == high) on = read_input(FN_IN_HIGH_BEAM, &found);
        else if ((targets >> ch) & 1u) on = false;
        else continue;                                  /* not ours */

        const uint8_t fn = cfg.ch[ch].func;
        const bool headlight = fn == FN_HEADLIGHT_LOW || fn == FN_HEADLIGHT_HIGH;
        on = on || proto_follow_requests(ch);
        /* Headlights shed whatever asked for them. Anything else keeps a switch or an
         * ordinary follow slot through the crank, as it always did. */
        if (shed && headlight) on = false;
        if (auto_run && (auto_dark || !headlight) && proto_auto_requests(ch)) on = true;
        /* Reasserted every tick, the same as the RUN output: neither a failsafe nor a
         * stray CAN command can leave the lights in a state nobody asked for. */
        ch_command(ch, on);
    }
}
