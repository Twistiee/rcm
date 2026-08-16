/*
 * chnames.h -- names for the channel function and behaviour bytes.
 *
 * Separate from config.h so the tables are not dragged into every translation unit
 * that only needs the enums.
 */
#ifndef RCM_CHNAMES_H
#define RCM_CHNAMES_H

#include <stdint.h>
#include <stdbool.h>

const char *ch_func_name(uint8_t func);
const char *ch_behaviour_name(uint8_t behaviour);

/* Function labels are grouped so that every input sits at FN_IN_BRAKE or above. That is
 * what makes "this channel is labelled Fuel pump but configured as an input" a
 * detectable mistake rather than a silent one. */
bool ch_func_is_input(uint8_t func);
bool ch_func_matches_mode(uint8_t func, uint8_t mode);

#endif /* RCM_CHNAMES_H */
