#include "gui_widgets.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define TH (gui_theme())

// Disabled widgets (Engine Fidelity bounds, docs/UI.md "Widgets"): drawn faint, no focus / input.
static bool g_disabled;
void gw_disabled(bool on) { g_disabled = on; }
static GuiInteract interact(GuiId id, GuiRect r, u32 flags) {
    if (g_disabled) { GuiInteract none; memset(&none, 0, sizeof(none)); return none; }
    return gui_interact(id, r, flags);
}
#define DISABLED_ALPHA 0.38f

GuiCursor gc_make(GuiRect area, float gap) {
    GuiCursor c = { area, area.y, gap };
    return c;
}

GuiRect gc_row(GuiCursor *c, float h) {
    GuiRect r = { c->area.x, c->y, c->area.w, h };
    c->y += h + c->gap;
    return r;
}

float gc_used(const GuiCursor *c) { return c->y - c->area.y; }

void gui_split_h(GuiRect r, float f, float gap, GuiRect *a, GuiRect *b) {
    float wa = (r.w - gap) * f;
    *a = gui_rect(r.x, r.y, wa, r.h);
    *b = gui_rect(r.x + wa + gap, r.y, r.w - wa - gap, r.h);
}

static GuiText text_style(float size, int weight, u32 c, int align) {
    GuiText t = { size, weight, c, align, 0, false, false, 0 };
    return t;
}

static float text_y(GuiRect r, float size) { return r.y + (r.h - gd_line_height(size)) * 0.5f; }

void gw_label(GuiRect r, const char *text, float size, int weight, u32 color, int align) {
    GuiText t = text_style(size, weight, color, align);
    t.max_w = r.w;
    float x = align == GUI_CENTER ? r.x + r.w * 0.5f : (align == GUI_RIGHT ? r.x + r.w : r.x);
    gd_text(&t, x, text_y(r, size), text);
}

void gw_label_caps(GuiRect r, const char *text, float size, u32 color, int align) {
    GuiText t = text_style(size, GW_MEDIUM, color, align);
    t.caps = true;
    t.tracking = TH->tracking_caps;
    t.max_w = r.w;
    float x = align == GUI_CENTER ? r.x + r.w * 0.5f : (align == GUI_RIGHT ? r.x + r.w : r.x);
    gd_text(&t, x, text_y(r, size), text);
}

void gw_focus_ring(GuiRect r, float radius, float amount) {
    if (amount <= 0.01f) return;
    float t = fmaxf(1.0f, gui_s(1.5f));
    GuiRect o = gui_rect_inset(r, -t * 1.5f);
    gd_round_border(o, radius + t, t, 0, gui_rgba_mul(TH->focus, amount));
}

void gw_panel(GuiRect r, bool backdrop) {
    float rad = gui_s(TH->radius_panel);
    gd_shadow(r, rad, gui_s(26), TH->shadow);
    if (backdrop) gd_backdrop(r, UI_RGBA(255, 255, 255, 255));
    gd_round_border(r, rad, 1.0f, TH->panel, TH->panel_border);
}

void gw_hud_panel(GuiRect r) { gd_round(r, gui_s(TH->radius), TH->hud_panel); }

void gw_section(GuiRect r, const char *title) {
    float sz = gui_s(TH->t_label);
    GuiText t = text_style(sz, GW_SEMIBOLD, TH->text_faint, GUI_LEFT);
    t.caps = true;
    t.tracking = TH->tracking_caps * 1.4f;
    float ty = r.y + r.h - gd_line_height(sz) - gui_s(4);
    float w = gd_text(&t, r.x, ty, title);
    float ly = floorf(ty + gd_line_height(sz) * 0.5f);
    gd_rect(gui_rect(r.x + w + gui_s(10), ly, fmaxf(0, r.w - w - gui_s(10)), 1), TH->panel_border);
}

// ------------------------------------------------------------------ buttons

bool gw_menu_item(GuiId id, GuiRect r, const char *label, bool selected, float appear, bool enabled) {
    appear = gui_ease_out(appear);
    GuiInteract in = gui_interact(id, r, GUI_FOCUSABLE | (enabled ? 0 : GUI_DISABLED));
    float h = gui_tween(id ^ 0x51u, in.hot && enabled ? 1.0f : 0.0f, TH->hover_t);
    float sel = gui_tween(id ^ 0x77u, selected ? 1.0f : 0.0f, TH->hover_t * 1.5f);
    float dx = (1.0f - appear) * gui_s(-18);
    GuiRect rr = { r.x + dx, r.y, r.w, r.h };
    gd_alpha_push(appear * (enabled ? 1.0f : 0.45f));
    // Hover: a fill fading to the right; accent bar on the left grows with hover / selection.
    float hv = fmaxf(h, sel * 0.7f);
    if (hv > 0.01f) gd_rect_hgrad(rr, gui_rgba_mul(TH->hover, hv * 1.6f), gui_rgba_mul(TH->hover, 0.0f));
    float bar = fmaxf(h, sel);
    if (bar > 0.01f) {
        float bh = rr.h * (0.35f + 0.65f * bar);
        gd_rect(gui_rect(rr.x, rr.y + (rr.h - bh) * 0.5f, gui_s(TH->accent_bar), bh), gui_rgba_mul(TH->accent, fmaxf(bar, 0.5f)));
    }
    u32 tc = gui_rgba_lerp(TH->text_dim, TH->text, fmaxf(h, sel));
    if (selected) tc = gui_rgba_lerp(tc, TH->accent, 0.35f);
    GuiText t = text_style(gui_s(TH->t_body + 1), GW_MEDIUM, tc, GUI_LEFT);
    t.caps = true;
    t.tracking = TH->tracking_caps;
    t.shadow = true;
    gd_text(&t, rr.x + gui_s(14) + h * gui_s(4), text_y(rr, t.size), label);
    gd_alpha_pop();
    if (in.clicked && enabled) gui_sound(TH->snd_click);
    return in.clicked && enabled;
}

