// Settings modal (docs/UI.md "Settings", docs/SETTINGS_AUDIT.md): Graphics (Quality: Overall and the
// seven quality groups, the only thing that sets the quality features; Display and Comfort; what
// changes visibility is the same for every player and not here), Controls, HUD (crosshair, compass, kills, pings, wheels), Audio, Character,
// Phone. Display settings (resolution, window mode, vsync) wait for Apply and then ask to be
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
#include "player/camera.h"
#include "app/video.h"
#include "render/render.h"
#include "render/fidelity/fidelity.h"
#include "render/post/look.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define TH (gui_theme())
#define MAX_RES 48

enum { TAB_GRAPHICS, TAB_CONTROLS, TAB_CROSSHAIR, TAB_AUDIO, TAB_CHARACTER, TAB_POST, TAB_PHONE, TAB_HUD, TAB_COUNT };
static const char *const k_tabs[TAB_COUNT] = { "Graphics", "Controls", "Crosshair", "Audio", "Character", "Post-processing", "Phone", "HUD" };
// Display order of the tabs. Post-processing lives in the Graphics tab, the crosshair in the HUD tab;
// TAB_POST / TAB_CROSSHAIR stay as indices (settings_open by number) and open Graphics / HUD.
#define TAB_SHOWN (TAB_COUNT - 3)
static const int k_order[TAB_SHOWN] = { TAB_GRAPHICS, TAB_CONTROLS, TAB_HUD, TAB_AUDIO, TAB_PHONE };
// Tabs that were merged into others (their indices stay valid for `menu settings:<n>`): Post-processing
// -> Graphics, Crosshair and Character (UI scale, safe area) -> HUD.
static int tab_shown(int t) { return t == TAB_POST ? TAB_GRAPHICS : ((t == TAB_CROSSHAIR || t == TAB_CHARACTER) ? TAB_HUD : t); }

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

// Developer mode (the `developer` cvar): the developer-only rows of the other tabs. A player never
// sees them.
static bool dev_mode(void) {
    Cvar *d = cvar_find("developer");
    return d && d->i;
}

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
    if (tab >= 0 && tab < TAB_COUNT) S.tab = tab_shown(tab);
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

// Graphics defaults: the player's graphics cvars (CVAR_EDITABLE r_*, post_*, vfx_* and the Comfort
// settings of the Graphics tab) go back to their registered defaults; then the Engine Fidelity level
// (itself back to its default) applies its table defaults to every feature, with the custom tweaks
// cleared. Kept: the display (Apply / Revert, confirmed) and the fidelity benchmark results. Tier
// and locked cvars are not the player's (docs/SETTINGS_AUDIT.md).
static const char *const k_gfx_keep[] = { "r_width", "r_height", "r_mode", "r_vsync" };
static const char *const k_gfx_extra[] = { "cl_fov", "cl_ads_fov", "cl_weapon_fov", "cl_showfps" };

static bool gfx_cvar(const Cvar *c) {
    if (!(c->flags & CVAR_EDITABLE)) return false;
    for (int k = 0; k < ARRAY_COUNT(k_gfx_keep); k++) if (!strcmp(c->name, k_gfx_keep[k])) return false;
    for (int k = 0; k < ARRAY_COUNT(k_gfx_extra); k++) if (!strcmp(c->name, k_gfx_extra[k])) return true;
    if (!(c->flags & CVAR_ARCHIVE)) return false;
    return !strncmp(c->name, "r_", 2) || !strncmp(c->name, "post_", 5) || !strncmp(c->name, "vfx_", 4);
}

