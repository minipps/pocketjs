#ifndef POCKET_WII_H
#define POCKET_WII_H

/*
 * PocketJS Wii host runtime.
 *
 * The application owns the VI and GX lifetimes: it initializes/configures the
 * video mode and GX FIFO, starts a frame, calls pocket_wii_update(), then
 * calls pocket_wii_draw() while its EFB target is current. The host never
 * presents, waits for VSync, or restores GX state.
 *
 * The package and all of its sections are borrowed. Keep `package` unchanged
 * and alive until pocket_wii_shutdown(). This API is a singleton and must be
 * called from one thread.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum PocketWiiResult {
  POCKET_WII_OK = 0,
  POCKET_WII_ERR_ARGUMENT = -1,
  POCKET_WII_ERR_STATE = -2,
  POCKET_WII_ERR_PACKAGE = -3,
  POCKET_WII_ERR_GUEST = -4,
  POCKET_WII_ERR_RENDER = -5
} PocketWiiResult;

/* C API version. This is independent of the package host ABI (wii-dev uses
 * host ABI 9) and remains 1 while this singleton contract is compatible. */
uint32_t pocket_wii_api_version(void);

/* Empty on success; otherwise a process-lifetime diagnostic string. */
const char *pocket_wii_last_error(void);

/* Mount one target-thinned `.pocket` package. The caller retains ownership. */
PocketWiiResult pocket_wii_init(const uint8_t *package, size_t length);

/* Submit one hardware-neutral input sample and advance one fixed 1/60 tick.
 * `analog` packs the relative left-axis sample as (x << 8) | y, with the
 * shared center value 0x8080. */
PocketWiiResult pocket_wii_update(uint32_t buttons, uint32_t analog);

/* Draw the most recently updated DrawList into the caller-selected EFB rect.
 * No framebuffer copy/present or VSync operation is performed. */
PocketWiiResult pocket_wii_draw(float x, float y, float width, float height);

/* Wait for queued GX work, release GPU resources, and unmount the guest. */
void pocket_wii_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
