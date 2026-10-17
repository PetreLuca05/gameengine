#include "gui.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/log.h"
#include "audio/audio.h"
#include "render/render.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define MAX_FOCUSABLE 512
#define MAX_TWEENS 1024
#define MAX_CHARS 32
#define INPUT_STACK 16

typedef struct Focusable { GuiId id; GuiRect r; u32 flags; } Focusable;
typedef struct Tween { GuiId id; float v; double start; u64 frame; bool init; } Tween;

static struct {
    bool init;
    float dt;
    double time;
    u64 frame;
    float scale, w, h;
    Cvar *ui_scale, *ui_debug, *ui_safe;
    // mouse
    vec2 mouse, mouse_prev;
    bool mdown[3], mpressed[3], mreleased[3], dbl;
    float wheel;
    bool shift, ctrl;
    // navigation
    bool nav[NAV_COUNT];
    bool nav_mode;
    float repeat_t[4];
    vec2 stick;
    int chars[MAX_CHARS], char_count, char_read;
    int keys[16], key_count;      // scancodes pressed this frame (gui_key_pressed)
    int backspaces;
    // interaction
    GuiId hover, hover_next;      // the topmost widget under the mouse (resolved at frame end)
    int hover_next_prio;
    GuiId active, focus, focus_next_frame;
    GuiId press_id;               // the widget a mouse press started on
    Focusable foc[MAX_FOCUSABLE];
    int foc_count;
    GuiRect focus_rect;
    bool focus_seen;
    bool input[INPUT_STACK];
    int input_n;
    bool scope_set;
    GuiRect scope;
    GuiId last_hover_sound;
    bool text_mode;               // a text field has focus: letters are text, not shortcuts
    // tooltips
    GuiId tip_id;
    double tip_start;
    char tip[256];
    bool tip_set;
    Tween tw[MAX_TWEENS];
} G;

static const char *find_sound(const char *n) { return n && n[0] ? n : NULL; }

void gui_init(void) {
    memset(&G, 0, sizeof(G));
    G.ui_scale = cvar_find("ui_scale");
    G.ui_debug = cvar_reg_int("ui_debug", 0, 0, 2, 0, "UI debug overlay: 1 widget / layout rects + focus, 2 + draw stats");
    G.ui_safe = cvar_reg_float("ui_safe_area", -1.0f, -1.0f, 0.15f, CVAR_ARCHIVE, "UI safe area margin (fraction of the smaller side, -1 = theme)");
    G.init = true;
}

void gui_shutdown(void) { G.init = false; }

void gui_reload(void) {
    gui_theme_load("assets/ui/theme.json");
    gd_reload_fonts();
}

bool gui_focus_rect(GuiRect *out) {
    if (!G.focus) return false;
    *out = G.focus_rect;
    return true;
}

GuiId gui_id(const char *s) { return dg_hash_str(s) | 1u; }
GuiId gui_idi(const char *s, int i) { return (dg_hash_str(s) ^ dg_hash_u32((u32)i * 2654435761u + 17u)) | 1u; }
float gui_s(float v) { return v * G.scale; }
float gui_scale(void) { return G.scale; }
float gui_w(void) { return G.w; }
float gui_h(void) { return G.h; }
float gui_dt(void) { return G.dt; }
double gui_time(void) { return G.time; }

LayoutParams gui_layout_params(void) {
    const GuiTheme *t = gui_theme();
    LayoutParams p;
    p.w = G.w;
    p.h = G.h;
    p.scale = G.scale;
    p.safe_frac = (G.ui_safe && G.ui_safe->f >= 0.0f) ? G.ui_safe->f : t->safe_frac;
    p.max_aspect = t->max_aspect;
    return p;
}

static float pixel_density(void) {
    float d = g_engine.window ? SDL_GetWindowPixelDensity(g_engine.window) : 1.0f;
    return d > 0 ? d : 1.0f;
}

static void set_nav(int n) { G.nav[n] = true; G.nav_mode = n != NAV_MENU && n != NAV_VIEW ? true : G.nav_mode; }

