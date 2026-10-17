// Settings modal (docs/UI.md "Settings"): Graphics (Engine Fidelity on top, then display,
// resolution, post-processing & effects, shadows, geometry, effects, sights, weather), Controls,
// Crosshair, Audio, Character, Phone. Display settings (resolution, window mode, vsync) wait for Apply and then ask to be
// kept within 15 seconds (reverted otherwise); everything else applies immediately (cvars, saved
// with the config when the modal closes).
#include "screens.h"
#include "ui/crosshair.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/log.h"
#include "game/game.h"
#include "game/game_internal.h"
#include "gameplay/characters.h"
#include "input/input.h"
#include "app/video.h"
#include "render/render.h"
#include "render/fidelity/fidelity.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define TH (gui_theme())
#define MAX_RES 48

enum { TAB_GRAPHICS, TAB_CONTROLS, TAB_CROSSHAIR, TAB_AUDIO, TAB_CHARACTER, TAB_POST, TAB_PHONE, TAB_COUNT };
static const char *const k_tabs[TAB_COUNT] = { "Graphics", "Controls", "Crosshair", "Audio", "Character", "Post-processing", "Phone" };
// Display order of the tabs. Post-processing lives in the Graphics tab (under Engine Fidelity);
// TAB_POST stays as an index (settings_open by number) and opens Graphics.
#define TAB_SHOWN (TAB_COUNT - 1)
static const int k_order[TAB_SHOWN] = { TAB_GRAPHICS, TAB_CONTROLS, TAB_CROSSHAIR, TAB_AUDIO, TAB_CHARACTER, TAB_PHONE };

typedef struct Display { int width, height, mode, vsync; } Display;

static struct {
    int tab;
    GuiScroll scroll[TAB_COUNT];
    Display pending, backup;
    float confirm_t;
    int res_w[MAX_RES], res_h[MAX_RES], res_count;
    char res_labels[MAX_RES][24];
    const char *res_items[MAX_RES];
} S;

static Cvar *cv(const char *n) { return cvar_find(n); }

static void collect_resolutions(void) {
    S.res_count = 0;
    SDL_DisplayID d = SDL_GetDisplayForWindow(g_engine.window);
    int count = 0;
    SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(d, &count);
    for (int i = 0; i < count && S.res_count < MAX_RES; i++) {
        bool dup = false;
        for (int k = 0; k < S.res_count; k++) if (S.res_w[k] == modes[i]->w && S.res_h[k] == modes[i]->h) dup = true;
        if (dup) continue;
        S.res_w[S.res_count] = modes[i]->w;
        S.res_h[S.res_count] = modes[i]->h;
        S.res_count++;
    }
    SDL_free(modes);
    static const int fallback[][2] = { { 1280, 720 }, { 1600, 900 }, { 1920, 1080 }, { 2560, 1080 }, { 2560, 1440 }, { 3840, 2160 } };
    if (!S.res_count)
        for (int i = 0; i < 6; i++) { S.res_w[i] = fallback[i][0]; S.res_h[i] = fallback[i][1]; S.res_count++; }
    // The current window size is always selectable (custom sizes, ultrawide windows).
    {
        int cw = cv("r_width")->i, ch = cv("r_height")->i;
        bool have = false;
        for (int k = 0; k < S.res_count; k++) if (S.res_w[k] == cw && S.res_h[k] == ch) have = true;
        if (!have && S.res_count < MAX_RES) { S.res_w[S.res_count] = cw; S.res_h[S.res_count] = ch; S.res_count++; }
    }
    for (int i = 0; i < S.res_count; i++) {
        snprintf(S.res_labels[i], sizeof(S.res_labels[i]), "%d \xC3\x97 %d", S.res_w[i], S.res_h[i]);
        S.res_items[i] = S.res_labels[i];
    }
}

static Display read_display(void) {
    Display d = { cv("r_width")->i, cv("r_height")->i, cv("r_mode")->i, cv("r_vsync")->i };
    return d;
}

static void write_display(const Display *d) {
    cvar_set_int(cv("r_width"), d->width);
    cvar_set_int(cv("r_height"), d->height);
    cvar_set_int(cv("r_mode"), d->mode);
    cvar_set_int(cv("r_vsync"), d->vsync);
    video_apply();
}

void settings_open(int tab) {
    if (tab >= 0 && tab < TAB_COUNT) S.tab = tab == TAB_POST ? TAB_GRAPHICS : tab;
    S.pending = read_display();
    collect_resolutions();
}

float settings_confirm_left(void) { return S.confirm_t; }

void settings_tick(float dt) {
    if (S.confirm_t <= 0) return;
    S.confirm_t -= dt;
    if (S.confirm_t <= 0) {
        S.confirm_t = 0;
        screens_confirm_expired();
    }
}

void settings_confirm_result(bool keep) {
    S.confirm_t = 0;
    if (keep) { config_save(video_config_path()); log_info("video: display settings kept"); return; }
    write_display(&S.backup);
    S.pending = S.backup;
    log_info("video: display settings reverted");
}

// ------------------------------------------------------------------ rows

static GuiId rid(const char *label) { return gui_id(label); }

static void row_slider(GuiCursor *c, const char *label, const char *hint, float *v, float lo, float hi, float step, const char *fmt) {
    GuiRect r = gw_setting_row(c, label, hint, rid(label));
    gw_slider(rid(label), r, v, lo, hi, step, fmt);
}

static void row_slider_cvar(GuiCursor *c, const char *label, const char *name, float lo, float hi, float step, const char *fmt, const char *hint) {
    Cvar *v = cv(name);
    if (!v) return;
    float f = v->f;
    GuiRect r = gw_setting_row(c, label, hint, rid(name));
    if (gw_slider(rid(name), r, &f, lo, hi, step, fmt)) cvar_set_float(v, f);
}

static void row_toggle_cvar(GuiCursor *c, const char *label, const char *name, const char *hint) {
    Cvar *v = cv(name);
    if (!v) return;
    bool b = v->i != 0;
    GuiRect r = gw_setting_row(c, label, hint, rid(name));
    if (gw_toggle(rid(name), r, &b)) cvar_set_int(v, b ? 1 : 0);
}

