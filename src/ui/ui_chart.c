// Chart primitives (ui_chart.h): cached triangle / label buffers built in the chart's local space
// (so a scrolled chart is not rebuilt), drawn every frame at the rect's position.
#include "ui_chart.h"
#include "ui/gui/gui.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TH (gui_theme())
#define TAU 6.2831853f

ChartStyle chart_style_default(void) {
    ChartStyle s;
    memset(&s, 0, sizeof(s));
    s.text = TH->text;
    s.text_dim = TH->text_dim;
    s.grid = UI_RGBA(255, 255, 255, 16);
    s.axis = TH->panel_border;
    s.panel = TH->panel;
    s.accent = TH->accent;
    s.good = TH->ok;
    s.bad = TH->danger;
    s.warn = TH->warning;
    s.marker = TH->info;
    s.empty = UI_RGBA(255, 255, 255, 10);
    // Categorical: the accent first, then hues far apart on a dark panel (weapons, players).
    const u32 series[CHART_MAX_SERIES] = { TH->accent, UI_RGBA(92, 152, 232, 255), UI_RGBA(104, 192, 112, 255), UI_RGBA(232, 198, 82, 255),
                                           UI_RGBA(164, 112, 224, 255), UI_RGBA(226, 86, 76, 255), UI_RGBA(62, 192, 180, 255), UI_RGBA(228, 112, 172, 255),
                                           UI_RGBA(172, 212, 82, 255), UI_RGBA(172, 122, 82, 255), UI_RGBA(112, 202, 236, 255), UI_RGBA(152, 152, 162, 255) };
    memcpy(s.series, series, sizeof(series));
    s.font = gui_s(TH->t_small);
    s.font_small = gui_s(TH->t_tiny);
    s.pad = gui_s(6);
    s.line_w = gui_s(2.0f);
    s.point_r = gui_s(3.2f);
    return s;
}

static u32 with_alpha(u32 c, float a) { return (c & 0x00FFFFFFu) | ((u32)DG_CLAMP((int)((float)(c >> 24) * a), 0, 255) << 24); }

// ---------------------------------------------------------------- cache building

static ChartCache *B;   // the cache being built
static vec2 g_uv;

static bool grow(void **p, int *cap, int need, size_t elem) {
    if (need <= *cap) return true;
    int nc = DG_MAX(need, DG_MAX(64, *cap * 2));
    void *np = realloc(*p, (size_t)nc * elem);
    if (!np) return false;
    *p = np;
    *cap = nc;
    return true;
}

void chart_cache_free(ChartCache *c) {
    free(c->v);
    free(c->idx);
    free(c->labels);
    free(c->hx);
    free(c->hy);
    memset(c, 0, sizeof(*c));
}

void chart_cache_invalidate(ChartCache *c) { c->built = false; }

static bool needs_build(ChartCache *c, GuiRect r, u32 rev) {
    return !c->built || c->rev != rev || fabsf(c->rect.w - r.w) > 0.5f || fabsf(c->rect.h - r.h) > 0.5f;
}

static void begin(ChartCache *c, GuiRect r, u32 rev) {
    B = c;
    c->nv = c->ni = c->nl = 0;
    c->rect = r;
    c->rev = rev;
    c->built = true;
    c->n = 0;
    memset(c->aux, 0, sizeof(c->aux));
    g_uv = gd_white_uv();
}

static bool hover_alloc(int n) {
    if (!grow((void **)&B->hx, &B->cap_h, n, sizeof(float))) return false;
    int cap = B->cap_h;
    void *ny = realloc(B->hy, (size_t)cap * sizeof(float));
    if (!ny) return false;
    B->hy = (float *)ny;
    return true;
}

static int vtx(float x, float y, u32 c) {
    if (!grow((void **)&B->v, &B->cap_v, B->nv + 1, sizeof(UiVertex))) return 0;
    UiVertex *v = &B->v[B->nv];
    v->pos = v2(x, y);
    v->uv = g_uv;
    v->color = c;
    return B->nv++;
}

static void tri_i(int a, int b, int c) {
    if (!grow((void **)&B->idx, &B->cap_i, B->ni + 3, sizeof(u32))) return;
    B->idx[B->ni++] = (u32)a;
    B->idx[B->ni++] = (u32)b;
    B->idx[B->ni++] = (u32)c;
}

static void quad_i(int a, int b, int c, int d) { tri_i(a, b, c); tri_i(a, c, d); }

static void quad(float x0, float y0, float x1, float y1, u32 c) {
    if ((c >> 24) == 0 || x1 <= x0 || y1 <= y0) return;
    int a = vtx(x0, y0, c), b = vtx(x1, y0, c), d = vtx(x1, y1, c), e = vtx(x0, y1, c);
    quad_i(a, b, d, e);
}

static void quad_vgrad(float x0, float y0, float x1, float y1, u32 top, u32 bottom) {
    int a = vtx(x0, y0, top), b = vtx(x1, y0, top), d = vtx(x1, y1, bottom), e = vtx(x0, y1, bottom);
    quad_i(a, b, d, e);
}

static void tri(float x0, float y0, float x1, float y1, float x2, float y2, u32 c) { tri_i(vtx(x0, y0, c), vtx(x1, y1, c), vtx(x2, y2, c)); }

// Anti-aliased segment: a core and a 1 px feather on each side.
static void seg(float x0, float y0, float x1, float y1, float w, u32 c) {
    float dx = x1 - x0, dy = y1 - y0, l = sqrtf(dx * dx + dy * dy);
    if (l < 1e-3f || (c >> 24) == 0) return;
    float nx = -dy / l, ny = dx / l;
    float core = fmaxf(0.0f, w * 0.5f - 0.5f), outer = core + 1.0f;
    u32 ce = w < 1.0f ? with_alpha(c, fmaxf(w, 0.25f)) : c, cf = c & 0x00FFFFFFu;
    int a0 = vtx(x0 + nx * outer, y0 + ny * outer, cf), a1 = vtx(x0 + nx * core, y0 + ny * core, ce);
    int a2 = vtx(x0 - nx * core, y0 - ny * core, ce), a3 = vtx(x0 - nx * outer, y0 - ny * outer, cf);
    int b0 = vtx(x1 + nx * outer, y1 + ny * outer, cf), b1 = vtx(x1 + nx * core, y1 + ny * core, ce);
    int b2 = vtx(x1 - nx * core, y1 - ny * core, ce), b3 = vtx(x1 - nx * outer, y1 - ny * outer, cf);
    quad_i(a0, b0, b1, a1);
    quad_i(a1, b1, b2, a2);
    quad_i(a2, b2, b3, a3);
}

static void dashed(float x0, float y0, float x1, float y1, float w, u32 c, float dash) {
    float dx = x1 - x0, dy = y1 - y0, l = sqrtf(dx * dx + dy * dy);
    if (l < 1e-3f) return;
    for (float t = 0; t < l; t += dash * 2) {
        float e = fminf(l, t + dash);
        seg(x0 + dx * t / l, y0 + dy * t / l, x0 + dx * e / l, y0 + dy * e / l, w, c);
    }
}

