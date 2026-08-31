/*
 * Direct GX DrawList backend for the Wii host.
 *
 * The caller owns VIDEO/GX initialization and the current EFB target. This
 * file only loads the state needed by the 2-D pipeline and emits immediate
 * GX vertices in DrawList order. It never copies or presents the framebuffer.
 *
 * DrawList words are native u32 values produced by the Rust core. Texture
 * payloads remain portable little-endian bytes, so the PSM16 readers below
 * decode them explicitly instead of relying on the PowerPC byte order. GX
 * texture memory is also written byte-by-byte: RGBA8 uses interleaved AR/GB
 * 4x4 tiles and IA8 uses I/A 4x4 tiles.
 */

#include "pocket_wii_gx.h"

#include <ogc/cache.h>
#include <ogc/gx.h>
#include <ogc/gu.h>

#include <malloc.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "pocket_core.h"

/* contracts/spec/spec.ts DRAW_OP. */
#define DRAW_RECT 1u
#define DRAW_GRAD_RECT 2u
#define DRAW_GLYPH_RUN 3u
#define DRAW_TEX_QUAD 4u
#define DRAW_SCISSOR 5u
#define DRAW_SCISSOR_POP 6u
#define DRAW_TRI 7u
#define DRAW_TEX_TRI 8u
#define DRAW_TEXT_RUN 9u
#define DRAW_SURFACE_QUAD 10u

/* contracts/spec/spec.ts GradDir. */
#define GRAD_TO_TOP 0u
#define GRAD_TO_BOTTOM 1u
#define GRAD_TO_LEFT 2u
#define GRAD_TO_RIGHT 3u

/* contracts/spec/spec.ts PSM. */
#define PSM_5650 0u
#define PSM_4444 2u
#define PSM_8888 3u
#define PSM_T8 5u

#define WII_TEX_MIN 4u
#define WII_TEX_MAX 1024u
#define WII_MAX_CLIP_DEPTH 64u

typedef struct {
  int32_t x;
  int32_t y;
  int32_t width;
  int32_t height;
} PocketWiiClip;

typedef struct {
  GXTexObj object;
  uint8_t *data;
  size_t data_length;
  uint16_t width;
  uint16_t height;
  uint8_t format;
  bool live;
} PocketWiiGpuTexture;

typedef struct {
  PocketWiiGpuTexture texture;
  int32_t handle;
  uint64_t revision;
  uint32_t source_width;
  uint32_t source_height;
  uint32_t source_psm;
  uint32_t source_linear;
  float u_scale;
  float v_scale;
} PocketWiiImage;

typedef struct {
  PocketWiiGpuTexture texture;
  const uint8_t *coverage;
  uint64_t raster_revision;
  uint32_t glyph_count;
  uint32_t cell_width;
  uint32_t cell_height;
  uint32_t coverage_width;
  uint32_t coverage_height;
  uint32_t columns;
} PocketWiiFont;

static PocketWiiGpuTexture white_texture;
static PocketWiiImage *images;
static size_t image_capacity;
static PocketWiiFont *fonts;
static size_t font_capacity;
static bool renderer_initialized;
static bool renderer_submitted;
static char renderer_error[256];

static void set_error(const char *message) {
  size_t length = message == NULL ? 0 : strlen(message);
  if (length >= sizeof renderer_error) length = sizeof renderer_error - 1;
  if (length > 0) memcpy(renderer_error, message, length);
  renderer_error[length] = '\0';
}

const char *pocket_wii_gx_last_error(void) {
  return renderer_error;
}

/* ------------------------------------------------------------------------- */
/* DrawList word decoding                                                    */
/* ------------------------------------------------------------------------- */

static inline float word_x(uint32_t word) {
  return (float)(int16_t)(word & 0xffffu);
}

static inline float word_y(uint32_t word) {
  return (float)(int16_t)((word >> 16) & 0xffffu);
}

static inline float word_width(uint32_t word) {
  return (float)(word & 0xffffu);
}

static inline float word_height(uint32_t word) {
  return (float)((word >> 16) & 0xffffu);
}

/* DrawList f32 words are native u32 values from the Rust core. */
static inline float word_float(uint32_t word) {
  float value;
  memcpy(&value, &word, sizeof value);
  return value;
}

/* DrawList colors are numeric ABGR; extraction is independent of host
 * endianness and yields the RGBA bytes expected by GX_Color4u8. */
static inline void unpack_color(uint32_t color, uint8_t out[4]) {
  out[0] = (uint8_t)(color & 0xffu);
  out[1] = (uint8_t)((color >> 8) & 0xffu);
  out[2] = (uint8_t)((color >> 16) & 0xffu);
  out[3] = (uint8_t)((color >> 24) & 0xffu);
}

/* ------------------------------------------------------------------------- */
/* GX texture conversion                                                     */
/* ------------------------------------------------------------------------- */