static bool row_combo(GuiCursor *c, const char *label, const char *const *items, int n, int *sel, const char *hint) {
    GuiRect r = gw_setting_row(c, label, hint, rid(label));
    r.y += gui_s(3);
    r.h -= gui_s(6);
    return gw_dropdown(rid(label), r, items, n, sel);
}

static void row_combo_cvar(GuiCursor *c, const char *label, const char *name, const char *const *items, int n, const char *hint) {
    Cvar *v = cv(name);
    if (!v) return;
    int sel = DG_CLAMP(v->i, 0, n - 1);
    if (row_combo(c, label, items, n, &sel, hint)) cvar_set_int(v, sel);
}

static void section(GuiCursor *c, const char *title) {
    c->y += gui_s(10);
    gw_section(gc_row(c, gui_s(26)), title);
}

// ------------------------------------------------------------------ tabs

// ------------------------------------------------------------------ Engine Fidelity bounds
// Every graphics control below is bounded by the Engine Fidelity level (docs/ENGINE_FIDELITY.md):
// options above the level are greyed with the level they need, a feature the level doesn't have is
// locked with "Requires Engine Fidelity: <level>".

// Lowest level (this era) at which the feature exists at all, -1 = the era doesn't have it.
static int feature_level(int f) {
    for (int l = 0; l < FID_LEVELS; l++) if (fid_bounds(l, fid_era(), f).avail != FID_UNAVAILABLE) return l;
    return -1;
}

static bool feature_locked(int f, char *why, size_t n) {
    if (!fid_loaded()) return false;
    FidBounds b = fid_bounds(fid_level(), fid_era(), f);
    if (b.avail != FID_UNAVAILABLE) return false;
    int need = feature_level(f);
    if (need < 0) snprintf(why, n, "Not in the %s era", fid_era_id(fid_era()));
    else snprintf(why, n, "Requires Engine Fidelity: %s", fid_level_name(need));
    return true;
}

static const char *value_note(int f, float v, char *buf, size_t n) {
    buf[0] = 0;
    if (!fid_loaded() || fid_value_allowed(fid_level(), fid_era(), f, v)) return buf;
    int need = fid_min_level_for(fid_era(), f, v);
    if (need < 0) snprintf(buf, n, "not in the %s era", fid_era_id(fid_era()));
    else snprintf(buf, n, "requires %s", fid_level_name(need));
    return buf;
}

// A locked row: the label faint, the reason where the control would be.
static void row_locked(GuiCursor *c, const char *label, const char *why, const char *hint) {
    gw_disabled(true);
    GuiRect r = gw_setting_row(c, label, hint, rid(label));
    gw_disabled(false);
    gw_label(r, why, gui_s(TH->t_body - 2), GW_REGULAR, TH->text_faint, GUI_LEFT);
}

// values: the cvar value of each item (NULL = the item index).
static void row_fid_combo(GuiCursor *c, const char *label, int f, const char *const *items, const int *values, int n, const char *hint) {
    Cvar *v = cv(fid_feature(f)->cvar);
    if (!v) return;
    char why[96];
    if (feature_locked(f, why, sizeof(why))) { row_locked(c, label, why, hint); return; }
    bool en[16];
    char notes[16][40];
    const char *np[16];
    int sel = 0;
    n = DG_MIN(n, 16);
    for (int i = 0; i < n; i++) {
        int val = values ? values[i] : i;
        value_note(f, (float)val, notes[i], sizeof(notes[i]));
        en[i] = !notes[i][0];
        np[i] = notes[i];
        if (v->i == val) sel = i;
    }
    GuiRect r = gw_setting_row(c, label, hint, rid(fid_feature(f)->cvar));
    r.y += gui_s(3);
    r.h -= gui_s(6);
    if (gw_dropdown_ex(rid(fid_feature(f)->cvar), r, items, n, &sel, en, np)) cvar_set_int(v, values ? values[sel] : sel);
}

static void row_fid_toggle(GuiCursor *c, const char *label, int f, const char *hint) {
    char why[96];
    if (feature_locked(f, why, sizeof(why))) { row_locked(c, label, why, hint); return; }
    row_toggle_cvar(c, label, fid_feature(f)->cvar, hint);
}

// The slider spans the level's own range; the tooltip says where more is allowed.
static void row_fid_slider(GuiCursor *c, const char *label, int f, float lo, float hi, float step, const char *fmt, const char *hint) {
    Cvar *v = cv(fid_feature(f)->cvar);
    if (!v) return;
    char why[96];
    if (feature_locked(f, why, sizeof(why))) { row_locked(c, label, why, hint); return; }
    float blo = lo, bhi = hi;
    char tip[200];
    dg_strcpy(tip, sizeof(tip), hint ? hint : "");
    if (fid_loaded()) {
        FidBounds b = fid_bounds(fid_level(), fid_era(), f);
        blo = fmaxf(lo, b.lo);
        bhi = fminf(hi, b.hi);
        if (bhi < hi - 1e-4f || blo > lo + 1e-4f) {
            int need = fid_min_level_for(fid_era(), f, fid_feature(f)->cheaper_high ? lo : hi);
            snprintf(tip + strlen(tip), sizeof(tip) - strlen(tip), "%s%s: %g .. %g at Engine Fidelity %s%s%s.", tip[0] ? " " : "", label, blo, bhi,
                     fid_level_name(fid_level()), need > fid_level() ? "; more at " : "", need > fid_level() ? fid_level_name(need) : "");
        }
    }
    if (bhi <= blo + 1e-4f) {   // a single value at this level
        char val[64], fixed[96];
        snprintf(val, sizeof(val), fmt, v->f);
        snprintf(fixed, sizeof(fixed), "%s (fixed at Engine Fidelity %s)", val, fid_level_name(fid_level()));
        row_locked(c, label, fixed, tip[0] ? tip : NULL);
        return;
    }
    float f_val = v->f;
    GuiRect r = gw_setting_row(c, label, tip[0] ? tip : NULL, rid(fid_feature(f)->cvar));
    if (gw_slider(rid(fid_feature(f)->cvar), r, &f_val, blo, bhi, step, fmt)) cvar_set_float(v, f_val);
}

