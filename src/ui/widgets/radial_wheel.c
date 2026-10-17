// Radial wheel (src/ui/widgets/radial_wheel.h).
#include "radial_wheel.h"
#include "ui/gui/gui_widgets.h"
#include "ui/hud/hud_icons.h"
#include "ui/icons.h"
#include <math.h>
#include <string.h>

#define TH (gui_theme())

RadialParams radial_wheel_params(const RadialWheel *w) {
    RadialParams p;
    p.count = w->count;
    p.dead_zone = gui_s(gui_tok("wheel_dead_zone"));
    p.radius = gui_s(gui_tok("wheel_cursor"));
    p.hysteresis = gui_tok("wheel_hysteresis_deg") * DG_DEG2RAD;
    return p;
}

void radial_wheel_open(RadialWheel *w, const WheelDef *def, const WheelSlot *slots, int n, int initial) {
    float t = w->open ? w->t : 0.0f;
    memset(w, 0, sizeof(*w));
    w->def = def;
    w->count = DG_CLAMP(n, RADIAL_MIN_SLOTS, RADIAL_MAX_SLOTS);
    for (int i = 0; i < w->count; i++) {
        if (i < n) w->slots[i] = slots[i];
        else w->slots[i].empty = true;
    }
    w->sel.slot = initial >= 0 && initial < w->count ? initial : -1;
    w->last_slot = w->sel.slot;
    w->open = true;
    w->t = t;
    gui_sound(TH->snd_open);
}

static void changed(RadialWheel *w) {
    if (w->sel.slot != w->last_slot) {
        w->last_slot = w->sel.slot;
        if (w->sel.slot >= 0) gui_sound(TH->snd_hover);
    }
}

void radial_wheel_feed_mouse(RadialWheel *w, vec2 delta_px) {
    if (!w->open) return;
    RadialParams p = radial_wheel_params(w);
    radial_feed_delta(&w->sel, &p, delta_px);
    changed(w);
}

void radial_wheel_feed_stick(RadialWheel *w, vec2 stick) {
    if (!w->open) return;
    RadialParams p = radial_wheel_params(w);
    radial_feed_stick(&w->sel, &p, stick);
    changed(w);
}

void radial_wheel_pick(RadialWheel *w, int slot) {
    if (!w->open) return;
    RadialParams p = radial_wheel_params(w);
    radial_select(&w->sel, &p, slot);
    changed(w);
}

void radial_wheel_step(RadialWheel *w, int dir) {
    if (!w->open) return;
    RadialParams p = radial_wheel_params(w);
    radial_step(&w->sel, &p, dir);
    changed(w);
}

int radial_wheel_release(RadialWheel *w) {
    if (!w->open) return -1;
    w->open = false;
    int k = w->sel.slot;
    if (k < 0 || k >= w->count || !wheel_slot_selectable(&w->slots[k])) {
        if (k >= 0 && k < w->count && w->slots[k].disabled) gui_sound(TH->snd_error);
        return -1;
    }
    gui_sound(TH->snd_click);
    return k;
}

void radial_wheel_cancel(RadialWheel *w) {
    if (w->open) gui_sound(TH->snd_back);
    w->open = false;
}

void radial_wheel_tick(RadialWheel *w, float dt) {
    float k = dt / 0.12f;   // 120 ms open / close
    w->t = w->open ? fminf(1.0f, w->t + k) : fmaxf(0.0f, w->t - k);
    for (int i = 0; i < w->count; i++) {
        float target = i == w->sel.slot && w->open ? 1.0f : 0.0f;
        w->hl[i] += (target - w->hl[i]) * saturate(dt * 22.0f);
    }
}

bool radial_wheel_visible(const RadialWheel *w) { return w->t > 0.001f; }

// Arc (clockwise from the top, radians) as short segments.
static void arc(float cx, float cy, float r, float a0, float a1, float th, u32 c) {
    int n = DG_MAX(2, (int)((a1 - a0) / 0.06f));
    float px = cx + sinf(a0) * r, py = cy - cosf(a0) * r;
    for (int i = 1; i <= n; i++) {
        float a = a0 + (a1 - a0) * (float)i / (float)n;
        float x = cx + sinf(a) * r, y = cy - cosf(a) * r;
        gd_line(px, py, x, y, th, c);
        px = x;
        py = y;
    }
}