static size_t align32(size_t length) {
  return (length + 31u) & ~(size_t)31u;
}

static uint32_t next_power_of_two(uint32_t value) {
  uint32_t result = WII_TEX_MIN;
  while (result < value && result < WII_TEX_MAX) result <<= 1;
  return result < value ? 0 : result;
}

static void release_texture(PocketWiiGpuTexture *texture) {
  if (texture == NULL) return;
  if (texture->data != NULL) free(texture->data);
  memset(texture, 0, sizeof *texture);
}

static bool allocate_texture(
  PocketWiiGpuTexture *texture,
  uint32_t width,
  uint32_t height,
  uint8_t format
) {
  if (texture == NULL || width < WII_TEX_MIN || height < WII_TEX_MIN ||
      width > WII_TEX_MAX || height > WII_TEX_MAX ||
      (width & 3u) != 0 || (height & 3u) != 0) {
    return false;
  }
  u32 required = GX_GetTexBufferSize(
    (u16)width,
    (u16)height,
    format,
    GX_FALSE,
    0
  );
  if (required == 0) return false;
  size_t data_length = align32((size_t)required);
  uint8_t *data = memalign(32, data_length);
  if (data == NULL) return false;
  memset(data, 0, data_length);
  GX_InitTexObj(
    &texture->object,
    data,
    (u16)width,
    (u16)height,
    format,
    GX_CLAMP,
    GX_CLAMP,
    GX_FALSE
  );
  texture->data = data;
  texture->data_length = data_length;
  texture->width = (uint16_t)width;
  texture->height = (uint16_t)height;
  texture->format = format;
  texture->live = true;
  return true;
}

static void commit_texture(PocketWiiGpuTexture *texture, bool linear) {
  GX_InitTexObjWrapMode(&texture->object, GX_CLAMP, GX_CLAMP);
  GX_InitTexObjFilterMode(
    &texture->object,
    linear ? GX_LINEAR : GX_NEAR,
    linear ? GX_LINEAR : GX_NEAR
  );
  /* GX reads main memory directly. The aligned allocation and rounded range
   * satisfy the cache API's 32-byte contract. */
  DCFlushRange(texture->data, (u32)texture->data_length);
  GX_InvalidateTexAll();
}

/* Read source PSM bytes as RGBA. Image and CLUT bytes are already RGBA in
 * memory; only PSM16 fields need little-endian assembly. */
static bool fetch_texel(
  const PocketTexture *source,
  uint32_t x,
  uint32_t y,
  uint8_t out[4]
) {
  if (source == NULL || x >= source->width || y >= source->height ||
      source->pixels == NULL) {
    return false;
  }
  size_t index = (size_t)y * source->width + x;
  switch (source->pixel_storage) {
    case PSM_5650: {
      if (index >= source->pixels_length / 2u) return false;
      size_t offset = index * 2u;
      uint32_t pixel = (uint32_t)source->pixels[offset] |
                       ((uint32_t)source->pixels[offset + 1u] << 8);
      uint32_t red = pixel & 0x1fu;
      uint32_t green = (pixel >> 5) & 0x3fu;
      uint32_t blue = (pixel >> 11) & 0x1fu;
      out[0] = (uint8_t)((red << 3) | (red >> 2));
      out[1] = (uint8_t)((green << 2) | (green >> 4));
      out[2] = (uint8_t)((blue << 3) | (blue >> 2));
      out[3] = 255;
      return true;
    }
    case PSM_4444: {
      if (index >= source->pixels_length / 2u) return false;
      size_t offset = index * 2u;
      uint32_t pixel = (uint32_t)source->pixels[offset] |
                       ((uint32_t)source->pixels[offset + 1u] << 8);
      out[0] = (uint8_t)((pixel & 0x0fu) * 17u);
      out[1] = (uint8_t)(((pixel >> 4) & 0x0fu) * 17u);
      out[2] = (uint8_t)(((pixel >> 8) & 0x0fu) * 17u);
      out[3] = (uint8_t)(((pixel >> 12) & 0x0fu) * 17u);
      return true;
    }
    case PSM_8888: {
      if (index >= source->pixels_length / 4u) return false;
      memcpy(out, source->pixels + index * 4u, 4u);
      return true;
    }
    case PSM_T8: {
      if (index >= source->pixels_length || source->palette == NULL ||
          source->palette_length < 1024u) {
        return false;
      }
      size_t palette_offset = (size_t)source->pixels[index] * 4u;
      if (palette_offset + 4u > source->palette_length) return false;
      memcpy(out, source->palette + palette_offset, 4u);
      return true;
    }
    default:
      return false;
  }
}

/* GX_TF_RGBA8: each 4x4 tile has 16 interleaved A,R pairs followed by 16
 * interleaved G,B pairs. Do not write a uint16_t here: explicit bytes make
 * the layout correct on the Wii's big-endian PowerPC and in host-side tests. */
