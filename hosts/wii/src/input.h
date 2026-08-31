#ifndef POCKETJS_WII_INPUT_H
#define POCKETJS_WII_INPUT_H

#include <stdbool.h>
#include <stdint.h>

/* Poll once per frame, then map held Wii Remote buttons to the portable
 * contracts/spec/spec.ts button bitmask. HOME is a host control, never guest
 * input. */
void wii_input_scan(void);
int32_t wii_input_buttons(void);
bool wii_input_home_down(void);

#endif