bool gui_handle_event(const SDL_Event *ev) {
    if (!G.init) return false;
    float pd = pixel_density();
    switch (ev->type) {
    case SDL_EVENT_MOUSE_MOTION:
        G.mouse = v2(ev->motion.x * pd, ev->motion.y * pd);
        if (fabsf(ev->motion.xrel) + fabsf(ev->motion.yrel) > 1.5f) G.nav_mode = false;
        return false;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        int b = ev->button.button == SDL_BUTTON_LEFT ? 0 : (ev->button.button == SDL_BUTTON_RIGHT ? 1 : (ev->button.button == SDL_BUTTON_MIDDLE ? 2 : -1));
        G.mouse = v2(ev->button.x * pd, ev->button.y * pd);
        if (b < 0) return false;
        bool down = ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        if (down) { G.mpressed[b] = true; if (b == 0 && ev->button.clicks >= 2) G.dbl = true; }
        else G.mreleased[b] = true;
        G.mdown[b] = down;
        G.nav_mode = false;
        return false;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        G.wheel += ev->wheel.y;
        return false;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        bool down = ev->type == SDL_EVENT_KEY_DOWN;
        SDL_Keymod m = ev->key.mod;
        G.shift = (m & SDL_KMOD_SHIFT) != 0;
        G.ctrl = (m & SDL_KMOD_CTRL) != 0;
        if (!down) return false;
        if (!ev->key.repeat && G.key_count < 16) G.keys[G.key_count++] = (int)ev->key.scancode;
        switch (ev->key.scancode) {
        case SDL_SCANCODE_UP: set_nav(NAV_UP); break;
        case SDL_SCANCODE_DOWN: set_nav(NAV_DOWN); break;
        case SDL_SCANCODE_LEFT: set_nav(NAV_LEFT); break;
        case SDL_SCANCODE_RIGHT: set_nav(NAV_RIGHT); break;
        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: if (!ev->key.repeat) set_nav(NAV_ACCEPT); break;
        case SDL_SCANCODE_SPACE: if (!ev->key.repeat && !G.text_mode) set_nav(NAV_ACCEPT); break;
        case SDL_SCANCODE_ESCAPE: if (!ev->key.repeat) G.nav[NAV_BACK] = true; break;
        case SDL_SCANCODE_TAB:
            if (ev->key.repeat) break;
            if (G.ctrl) set_nav(G.shift ? NAV_TAB_PREV : NAV_TAB_NEXT);
            else G.nav[NAV_VIEW] = true;
            break;
        case SDL_SCANCODE_I: if (!ev->key.repeat && !G.text_mode) G.nav[NAV_VIEW] = true; break;
        case SDL_SCANCODE_BACKSPACE: G.backspaces++; break;
        case SDL_SCANCODE_E: if (!ev->key.repeat && !G.text_mode) G.nav[NAV_TAB_NEXT] = true; break;
        case SDL_SCANCODE_Q: if (!ev->key.repeat && !G.text_mode) G.nav[NAV_TAB_PREV] = true; break;
        default: break;
        }
        return false;
    }
    case SDL_EVENT_TEXT_INPUT: {
        const char *t = ev->text.text;
        while (*t && G.char_count < MAX_CHARS) {
            unsigned char c = (unsigned char)*t++;
            if (c >= 32 && c < 127) G.chars[G.char_count++] = c;
        }
        return false;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        switch (ev->gbutton.button) {
        case SDL_GAMEPAD_BUTTON_DPAD_UP: set_nav(NAV_UP); break;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: set_nav(NAV_DOWN); break;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: set_nav(NAV_LEFT); break;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: set_nav(NAV_RIGHT); break;
        case SDL_GAMEPAD_BUTTON_SOUTH: set_nav(NAV_ACCEPT); break;
        case SDL_GAMEPAD_BUTTON_EAST: G.nav[NAV_BACK] = true; G.nav_mode = true; break;
        case SDL_GAMEPAD_BUTTON_WEST: set_nav(NAV_ALT); break;
        case SDL_GAMEPAD_BUTTON_NORTH: set_nav(NAV_ALT2); break;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: set_nav(NAV_TAB_NEXT); break;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: set_nav(NAV_TAB_PREV); break;
        case SDL_GAMEPAD_BUTTON_START: G.nav[NAV_MENU] = true; break;
        case SDL_GAMEPAD_BUTTON_BACK: G.nav[NAV_VIEW] = true; break;
        default: break;
        }
        return false;
    default: return false;
    }
}

