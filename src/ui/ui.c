#include "ui.h"
#include "core/engine.h"
#include "core/log.h"
#include "core/cvar.h"
#include "render/render.h"
#include "menus.h"
#include "nuklear_config.h"
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

#define HUD_MAX_VERTS 65536
#define HUD_MAX_INDICES 131072

typedef struct UiSystem {
    struct nk_context ctx;
    struct nk_buffer cmds;
    struct nk_buffer vbuf, ibuf;
    UiVertex *hud_verts;
    u32 hud_vcount;
    u32 *hud_indices;
    u32 hud_icount;
    float scale;
    Cvar *ui_scale;
    // The second window (ui_window2_*).
    struct nk_context ctx2;
    SDL_Window *win2;
    bool close2;
    u32 bg2;
} UiSystem;

#define U (g_engine.ui)

static void apply_style(struct nk_context *ctx) {
    struct nk_color t[NK_COLOR_COUNT];
    t[NK_COLOR_TEXT] = nk_rgba(220, 220, 215, 255);
    t[NK_COLOR_WINDOW] = nk_rgba(24, 26, 28, 235);
    t[NK_COLOR_HEADER] = nk_rgba(40, 42, 45, 255);
    t[NK_COLOR_BORDER] = nk_rgba(70, 72, 75, 255);
    t[NK_COLOR_BUTTON] = nk_rgba(48, 50, 54, 255);
    t[NK_COLOR_BUTTON_HOVER] = nk_rgba(80, 60, 40, 255);
    t[NK_COLOR_BUTTON_ACTIVE] = nk_rgba(190, 110, 40, 255);
    t[NK_COLOR_TOGGLE] = nk_rgba(55, 58, 62, 255);
    t[NK_COLOR_TOGGLE_HOVER] = nk_rgba(80, 82, 86, 255);
    t[NK_COLOR_TOGGLE_CURSOR] = nk_rgba(210, 120, 40, 255);
    t[NK_COLOR_SELECT] = nk_rgba(48, 50, 54, 255);
    t[NK_COLOR_SELECT_ACTIVE] = nk_rgba(160, 95, 35, 255);
    t[NK_COLOR_SLIDER] = nk_rgba(48, 50, 54, 255);
    t[NK_COLOR_SLIDER_CURSOR] = nk_rgba(200, 115, 40, 255);
    t[NK_COLOR_SLIDER_CURSOR_HOVER] = nk_rgba(230, 140, 60, 255);
    t[NK_COLOR_SLIDER_CURSOR_ACTIVE] = nk_rgba(250, 160, 70, 255);
    t[NK_COLOR_PROPERTY] = nk_rgba(40, 42, 45, 255);
    t[NK_COLOR_EDIT] = nk_rgba(18, 18, 20, 255);
    t[NK_COLOR_EDIT_CURSOR] = nk_rgba(230, 230, 230, 255);
    t[NK_COLOR_COMBO] = nk_rgba(40, 42, 45, 255);
    t[NK_COLOR_CHART] = nk_rgba(40, 42, 45, 255);
    t[NK_COLOR_CHART_COLOR] = nk_rgba(200, 115, 40, 255);
    t[NK_COLOR_CHART_COLOR_HIGHLIGHT] = nk_rgba(255, 0, 0, 255);
    t[NK_COLOR_SCROLLBAR] = nk_rgba(30, 32, 34, 255);
    t[NK_COLOR_SCROLLBAR_CURSOR] = nk_rgba(70, 72, 76, 255);
    t[NK_COLOR_SCROLLBAR_CURSOR_HOVER] = nk_rgba(90, 92, 96, 255);
    t[NK_COLOR_SCROLLBAR_CURSOR_ACTIVE] = nk_rgba(110, 112, 116, 255);
    t[NK_COLOR_TAB_HEADER] = nk_rgba(48, 50, 54, 255);
    t[NK_COLOR_KNOB] = nk_rgba(48, 50, 54, 255);
    t[NK_COLOR_KNOB_CURSOR] = nk_rgba(200, 115, 40, 255);
    t[NK_COLOR_KNOB_CURSOR_HOVER] = nk_rgba(230, 140, 60, 255);
    t[NK_COLOR_KNOB_CURSOR_ACTIVE] = nk_rgba(250, 160, 70, 255);
    nk_style_from_table(ctx, t);
    ctx->style.window.padding = nk_vec2(10 * U->scale, 8 * U->scale);
    ctx->style.window.spacing = nk_vec2(6 * U->scale, 6 * U->scale);
    ctx->style.button.rounding = 2;
    ctx->style.window.rounding = 2;
}

