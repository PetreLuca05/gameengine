// UI core: Nuklear context (menus, console, inventory) + a small immediate 2D layer for the HUD.
// Coordinates are in swapchain pixels, origin top-left.
#ifndef DG_UI_H
#define DG_UI_H

#include "core/base.h"
#include "font.h"

union SDL_Event;
struct nk_context;

enum { UI_ALIGN_LEFT = 0, UI_ALIGN_CENTER = 1, UI_ALIGN_RIGHT = 2 };

bool ui_init(void);
void ui_shutdown(void);
void ui_input_begin(void);
void ui_handle_event(const union SDL_Event *ev);
void ui_input_end(void);
struct nk_context *ui_nk(void);
bool ui_mouse_over_window(void);
void ui_set_text_input(bool on);

void ui_rect(float x, float y, float w, float h, u32 color);
void ui_rect_outline(float x, float y, float w, float h, float t, u32 color);
void ui_line(float x0, float y0, float x1, float y1, float thickness, u32 color);
float ui_text(float x, float y, FontSize f, u32 color, int align, const char *text);
float ui_textf(float x, float y, FontSize f, u32 color, int align, const char *fmt, ...);
float ui_text_shadow(float x, float y, FontSize f, u32 color, int align, const char *text);

void ui_render(void);  // flushes HUD + Nuklear output to the renderer
float ui_width(void);
float ui_height(void);
float ui_scale(void);

#define UI_RGBA(r, g, b, a) ((u32)(r) | ((u32)(g) << 8) | ((u32)(b) << 16) | ((u32)(a) << 24))

#endif