static void disc(float cx, float cy, float r, u32 c) {
    int segs = r < 4 ? 10 : r < 10 ? 16 : 28;
    int center = vtx(cx, cy, c);
    int first = B->nv;
    for (int i = 0; i < segs; i++) {
        float a = TAU * (float)i / (float)segs;
        vtx(cx + cosf(a) * r, cy + sinf(a) * r, c);
        vtx(cx + cosf(a) * (r + 1.0f), cy + sinf(a) * (r + 1.0f), c & 0x00FFFFFFu);
    }
    for (int i = 0; i < segs; i++) {
        int j = (i + 1) % segs;
        int ri = first + i * 2, rj = first + j * 2;
        tri_i(center, ri, rj);
        quad_i(ri, ri + 1, rj + 1, rj);
    }
}

// Ring band between radii r0..r1 over angles a0..a1 (clockwise from the top), feathered edges.
static void arc(float cx, float cy, float r0, float r1, float a0, float a1, u32 c) {
    if (a1 <= a0 || (c >> 24) == 0) return;
    int segs = DG_MAX(2, (int)ceilf((a1 - a0) / TAU * 96.0f));
    u32 cf = c & 0x00FFFFFFu;
    int prev[4] = { 0 };
    for (int i = 0; i <= segs; i++) {
        float a = a0 + (a1 - a0) * (float)i / (float)segs - TAU * 0.25f;
        float ca = cosf(a), sa = sinf(a);
        int cur[4] = { vtx(cx + ca * fmaxf(0, r0 - 1), cy + sa * fmaxf(0, r0 - 1), cf), vtx(cx + ca * r0, cy + sa * r0, c),
                       vtx(cx + ca * r1, cy + sa * r1, c), vtx(cx + ca * (r1 + 1), cy + sa * (r1 + 1), cf) };
        if (i > 0) {
            quad_i(prev[0], cur[0], cur[1], prev[1]);
            quad_i(prev[1], cur[1], cur[2], prev[2]);
            quad_i(prev[2], cur[2], cur[3], prev[3]);
        }
        memcpy(prev, cur, sizeof(prev));
    }
}

static void label(float x, float y, float size, u32 color, int align, int weight, const char *text) {
    if (!text || !text[0]) return;
    if (!grow((void **)&B->labels, &B->cap_l, B->nl + 1, sizeof(ChartLabel))) return;
    ChartLabel *l = &B->labels[B->nl++];
    l->x = x;
    l->y = y;
    l->size = size;
    l->color = color;
    l->align = (u8)align;
    l->weight = (u8)weight;
    dg_strcpy(l->text, sizeof(l->text), text);
}

static float text_w(float size, const char *s) {
    GuiText t = { size, GW_REGULAR, 0, GUI_LEFT, 0, false, false, 0 };
    return gd_text_width(&t, s);
}

static void placeholder(GuiRect r, const ChartStyle *st, const char *text) {
    quad(0, 0, r.w, r.h, st->empty);
    label(r.w * 0.5f, r.h * 0.5f - gd_line_height(st->font) * 0.5f, st->font, st->text_dim, GUI_CENTER, GW_REGULAR, text ? text : "No data yet");
}

// Draw the cached buffers at r's position.
static UiVertex *g_scratch;
static int g_scratch_cap;

static void draw_cache(const ChartCache *c, GuiRect r) {
    if (c->nv && c->ni && grow((void **)&g_scratch, &g_scratch_cap, c->nv, sizeof(UiVertex))) {
        for (int i = 0; i < c->nv; i++) {
            g_scratch[i] = c->v[i];
            g_scratch[i].pos.x += r.x;
            g_scratch[i].pos.y += r.y;
        }
        gd_tris(gd_atlas(), g_scratch, c->nv, c->idx, c->ni);
    }
    for (int i = 0; i < c->nl; i++) {
        const ChartLabel *l = &c->labels[i];
        GuiText t = { l->size, l->weight, l->color, l->align, 0, false, false, 0 };
        gd_text(&t, r.x + l->x, r.y + l->y, l->text);
    }
}

static vec2 mouse_local(GuiRect r) {
    vec2 m = gui_mouse();
    return v2(m.x - r.x, m.y - r.y);
}

static bool mouse_in(GuiRect r) {
    vec2 m = gui_mouse();
    return gui_input_enabled() && gui_rect_contains(r, m.x, m.y) && gui_rect_contains(gd_clip(), m.x, m.y);
}

// ---------------------------------------------------------------- shared helpers

void chart_tooltip(const char *text) {
    if (!text || !text[0]) return;
    int layer = gd_layer();
    gd_set_layer(GL_TOP);
    float size = gui_s(TH->t_small), lh = gd_line_height(size), pad = gui_s(7);
    GuiText t = { size, GW_MEDIUM, TH->text, GUI_LEFT, 0, false, false, 0 };
    char lines[8][96];
    int n = 0;
    float w = 0;
    for (const char *p = text; *p && n < 8;) {
        const char *e = strchr(p, '\n');
        int len = e ? (int)(e - p) : (int)strlen(p);
        len = DG_MIN(len, 95);
        memcpy(lines[n], p, (size_t)len);
        lines[n][len] = 0;
        w = fmaxf(w, gd_text_width(&t, lines[n]));
        n++;
        if (!e) break;
        p = e + 1;
    }
    vec2 m = gui_mouse();
    GuiRect r = { m.x + gui_s(14), m.y + gui_s(10), w + pad * 2, lh * (float)n + pad * 2 };
    if (r.x + r.w > gui_w() - 4) r.x = m.x - r.w - gui_s(10);
    if (r.y + r.h > gui_h() - 4) r.y = m.y - r.h - gui_s(6);
    gd_shadow(r, gui_s(TH->radius), gui_s(10), TH->shadow);
    gd_round_border(r, gui_s(TH->radius), 1.0f, UI_RGBA(18, 19, 20, 245), TH->panel_border);
    for (int i = 0; i < n; i++) {
        t.color = i == 0 ? TH->text : TH->text_dim;
        t.weight = i == 0 ? GW_SEMIBOLD : GW_REGULAR;
        gd_text(&t, r.x + pad, r.y + pad + lh * (float)i, lines[i]);
    }
    gd_set_layer(layer);
}

void chart_legend(GuiRect r, const ChartStyle *st, const char *const *names, const u32 *colors, int n) {
    float x = r.x, sw = st->font * 0.7f, lh = gd_line_height(st->font_small);
    float y = r.y + (r.h - lh) * 0.5f;
    GuiText t = { st->font_small, GW_REGULAR, st->text_dim, GUI_LEFT, 0, false, false, 0 };
    for (int i = 0; i < n; i++) {
        if (!names[i]) continue;
        float tw = gd_text_width(&t, names[i]);
        if (x + sw + 4 + tw > r.x + r.w) break;
        gd_round(gui_rect(x, y + (lh - sw) * 0.5f, sw, sw), sw * 0.25f, colors[i]);
        gd_text(&t, x + sw + gui_s(5), y, names[i]);
        x += sw + gui_s(5) + tw + gui_s(14);
    }
}

static const char *default_fmt(double v, char *buf, int size) { return chart_fmt_compact(buf, size, v); }