bool ui_init(void) {
    U = (UiSystem *)calloc(1, sizeof(UiSystem));
    U->ui_scale = cvar_reg_float("ui_scale", 0.0f, 0.0f, 3.0f, CVAR_ARCHIVE | CVAR_EDITABLE, "UI scale (0 = auto from resolution, needs restart)");
    float scale = U->ui_scale->f;
    if (scale <= 0.01f) scale = clampf((float)render_height() / 1080.0f, 0.75f, 2.5f);
    U->scale = scale;
    if (!font_init(scale)) return false;
    nk_init_default(&U->ctx, font_nk(FONT_UI));
    nk_buffer_init_default(&U->cmds);
    nk_buffer_init_default(&U->vbuf);
    nk_buffer_init_default(&U->ibuf);
    U->hud_verts = (UiVertex *)malloc(sizeof(UiVertex) * HUD_MAX_VERTS);
    U->hud_indices = (u32 *)malloc(sizeof(u32) * HUD_MAX_INDICES);
    apply_style(&U->ctx);
    return true;
}

void ui_shutdown(void) {
    if (!U) return;
    ui_window2_close();
    nk_buffer_free(&U->cmds);
    nk_buffer_free(&U->vbuf);
    nk_buffer_free(&U->ibuf);
    nk_free(&U->ctx);
    font_shutdown();
    free(U->hud_verts);
    free(U->hud_indices);
    free(U);
    U = NULL;
}

struct nk_context *ui_nk(void) { return &U->ctx; }
float ui_width(void) { return (float)render_width(); }
float ui_height(void) { return (float)render_height(); }
float ui_scale(void) { return U->scale; }
bool ui_mouse_over_window(void) { return nk_window_is_any_hovered(&U->ctx) != 0; }

void ui_set_text_input(bool on) {
    if (!g_engine.window) return;
    if (on) SDL_StartTextInput(g_engine.window);
    else SDL_StopTextInput(g_engine.window);
}

void ui_input_begin(void) {
    nk_input_begin(&U->ctx);
    if (U->win2) nk_input_begin(&U->ctx2);
}
void ui_input_end(void) {
    nk_input_end(&U->ctx);
    if (U->win2) nk_input_end(&U->ctx2);
}

static float pixel_density(void) {
    float d = g_engine.window ? SDL_GetWindowPixelDensity(g_engine.window) : 1.0f;
    return d > 0 ? d : 1.0f;
}

static void nk_feed(struct nk_context *ctx, const SDL_Event *ev, float pd);

void ui_handle_event(const SDL_Event *ev) { nk_feed(&U->ctx, ev, pixel_density()); }

static void nk_feed(struct nk_context *ctx, const SDL_Event *ev, float pd) {
    switch (ev->type) {
    case SDL_EVENT_MOUSE_MOTION:
        nk_input_motion(ctx, (int)(ev->motion.x * pd), (int)(ev->motion.y * pd));
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        bool down = ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        int x = (int)(ev->button.x * pd), y = (int)(ev->button.y * pd);
        enum nk_buttons b = NK_BUTTON_LEFT;
        if (ev->button.button == SDL_BUTTON_RIGHT) b = NK_BUTTON_RIGHT;
        else if (ev->button.button == SDL_BUTTON_MIDDLE) b = NK_BUTTON_MIDDLE;
        if (b == NK_BUTTON_LEFT && down && ev->button.clicks == 2) nk_input_button(ctx, NK_BUTTON_DOUBLE, x, y, true);
        else if (b == NK_BUTTON_LEFT && !down) nk_input_button(ctx, NK_BUTTON_DOUBLE, x, y, false);
        nk_input_button(ctx, b, x, y, down);
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        nk_input_scroll(ctx, nk_vec2(ev->wheel.x, ev->wheel.y));
        break;
    case SDL_EVENT_TEXT_INPUT: {
        const char *t = ev->text.text;
        while (*t) {
            nk_rune r;
            int n = nk_utf_decode(t, &r, 4);
            if (n <= 0) break;
            if (r != '`' && r != '~') nk_input_unicode(ctx, r);
            t += n;
        }
        break;
    }
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        bool down = ev->type == SDL_EVENT_KEY_DOWN;
        bool ctrl = (ev->key.mod & SDL_KMOD_CTRL) != 0;
        switch (ev->key.key) {
        case SDLK_BACKSPACE: nk_input_key(ctx, NK_KEY_BACKSPACE, down); break;
        case SDLK_DELETE: nk_input_key(ctx, NK_KEY_DEL, down); break;
        case SDLK_RETURN: case SDLK_KP_ENTER: nk_input_key(ctx, NK_KEY_ENTER, down); break;
        case SDLK_TAB: nk_input_key(ctx, NK_KEY_TAB, down); break;
        case SDLK_LEFT: nk_input_key(ctx, ctrl ? NK_KEY_TEXT_WORD_LEFT : NK_KEY_LEFT, down); break;
        case SDLK_RIGHT: nk_input_key(ctx, ctrl ? NK_KEY_TEXT_WORD_RIGHT : NK_KEY_RIGHT, down); break;
        case SDLK_UP: nk_input_key(ctx, NK_KEY_UP, down); break;
        case SDLK_DOWN: nk_input_key(ctx, NK_KEY_DOWN, down); break;
        case SDLK_HOME: nk_input_key(ctx, NK_KEY_TEXT_START, down); break;
        case SDLK_END: nk_input_key(ctx, NK_KEY_TEXT_END, down); break;
        case SDLK_A: if (ctrl) nk_input_key(ctx, NK_KEY_TEXT_SELECT_ALL, down); break;
        case SDLK_C: if (ctrl) nk_input_key(ctx, NK_KEY_COPY, down); break;
        case SDLK_V: if (ctrl) nk_input_key(ctx, NK_KEY_PASTE, down); break;
        case SDLK_X: if (ctrl) nk_input_key(ctx, NK_KEY_CUT, down); break;
        case SDLK_Z: if (ctrl) nk_input_key(ctx, NK_KEY_TEXT_UNDO, down); break;
        case SDLK_Y: if (ctrl) nk_input_key(ctx, NK_KEY_TEXT_REDO, down); break;
        default: break;
        }
        break;
    }
    default: break;
    }
}