static void write_rgba8(
  PocketWiiGpuTexture *texture,
  uint32_t x,
  uint32_t y,
  const uint8_t rgba[4]
) {
  uint32_t tiles_across = texture->width / 4u;
  uint32_t tile = (y / 4u) * tiles_across + (x / 4u);
  uint32_t local = (y & 3u) * 4u + (x & 3u);
  uint8_t *destination = texture->data + (size_t)tile * 64u;
  destination[local * 2u] = rgba[3];
  destination[local * 2u + 1u] = rgba[0];
  destination[32u + local * 2u] = rgba[1];
  destination[32u + local * 2u + 1u] = rgba[2];
}

/* GX_TF_IA8: each 4x4 tile contains interleaved intensity, alpha bytes. */
static void write_ia8(
  PocketWiiGpuTexture *texture,
  uint32_t x,
  uint32_t y,
  uint8_t intensity,
  uint8_t alpha
) {
  uint32_t tiles_across = texture->width / 4u;
  uint32_t tile = (y / 4u) * tiles_across + (x / 4u);
  uint32_t local = (y & 3u) * 4u + (x & 3u);
  uint8_t *destination = texture->data + (size_t)tile * 32u;
  destination[local * 2u] = intensity;
  destination[local * 2u + 1u] = alpha;
}

static bool upload_image(
  PocketWiiGpuTexture *texture,
  const PocketTexture *source,
  uint32_t texture_width,
  uint32_t texture_height
) {
  if (!allocate_texture(texture, texture_width, texture_height, GX_TF_RGBA8)) {
    return false;
  }
  bool valid = true;
  for (uint32_t y = 0; y < source->height; y += 1u) {
    for (uint32_t x = 0; x < source->width; x += 1u) {
      uint8_t rgba[4];
      if (fetch_texel(source, x, y, rgba)) {
        write_rgba8(texture, x, y, rgba);
      } else {
        valid = false;
      }
    }
  }
  if (!valid) {
    release_texture(texture);
    return false;
  }
  commit_texture(texture, source->linear != 0);
  return true;
}

static bool font_grid(
  const PocketFontAtlas *atlas,
  uint32_t *out_columns,
  uint32_t *out_width,
  uint32_t *out_height
) {
  if (atlas == NULL || atlas->coverage == NULL || atlas->coverage_length == 0 ||
      atlas->glyph_count == 0 || atlas->coverage_width == 0 ||
      atlas->coverage_height == 0 || atlas->coverage_width > WII_TEX_MAX ||
      atlas->coverage_height > WII_TEX_MAX) {
    return false;
  }
  uint32_t max_columns = WII_TEX_MAX / atlas->coverage_width;
  if (max_columns == 0) return false;

  /* Start near a square atlas, then fall back to narrower grids if the row
   * count would exceed GX's 1024-pixel texture limit. */
  uint32_t square = 1;
  while ((uint64_t)square * square < atlas->glyph_count && square < max_columns) {
    square += 1u;
  }
  for (uint32_t pass = 0; pass < 2; pass += 1u) {
    uint32_t begin = pass == 0 ? square : 1u;
    uint32_t end = pass == 0 ? max_columns : square;
    for (uint32_t columns = begin; columns <= end; columns += 1u) {
      uint64_t rows_wide = (uint64_t)atlas->glyph_count + columns - 1u;
      uint64_t rows = rows_wide / columns;
      uint64_t width_wide = (uint64_t)columns * atlas->coverage_width;
      uint64_t height_wide = rows * atlas->coverage_height;
      uint32_t width = width_wide <= UINT32_MAX
        ? next_power_of_two((uint32_t)width_wide)
        : 0;
      uint32_t height = height_wide <= UINT32_MAX
        ? next_power_of_two((uint32_t)height_wide)
        : 0;
      if (width != 0 && height != 0 && width <= WII_TEX_MAX && height <= WII_TEX_MAX) {
        *out_columns = columns;
        *out_width = width;
        *out_height = height;
        return true;
      }
      if (columns == end) break;
    }
  }
  return false;
}