// ------------------------------------------------------------------ Engine Fidelity section

static void fidelity_section(GuiCursor *c) {
    if (!fid_loaded()) {
        section(c, "Engine Fidelity");
        gw_label(gc_row(c, gui_s(22)), "Fidelity tables missing (assets/render/fidelity): every setting is unbounded.", gui_s(TH->t_body - 2), GW_REGULAR, TH->text_faint, GUI_LEFT);
        return;
    }
    c->y += gui_s(10);
    float row = gui_s(TH->row_h), line = gui_s(22), bh = gui_s(TH->button_h + 4);
    float card_h = gui_s(26) + row + line * 2 + bh + gui_s(30);
    GuiRect card = { c->area.x - gui_s(10), c->y - gui_s(4), c->area.w + gui_s(20), card_h };
    gd_round_border(card, gui_s(TH->radius_panel), 1, UI_RGBA(255, 255, 255, 10), gui_rgba_mul(TH->accent, 0.55f));
    gw_section(gc_row(c, gui_s(26)), "Engine Fidelity");
    int lvl = fid_level(), rec = game_fidelity_recommended();
    const char *items[FID_LEVELS];
    const char *notes[FID_LEVELS];
    for (int l = 0; l < FID_LEVELS; l++) { items[l] = fid_level_name(l); notes[l] = l == rec ? "Recommended" : ""; }
    int sel = lvl;
    GuiRect r = gw_setting_row(c, "Engine Fidelity", "The base of every graphics setting: which systems the renderer runs and how far the settings below can go. Applied live.",
                               rid("r_fidelity"));
    r.y += gui_s(3);
    r.h -= gui_s(6);
    if (gw_dropdown_ex(rid("r_fidelity"), r, items, FID_LEVELS, &sel, NULL, notes) && sel != lvl) game_fidelity_set(sel);
    gw_label(gc_row(c, line), fid_level_desc(lvl), gui_s(TH->t_body - 2), GW_REGULAR, TH->text_dim, GUI_LEFT);
    char buf[200];
    float ms = game_fidelity_bench_ms(lvl);
    if (game_fidelity_busy()) snprintf(buf, sizeof(buf), "Measuring...");
    else if (ms > 0) snprintf(buf, sizeof(buf), "Last benchmark: %.0f fps (%.1f ms GPU)  \xC2\xB7  budget %.0f ms at 1080p%s%s", 1000.0f / ms, ms, fid_level_budget_ms(lvl),
                              rec >= 0 ? "  \xC2\xB7  recommended: " : "", rec >= 0 ? fid_level_name(rec) : "");
    else snprintf(buf, sizeof(buf), "Not measured yet  \xC2\xB7  budget %.0f ms at 1080p", fid_level_budget_ms(lvl));
    gw_label(gc_row(c, line), buf, gui_s(TH->t_body - 2), GW_REGULAR, TH->text_faint, GUI_LEFT);
    GuiRect br = gc_row(c, bh);
    float bw = gui_s(190), gap = gui_s(10);
    GuiRect b1 = { br.x, br.y, bw, br.h }, b2 = { br.x + bw + gap, br.y, bw, br.h }, b3 = { br.x + (bw + gap) * 2, br.y, bw, br.h };
    if (gw_button(rid("fid_detect"), b1, "Run benchmark", GB_SECONDARY, !game_fidelity_busy())) game_fidelity_detect(false);
    if (gw_button(rid("fid_apply_rec"), b2, "Apply recommended", GB_PRIMARY, rec >= 0 && rec != lvl && !game_fidelity_busy())) game_fidelity_apply_recommended();
    int custom = fid_custom_count();
    char reset[48];
    snprintf(reset, sizeof(reset), custom ? "Reset %d tweak%s" : "Level defaults", custom, custom == 1 ? "" : "s");
    if (gw_button(rid("fid_reset"), b3, reset, GB_SECONDARY, custom > 0)) {
        fid_custom_load("");
        cvar_set("r_fidelity_custom", "");
        fid_apply(lvl);
    }
    c->y = card.y + card.h + gui_s(6);
}

// ------------------------------------------------------------------ tabs

