/* Minimal Wii DOL entry point used by tools/wii.ts.
 *
 * It owns video/GX/input setup, admits one target-thinned package through the
 * public Wii runtime, and presents one DrawList per 60 Hz frame. It is an
 * example shell; applications that already own GX can call pocket_wii_* from
 * their own frame loop instead. */

#include <gccore.h>
#include <ogc/console.h>
#include <ogc/system.h>
#include <ogc/video.h>
#include <wiiuse/wpad.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "app_pocket_bin.h"
#include "input.h"
#include "pocket_spec.h"
#include "pocket_wii.h"

#ifndef POCKETJS_TARGET_ID
#error "POCKETJS_TARGET_ID must come from the resolved Wii build plan"
#endif
#ifndef POCKETJS_HOST_ABI
#error "POCKETJS_HOST_ABI must come from the resolved Wii build plan"
#endif
#ifndef POCKETJS_VIEW_W
#error "POCKETJS_VIEW_W must come from the resolved Wii build plan"
#endif
#ifndef POCKETJS_VIEW_H
#error "POCKETJS_VIEW_H must come from the resolved Wii build plan"
#endif
#ifndef POCKETJS_INTEGER_FIT
#error "POCKETJS_INTEGER_FIT must come from the resolved Wii build plan"
#endif
#ifndef POCKETJS_RASTER_DENSITY
#error "POCKETJS_RASTER_DENSITY must come from the resolved Wii build plan"
#endif

static void *framebuffers[2] ATTRIBUTE_ALIGN(32);
static GXRModeObj *video_mode;
static uint8_t gx_fifo[256 * 1024] ATTRIBUTE_ALIGN(32);

/* libogc's default main stack is 128 KiB. QuickJS's native stack guard is
 * 192 KiB, so replace the SDK's weak __ppc_main_sp with a larger stack. */
#define POCKET_WII_MAIN_STACK_SIZE (256 * 1024)
static uint8_t wii_main_stack[POCKET_WII_MAIN_STACK_SIZE] ATTRIBUTE_ALIGN(32);
void *__ppc_main_sp = wii_main_stack + sizeof wii_main_stack;

static void show_failure(const char *message) {
  printf("PocketJS Wii\nFAILED: %s\nPress HOME to exit.\n", message ? message : "unknown error");
  VIDEO_Flush();
  VIDEO_WaitVSync();
  for (;;) {
    wii_input_scan();
    if (wii_input_home_down()) return;
    VIDEO_WaitVSync();
  }
}

static void init_gx(void) {
  GX_Init(gx_fifo, sizeof gx_fifo);
  GX_SetCopyClear((GXColor){ 0, 0, 0, 255 }, GX_MAX_Z24);
  GX_SetViewport(
    0.0f,
    0.0f,
    (f32)video_mode->fbWidth,
    (f32)video_mode->efbHeight,
    0.0f,
    1.0f
  );
  GX_SetScissor(0, 0, video_mode->fbWidth, video_mode->efbHeight);
  GX_SetDispCopySrc(0, 0, video_mode->fbWidth, video_mode->efbHeight);
  GX_SetDispCopyDst(video_mode->fbWidth, video_mode->xfbHeight);
  GX_SetCopyFilter(
    video_mode->aa,
    video_mode->sample_pattern,
    GX_TRUE,
    video_mode->vfilter
  );
  GX_SetCopyClamp(GX_CLAMP_NONE);
  GX_SetDispCopyGamma(GX_GM_1_0);
  GX_CopyDisp(framebuffers[0], GX_TRUE);
  GX_Flush();
}

static void fit_viewport(
  float *x,
  float *y,
  float *width,
  float *height
) {
  const float physical_width = (float)video_mode->fbWidth;
  const float physical_height = (float)video_mode->efbHeight;
#if POCKETJS_INTEGER_FIT
  const unsigned scale_x = video_mode->fbWidth / POCKETJS_VIEW_W;
  const unsigned scale_y = video_mode->efbHeight / POCKETJS_VIEW_H;
  const unsigned scale = scale_x < scale_y ? scale_x : scale_y;
  *width = (float)(POCKETJS_VIEW_W * scale);
  *height = (float)(POCKETJS_VIEW_H * scale);
#else
  const float scale_x = physical_width / (float)POCKETJS_VIEW_W;
  const float scale_y = physical_height / (float)POCKETJS_VIEW_H;
  const float scale = scale_x < scale_y ? scale_x : scale_y;
  *width = (float)POCKETJS_VIEW_W * scale;
  *height = (float)POCKETJS_VIEW_H * scale;
#endif
  *x = (physical_width - *width) * 0.5f;
  *y = (physical_height - *height) * 0.5f;
}

int main(void) {
  VIDEO_Init();
  WPAD_Init();

  video_mode = VIDEO_GetPreferredMode(NULL);
  framebuffers[0] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(video_mode));
  framebuffers[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(video_mode));
  console_init(
    framebuffers[0],
    20,
    20,
    video_mode->fbWidth,
    video_mode->xfbHeight,
    video_mode->fbWidth * VI_DISPLAY_PIX_SZ
  );
  VIDEO_Configure(video_mode);
  VIDEO_SetNextFramebuffer(framebuffers[0]);
  VIDEO_SetBlack(FALSE);
  VIDEO_Flush();
  VIDEO_WaitVSync();
  init_gx();
  if (pocket_wii_init(app_pocket_bin, app_pocket_bin_size) != POCKET_WII_OK) {
    show_failure(pocket_wii_last_error());
    return 1;
  }

  printf("PocketJS %s (%ux%u)\n", POCKETJS_TARGET_ID, POCKETJS_VIEW_W, POCKETJS_VIEW_H);
  float target_x;
  float target_y;
  float target_width;
  float target_height;
  fit_viewport(&target_x, &target_y, &target_width, &target_height);
  unsigned int framebuffer = 0;
  for (;;) {
    wii_input_scan();
    if (wii_input_home_down()) break;
    if (pocket_wii_update(wii_input_buttons(), POCKET_ANALOG_CENTER) != POCKET_WII_OK) {
      show_failure(pocket_wii_last_error());
      break;
    }
    if (pocket_wii_draw(
          target_x,
          target_y,
          target_width,
          target_height
        ) != POCKET_WII_OK) {
      show_failure(pocket_wii_last_error());
      break;
    }
    GX_DrawDone();
    GX_SetCopyClear((GXColor){ 0, 0, 0, 255 }, GX_MAX_Z24);
    framebuffer ^= 1u;
    GX_CopyDisp(framebuffers[framebuffer], GX_TRUE);
    GX_Flush();
    VIDEO_SetNextFramebuffer(framebuffers[framebuffer]);
    VIDEO_Flush();
    VIDEO_WaitVSync();
  }

  pocket_wii_shutdown();
  return 0;
}