static bool upload_font(
  PocketWiiFont *entry,
  const PocketFontAtlas *atlas
) {
  uint32_t columns = 0;
  uint32_t texture_width = 0;
  uint32_t texture_height = 0;
  if (!font_grid(atlas, &columns, &texture_width, &texture_height) ||
      !allocate_texture(&entry->texture, texture_width, texture_height, GX_TF_IA8)) {
    return false;
  }
  size_t glyph_stride = (size_t)atlas->coverage_width * atlas->coverage_height;
  bool valid = true;
  for (uint32_t glyph = 0; glyph < atlas->glyph_count; glyph += 1u) {
    size_t source_offset = (size_t)glyph * glyph_stride;
    if (glyph_stride > atlas->coverage_length ||
        source_offset > atlas->coverage_length - glyph_stride) {
      valid = false;
      break;
    }
    uint32_t base_x = (glyph % columns) * atlas->coverage_width;
    uint32_t base_y = (glyph / columns) * atlas->coverage_height;
    for (uint32_t y = 0; y < atlas->coverage_height; y += 1u) {
      for (uint32_t x = 0; x < atlas->coverage_width; x += 1u) {
        write_ia8(
          &entry->texture,
          base_x + x,
          base_y + y,
          255,
          atlas->coverage[source_offset + (size_t)y * atlas->coverage_width + x]
        );
      }
    }
  }
  if (!valid) {
    release_texture(&entry->texture);
    return false;
  }
  commit_texture(&entry->texture, true);
  entry->coverage = atlas->coverage;
  entry->glyph_count = atlas->glyph_count;
  entry->cell_width = atlas->cell_width;
  entry->cell_height = atlas->cell_height;
  entry->coverage_width = atlas->coverage_width;
  entry->coverage_height = atlas->coverage_height;
  entry->columns = columns;
  return true;
}

static bool ensure_capacity(void **storage, size_t *capacity, size_t count, size_t element_size) {
  if (count <= *capacity) return true;
  if (count > SIZE_MAX / element_size) {
    set_error("Wii GX registry is too large");
    return false;
  }
  void *grown = realloc(*storage, count * element_size);
  if (grown == NULL) {
    set_error("Wii GX registry allocation failed");
    return false;
  }
  memset((uint8_t *)grown + *capacity * element_size, 0,
         (count - *capacity) * element_size);
  *storage = grown;
  *capacity = count;
  return true;
}

static bool same_image(const PocketWiiImage *entry, const PocketTexture *source) {
  return entry->texture.live && entry->handle == source->handle &&
         entry->revision == source->revision &&
         entry->source_width == source->width &&
         entry->source_height == source->height &&
         entry->source_psm == source->pixel_storage &&
         entry->source_linear == source->linear;
}

static bool same_font(
  const PocketWiiFont *entry,
  const PocketFontAtlas *atlas,
  uint64_t raster_revision
) {
  return entry->texture.live && entry->coverage == atlas->coverage &&
         entry->raster_revision == raster_revision &&
         entry->glyph_count == atlas->glyph_count &&
         entry->cell_width == atlas->cell_width &&
         entry->cell_height == atlas->cell_height &&
         entry->coverage_width == atlas->coverage_width &&
         entry->coverage_height == atlas->coverage_height;
}

static bool sync_resources(void) {
  size_t texture_slots = ui_texture_slot_count();
  if (!ensure_capacity((void **)&images, &image_capacity, texture_slots, sizeof *images)) {
    return false;
  }
  for (size_t slot = 0; slot < image_capacity; slot += 1u) {
    PocketWiiImage *entry = &images[slot];
    PocketTexture source;
    if (slot >= texture_slots || !ui_texture_at((uint32_t)slot, &source)) {
      release_texture(&entry->texture);
      entry->handle = -1;
      entry->revision = 0;
      continue;
    }
    if (same_image(entry, &source)) continue;
    release_texture(&entry->texture);
    entry->handle = -1;
    entry->revision = 0;
    uint32_t width = next_power_of_two(source.width);
    uint32_t height = next_power_of_two(source.height);
    if (source.width == 0 || source.height == 0 || width == 0 || height == 0 ||
        !upload_image(&entry->texture, &source, width, height)) {
      set_error("Wii GX image upload failed");
      return false;
    }
    entry->handle = source.handle;
    entry->revision = source.revision;
    entry->source_width = source.width;
    entry->source_height = source.height;
    entry->source_psm = source.pixel_storage;
    entry->source_linear = source.linear;
    entry->u_scale = (float)source.width / (float)width;
    entry->v_scale = (float)source.height / (float)height;
  }

  size_t font_slots = ui_font_slot_count();
  uint64_t raster_revision = ui_raster_revision();
  if (!ensure_capacity((void **)&fonts, &font_capacity, font_slots, sizeof *fonts)) {
    return false;
  }
  for (size_t slot = 0; slot < font_capacity; slot += 1u) {
    PocketWiiFont *entry = &fonts[slot];
    PocketFontAtlas atlas;
    if (slot >= font_slots || !ui_font_atlas((uint32_t)slot, &atlas)) {
      release_texture(&entry->texture);
      memset(entry, 0, sizeof *entry);
      continue;
    }
    if (same_font(entry, &atlas, raster_revision)) continue;
    release_texture(&entry->texture);
    memset(entry, 0, sizeof *entry);
    if (!upload_font(entry, &atlas)) {
      set_error("Wii GX font upload failed");
      return false;
    }
    entry->raster_revision = raster_revision;
  }
  return true;
}