static void tab_graphics(GuiCursor *c, GuiRect content) {
    fidelity_section(c);
    section(c, "Display");
    int sel = 0;
    for (int i = 0; i < S.res_count; i++) if (S.res_w[i] == S.pending.width && S.res_h[i] == S.pending.height) sel = i;
    if (row_combo(c, "Resolution", S.res_items, S.res_count, &sel, "Applied with Apply, then confirmed within 15 seconds.")) {
        S.pending.width = S.res_w[sel];
        S.pending.height = S.res_h[sel];
    }
    static const char *const modes[] = { "Windowed", "Borderless", "Fullscreen" };
    row_combo(c, "Window mode", modes, 3, &S.pending.mode, NULL);
    bool vs = S.pending.vsync != 0;
    GuiRect r = gw_setting_row(c, "VSync", NULL, rid("vsync_p"));
    if (gw_toggle(rid("vsync_p"), r, &vs)) S.pending.vsync = vs;
    Display cur = read_display();
    bool dirty = memcmp(&cur, &S.pending, sizeof(cur)) != 0;
    GuiRect br = gc_row(c, gui_s(TH->button_h + 4));
    GuiRect apply = { br.x + br.w - gui_s(140), br.y, gui_s(140), br.h }, revert = { apply.x - gui_s(150), br.y, gui_s(140), br.h };
    if (gw_button(rid("apply_display"), apply, "Apply", GB_PRIMARY, dirty)) {
        bool display_change = cur.width != S.pending.width || cur.height != S.pending.height || cur.mode != S.pending.mode;
        S.backup = cur;
        write_display(&S.pending);
        if (display_change) {
            S.confirm_t = 15.0f;
            screens_confirm(CONFIRM_DISPLAY, "Keep these display settings?", "The previous resolution and mode come back if you don't confirm.", 0);
        }
    }
    if (gw_button(rid("revert_display"), revert, "Revert", GB_SECONDARY, dirty)) S.pending = cur;

    section(c, "View");
    row_slider_cvar(c, "Field of view", "cl_fov", 50, 120, 1, "%.0f\xC2\xB0", "Vertical-equivalent camera FOV. Aiming down sights uses each sight's own FOV.");
    {
        // Stretched resolution simulator: the slider and what it looks like on this display.
        Cvar *st = cv("r_stretch");
        if (st) {
            float v = st->f * 100.0f;
            GuiRect r = gw_setting_row(c, "Stretched resolution", "The \"4:3 stretched\" look: the 3D view is rendered for a narrower screen and stretched to fill it, so everything looks wider. The HUD and menus stay normal.",
                                       rid("r_stretch"));
            if (gw_slider(rid("r_stretch"), r, &v, 0, 100, 1, v < 0.5f ? "Normal" : "%.0f%%")) cvar_set_float(st, v / 100.0f);
            if (st->f > 0.001f) {
                float k = render_stretch_factor();
                int ow = cv("r_width")->i, oh = cv("r_height")->i;
                char buf[128];
                snprintf(buf, sizeof(buf), "Looks like %d \xC3\x97 %d stretched to %d \xC3\x97 %d (%.2fx wider)", (int)((float)ow / k + 0.5f), oh, ow, oh, k);
                GuiRect lr = gc_row(c, gui_s(20));
                gw_label(gui_rect(lr.x + lr.w * 0.46f + gui_s(16), lr.y, lr.w * 0.54f, lr.h), buf, gui_s(TH->t_body - 2), GW_REGULAR, TH->text_faint, GUI_LEFT);
            }
        }
    }
    {
        Cvar *vm = cv("cam_weapon_fov");
        if (vm) {
            bool follow = vm->f < 1.0f;
            GuiRect tr = gw_setting_row(c, "Hands FOV follows camera", NULL, rid("vm_follow"));
            if (gw_toggle(rid("vm_follow"), tr, &follow)) cvar_set_float(vm, follow ? 0.0f : cv("cl_fov")->f);
            if (!follow) row_slider_cvar(c, "Hands / weapon FOV", "cam_weapon_fov", 40, 120, 1, "%.0f\xC2\xB0", NULL);
        }
    }
    row_slider_cvar(c, "Brightness", "r_brightness", 0.5f, 2.0f, 0.05f, "%.2f", NULL);
    row_slider_cvar(c, "Gamma", "r_gamma", 0.6f, 1.6f, 0.02f, "%.2f", NULL);
    {
        Cvar *fps = cv("r_maxfps");
        if (fps) {
            float f = (float)fps->i;
            GuiRect sr = gw_setting_row(c, "Frame rate limit", "0 = unlimited", rid("r_maxfps"));
            if (gw_slider(rid("r_maxfps"), sr, &f, 0, 300, 10, f < 1 ? "Off" : "%.0f")) cvar_set_int(fps, (int)f);
        }
    }

    row_toggle_cvar(c, "Performance overlay", "r_perf_overlay", "FPS, CPU / GPU frame time and the Engine Fidelity budget.");

    section(c, "Resolution & upscaling");
    row_fid_slider(c, "Render scale", FF_RENDER_SCALE, 0.25f, 2.0f, 0.05f, "%.2fx", "3D resolution. Below 1 is upscaled to the display, above 1 supersamples.");
    static const char *const up[] = { "Off (native)", "TAA-U", "FSR 1", "FSR 2 (uses FSR 1)", "DLSS (uses FSR 1)" };
    row_fid_combo(c, "Upscaler", FF_UPSCALER, up, NULL, 5, NULL);
    if (cv("r_upscaler") && cv("r_upscaler")->i) row_fid_slider(c, "Upscaler resolution", FF_UPSCALE_RATIO, 0.5f, 1.0f, 0.01f, "%.2fx", NULL);
    static const char *const aa[] = { "Off", "FXAA", "SMAA 1x", "TAA" };
    row_fid_combo(c, "Anti-aliasing", FF_AA, aa, NULL, 4, NULL);
    row_fid_slider(c, "Sharpening", FF_SHARPEN, 0, 1, 0.05f, "%.2f", NULL);

    section(c, "Post-processing & Effects");
    if (!fid_level_post(fid_level()))
        gw_label(gc_row(c, gui_s(22)), "Engine Fidelity Off: no post-processing. The scene goes straight to the screen.", gui_s(TH->t_body - 2), GW_REGULAR, TH->text_faint, GUI_LEFT);
    {
        static const char *const presets[] = { "Low", "Medium", "High", "Ultra", "Custom" };
        bool en[5];
        char notes[5][40];
        const char *np[5];
        for (int p = 0; p < 5; p++) {
            int req = 0;
            en[p] = p < 4 && game_post_preset_allowed(p, &req);
            notes[p][0] = 0;
            if (p < 4 && !en[p]) snprintf(notes[p], sizeof(notes[p]), req < 0 ? "not in this era" : "requires %s", req < 0 ? "" : fid_level_name(req));
            np[p] = notes[p];
        }
        int sel = game_post_detect_preset();
        bool any = false;
        int need = -1;
        for (int p = 0; p < 4; p++) {
            any |= en[p];
            int req = -1;
            game_post_preset_allowed(p, &req);
            if (req >= 0 && (need < 0 || req < need)) need = req;
        }
        char why[64];
        snprintf(why, sizeof(why), need >= 0 ? "Requires Engine Fidelity: %s" : "Not in this era", need >= 0 ? fid_level_name(need) : "");
        if (!any) row_locked(c, "Quality preset", why, NULL);
        else {
            GuiRect r = gw_setting_row(c, "Quality preset", "Sets the effects below at once, within the Engine Fidelity level.", rid("post_preset"));
            r.y += gui_s(3);
            r.h -= gui_s(6);
            en[4] = sel == 4;
            if (gw_dropdown_ex(rid("post_preset"), r, presets, 5, &sel, en, np) && sel < 4) game_post_apply_preset(sel);
        }
    }
    static const char *const ao[] = { "Off", "Low", "High" };
    row_fid_combo(c, "Ambient occlusion", FF_AO, ao, NULL, 3, NULL);
    row_fid_combo(c, "Contact shadows", FF_CONTACT_SHADOWS, ao, NULL, 3, NULL);
    static const char *const ssr[] = { "Off", "Half resolution", "Full resolution" };
    row_fid_combo(c, "Screen-space reflections", FF_SSR, ssr, NULL, 3, "Without them: sky / probe reflections.");
    row_fid_toggle(c, "Screen-space global illumination", FF_SSGI, NULL);
    static const char *const vol[] = { "Off (distance fog)", "Low", "High", "Ultra" };
    row_fid_combo(c, "Volumetric fog", FF_VOLUMETRICS, vol, NULL, 4, NULL);
    if (cv("r_volumetrics") && cv("r_volumetrics")->i) row_fid_toggle(c, "Light scattering (lamps)", FF_VOL_LOCAL_LIGHTS, "Point lights glow in the fog; off: fog only.");
    static const char *const mb[] = { "Off", "Camera", "Camera + objects" };
    row_fid_combo(c, "Motion blur", FF_MOTION_BLUR, mb, NULL, 3, NULL);
    if (cv("r_motion_blur") && cv("r_motion_blur")->i) row_slider_cvar(c, "Motion blur strength", "r_motion_blur_strength", 0.05f, 1, 0.05f, "%.2f", NULL);
    static const char *const dof[] = { "Off", "When aiming", "Always" };
    row_fid_combo(c, "Depth of field", FF_DOF, dof, NULL, 3, NULL);
    static const char *const dofq[] = { "Gaussian (16 taps)", "Hybrid (32 taps)", "Bokeh (48 taps)" };
    if (cv("r_dof") && cv("r_dof")->i) row_fid_combo(c, "Depth of field quality", FF_DOF_QUALITY, dofq, NULL, 3, NULL);
    row_fid_toggle(c, "Bloom", FF_BLOOM, NULL);
    static const char *const bq[] = { "Tiny", "Low", "High" };
    if (cv("r_bloom") && cv("r_bloom")->i) {
        row_fid_combo(c, "Bloom quality", FF_BLOOM_QUALITY, bq, NULL, 3, NULL);
        row_slider_cvar(c, "Bloom intensity", "r_bloom_intensity", 0, 0.3f, 0.005f, "%.3f", NULL);
    }
    row_fid_slider(c, "Lens flare", FF_LENS_FLARE, 0, 2, 0.05f, "%.2f", NULL);
    row_fid_slider(c, "Lens dirt", FF_LENS_DIRT, 0, 2, 0.05f, "%.2f", NULL);
    row_fid_slider(c, "Chromatic aberration", FF_CHROMATIC, 0, 2, 0.05f, "%.2f", NULL);
    row_fid_slider(c, "Lens distortion", FF_LENS_DISTORTION, -1, 1, 0.05f, "%.2f", NULL);
    row_fid_toggle(c, "Lens droplets", FF_LENS_DROPS, "Rain drops on the camera in first person.");
    if (cv("r_lens_drops") && cv("r_lens_drops")->i && fid_level() >= FID_HIGH) row_slider_cvar(c, "Lens droplet intensity", "r_lens_drops_intensity", 0.0f, 1.0f, 0.05f, "%.2f", NULL);
    row_fid_slider(c, "Vignette", FF_VIGNETTE, 0, 1, 0.05f, "%.2f", NULL);
    row_fid_slider(c, "Film grain", FF_FILM_GRAIN, 0, 1, 0.01f, "%.2f", NULL);
    static const char *const tm[] = { "ACES (filmic)", "AgX", "PBR Neutral" };
    row_fid_combo(c, "Tonemapper", FF_TONEMAPPER, tm, NULL, 3, NULL);
    row_fid_toggle(c, "Auto exposure", FF_AUTO_EXPOSURE, "Off: fixed exposure.");
    row_slider_cvar(c, "Exposure compensation", "r_exposure", -3, 3, 0.1f, "%+.1f EV", NULL);
    if (render_hdr_supported()) {
        row_fid_toggle(c, "HDR10 output", FF_HDR_OUTPUT, NULL);
        if (cv("r_hdr_output") && cv("r_hdr_output")->i) {
            row_slider_cvar(c, "HDR paper white", "r_hdr_paper_white", 80, 500, 10, "%.0f nits", NULL);
            row_slider_cvar(c, "HDR peak brightness", "r_hdr_max_nits", 400, 4000, 50, "%.0f nits", NULL);
        }
    }

    section(c, "Shadows & lighting");
    static const char *const shadow[] = { "Off", "Low (1024)", "Medium (2048)", "High (4096)" };
    row_fid_combo(c, "Shadows", FF_SHADOW_QUALITY, shadow, NULL, 4, NULL);
    static const char *const casc[] = { "1", "2", "3" };
    static const int casc_v[] = { 1, 2, 3 };
    row_fid_combo(c, "Shadow cascades", FF_SHADOW_CASCADES, casc, casc_v, 3, NULL);
    row_fid_slider(c, "Shadow distance", FF_SHADOW_DISTANCE, 0.25f, 1.0f, 0.05f, "%.2fx", NULL);
    static const char *const filt[] = { "Hard", "Soft (PCF 3x3)", "Softer (PCF 5x5)" };
    row_fid_combo(c, "Shadow softness", FF_SHADOW_FILTER, filt, NULL, 3, "Softer needs High shadow quality.");
    row_fid_slider(c, "Skip shadows of props under", FF_SHADOW_MIN_RADIUS, 0, 4, 0.25f, "%.2f m", "Small props cast no sun shadow (cheaper).");
    row_fid_toggle(c, "Lamp / flashlight shadows", FF_LIGHT_SHADOWS, NULL);
    row_fid_toggle(c, "Cloud shadows", FF_CLOUD_SHADOWS, NULL);

    section(c, "Geometry, textures & foliage");
    {
        Cvar *era = cv("r_era");
        if (era) {
            static const char *const eras[] = { "Modern", "2020", "2015", "2010", "Custom" };
            int sel = DG_CLAMP(era->i, 0, 4);
            if (row_combo(c, "Era look", eras, 5, &sel, "The geometry / foliage of an era (and the features it had). Engine Fidelity bounds its cost.") && sel < 4) cvar_set_int(era, sel);
        }
    }
    static const char *const texq[] = { "Low (mips -2)", "Medium (mips -1)", "High" };
    row_fid_combo(c, "Textures", FF_TEXTURE_QUALITY, texq, NULL, 3, NULL);
    {
        static const char *const aniso[] = { "Off", "2x", "4x", "8x", "16x" };
        static const int vals[] = { 1, 2, 4, 8, 16 };
        row_fid_combo(c, "Anisotropic filtering", FF_ANISO, aniso, vals, 5, NULL);
    }
    row_fid_slider(c, "Foliage density", FF_FOLIAGE_DENSITY, 0, 1, 0.05f, "%.2f", NULL);
    row_fid_slider(c, "Impostors below", FF_IMPOSTOR_PX, 0, 128, 4, "%.0f px", "Distant foliage switches to impostor cards below this size (larger: earlier).");
    row_fid_slider(c, "Character detail (LOD)", FF_LOD_BIAS, 0.25f, 4.0f, 0.05f, "%.2fx", NULL);
    static const char *const chq[] = { "Low", "Medium", "High", "Ultra" };
    row_fid_combo(c, "Character quality", FF_CHAR_QUALITY, chq, NULL, 4, NULL);

    section(c, "Effects");
    static const char *const vfxq[] = { "Low", "Medium", "High", "Ultra" };
    row_fid_combo(c, "Particles", FF_VFX_QUALITY, vfxq, NULL, 4, NULL);
    row_fid_toggle(c, "Lit particles", FF_VFX_LIGHTING, "Sun, shadows, lamps and 6-way lighting on smoke / dust; off: unlit soft particles.");
    {
        Cvar *dm = cv("vfx_decal_max");
        if (dm && fid_loaded()) {
            FidBounds b = fid_bounds(fid_level(), fid_era(), FF_DECAL_MAX);
            row_fid_slider(c, "Impact decals", FF_DECAL_MAX, 0, fminf(b.hi, 1024.0f), 8, "%.0f", NULL);
        }
    }
    static const char *const gore[] = { "Off", "Low", "High" };
    row_combo_cvar(c, "Gore", "gore_level", gore, 3, NULL);
    static const char *const cb[] = { "Off", "Protanopia", "Deuteranopia", "Tritanopia" };
    row_combo_cvar(c, "Colour-blind filter", "r_colorblind", cb, 4, "Every Engine Fidelity level, Off included.");
    row_slider_cvar(c, "Screen effects", "r_post_feedback", 0, 1, 0.05f, "%.2f", "Damage, flash and low-health effects.");

    section(c, "Sights");
    static const char *const scope[] = { "Picture-in-picture", "Full screen" };
    row_combo_cvar(c, "Scopes", "r_scope_mode", scope, 2, "Picture-in-picture keeps the world visible around the scope.");
    row_fid_slider(c, "Scope picture resolution", FF_SCOPE_PIP_SCALE, 0.2f, 2.0f, 0.05f, "%.2f", "1 = the render height, above 1 supersamples (sharper, slower).");
    row_slider_cvar(c, "Red dot brightness", "sight_brightness", 0.0f, 3.0f, 0.05f, "%.2f", NULL);
    row_fid_toggle(c, "Depth of field while aiming", FF_ADS_DOF, "A slight blur on the weapon while aimed.");
    section(c, "Weather");
    {
        static const char *const q[] = { "Auto (post preset)", "Off", "Low", "Medium", "High" };
        static const int qv[] = { -1, 0, 1, 2, 3 };
        row_fid_combo(c, "Rain / snow quality", FF_PRECIP_QUALITY, q, qv, 5, "Particle count of the rain / snow around you.");
    }
    row_fid_toggle(c, "Puddle reflections", FF_PUDDLE_REFLECTIONS, "Mirror-smooth puddles (screen-space reflections).");
    (void)content;
}

