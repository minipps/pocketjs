/*
 * PocketJS Wii singleton runtime.
 *
 * VI/GX are intentionally caller-owned. Mounting and updating only touch the
 * package, QuickJS and Rust core; the first draw lazily prepares GX state
 * after the caller has selected an EFB target. Drawing also leaves GX state
 * in the configured 2-D state for the caller to replace as needed.
 */

#include "pocket_wii.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pocket_core.h"
#include "pocket_wii_gx.h"
#include "pocket_wii_qjs.h"

#ifndef POCKETJS_TARGET_ID
#error "POCKETJS_TARGET_ID must come from the verified ResolvedBuildPlan"
#endif
#ifndef POCKETJS_HOST_ABI
#error "POCKETJS_HOST_ABI must come from the verified ResolvedBuildPlan"
#endif
#ifndef POCKETJS_RASTER_DENSITY
#error "POCKETJS_RASTER_DENSITY must come from the verified ResolvedBuildPlan"
#endif
#ifndef POCKETJS_VIEW_W
#error "POCKETJS_VIEW_W must come from the verified ResolvedBuildPlan"
#endif
#ifndef POCKETJS_VIEW_H
#error "POCKETJS_VIEW_H must come from the verified ResolvedBuildPlan"
#endif

#define POCKET_WII_VIEW_WIDTH POCKETJS_VIEW_W
#define POCKET_WII_VIEW_HEIGHT POCKETJS_VIEW_H

#define POCKET_WII_API_VERSION 1u

static bool mounted;
static bool frame_ready;
static const uint8_t *installed_package;
static size_t installed_package_length;
static char last_error[256];

static void clear_error(void) {
  last_error[0] = '\0';
}

static void set_error(const char *message) {
  size_t length = message == NULL ? 0 : strlen(message);
  if (length >= sizeof last_error) length = sizeof last_error - 1;
  if (length > 0) memcpy(last_error, message, length);
  last_error[length] = '\0';
}

static void set_package_error(int32_t code) {
  (void)snprintf(last_error, sizeof last_error, "package rejected (code %ld)", (long)code);
}

static void set_guest_error(void) {
  const char *message = qjs_last_error();
  if (message == NULL || message[0] == '\0') {
    set_error("guest evaluation failed");
    return;
  }
  size_t prefix_length = strlen("guest: ");
  size_t message_length = strlen(message);
  if (prefix_length + message_length >= sizeof last_error) {
    message_length = sizeof last_error - prefix_length - 1u;
  }
  memcpy(last_error, "guest: ", prefix_length);
  memcpy(last_error + prefix_length, message, message_length);
  last_error[prefix_length + message_length] = '\0';
}

uint32_t pocket_wii_api_version(void) {
  return POCKET_WII_API_VERSION;
}

const char *pocket_wii_last_error(void) {
  return last_error;
}

PocketWiiResult pocket_wii_init(const uint8_t *package, size_t length) {
  if (mounted) {
    set_error("Wii guest is already mounted");
    return POCKET_WII_ERR_STATE;
  }
  if (package == NULL || length == 0) {
    set_error("Wii package is empty");
    return POCKET_WII_ERR_ARGUMENT;
  }

  PocketGuestPackage guest;
  const uint8_t *target = (const uint8_t *)POCKETJS_TARGET_ID;
  int32_t package_result = pocket_package_open(
    package,
    length,
    target,
    strlen(POCKETJS_TARGET_ID),
    (uint32_t)POCKETJS_HOST_ABI,
    &guest
  );
  if (package_result != 0) {
    set_package_error(package_result);
    return POCKET_WII_ERR_PACKAGE;
  }
  if (guest.javascript == NULL || guest.javascript_length == 0) {
    set_error("Wii package has no JavaScript section");
    return POCKET_WII_ERR_PACKAGE;
  }

  /* The package bytes, including JS and pak sections, remain caller-owned. */
  ui_init((uint32_t)POCKETJS_RASTER_DENSITY);
  ui_set_viewport((float)POCKET_WII_VIEW_WIDTH, (float)POCKET_WII_VIEW_HEIGHT);
  if (guest.pak != NULL && guest.pak_length > 0) {
    ui_feed_pak(guest.pak, guest.pak_length);
  }
  if (!qjs_boot(
        (const char *)guest.javascript,
        guest.javascript_length > 0 ? guest.javascript_length - 1u : 0u,
        guest.pak,
        guest.pak_length
      )) {
    set_guest_error();
    qjs_shutdown();
    ui_shutdown();
    return POCKET_WII_ERR_GUEST;
  }

  installed_package = package;
  installed_package_length = length;
  mounted = true;
  frame_ready = false;
  clear_error();
  return POCKET_WII_OK;
}

PocketWiiResult pocket_wii_update(uint32_t buttons, uint32_t analog) {
  if (!mounted) {
    set_error("Wii guest is not mounted");
    return POCKET_WII_ERR_STATE;
  }
  /* The Wii host ingests only the portable buttons/packed-relative-axis
   * contract. WPAD/PAD SDK types never cross this API. */
  if (!qjs_frame((int32_t)buttons, (int32_t)analog, NULL, NULL, 0)) {
    frame_ready = false;
    set_guest_error();
    return POCKET_WII_ERR_GUEST;
  }
  ui_tick();
  (void)ui_draw();
  frame_ready = true;
  clear_error();
  return POCKET_WII_OK;
}

PocketWiiResult pocket_wii_draw(float x, float y, float width, float height) {
  if (!mounted || !frame_ready) {
    set_error("Wii DrawList is not ready");
    return POCKET_WII_ERR_STATE;
  }
  const uint32_t *words = ui_draw_list_ptr();
  size_t length = ui_draw_list_len();
  if (!pocket_wii_gx_draw(
        words,
        length,
        ui_viewport_width(),
        ui_viewport_height(),
        x,
        y,
      width,
      height
    )) {
    frame_ready = false;
    const char *message = pocket_wii_gx_last_error();
    if (message == NULL || message[0] == '\0') set_error("Wii GX draw failed");
    else set_error(message);
    return POCKET_WII_ERR_RENDER;
  }
  clear_error();
  return POCKET_WII_OK;
}

void pocket_wii_shutdown(void) {
  if (!mounted) return;
  /* GX resources are released first; the renderer waits for the FIFO before
   * freeing texture memory that a previous draw may still reference. */
  pocket_wii_gx_shutdown();
  qjs_shutdown();
  ui_shutdown();
  installed_package = NULL;
  installed_package_length = 0;
  mounted = false;
  frame_ready = false;
  clear_error();
}
