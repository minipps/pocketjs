#ifndef POCKET_WII_EXAMPLE_INPUT_H
#define POCKET_WII_EXAMPLE_INPUT_H

#include <stdint.h>

#define POCKET_WII_WPAD_CHANNELS 4

typedef struct {
  uint32_t buttons;
  uint32_t analog;
  int wpad_status;
  int wpad_probe[POCKET_WII_WPAD_CHANNELS];
} pocket_wii_input_t;

int pocket_wii_input_init(void);
void pocket_wii_input_poll(pocket_wii_input_t *input);

#endif