static void read_stick(void) {
    G.stick = v2(0, 0);
    SDL_Gamepad *pad = NULL;
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    if (ids && n > 0) pad = SDL_GetGamepadFromID(ids[0]);
    SDL_free(ids);
    if (!pad) return;
    float x = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f, y = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
    float m = sqrtf(x * x + y * y);
    if (m < 0.25f) return;
    float k = (m - 0.25f) / 0.75f / m;
    G.stick = v2(x * k, y * k);
    // The stick also navigates (with auto repeat) when it's mostly along one axis.
    int dir = fabsf(x) > fabsf(y) ? (x > 0 ? NAV_RIGHT : NAV_LEFT) : (y > 0 ? NAV_DOWN : NAV_UP);
    for (int d = 0; d < 4; d++) {
        if (d != dir || m < 0.6f) { G.repeat_t[d] = 0; continue; }
        if (G.repeat_t[d] <= 0) { set_nav(d); G.repeat_t[d] = G.repeat_t[d] < 0 ? 0.09f : 0.32f; }
        G.repeat_t[d] -= G.dt;
        if (G.repeat_t[d] <= 0) G.repeat_t[d] = -1;   // next frame repeats
    }
}

void gui_frame_begin(float dt) {
    G.dt = dt;
    G.time += dt;
    G.frame++;
    G.w = (float)render_width();
    G.h = (float)render_height();
    float user = G.ui_scale ? G.ui_scale->f : 0.0f;
    G.scale = gui_layout_scale(G.w, G.h, user);
    gd_begin((int)G.w, (int)G.h);
    read_stick();
    G.foc_count = 0;
    G.focus_seen = false;
    G.input_n = 1;
    G.input[0] = true;
    G.scope_set = false;
    G.hover_next = 0;
    G.hover_next_prio = -1;
    G.tip_set = false;
    if (G.mpressed[0] || G.mpressed[1]) G.press_id = G.hover;
}

// Input edges are cleared after the frame used them.
static void end_input(void) {
    for (int b = 0; b < 3; b++) { G.mpressed[b] = false; G.mreleased[b] = false; }
    G.dbl = false;
    G.wheel = 0;
    memset(G.nav, 0, sizeof(G.nav));
    G.char_count = G.char_read = 0;
    G.key_count = 0;
    G.backspaces = 0;
    G.mouse_prev = G.mouse;
    if (!G.mdown[0] && !G.mdown[1]) G.press_id = 0;
}

vec2 gui_mouse(void) { return G.mouse; }
bool gui_mouse_down(int b) { return b >= 0 && b < 3 && G.mdown[b]; }
bool gui_mouse_pressed(int b) { return b >= 0 && b < 3 && G.mpressed[b]; }
bool gui_mouse_released(int b) { return b >= 0 && b < 3 && G.mreleased[b]; }
bool gui_double_clicked(void) { return G.dbl; }
bool gui_mouse_moved(void) { return G.mouse.x != G.mouse_prev.x || G.mouse.y != G.mouse_prev.y; }
float gui_wheel(void) { return G.wheel; }
void gui_wheel_consume(void) { G.wheel = 0; }
bool gui_shift(void) { return G.shift || (SDL_GetModState() & SDL_KMOD_SHIFT) != 0; }
bool gui_ctrl(void) { return G.ctrl; }
void gui_set_mods(bool shift, bool ctrl) { G.shift = shift; G.ctrl = ctrl; }
bool gui_nav(int n) { return n >= 0 && n < NAV_COUNT && G.nav[n]; }
void gui_nav_consume(int n) { if (n >= 0 && n < NAV_COUNT) G.nav[n] = false; }
bool gui_nav_mode(void) { return G.nav_mode; }
vec2 gui_pad_stick(void) { return G.stick; }
int gui_backspaces(void) { return G.backspaces; }
void gui_set_text_mode(bool on) {
    if (on == G.text_mode) return;
    G.text_mode = on;
    if (g_engine.window) { if (on) SDL_StartTextInput(g_engine.window); else SDL_StopTextInput(g_engine.window); }
}

