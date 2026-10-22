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

void ui_render(void);  // flushes HUD + Nuklear output to the renderer (and the second window's)

// Second window (dev tools: the hand pose editor's own window, docs/HANDPOSE.md "Own window"): an OS
// window with its own Nuklear context (same font, style and scale). Its events never reach the game
// or the main UI; coordinates are its own pixels. render.h "Second window" draws it.
bool ui_window2_open(const char *title);          // false: it couldn't be created (logged)
void ui_window2_close(void);
bool ui_window2_is_open(void);
struct nk_context *ui_window2_nk(void);
bool ui_window2_handle_event(const union SDL_Event *ev);   // true: the event is its own (consumed)
float ui_window2_width(void);
float ui_window2_height(void);
bool ui_window2_focused(void);                    // it has the keyboard focus
bool ui_window2_close_requested(void);            // its close button since the last call
void ui_window2_set_background(u32 texture);      // this frame: a TextureId over the whole window under the panels (0 = none)
float ui_width(void);
float ui_height(void);
float ui_scale(void);

#define UI_RGBA(r, g, b, a) ((u32)(r) | ((u32)(g) << 8) | ((u32)(b) << 16) | ((u32)(a) << 24))

#endif