static float map_y(double v, const ChartTicks *t, GuiRect p) {
    double span = t->hi - t->lo;
    float f = span > 0 ? (float)((v - t->lo) / span) : 0.5f;
    return p.y + p.h - f * p.h;
}

// Y axis ticks + grid (left or right side).
static void y_axis(const ChartTicks *t, GuiRect p, const ChartStyle *st, bool right, bool grid, ChartValueFn fmt, void *user) {
    char b[32];
    float lh = gd_line_height(st->font_small);
    for (int i = 0; i < t->count; i++) {
        double v = t->lo + t->step * i;
        float y = map_y(v, t, p);
        if (grid) quad(p.x, y - 0.5f, p.x + p.w, y + 0.5f, i == 0 ? st->axis : st->grid);
        const char *s = fmt ? fmt(v, b, sizeof(b), user) : default_fmt(v, b, sizeof(b));
        if (right) label(p.x + p.w + st->pad, y - lh * 0.5f, st->font_small, st->text_dim, GUI_LEFT, GW_REGULAR, s);
        else label(p.x - st->pad, y - lh * 0.5f, st->font_small, st->text_dim, GUI_RIGHT, GW_REGULAR, s);
    }
}

static float axis_label_w(const ChartTicks *t, const ChartStyle *st, ChartValueFn fmt, void *user) {
    char b[32];
    float w = 0;
    for (int i = 0; i < t->count; i++) {
        double v = t->lo + t->step * i;
        w = fmaxf(w, text_w(st->font_small, fmt ? fmt(v, b, sizeof(b), user) : default_fmt(v, b, sizeof(b))));
    }
    return w;
}

static int tick_count_for(float h, const ChartStyle *st) { return DG_CLAMP((int)(h / (gd_line_height(st->font_small) * 2.4f)) + 1, 2, 7); }

// X labels for n evenly spaced points (thinned to fit).
static void x_labels(int n, GuiRect p, const ChartStyle *st, ChartLabelFn fn, void *user, bool centered_slots) {
    if (n <= 0) return;
    char b[40];
    float slot = centered_slots ? p.w / (float)n : (n > 1 ? p.w / (float)(n - 1) : p.w);
    float lw = text_w(st->font_small, "00 Mmm") + st->pad;
    int every = DG_MAX(1, (int)ceilf(lw / fmaxf(1.0f, slot)));
    for (int i = 0; i < n; i += every) {
        float x = centered_slots ? p.x + slot * ((float)i + 0.5f) : (n > 1 ? p.x + slot * (float)i : p.x + p.w * 0.5f);
        const char *s = fn ? fn(i, b, sizeof(b), user) : (snprintf(b, sizeof(b), "%d", i + 1), b);
        label(x, p.y + p.h + st->pad * 0.6f, st->font_small, st->text_dim, GUI_CENTER, GW_REGULAR, s);
    }
}

// ---------------------------------------------------------------- line

static void minmax(const float *y, int n, double *lo, double *hi) {
    for (int i = 0; i < n; i++) {
        if (isnan(y[i])) continue;
        if (y[i] < *lo) *lo = y[i];
        if (y[i] > *hi) *hi = y[i];
    }
}

static void polyline_series(const float *y, int n, GuiRect p, const ChartTicks *t, float w, u32 c, bool dash, float *px_out) {
    float prevx = 0, prevy = 0;
    bool have = false;
    for (int i = 0; i < n; i++) {
        float x = n > 1 ? p.x + p.w * (float)i / (float)(n - 1) : p.x + p.w * 0.5f;
        if (px_out) px_out[i] = x;
        if (isnan(y[i])) { have = false; continue; }
        float yy = map_y(y[i], t, p);
        if (have) {
            if (dash) dashed(prevx, prevy, x, yy, w, c, gui_s(4));
            else seg(prevx, prevy, x, yy, w, c);
        }
        prevx = x;
        prevy = yy;
        have = true;
    }
}

static void fill_series(const float *y, int n, GuiRect p, const ChartTicks *t, u32 c) {
    float base = map_y(DG_CLAMP(0.0, t->lo, t->hi), t, p);
    for (int i = 0; i + 1 < n; i++) {
        if (isnan(y[i]) || isnan(y[i + 1])) continue;
        float x0 = p.x + p.w * (float)i / (float)(n - 1), x1 = p.x + p.w * (float)(i + 1) / (float)(n - 1);
        float y0 = map_y(y[i], t, p), y1 = map_y(y[i + 1], t, p);
        int a = vtx(x0, y0, with_alpha(c, 0.30f)), b = vtx(x1, y1, with_alpha(c, 0.30f));
        int d = vtx(x1, base, with_alpha(c, 0.03f)), e = vtx(x0, base, with_alpha(c, 0.03f));
        quad_i(a, b, d, e);
    }
}

int chart_line(ChartCache *c, u32 rev, GuiRect r, const ChartStyle *st, const ChartLineSpec *sp) {
    if (needs_build(c, r, rev)) {
        begin(c, r, rev);
        GuiRect lr = { 0, 0, r.w, r.h };
        bool any = false;
        for (int s = 0; s < sp->nseries; s++)
            for (int i = 0; i < sp->n && !any; i++) any = !isnan(sp->s[s].y[i]);
        if (sp->n <= 0 || !any) placeholder(lr, st, sp->empty);
        else {
            // Ranges: left (and the moving averages, within the series' range anyway) / right axes.
            double lo[2] = { 1e30, 1e30 }, hi[2] = { -1e30, -1e30 };
            bool has_right = false;
            for (int s = 0; s < sp->nseries; s++) {
                int ax = sp->s[s].right_axis ? 1 : 0;
                has_right |= ax == 1;
                minmax(sp->s[s].y, sp->n, &lo[ax], &hi[ax]);
            }
            if (lo[0] > hi[0]) { lo[0] = 0; hi[0] = 1; }
            if (lo[1] > hi[1]) { lo[1] = 0; hi[1] = 1; }
            float lh = gd_line_height(st->font_small);
            int nt = tick_count_for(r.h - lh * 2, st);
            ChartTicks tl = chart_nice_ticks(lo[0], hi[0], nt, sp->include_zero);
            ChartTicks tr = chart_nice_ticks(lo[1], hi[1], nt, sp->include_zero);
            float lw = axis_label_w(&tl, st, sp->y_fmt, sp->user) + st->pad * 1.5f;
            float rw = has_right ? axis_label_w(&tr, st, sp->y2_fmt, sp->user) + st->pad * 1.5f : st->pad;
            GuiRect p = { lw, lh * 0.6f, r.w - lw - rw, r.h - lh * 0.6f - lh - st->pad };
            c->plot = p;
            y_axis(&tl, p, st, false, true, sp->y_fmt, sp->user);
            if (has_right) y_axis(&tr, p, st, true, false, sp->y2_fmt, sp->user);
            x_labels(sp->n, p, st, sp->x_label, sp->user, false);
            hover_alloc(sp->n);
            c->n = sp->n;
            float *ma = (float *)malloc(sizeof(float) * (size_t)sp->n);
            for (int s = 0; s < sp->nseries; s++) {
                const ChartSeries *se = &sp->s[s];
                const ChartTicks *t = se->right_axis ? &tr : &tl;
                u32 col = se->color ? se->color : st->series[s % CHART_MAX_SERIES];
                if (se->fill && sp->n > 1) fill_series(se->y, sp->n, p, t, col);
                bool ma_on = se->ma_window > 1 && ma;
                polyline_series(se->y, sp->n, p, t, st->line_w, ma_on ? with_alpha(col, 0.45f) : col, se->dashed, s == 0 ? c->hx : NULL);
                if (ma_on) {
                    chart_moving_average(se->y, sp->n, se->ma_window, ma);
                    polyline_series(ma, sp->n, p, t, st->line_w * 1.25f, col, false, NULL);
                }
                if (s == 0)
                    for (int i = 0; i < sp->n; i++) c->hy[i] = isnan(se->y[i]) ? NAN : map_y(se->y[i], t, p);
            }
            free(ma);
            // Points on series 0: win / loss colours, or plain dots when sparse.
            if (sp->point_class || sp->n <= 40) {
                for (int i = 0; i < sp->n; i++) {
                    if (isnan(c->hy[i])) continue;
                    u32 col = sp->s[0].color ? sp->s[0].color : st->series[0];
                    if (sp->point_class) col = sp->point_class[i] == 1 ? st->good : sp->point_class[i] == 2 ? st->bad : sp->point_class[i] == 3 ? st->text_dim : col;
                    disc(c->hx[i], c->hy[i], st->point_r, col);
                }
            }
        }
    }
    draw_cache(c, r);
    if (!c->n || !mouse_in(r)) return -1;
    vec2 m = mouse_local(r);
    GuiRect p = c->plot;
    if (m.x < p.x - 6 || m.x > p.x + p.w + 6) return -1;
    int i = chart_nearest_index(m.x, p.x, p.x + p.w, c->n);
    if (i < 0) return -1;
    gd_rect(gui_rect(r.x + c->hx[i] - 0.5f, r.y + p.y, 1, p.h), with_alpha(st->text, 0.35f));
    if (!isnan(c->hy[i])) gd_circle(r.x + c->hx[i], r.y + c->hy[i], st->point_r * 1.6f, st->text);
    return i;
}

