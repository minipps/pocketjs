#ifndef POCKET_WII_EXAMPLE_INPUT_H
#define POCKET_WII_EXAMPLE_INPUT_H

#include <stdint.h>

typedef struct {
  uint32_t buttons;
  uint32_t analog;
} pocket_wii_input_t;

void pocket_wii_input_init(void);
void pocket_wii_input_poll(pocket_wii_input_t *input);

#endif