void settings_graphics_defaults(void) {
    int n = 0;
    for (int i = 0; i < cvar_count(); i++) {
        Cvar *c = cvar_at(i);
        if (!c || !gfx_cvar(c) || !strcmp(c->str, c->default_str)) continue;
        cvar_reset(c);
        n++;
    }
    game_quality_apply();   // the quality settings' defaults on their features
    S.pending = read_display();
    log_info("settings: %d graphics settings back to their defaults", n);
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

// ------------------------------------------------------------------ tabs

// ------------------------------------------------------------------ Graphics

// Quality (docs/ENGINE_FIDELITY.md "Player settings"): Overall and seven groups, each Low / Medium /
// High / Ultra. These are the only thing that sets the quality features: Overall sets all seven (it
// reads Custom when they differ), a group sets its own features at that tier.
static const char *const k_tiers[5] = { "Low", "Medium", "High", "Ultra", "Custom" };

static void quality_row(GuiCursor *c, const char *label, int group, const char *hint) {
    int sel = game_quality_tier(group);
    if (row_combo(c, label, k_tiers, 4, &sel, hint)) game_quality_set(group, sel);
}

static void graphics_quality(GuiCursor *c) {
    section(c, "Quality");
    {
        int all = game_quality_overall(), sel = all < 0 ? 4 : all;
        bool en[5] = { true, true, true, true, false };
        const char *notes[5] = { "", "", "", "", "" };
        GuiRect r = gw_setting_row(c, "Overall", "Sets every quality below. Custom: they differ.", rid("gfx_overall"));
        r.y += gui_s(3);
        r.h -= gui_s(6);
        if (gw_dropdown_ex(rid("gfx_overall"), r, k_tiers, 5, &sel, en, notes) && sel < 4) game_quality_set_overall(sel);
    }
    quality_row(c, "Anti-aliasing", FG_AA, "Low FXAA, Medium SMAA, High and Ultra TAA (with sharpening from Medium).");
    quality_row(c, "Post-processing", FG_POST, "Ambient occlusion, reflections, volumetrics, depth of field, bloom, film grain, chromatic aberration, vignette.");
    quality_row(c, "Shadows", FG_SHADOWS, "Sun, moon and lamp shadows: resolution, cascades, softness.");
    quality_row(c, "Textures", FG_TEXTURES, "Texture resolution, filtering, terrain detail.");
    quality_row(c, "Effects", FG_EFFECTS, "Particles, rain and snow, water, the scope picture.");
    quality_row(c, "Foliage", FG_FOLIAGE, "Foliage wind animation (the amount of foliage is the same for everyone).");
    quality_row(c, "View distance", FG_VIEW_DISTANCE, "Shadow detail of distant geometry (what you can see is the same for everyone).");
}

// The tab header: "Why can't I change X?" (hover: the fair-play reason) and Default.
static const char k_why_fixed[] =
    "Settings that change what you can see compared with other players are the same for everyone, for fair play: foliage and grass, fog and weather, view, "
    "shadow and detail distances, lens effects, exposure and brightness beyond a small range. The quality settings change how good the "
    "world looks and how fast it runs, never what you can see. The host's lobby rules may set some of them for a whole game.";

static void graphics_header(GuiCursor *c) {
    GuiRect br = gc_row(c, gui_s(TH->button_h + 4));
    GuiRect def = { br.x + br.w - gui_s(140), br.y, gui_s(140), br.h };
    GuiRect why = { br.x, br.y + gui_s(4), gui_s(250), br.h - gui_s(8) };
    GuiInteract in = gui_interact(rid("gfx_why"), why, GUI_FOCUSABLE);
    gd_round_border(why, gui_s(TH->radius), 1.0f, UI_RGBA(255, 255, 255, in.hovered ? 26 : 10), in.hovered ? TH->accent : TH->panel_border);
    gw_label(why, "(i)  Why can't I change X?", gui_s(TH->t_small + 1), GW_SEMIBOLD, in.hovered ? TH->text : TH->text_dim, GUI_CENTER);
    if (in.hovered || in.focused) gui_tooltip(rid("gfx_why"), k_why_fixed);
    if (gw_button(rid("gfx_defaults"), def, "Default", GB_SECONDARY, true))
        screens_confirm(CONFIRM_GRAPHICS_DEFAULTS, "Reset graphics settings?", "Every graphics setting goes back to its default, the qualities included. Resolution, window mode and VSync stay.", 0);
}

static void graphics_display(GuiCursor *c) {
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
    {
        Cvar *fps = cv("r_maxfps");
        if (fps) {
            float f = (float)fps->i;
            GuiRect sr = gw_setting_row(c, "Frame rate limit", "0 = unlimited", rid("r_maxfps"));
            if (gw_slider(rid("r_maxfps"), sr, &f, 0, 300, 10, f < 1 ? "Off" : "%.0f")) cvar_set_int(fps, (int)f);
        }
    }
    row_fid_slider(c, "Render scale", FF_RENDER_SCALE, 0.25f, 2.0f, 0.05f, "%.2fx", "3D resolution. Below 1 is upscaled to the display (faster), above 1 supersamples (sharper, slower).");
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
        // Every upscaler (FSR 2 / DLSS are slots that run FSR 1 for now).
        static const char *const up[] = { "Off (native)", "TAA-U", "FSR 1", "FSR 2 (uses FSR 1)", "DLSS (uses FSR 1)" };
        row_fid_combo(c, "Upscaling", FF_UPSCALER, up, NULL, 5, "Renders fewer pixels and upscales them to the display: faster, a little softer.");
        if (cv("r_upscaler") && cv("r_upscaler")->i) row_fid_slider(c, "Upscaling resolution", FF_UPSCALE_RATIO, 0.5f, 1.0f, 0.01f, "%.2fx", "Share of the display resolution rendered before upscaling.");
    }
    if (render_hdr_supported()) {
        row_fid_toggle(c, "HDR10 output", FF_HDR_OUTPUT, NULL);
        if (cv("r_hdr_output") && cv("r_hdr_output")->i) row_slider_cvar(c, "HDR paper white", "r_hdr_paper_white", 100, 300, 10, "%.0f nits", "How bright white menus and the image's mid-tones are.");
    }
    row_slider_cvar(c, "Brightness", "r_brightness", 0.85f, 1.15f, 0.01f, "%.2f", "Calibrates the picture for your display. A small range: night stays dark for everyone.");
    static const char *const cb[] = { "Off", "Protanopia", "Deuteranopia", "Tritanopia" };
    row_combo_cvar(c, "Colour-blind filter", "r_colorblind", cb, 4, "Every Engine Fidelity level, Off included.");
    row_toggle_cvar(c, "Show FPS", "cl_showfps", NULL);
}