bool gw_button(GuiId id, GuiRect r, const char *label, int kind, bool enabled) {
    GuiInteract in = gui_interact(id, r, GUI_FOCUSABLE | (enabled ? 0 : GUI_DISABLED));
    float h = gui_tween(id ^ 0x51u, in.hot && enabled ? 1.0f : 0.0f, TH->hover_t);
    float rad = gui_s(TH->radius);
    u32 fill, border, tc;
    switch (kind) {
    case GB_PRIMARY:
        fill = gui_rgba_lerp(TH->accent, UI_RGBA(250, 170, 90, 255), h * 0.35f);
        border = 0;
        tc = TH->text_dark;
        break;
    case GB_DANGER:
        fill = gui_rgba_lerp(UI_RGBA(120, 36, 30, 220), TH->danger, h * 0.6f);
        border = gui_rgba_mul(TH->danger, 0.6f);
        tc = TH->text;
        break;
    case GB_GHOST:
        fill = gui_rgba_mul(TH->hover, h * 1.5f);
        border = 0;
        tc = gui_rgba_lerp(TH->text_dim, TH->text, h);
        break;
    default:
        fill = gui_rgba_lerp(UI_RGBA(255, 255, 255, 12), UI_RGBA(255, 255, 255, 30), h);
        border = gui_rgba_lerp(TH->panel_border, UI_RGBA(255, 255, 255, 60), h);
        tc = TH->text;
        break;
    }
    if (in.held) fill = gui_rgba_lerp(fill, UI_RGBA(0, 0, 0, 255), 0.15f);
    gd_alpha_push(enabled ? 1.0f : 0.4f);
    gd_round_border(r, rad, 1.0f, fill, border);
    GuiText t = text_style(gui_s(TH->t_small + 1), GW_SEMIBOLD, tc, GUI_CENTER);
    t.caps = true;
    t.tracking = TH->tracking_caps;
    t.max_w = r.w - gui_s(8);
    gd_text(&t, r.x + r.w * 0.5f, text_y(r, t.size), label);
    gd_alpha_pop();
    if (in.focused) gw_focus_ring(r, rad, 1.0f);
    if (in.clicked && enabled) gui_sound(TH->snd_click);
    return in.clicked && enabled;
}

int gw_tabs(GuiId id, GuiRect r, const char *const *labels, int n, int cur) {
    if (n <= 0) return cur;
    float sz = gui_s(TH->t_small + 1);
    GuiText t = text_style(sz, GW_SEMIBOLD, TH->text_dim, GUI_LEFT);
    t.caps = true;
    t.tracking = TH->tracking_caps;
    float x = r.x, gap = gui_s(26);
    int res = cur;
    float cur_x = r.x, cur_w = 0;
    for (int i = 0; i < n; i++) {
        float w = gd_text_width(&t, labels[i]);
        GuiRect tr = { x - gui_s(6), r.y, w + gui_s(12), r.h };
        GuiId tid = gui_idi("tab", (int)id + i);
        GuiInteract in = gui_interact(tid, tr, GUI_FOCUSABLE);
        float h = gui_tween(tid, in.hot || i == cur ? 1.0f : 0.0f, TH->hover_t);
        t.color = i == cur ? TH->text : gui_rgba_lerp(TH->text_faint, TH->text, h * 0.7f);
        gd_text(&t, x, text_y(r, sz), labels[i]);
        if (in.focused) gw_focus_ring(tr, gui_s(2), 1.0f);
        if (in.clicked && i != cur) { res = i; gui_sound(TH->snd_tab); }
        if (i == cur) { cur_x = x; cur_w = w; }
        x += w + gap;
    }
    // Animated underline under the current tab.
    float ux = gui_tween(id ^ 0xA1u, cur_x, 0.12f), uw = gui_tween(id ^ 0xA2u, cur_w, 0.12f);
    gd_rect(gui_rect(r.x, r.y + r.h - 1, r.w, 1), TH->panel_border);
    gd_rect(gui_rect(ux, r.y + r.h - gui_s(2), uw, gui_s(2)), TH->accent);
    return res;
}

// ------------------------------------------------------------------ top tab bar / power / avatar

static void initials(const char *name, char *out);

