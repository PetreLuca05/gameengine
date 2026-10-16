// Settings modal (docs/UI.md "Settings"): Graphics, Post-processing, Controls, Crosshair, Audio,
// Character. Display settings (resolution, window mode, vsync) wait for Apply and then ask to be
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
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define TH (gui_theme())
#define MAX_RES 48

enum { TAB_GRAPHICS, TAB_CONTROLS, TAB_CROSSHAIR, TAB_AUDIO, TAB_CHARACTER, TAB_POST, TAB_PHONE, TAB_COUNT };
static const char *const k_tabs[TAB_COUNT] = { "Graphics", "Controls", "Crosshair", "Audio", "Character", "Post-processing", "Phone" };
// Display order of the tabs.
static const int k_order[TAB_COUNT] = { TAB_GRAPHICS, TAB_POST, TAB_CONTROLS, TAB_CROSSHAIR, TAB_AUDIO, TAB_CHARACTER, TAB_PHONE };

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
    if (tab >= 0 && tab < TAB_COUNT) S.tab = tab;
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

static void tab_graphics(GuiCursor *c, GuiRect content) {
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

    section(c, "Quality");
    row_slider_cvar(c, "Render scale", "r_render_scale", 0.5f, 2.0f, 0.05f, "%.2fx", NULL);
    static const char *const shadow[] = { "Off", "Low", "Medium", "High" };
    row_combo_cvar(c, "Shadows", "r_shadow_quality", shadow, 4, NULL);
    static const char *const texq[] = { "Low", "Medium", "High" };
    row_combo_cvar(c, "Textures", "r_texture_quality", texq, 3, NULL);
    static const char *const vfxq[] = { "Low", "Medium", "High", "Ultra" };
    row_combo_cvar(c, "Effects (particles)", "vfx_quality", vfxq, 4, NULL);
    static const char *const gore[] = { "Off", "Low", "High" };
    row_combo_cvar(c, "Gore", "gore_level", gore, 3, NULL);
    {
        Cvar *an = cv("r_aniso");
        if (an) {
            static const char *const aniso[] = { "Off", "2x", "4x", "8x", "16x" };
            static const int vals[] = { 1, 2, 4, 8, 16 };
            int s = 0;
            for (int i = 0; i < 5; i++) if (vals[i] == an->i) s = i;
            if (row_combo(c, "Anisotropic filtering", aniso, 5, &s, NULL)) cvar_set_int(an, vals[s]);
        }
    }
    section(c, "Sights");
    static const char *const scope[] = { "Picture-in-picture", "Full screen" };
    row_combo_cvar(c, "Scopes", "r_scope_mode", scope, 2, "Picture-in-picture keeps the world visible around the scope.");
    row_slider_cvar(c, "Scope picture resolution", "r_scope_pip_scale", 0.2f, 2.0f, 0.05f, "%.2f", "1 = the render height, above 1 supersamples (sharper, slower).");
    row_slider_cvar(c, "Red dot brightness", "sight_brightness", 0.0f, 3.0f, 0.05f, "%.2f", NULL);
    row_toggle_cvar(c, "Depth of field while aiming", "ads_dof", "A slight blur on the weapon while aimed.");
    section(c, "Weather");
    {
        Cvar *pq = cv("r_precip_quality");
        if (pq) {
            static const char *const q[] = { "Auto (post preset)", "Off", "Low", "Medium", "High" };
            int sel = pq->i + 1;
            if (row_combo(c, "Rain / snow quality", q, 5, &sel, "Particle count of the rain / snow around you.")) cvar_set_int(pq, sel - 1);
        }
    }
    row_toggle_cvar(c, "Puddle reflections", "r_puddle_reflections", "Mirror-smooth puddles (screen-space reflections).");
    row_toggle_cvar(c, "Lens droplets", "r_lens_drops", "Rain drops on the camera in first person.");
    if (cv("r_lens_drops") && cv("r_lens_drops")->i) row_slider_cvar(c, "Lens droplet intensity", "r_lens_drops_intensity", 0.0f, 1.0f, 0.05f, "%.2f", NULL);
    row_toggle_cvar(c, "Cloud shadows", "r_cloud_shadows", NULL);
    (void)content;
}