// ------------------------------------------------------------------ HUD primitives

static void hud_quad(vec2 a, vec2 b, vec2 c, vec2 d, vec2 uva, vec2 uvb, vec2 uvc, vec2 uvd, u32 color) {
    if (U->hud_vcount + 4 > HUD_MAX_VERTS || U->hud_icount + 6 > HUD_MAX_INDICES) return;
    u32 base = U->hud_vcount;
    UiVertex *v = U->hud_verts + base;
    v[0] = (UiVertex){ a, uva, color };
    v[1] = (UiVertex){ b, uvb, color };
    v[2] = (UiVertex){ c, uvc, color };
    v[3] = (UiVertex){ d, uvd, color };
    U->hud_vcount += 4;
    u32 *i = U->hud_indices + U->hud_icount;
    i[0] = base; i[1] = base + 1; i[2] = base + 2; i[3] = base; i[4] = base + 2; i[5] = base + 3;
    U->hud_icount += 6;
}

void ui_rect(float x, float y, float w, float h, u32 color) {
    vec2 uv = font_white_uv();
    hud_quad(v2(x, y), v2(x + w, y), v2(x + w, y + h), v2(x, y + h), uv, uv, uv, uv, color);
}

void ui_rect_outline(float x, float y, float w, float h, float t, u32 color) {
    ui_rect(x, y, w, t, color);
    ui_rect(x, y + h - t, w, t, color);
    ui_rect(x, y + t, t, h - 2 * t, color);
    ui_rect(x + w - t, y + t, t, h - 2 * t, color);
}

void ui_line(float x0, float y0, float x1, float y1, float th, u32 color) {
    vec2 d = v2(x1 - x0, y1 - y0);
    float l = v2_len(d);
    if (l < 1e-4f) return;
    vec2 n = v2(-d.y / l * th * 0.5f, d.x / l * th * 0.5f);
    vec2 uv = font_white_uv();
    hud_quad(v2(x0 + n.x, y0 + n.y), v2(x1 + n.x, y1 + n.y), v2(x1 - n.x, y1 - n.y), v2(x0 - n.x, y0 - n.y), uv, uv, uv, uv, color);
}

typedef struct HudTextCtx { u32 color; } HudTextCtx;
static void hud_glyph(void *user, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1) {
    HudTextCtx *c = (HudTextCtx *)user;
    hud_quad(v2(x0, y0), v2(x1, y0), v2(x1, y1), v2(x0, y1), v2(u0, v0), v2(u1, v0), v2(u1, v1), v2(u0, v1), c->color);
}

float ui_text(float x, float y, FontSize f, u32 color, int align, const char *text) {
    float w = font_text_width(f, text);
    if (align == UI_ALIGN_CENTER) x -= w * 0.5f;
    else if (align == UI_ALIGN_RIGHT) x -= w;
    HudTextCtx c = { color };
    font_layout_2d(f, floorf(x), floorf(y), text, hud_glyph, &c);
    return w;
}