bool gui_text_mode(void) { return G.text_mode; }

bool gui_key_pressed(int scancode) {
    if (G.text_mode) return false;
    for (int i = 0; i < G.key_count; i++) if (G.keys[i] == scancode) return true;
    return false;
}

bool gui_text_input_char(int *cp) {
    if (G.char_read >= G.char_count) return false;
    *cp = G.chars[G.char_read++];
    return true;
}

void gui_push_input(bool on) { if (G.input_n < INPUT_STACK) { G.input[G.input_n] = G.input[G.input_n - 1] && on; G.input_n++; } }
void gui_pop_input(void) { if (G.input_n > 1) G.input_n--; }
bool gui_input_enabled(void) { return G.input[G.input_n - 1]; }
GuiId gui_focus(void) { return G.focus; }
void gui_set_focus(GuiId id) { G.focus = id; G.focus_seen = true; }
GuiId gui_active(void) { return G.active; }
void gui_set_active(GuiId id) { G.active = id; }
void gui_focus_scope(GuiRect r) { G.scope = r; G.scope_set = true; }

static void hover_candidate(GuiId id, GuiRect r) {
    if (!gui_rect_contains(r, G.mouse.x, G.mouse.y) || !gui_rect_contains(gd_clip(), G.mouse.x, G.mouse.y)) return;
    int prio = gd_layer() * 100000 + G.foc_count;
    if (prio >= G.hover_next_prio) { G.hover_next_prio = prio; G.hover_next = id; }
}

void gui_block_rect(GuiRect r) {
    if (!gui_input_enabled()) return;
    hover_candidate(0xFFFFFFFFu, r);
}

GuiInteract gui_interact(GuiId id, GuiRect r, u32 flags) {
    GuiInteract o;
    memset(&o, 0, sizeof(o));
    bool en = gui_input_enabled() && !(flags & GUI_DISABLED);
    if (!gui_input_enabled()) return o;
    hover_candidate(id, r);
    if ((flags & GUI_FOCUSABLE) && G.foc_count < MAX_FOCUSABLE) {
        Focusable *f = &G.foc[G.foc_count++];
        f->id = id;
        f->r = r;
        f->flags = flags;
    }
    if (G.focus == id) { G.focus_seen = true; G.focus_rect = r; }
    if (!en) return o;
    o.hovered = G.hover == id && !G.nav_mode;
    o.focused = G.focus == id && G.nav_mode;
    if (o.hovered && G.mpressed[0]) { o.pressed = true; G.press_id = id; G.focus = id; }
    o.held = G.press_id == id && G.mdown[0];
    if (G.press_id == id && G.mreleased[0] && G.hover == id) o.clicked = true;
    if (o.hovered && G.mpressed[1]) o.right_clicked = true;
    if (o.hovered && G.dbl) o.double_clicked = true;
    if (o.focused && G.nav[NAV_ACCEPT]) { o.clicked = true; G.nav[NAV_ACCEPT] = false; }
    o.hot = o.hovered || o.focused;
    if (o.hovered && G.last_hover_sound != id && !(flags & GUI_NO_HOVER_SOUND) && (flags & GUI_FOCUSABLE)) {
        G.last_hover_sound = id;
        gui_sound(gui_theme()->snd_hover);
    }
    return o;
}

