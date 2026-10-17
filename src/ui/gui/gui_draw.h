// UI draw list (docs/UI.md "Rendering"): rects, SDF rounded rects / circles, 9-slice shadows,
// images, the blurred backdrop and SDF text (Barlow, three weights, kerning, shadows, small caps)
// in three layers (base, overlay for popups, top for tooltips / drag ghosts). Flushed to the
// renderer once per frame between the HUD batch and Nuklear.
#ifndef DG_GUI_DRAW_H
#define DG_GUI_DRAW_H

#include "core/base.h"
#include "core/dgmath.h"
#include "render/render.h"
#include "gui_layout.h"

typedef enum GuiWeight { GW_REGULAR, GW_MEDIUM, GW_SEMIBOLD, GW_COUNT } GuiWeight;
typedef enum GuiLayer { GL_BASE, GL_OVERLAY, GL_TOP, GL_COUNT } GuiLayer;
enum { GUI_LEFT = 0, GUI_CENTER = 1, GUI_RIGHT = 2 };
#ifndef UI_RGBA
#define UI_RGBA(r, g, b, a) ((u32)(r) | ((u32)(g) << 8) | ((u32)(b) << 16) | ((u32)(a) << 24))
#endif

typedef struct GuiText {
    float size;               // pixels (already scaled)
    int weight;               // GuiWeight
    u32 color;
    int align;
    float tracking;           // letter spacing (em)
    bool caps;                // upper case (small-caps labels: with tracking, smaller size)
    bool shadow;              // soft drop shadow
    float max_w;              // > 0: truncate with an ellipsis
} GuiText;

bool gd_init(void);           // fonts + atlas (needs the theme)
void gd_shutdown(void);
bool gd_reload_fonts(void);
void gd_begin(int w, int h);
void gd_flush(void);
void gd_set_layer(int layer);
int gd_layer(void);
void gd_alpha_push(float a);  // multiplies every colour until popped
void gd_alpha_pop(void);
void gd_clip_push(GuiRect r); // intersected with the current clip
void gd_clip_pop(void);
GuiRect gd_clip(void);

void gd_rect(GuiRect r, u32 c);
void gd_rect_grad(GuiRect r, u32 top, u32 bottom);
void gd_rect_hgrad(GuiRect r, u32 left, u32 right);
void gd_border(GuiRect r, float t, u32 c);
void gd_round(GuiRect r, float radius, u32 c);
void gd_round_border(GuiRect r, float radius, float t, u32 fill, u32 border);
void gd_round_outline(GuiRect r, float radius, float t, u32 c);
void gd_shadow(GuiRect r, float radius, float spread, u32 c);
void gd_circle(float cx, float cy, float radius, u32 c);
void gd_ring(float cx, float cy, float radius, float t, float frac, u32 c);   // frac 0..1 of a circle, clockwise from the top
void gd_line(float x0, float y0, float x1, float y1, float t, u32 c);
void gd_image(GuiRect r, TextureId tex, vec4 uv, u32 tint);
void gd_image_mask(GuiRect r, TextureId tex, vec4 uv, u32 color);   // the image's silhouette (its alpha) in one colour
void gd_backdrop(GuiRect r, u32 tint);   // the blurred scene behind r (falls back to a dark fill)

float gd_text(const GuiText *t, float x, float y, const char *s);   // y = top of the line box; returns width
float gd_text_simple(float x, float y, float size, int weight, u32 c, int align, const char *s);
float gd_text_width(const GuiText *t, const char *s);
float gd_line_height(float size);
// Wrapped paragraph in width w; returns the height used (draw = false: measure only).
float gd_text_wrap(const GuiText *t, float x, float y, float w, const char *s, bool draw);

TextureId gd_atlas(void);
void gd_register_commands(void);
int gd_stats_cmds(void);      // last flush: draw commands (ui_debug)
int gd_stats_verts(void);

#endif