static void tab_controls(GuiCursor *c) {
    section(c, "Mouse");
    row_slider_cvar(c, "Sensitivity", "cl_sensitivity", 0.1f, 10, 0.05f, "%.2f", NULL);
    row_slider_cvar(c, "Vertical sensitivity", "cl_sensitivity_y", 0.1f, 10, 0.05f, "%.2f", NULL);
    row_slider_cvar(c, "Aiming sensitivity", "cl_ads_sensitivity", 0.1f, 1.5f, 0.05f, "%.2f", NULL);
    {
        Cvar *m = cv("ads_sens_mode");
        if (m) {
            static const char *const modes[3] = { "monitor_distance", "fov_ratio", "off" };
            static const char *const labels[3] = { "Monitor distance", "FOV ratio", "Off" };
            int sel = 0;
            for (int i = 0; i < 3; i++) if (!strcmp(m->str, modes[i])) sel = i;
            if (row_combo(c, "Aiming sensitivity scaling", labels, 3, &sel, "How the sight's zoom scales the sensitivity.")) cvar_set(m->name, modes[sel]);
            row_slider_cvar(c, sel == 1 ? "FOV ratio coefficient" : "Monitor distance coefficient", "ads_sens_coef", 0.0f, 2.0f, 0.01f, "%.2f", NULL);
        }
    }
    row_toggle_cvar(c, "Invert Y", "cl_invert_y", NULL);
    section(c, "Gamepad");
    row_slider_cvar(c, "Gamepad sensitivity", "pad_sensitivity", 0.5f, 8, 0.1f, "%.1f", NULL);
    row_slider_cvar(c, "Gamepad deadzone", "pad_deadzone", 0, 0.5f, 0.01f, "%.2f", NULL);
    section(c, "Movement");
    row_toggle_cvar(c, "Toggle crouch", "cl_crouch_toggle", "Press once instead of holding.");
    row_toggle_cvar(c, "Toggle sprint", "cl_sprint_toggle", "Press once instead of holding.");
    row_toggle_cvar(c, "Head bob", "cl_headbob", NULL);
    section(c, "Key bindings");
    for (int a = 0; a < ACT_COUNT; a++) {
        GuiRect r = gw_setting_row(c, action_label((Action)a), NULL, gui_idi("bindrow", a));
        float bw = (r.w - gui_s(8)) * 0.5f;
        for (int s = 0; s < BINDS_PER_ACTION; s++) {
            GuiRect br = { r.x + (bw + gui_s(8)) * (float)s, r.y + gui_s(3), bw, r.h - gui_s(6) };
            char name[48];
            binding_name(input_get_binding((Action)a, s), name, sizeof(name));
            bool cap = input_capturing() && input_capture_action() == a && input_capture_slot() == s;
            if (gw_keybind(gui_idi("bind", a * 4 + s), br, name[0] ? name : "-", cap)) input_begin_capture((Action)a, s);
        }
    }
    GuiRect br = gc_row(c, gui_s(TH->button_h + 4));
    if (gw_button(rid("reset_binds"), gui_rect(br.x + br.w - gui_s(220), br.y, gui_s(220), br.h), "Reset to defaults", GB_SECONDARY, true))
        screens_confirm(CONFIRM_RESET_BINDS, "Reset key bindings?", "Every action goes back to its default keys and buttons.", 0);
}

