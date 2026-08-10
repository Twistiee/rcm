/*
 * app.h -- the small amount of board-wide state that does not belong to any one
 * driver: whether the outputs are live, whether the ignition is up, and the
 * shutdown sequence.
 */
#ifndef RCM_APP_H
#define RCM_APP_H

#include <stdint.h>
#include <stdbool.h>

bool app_outputs_live(void);
void app_set_outputs_live(bool live);

bool     app_ignition_on(void);
uint16_t app_ignition_mv(void);     /* at J_IGN, reconstructed through the divider */

bool app_eeprom_ok(void);

#endif /* RCM_APP_H */
