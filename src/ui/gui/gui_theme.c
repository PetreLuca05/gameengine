#include "gui_theme.h"
#include "core/log.h"
#include "core/platform.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define RGBA(r, g, b, a) ((u32)(r) | ((u32)(g) << 8) | ((u32)(b) << 16) | ((u32)(a) << 24))

static GuiTheme T;
static bool g_init;

static void defaults(GuiTheme *t) {
    memset(t, 0, sizeof(*t));
    t->accent = RGBA(226, 140, 52, 255);
    t->accent_dim = RGBA(226, 140, 52, 90);
    t->text = RGBA(232, 232, 226, 255);
    t->text_dim = RGBA(232, 232, 226, 150);
    t->text_faint = RGBA(232, 232, 226, 80);
    t->text_dark = RGBA(16, 16, 18, 255);
    t->panel = RGBA(12, 14, 16, 168);
    t->panel_hi = RGBA(28, 31, 35, 215);
    t->panel_border = RGBA(255, 255, 255, 22);
    t->panel_header = RGBA(255, 255, 255, 8);
    t->hover = RGBA(255, 255, 255, 14);
    t->press = RGBA(255, 255, 255, 26);
    t->focus = RGBA(226, 140, 52, 200);
    t->scrim = RGBA(4, 5, 6, 110);
    t->shadow = RGBA(0, 0, 0, 120);
    t->danger = RGBA(226, 72, 58, 255);
    t->warning = RGBA(232, 190, 80, 255);
    t->ok = RGBA(110, 196, 112, 255);
    t->info = RGBA(110, 160, 226, 255);
    t->slot_bg = RGBA(255, 255, 255, 10);
    t->slot_border = RGBA(255, 255, 255, 20);
    t->slot_hi = RGBA(255, 255, 255, 28);
    t->type_weapon = RGBA(196, 106, 72, 255);
    t->type_ammo = RGBA(200, 170, 82, 255);
    t->type_consumable = RGBA(104, 186, 120, 255);
    t->type_attachment = RGBA(104, 150, 214, 255);
    t->type_key = RGBA(196, 120, 200, 255);
    t->type_misc = RGBA(150, 150, 150, 255);
    t->hud_panel = RGBA(10, 12, 14, 130);
    t->hud_text = RGBA(236, 236, 230, 255);
    t->t_tiny = 12;
    t->t_small = 14;
    t->t_body = 17;
    t->t_label = 13;
    t->t_title = 26;
    t->t_large = 30;
    t->t_huge = 46;
    t->tracking_caps = 0.09f;
    t->button_h = 30;
    t->row_h = 36;
    t->radius = 3;
    t->radius_panel = 6;
    t->border = 1;
    t->accent_bar = 3;
    t->pad = 20;
    t->gap = 8;
    t->slot = 66;
    t->slot_gap = 6;
    t->modal_max_w_pct = 0.6f;
    t->scroll_speed = 60;
    t->safe_frac = 0.035f;
    t->max_aspect = 2.39f;
    t->menu_in = 0.15f;
    t->menu_stagger = 0.022f;
    t->hover_t = 0.08f;
    t->modal_in = 0.14f;
    t->shake = 0.35f;
    t->tooltip_delay = 0.35f;
    t->blur = 1;
    t->dim = 0.35f;
    dg_strcpy(t->snd_hover, sizeof(t->snd_hover), "ui_hover");
    dg_strcpy(t->snd_click, sizeof(t->snd_click), "ui_click");
    dg_strcpy(t->snd_back, sizeof(t->snd_back), "ui_back");
    dg_strcpy(t->snd_open, sizeof(t->snd_open), "ui_open");
    dg_strcpy(t->snd_error, sizeof(t->snd_error), "ui_error");
    dg_strcpy(t->snd_drop, sizeof(t->snd_drop), "ui_drop");
    dg_strcpy(t->snd_equip, sizeof(t->snd_equip), "ui_equip");
    dg_strcpy(t->snd_tab, sizeof(t->snd_tab), "ui_tab");
    t->snd_volume = 0.6f;
    dg_strcpy(t->font_regular, sizeof(t->font_regular), "assets/ui/fonts/Barlow-Regular.ttf");
    dg_strcpy(t->font_medium, sizeof(t->font_medium), "assets/ui/fonts/Barlow-Medium.ttf");
    dg_strcpy(t->font_semibold, sizeof(t->font_semibold), "assets/ui/fonts/Barlow-SemiBold.ttf");
}

const GuiTheme *gui_theme(void) {
    if (!g_init) { defaults(&T); g_init = true; }
    return &T;
}

u32 gui_rgba_mul(u32 c, float a) {
    float al = (float)(c >> 24) * (a < 0 ? 0 : (a > 1 ? 1 : a));
    return (c & 0x00FFFFFFu) | ((u32)(al + 0.5f) << 24);
}