// Spatial navigation: the nearest focusable in the pressed direction (centre distance, with the
// perpendicular offset weighted x2), inside the focus scope.
static void navigate(void) {
    int dir = -1;
    for (int d = 0; d < 4; d++) if (G.nav[d]) { dir = d; break; }
    bool any_nav = dir >= 0 || G.nav[NAV_ACCEPT];
    if (G.foc_count == 0) return;
    int cur = -1;
    for (int i = 0; i < G.foc_count; i++) if (G.foc[i].id == G.focus) cur = i;
    if (cur < 0) {
        // Nothing focused: the default (or first) focusable, once the keyboard / pad is used.
        if (!any_nav && !G.nav_mode) return;
        int pick = 0;
        for (int i = 0; i < G.foc_count; i++) if (G.foc[i].flags & GUI_DEFAULT_FOCUS) { pick = i; break; }
        if (G.scope_set)
            for (int i = 0; i < G.foc_count; i++) {
                GuiRect r = G.foc[i].r;
                if (gui_rect_contains(G.scope, r.x + r.w * 0.5f, r.y + r.h * 0.5f)) { pick = i; break; }
            }
        G.focus = G.foc[pick].id;
        return;
    }
    if (dir < 0) return;
    GuiRect fr = G.foc[cur].r;
    float cx = fr.x + fr.w * 0.5f, cy = fr.y + fr.h * 0.5f;
    float best = 1e30f;
    int bi = -1;
    for (int i = 0; i < G.foc_count; i++) {
        if (i == cur) continue;
        GuiRect r = G.foc[i].r;
        float x = r.x + r.w * 0.5f, y = r.y + r.h * 0.5f;
        if (G.scope_set && !gui_rect_contains(G.scope, x, y)) continue;
        float dx = x - cx, dy = y - cy, primary, secondary;
        switch (dir) {
        case NAV_UP: primary = -dy; secondary = fabsf(dx); break;
        case NAV_DOWN: primary = dy; secondary = fabsf(dx); break;
        case NAV_LEFT: primary = -dx; secondary = fabsf(dy); break;
        default: primary = dx; secondary = fabsf(dy); break;
        }
        if (primary <= 1.0f) continue;
        // Vertical moves favour the next row (lists, settings), horizontal ones the same row.
        bool vertical = dir == NAV_UP || dir == NAV_DOWN;
        float score = vertical ? primary * 2.0f + secondary * 0.5f : primary + secondary * 2.0f;
        if (score < best) { best = score; bi = i; }
    }
    if (bi >= 0) {
        G.focus = G.foc[bi].id;
        gui_sound(gui_theme()->snd_hover);
    }
}

