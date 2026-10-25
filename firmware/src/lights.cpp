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

bool lights_owns(uint8_t ch)
{
    return ch < RCM_CHANNELS
        && (ch == owned_output(FN_IN_HEADLIGHT, FN_HEADLIGHT_LOW)
         || ch == owned_output(FN_IN_HIGH_BEAM, FN_HEADLIGHT_HIGH));
}

static void follow(uint8_t in_func, uint8_t out_func, bool shed)
{
    const uint8_t ch = owned_output(in_func, out_func);
    if (ch >= RCM_CHANNELS) return;
    bool found;
    const bool sw = read_input(in_func, &found);
    /* Reasserted every tick, the same as the RUN output: neither a failsafe nor a stray
     * CAN command can leave the headlights in a state nobody asked for. */
    ch_command(ch, (sw || proto_follow_requests(ch)) && !shed);
}

void lights_tick(uint32_t now_ms)
{
    /* Shed while ANY board is starting the car -- the keypad, which owns the start
     * button, or this board if it ever owns one itself. Both answers already time out
     * on their own (crank window, stale STATUS), so the headlights always come back. */
    const bool shed = proto_peer_starting(now_ms) || ign_starting();
    follow(FN_IN_HEADLIGHT, FN_HEADLIGHT_LOW,  shed);
    follow(FN_IN_HIGH_BEAM, FN_HEADLIGHT_HIGH, shed);
}