static PocketWiiGpuTexture *image_texture(int32_t handle, float *u_scale, float *v_scale) {
  if (handle < 0 || images == NULL) return NULL;
  size_t slot = (size_t)((uint32_t)handle & ui_texture_slot_mask());
  if (slot >= image_capacity) return NULL;
  PocketWiiImage *entry = &images[slot];
  if (!entry->texture.live || entry->handle != handle) return NULL;
  if (u_scale != NULL) *u_scale = entry->u_scale;
  if (v_scale != NULL) *v_scale = entry->v_scale;
  return &entry->texture;
}

/* ------------------------------------------------------------------------- */
/* GX state and immediate geometry                                            */
/* ------------------------------------------------------------------------- */

static void configure_state(void) {
  GX_ClearVtxDesc();
  GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
  GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
  GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
  GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
  GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
  GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
  /* Feed the direct vertex color through the rasterizer without lighting.
   * Setting this explicitly matters when an embedding app leaves a lit GX
   * channel configured before handing us its EFB. */
  GX_SetChanCtrl(
    GX_COLOR0A0,
    GX_DISABLE,
    GX_SRC_VTX,
    GX_SRC_VTX,
    GX_LIGHTNULL,
    GX_DF_NONE,
    GX_AF_NONE
  );
  GX_SetNumChans(1);
  GX_SetNumTexGens(1);
  GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
  GX_SetNumTevStages(1);
  GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
  GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
  GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
  GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
  GX_SetZMode(GX_DISABLE, GX_ALWAYS, GX_FALSE);
  GX_SetCullMode(GX_CULL_NONE);
  GX_SetColorUpdate(GX_TRUE);
  GX_SetAlphaUpdate(GX_TRUE);
  GX_SetDither(GX_FALSE);
  GX_SetClipMode(GX_CLIP_ENABLE);
}

static bool ensure_renderer(void) {
  if (renderer_initialized) return true;
  renderer_error[0] = '\0';
  if (!allocate_texture(&white_texture, WII_TEX_MIN, WII_TEX_MIN, GX_TF_IA8)) {
    set_error("Wii GX white texture allocation failed");
    return false;
  }
  for (uint32_t y = 0; y < WII_TEX_MIN; y += 1u) {
    for (uint32_t x = 0; x < WII_TEX_MIN; x += 1u) {
      write_ia8(&white_texture, x, y, 255, 255);
    }
  }
  commit_texture(&white_texture, false);
  renderer_initialized = true;
  return true;
}

static void bind_texture(PocketWiiGpuTexture **bound, PocketWiiGpuTexture *texture) {
  if (*bound == texture) return;
  GX_LoadTexObj(&texture->object, GX_TEXMAP0);
  /* A texture load is queued even when the following DrawList op is later
   * rejected. Keep shutdown/resource replacement conservative and wait for
   * that command before freeing its backing memory. */
  renderer_submitted = true;
  *bound = texture;
}

static void emit_vertex(
  float x,
  float y,
  float u,
  float v,
  const uint8_t color[4]
) {
  GX_Position2f32(x, y);
  GX_Color4u8(color[0], color[1], color[2], color[3]);
  GX_TexCoord2f32(u, v);
}

static void begin_geometry(u16 count) {
  GX_Begin(GX_TRIANGLES, GX_VTXFMT0, count);
  renderer_submitted = true;
}

static void draw_quad(
  float x0,
  float y0,
  float x1,
  float y1,
  float u0,
  float v0,
  float u1,
  float v1,
  const uint8_t colors[4][4]
) {
  begin_geometry(6);
  emit_vertex(x0, y0, u0, v0, colors[0]);
  emit_vertex(x1, y0, u1, v0, colors[1]);
  emit_vertex(x1, y1, u1, v1, colors[2]);
  emit_vertex(x0, y0, u0, v0, colors[0]);
  emit_vertex(x1, y1, u1, v1, colors[2]);
  emit_vertex(x0, y1, u0, v1, colors[3]);
  GX_End();
}

static void draw_triangle(
  const float points[3][2],
  const float uvs[3][2],
  const uint8_t colors[3][4]
) {
  begin_geometry(3);
  for (size_t index = 0; index < 3u; index += 1u) {
    emit_vertex(
      points[index][0], points[index][1],
      uvs[index][0], uvs[index][1],
      colors[index]
    );
  }
  GX_End();
}

static int32_t floor_nonnegative(float value) {
  if (value <= 0.0f) return 0;
  if (value >= 2147483000.0f) return 2147483000;
  return (int32_t)value;
}

static int32_t ceil_nonnegative(float value) {
  int32_t floor = floor_nonnegative(value);
  if ((float)floor < value && floor < 2147483000) floor += 1;
  return floor;
}