int chart_sparkline(ChartCache *c, u32 rev, GuiRect r, const ChartStyle *st, const float *y, int n, u32 color) {
    if (needs_build(c, r, rev)) {
        begin(c, r, rev);
        double lo = 1e30, hi = -1e30;
        minmax(y, n, &lo, &hi);
        if (n >= 2 && lo <= hi) {
            double pad = (hi - lo) * 0.15 + 1e-6;
            ChartTicks t = { lo - pad, hi + pad, 1, 2 };
            float dot = st->point_r * 0.9f;
            GuiRect p = { dot, dot, r.w - dot * 2.5f, r.h - dot * 2 };
            fill_series(y, n, p, &t, color);
            polyline_series(y, n, p, &t, fmaxf(1.0f, st->line_w * 0.75f), color, false, NULL);
            for (int i = n - 1; i >= 0; i--) {
                if (isnan(y[i])) continue;
                float x = p.x + p.w * (float)i / (float)(n - 1);
                disc(x, map_y(y[i], &t, p), dot, color);
                break;
            }
        } else quad(0, r.h * 0.5f - 0.5f, r.w, r.h * 0.5f + 0.5f, st->grid);
        c->n = n;
    }
    draw_cache(c, r);
    return -1;
}

// ---------------------------------------------------------------- bar

int chart_bar(ChartCache *c, u32 rev, GuiRect r, const ChartStyle *st, const ChartBarSpec *sp) {
    int groups = DG_MAX(1, sp->groups);
    if (needs_build(c, r, rev)) {
        begin(c, r, rev);
        GuiRect lr = { 0, 0, r.w, r.h };
        double lo = 0, hi = -1e30;
        for (int i = 0; i < sp->n * groups; i++) if (!isnan(sp->v[i])) hi = fmax(hi, sp->v[i]);
        if (!isnan(sp->ref)) hi = fmax(hi, sp->ref);
        if (sp->n <= 0 || hi <= 0) placeholder(lr, st, sp->empty);
        else {
            char b[48];
            float lh = gd_line_height(st->font_small);
            hover_alloc(sp->n);
            c->n = sp->n;
            if (sp->horizontal) {
                float labw = 0;
                for (int i = 0; i < sp->n; i++) labw = fmaxf(labw, text_w(st->font_small, sp->label ? sp->label(i, b, sizeof(b), sp->user) : ""));
                labw = fminf(labw + st->pad * 1.5f, r.w * 0.38f);
                float valw = sp->value_labels ? text_w(st->font_small, "00.0 %") + st->pad : st->pad;
                ChartTicks t = chart_nice_ticks(lo, hi, DG_CLAMP((int)((r.w - labw) / gui_s(70)), 2, 6), true);
                float top = !isnan(sp->ref) && sp->ref_label ? lh * 1.2f : 0.0f;   // the reference label above the bars
                GuiRect p = { labw, top, r.w - labw - valw, r.h - top - lh - st->pad };
                c->plot = p;
                for (int k = 0; k < t.count; k++) {
                    double v = t.lo + t.step * k;
                    float x = p.x + p.w * (float)((v - t.lo) / (t.hi - t.lo));
                    quad(x - 0.5f, p.y, x + 0.5f, p.y + p.h, k == 0 ? st->axis : st->grid);
                    label(x, p.y + p.h + st->pad * 0.5f, st->font_small, st->text_dim, GUI_CENTER, GW_REGULAR, sp->fmt ? sp->fmt(v, b, sizeof(b), sp->user) : default_fmt(v, b, sizeof(b)));
                }
                float row = p.h / (float)sp->n, bh = fminf(row * 0.72f, gui_s(22)), gh = bh / (float)groups;
                for (int i = 0; i < sp->n; i++) {
                    float y0 = p.y + row * (float)i + (row - bh) * 0.5f;
                    c->hx[i] = p.x;
                    c->hy[i] = p.y + row * ((float)i + 0.5f);
                    label(p.x - st->pad, c->hy[i] - lh * 0.5f, st->font_small, i == sp->highlight ? st->text : st->text_dim, GUI_RIGHT, GW_REGULAR, sp->label ? sp->label(i, b, sizeof(b), sp->user) : "");
                    for (int g = 0; g < groups; g++) {
                        float v = sp->v[i * groups + g];
                        if (isnan(v) || v <= 0) continue;
                        u32 col = sp->bar_colors ? sp->bar_colors[i] : sp->group_colors ? sp->group_colors[g] : st->series[g % CHART_MAX_SERIES];
                        if (sp->highlight >= 0 && i != sp->highlight) col = with_alpha(col, 0.55f);
                        float x1 = p.x + p.w * (float)((v - t.lo) / (t.hi - t.lo));
                        quad(p.x, y0 + gh * (float)g, x1, y0 + gh * (float)(g + 1) - (groups > 1 ? 1.0f : 0.0f), col);
                        if (sp->value_labels && g == groups - 1)
                            label(x1 + st->pad * 0.6f, c->hy[i] - lh * 0.5f, st->font_small, st->text_dim, GUI_LEFT, GW_REGULAR, sp->fmt ? sp->fmt(v, b, sizeof(b), sp->user) : default_fmt(v, b, sizeof(b)));
                    }
                }
                if (!isnan(sp->ref)) {
                    float x = p.x + p.w * (float)((sp->ref - t.lo) / (t.hi - t.lo));
                    dashed(x, p.y - lh * 0.2f, x, p.y + p.h, fmaxf(1.0f, st->line_w * 0.75f), st->text, gui_s(4));
                    bool right = x > p.x + p.w * 0.7f;
                    if (sp->ref_label) label(right ? x - st->pad * 0.5f : x + st->pad * 0.5f, p.y - lh * 1.15f, st->font_small, st->text, right ? GUI_RIGHT : GUI_LEFT, GW_MEDIUM, sp->ref_label);
                }
            } else {
                ChartTicks t = chart_nice_ticks(lo, hi, tick_count_for(r.h - lh * 2, st), true);
                float lw = axis_label_w(&t, st, sp->fmt, sp->user) + st->pad * 1.5f;
                GuiRect p = { lw, lh * 0.6f, r.w - lw - st->pad, r.h - lh * 0.6f - lh - st->pad };
                c->plot = p;
                y_axis(&t, p, st, false, true, sp->fmt, sp->user);
                x_labels(sp->n, p, st, sp->label, sp->user, true);
                float slot = p.w / (float)sp->n, bw = fminf(slot * 0.72f, gui_s(40)), gw = bw / (float)groups;
                for (int i = 0; i < sp->n; i++) {
                    float x0 = p.x + slot * (float)i + (slot - bw) * 0.5f;
                    c->hx[i] = p.x + slot * ((float)i + 0.5f);
                    c->hy[i] = p.y;
                    for (int g = 0; g < groups; g++) {
                        float v = sp->v[i * groups + g];
                        if (isnan(v) || v <= 0) continue;
                        u32 col = sp->bar_colors ? sp->bar_colors[i] : sp->group_colors ? sp->group_colors[g] : st->series[g % CHART_MAX_SERIES];
                        if (sp->highlight >= 0 && i != sp->highlight) col = with_alpha(col, 0.55f);
                        quad(x0 + gw * (float)g, map_y(v, &t, p), x0 + gw * (float)(g + 1) - (groups > 1 ? 1.0f : 0.0f), p.y + p.h, col);
                    }
                }
                if (!isnan(sp->ref)) {
                    float y = map_y(sp->ref, &t, p);
                    dashed(p.x, y, p.x + p.w, y, fmaxf(1.0f, st->line_w * 0.75f), st->text, gui_s(4));
                    if (sp->ref_label) label(p.x + p.w, y - lh - 1, st->font_small, st->text, GUI_RIGHT, GW_MEDIUM, sp->ref_label);
                }
            }
        }
    }
    draw_cache(c, r);
    if (!c->n || !mouse_in(r)) return -1;
    vec2 m = mouse_local(r);
    GuiRect p = c->plot;
    if (sp->horizontal) {
        if (m.y < p.y || m.y >= p.y + p.h) return -1;
        int i = (int)((m.y - p.y) / (p.h / (float)c->n));
        return DG_CLAMP(i, 0, c->n - 1);
    }
    if (m.x < p.x || m.x >= p.x + p.w) return -1;
    int i = (int)((m.x - p.x) / (p.w / (float)c->n));
    return DG_CLAMP(i, 0, c->n - 1);
}

