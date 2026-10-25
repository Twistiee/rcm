/*
 * lights.h -- headlights driven by this board, from their own switches.
 *
 * The low- and high-beam switches are read as INPUTS and this board drives the relays,
 * rather than the switches grounding the coils directly. That is what lets the board
 * drop the headlights while the engine is being cranked, and it is the hook for any
 * later lighting logic. Park stays wired direct, because it has to work with the car off.
 *
 * The pairing is by label, like every other role on this board:
 *     FN_IN_HEADLIGHT  -> FN_HEADLIGHT_LOW
 *     FN_IN_HIGH_BEAM  -> FN_HEADLIGHT_HIGH
 * The input may be a channel or a J_AUX pin. With no input carrying the label, the
 * output is left entirely alone -- a board that does not use this is unaffected.
 *
 * A headlight is ON when its switch is on OR a follow slot asks for it (auto headlights
 * from a keypad button), and OFF regardless while any board is starting the car.
 */
#ifndef RCM_LIGHTS_H
#define RCM_LIGHTS_H

#include <stdint.h>

void lights_tick(uint32_t now_ms);

/* True for a headlight output whose switch input is labelled -- this module drives it,
 * and follow slots aimed at it are ORed in here rather than commanding it directly. */
bool lights_owns(uint8_t ch);

#endif /* RCM_LIGHTS_H */
