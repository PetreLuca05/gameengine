// Settings modal (docs/UI.md "Settings", docs/SETTINGS_AUDIT.md): Graphics (Engine Fidelity, Era,
// Look, then Display and Comfort; everything that changes what a player can see compared with others
// is locked, the quality tiers come from the Engine Fidelity level; the Debug sub-tab with every
// per-feature control exists in dev builds in developer mode only), HUD (crosshair included),
// Controls, Audio, Character, Phone. Display settings (resolution, window mode, vsync) wait for Apply and then ask to be
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
// TAB_POST / TAB_CROSSHAIR stay as indices (settings_open by number) and open those.
#define TAB_SHOWN (TAB_COUNT - 2)
static const int k_order[TAB_SHOWN] = { TAB_GRAPHICS, TAB_HUD, TAB_CONTROLS, TAB_AUDIO, TAB_CHARACTER, TAB_PHONE };
static int tab_alias(int tab) { return tab == TAB_POST ? TAB_GRAPHICS : (tab == TAB_CROSSHAIR ? TAB_HUD : tab); }

// Graphics sub-tabs: Normal (what a player sets) and Debug (developer mode only, docs/UI.md).
enum { GFX_NORMAL, GFX_DEBUG, GFX_PAGES };
static const char *const k_gfx_pages[GFX_PAGES] = { "Normal", "Debug" };

typedef struct Display { int width, height, mode, vsync; } Display;

static struct {
    int tab, gfx_page;
    GuiScroll scroll[TAB_COUNT + 1];   // + the Graphics Debug page
    Display pending, backup;
    float confirm_t;
    int res_w[MAX_RES], res_h[MAX_RES], res_count;
    char res_labels[MAX_RES][24];
    const char *res_items[MAX_RES];
} S;

static Cvar *cv(const char *n) { return cvar_find(n); }

// Developer UI: a dev build in developer mode (the `developer` cvar). The Graphics > Debug page and
// the developer-only rows of the other tabs; a release build never shows them, whatever `developer`
// says (their TIER / LOCKED cvars reject a player's writes there anyway, docs/SETTINGS_AUDIT.md).
static bool dev_ui(void) {
    Cvar *d = cvar_find("developer");
    return cvar_dev_build() && d && d->i;
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
    if (tab >= 0 && tab < TAB_COUNT) S.tab = tab_alias(tab);
    S.pending = read_display();
    collect_resolutions();
}

void settings_graphics_page(int page) { S.gfx_page = DG_CLAMP(page, 0, GFX_PAGES - 1); }

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

// Graphics defaults: the player's graphics settings (the archived CVAR_EDITABLE r_*, post_*, vfx_*
// and the Comfort rows of the Graphics tab) go back to their registered defaults; then the Engine
// Fidelity level (itself back to its default) applies its table defaults to every feature, with the
// custom tweaks cleared. Kept: the display (Apply / Revert, confirmed) and the fidelity benchmark
// results. Tier and locked cvars aren't the player's (docs/SETTINGS_AUDIT.md).
static const char *const k_gfx_keep[] = { "r_width", "r_height", "r_mode", "r_vsync", "r_fidelity_bench", "r_fidelity_recommended", "r_fidelity_autodetect", "r_fidelity_custom" };
static const char *const k_gfx_extra[] = { "cl_fov", "cl_sprint_fov", "cl_headbob", "cl_headbob_amount", "cam_kick_scale", "cl_weapon_sway", "wpn_visual_scale", "cl_showfps" };

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
    fid_custom_load("");
    cvar_set("r_fidelity_custom", "");
    fid_apply(fid_level());
    S.pending = read_display();
    log_info("settings: %d graphics settings back to their defaults (Engine Fidelity %s)", n, fid_level_name(fid_level()));
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

// ------------------------------------------------------------------ Look