int gw_tabbar(GuiId id, GuiRect r, const char *const *labels, const bool *enabled, const char *const *reason, int n, int cur, bool focusable) {
    if (n <= 0) return cur;
    float sz = gui_s(gui_tok("tab_text"));
    GuiText t = text_style(sz, GW_SEMIBOLD, TH->text_dim, GUI_LEFT);
    t.caps = true;
    t.tracking = TH->tracking_caps * 1.3f;
    float gap = gui_s(gui_tok("tab_gap")), padx = gui_s(gui_tok("tab_pad"));
    float under = fmaxf(1.0f, gui_s(gui_tok("tab_underline")));
    float x = r.x;
    int res = cur;
    float cur_x = r.x, cur_w = 0;
    gd_rect(gui_rect(r.x, r.y + r.h - 1, r.w, 1), TH->panel_border);
    for (int i = 0; i < n; i++) {
        bool on = !enabled || enabled[i];
        float w = gd_text_width(&t, labels[i]);
        GuiRect tr = { x - padx, r.y, w + padx * 2, r.h };
        GuiId tid = gui_idi("tabbar", (int)id + i);
        GuiInteract in = gui_interact(tid, tr, on ? (focusable ? GUI_FOCUSABLE : 0) : GUI_DISABLED);
        float h = gui_tween(tid, (in.hot && on) || i == cur ? 1.0f : 0.0f, TH->hover_t);
        if (in.hot && on && i != cur) gd_rect_grad(tr, gui_rgba_mul(TH->hover, 0.0f), gui_rgba_mul(TH->hover, h * 1.4f));
        t.color = !on ? gui_rgba_mul(TH->text_faint, 0.6f) : (i == cur ? TH->text : gui_rgba_lerp(TH->text_faint, TH->text, h * 0.75f));
        gd_text(&t, x, text_y(r, sz), labels[i]);
        // Key hint while the keyboard / pad drives.
        if (gui_nav_mode() && on) {
            char k[12];
            snprintf(k, sizeof(k), "%d", i + 1);
            GuiText kt = text_style(gui_s(TH->t_tiny), GW_MEDIUM, TH->text_faint, GUI_LEFT);
            gd_text(&kt, x + w + gui_s(4), r.y + gui_s(gui_tok("tab_hint_y")), k);
        }
        if (in.focused) gw_focus_ring(tr, gui_s(TH->radius), 1.0f);
        if (!on && reason && reason[i] && gui_rect_contains(tr, gui_mouse().x, gui_mouse().y)) gui_tooltip(tid, reason[i]);
        if (in.clicked && on && i != cur) { res = i; gui_sound(TH->snd_tab); }
        if (i == cur) { cur_x = x; cur_w = w; }
        x += w + gap;
    }
    float ux = gui_tween(id ^ 0xB1u, cur_x, 0.14f), uw = gui_tween(id ^ 0xB2u, cur_w, 0.14f);
    gd_rect(gui_rect(ux, r.y + r.h - under, uw, under), TH->accent);
    return res;
}

bool gw_power(GuiId id, GuiRect r, const char *tooltip) {
    GuiInteract in = gui_interact(id, r, GUI_FOCUSABLE);
    float h = gui_tween(id ^ 0x51u, in.hot ? 1.0f : 0.0f, TH->hover_t);
    float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
    float rad = fminf(r.w, r.h) * 0.5f;
    if (h > 0.01f) gd_circle(cx, cy, rad, gui_rgba_mul(TH->danger, h * 0.18f));
    u32 c = gui_rgba_lerp(TH->text_dim, TH->danger, h);
    float ir = rad * 0.46f, th = fmaxf(1.2f, gui_s(gui_tok("power_stroke")));
    // The ring opens at the top (80 degrees); the stroke stands in the gap.
    const int seg = 28;
    const float gap = 80.0f * DG_DEG2RAD, span = 2.0f * DG_PI - gap;
    for (int k = 0; k < seg; k++) {
        float a0 = gap * 0.5f + span * (float)k / (float)seg, a1 = gap * 0.5f + span * (float)(k + 1) / (float)seg;
        gd_line(cx + sinf(a0) * ir, cy - cosf(a0) * ir, cx + sinf(a1) * ir, cy - cosf(a1) * ir, th, c);
    }
    gd_line(cx, cy - ir - th, cx, cy - ir * 0.12f, th, c);
    if (in.focused) gw_focus_ring(r, rad, 1.0f);
    if (in.hovered && tooltip) gui_tooltip(id, tooltip);
    if (in.clicked) gui_sound(TH->snd_click);
    return in.clicked;
}

void gw_avatar(GuiRect r, TextureId tex, const char *name, u32 tint) {
    float rad = gui_s(TH->radius);
    if (tex) {
        gd_image(r, tex, v4(0, 0, 1, 1), UI_RGBA(255, 255, 255, 255));
        gd_round_outline(r, rad, 1.0f, TH->panel_border);
        return;
    }
    char ini[4];
    initials(name && name[0] ? name : "?", ini);
    float s = fminf(r.w, r.h);
    gd_circle(r.x + r.w * 0.5f, r.y + r.h * 0.5f, s * 0.5f, gui_rgba_mul(tint, 0.55f));
    gw_label(r, ini, s * 0.42f, GW_SEMIBOLD, TH->text, GUI_CENTER);
}