static void tab_post(GuiCursor *c) {
    section(c, "Preset");
    int cur = game_post_detect_preset();
    static const char *const presets[] = { "Low", "Medium", "High", "Ultra", "Custom" };
    int sel = cur;
    if (row_combo(c, "Quality preset", presets, 5, &sel, NULL) && sel < 4) game_post_apply_preset(sel);
    section(c, "Anti-aliasing / upscaling");
    static const char *const aa[] = { "Off", "FXAA", "SMAA 1x", "TAA" };
    row_combo_cvar(c, "Anti-aliasing", "r_aa", aa, 4, NULL);
    static const char *const up[] = { "Off (native)", "TAA-U", "FSR 1", "FSR 2 (uses FSR 1)", "DLSS (uses FSR 1)" };
    row_combo_cvar(c, "Upscaler", "r_upscaler", up, 5, NULL);
    if (cv("r_upscaler") && cv("r_upscaler")->i) row_slider_cvar(c, "Upscaler resolution", "r_upscale_ratio", 0.5f, 1.0f, 0.01f, "%.2fx", NULL);
    row_slider_cvar(c, "Sharpening", "r_sharpen", 0, 1, 0.05f, "%.2f", NULL);
    section(c, "Lighting");
    static const char *const ao[] = { "Off", "Low", "High" };
    row_combo_cvar(c, "Ambient occlusion", "r_ao", ao, 3, NULL);
    row_combo_cvar(c, "Contact shadows", "r_contact_shadows", ao, 3, NULL);
    static const char *const ssr[] = { "Off", "Half resolution", "Full resolution" };
    row_combo_cvar(c, "Screen-space reflections", "r_ssr", ssr, 3, NULL);
    row_toggle_cvar(c, "Screen-space global illumination", "r_ssgi", NULL);
    static const char *const vol[] = { "Off", "Low", "High", "Ultra" };
    row_combo_cvar(c, "Volumetric fog", "r_volumetrics", vol, 4, NULL);
    section(c, "Camera");
    static const char *const mb[] = { "Off", "Camera", "Camera + objects" };
    row_combo_cvar(c, "Motion blur", "r_motion_blur", mb, 3, NULL);
    if (cv("r_motion_blur") && cv("r_motion_blur")->i) row_slider_cvar(c, "Motion blur strength", "r_motion_blur_strength", 0.05f, 1, 0.05f, "%.2f", NULL);
    static const char *const dof[] = { "Off", "When aiming", "Always" };
    row_combo_cvar(c, "Depth of field", "r_dof", dof, 3, NULL);
    row_toggle_cvar(c, "Bloom", "r_bloom", NULL);
    static const char *const bq[] = { "-", "Low", "High" };
    row_combo_cvar(c, "Bloom quality", "r_bloom_quality", bq, 3, NULL);
    row_slider_cvar(c, "Bloom intensity", "r_bloom_intensity", 0, 0.3f, 0.005f, "%.3f", NULL);
    row_slider_cvar(c, "Lens flare", "r_lens_flare", 0, 2, 0.05f, "%.2f", NULL);
    row_slider_cvar(c, "Lens dirt", "r_lens_dirt", 0, 2, 0.05f, "%.2f", NULL);
    row_slider_cvar(c, "Chromatic aberration", "r_chromatic", 0, 2, 0.05f, "%.2f", NULL);
    row_slider_cvar(c, "Vignette", "r_vignette", 0, 1, 0.05f, "%.2f", NULL);
    row_slider_cvar(c, "Film grain", "r_film_grain", 0, 1, 0.01f, "%.2f", NULL);
    section(c, "Image");
    static const char *const tm[] = { "ACES (filmic)", "AgX", "PBR Neutral" };
    row_combo_cvar(c, "Tonemapper", "r_tonemapper", tm, 3, NULL);
    row_toggle_cvar(c, "Auto exposure", "r_auto_exposure", NULL);
    row_slider_cvar(c, "Exposure compensation", "r_exposure", -3, 3, 0.1f, "%+.1f EV", NULL);
    static const char *const cb[] = { "Off", "Protanopia", "Deuteranopia", "Tritanopia" };
    row_combo_cvar(c, "Colour-blind filter", "r_colorblind", cb, 4, NULL);
    row_slider_cvar(c, "Screen effects", "r_post_feedback", 0, 1, 0.05f, "%.2f", "Damage, flash and low-health effects.");
    if (render_hdr_supported()) {
        row_toggle_cvar(c, "HDR10 output", "r_hdr_output", NULL);
        row_slider_cvar(c, "HDR paper white", "r_hdr_paper_white", 80, 500, 10, "%.0f nits", NULL);
        row_slider_cvar(c, "HDR peak brightness", "r_hdr_max_nits", 400, 4000, 50, "%.0f nits", NULL);
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
    const char *labels[TAB_COUNT];
    int cur = 0;
    for (int i = 0; i < TAB_COUNT; i++) { labels[i] = k_tabs[k_order[i]]; if (k_order[i] == S.tab) cur = i; }
    GuiRect tr = { content.x, content.y - gui_s(8), content.w, gui_s(34) };
    int ni = gw_tabs(gui_id("settings_tabs"), tr, labels, TAB_COUNT, cur);
    if (gui_nav(NAV_TAB_NEXT)) { ni = (cur + 1) % TAB_COUNT; gui_nav_consume(NAV_TAB_NEXT); gui_sound(TH->snd_tab); }
    if (gui_nav(NAV_TAB_PREV)) { ni = (cur + TAB_COUNT - 1) % TAB_COUNT; gui_nav_consume(NAV_TAB_PREV); gui_sound(TH->snd_tab); }
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
    case TAB_POST: tab_post(&c); break;
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
