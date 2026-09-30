#include <stdint.h>

static uint64_t pocket_wii_schedule_ticks(uint64_t elapsed,
                                          uint64_t ticks_per_second,
                                          uint64_t *phase) {
  uint64_t scaled_elapsed = *phase + elapsed * 60;
  *phase = scaled_elapsed % ticks_per_second;
  return scaled_elapsed / ticks_per_second;
}

#ifdef POCKET_WII_SCHEDULE_CHECK
#include <assert.h>

static uint64_t simulate_video_rate(uint64_t video_hz) {
  const uint64_t ticks_per_second = 30000;
  uint64_t phase = 0;
  uint64_t ticks = 0;
  assert(ticks_per_second % video_hz == 0);
  for (uint64_t frame = 0; frame < video_hz; ++frame)
    ticks += pocket_wii_schedule_ticks(ticks_per_second / video_hz,
                                       ticks_per_second, &phase);
  assert(phase == 0);
  return ticks;
}

int main(void) {
  uint64_t phase = 0;
  assert(simulate_video_rate(50) == 60);
  assert(simulate_video_rate(60) == 60);
  assert(pocket_wii_schedule_ticks(499, 30000, &phase) == 0);
  assert(pocket_wii_schedule_ticks(1, 30000, &phase) == 1);
  assert(phase == 0);
  return 0;
}
#else
#include <gccore.h>
#include <malloc.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/system.h>
#include <stdio.h>
#include <string.h>

#include "pocket_wii.h"
#include "hero_package.h"
#include "input.h"

#define FIFO_SIZE (256 * 1024)

static void caller_gx_state(const GXRModeObj *mode) {
  Mtx identity;
  Mtx44 projection;
  guMtxIdentity(identity);
  guOrtho(projection, 0.0f, (f32)mode->efbHeight, 0.0f,
          (f32)mode->fbWidth, 0.0f, 1.0f);
  GX_LoadPosMtxImm(identity, GX_PNMTX0);
  GX_SetCurrentMtx(GX_PNMTX0);
  GX_LoadProjectionMtx(projection, GX_ORTHOGRAPHIC);
  GX_SetViewport(0.0f, 0.0f, (f32)mode->fbWidth, (f32)mode->efbHeight,
                 0.0f, 1.0f);
  GX_SetScissor(0, 0, mode->fbWidth, mode->efbHeight);
  GX_ClearVtxDesc();
  GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
  GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
  GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
  GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
  GX_SetNumChans(1);
  GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
                 GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
  GX_SetNumTexGens(0);
  GX_SetNumTevStages(1);
  GX_SetNumIndStages(0);
  GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
  GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
  GX_SetCullMode(GX_CULL_NONE);
  GX_SetZMode(GX_DISABLE, GX_ALWAYS, GX_FALSE);
  GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
  GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
  GX_SetColorUpdate(GX_TRUE);
  GX_SetAlphaUpdate(GX_TRUE);
}

static void draw_caller_rect(f32 x, f32 y, f32 size, GXColor color) {
  GX_Begin(GX_TRIANGLES, GX_VTXFMT0, 6);
  GX_Position2f32(x, y); GX_Color4u8(color.r, color.g, color.b, color.a);
  GX_Position2f32(x + size, y); GX_Color4u8(color.r, color.g, color.b, color.a);
  GX_Position2f32(x + size, y + size); GX_Color4u8(color.r, color.g, color.b, color.a);
  GX_Position2f32(x, y); GX_Color4u8(color.r, color.g, color.b, color.a);
  GX_Position2f32(x + size, y + size); GX_Color4u8(color.r, color.g, color.b, color.a);
  GX_Position2f32(x, y + size); GX_Color4u8(color.r, color.g, color.b, color.a);
  GX_End();
}

static int init_video(GXRModeObj **mode_out, void **xfb, void **fifo) {
  VIDEO_Init();
  GXRModeObj *mode = VIDEO_GetPreferredMode(NULL);
  if (mode == NULL) return 0;
  xfb[0] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(mode));
  xfb[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(mode));
  *fifo = MEM_K0_TO_K1(memalign(32, FIFO_SIZE));
  if (xfb[0] == NULL || xfb[1] == NULL || *fifo == NULL) return 0;

  memset(*fifo, 0, FIFO_SIZE);
  VIDEO_Configure(mode);
  VIDEO_SetNextFramebuffer(xfb[0]);
  VIDEO_SetBlack(true);
  VIDEO_Flush();
  VIDEO_WaitVSync();

  GX_Init(*fifo, FIFO_SIZE);
  GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
  GX_SetCopyClear((GXColor){0, 0, 0, 255}, GX_MAX_Z24);
  GX_SetScissor(0, 0, mode->fbWidth, mode->efbHeight);
  GX_SetDispCopySrc(0, 0, mode->fbWidth, mode->efbHeight);
  GX_SetDispCopyDst(mode->fbWidth, mode->xfbHeight);
  GX_SetDispCopyYScale(GX_GetYScaleFactor(mode->efbHeight, mode->xfbHeight));
  GX_SetCopyFilter(mode->aa, mode->sample_pattern, GX_TRUE, mode->vfilter);
  caller_gx_state(mode);
  *mode_out = mode;
  return 1;
}

int main(void) {
  SYS_STDIO_Report(true);
  GXRModeObj *mode = NULL;
  void *xfb[2] = { NULL, NULL };
  void *fifo = NULL;
  if (!init_video(&mode, xfb, &fifo)) {
    puts("W22 FAIL: video or GX setup failed");
    free(fifo);
    return 1;
  }

  pocket_wii_input_init();
  if (pocket_wii_boot(hero_main_pocket, hero_main_pocket_len) != 0) {
    printf("W22 FAIL: PocketJS boot: %s\n", pocket_wii_last_error());
    pocket_wii_shutdown();
    return 2;
  }
  puts("W22 PASS: hero-main.pocket booted");

  unsigned framebuffer = 0;
  pocket_wii_input_t input;
  uint64_t last_time = gettime();
  uint64_t tick_phase = 0;
  for (;;) {
    pocket_wii_input_poll(&input);

    uint64_t now = gettime();
    uint64_t ticks_due = pocket_wii_schedule_ticks(
        diff_ticks(last_time, now), PPC_TIMER_CLOCK, &tick_phase);
    last_time = now;
    int tick_failed = 0;
    /* ponytail: drain all overdue ticks; cap catch-up only if stalls cause persistent lag. */
    while (ticks_due != 0) {
      --ticks_due;
      if (pocket_wii_tick(input.buttons, input.analog) != 0) {
        printf("W24 FAIL: PocketJS tick: %s\n", pocket_wii_last_error());
        tick_failed = 1;
        break;
      }
    }
    if (tick_failed) {
      break;
    }

    caller_gx_state(mode);
    draw_caller_rect(12.0f, 12.0f, 24.0f, (GXColor){255, 48, 48, 255});
    if (pocket_wii_draw(80, 80, 480, 272) != 0) {
      printf("W22 FAIL: PocketJS draw: %s\n", pocket_wii_last_error());
      break;
    }
    /* pocket_wii_draw owns GX state until the caller binds its state again. */
    caller_gx_state(mode);
    draw_caller_rect((f32)mode->fbWidth - 36.0f, 12.0f, 24.0f,
                     (GXColor){48, 255, 96, 255});

    GX_CopyDisp(xfb[framebuffer], GX_TRUE);
    GX_DrawDone();
    VIDEO_SetNextFramebuffer(xfb[framebuffer]);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    framebuffer ^= 1;
  }

  pocket_wii_shutdown();
  free(fifo);
  return 3;
}
#endif
