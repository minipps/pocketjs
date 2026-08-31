#ifndef POCKET_WII_GX_H
#define POCKET_WII_GX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The renderer is deliberately private to pocket_wii.c. It assumes that the
 * caller has already initialized GX and selected the current EFB target. */
bool pocket_wii_gx_draw(
  const uint32_t *words,
  size_t length,
  uint32_t logical_width,
  uint32_t logical_height,
  float target_x,
  float target_y,
  float target_width,
  float target_height
);
const char *pocket_wii_gx_last_error(void);
void pocket_wii_gx_shutdown(void);

#endif