float ui_text_shadow(float x, float y, FontSize f, u32 color, int align, const char *text) {
    u32 a = color >> 24;
    ui_text(x + 1.5f * U->scale, y + 1.5f * U->scale, f, UI_RGBA(0, 0, 0, a * 3 / 4), align, text);
    return ui_text(x, y, f, color, align, text);
}

float ui_textf(float x, float y, FontSize f, u32 color, int align, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return ui_text_shadow(x, y, f, color, align, buf);
}

// ------------------------------------------------------------------ flush

// The second window: its background image (ui_window2_set_background) as a full-window quad under
// the Nuklear output (a Nuklear window there would count as UI under the mouse).
static void submit_window2(const UiVertex *verts, u32 vcount, const u32 *indices, u32 icount, const UiDrawCmd *cmds, u32 ccount) {
    if (!U->bg2) { render_window2_ui(verts, vcount, indices, icount, cmds, ccount); return; }
    UiVertex *v = (UiVertex *)malloc(sizeof(UiVertex) * (vcount + 4));
    u32 *ix = (u32 *)malloc(sizeof(u32) * (icount + 6));
    UiDrawCmd *c = (UiDrawCmd *)malloc(sizeof(UiDrawCmd) * (ccount + 1));
    if (!v || !ix || !c) { free(v); free(ix); free(c); return; }
    float w = ui_window2_width(), h = ui_window2_height();
    v[0] = (UiVertex){ v2(0, 0), v2(0, 0), 0xFFFFFFFFu };
    v[1] = (UiVertex){ v2(w, 0), v2(1, 0), 0xFFFFFFFFu };
    v[2] = (UiVertex){ v2(w, h), v2(1, 1), 0xFFFFFFFFu };
    v[3] = (UiVertex){ v2(0, h), v2(0, 1), 0xFFFFFFFFu };
    static const u32 q[6] = { 0, 1, 2, 0, 2, 3 };
    memcpy(ix, q, sizeof(q));
    if (vcount) memcpy(v + 4, verts, sizeof(UiVertex) * vcount);
    for (u32 i = 0; i < icount; i++) ix[6 + i] = indices[i] + 4;
    c[0] = (UiDrawCmd){ 0, 6, (TextureId)U->bg2, 0, 0, (int)w, (int)h };
    for (u32 i = 0; i < ccount; i++) {
        c[1 + i] = cmds[i];
        c[1 + i].index_offset += 6;
    }
    render_window2_ui(v, vcount + 4, ix, icount + 6, c, ccount + 1);
    free(v);
    free(ix);
    free(c);
}

static void nk_submit(struct nk_context *ctx, bool window2);

void ui_render(void) {
    int sw = render_width(), sh = render_height();
    if (U->hud_icount) {
        UiDrawCmd cmd = { 0, U->hud_icount, font_atlas_texture(), 0, 0, sw, sh };
        render_ui(U->hud_verts, U->hud_vcount, U->hud_indices, U->hud_icount, &cmd, 1);
    }
    U->hud_vcount = U->hud_icount = 0;
    menus_frame_end();   // the game UI (src/ui/gui) between the HUD batch and Nuklear

    nk_submit(&U->ctx, false);
    nk_clear(&U->ctx);
    if (U->win2) {
        nk_submit(&U->ctx2, true);
        nk_clear(&U->ctx2);
        U->bg2 = 0;
    }
}