// The post-processing Look (docs/LOOKS.md): one dropdown, its line of description, the live
// preview strip (the scene under each look; a click picks it) and hold-to-compare with Realism.
// Looks are fixed: no sliders, no parameters here.
static void look_section(GuiCursor *c) {
    const char *items[LOOK_COUNT];
    for (int i = 0; i < LOOK_COUNT; i++) items[i] = look_display_name(i);
    int sel = game_look_selected();
    GuiRect r = gw_setting_row(c, "Look", "How the image is graded and finished. Independent of Engine Fidelity (what runs) and of the era look.", rid("r_look"));
    r.y += gui_s(3);
    r.h -= gui_s(6);
    if (gw_dropdown(rid("r_look"), r, items, LOOK_COUNT, &sel)) game_look_set(sel);
    gw_label(gc_row(c, gui_s(22)), look_desc(sel), gui_s(TH->t_body - 2), GW_REGULAR, TH->text_dim, GUI_LEFT);

    int tiles = LOOK_COUNT;
    float aspect = 16.0f / 9.0f;
    TextureId tex = render_look_preview(&tiles, &aspect);
    float gap = gui_s(8), cap = gui_s(18);
    float tw = (c->area.w - gap * (float)(tiles - 1)) / (float)tiles, th = tw / aspect;
    GuiRect strip = gc_row(c, th + cap + gui_s(4));
    int shown = game_look_current();
    for (int i = 0; i < tiles && i < LOOK_COUNT; i++) {
        GuiRect t = gui_rect(strip.x + (float)i * (tw + gap), strip.y, tw, th);
        GuiInteract in = gui_interact(gui_idi("look_tile", i), t, 0);
        if (tex) gd_image(t, tex, v4((float)i / (float)tiles, 0.0f, (float)(i + 1) / (float)tiles, 1.0f), UI_RGBA(255, 255, 255, 255));
        else gd_rect(t, UI_RGBA(20, 22, 26, 255));
        u32 border = i == sel ? TH->accent : (in.hovered ? UI_RGBA(255, 255, 255, 90) : UI_RGBA(255, 255, 255, 24));
        gd_round_border(t, gui_s(2), i == sel ? gui_s(2) : 1.0f, 0, border);
        u32 tc = i == shown ? TH->text : TH->text_dim;
        gw_label(gui_rect(t.x, t.y + th + gui_s(2), tw, cap), look_display_name(i), gui_s(TH->t_small), i == sel ? GW_SEMIBOLD : GW_REGULAR, tc, GUI_CENTER);
        if (in.clicked) game_look_set(i);
    }

    GuiRect br = gc_row(c, gui_s(TH->button_h + 4));
    GuiRect b = gui_rect(br.x, br.y, gui_s(300), br.h);
    bool can = sel != LOOK_REALISM;
    GuiInteract in = gui_interact(rid("look_ab"), b, can ? GUI_FOCUSABLE : GUI_DISABLED);
    bool held = can && in.held;
    gd_alpha_push(can ? 1.0f : 0.4f);
    gd_round_border(b, gui_s(TH->radius), 1.0f, held ? gui_rgba_mul(TH->accent, 0.35f) : UI_RGBA(255, 255, 255, in.hovered ? 30 : 12),
                    held ? TH->accent : TH->panel_border);
    gw_label(b, held ? "Showing Realism (release to go back)" : "Hold to compare with Realism", gui_s(TH->t_small + 1), GW_SEMIBOLD, TH->text, GUI_CENTER);
    gd_alpha_pop();
    if (held) game_look_compare_hold(true);
}

// ------------------------------------------------------------------ tabs

// ------------------------------------------------------------------ Graphics

// The tab header: why most graphics settings aren't here (the tooltip), and Default.
#define WHY_LOCKED "Settings that change what you can see compared with other players are the same for everyone, for fair play: foliage and grass density, fog, view, shadow and detail distances, weather, lens droplets, exposure and brightness beyond a small range, the weapon's FOV. Engine Fidelity changes how good they look, never what you can see. A host can set some of them for the whole game (lobby rules)."

static void graphics_header(GuiCursor *c) {
    GuiRect br = gc_row(c, gui_s(TH->button_h + 4));
    GuiRect def = { br.x + br.w - gui_s(140), br.y, gui_s(140), br.h };
    GuiRect why = { br.x, br.y, br.w - def.w - gui_s(16), br.h };
    GuiInteract in = gui_interact(rid("gfx_why"), why, 0);
    gw_label(why, "(i)  Why can't I change X?", gui_s(TH->t_body - 1), GW_SEMIBOLD, in.hovered ? TH->text : TH->text_dim, GUI_LEFT);
    if (in.hovered) gui_tooltip(rid("gfx_why"), WHY_LOCKED);
    if (gw_button(rid("gfx_defaults"), def, "Default", GB_SECONDARY, true))
        screens_confirm(CONFIRM_GRAPHICS_DEFAULTS, "Reset graphics settings?", "Every graphics setting goes back to its default, Engine Fidelity included. Resolution, window mode and VSync stay.", 0);
}

static void era_row(GuiCursor *c) {
    Cvar *era = cv("r_era");
    if (!era) return;
    static const char *const eras[] = { "Modern", "2020", "2015", "2010", "Custom" };
    int sel = DG_CLAMP(era->i, 0, 4);
    if (row_combo(c, "Era", eras, 5, &sel, "How the world's geometry is built and drawn (an era's renderer and features). It never changes what you can see; Engine Fidelity bounds its cost.") && sel < 4)
        cvar_set_int(era, sel);
}