void gui_frame_end(void) {
    navigate();
    G.hover = G.hover_next;
    if (!G.hover) G.last_hover_sound = 0;
    // Tooltip: after the delay, next to the mouse, on top.
    if (G.tip_set && !G.nav_mode) {
        double age = G.time - G.tip_start;
        if (age > gui_theme()->tooltip_delay) {
            const GuiTheme *t = gui_theme();
            GuiText tx = { gui_s(t->t_small), GW_REGULAR, t->text, GUI_LEFT, 0, false, false, 0 };
            float wmax = gui_s(320);
            float tw = fminf(gd_text_width(&tx, G.tip), wmax);
            float th = gd_text_wrap(&tx, 0, 0, wmax, G.tip, false);
            float pad = gui_s(8);
            GuiRect r = { G.mouse.x + gui_s(16), G.mouse.y + gui_s(18), tw + pad * 2, th + pad * 2 };
            if (r.x + r.w > G.w) r.x = G.mouse.x - r.w - gui_s(8);
            if (r.y + r.h > G.h) r.y = G.mouse.y - r.h - gui_s(8);
            int layer = gd_layer();
            gd_set_layer(GL_TOP);
            gd_alpha_push(saturate((float)(age - t->tooltip_delay) / 0.1f));
            gd_shadow(r, gui_s(4), gui_s(10), t->shadow);
            gd_round_border(r, gui_s(t->radius), 1, UI_RGBA(20, 22, 25, 245), t->panel_border);
            gd_text_wrap(&tx, r.x + pad, r.y + pad, wmax, G.tip, true);
            gd_alpha_pop();
            gd_set_layer(layer);
        }
    } else if (!G.tip_set) {
        G.tip_id = 0;
    }
    if (G.ui_debug && G.ui_debug->i >= 1) {
        char line[160];
        snprintf(line, sizeof(line), "ui  scale %.2f  %dx%d  hover %08x  focus %08x  active %08x  nav %d  focusables %d  cmds %d  verts %d", G.scale,
                 (int)G.w, (int)G.h, G.hover, G.focus, G.active, (int)G.nav_mode, G.foc_count, gd_stats_cmds(), gd_stats_verts());
        int layer = gd_layer();
        gd_set_layer(GL_TOP);
        gd_rect(gui_rect(0, G.h - gui_s(22), G.w, gui_s(22)), UI_RGBA(0, 0, 0, 170));
        gd_text_simple(gui_s(8), G.h - gui_s(21), gui_s(13), GW_REGULAR, UI_RGBA(140, 255, 160, 255), GUI_LEFT, line);
        for (int i = 0; i < G.foc_count; i++) gd_border(G.foc[i].r, 1, G.foc[i].id == G.focus ? UI_RGBA(255, 220, 0, 255) : UI_RGBA(0, 200, 255, 120));
        LayoutParams lp = gui_layout_params();
        gd_border(gui_layout_safe_rect(&lp), 1, UI_RGBA(255, 0, 255, 140));
        gd_set_layer(layer);
    }
    gd_flush();
    end_input();
}

// ------------------------------------------------------------------ animation

static Tween *tween_get(GuiId id) {
    u32 h = dg_hash_u32(id) % MAX_TWEENS;
    for (int k = 0; k < MAX_TWEENS; k++) {
        Tween *t = &G.tw[(h + (u32)k) % MAX_TWEENS];
        if (t->id == id) { t->frame = G.frame; return t; }
        // Free (or stale for > 10 s of frames): take it.
        if (!t->id || G.frame - t->frame > 600) {
            memset(t, 0, sizeof(*t));
            t->id = id;
            t->start = G.time;
            t->frame = G.frame;
            return t;
        }
    }
    return &G.tw[h];
}

float gui_tween(GuiId id, float target, float duration) {
    Tween *t = tween_get(id);
    if (!t->init) { t->v = target; t->init = true; return target; }
    float k = duration > 1e-4f ? 1.0f - expf(-G.dt * 4.6f / duration) : 1.0f;
    t->v += (target - t->v) * k;
    if (fabsf(target - t->v) < 1e-4f) t->v = target;
    return t->v;
}

float gui_anim_t(GuiId id) {
    Tween *t = tween_get(id);
    return (float)(G.time - t->start);
}

void gui_anim_restart(GuiId id) {
    Tween *t = tween_get(id);
    t->start = G.time;
}

float gui_ease_out(float t) {
    t = saturate(t);
    float u = 1.0f - t;
    return 1.0f - u * u * u;
}

void gui_sound(const char *name) {
    const char *n = find_sound(name);
    if (n) audio_play_2d(n, gui_theme()->snd_volume, SND_UI);
}

void gui_tooltip(GuiId id, const char *text) {
    if (!text || !text[0]) return;
    if (G.tip_id != id) { G.tip_id = id; G.tip_start = G.time; }
    dg_strcpy(G.tip, sizeof(G.tip), text);
    G.tip_set = true;
}

bool gui_debug(void) { return G.ui_debug && G.ui_debug->i > 0; }

void gui_debug_rect(GuiRect r, u32 color, const char *label) {
    if (!gui_debug()) return;
    int layer = gd_layer();
    gd_set_layer(GL_TOP);
    gd_border(r, 1, color);
    if (label) gd_text_simple(r.x + 2, r.y + 1, gui_s(11), GW_REGULAR, color, GUI_LEFT, label);
    gd_set_layer(layer);
}