// Converts a Nuklear context's output and hands it to the renderer: the main UI or the second window.
static void nk_submit(struct nk_context *ctx, bool window2) {
    static const struct nk_draw_vertex_layout_element layout[] = {
        { NK_VERTEX_POSITION, NK_FORMAT_FLOAT, offsetof(UiVertex, pos) },
        { NK_VERTEX_TEXCOORD, NK_FORMAT_FLOAT, offsetof(UiVertex, uv) },
        { NK_VERTEX_COLOR, NK_FORMAT_R8G8B8A8, offsetof(UiVertex, color) },
        { NK_VERTEX_LAYOUT_END }
    };
    struct nk_convert_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.vertex_layout = layout;
    cfg.vertex_size = sizeof(UiVertex);
    cfg.vertex_alignment = 4;
    cfg.circle_segment_count = 22;
    cfg.curve_segment_count = 22;
    cfg.arc_segment_count = 22;
    cfg.global_alpha = 1.0f;
    cfg.shape_AA = NK_ANTI_ALIASING_ON;
    cfg.line_AA = NK_ANTI_ALIASING_ON;
    vec2 wuv = font_white_uv();
    cfg.tex_null.texture = nk_handle_id((int)font_atlas_texture());
    cfg.tex_null.uv = nk_vec2(wuv.x, wuv.y);

    nk_buffer_clear(&U->cmds);
    nk_buffer_clear(&U->vbuf);
    nk_buffer_clear(&U->ibuf);
    nk_convert(ctx, &U->cmds, &U->vbuf, &U->ibuf, &cfg);
    u32 vcount = (u32)(U->vbuf.needed / sizeof(UiVertex));
    u32 icount = (u32)(U->ibuf.needed / sizeof(u32));
    if (vcount && icount) {
        UiDrawCmd cmds[1024];
        u32 ccount = 0, offset = 0;
        const struct nk_draw_command *c;
        nk_draw_foreach(c, ctx, &U->cmds) {
            if (!c->elem_count) continue;
            if (ccount < 1024) {
                UiDrawCmd *d = &cmds[ccount++];
                memset(d, 0, sizeof(*d));
                d->index_offset = offset;
                d->index_count = c->elem_count;
                d->texture = (TextureId)c->texture.id;
                d->clip_x = (int)c->clip_rect.x;
                d->clip_y = (int)c->clip_rect.y;
                d->clip_w = (int)c->clip_rect.w;
                d->clip_h = (int)c->clip_rect.h;
            }
            offset += c->elem_count;
        }
        if (window2) submit_window2((const UiVertex *)nk_buffer_memory_const(&U->vbuf), vcount, (const u32 *)nk_buffer_memory_const(&U->ibuf), icount, cmds, ccount);
        else render_ui((const UiVertex *)nk_buffer_memory_const(&U->vbuf), vcount, (const u32 *)nk_buffer_memory_const(&U->ibuf), icount, cmds, ccount);
    }
}

// ------------------------------------------------------------------ second window

bool ui_window2_open(const char *title) {
    if (U->win2) return true;
    // 1600 x 900 (or 90 % of the game window's display), centred on that display.
    SDL_DisplayID disp = SDL_GetDisplayForWindow(g_engine.window);
    SDL_Rect usable = { 0, 0, 1920, 1080 };
    SDL_GetDisplayUsableBounds(disp, &usable);
    int w = DG_MIN(1600, usable.w * 9 / 10), h = DG_MIN(900, usable.h * 9 / 10);
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, w);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, h);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(disp));
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(disp));
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    SDL_Window *win = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (!win) { log_error("ui: second window: %s", SDL_GetError()); return false; }
    if (!render_window2_open(win)) { SDL_DestroyWindow(win); return false; }
    U->win2 = win;
    U->close2 = false;
    U->bg2 = 0;
    nk_init_default(&U->ctx2, font_nk(FONT_UI));
    apply_style(&U->ctx2);
    nk_input_begin(&U->ctx2);   // inside this frame's input (ui_input_end closes it)
    SDL_StartTextInput(win);    // typed values (double-click a field)
    SDL_RaiseWindow(win);
    return true;
}

void ui_window2_close(void) {
    if (!U || !U->win2) return;
    render_window2_close();
    SDL_StopTextInput(U->win2);
    SDL_DestroyWindow(U->win2);
    U->win2 = NULL;
    nk_free(&U->ctx2);
    if (g_engine.window) SDL_RaiseWindow(g_engine.window);
}

bool ui_window2_is_open(void) { return U && U->win2; }
struct nk_context *ui_window2_nk(void) { return U->win2 ? &U->ctx2 : &U->ctx; }

bool ui_window2_handle_event(const SDL_Event *ev) {
    if (!U || !U->win2 || SDL_GetWindowFromEvent(ev) != U->win2) return false;
    if (ev->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) U->close2 = true;
    float pd = SDL_GetWindowPixelDensity(U->win2);
    nk_feed(&U->ctx2, ev, pd > 0 ? pd : 1.0f);
    return true;
}

float ui_window2_width(void) {
    int w = 0, h = 0;
    if (U->win2) SDL_GetWindowSizeInPixels(U->win2, &w, &h);
    return (float)w;
}

float ui_window2_height(void) {
    int w = 0, h = 0;
    if (U->win2) SDL_GetWindowSizeInPixels(U->win2, &w, &h);
    return (float)h;
}

bool ui_window2_focused(void) { return U->win2 && SDL_GetKeyboardFocus() == U->win2; }

bool ui_window2_close_requested(void) {
    bool r = U->close2;
    U->close2 = false;
    return r;
}

void ui_window2_set_background(u32 texture) { U->bg2 = texture; }