static void display_section(GuiCursor *c) {
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
    Cvar *fps = cv("r_maxfps");
    if (fps) {
        float f = (float)fps->i;
        GuiRect sr = gw_setting_row(c, "Frame rate limit", "0 = unlimited", rid("r_maxfps"));
        if (gw_slider(rid("r_maxfps"), sr, &f, 0, 300, 10, f < 1 ? "Off" : "%.0f")) cvar_set_int(fps, (int)f);
    }
    row_fid_slider(c, "Render scale", FF_RENDER_SCALE, 0.25f, 2.0f, 0.05f, "%.2fx", "3D resolution. Below 1 is upscaled to the display (faster), above 1 supersamples (sharper, slower).");
    {
        // The upscalers that exist (the FSR 2 / DLSS slots stay in Debug).
        static const char *const up[] = { "Off (native)", "TAA-U", "FSR 1" };
        row_fid_combo(c, "Upscaling", FF_UPSCALER, up, NULL, 3, "Renders fewer pixels and upscales them to the display: faster, a little softer.");
        if (cv("r_upscaler") && cv("r_upscaler")->i) row_fid_slider(c, "Upscaling resolution", FF_UPSCALE_RATIO, 0.5f, 1.0f, 0.01f, "%.2fx", "Share of the display resolution rendered before upscaling.");
    }
    if (render_hdr_supported()) {
        row_fid_toggle(c, "HDR10 output", FF_HDR_OUTPUT, NULL);
        if (cv("r_hdr_output") && cv("r_hdr_output")->i) row_slider_cvar(c, "HDR paper white", "r_hdr_paper_white", 100, 300, 10, "%.0f nits", "How bright white menus and mid-tones are on an HDR display.");
    }
    row_slider_cvar(c, "Brightness", "r_brightness", 0.85f, 1.15f, 0.01f, "%.2f", "Display calibration in a narrow range: the darkness at night is the same for every player.");
    static const char *const cb[] = { "Off", "Protanopia", "Deuteranopia", "Tritanopia" };
    row_combo_cvar(c, "Colour-blind filter", "r_colorblind", cb, 4, "Every Engine Fidelity level, Off included.");
    row_toggle_cvar(c, "Show FPS", "cl_showfps", NULL);
}

static void comfort_section(GuiCursor *c) {
    section(c, "Comfort");
    {
        // The world camera FOV is the player's, unless the host forces one (sv_fov) or caps it (sv_fov_max).
        Cvar *svf = cv("sv_fov"), *svm = cv("sv_fov_max");
        if (svf && svf->f >= 1.0f) {
            GuiRect r = gw_setting_row(c, "Field of view", "This server sets the same FOV for every player.", rid("fov_fixed"));
            char buf[64];
            snprintf(buf, sizeof(buf), "%.0f\xC2\xB0 (server)", camera_base_fov());
            gw_label(r, buf, gui_s(TH->t_body), GW_REGULAR, TH->text_faint, GUI_LEFT);
        } else {
            row_slider_cvar(c, "Field of view", "cl_fov", 60, 120, 1, "%.0f\xC2\xB0",
                            svm && svm->f >= 1.0f ? "Vertical camera FOV, 60 .. 120 (this server caps it). Your hands / weapon follow it; scopes magnify inside the scope."
                                                  : "Vertical camera FOV, 60 .. 120 (75 = 107\xC2\xB0 horizontal at 16:9). Your hands / weapon follow it; scopes magnify inside the scope.");
        }
    }
    row_slider_cvar(c, "Sprint FOV effect", "cl_sprint_fov", 0, 7, 0.5f, "%.1f\xC2\xB0", "How much the view widens while sprinting (0 = none).");
    row_slider_cvar(c, "Head bob", "cl_headbob_amount", 0, 1, 0.05f, "%.2f", "Camera bob while walking and running.");
    row_slider_cvar(c, "Camera shake", "cam_kick_scale", 0.25f, 1, 0.05f, "%.2f", "Camera kick and punch on shots (aim recoil is the same at every setting).");
    row_slider_cvar(c, "Weapon sway", "cl_weapon_sway", 0, 1, 0.05f, "%.2f", "Breathing, look lag, bob and landing of the held weapon (cosmetic: aiming stays exact).");
    row_slider_cvar(c, "Weapon kick", "wpn_visual_scale", 0.5f, 1, 0.05f, "%.2f", "Visual kick of the camera and the weapon on shots (aim recoil is the same at every setting).");
    static const char *const mb[] = { "Off", "Camera", "Camera + objects" };
    row_fid_combo(c, "Motion blur", FF_MOTION_BLUR, mb, NULL, 3, "Your weapon is never blurred.");
    if (cv("r_motion_blur") && cv("r_motion_blur")->i) row_slider_cvar(c, "Motion blur strength", "r_motion_blur_strength", 0.05f, 1, 0.05f, "%.2f", NULL);
    static const char *const dof[] = { "Off", "When aiming", "Always" };
    row_fid_combo(c, "Depth of field", FF_DOF, dof, NULL, 3, "Off also turns off the slight blur on your weapon while aiming.");
    row_fid_slider(c, "Film grain", FF_FILM_GRAIN, 0, 1, 0.01f, "%.2f", "Strength of the Look's film grain (0.04 = as the Look intends).");
    row_fid_slider(c, "Chromatic aberration", FF_CHROMATIC, 0, 2, 0.05f, "%.2f", "Strength of the Look's chromatic aberration (0.15 = as the Look intends).");
    row_fid_slider(c, "Vignette", FF_VIGNETTE, 0, 1, 0.05f, "%.2f", "Strength of the Look's vignette (0.25 = as the Look intends).");
    row_fid_slider(c, "Sharpening", FF_SHARPEN, 0, 1, 0.05f, "%.2f", "0.35 = as the Look intends.");
    row_slider_cvar(c, "Screen effects", "r_post_feedback", 0.5f, 1, 0.05f, "%.2f", "Damage, flash and low-health effects (reduced for photosensitivity, never off).");
}