static void graphics_comfort(GuiCursor *c) {
    section(c, "Comfort");
    // The FOV model (docs/CAMERA_HOLDPOINT.md "Field of view"): Field of View, ADS Field of View, Weapon
    // Field of View. A host may force one Field of View (sv_fov) or cap it (sv_fov_max).
    {
        Cvar *svf = cv("sv_fov"), *svm = cv("sv_fov_max");
        if (svf && svf->f >= 1.0f) {
            GuiRect r = gw_setting_row(c, "Field of View", "This server sets the same field of view for every player.", rid("fov_fixed"));
            char buf[64];
            snprintf(buf, sizeof(buf), "%.0f (server)", svf->f);
            gw_label(r, buf, gui_s(TH->t_body), GW_REGULAR, TH->text_faint, GUI_LEFT);
        } else {
            row_slider_cvar(c, "Field of View", "cl_fov", FOV_MIN, FOV_MAX, 1, "%.0f",
                            svm && svm->f >= 1.0f ? "Adjust the field of view of the player camera (this server caps it). Higher values show more of the world."
                                                  : "Adjust the field of view of the player camera. Higher values show more of the world; wider screens see more to the sides.");
        }
    }
    static const char *const ads_fov[] = { "Independent", "Affected" };
    row_combo_cvar(c, "ADS Field of View", "cl_ads_fov", ads_fov, 2,
                   "Affected: the field of view while aiming down sights scales with Field of View. Independent: aiming down sights uses the default field of "
                   "view, whatever Field of View is set to.");
    static const char *const weapon_fov[] = { "Narrow", "Default", "Wide" };
    row_combo_cvar(c, "Weapon Field of View", "cl_weapon_fov", weapon_fov, 3,
                   "Adjust the field of view of your weapon and arms, independent of Field of View. Narrow brings the weapon closer, Wide shows more of it.");
    static const char *const motion_blur[] = { "Off", "World", "Hands and world" };
    row_combo_cvar(c, "Motion Blur", "r_motion_blur", motion_blur, 3,
                   "World: the view and moving things blur with motion, your hands and weapon stay sharp. Hands and world: your hands and weapon blur too. Off: no motion blur.");
    row_slider_cvar(c, "Screen effects", "r_post_feedback", 0.5f, 1, 0.05f, "%.2f", "Damage, flash and low-health effects (photosensitivity).");
    static const char *const gore[] = { "Off", "Low", "High" };
    row_combo_cvar(c, "Gore", "gore_level", gore, 3, "Off: neutral impact puffs (hits stay readable), no blood.");
}