static void set_scissor(
  PocketWiiClip clip,
  uint32_t logical_width,
  uint32_t logical_height,
  float target_x,
  float target_y,
  float target_width,
  float target_height
) {
  if (clip.width <= 0 || clip.height <= 0 || logical_width == 0 || logical_height == 0) {
    GX_SetScissor(0, 0, 0, 0);
    return;
  }
  float scale_x = target_width / (float)logical_width;
  float scale_y = target_height / (float)logical_height;
  int32_t left = floor_nonnegative(target_x + (float)clip.x * scale_x);
  int32_t top = floor_nonnegative(target_y + (float)clip.y * scale_y);
  int32_t right = ceil_nonnegative(
    target_x + (float)(clip.x + clip.width) * scale_x
  );
  int32_t bottom = ceil_nonnegative(
    target_y + (float)(clip.y + clip.height) * scale_y
  );
  if (right <= left || bottom <= top) {
    GX_SetScissor(0, 0, 0, 0);
    return;
  }
  if (left > 2047) left = 2047;
  if (top > 2047) top = 2047;
  if (right > 2048) right = 2048;
  if (bottom > 2048) bottom = 2048;
  if (right <= left || bottom <= top) {
    GX_SetScissor(0, 0, 0, 0);
    return;
  }
  GX_SetScissor(
    (u32)left,
    (u32)top,
    (u32)(right - left),
    (u32)(bottom - top)
  );
}

static PocketWiiClip intersect_clip(PocketWiiClip a, PocketWiiClip b) {
  int32_t left = a.x > b.x ? a.x : b.x;
  int32_t top = a.y > b.y ? a.y : b.y;
  int32_t right_a = a.x + a.width;
  int32_t right_b = b.x + b.width;
  int32_t bottom_a = a.y + a.height;
  int32_t bottom_b = b.y + b.height;
  int32_t right = right_a < right_b ? right_a : right_b;
  int32_t bottom = bottom_a < bottom_b ? bottom_a : bottom_b;
  PocketWiiClip result = { left, top, right - left, bottom - top };
  if (result.width < 0) result.width = 0;
  if (result.height < 0) result.height = 0;
  return result;
}

static bool valid_target_rect(
  float x,
  float y,
  float width,
  float height
) {
  /* The Wii EFB scissor is top-left based and cannot represent a negative
   * origin. NaN also turns the integer conversion below undefined. */
  return isfinite(x) && isfinite(y) && isfinite(width) && isfinite(height) &&
         isfinite(x + width) && isfinite(y + height) &&
         x >= 0.0f && y >= 0.0f && width > 0.0f && height > 0.0f;
}

/* ------------------------------------------------------------------------- */
/* DrawList walk                                                              */
/* ------------------------------------------------------------------------- */