u32 gui_rgba_lerp(u32 a, u32 b, float t) {
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    u32 r = 0;
    for (int k = 0; k < 4; k++) {
        float x = (float)((a >> (8 * k)) & 255), y = (float)((b >> (8 * k)) & 255);
        r |= (u32)(x + (y - x) * t + 0.5f) << (8 * k);
    }
    return r;
}

static void col(const cJSON *o, const char *k, u32 *dst) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsArray(v) || cJSON_GetArraySize(v) < 3) return;
    int c[4] = { 255, 255, 255, 255 };
    for (int i = 0; i < 4 && i < cJSON_GetArraySize(v); i++) c[i] = DG_CLAMP((int)cJSON_GetArrayItem(v, i)->valuedouble, 0, 255);
    *dst = RGBA(c[0], c[1], c[2], c[3]);
}

static void num(const cJSON *o, const char *k, float *dst) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsNumber(v)) *dst = (float)v->valuedouble;
}

static void str(const cJSON *o, const char *k, char *dst, size_t n) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsString(v)) dg_strcpy(dst, n, v->valuestring);
}

bool gui_theme_load(const char *path) {
    GuiTheme t;
    defaults(&t);
    char full[512];
    snprintf(full, sizeof(full), "%s%s", platform_base_dir(), path);
    size_t size = 0;
    char *text = platform_read_file(full, &size);
    if (!text) { log_warn("ui: theme %s not found (defaults)", path); T = t; g_init = true; return false; }
    cJSON *root = cJSON_ParseWithLength(text, size);
    free(text);
    if (!root) { log_error("ui: theme %s: JSON error near %.40s", path, cJSON_GetErrorPtr() ? cJSON_GetErrorPtr() : "?"); return false; }
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(root, "colors");
    if (c) {
#define C(n) col(c, #n, &t.n)
        C(accent); C(accent_dim); C(text); C(text_dim); C(text_faint); C(text_dark);
        C(panel); C(panel_hi); C(panel_border); C(panel_header);
        C(hover); C(press); C(focus); C(scrim); C(shadow);
        C(danger); C(warning); C(ok); C(info);
        C(slot_bg); C(slot_border); C(slot_hi);
        C(type_weapon); C(type_ammo); C(type_consumable); C(type_attachment); C(type_key); C(type_misc);
        C(hud_panel); C(hud_text);
#undef C
    }
    const cJSON *s = cJSON_GetObjectItemCaseSensitive(root, "sizes");
    if (s) {
#define N(n) num(s, #n, &t.n)
        N(t_tiny); N(t_small); N(t_body); N(t_label); N(t_title); N(t_large); N(t_huge); N(tracking_caps);
        N(button_h); N(row_h); N(radius); N(radius_panel); N(border); N(accent_bar); N(pad); N(gap);
        N(slot); N(slot_gap); N(modal_max_w_pct); N(scroll_speed); N(safe_frac); N(max_aspect);
#undef N
    }
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(root, "anim");
    if (a) {
        num(a, "menu_in", &t.menu_in);
        num(a, "menu_stagger", &t.menu_stagger);
        num(a, "hover", &t.hover_t);
        num(a, "modal_in", &t.modal_in);
        num(a, "shake", &t.shake);
        num(a, "tooltip_delay", &t.tooltip_delay);
    }
    const cJSON *b = cJSON_GetObjectItemCaseSensitive(root, "backdrop");
    if (b) { num(b, "blur", &t.blur); num(b, "dim", &t.dim); }
    const cJSON *so = cJSON_GetObjectItemCaseSensitive(root, "sounds");
    if (so) {
        str(so, "hover", t.snd_hover, sizeof(t.snd_hover));
        str(so, "click", t.snd_click, sizeof(t.snd_click));
        str(so, "back", t.snd_back, sizeof(t.snd_back));
        str(so, "open", t.snd_open, sizeof(t.snd_open));
        str(so, "error", t.snd_error, sizeof(t.snd_error));
        str(so, "drop", t.snd_drop, sizeof(t.snd_drop));
        str(so, "equip", t.snd_equip, sizeof(t.snd_equip));
        str(so, "tab", t.snd_tab, sizeof(t.snd_tab));
        num(so, "volume", &t.snd_volume);
    }
    const cJSON *f = cJSON_GetObjectItemCaseSensitive(root, "fonts");
    if (f) {
        str(f, "regular", t.font_regular, sizeof(t.font_regular));
        str(f, "medium", t.font_medium, sizeof(t.font_medium));
        str(f, "semibold", t.font_semibold, sizeof(t.font_semibold));
    }
    cJSON_Delete(root);
    T = t;
    g_init = true;
    return true;
}