static void tab_crosshair(GuiCursor *c) {
    section(c, "Shape");
    static const char *const styles[] = { "Cross", "Cross + dot", "Dot", "Circle" };
    row_combo_cvar(c, "Style", "crosshair_style", styles, 4, NULL);
    Cvar *col = cv("crosshair_color");
    if (col) {
        const char *names[8];
        int n = DG_MIN(crosshair_color_count(), 8);
        for (int i = 0; i < n; i++) names[i] = crosshair_color_name(i);
        int sel = DG_CLAMP(col->i, 0, n - 1);
        if (n > 0 && row_combo(c, "Colour", names, n, &sel, NULL)) cvar_set_int(col, sel);
    }
    row_slider_cvar(c, "Line length", "crosshair_length", 1, 40, 0.5f, "%.1f", NULL);
    row_slider_cvar(c, "Line thickness", "crosshair_thickness", 1, 8, 0.5f, "%.1f", NULL);
    row_slider_cvar(c, "Extra gap", "crosshair_gap", 0, 30, 0.5f, "%.1f", "The gap shows the exact bullet spread cone, plus this.");
    row_slider_cvar(c, "Opacity", "crosshair_opacity", 0.1f, 1, 0.05f, "%.2f", NULL);
    row_toggle_cvar(c, "Outline", "crosshair_outline", NULL);
    section(c, "Feedback");
    row_toggle_cvar(c, "Tint over enemies / friendlies", "crosshair_target_color", NULL);
    row_toggle_cvar(c, "Hit markers", "crosshair_hitmarker", NULL);
    row_toggle_cvar(c, "Follow camera kick", "crosshair_follow_kick", "Off: pinned to the screen centre.");
    row_slider_cvar(c, "Weapon kick (visual)", "wpn_visual_scale", 0, 2, 0.05f, "%.2f", NULL);
    row_slider_cvar(c, "Weapon sway", "cl_weapon_sway", 0, 1, 0.05f, "%.2f", "Breathing, look lag, bob and landing of the held weapon (cosmetic: aiming stays exact).");
}