static bool draw_words(
  const uint32_t *words,
  size_t length,
  uint32_t logical_width,
  uint32_t logical_height,
  float target_x,
  float target_y,
  float target_width,
  float target_height
) {
  if (words == NULL && length != 0) {
    set_error("Wii DrawList pointer is null");
    return false;
  }
  PocketWiiClip full = { 0, 0, (int32_t)logical_width, (int32_t)logical_height };
  PocketWiiClip clip = full;
  PocketWiiClip stack[WII_MAX_CLIP_DEPTH];
  size_t depth = 0;
  PocketWiiGpuTexture *bound = NULL;
  bind_texture(&bound, &white_texture);
  set_scissor(clip, logical_width, logical_height, target_x, target_y,
              target_width, target_height);

  size_t index = 0;
  while (index < length) {
    uint32_t op = words[index];
    switch (op) {
      case DRAW_RECT: {
        if (length - index < 4u) goto truncated;
        float x = word_x(words[index + 1u]);
        float y = word_y(words[index + 1u]);
        float width = word_width(words[index + 2u]);
        float height = word_height(words[index + 2u]);
        uint8_t color[4];
        unpack_color(words[index + 3u], color);
        set_scissor(clip, logical_width, logical_height, target_x, target_y,
                    target_width, target_height);
        if (width > 0.0f && height > 0.0f && color[3] != 0) {
          uint8_t colors[4][4];
          for (size_t corner = 0; corner < 4u; corner += 1u) memcpy(colors[corner], color, 4u);
          draw_quad(x, y, x + width, y + height, 0.0f, 0.0f, 1.0f, 1.0f, colors);
        }
        index += 4u;
        break;
      }
      case DRAW_GRAD_RECT: {
        if (length - index < 6u) goto truncated;
        float x = word_x(words[index + 1u]);
        float y = word_y(words[index + 1u]);
        float width = word_width(words[index + 2u]);
        float height = word_height(words[index + 2u]);
        uint8_t from[4];
        uint8_t to[4];
        unpack_color(words[index + 3u], from);
        unpack_color(words[index + 4u], to);
        uint8_t colors[4][4];
        switch (words[index + 5u]) {
          case GRAD_TO_TOP:
            memcpy(colors[0], to, 4u); memcpy(colors[1], to, 4u);
            memcpy(colors[2], from, 4u); memcpy(colors[3], from, 4u);
            break;
          case GRAD_TO_LEFT:
            memcpy(colors[0], to, 4u); memcpy(colors[1], from, 4u);
            memcpy(colors[2], from, 4u); memcpy(colors[3], to, 4u);
            break;
          case GRAD_TO_RIGHT:
            memcpy(colors[0], from, 4u); memcpy(colors[1], to, 4u);
            memcpy(colors[2], to, 4u); memcpy(colors[3], from, 4u);
            break;
          case GRAD_TO_BOTTOM:
          default:
            memcpy(colors[0], from, 4u); memcpy(colors[1], from, 4u);
            memcpy(colors[2], to, 4u); memcpy(colors[3], to, 4u);
            break;
        }
        set_scissor(clip, logical_width, logical_height, target_x, target_y,
                    target_width, target_height);
        if (width > 0.0f && height > 0.0f) {
          draw_quad(x, y, x + width, y + height, 0.0f, 0.0f, 1.0f, 1.0f, colors);
        }
        index += 6u;
        break;
      }
      case DRAW_GLYPH_RUN: {
        if (length - index < 3u) goto truncated;
        uint32_t header = words[index + 1u];
        size_t count = (size_t)(header >> 16);
        if (count > (length - index - 3u) / 2u) goto truncated;
        size_t slot = (size_t)(header & 0xffu);
        PocketWiiFont *font = slot < font_capacity ? &fonts[slot] : NULL;
        if (font == NULL || !font->texture.live) {
          index += 3u + count * 2u;
          break;
        }
        uint8_t color[4];
        unpack_color(words[index + 2u], color);
        if (color[3] != 0) {
          bind_texture(&bound, &font->texture);
          for (size_t glyph = 0; glyph < count; glyph += 1u) {
            size_t body = index + 3u + glyph * 2u;
            uint32_t gid = words[body + 1u] & 0xffffu;
            if (gid >= font->glyph_count) continue;
            uint32_t column = gid % font->columns;
            uint32_t row = gid / font->columns;
            float u0 = (float)(column * font->coverage_width) /
                       (float)font->texture.width;
            float v0 = (float)(row * font->coverage_height) /
                       (float)font->texture.height;
            float u1 = (float)((column + 1u) * font->coverage_width) /
                       (float)font->texture.width;
            float v1 = (float)((row + 1u) * font->coverage_height) /
                       (float)font->texture.height;
            float x = word_x(words[body]);
            float y = word_y(words[body]);
            uint8_t colors[4][4];
            for (size_t corner = 0; corner < 4u; corner += 1u) memcpy(colors[corner], color, 4u);
            set_scissor(clip, logical_width, logical_height, target_x, target_y,
                        target_width, target_height);
            draw_quad(
              x, y,
              x + (float)font->cell_width,
              y + (float)font->cell_height,
              u0, v0, u1, v1, colors
            );
          }
        }
        index += 3u + count * 2u;
        break;
      }
      case DRAW_TEX_QUAD: {
        if (length - index < 9u) goto truncated;
        float u_scale = 1.0f;
        float v_scale = 1.0f;
        PocketWiiGpuTexture *texture = image_texture((int32_t)words[index + 1u],
                                                       &u_scale, &v_scale);
        if (texture != NULL) {
          float x = word_x(words[index + 2u]);
          float y = word_y(words[index + 2u]);
          float width = word_width(words[index + 3u]);
          float height = word_height(words[index + 3u]);
          uint8_t color[4];
          unpack_color(words[index + 8u], color);
          if (width > 0.0f && height > 0.0f && color[3] != 0) {
            bind_texture(&bound, texture);
            uint8_t colors[4][4];
            for (size_t corner = 0; corner < 4u; corner += 1u) memcpy(colors[corner], color, 4u);
            set_scissor(clip, logical_width, logical_height, target_x, target_y,
                        target_width, target_height);
            draw_quad(
              x, y, x + width, y + height,
              word_float(words[index + 4u]) * u_scale,
              word_float(words[index + 5u]) * v_scale,
              word_float(words[index + 6u]) * u_scale,
              word_float(words[index + 7u]) * v_scale,
              colors
            );
          }
        }
        index += 9u;
        break;
      }
      case DRAW_TEX_TRI: {
        if (length - index < 12u) goto truncated;
        float u_scale = 1.0f;
        float v_scale = 1.0f;
        PocketWiiGpuTexture *texture = image_texture(
          (int32_t)words[index + 1u],
          &u_scale,
          &v_scale
        );
        if (texture != NULL) {
          bind_texture(&bound, texture);
          float points[3][2];
          float uvs[3][2];
          uint8_t color[4];
          uint8_t colors[3][4];
          unpack_color(words[index + 11u], color);
          for (size_t corner = 0; corner < 3u; corner += 1u) {
            size_t offset = index + 2u + corner * 3u;
            points[corner][0] = word_x(words[offset]);
            points[corner][1] = word_y(words[offset]);
            uvs[corner][0] = word_float(words[offset + 1u]) * u_scale;
            uvs[corner][1] = word_float(words[offset + 2u]) * v_scale;
            memcpy(colors[corner], color, 4u);
          }
          if (color[3] != 0) {
            set_scissor(clip, logical_width, logical_height, target_x, target_y,
                        target_width, target_height);
            draw_triangle(points, uvs, colors);
          }
        }
        index += 12u;
        break;
      }
      case DRAW_TRI: {
        if (length - index < 7u) goto truncated;
        float points[3][2];
        float uvs[3][2] = {{ 0.0f, 0.0f }, { 0.0f, 0.0f }, { 0.0f, 0.0f }};
        uint8_t colors[3][4];
        for (size_t corner = 0; corner < 3u; corner += 1u) {
          points[corner][0] = word_x(words[index + 1u + corner]);
          points[corner][1] = word_y(words[index + 1u + corner]);
          unpack_color(words[index + 4u + corner], colors[corner]);
        }
        set_scissor(clip, logical_width, logical_height, target_x, target_y,
                    target_width, target_height);
        if (colors[0][3] != 0 || colors[1][3] != 0 || colors[2][3] != 0) {
          bind_texture(&bound, &white_texture);
          draw_triangle(points, uvs, colors);
        }
        index += 7u;
        break;
      }
      case DRAW_SCISSOR: {
        if (length - index < 3u) goto truncated;
        if (depth >= WII_MAX_CLIP_DEPTH) {
          set_error("Wii DrawList scissor stack overflow");
          return false;
        }
        stack[depth++] = clip;
        PocketWiiClip requested = {
          (int32_t)word_x(words[index + 1u]),
          (int32_t)word_y(words[index + 1u]),
          (int32_t)word_width(words[index + 2u]),
          (int32_t)word_height(words[index + 2u])
        };
        clip = intersect_clip(clip, requested);
        index += 3u;
        break;
      }
      case DRAW_SCISSOR_POP:
        if (depth == 0) {
          set_error("Wii DrawList scissor stack underflow");
          return false;
        }
        clip = stack[--depth];
        index += 1u;
        break;
      case DRAW_TEXT_RUN:
      case DRAW_SURFACE_QUAD:
        /* Wii's wii-dev profile uses baked atlas text and has no compositor
         * surface provider. Refuse an instruction that this backend cannot
         * render rather than silently changing the guest's pixels. */
        set_error("Wii DrawList contains an unsupported native operation");
        return false;
      default:
        set_error("Wii DrawList contains an unknown operation");
        return false;
    }
  }
  if (depth != 0) {
    set_error("Wii DrawList scissor stack is unbalanced");
    return false;
  }
  return true;

truncated:
  set_error("Wii DrawList is truncated");
  return false;
}