// ------------------------------------------------------------------ settings controls

GuiRect gw_setting_row(GuiCursor *c, const char *label, const char *hint, GuiId id) {
    GuiRect r = gc_row(c, gui_s(TH->row_h));
    GuiRect a, b;
    gui_split_h(r, 0.46f, gui_s(16), &a, &b);
    GuiInteract in = gui_interact(id ^ 0x3C3Cu, r, 0);
    bool focus_here = false;
    GuiRect fr;
    if (gui_focus_rect(&fr) && gui_nav_mode() && gui_rect_contains(r, fr.x + fr.w * 0.5f, fr.y + fr.h * 0.5f)) focus_here = true;
    float h = gui_tween(id ^ 0x3C3Du, in.hovered || focus_here ? 1.0f : 0.0f, TH->hover_t);
    if (h > 0.01f) gd_round(gui_rect(r.x - gui_s(10), r.y, r.w + gui_s(20), r.h), gui_s(TH->radius), gui_rgba_mul(TH->hover, h));
    gw_label(a, label, gui_s(TH->t_body - 1), GW_REGULAR, g_disabled ? TH->text_faint : gui_rgba_lerp(TH->text_dim, TH->text, h), GUI_LEFT);
    if (hint && in.hovered) gui_tooltip(id ^ 0x3C3Cu, hint);
    return b;
}

bool gw_slider(GuiId id, GuiRect r, float *v, float lo, float hi, float step, const char *fmt) {
    float old = *v;
    float vw = gui_s(64);
    GuiRect track = { r.x, r.y, r.w - vw - gui_s(12), r.h };
    GuiInteract in = interact(id, track, GUI_FOCUSABLE);
    gd_alpha_push(g_disabled ? DISABLED_ALPHA : 1.0f);
    if (in.pressed) gui_set_active(id);
    bool user = false;   // only a drag / key press snaps and changes the value (drawing never does)
    if (!g_disabled && gui_active() == id) {
        if (!gui_mouse_down(0)) gui_set_active(0);
        else {
            float f = saturate((gui_mouse().x - track.x) / fmaxf(1.0f, track.w));
            *v = lo + (hi - lo) * f;
            user = true;
        }
    }
    if (in.focused) {
        float st = step > 0 ? step : (hi - lo) / 50.0f;
        if (gui_shift()) st *= 5;
        if (gui_nav(NAV_LEFT)) { *v -= st; gui_nav_consume(NAV_LEFT); user = true; }
        if (gui_nav(NAV_RIGHT)) { *v += st; gui_nav_consume(NAV_RIGHT); user = true; }
    }
    if (user) {
        if (step > 0) *v = lo + roundf((*v - lo) / step) * step;
        *v = clampf(*v, lo, hi);
    }
    float f = (hi > lo) ? saturate((*v - lo) / (hi - lo)) : 0;
    float h = gui_tween(id ^ 0x51u, in.hot || gui_active() == id ? 1.0f : 0.0f, TH->hover_t);
    float th = gui_s(3), cy = track.y + track.h * 0.5f;
    gd_round(gui_rect(track.x, cy - th * 0.5f, track.w, th), th * 0.5f, UI_RGBA(255, 255, 255, 34));
    gd_round(gui_rect(track.x, cy - th * 0.5f, track.w * f, th), th * 0.5f, TH->accent);
    float kr = gui_s(6) + h * gui_s(1.5f);
    gd_circle(track.x + track.w * f, cy, kr + gui_s(1), UI_RGBA(0, 0, 0, 90));
    gd_circle(track.x + track.w * f, cy, kr, gui_rgba_lerp(UI_RGBA(230, 230, 225, 255), UI_RGBA(255, 255, 255, 255), h));
    if (in.focused) gw_focus_ring(gui_rect(track.x + track.w * f - kr, cy - kr, kr * 2, kr * 2), kr, 1.0f);
    char buf[32];
    snprintf(buf, sizeof(buf), fmt ? fmt : "%.2f", *v);
    gw_label(gui_rect(r.x + r.w - vw, r.y, vw, r.h), buf, gui_s(TH->t_body - 1), GW_MEDIUM, TH->text, GUI_RIGHT);
    gd_alpha_pop();
    return user && *v != old;
}