static void tab_audio(GuiCursor *c) {
    section(c, "Volume");
    row_slider_cvar(c, "Master", "snd_master", 0, 1, 0.01f, "%.2f", NULL);
    row_slider_cvar(c, "Music", "snd_music", 0, 1, 0.01f, "%.2f", NULL);
    row_slider_cvar(c, "Effects", "snd_sfx", 0, 1, 0.01f, "%.2f", NULL);
    row_slider_cvar(c, "Voice", "snd_voice", 0, 1, 0.01f, "%.2f", NULL);
    row_slider_cvar(c, "Interface", "snd_ui", 0, 1, 0.01f, "%.2f", NULL);
}

static void tab_character(GuiCursor *c) {
    section(c, "Skin");
    Cvar *cs = cv("cl_skin");
    int n = chars_skin_count();
    const char *names[32];
    int sel = 0;
    for (int i = 0; i < n && i < 32; i++) {
        names[i] = chars_skin_name(i);
        if (cs && !strcmp(cs->str, chars_skin_id(i))) sel = i;
    }
    if (n > 0 && row_combo(c, "Character", names, DG_MIN(n, 32), &sel, "Seen in third person, mirrors and your shadow.") && cs) cvar_set_cv(cs, chars_skin_id(sel));
    row_toggle_cvar(c, "Third person view", "cl_thirdperson", NULL);
    section(c, "Interface");
    row_slider_cvar(c, "UI scale", "ui_scale", 0, 3, 0.05f, "%.2f", "0 = automatic from the resolution.");
    row_slider_cvar(c, "Safe area", "ui_safe_area", -1, 0.15f, 0.005f, "%.3f", "Margin around menus and the HUD (-1 = theme default).");
    row_toggle_cvar(c, "Health bars over characters", "cl_healthbars", NULL);
    row_toggle_cvar(c, "Show FPS", "cl_showfps", NULL);
    // Spectating (docs/SPECTATE.md).
    section(c, "Spectating");
    row_slider_cvar(c, "Death camera time", "spec_death_cam_time", 0, 10, 0.5f, "%.1f s",
                    "How long the camera stays on your body before you watch your killer (or the nearest player). Fire skips it.");
    row_toggle_cvar(c, "Spectator delay readout", "spec_show_delay",
                    "Time from the watched player's input to your screen. The minimum is one network hop from them to the host plus one from the host to you: their own commands are replayed here, no interpolation delay on top.");
    row_slider_cvar(c, "Camera blend on switch", "spec_blend_time", 0, 0.5f, 0.01f, "%.2f s", "Smooths the jump to the next player.");
    row_toggle_cvar(c, "Free camera collision", "spec_free_collide", NULL);
}