// ---------------------------------------------------------------- stacked area

int chart_stacked_area(ChartCache *c, u32 rev, GuiRect r, const ChartStyle *st, const ChartStackSpec *sp, int *series_out) {
    if (series_out) *series_out = -1;
    if (needs_build(c, r, rev)) {
        begin(c, r, rev);
        GuiRect lr = { 0, 0, r.w, r.h };
        double hi = 0;
        for (int i = 0; i < sp->n; i++) {
            double sum = 0;
            for (int s = 0; s < sp->nseries; s++) sum += fmax(0.0, sp->y[s][i]);
            hi = fmax(hi, sum);
        }
        if (sp->n < 2 || hi <= 0) placeholder(lr, st, sp->empty);
        else {
            float lh = gd_line_height(st->font_small);
            ChartTicks t = chart_nice_ticks(0, hi, tick_count_for(r.h - lh * 2, st), true);
            float lw = axis_label_w(&t, st, NULL, NULL) + st->pad * 1.5f;
            GuiRect p = { lw, lh * 0.6f, r.w - lw - st->pad, r.h - lh * 0.6f - lh - st->pad };
            c->plot = p;
            y_axis(&t, p, st, false, true, NULL, NULL);
            x_labels(sp->n, p, st, sp->x_label, sp->user, false);
            c->n = sp->n;
            float *base = (float *)calloc((size_t)sp->n, sizeof(float));
            if (base) {
                for (int s = 0; s < sp->nseries; s++) {
                    u32 col = sp->colors ? sp->colors[s] : st->series[s % CHART_MAX_SERIES];
                    for (int i = 0; i + 1 < sp->n; i++) {
                        float x0 = p.x + p.w * (float)i / (float)(sp->n - 1), x1 = p.x + p.w * (float)(i + 1) / (float)(sp->n - 1);
                        float b0 = base[i], b1 = base[i + 1];
                        float t0 = b0 + fmaxf(0.0f, sp->y[s][i]), t1 = b1 + fmaxf(0.0f, sp->y[s][i + 1]);
                        int a = vtx(x0, map_y(t0, &t, p), with_alpha(col, 0.78f)), b = vtx(x1, map_y(t1, &t, p), with_alpha(col, 0.78f));
                        int d = vtx(x1, map_y(b1, &t, p), with_alpha(col, 0.55f)), e = vtx(x0, map_y(b0, &t, p), with_alpha(col, 0.55f));
                        quad_i(a, b, d, e);
                        seg(x0, map_y(t0, &t, p), x1, map_y(t1, &t, p), 1.0f, col);
                    }
                    for (int i = 0; i < sp->n; i++) base[i] += fmaxf(0.0f, sp->y[s][i]);
                }
                free(base);
            }
            c->aux[0] = (float)t.lo;
            c->aux[1] = (float)t.hi;
        }
    }
    draw_cache(c, r);
    if (!c->n || !mouse_in(r)) return -1;
    vec2 m = mouse_local(r);
    GuiRect p = c->plot;
    int i = chart_nearest_index(m.x, p.x, p.x + p.w, c->n);
    if (i < 0) return -1;
    float x = p.x + p.w * (float)i / (float)(c->n - 1);
    gd_rect(gui_rect(r.x + x - 0.5f, r.y + p.y, 1, p.h), with_alpha(st->text, 0.35f));
    if (series_out && m.y >= p.y && m.y <= p.y + p.h) {
        double v = c->aux[0] + (c->aux[1] - c->aux[0]) * (double)(p.y + p.h - m.y) / (double)p.h, acc = 0;
        for (int s = 0; s < sp->nseries; s++) {
            acc += fmax(0.0, sp->y[s][i]);
            if (v <= acc) { *series_out = s; break; }
        }
    }
    return i;
}

// ---------------------------------------------------------------- donut

