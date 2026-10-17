// Timed-action ring (src/ui/widgets/timed_ring.h).
#include "timed_ring.h"
#include "ui/gui/gui_widgets.h"
#include <string.h>

#define TH (gui_theme())

float timed_ring_update(TimedRingState *s, bool active, float duration, float remaining, int mode, const char *label, float dt) {
    if (active) {
        if (!s->active) { s->alpha = 0; s->cancelled = false; }
        s->active = true;
        s->duration = duration;
        s->remaining = remaining;
        s->mode = mode;
        if (label) dg_strcpy(s->label, sizeof(s->label), label);
        s->alpha = fminf(1.0f, s->alpha + dt / 0.12f);
    } else {
        if (s->active) s->cancelled = s->remaining > 0.05f;   // stopped before the end
        s->active = false;
        s->alpha = fmaxf(0.0f, s->alpha - dt / (s->cancelled ? 0.25f : 0.4f));
    }
    return s->alpha;
}

void ui_timed_ring_at(float cx, float cy, float radius_scale, float duration, float remaining, int mode, const char *label, float alpha) {
    if (alpha <= 0.01f) return;
    float r = gui_s(gui_tok("ring_radius")) * radius_scale;
    float th = fmaxf(1.5f, gui_s(gui_tok("ring_thickness_wheel")));
    float fill = timed_ring_fill(duration, remaining, mode);
    gd_alpha_push(alpha);
    // Track, then the white progress (clockwise from the top).
    gd_ring(cx, cy, r, th, 1.0f, gui_rgba_mul(TH->ring, 0.18f));
    if (fill > 0.001f) gd_ring(cx, cy, r, th, fill, TH->ring);
    char t[16];
    timed_ring_text(remaining, t, sizeof(t));
    GuiText num = { gui_s(TH->t_large) * radius_scale, GW_SEMIBOLD, TH->ring, GUI_CENTER, 0, false, true, 0 };
    float lh = gd_line_height(num.size);
    gd_text(&num, cx, cy - lh * 0.62f, t);
    if (label && label[0]) {
        GuiText lb = { gui_s(TH->t_label) * fmaxf(0.8f, radius_scale * 0.9f), GW_SEMIBOLD, TH->text_dim, GUI_CENTER, TH->tracking_caps, true, true, r * 1.6f };
        gd_text(&lb, cx, cy + lh * 0.38f, label);
    }
    gd_alpha_pop();
}

void ui_timed_ring(float duration, float remaining, const char *label) {
    ui_timed_ring_at(gui_w() * 0.5f, gui_h() * 0.5f + gui_s(gui_tok("ring_hud_offset")), 1.0f, duration, remaining, TR_DRAIN, label, 1.0f);
}

void ui_timed_ring_state(const TimedRingState *s, float cx, float cy, float radius_scale) {
    if (s->alpha <= 0.01f) return;
    ui_timed_ring_at(cx, cy, radius_scale, s->duration, s->remaining, s->mode, s->cancelled && !s->active ? "Cancelled" : s->label, s->alpha);
}