bool gw_toggle(GuiId id, GuiRect r, bool *v) {
    float w = gui_s(38), h = gui_s(20);
    GuiRect sw = { r.x, r.y + (r.h - h) * 0.5f, w, h };
    GuiInteract in = interact(id, gui_rect(r.x, r.y, fmaxf(w, r.w * 0.5f), r.h), GUI_FOCUSABLE);
    gd_alpha_push(g_disabled ? DISABLED_ALPHA : 1.0f);
    bool changed = false;
    if (in.clicked) { *v = !*v; changed = true; gui_sound(TH->snd_click); }
    if (in.focused && (gui_nav(NAV_LEFT) || gui_nav(NAV_RIGHT))) {
        bool nv = gui_nav(NAV_RIGHT);
        gui_nav_consume(NAV_LEFT);
        gui_nav_consume(NAV_RIGHT);
        if (nv != *v) { *v = nv; changed = true; gui_sound(TH->snd_click); }
    }
    float k = gui_tween(id ^ 0x99u, *v ? 1.0f : 0.0f, 0.1f);
    float hv = gui_tween(id ^ 0x51u, in.hot ? 1.0f : 0.0f, TH->hover_t);
    gd_round(sw, h * 0.5f, gui_rgba_lerp(UI_RGBA(255, 255, 255, (int)(30 + 20 * hv)), TH->accent, k));
    float kr = h * 0.5f - gui_s(3);
    gd_circle(sw.x + h * 0.5f + (w - h) * k, sw.y + h * 0.5f, kr, gui_rgba_lerp(UI_RGBA(200, 200, 196, 255), UI_RGBA(255, 255, 255, 255), k));
    gw_label(gui_rect(sw.x + w + gui_s(12), r.y, gui_s(60), r.h), *v ? "On" : "Off", gui_s(TH->t_body - 1), GW_MEDIUM, *v ? TH->text : TH->text_faint, GUI_LEFT);
    if (in.focused) gw_focus_ring(sw, h * 0.5f, 1.0f);
    gd_alpha_pop();
    return changed;
}

// Dropdown: the list opens in the overlay layer; left / right cycle while focused.
static GuiId g_open_dropdown;
static float g_dropdown_scroll;

bool gw_dropdown(GuiId id, GuiRect r, const char *const *items, int n, int *sel) { return gw_dropdown_ex(id, r, items, n, sel, NULL, NULL); }

// enabled[i] false: the item is greyed (not selectable, skipped by left / right), notes[i] is shown
// right-aligned in the list ("requires High").
static int step_enabled(int from, int dir, int n, const bool *enabled) {
    for (int k = 1; k <= n; k++) {
        int i = ((from + dir * k) % n + n) % n;
        if (!enabled || enabled[i]) return i;
    }
    return from;
}