// Phone companion (docs/PHONE.md): server, stream, orientation, capture.
static void tab_phone(GuiCursor *c) {
    static const char *const server[] = { "Off", "On", "Automatic" };
    section(c, "Companion page");
    row_combo_cvar(c, "Phone server", "phone_server", server, 3, "Automatic: starts the first time you take the phone out (X).");
    row_toggle_cvar(c, "Allow pairing", "phone_pairing", "Scan the QR code (P while holding the phone) with your real phone.");
    row_slider_cvar(c, "Server port", "phone_port", 1024, 65000, 1, "%.0f", "HTTPS port; the certificate helper uses the next one. Same Wi-Fi as the phone.");
    row_toggle_cvar(c, "HTTPS", "phone_tls", "Browsers only give motion sensors to secure pages. Off: Android flags only.");
    row_slider_cvar(c, "Pairing code lifetime", "phone_token_ttl", 10, 600, 10, "%.0f s", NULL);
    section(c, "Stream");
    row_slider_cvar(c, "Frame rate", "phone_fps", 5, 60, 1, "%.0f Hz", "Camera render and stream rate.");
    row_slider_cvar(c, "Quality", "phone_quality", 30, 95, 1, "%.0f", "Maximum JPEG quality; lowered automatically on a slow network.");
    row_slider_cvar(c, "Width", "phone_res_w", 128, 1440, 16, "%.0f px", NULL);
    row_slider_cvar(c, "Height", "phone_res_h", 256, 2560, 16, "%.0f px", NULL);
    section(c, "Orientation");
    row_slider_cvar(c, "Smoothing", "phone_smoothing", 0, 1, 0.05f, "%.2f", "Higher: steadier but slower to follow the real phone.");
    row_slider_cvar(c, "Sensitivity", "phone_sensitivity", 0.25f, 2, 0.05f, "%.2f", NULL);
    row_toggle_cvar(c, "Invert yaw", "phone_invert_yaw", NULL);
    row_toggle_cvar(c, "Invert pitch", "phone_invert_pitch", NULL);
    row_toggle_cvar(c, "Invert roll", "phone_invert_roll", NULL);
    section(c, "Camera");
    static const char *const filters[] = { "None", "Vivid", "Warm", "Cool", "Mono", "Noir" };
    row_combo_cvar(c, "Photo filter", "phone_filter", filters, 6, NULL);
    row_toggle_cvar(c, "High resolution photos", "phone_photo_hires", "Re-renders the frame at 1080 x 1920.");
    row_slider_cvar(c, "Exposure", "phone_exposure", -3, 3, 0.1f, "%.1f EV", NULL);
    row_slider_cvar(c, "Screen brightness", "phone_screen_brightness", 0.1f, 2, 0.05f, "%.2f", NULL);
    row_slider_cvar(c, "Video bitrate", "phone_video_kbps", 500, 20000, 500, "%.0f kbit/s", NULL);
    row_slider_cvar(c, "Video max length", "phone_record_max", 2, 3600, 1, "%.0f s", NULL);
}

bool settings_draw(GuiRect r) {
    GuiRect content = screens_modal_frame(r, "Settings", NULL);
    // Tabs (LB / RB, Ctrl+Tab cycle).
    const char *labels[TAB_SHOWN];
    int cur = 0;
    if (S.tab == TAB_POST) S.tab = TAB_GRAPHICS;
    for (int i = 0; i < TAB_SHOWN; i++) { labels[i] = k_tabs[k_order[i]]; if (k_order[i] == S.tab) cur = i; }
    GuiRect tr = { content.x, content.y - gui_s(8), content.w, gui_s(34) };
    int ni = gw_tabs(gui_id("settings_tabs"), tr, labels, TAB_SHOWN, cur);
    if (gui_nav(NAV_TAB_NEXT)) { ni = (cur + 1) % TAB_SHOWN; gui_nav_consume(NAV_TAB_NEXT); gui_sound(TH->snd_tab); }
    if (gui_nav(NAV_TAB_PREV)) { ni = (cur + TAB_SHOWN - 1) % TAB_SHOWN; gui_nav_consume(NAV_TAB_PREV); gui_sound(TH->snd_tab); }
    if (k_order[ni] != S.tab) { S.tab = k_order[ni]; gui_set_focus(0); gui_anim_restart(gui_id("settings_tab_in")); }
    float tin = gui_ease_out(gui_anim_t(gui_id("settings_tab_in")) / 0.15f);
    GuiRect view = { content.x, tr.y + tr.h + gui_s(10), content.w - gui_s(14), content.y + content.h - (tr.y + tr.h + gui_s(10)) - gui_s(24) };
    GuiScroll *sc = &S.scroll[S.tab];
    gw_scroll_begin(sc, gui_idi("settings_scroll", S.tab), view);
    gd_alpha_push(tin);
    GuiRect inner = { view.x + gui_s(10), gw_scroll_y(sc) + (1.0f - tin) * gui_s(8), view.w - gui_s(20), 100000 };
    GuiCursor c = gc_make(inner, gui_s(2));
    switch (S.tab) {
    case TAB_GRAPHICS: tab_graphics(&c, view); break;
    case TAB_POST: tab_graphics(&c, view); break;
    case TAB_CONTROLS: tab_controls(&c); break;
    case TAB_CROSSHAIR: tab_crosshair(&c); break;
    case TAB_AUDIO: tab_audio(&c); break;
    case TAB_CHARACTER: tab_character(&c); break;
    case TAB_PHONE: tab_phone(&c); break;
    }
    gd_alpha_pop();
    gw_scroll_end(sc, gc_used(&c) + gui_s(16));
    screens_footer_hint(gui_rect(content.x, content.y + content.h - gui_s(18), content.w, gui_s(18)),
                        input_capturing() ? "Press a key or button  \xC2\xB7  Esc cancels" : "Esc / B  back     LB / RB  tabs");
    return false;
}