static void slot_icon(const WheelSlot *s, float cx, float cy, float size, u32 col) {
    if (s->icon[0] && hud_icon(s->icon, cx, cy, size * 0.62f, col)) return;
    TextureId tex;
    vec4 uv;
    if (s->item && icons_get(s->item, &tex, &uv)) {
        float h = size * 0.78f;
        gd_image(gui_rect(cx - h * 0.5f, cy - h * 0.5f, h, h), tex, uv, gui_rgba_mul(UI_RGBA(255, 255, 255, 255), (float)(col >> 24) / 255.0f));
        return;
    }
    // Fallback: initials.
    char ini[4] = { 0 };
    const char *l = s->label[0] ? s->label : s->id;
    ini[0] = l[0];
    const char *sp = strchr(l, ' ');
    if (sp && sp[1]) ini[1] = sp[1];
    GuiText t = { size * 0.34f, GW_SEMIBOLD, col, GUI_CENTER, 0.04f, true, false, 0 };
    gd_text(&t, cx, cy - gd_line_height(t.size) * 0.5f, ini);
}

void radial_wheel_draw(const RadialWheel *w, const TimedRingState *ring) {
    if (!radial_wheel_visible(w)) return;
    float e = gui_ease_out(w->t);
    float cx = gui_w() * 0.5f, cy = gui_h() * 0.5f;
    float sc = 0.86f + 0.14f * e;
    float R = gui_s(gui_tok("wheel_radius")) * sc, Ri = gui_s(gui_tok("wheel_inner")) * sc;
    float chip = gui_s(gui_tok("wheel_slot")) * sc;
    float rm = (R + Ri) * 0.5f;
    int n = w->count;
    float step = 2.0f * DG_PI / (float)n;
    gd_alpha_push(e);
    // Body: soft shadow, the dark disc, the outer track.
    for (int k = 4; k >= 1; k--) gd_circle(cx, cy, R + gui_s(7) * (float)k, UI_RGBA(0, 0, 0, 14));   // soft shadow
    gd_circle(cx, cy, R, TH->hud_panel);
    gd_ring(cx, cy, R, fmaxf(1.0f, gui_s(gui_tok("wheel_ring"))), 1.0f, UI_RGBA(255, 255, 255, 28));
    for (int i = 0; i < n; i++) {
        float a = (float)i * step + step * 0.5f;
        gd_line(cx + sinf(a) * (Ri + gui_s(6)), cy - cosf(a) * (Ri + gui_s(6)), cx + sinf(a) * (R - gui_s(6)), cy - cosf(a) * (R - gui_s(6)), 1.0f, UI_RGBA(255, 255, 255, 16));
    }
    // Selected slot: a white arc on the outer edge.
    if (w->sel.slot >= 0 && w->sel.slot < n) {
        float a = radial_slot_angle(w->sel.slot, n);
        float h = step * 0.5f - 0.03f;
        arc(cx, cy, R - gui_s(2.5f), a - h, a + h, fmaxf(2.0f, gui_s(gui_tok("bar_thickness")) * 1.6f), TH->select);
    }
    for (int i = 0; i < n; i++) {
        const WheelSlot *s = &w->slots[i];
        float a = radial_slot_angle(i, n);
        float hl = w->hl[i];
        float r = rm + hl * gui_s(6);
        float sx = cx + sinf(a) * r, sy = cy - cosf(a) * r;
        float cs = chip * (1.0f + hl * 0.08f);
        GuiRect cr = { sx - cs * 0.5f, sy - cs * 0.5f, cs, cs };
        float rad = cs * 0.5f;
        if (s->empty) {
            gd_round_outline(cr, rad, 1.0f, UI_RGBA(255, 255, 255, 30));
            continue;
        }
        u32 tint = s->color ? s->color : TH->hud_text;
        u32 bg = gui_rgba_lerp(UI_RGBA(255, 255, 255, 14), TH->select, hl);
        gd_round_border(cr, rad, fmaxf(1.0f, gui_s(1.2f)), bg, gui_rgba_lerp(gui_rgba_mul(tint, s->disabled ? 0.25f : 0.55f), TH->select, hl));
        u32 ic = s->disabled ? gui_rgba_mul(TH->text_faint, 0.8f) : gui_rgba_lerp(tint, TH->text_dark, hl);
        slot_icon(s, sx, sy, cs, ic);
        if (s->disabled) gd_line(cr.x + cs * 0.22f, cr.y + cs * 0.78f, cr.x + cs * 0.78f, cr.y + cs * 0.22f, fmaxf(1.0f, gui_s(1.5f)), gui_rgba_mul(TH->danger, 0.7f));
        if (s->count > 1) {
            char b[12];
            snprintf(b, sizeof(b), "%d", s->count);
            GuiText ct = { gui_s(TH->t_tiny), GW_SEMIBOLD, hl > 0.5f ? TH->text_dark : TH->hud_text, GUI_RIGHT, 0, false, hl < 0.5f, 0 };
            gd_text(&ct, cr.x + cs - gui_s(2), cr.y + cs - gd_line_height(ct.size) - gui_s(1), b);
        }
        // Number key (1..9) outside the chip, towards the rim.
        if (i < 9) {
            char k[4] = { (char)('1' + i), 0 };
            GuiText kt = { gui_s(TH->t_tiny), GW_SEMIBOLD, gui_rgba_mul(TH->text_faint, s->disabled ? 0.5f : 1.0f), GUI_CENTER, 0, false, false, 0 };
            float kr = R - gui_s(12);
            gd_text(&kt, cx + sinf(a) * kr, cy - cosf(a) * kr - gd_line_height(kt.size) * 0.5f, k);
        }
    }
    // Centre disc: the selection (or the running action's ring).
    gd_circle(cx, cy, Ri - gui_s(4), UI_RGBA(0, 0, 0, 90));
    gd_ring(cx, cy, Ri - gui_s(4), 1.0f, 1.0f, UI_RGBA(255, 255, 255, 22));
    float tw = (Ri - gui_s(10)) * 1.7f;
    if (ring && ring->alpha > 0.01f) {
        ui_timed_ring_state(ring, cx, cy, (Ri * 0.62f) / fmaxf(1.0f, gui_s(gui_tok("ring_radius"))));
    } else {
        const WheelSlot *s = w->sel.slot >= 0 && w->sel.slot < n ? &w->slots[w->sel.slot] : NULL;
        if (w->def && w->def->title[0]) {
            GuiText tt = { gui_s(TH->t_tiny), GW_SEMIBOLD, TH->text_faint, GUI_CENTER, TH->tracking_caps * 1.3f, true, false, tw };
            gd_text(&tt, cx, cy - Ri * 0.62f, w->def->title);
        }
        if (s && !s->empty) {
            GuiText nt = { gui_s(TH->t_body + 1), GW_SEMIBOLD, s->disabled ? TH->text_dim : TH->select, GUI_CENTER, TH->tracking_caps, true, true, tw };
            float y = cy - gui_s(20);
            gd_text(&nt, cx, y, s->label);
            y += gd_line_height(nt.size) + gui_s(2);
            if (s->disabled && s->reason[0]) {
                GuiText rt = { gui_s(TH->t_small), GW_MEDIUM, TH->warning, GUI_CENTER, 0, false, true, tw };
                gd_text(&rt, cx, y, s->reason);
            } else if (s->desc[0]) {
                GuiText dt = { gui_s(TH->t_tiny + 1), GW_REGULAR, TH->text_dim, GUI_CENTER, 0, false, true, 0 };
                gd_text_wrap(&dt, cx - tw * 0.5f, y, tw, s->desc, true);
            }
        } else {
            GuiText et = { gui_s(TH->t_small), GW_MEDIUM, TH->text_faint, GUI_CENTER, 0, false, true, tw };
            gd_text(&et, cx, cy - gd_line_height(et.size) * 0.5f, s && s->empty ? "Empty" : "Choose");
        }
    }
    gd_alpha_pop();
}