bool gw_dropdown_ex(GuiId id, GuiRect r, const char *const *items, int n, int *sel, const bool *enabled, const char *const *notes) {
    if (n <= 0) return false;
    int old = *sel;
    *sel = DG_CLAMP(*sel, 0, n - 1);
    if (g_disabled && g_open_dropdown == id) g_open_dropdown = 0;
    bool open = g_open_dropdown == id;
    GuiInteract in = interact(id, r, GUI_FOCUSABLE);
    gd_alpha_push(g_disabled ? DISABLED_ALPHA : 1.0f);
    if (in.clicked) {
        g_open_dropdown = open ? 0 : id;
        open = !open;
        g_dropdown_scroll = 0;
        gui_sound(TH->snd_click);
    }
    if (in.focused && !open) {
        if (gui_nav(NAV_LEFT)) { *sel = step_enabled(*sel, -1, n, enabled); gui_nav_consume(NAV_LEFT); gui_sound(TH->snd_click); }
        if (gui_nav(NAV_RIGHT)) { *sel = step_enabled(*sel, 1, n, enabled); gui_nav_consume(NAV_RIGHT); gui_sound(TH->snd_click); }
    }
    float h = gui_tween(id ^ 0x51u, in.hot || open ? 1.0f : 0.0f, TH->hover_t);
    float rad = gui_s(TH->radius);
    gd_round_border(r, rad, 1, gui_rgba_lerp(UI_RGBA(255, 255, 255, 10), UI_RGBA(255, 255, 255, 22), h),
                    gui_rgba_lerp(TH->panel_border, UI_RGBA(255, 255, 255, 50), h));
    GuiRect tr = { r.x + gui_s(10), r.y, r.w - gui_s(34), r.h };
    gw_label(tr, items[*sel], gui_s(TH->t_body - 1), GW_MEDIUM, TH->text, GUI_LEFT);
    // Chevron
    float cx = r.x + r.w - gui_s(16), cy = r.y + r.h * 0.5f, cs = gui_s(4);
    u32 cc = gui_rgba_lerp(TH->text_faint, TH->text, h);
    if (open) { gd_line(cx - cs, cy + cs * 0.5f, cx, cy - cs * 0.5f, gui_s(1.5f), cc); gd_line(cx, cy - cs * 0.5f, cx + cs, cy + cs * 0.5f, gui_s(1.5f), cc); }
    else { gd_line(cx - cs, cy - cs * 0.5f, cx, cy + cs * 0.5f, gui_s(1.5f), cc); gd_line(cx, cy + cs * 0.5f, cx + cs, cy - cs * 0.5f, gui_s(1.5f), cc); }
    if (in.focused) gw_focus_ring(r, rad, 1.0f);

    if (open) {
        float ih = gui_s(30);
        int vis = DG_MIN(n, 9);
        GuiRect lr = { r.x, r.y + r.h + gui_s(4), r.w, ih * (float)vis + gui_s(8) };
        if (lr.y + lr.h > gui_h() - gui_s(8)) lr.y = r.y - lr.h - gui_s(4);
        int layer = gd_layer();
        gd_set_layer(GL_OVERLAY);
        GuiRect clip_save = gd_clip();
        (void)clip_save;
        gd_clip_push(gui_rect(0, 0, gui_w(), gui_h()));
        gd_shadow(lr, rad, gui_s(18), TH->shadow);
        gd_round_border(lr, rad, 1, UI_RGBA(22, 24, 27, 250), UI_RGBA(255, 255, 255, 30));
        gui_block_rect(lr);
        float max_scroll = fmaxf(0.0f, (float)n * ih - (lr.h - gui_s(8)));
        if (gui_rect_contains(lr, gui_mouse().x, gui_mouse().y) && gui_wheel() != 0) {
            g_dropdown_scroll = clampf(g_dropdown_scroll - gui_wheel() * ih * 2, 0, max_scroll);
            gui_wheel_consume();
        }
        gd_clip_push(gui_rect_inset(lr, gui_s(4)));
        for (int i = 0; i < n; i++) {
            GuiRect ir = { lr.x + gui_s(4), lr.y + gui_s(4) + (float)i * ih - g_dropdown_scroll, lr.w - gui_s(8), ih };
            if (ir.y + ir.h < lr.y || ir.y > lr.y + lr.h) continue;
            GuiId iid = gui_idi("dd_item", (int)id + i);
            bool on = !enabled || enabled[i];
            GuiInteract ii = gui_interact(iid, ir, on ? (GUI_FOCUSABLE | (i == *sel ? GUI_DEFAULT_FOCUS : 0)) : 0);
            if (ii.focused) {
                float top = ir.y - (lr.y + gui_s(4)), bot = ir.y + ir.h - (lr.y + lr.h - gui_s(4));
                if (top < 0) g_dropdown_scroll = clampf(g_dropdown_scroll + top, 0, max_scroll);
                if (bot > 0) g_dropdown_scroll = clampf(g_dropdown_scroll + bot, 0, max_scroll);
            }
            if (ii.hot && on) gd_round(ir, rad, TH->hover);
            if (i == *sel) gd_rect(gui_rect(ir.x, ir.y + ir.h * 0.25f, gui_s(2), ir.h * 0.5f), TH->accent);
            gw_label(gui_rect(ir.x + gui_s(10), ir.y, ir.w - gui_s(14), ir.h), items[i], gui_s(TH->t_body - 1), i == *sel ? GW_SEMIBOLD : GW_REGULAR,
                     !on ? TH->text_faint : (i == *sel ? TH->text : TH->text_dim), GUI_LEFT);
            if (notes && notes[i] && notes[i][0])
                gw_label(gui_rect(ir.x + gui_s(10), ir.y, ir.w - gui_s(20), ir.h), notes[i], gui_s(TH->t_small), GW_REGULAR, TH->text_faint, GUI_RIGHT);
            if (ii.clicked && on) { *sel = i; g_open_dropdown = 0; gui_sound(TH->snd_click); }
        }
        gd_clip_pop();
        gd_clip_pop();
        gui_focus_scope(lr);
        gd_set_layer(layer);
        // Close: back, or a click outside.
        if (gui_nav(NAV_BACK)) { gui_nav_consume(NAV_BACK); g_open_dropdown = 0; gui_set_focus(id); }
        else if (gui_mouse_pressed(0) && !gui_rect_contains(lr, gui_mouse().x, gui_mouse().y) && !gui_rect_contains(r, gui_mouse().x, gui_mouse().y))
            g_open_dropdown = 0;
        if (!g_open_dropdown) gui_set_focus(id);
    }
    gd_alpha_pop();
    return *sel != old;
}

bool gw_keybind(GuiId id, GuiRect r, const char *text, bool capturing) {
    GuiInteract in = gui_interact(id, r, GUI_FOCUSABLE);
    float h = gui_tween(id ^ 0x51u, in.hot || capturing ? 1.0f : 0.0f, TH->hover_t);
    float rad = gui_s(TH->radius);
    u32 border = capturing ? TH->accent : gui_rgba_lerp(TH->panel_border, UI_RGBA(255, 255, 255, 50), h);
    gd_round_border(r, rad, 1, gui_rgba_lerp(UI_RGBA(255, 255, 255, 8), UI_RGBA(255, 255, 255, 20), h), border);
    float pulse = capturing ? 0.55f + 0.45f * sinf((float)gui_time() * 8.0f) : 1.0f;
    gw_label(gui_rect_inset(r, gui_s(4)), capturing ? "Press a key..." : text, gui_s(TH->t_small + 1), GW_MEDIUM,
             capturing ? gui_rgba_mul(TH->accent, pulse) : (text[0] && strcmp(text, "-") ? TH->text : TH->text_faint), GUI_CENTER);
    if (in.focused) gw_focus_ring(r, rad, 1.0f);
    if (in.clicked) gui_sound(TH->snd_click);
    return in.clicked;
}

static GuiId g_edit;

void gw_textfield_activate(GuiId id) { g_edit = id; gui_set_text_mode(true); gui_set_focus(id); }
bool gw_textfield_editing(GuiId id) { return g_edit == id; }