static void tab_graphics(GuiCursor *c) {
    graphics_header(c);
    fidelity_section(c);
    era_row(c);
    look_section(c);
    display_section(c);
    comfort_section(c);
}

// ------------------------------------------------------------------ Graphics > Debug
// Everything else (developer mode only): the plain Engine Fidelity switch and its tweaks, the
// diagnostics, every per-feature control behind the simple qualities, the renderer switch.

static void tab_graphics_debug(GuiCursor *c) {
    fidelity_section(c);
    section(c, "Diagnostics");
    row_toggle_cvar(c, "Performance overlay", "r_perf_overlay", "FPS, CPU / GPU frame time and the Engine Fidelity budget.");

    section(c, "Resolution & upscaling");
    static const char *const up[] = { "Off (native)", "TAA-U", "FSR 1", "FSR 2 (uses FSR 1)", "DLSS (uses FSR 1)" };
    row_fid_combo(c, "Upscaler", FF_UPSCALER, up, NULL, 5, NULL);
    if (cv("r_upscaler") && cv("r_upscaler")->i) row_fid_slider(c, "Upscaler resolution", FF_UPSCALE_RATIO, 0.5f, 1.0f, 0.01f, "%.2fx", NULL);

    section(c, "Post-processing & Effects");
    if (!fid_level_post(fid_level()))
        gw_label(gc_row(c, gui_s(22)), "Engine Fidelity Off: no post-processing. The scene goes straight to the screen (the Look's colour grade stays).", gui_s(TH->t_body - 2), GW_REGULAR, TH->text_faint, GUI_LEFT);
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
        if (!any) row_locked(c, "Post-processing preset", why, NULL);
        else {
            GuiRect r = gw_setting_row(c, "Post-processing preset", "Sets the effects below at once, within the Engine Fidelity level.", rid("post_preset"));
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
    if (cv("r_motion_blur") && cv("r_motion_blur")->i) row_slider_cvar(c, "Motion blur strength", "r_motion_blur_strength", 0.05f, 1, 0.05f, "%.2f", NULL);
    static const char *const dof[] = { "Off", "When aiming", "Always" };
    row_fid_combo(c, "Depth of field", FF_DOF, dof, NULL, 3, NULL);
    static const char *const dofq[] = { "Gaussian (16 taps)", "Hybrid (32 taps)", "Bokeh (48 taps)" };
    if (cv("r_dof") && cv("r_dof")->i) row_fid_combo(c, "Depth of field quality", FF_DOF_QUALITY, dofq, NULL, 3, NULL);
    row_fid_toggle(c, "Bloom", FF_BLOOM, NULL);
    static const char *const bq[] = { "Tiny", "Low", "High" };
    if (cv("r_bloom") && cv("r_bloom")->i) {
        row_fid_combo(c, "Bloom quality", FF_BLOOM_QUALITY, bq, NULL, 3, NULL);
        row_slider_cvar(c, "Bloom intensity", "r_bloom_intensity", 0, 0.3f, 0.005f, "%.3f", "Strength of the Look's bloom (0.05 = as the Look intends).");
    }
    row_fid_slider(c, "Lens flare", FF_LENS_FLARE, 0, 2, 0.05f, "%.2f", NULL);
    row_fid_slider(c, "Lens dirt", FF_LENS_DIRT, 0, 2, 0.05f, "%.2f", NULL);
    row_fid_slider(c, "Chromatic aberration", FF_CHROMATIC, 0, 2, 0.05f, "%.2f", "Strength of the Look's chromatic aberration (0.15 = as the Look intends).");
    row_fid_slider(c, "Lens distortion", FF_LENS_DISTORTION, -1, 1, 0.05f, "%.2f", NULL);
    row_fid_toggle(c, "Lens droplets", FF_LENS_DROPS, "Rain drops on the camera in first person.");
    if (cv("r_lens_drops") && cv("r_lens_drops")->i && fid_level() >= FID_HIGH) row_slider_cvar(c, "Lens droplet intensity", "r_lens_drops_intensity", 0.0f, 1.0f, 0.05f, "%.2f", NULL);
    row_fid_slider(c, "Vignette", FF_VIGNETTE, 0, 1, 0.05f, "%.2f", "Strength of the Look's vignette (0.25 = as the Look intends).");
    row_fid_slider(c, "Film grain", FF_FILM_GRAIN, 0, 1, 0.01f, "%.2f", "Strength of the Look's film grain (0.04 = as the Look intends).");
    // The tonemapper belongs to the Look (docs/LOOKS.md); r_tonemapper only drives r_post_compare / photo mode.
    row_fid_toggle(c, "Auto exposure", FF_AUTO_EXPOSURE, "Off: fixed exposure.");
    row_slider_cvar(c, "Exposure compensation", "r_exposure", -3, 3, 0.1f, "%+.1f EV", NULL);

    // Unity-style Quality > Shadows (docs/SHADOWS_UNITY_MODEL.md), bounded by Engine Fidelity.
    section(c, "Shadows & lighting");
    static const char *const shmode[] = { "Disable", "Hard Only", "Hard and Soft" };
    row_fid_combo(c, "Shadows", FF_SHADOWS, shmode, NULL, 3, "Hard Only renders lights set to Soft with hard edges.");
    static const char *const shres[] = { "Low", "Medium", "High", "Very High" };
    row_fid_combo(c, "Shadow resolution", FF_SHADOW_RESOLUTION, shres, NULL, 4, "For lights on 'Use Quality Settings': sun cascades 512 .. 4096, lamps / flashlights 256 .. 2048.");
    static const char *const shproj[] = { "Close Fit", "Stable Fit" };
    row_fid_combo(c, "Shadow projection", FF_SHADOW_PROJECTION, shproj, NULL, 2, "Close Fit: sharper, edges swim as you turn. Stable Fit: edges never swim.");
    row_fid_slider(c, "Shadow distance", FF_SHADOW_DISTANCE, 10.0f, 1000.0f, 5.0f, "%.0f m", "The last cascade ends here and fades over the cascade border.");
    static const char *const casc[] = { "No Cascades", "Two Cascades", "Four Cascades" };
    static const int casc_v[] = { 1, 2, 4 };
    row_fid_combo(c, "Shadow cascades", FF_SHADOW_CASCADES, casc, casc_v, 3, NULL);
    {
        // Cascade splits (Unity: percentages of the shadow distance) and the live split bar.
        Cvar *cc = cv("r_shadow_cascades");
        int nc = cc ? (cc->i >= 3 ? 4 : cc->i) : 4;
        if (nc == 2) row_slider_cvar(c, "Cascade split", "r_shadow_split2", 1, 99, 0.5f, "%.1f %%", "Where cascade 1 starts (% of the shadow distance).");
        if (nc == 4) {
            row_slider_cvar(c, "Cascade split 1", "r_shadow_split4_1", 1, 97, 0.5f, "%.1f %%", NULL);
            row_slider_cvar(c, "Cascade split 2", "r_shadow_split4_2", 2, 98, 0.5f, "%.1f %%", NULL);
            row_slider_cvar(c, "Cascade split 3", "r_shadow_split4_3", 3, 99, 0.5f, "%.1f %%", NULL);
        }
        float pct[4] = { 100, 100, 100, 100 };
        if (nc == 2 && cv("r_shadow_split2")) pct[0] = cv("r_shadow_split2")->f;
        if (nc == 4) {
            const char *k[3] = { "r_shadow_split4_1", "r_shadow_split4_2", "r_shadow_split4_3" };
            for (int i = 0; i < 3; i++) pct[i] = cv(k[i]) ? cv(k[i])->f : 100.0f;
        }
        GuiRect r = gw_setting_row(c, "Cascade bar", "Each cascade's share of the shadow distance (the colours of r_shadow_debug 4).", rid("shadow_split_bar"));
        r.y += gui_s(5);
        r.h -= gui_s(10);
        static const u32 col[4] = { UI_RGBA(230, 70, 60, 255), UI_RGBA(70, 220, 90, 255), UI_RGBA(80, 110, 240, 255), UI_RGBA(240, 210, 60, 255) };
        float x0 = r.x, prev = 0.0f;
        for (int i = 0; i < nc; i++) {
            float p = i == nc - 1 ? 100.0f : fminf(fmaxf(pct[i], prev), 100.0f);
            float x1 = r.x + r.w * p * 0.01f;
            gd_rect((GuiRect){ x0, r.y, fmaxf(x1 - x0, 1.0f), r.h }, col[i]);
            char t[16];
            snprintf(t, sizeof(t), "%.1f%%", p - prev);
            gd_text_simple(x0 + gui_s(4), r.y + r.h * 0.5f - gui_s(7), gui_s(12), GW_REGULAR, UI_RGBA(20, 20, 20, 255), GUI_LEFT, t);
            x0 = x1;
            prev = p;
        }
        gd_border(r, 1.0f, UI_RGBA(235, 235, 220, 200));
        row_slider_cvar(c, "Cascade border", "r_shadow_cascade_border", 0, 50, 1, "%.0f %%", "Blend band at the end of each cascade, and the fade at the shadow distance.");
    }
    static const char *const soft[] = { "Low (tent 3x3)", "Medium (tent 5x5)", "High (tent 7x7)", "Ultra (contact hardening)" };
    row_fid_combo(c, "Soft shadow quality", FF_SOFT_SHADOW_QUALITY, soft, NULL, 4, "Softness also follows the sky: crisp under the sun, soft in haze, none under overcast.");
    row_slider_cvar(c, "Shadow near plane offset", "r_shadow_near_offset", 2, 5000, 10, "%.0f m", "How far each cascade reaches towards the sun for casters outside the view.");
    static const char *const smask[] = { "Shadowmask", "Distance Shadowmask" };
    row_combo_cvar(c, "Shadowmask mode", "r_shadowmask_mode", smask, 2, "Reserved for baked lighting (no baked data yet).");
    row_fid_slider(c, "Skip shadows of props under", FF_SHADOW_MIN_RADIUS, 0, 4, 0.25f, "%.2f m", "Small props cast no sun shadow (cheaper).");
    row_fid_toggle(c, "Lamp / flashlight shadows", FF_LIGHT_SHADOWS, NULL);
    row_fid_toggle(c, "Cloud shadows", FF_CLOUD_SHADOWS, NULL);
    row_fid_toggle(c, "Moon shadows at twilight", FF_MOON_SHADOWS, "The moon casts its own shadows while the sun is still up.");

    section(c, "Geometry, textures & foliage");
    {
        Cvar *era = cv("r_era");
        if (era) {
            static const char *const eras[] = { "Modern", "2020", "2015", "2010", "Custom" };
            int sel = DG_CLAMP(era->i, 0, 4);
            if (row_combo(c, "Era look", eras, 5, &sel, "The geometry / foliage of an era (and the features it had). Engine Fidelity bounds its cost.") && sel < 4) cvar_set_int(era, sel);
        }
    }
    row_fid_toggle(c, "Virtualized geometry (Nanite)", FF_VGEO,
                   "On: GPU cluster culling, continuous LOD, occlusion culling. Off: the classic instanced renderer (discrete LODs, impostors; often faster on foliage). Applies at once.");
    static const char *const texq[] = { "Low (mips -2)", "Medium (mips -1)", "High" };
    row_fid_combo(c, "Textures", FF_TEXTURE_QUALITY, texq, NULL, 3, NULL);
    {
        static const char *const aniso[] = { "Off", "2x", "4x", "8x", "16x" };
        static const int vals[] = { 1, 2, 4, 8, 16 };
        row_fid_combo(c, "Anisotropic filtering", FF_ANISO, aniso, vals, 5, NULL);
    }
    row_fid_slider(c, "Foliage density", FF_FOLIAGE_DENSITY, 0, 1, 0.05f, "%.2f", NULL);
    row_fid_slider(c, "Impostors below", FF_IMPOSTOR_PX, 0, 128, 4, "%.0f px", "Distant foliage switches to impostor cards below this size (larger: earlier).");
    static const char *const terrain[] = { "Minimal", "Low", "Medium", "High", "Ultra" };
    row_fid_combo(c, "Terrain detail", FF_TERRAIN_QUALITY, terrain, NULL, 5,
                  "Ground texturing: tiling breakers, colour variation, transitions, parallax, scatter (docs/TERRAIN_SHADING.md).");
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

    section(c, "Sights");
    row_fid_slider(c, "Scope picture resolution", FF_SCOPE_PIP_SCALE, 0.2f, 2.0f, 0.05f, "%.2f", "1 = the render height, above 1 supersamples (sharper, slower).");
    row_fid_toggle(c, "Depth of field while aiming", FF_ADS_DOF, "A slight blur on the weapon while aimed.");
    section(c, "Weather");
    {
        static const char *const q[] = { "Auto (post preset)", "Off", "Low", "Medium", "High" };
        static const int qv[] = { -1, 0, 1, 2, 3 };
        row_fid_combo(c, "Rain / snow quality", FF_PRECIP_QUALITY, q, qv, 5, "Particle count of the rain / snow around you.");
    }
    row_fid_toggle(c, "Puddle reflections", FF_PUDDLE_REFLECTIONS, "Mirror-smooth puddles (screen-space reflections).");
    {
        static const char *const wq[] = { "Low (sky reflection)", "Medium (+ reflections, refraction)", "High (+ ripples, caustics, waves)", "Ultra (+ every detail)" };
        row_fid_combo(c, "Water", FF_WATER, wq, NULL, 4, "Lakes, rivers and the sea: reflections, refraction, ripples, caustics, ocean waves.");
    }
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

// HUD > Crosshair: the style is the player's; where it points (the true aim point, obstruction) and
// its spread gap are fixed (docs/SETTINGS_AUDIT.md).
static void crosshair_sections(GuiCursor *c) {
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
    section(c, "Interface");
    row_slider_cvar(c, "UI scale", "ui_scale", 0, 3, 0.05f, "%.2f", "0 = automatic from the resolution.");
    row_slider_cvar(c, "Safe area", "ui_safe_area", -1, 0.15f, 0.005f, "%.3f", "Margin around menus and the HUD (-1 = theme default).");
    section(c, "Content");
    static const char *const gore[] = { "Off", "Low", "High" };
    row_combo_cvar(c, "Gore", "gore_level", gore, 3, "Off: dust puffs instead of blood (hits stay readable).");
    // Spectating (docs/SPECTATE.md).
    section(c, "Spectating");
    row_slider_cvar(c, "Death camera time", "spec_death_cam_time", 0, 10, 0.5f, "%.1f s",
                    "How long the camera stays on your body before you watch your killer (or the nearest player). Fire skips it.");
    row_slider_cvar(c, "Camera blend on switch", "spec_blend_time", 0, 0.5f, 0.01f, "%.2f s", "Smooths the jump to the next player.");
    if (dev_ui()) {
        section(c, "Developer");
        row_toggle_cvar(c, "Third person view", "cl_thirdperson", NULL);
        row_toggle_cvar(c, "Spectator delay readout", "spec_show_delay",
                        "Time from the watched player's input to your screen. The minimum is one network hop from them to the host plus one from the host to you: their own commands are replayed here, no interpolation delay on top.");
    }
}

// Phone companion (docs/PHONE.md): server, stream, orientation, capture.
// HUD (docs/UI_WHEELS_HUD.md "Settings"): the compass, kill counter / feed, pings and wheels.
static void tab_hud(GuiCursor *c) {
    crosshair_sections(c);
    section(c, "Layout");
    row_slider_cvar(c, "HUD scale", "hud_scale", 0.75f, 1.5f, 0.05f, "%.2f", "Size of the compass and the ping markers.");
    row_slider_cvar(c, "HUD opacity", "hud_opacity", 0.2f, 1, 0.05f, "%.2f", "Compass, kill counter, kill feed and ping markers.");
    row_slider_cvar(c, "Phone panel size", "hud_phone_size", 0.12f, 0.4f, 0.01f, "%.2f", NULL);
    static const char *const corners[] = { "Bottom right", "Bottom left", "Top right", "Top left" };
    row_combo_cvar(c, "Phone panel corner", "hud_phone_corner", corners, 4, NULL);
    section(c, "Compass");
    row_toggle_cvar(c, "Compass", "hud_compass", NULL);
    row_slider_cvar(c, "Compass arc", "hud_compass_arc", 90, 180, 5, "%.0fÂ°", "Degrees of bearing the strip spans.");
    row_toggle_cvar(c, "Coordinates", "hud_coords", "X, Z and height under the compass. Copy them from the pause menu.");
    row_toggle_cvar(c, "Pings and teammates on the compass", "hud_ping_compass", NULL);
    section(c, "Kills");
    row_toggle_cvar(c, "Kill counter", "hud_killcount", NULL);
    row_toggle_cvar(c, "Always show zombies / players", "hud_killcount_split", "Otherwise on hover and for a moment after each kill.");
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
    row_combo_cvar(c, "Phone server", "phone_server", server, 3, "Automatic: starts the first time you take the phone out (X).");
    row_toggle_cvar(c, "Allow pairing", "phone_pairing", "Scan the QR code on the Connect portal with your real phone.");
    row_toggle_cvar(c, "Pair on equip", "phone_auto_pair", "Equipping the phone opens the Connect portal at once. Off: the in-game phone.");
    row_slider_cvar(c, "Pairing timeout", "phone_pair_timeout", 0, 300, 5, "%.0f s", "No device by then: the in-game phone (0 = keep waiting).");
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
    if (dev_ui()) {
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
    S.tab = tab_alias(S.tab);
    for (int i = 0; i < TAB_SHOWN; i++) { labels[i] = k_tabs[k_order[i]]; if (k_order[i] == S.tab) cur = i; }
    GuiRect tr = { content.x, content.y - gui_s(8), content.w, gui_s(34) };
    int ni = gw_tabs(gui_id("settings_tabs"), tr, labels, TAB_SHOWN, cur);
    if (gui_nav(NAV_TAB_NEXT)) { ni = (cur + 1) % TAB_SHOWN; gui_nav_consume(NAV_TAB_NEXT); gui_sound(TH->snd_tab); }
    if (gui_nav(NAV_TAB_PREV)) { ni = (cur + TAB_SHOWN - 1) % TAB_SHOWN; gui_nav_consume(NAV_TAB_PREV); gui_sound(TH->snd_tab); }
    if (k_order[ni] != S.tab) { S.tab = k_order[ni]; gui_set_focus(0); gui_anim_restart(gui_id("settings_tab_in")); }
    float tin = gui_ease_out(gui_anim_t(gui_id("settings_tab_in")) / 0.15f);
    float top = tr.y + tr.h + gui_s(10);
    // Graphics: the Normal / Debug sub-tabs, only in a dev build's developer mode (a player gets Normal alone).
    bool dev = dev_ui();
    if (!dev) S.gfx_page = GFX_NORMAL;
    if (S.tab == TAB_GRAPHICS && dev) {
        GuiRect sr = { content.x + gui_s(10), top - gui_s(2), content.w - gui_s(34), gui_s(30) };
        int np = gw_tabs(gui_id("settings_gfx_pages"), sr, k_gfx_pages, GFX_PAGES, S.gfx_page);
        if (np != S.gfx_page) { S.gfx_page = np; gui_anim_restart(gui_id("settings_tab_in")); }
        top = sr.y + sr.h + gui_s(8);
    }
    GuiRect view = { content.x, top, content.w - gui_s(14), content.y + content.h - top - gui_s(24) };
    int si = S.tab == TAB_GRAPHICS && S.gfx_page == GFX_DEBUG ? TAB_COUNT : S.tab;
    GuiScroll *sc = &S.scroll[si];
    gw_scroll_begin(sc, gui_idi("settings_scroll", si), view);
    gd_alpha_push(tin);
    GuiRect inner = { view.x + gui_s(10), gw_scroll_y(sc) + (1.0f - tin) * gui_s(8), view.w - gui_s(20), 100000 };
    GuiCursor c = gc_make(inner, gui_s(2));
    switch (S.tab) {
    case TAB_GRAPHICS:
    case TAB_POST:
        if (S.gfx_page == GFX_DEBUG) tab_graphics_debug(&c);
        else tab_graphics(&c);
        break;
    case TAB_CONTROLS: tab_controls(&c); break;
    case TAB_CROSSHAIR: tab_hud(&c); break;
    case TAB_AUDIO: tab_audio(&c); break;
    case TAB_CHARACTER: tab_character(&c); break;
    case TAB_PHONE: tab_phone(&c); break;
    case TAB_HUD: tab_hud(&c); break;
    }
    gd_alpha_pop();
    gw_scroll_end(sc, gc_used(&c) + gui_s(16));
    screens_footer_hint(gui_rect(content.x, content.y + content.h - gui_s(18), content.w, gui_s(18)),
                        input_capturing() ? "Press a key or button  \xC2\xB7  Esc cancels" : "Esc / B  back     LB / RB  tabs");
    return false;
}