int chart_donut(ChartCache *c, u32 rev, GuiRect r, const ChartStyle *st, const ChartDonutSeg *seg_in, int n, const char *center, const char *center_sub, bool legend) {
    float vals[32];
    int nn = DG_MIN(n, 32);
    double total = 0;
    for (int i = 0; i < nn; i++) { vals[i] = fmaxf(0.0f, seg_in[i].value); total += vals[i]; }
    if (needs_build(c, r, rev)) {
        begin(c, r, rev);
        GuiRect lr = { 0, 0, r.w, r.h };
        if (total <= 0) placeholder(lr, st, "No data yet");
        else {
            float side = legend ? fminf(r.h, r.w * 0.5f) : fminf(r.w, r.h);
            float ro = side * 0.5f - st->pad, ri = ro * 0.62f;
            float cx = legend ? side * 0.5f : r.w * 0.5f, cy = r.h * 0.5f;
            c->aux[0] = cx; c->aux[1] = cy; c->aux[2] = ri; c->aux[3] = ro;
            double acc = 0;
            float gap = nn > 1 ? 0.012f : 0.0f;
            for (int i = 0; i < nn; i++) {
                if (vals[i] <= 0) continue;
                float a0 = (float)(acc / total) * TAU, a1 = (float)((acc + vals[i]) / total) * TAU;
                acc += vals[i];
                arc(cx, cy, ri, ro, a0 + gap, fmaxf(a0 + gap + 0.001f, a1 - gap), seg_in[i].color);
            }
            float lh = gd_line_height(st->font);
            if (center) label(cx, cy - lh * (center_sub ? 0.95f : 0.5f), st->font * 1.25f, st->text, GUI_CENTER, GW_SEMIBOLD, center);
            if (center_sub) label(cx, cy + lh * 0.15f, st->font_small, st->text_dim, GUI_CENTER, GW_REGULAR, center_sub);
            if (legend) {
                float lx = side + st->pad, llh = gd_line_height(st->font_small) * 1.35f;
                int shown = 0;
                for (int i = 0; i < nn; i++) if (vals[i] > 0) shown++;
                float y = cy - llh * (float)DG_MIN(shown, (int)(r.h / llh)) * 0.5f;
                for (int i = 0; i < nn && y + llh <= r.h; i++) {
                    if (vals[i] <= 0) continue;
                    char b[64], p[16];
                    float sw = st->font_small * 0.7f;
                    quad(lx, y + (llh - sw) * 0.5f, lx + sw, y + (llh + sw) * 0.5f, seg_in[i].color);
                    snprintf(b, sizeof(b), "%s", seg_in[i].label ? seg_in[i].label : "");
                    label(lx + sw + st->pad, y + (llh - gd_line_height(st->font_small)) * 0.5f, st->font_small, st->text_dim, GUI_LEFT, GW_REGULAR, b);
                    chart_fmt_pct(p, sizeof(p), vals[i] / total, vals[i] / total < 0.1 ? 1 : 0);
                    label(r.w - st->pad, y + (llh - gd_line_height(st->font_small)) * 0.5f, st->font_small, st->text, GUI_RIGHT, GW_MEDIUM, p);
                    y += llh;
                }
            }
            c->n = nn;
        }
    }
    draw_cache(c, r);
    if (!c->n || !mouse_in(r)) return -1;
    vec2 m = mouse_local(r);
    float dx = m.x - c->aux[0], dy = m.y - c->aux[1], d = sqrtf(dx * dx + dy * dy);
    if (d < c->aux[2] || d > c->aux[3] + 2) return -1;
    float a = atan2f(dx, -dy);   // clockwise from the top
    if (a < 0) a += TAU;
    return chart_donut_segment(vals, nn, a);
}

// ---------------------------------------------------------------- radar

int chart_radar(ChartCache *c, u32 rev, GuiRect r, const ChartStyle *st, const ChartRadarSpec *sp) {
    int n = DG_CLAMP(sp->axes, 3, 8);
    if (needs_build(c, r, rev)) {
        begin(c, r, rev);
        GuiRect lr = { 0, 0, r.w, r.h };
        if (sp->polys <= 0 || !sp->values[0]) placeholder(lr, st, sp->empty);
        else {
            float lh = gd_line_height(st->font_small), lw = 0;
            for (int i = 0; i < n; i++) lw = fmaxf(lw, text_w(st->font_small, sp->labels[i]));
            float R = fmaxf(10.0f, fminf(r.w * 0.5f - lw - st->pad * 2, r.h * 0.5f - lh * 1.5f));
            float cx = r.w * 0.5f, cy = r.h * 0.5f;
            c->aux[0] = cx; c->aux[1] = cy; c->aux[2] = R;
            vec2 dir[8];
            for (int i = 0; i < n; i++) {
                float a = TAU * (float)i / (float)n;
                dir[i] = v2(sinf(a), -cosf(a));
            }
            for (int ring = 1; ring <= 4; ring++) {
                float rr = R * (float)ring / 4.0f;
                for (int i = 0; i < n; i++) {
                    int j = (i + 1) % n;
                    seg(cx + dir[i].x * rr, cy + dir[i].y * rr, cx + dir[j].x * rr, cy + dir[j].y * rr, 1.0f, ring == 4 ? st->axis : st->grid);
                }
            }
            for (int i = 0; i < n; i++) {
                seg(cx, cy, cx + dir[i].x * R, cy + dir[i].y * R, 1.0f, st->grid);
                float lx = cx + dir[i].x * (R + st->pad), ly = cy + dir[i].y * (R + st->pad) - lh * 0.5f;
                int al = dir[i].x > 0.2f ? GUI_LEFT : dir[i].x < -0.2f ? GUI_RIGHT : GUI_CENTER;
                if (dir[i].y < -0.9f) ly -= lh * 0.5f;
                if (dir[i].y > 0.9f) ly += lh * 0.5f;
                label(lx, ly, st->font_small, st->text_dim, al, GW_REGULAR, sp->labels[i]);
            }
            for (int p = 0; p < DG_MIN(sp->polys, 2); p++) {
                if (!sp->values[p]) continue;
                u32 col = sp->colors[p] ? sp->colors[p] : st->series[p];
                vec2 pt[8];
                for (int i = 0; i < n; i++) {
                    float v = DG_CLAMP(sp->values[p][i], 0.0f, 1.0f);
                    pt[i] = v2(cx + dir[i].x * R * v, cy + dir[i].y * R * v);
                }
                int ctr = vtx(cx, cy, with_alpha(col, 0.22f));
                for (int i = 0; i < n; i++) {
                    int j = (i + 1) % n;
                    tri_i(ctr, vtx(pt[i].x, pt[i].y, with_alpha(col, 0.32f)), vtx(pt[j].x, pt[j].y, with_alpha(col, 0.32f)));
                }
                for (int i = 0; i < n; i++) {
                    int j = (i + 1) % n;
                    seg(pt[i].x, pt[i].y, pt[j].x, pt[j].y, st->line_w, col);
                }
                for (int i = 0; i < n; i++) disc(pt[i].x, pt[i].y, st->point_r * 0.8f, col);
            }
            c->n = n;
        }
    }
    draw_cache(c, r);
    if (!c->n || !mouse_in(r)) return -1;
    vec2 m = mouse_local(r);
    float dx = m.x - c->aux[0], dy = m.y - c->aux[1];
    if (dx * dx + dy * dy > (c->aux[2] * 1.35f) * (c->aux[2] * 1.35f) || dx * dx + dy * dy < 16) return -1;
    float a = atan2f(dx, -dy);
    if (a < 0) a += TAU;
    return (int)lrintf(a / TAU * (float)c->n) % c->n;
}