bool gw_textfield(GuiId id, GuiRect r, char *buf, int size, const char *placeholder) {
    GuiInteract in = gui_interact(id, r, GUI_FOCUSABLE);
    bool editing = g_edit == id;
    if (in.clicked && !editing) { g_edit = id; editing = true; gui_set_text_mode(true); }
    bool enter = false;
    if (editing) {
        int cp;
        int len = (int)strlen(buf);
        while (gui_text_input_char(&cp)) if (len < size - 1) { buf[len++] = (char)cp; buf[len] = 0; }
        for (int b = gui_backspaces(); b > 0 && len > 0; b--) buf[--len] = 0;
        if (gui_nav(NAV_ACCEPT)) { gui_nav_consume(NAV_ACCEPT); enter = true; }
        bool click_out = gui_mouse_pressed(0) && !gui_rect_contains(r, gui_mouse().x, gui_mouse().y);
        if (enter || gui_nav(NAV_BACK) || click_out) {
            if (gui_nav(NAV_BACK)) gui_nav_consume(NAV_BACK);
            g_edit = 0;
            gui_set_text_mode(false);
        }
    }
    float h = gui_tween(id ^ 0x51u, in.hot || editing ? 1.0f : 0.0f, TH->hover_t);
    float rad = gui_s(TH->radius);
    gd_round_border(r, rad, 1, UI_RGBA(0, 0, 0, 90), editing ? TH->accent : gui_rgba_lerp(TH->panel_border, UI_RGBA(255, 255, 255, 50), h));
    GuiRect tr = { r.x + gui_s(10), r.y, r.w - gui_s(20), r.h };
    if (buf[0]) gw_label(tr, buf, gui_s(TH->t_body - 1), GW_MEDIUM, TH->text, GUI_LEFT);
    else if (placeholder && !editing) gw_label(tr, placeholder, gui_s(TH->t_body - 1), GW_REGULAR, TH->text_faint, GUI_LEFT);
    if (editing && fmodf((float)gui_time(), 1.0f) < 0.55f) {
        GuiText t = { gui_s(TH->t_body - 1), GW_MEDIUM, TH->text, GUI_LEFT, 0, false, false, 0 };
        float cx = tr.x + gd_text_width(&t, buf) + gui_s(1);
        gd_rect(gui_rect(cx, r.y + r.h * 0.25f, gui_s(1.5f), r.h * 0.5f), TH->accent);
    }
    if (in.focused && !editing) gw_focus_ring(r, rad, 1.0f);
    return enter;
}

// ------------------------------------------------------------------ scrolling

void gw_scroll_begin(GuiScroll *s, GuiId id, GuiRect view) {
    s->id = id;
    s->view = view;
    if (gui_input_enabled() && gui_rect_contains(view, gui_mouse().x, gui_mouse().y) && gui_wheel() != 0) {
        s->target -= gui_wheel() * gui_s(TH->scroll_speed);
        gui_wheel_consume();
    }
    // Keyboard / gamepad focus inside: keep it visible.
    GuiRect fr;
    if (gui_nav_mode() && gui_focus_rect(&fr) && fr.x >= view.x - 1 && fr.x < view.x + view.w) {
        float top = fr.y - view.y, bot = fr.y + fr.h - (view.y + view.h);
        float m = gui_s(12);
        if (top < m && top > -view.h * 4) s->target += top - m;
        else if (bot > -m && bot < view.h * 4) s->target += bot + m;
    }
    float max_off = fmaxf(0.0f, s->content_h - view.h);
    s->target = clampf(s->target, 0, max_off);
    s->offset = s->offset + (s->target - s->offset) * (1.0f - expf(-gui_dt() * 18.0f));
    if (fabsf(s->offset - s->target) < 0.5f) s->offset = s->target;
    gd_clip_push(view);
}

float gw_scroll_y(const GuiScroll *s) { return s->view.y - floorf(s->offset); }

void gw_scroll_to(GuiScroll *s, float y, float h) {
    if (y < s->target) s->target = y;
    if (y + h > s->target + s->view.h) s->target = y + h - s->view.h;
}

void gw_scroll_end(GuiScroll *s, float content_h) {
    gd_clip_pop();
    s->content_h = content_h;
    float max_off = fmaxf(0.0f, content_h - s->view.h);
    if (max_off <= 0.5f) { s->target = s->offset = 0; return; }
    // Scrollbar (draggable).
    float bw = gui_s(4);
    GuiRect tr = { s->view.x + s->view.w + gui_s(6), s->view.y, bw, s->view.h };
    float frac = s->view.h / content_h;
    float th = fmaxf(gui_s(24), tr.h * frac);
    float ty = tr.y + (tr.h - th) * (s->offset / max_off);
    GuiRect thumb = { tr.x - gui_s(3), ty, bw + gui_s(6), th };
    GuiInteract in = gui_interact(s->id ^ 0x5C5Cu, thumb, 0);
    if (in.pressed) { s->dragging = true; s->drag_from = gui_mouse().y; s->drag_off = s->target; gui_set_active(s->id ^ 0x5C5Cu); }
    if (s->dragging) {
        if (!gui_mouse_down(0)) { s->dragging = false; gui_set_active(0); }
        else s->target = s->offset = clampf(s->drag_off + (gui_mouse().y - s->drag_from) * content_h / s->view.h, 0, max_off);
    }
    gd_round(tr, bw * 0.5f, UI_RGBA(255, 255, 255, 14));
    gd_round(gui_rect(tr.x, ty, bw, th), bw * 0.5f, in.hot || s->dragging ? UI_RGBA(255, 255, 255, 110) : UI_RGBA(255, 255, 255, 60));
    // Fades at the clipped edges.
    if (s->offset > 1) gd_rect_grad(gui_rect(s->view.x, s->view.y, s->view.w, gui_s(14)), UI_RGBA(13, 15, 17, 160), UI_RGBA(13, 15, 17, 0));
    if (s->offset < max_off - 1) gd_rect_grad(gui_rect(s->view.x, s->view.y + s->view.h - gui_s(14), s->view.w, gui_s(14)), UI_RGBA(13, 15, 17, 0), UI_RGBA(13, 15, 17, 160));
}