/* ------------------------------------------------------------------------- */
/* Public-private renderer entry points                                      */
/* ------------------------------------------------------------------------- */

bool pocket_wii_gx_draw(
  const uint32_t *words,
  size_t length,
  uint32_t logical_width,
  uint32_t logical_height,
  float target_x,
  float target_y,
  float target_width,
  float target_height
) {
  if (logical_width == 0 || logical_height == 0 ||
      logical_width > 32767u || logical_height > 32767u ||
      !valid_target_rect(target_x, target_y, target_width, target_height)) {
    set_error("Wii GX draw rectangle is invalid");
    return false;
  }
  if (renderer_submitted) {
    /* Resource replacement and the next draw must not race the previous GX
     * command list's texture reads. */
    GX_DrawDone();
    renderer_submitted = false;
  }
  if (!ensure_renderer() || !sync_resources()) return false;

  Mtx44 projection;
  guOrtho(
    projection,
    0.0f,
    (f32)logical_height,
    0.0f,
    (f32)logical_width,
    0.0f,
    1.0f
  );
  Mtx modelview;
  guMtxIdentity(modelview);
  GX_LoadPosMtxImm(modelview, GX_PNMTX0);
  GX_SetCurrentMtx(GX_PNMTX0);
  GX_LoadProjectionMtx(projection, GX_ORTHOGRAPHIC);
  GX_SetViewport(target_x, target_y, target_width, target_height, 0.0f, 1.0f);
  configure_state();
  return draw_words(
    words,
    length,
    logical_width,
    logical_height,
    target_x,
    target_y,
    target_width,
    target_height
  );
}

void pocket_wii_gx_shutdown(void) {
  if (!renderer_initialized) return;
  if (renderer_submitted) {
    GX_DrawDone();
    renderer_submitted = false;
  }
  for (size_t index = 0; index < image_capacity; index += 1u) {
    release_texture(&images[index].texture);
  }
  for (size_t index = 0; index < font_capacity; index += 1u) {
    release_texture(&fonts[index].texture);
  }
  free(images);
  free(fonts);
  images = NULL;
  fonts = NULL;
  image_capacity = 0;
  font_capacity = 0;
  release_texture(&white_texture);
  renderer_initialized = false;
  renderer_error[0] = '\0';
}
