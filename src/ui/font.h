// Font atlas (baked through Nuklear's stb_truetype path, default ProggyClean font)
// shared by the Nuklear UI, the HUD and world-space text.
#ifndef DG_FONT_H
#define DG_FONT_H

#include "core/base.h"
#include "core/dgmath.h"
#include "render/render.h"

typedef enum FontSize { FONT_SMALL, FONT_UI, FONT_LARGE, FONT_HUGE, FONT_COUNT } FontSize;

struct nk_user_font;
struct nk_font_atlas;

bool font_init(float ui_scale);
void font_shutdown(void);
const struct nk_user_font *font_nk(FontSize f);
TextureId font_atlas_texture(void);
vec2 font_white_uv(void);
float font_line_height(FontSize f);
float font_text_width(FontSize f, const char *text);

// Emits glyph quads for a 2D string; returns advance. Used by the HUD batcher.
typedef void (*FontQuadFn)(void *user, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1);
float font_layout_2d(FontSize f, float x, float y, const char *text, FontQuadFn fn, void *user);

// World-space text: origin is the left baseline-ish top corner; right/up are unit axes; height in meters.
void font_text3d(vec3 origin, vec3 right, vec3 up, float height, u32 color, const char *text, bool centered);
void font_text3d_billboard(vec3 pos, vec3 cam_right, vec3 cam_up, float height, u32 color, const char *text);

#endif