// ---------------------------------------------------------------- histogram

int chart_histogram(ChartCache *c, u32 rev, GuiRect r, const ChartStyle *st, const ChartHistSpec *sp) {
    if (needs_build(c, r, rev)) {
        begin(c, r, rev);
        GuiRect lr = { 0, 0, r.w, r.h };
        double hi = 0;
        for (int i = 0; i < sp->n; i++) hi = fmax(hi, sp->counts[i]);
        if (sp->n <= 0 || hi <= 0) placeholder(lr, st, sp->empty);
        else {
            float lh = gd_line_height(st->font_small);
            ChartTicks t = chart_nice_ticks(0, hi, tick_count_for(r.h - lh * 2, st), true);
            float lw = axis_label_w(&t, st, NULL, NULL) + st->pad * 1.5f;
            GuiRect p = { lw, lh * 1.2f, r.w - lw - st->pad, r.h - lh * 1.2f - lh - st->pad };
            c->plot = p;
            y_axis(&t, p, st, false, true, NULL, NULL);
            x_labels(sp->n, p, st, sp->label, sp->user, true);
            float slot = p.w / (float)sp->n;
            u32 col = sp->color ? sp->color : st->accent;
            for (int i = 0; i < sp->n; i++) {
                if (sp->counts[i] <= 0) continue;
                quad_vgrad(p.x + slot * (float)i + 1, map_y(sp->counts[i], &t, p), p.x + slot * (float)(i + 1) - 1, p.y + p.h, col, with_alpha(col, 0.6f));
            }
            if (sp->marker >= 0) {
                float x = p.x + slot * fminf(sp->marker, (float)sp->n);
                seg(x, p.y - lh * 0.2f, x, p.y + p.h, st->line_w, st->marker);
                if (sp->marker_label) label(x, p.y - lh * 1.2f, st->font_small, st->marker, x > p.x + p.w * 0.7f ? GUI_RIGHT : GUI_LEFT, GW_MEDIUM, sp->marker_label);
            }
            c->n = sp->n;
        }
    }
    draw_cache(c, r);
    if (!c->n || !mouse_in(r)) return -1;
    vec2 m = mouse_local(r);
    GuiRect p = c->plot;
    if (m.x < p.x || m.x >= p.x + p.w) return -1;
    return DG_CLAMP((int)((m.x - p.x) / (p.w / (float)c->n)), 0, c->n - 1);
}

// ---------------------------------------------------------------- scatter

int chart_scatter(ChartCache *c, u32 rev, GuiRect r, const ChartStyle *st, const ChartScatterSpec *sp) {
    if (needs_build(c, r, rev)) {
        begin(c, r, rev);
        GuiRect lr = { 0, 0, r.w, r.h };
        if (sp->n <= 0) placeholder(lr, st, sp->empty);
        else {
            double xlo = 1e30, xhi = -1e30, ylo = 1e30, yhi = -1e30;
            minmax(sp->x, sp->n, &xlo, &xhi);
            minmax(sp->y, sp->n, &ylo, &yhi);
            float lh = gd_line_height(st->font_small);
            ChartTicks ty = chart_nice_ticks(ylo, yhi, tick_count_for(r.h - lh * 3, st), true);
            ChartTicks tx = chart_nice_ticks(xlo, xhi, DG_CLAMP((int)(r.w / gui_s(80)), 2, 7), true);
            float lw = axis_label_w(&ty, st, sp->y_fmt, sp->user) + st->pad * 1.5f + (sp->y_title ? lh : 0);
            float top = sp->y_title ? lh * 1.7f : lh * 0.6f;   // the Y title sits above the top tick
            GuiRect p = { lw, top, r.w - lw - st->pad * 2, r.h - top - lh * (sp->x_title ? 2.2f : 1.0f) - st->pad };
            c->plot = p;
            y_axis(&ty, p, st, false, true, sp->y_fmt, sp->user);
            char b[32];
            for (int k = 0; k < tx.count; k++) {
                double v = tx.lo + tx.step * k;
                float x = p.x + p.w * (float)((v - tx.lo) / (tx.hi - tx.lo));
                quad(x - 0.5f, p.y, x + 0.5f, p.y + p.h, k == 0 ? st->axis : st->grid);
                label(x, p.y + p.h + st->pad * 0.6f, st->font_small, st->text_dim, GUI_CENTER, GW_REGULAR, sp->x_fmt ? sp->x_fmt(v, b, sizeof(b), sp->user) : default_fmt(v, b, sizeof(b)));
            }
            if (sp->x_title) label(p.x + p.w * 0.5f, p.y + p.h + lh * 1.2f + st->pad * 0.6f, st->font_small, st->text_dim, GUI_CENTER, GW_MEDIUM, sp->x_title);
            if (sp->y_title) label(0, 0, st->font_small, st->text_dim, GUI_LEFT, GW_MEDIUM, sp->y_title);
            hover_alloc(sp->n);
            c->n = sp->n;
            for (int i = 0; i < sp->n; i++) {
                float x = p.x + p.w * (float)((sp->x[i] - tx.lo) / (tx.hi - tx.lo));
                float y = map_y(sp->y[i], &ty, p);
                c->hx[i] = x;
                c->hy[i] = y;
                disc(x, y, st->point_r * (sp->sizes ? sp->sizes[i] : 1.0f), with_alpha(sp->colors ? sp->colors[i] : st->accent, 0.85f));
            }
        }
    }
    draw_cache(c, r);
    if (!c->n || !mouse_in(r)) return -1;
    vec2 m = mouse_local(r);
    int i = chart_nearest_point(c->hx, c->hy, c->n, m.x, m.y, gui_s(10) * gui_s(10));
    if (i >= 0) gd_ring(r.x + c->hx[i], r.y + c->hy[i], st->point_r * 2.0f, 1.5f, 1.0f, st->text);
    return i;
}

// ---------------------------------------------------------------- calendar heatmap