static void tab_graphics_normal(GuiCursor *c) {
    graphics_header(c);
    graphics_quality(c);
    graphics_display(c);
    graphics_comfort(c);
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
    row_slider_cvar(c, "Weapon motion", "cl_viewmodel_sway_scale", 0, 1, 0.05f, "%.2f", "How much your weapon sways, bobs and settles in first person (0 = still).");
    section(c, "Gamepad");
    row_slider_cvar(c, "Gamepad sensitivity", "pad_sensitivity", 0.5f, 8, 0.1f, "%.1f", NULL);
    row_slider_cvar(c, "Gamepad deadzone", "pad_deadzone", 0, 0.5f, 0.01f, "%.2f", NULL);
    section(c, "Movement");
    row_toggle_cvar(c, "Toggle crouch", "cl_crouch_toggle", "Press once instead of holding.");
    row_toggle_cvar(c, "Toggle sprint", "cl_sprint_toggle", "Press once instead of holding.");
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

// The crosshair's style (not its behaviour: it always shows the true aim point and the exact spread,
// docs/SETTINGS_AUDIT.md).
static void hud_crosshair(GuiCursor *c) {
    section(c, "Crosshair");
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
    row_toggle_cvar(c, "Tint over enemies / friendlies", "crosshair_target_color", NULL);
    row_toggle_cvar(c, "Hit markers", "crosshair_hitmarker", NULL);
    row_slider_cvar(c, "Red dot brightness", "sight_brightness", 0.25f, 2.0f, 0.05f, "%.2f", "Illuminated reticles of red dots and scopes.");
}

static void tab_audio(GuiCursor *c) {
    section(c, "Volume");
    row_slider_cvar(c, "Master", "snd_master", 0, 1, 0.01f, "%.2f", NULL);
    row_slider_cvar(c, "Music", "snd_music", 0, 1, 0.01f, "%.2f", NULL);
    row_slider_cvar(c, "Effects", "snd_sfx", 0, 1, 0.01f, "%.2f", NULL);
    row_slider_cvar(c, "Voice", "snd_voice", 0, 1, 0.01f, "%.2f", NULL);
    row_slider_cvar(c, "Interface", "snd_ui", 0, 1, 0.01f, "%.2f", NULL);
}

// Phone companion (docs/PHONE.md): server, stream, orientation, capture.
// HUD (docs/UI_WHEELS_HUD.md "Settings"): the compass, kill feed, pings and wheels.
static void tab_hud(GuiCursor *c) {
    section(c, "Interface");
    row_slider_cvar(c, "UI scale", "ui_scale", 0, 3, 0.05f, "%.2f", "0 = automatic from the resolution.");
    row_slider_cvar(c, "Safe area", "ui_safe_area", -1, 0.15f, 0.005f, "%.3f", "Margin around menus and the HUD (-1 = theme default).");
    hud_crosshair(c);
    section(c, "Layout");
    row_slider_cvar(c, "HUD opacity", "hud_opacity", 0.2f, 1, 0.05f, "%.2f", "Compass, kill counter, kill feed and ping markers.");
    row_slider_cvar(c, "Compass and marker size", "hud_scale", 0.75f, 1.5f, 0.05f, "%.2f", NULL);
    section(c, "Compass");
    row_toggle_cvar(c, "Compass", "hud_compass", NULL);
    row_slider_cvar(c, "Compass arc", "hud_compass_arc", 90, 180, 5, "%.0f\xC2\xB0", "How many degrees of heading the strip shows.");
    row_toggle_cvar(c, "Coordinates", "hud_coords", "X, Z and height under the compass. Copy them from the pause menu.");
    row_toggle_cvar(c, "Pings and teammates on the compass", "hud_ping_compass", NULL);
    section(c, "Kills");
    row_toggle_cvar(c, "Kill feed", "hud_killfeed", NULL);
    section(c, "Pings");
    row_toggle_cvar(c, "Ping markers in the world", "hud_ping_markers", NULL);
    row_toggle_cvar(c, "Show other players' pings", "ping_enable", NULL);
    row_toggle_cvar(c, "Ping chat lines", "ping_chat", NULL);
    row_toggle_cvar(c, "Ping sounds", "ping_sound", NULL);
    section(c, "Radial wheels");
    row_toggle_cvar(c, "Hold to open wheels", "wheel_enable", "Off: the keys only do their tap action (quick heal, drop, ping, use).");
    row_slider_cvar(c, "Hold time", "wheel_hold_time", 0.1f, 0.6f, 0.01f, "%.2f s", "How long a key is held before its wheel opens.");
    row_slider_cvar(c, "Slow motion while open", "wheel_timescale", 0.05f, 1, 0.05f, "%.2f", "Single player and hosting alone only.");
    row_toggle_cvar(c, "Use healing items at once", "wheel_heal_auto_use", NULL);
    row_toggle_cvar(c, "Timed action ring", "hud_timed_ring", "The ring for healing, cooking grenades and hold interactions.");
}

static void tab_phone(GuiCursor *c) {
    static const char *const server[] = { "Off", "On", "Automatic" };
    section(c, "Companion page");
    row_combo_cvar(c, "Phone server", "phone_server", server, 3, "Automatic: starts with the main menu (auto-pair) or when the Connect portal opens.");
    row_toggle_cvar(c, "Allow pairing", "phone_pairing", "Scan the QR code on the Connect portal (Player tab, pause menu) with your real phone.");
    row_toggle_cvar(c, "Auto-pair", "phone_auto_pair", "Start the phone server with the main menu: a paired phone reconnects by itself.");
    row_slider_cvar(c, "Remember device", "phone_device_days", 0, 365, 1, "%.0f days", "The paired phone reconnects on the next launch (0 = forget it when the game closes).");
    row_slider_cvar(c, "Server port", "phone_port", 1024, 65000, 1, "%.0f", "HTTPS port; the certificate helper uses the next one. Same Wi-Fi as the phone.");
    section(c, "Stream");
    row_slider_cvar(c, "Frame rate", "phone_fps", 5, 60, 1, "%.0f Hz", "Camera render and stream rate.");
    row_slider_cvar(c, "Quality", "phone_quality", 30, 95, 1, "%.0f", "Maximum JPEG quality; lowered automatically on a slow network.");
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
    row_slider_cvar(c, "Video bitrate", "phone_video_kbps", 500, 20000, 500, "%.0f kbit/s", NULL);
    row_slider_cvar(c, "Video max length", "phone_record_max", 2, 3600, 1, "%.0f s", NULL);
    if (dev_mode()) {
        section(c, "Developer");
        row_toggle_cvar(c, "HTTPS", "phone_tls", "Browsers only give motion sensors to secure pages. Off: Android flags only.");
        row_slider_cvar(c, "Pairing code lifetime", "phone_token_ttl", 10, 600, 10, "%.0f s", NULL);
        row_slider_cvar(c, "Stream width", "phone_res_w", 128, 1440, 16, "%.0f px", NULL);
        row_slider_cvar(c, "Stream height", "phone_res_h", 256, 2560, 16, "%.0f px", NULL);
    }
}

bool settings_draw(GuiRect r) {
    GuiRect content = screens_modal_frame(r, "Settings", NULL);
    // Tabs (LB / RB, Ctrl+Tab cycle).
    const char *labels[TAB_SHOWN];
    int cur = 0;
    S.tab = tab_shown(S.tab);
    for (int i = 0; i < TAB_SHOWN; i++) { labels[i] = k_tabs[k_order[i]]; if (k_order[i] == S.tab) cur = i; }
    GuiRect tr = { content.x, content.y - gui_s(8), content.w, gui_s(34) };
    int ni = gw_tabs(gui_id("settings_tabs"), tr, labels, TAB_SHOWN, cur);
    if (gui_nav(NAV_TAB_NEXT)) { ni = (cur + 1) % TAB_SHOWN; gui_nav_consume(NAV_TAB_NEXT); gui_sound(TH->snd_tab); }
    if (gui_nav(NAV_TAB_PREV)) { ni = (cur + TAB_SHOWN - 1) % TAB_SHOWN; gui_nav_consume(NAV_TAB_PREV); gui_sound(TH->snd_tab); }
    if (k_order[ni] != S.tab) { S.tab = k_order[ni]; gui_set_focus(0); gui_anim_restart(gui_id("settings_tab_in")); }
    float tin = gui_ease_out(gui_anim_t(gui_id("settings_tab_in")) / 0.15f);
    float top = tr.y + tr.h + gui_s(10);
    GuiRect view = { content.x, top, content.w - gui_s(14), content.y + content.h - top - gui_s(24) };
    GuiScroll *sc = &S.scroll[S.tab];
    gw_scroll_begin(sc, gui_idi("settings_scroll", S.tab), view);
    gd_alpha_push(tin);
    GuiRect inner = { view.x + gui_s(10), gw_scroll_y(sc) + (1.0f - tin) * gui_s(8), view.w - gui_s(20), 100000 };
    GuiCursor c = gc_make(inner, gui_s(2));
    switch (S.tab) {
    case TAB_GRAPHICS:
    case TAB_POST: tab_graphics_normal(&c); break;
    case TAB_CONTROLS: tab_controls(&c); break;
    case TAB_AUDIO: tab_audio(&c); break;
    case TAB_PHONE: tab_phone(&c); break;
    case TAB_HUD: tab_hud(&c); break;
    }
    gd_alpha_pop();
    gw_scroll_end(sc, gc_used(&c) + gui_s(16));
    screens_footer_hint(gui_rect(content.x, content.y + content.h - gui_s(18), content.w, gui_s(18)),
                        input_capturing() ? "Press a key or button  \xC2\xB7  Esc cancels" : "Esc / B  back     LB / RB  tabs");
    return false;
}