// ------------------------------------------------------------------ slot

static void initials(const char *name, char *out) {
    int n = 0;
    bool word = true;
    for (const char *p = name; *p && n < 2; p++) {
        if (*p == ' ' || *p == '-') { word = true; continue; }
        if (word && ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9'))) {
            out[n++] = (char)((*p >= 'a' && *p <= 'z') ? *p - 32 : *p);
            word = false;
        }
    }
    out[n] = 0;
}

void gw_slot(GuiRect r, const GuiSlotView *v) {
    float rad = gui_s(TH->radius);
    float sx = v->shake > 0 ? sinf(v->shake * 40.0f) * v->shake * gui_s(5) : 0.0f;
    r.x += sx;
    u32 bg = v->empty ? TH->slot_bg : gui_rgba_lerp(TH->slot_bg, v->tint, 0.10f);
    if (v->hot) bg = gui_rgba_lerp(bg, TH->slot_hi, 0.8f);
    if (v->drop_state == 1) bg = gui_rgba_lerp(bg, TH->accent, 0.18f);
    if (v->drop_state == 2) bg = gui_rgba_lerp(bg, TH->danger, 0.25f);
    u32 border = v->selected ? TH->select : (v->drop_state == 1 ? gui_rgba_mul(TH->accent, 0.8f) : (v->drop_state == 2 ? TH->danger : TH->slot_border));
    gd_alpha_push(v->dim ? 0.4f : 1.0f);
    gd_round_border(r, rad, v->selected ? fmaxf(1.0f, gui_s(1.5f)) : 1.0f, bg, border);
    if (!v->empty) {
        // Type tint: a thin strip at the bottom edge.
        gd_rect(gui_rect(r.x + rad, r.y + r.h - gui_s(2), r.w - rad * 2, gui_s(2)), gui_rgba_mul(v->tint, 0.85f));
        GuiRect ir = gui_rect_inset(r, r.w * 0.12f);
        if (v->icon) {
            gd_image(ir, v->icon, v->uv, UI_RGBA(255, 255, 255, 255));
        } else {
            char ini[4];
            initials(v->name ? v->name : "?", ini);
            gd_circle(r.x + r.w * 0.5f, r.y + r.h * 0.46f, r.w * 0.24f, gui_rgba_mul(v->tint, 0.55f));
            gw_label(gui_rect(r.x, r.y + r.h * 0.46f - r.w * 0.24f, r.w, r.w * 0.48f), ini, r.w * 0.2f, GW_SEMIBOLD, TH->text, GUI_CENTER);
        }
        float fs = fmaxf(gui_s(TH->t_tiny), r.h * 0.19f);
        if (v->count > 1) {
            char b[16];
            snprintf(b, sizeof(b), "%d", v->count);
            GuiText t = { fs, GW_SEMIBOLD, TH->text, GUI_RIGHT, 0, false, true, 0 };
            gd_text(&t, r.x + r.w - gui_s(5), r.y + r.h - gd_line_height(fs) - gui_s(3), b);
        }
        if (v->ammo_frac >= 0) {
            float bh = gui_s(3);
            GuiRect ab = { r.x + gui_s(5), r.y + r.h - gui_s(8), r.w * 0.5f, bh };
            gd_rect(ab, UI_RGBA(0, 0, 0, 120));
            u32 ac = v->ammo_frac < 0.25f ? TH->danger : TH->text_dim;
            gd_rect(gui_rect(ab.x, ab.y, ab.w * saturate(v->ammo_frac), bh), ac);
            char b[16];
            snprintf(b, sizeof(b), "%d", v->ammo);
            GuiText t = { fs * 0.92f, GW_MEDIUM, TH->text_dim, GUI_RIGHT, 0, false, true, 0 };
            gd_text(&t, r.x + r.w - gui_s(5), r.y + r.h - gd_line_height(t.size) - gui_s(3), b);
        }
    }
    if (v->hotkey > 0) {
        char b[4];
        snprintf(b, sizeof(b), "%d", v->hotkey);
        GuiText t = { fmaxf(gui_s(TH->t_tiny), r.h * 0.17f), GW_SEMIBOLD, v->selected ? TH->select : TH->text_faint, GUI_LEFT, 0, false, false, 0 };
        gd_text(&t, r.x + gui_s(5), r.y + gui_s(2), b);
    }
    gd_alpha_pop();
    if (v->focus) gw_focus_ring(r, rad, 1.0f);
}