int chart_calendar_heatmap(ChartCache *c, u32 rev, GuiRect r, const ChartStyle *st, const ChartCalendarSpec *sp) {
    if (needs_build(c, r, rev)) {
        begin(c, r, rev);
        int cols = chart_calendar_cell(0, sp->days, sp->last_weekday, NULL, NULL);
        float lh = gd_line_height(st->font_small);
        float left = text_w(st->font_small, "Mon") + st->pad;
        float cell = fminf((r.w - left) / (float)DG_MAX(cols, 1), (r.h - lh - st->pad) / 7.0f);
        float gap = fmaxf(1.0f, cell * 0.14f);
        float top = lh + st->pad * 0.5f;
        c->aux[0] = left; c->aux[1] = top; c->aux[2] = cell; c->aux[3] = (float)cols;
        static const char *k_rows[7] = { "Mon", "", "Wed", "", "Fri", "", "" };
        for (int d = 0; d < 7; d++) label(0, top + cell * (float)d + (cell - lh) * 0.5f, st->font_small, st->text_dim, GUI_LEFT, GW_REGULAR, k_rows[d]);
        double hi = 0;
        for (int i = 0; i < sp->days; i++) hi = fmax(hi, sp->values[i]);
        hover_alloc(sp->days);
        float last_label_x = -1e9f;
        for (int i = 0; i < sp->days; i++) {
            int col, row;
            chart_calendar_cell(i, sp->days, sp->last_weekday, &col, &row);
            float x = left + cell * (float)col, y = top + cell * (float)row;
            c->hx[i] = x;
            c->hy[i] = y;
            float v = sp->values[i];
            u32 fill = v > 0 && hi > 0 ? gui_rgba_lerp(with_alpha(sp->color, 0.25f), sp->color, (float)(v / hi)) : st->empty;
            quad(x, y, x + cell - gap, y + cell - gap, fill);
            char b[16];
            if (sp->month_label && sp->month_label(i, b, sizeof(b), sp->user) && b[0] && x - last_label_x > text_w(st->font_small, "Mmm") + st->pad) {
                label(x, 0, st->font_small, st->text_dim, GUI_LEFT, GW_REGULAR, b);
                last_label_x = x;
            }
        }
        c->n = sp->days;
    }
    draw_cache(c, r);
    if (!c->n || !mouse_in(r)) return -1;
    vec2 m = mouse_local(r);
    float cell = c->aux[2];
    if (cell <= 0 || m.x < c->aux[0] || m.y < c->aux[1]) return -1;
    int col = (int)((m.x - c->aux[0]) / cell), row = (int)((m.y - c->aux[1]) / cell);
    if (row > 6 || col >= (int)c->aux[3]) return -1;
    for (int i = c->n - 1; i >= 0; i--) {
        if (fabsf(c->hx[i] - (c->aux[0] + cell * (float)col)) < 0.5f && fabsf(c->hy[i] - (c->aux[1] + cell * (float)row)) < 0.5f) {
            gd_border(gui_rect(r.x + c->hx[i] - 1, r.y + c->hy[i] - 1, cell - fmaxf(1.0f, cell * 0.14f) + 2, cell - fmaxf(1.0f, cell * 0.14f) + 2), 1.0f, st->text);
            return i;
        }
    }
    return -1;
}

// ---------------------------------------------------------------- gauge (immediate: one arc)

void chart_gauge(GuiRect r, const ChartStyle *st, float value01, const char *lbl, const char *value_text, u32 color) {
    static ChartCache scratch;
    float lh = gd_line_height(st->font_small);
    float side = fminf(r.w, (r.h - lh) * 1.25f);
    float ro = side * 0.5f - 2, ri = ro * 0.80f;
    float cx = r.w * 0.5f, cy = (r.h - lh) * 0.5f + ro * 0.12f + 2;
    // 270 degrees, opening at the bottom.
    float a0 = TAU * 0.625f, a1 = TAU * 1.375f;
    begin(&scratch, r, 0);
    arc(cx, cy, ri, ro, a0, a1, st->empty | 0x10000000u);
    float v = saturate(value01);
    if (v > 0.002f) arc(cx, cy, ri, ro, a0, a0 + (a1 - a0) * v, color ? color : st->accent);
    draw_cache(&scratch, r);
    scratch.built = false;
    GuiText vt = { st->font * 1.2f, GW_SEMIBOLD, st->text, GUI_CENTER, 0, false, false, 0 };
    gd_text(&vt, r.x + cx, r.y + cy - gd_line_height(vt.size) * 0.5f, value_text);
    GuiText lt = { st->font_small, GW_REGULAR, st->text_dim, GUI_CENTER, 0.04f, true, false, r.w };
    gd_text(&lt, r.x + cx, r.y + r.h - lh, lbl);
}

// ---------------------------------------------------------------- timeline

int chart_timeline(ChartCache *c, u32 rev, GuiRect r, const ChartStyle *st, const ChartTimelineSpec *sp) {
    if (needs_build(c, r, rev)) {
        begin(c, r, rev);
        GuiRect lr = { 0, 0, r.w, r.h };
        double hi = 0;
        bool any = false;
        for (int i = 0; i < sp->n; i++) {
            hi = fmax(hi, sp->bars[i]);
            any |= sp->bars[i] > 0 || (sp->downs && sp->downs[i]) || (sp->revives && sp->revives[i]);
        }
        if (sp->n <= 0 || !any) placeholder(lr, st, sp->empty);
        else {
            float lh = gd_line_height(st->font_small);
            float mk = gui_s(7);
            int maxm = 0;
            for (int i = 0; i < sp->n; i++) maxm = DG_MAX(maxm, (sp->downs ? sp->downs[i] : 0) + (sp->revives ? sp->revives[i] : 0));
            float top = lh * 0.6f + mk * 1.2f * (float)DG_MIN(maxm, 4);
            ChartTicks t = chart_nice_ticks(0, fmax(hi, 1), tick_count_for(r.h - top - lh * 2, st), true);
            float lw = axis_label_w(&t, st, NULL, NULL) + st->pad * 1.5f;
            GuiRect p = { lw, top, r.w - lw - st->pad, r.h - top - lh - st->pad };
            c->plot = p;
            y_axis(&t, p, st, false, true, NULL, NULL);
            x_labels(sp->n, p, st, sp->label, sp->user, true);
            float slot = p.w / (float)sp->n, bw = fminf(slot * 0.7f, gui_s(34));
            for (int i = 0; i < sp->n; i++) {
                float x0 = p.x + slot * (float)i + (slot - bw) * 0.5f, xc = p.x + slot * ((float)i + 0.5f);
                float y = sp->bars[i] > 0 ? map_y(sp->bars[i], &t, p) : p.y + p.h;
                if (sp->bars[i] > 0) quad_vgrad(x0, y, x0 + bw, p.y + p.h, st->accent, with_alpha(st->accent, 0.55f));
                // Markers above the bar: downs (red, pointing down), revives (green, pointing up).
                float my = y - mk * 0.7f;
                int d = sp->downs ? sp->downs[i] : 0, v = sp->revives ? sp->revives[i] : 0;
                for (int k = 0; k < DG_MIN(d, 4); k++, my -= mk * 1.2f) tri(xc - mk * 0.5f, my - mk * 0.5f, xc + mk * 0.5f, my - mk * 0.5f, xc, my + mk * 0.45f, st->bad);
                for (int k = 0; k < DG_MIN(v, 4); k++, my -= mk * 1.2f) tri(xc - mk * 0.5f, my + mk * 0.45f, xc + mk * 0.5f, my + mk * 0.45f, xc, my - mk * 0.5f, st->good);
            }
            c->n = sp->n;
        }
    }
    draw_cache(c, r);
    if (!c->n || !mouse_in(r)) return -1;
    vec2 m = mouse_local(r);
    GuiRect p = c->plot;
    if (m.x < p.x || m.x >= p.x + p.w) return -1;
    return DG_CLAMP((int)((m.x - p.x) / (p.w / (float)c->n)), 0, c->n - 1);
}
