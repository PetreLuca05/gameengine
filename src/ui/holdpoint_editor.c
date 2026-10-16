// Hold point editor (holdpoint_editor / F8): live-edit every hold point style x state, preview a
// state on the local player (forced, procedural layer optional), save to assets/human/holdpoint.json.
// The gizmo is cam_debug's hold point axes / pivot / sockets (enabled while the editor is open).
#include "holdpoint_editor.h"
#include "ui.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/log.h"
#include "anim/anim_library.h"
#include "game/game_internal.h"
#include "nuklear_config.h"
#include <stdio.h>
#include <string.h>

static struct {
    bool open;
    int style, state;
    bool preview, static_pose;
    int prev_debug;
    char status[160];
} E = { .style = 2, .preview = true };

static float S(float v) { return v * ui_scale(); }

bool holdpoint_editor_open(void) { return E.open; }

static void set_open(bool on) {
    if (on == E.open) return;
    E.open = on;
    Cvar *cd = cvar_find("cam_debug");
    if (on) {
        E.prev_debug = cd ? cd->i : 0;
        if (cd && !cd->i) cvar_set_int(cd, 1);
    } else {
        if (cd) cvar_set_int(cd, E.prev_debug);
        cvar_set_int(cvar_find("holdpoint_force_state"), -1);
        cvar_set_int(cvar_find("holdpoint_static"), 0);
    }
}

void holdpoint_editor_toggle(void) { set_open(!E.open); }
static void cmd_editor(int argc, const char **argv) { (void)argc; (void)argv; holdpoint_editor_toggle(); }
void holdpoint_editor_init(void) { cmd_register("holdpoint_editor", cmd_editor, "toggle the hold point editor (F8)"); }

static bool vrow(struct nk_context *ctx, const char *label, vec3 *v, float lo, float hi, float step) {
    vec3 o = *v;
    nk_layout_row_dynamic(ctx, S(20), 4);
    nk_label(ctx, label, NK_TEXT_LEFT);
    nk_property_float(ctx, "#", lo, &v->x, hi, step, step * 0.2f);
    nk_property_float(ctx, "#", lo, &v->y, hi, step, step * 0.2f);
    nk_property_float(ctx, "#", lo, &v->z, hi, step, step * 0.2f);
    return memcmp(&o, v, sizeof(o)) != 0;
}
static bool frow(struct nk_context *ctx, const char *label, float *v, float lo, float hi, float step) {
    float o = *v;
    nk_layout_row_dynamic(ctx, S(20), 2);
    nk_label(ctx, label, NK_TEXT_LEFT);
    nk_property_float(ctx, "#", lo, v, hi, step, step * 0.2f);
    return o != *v;
}

void holdpoint_editor_draw(void) {
    if (!E.open) return;
    AnimLibrary *lib = g_engine.anim;
    if (!lib) return;
    struct nk_context *ctx = ui_nk();
    if (nk_begin(ctx, "HOLD POINT EDITOR", nk_rect(S(490), S(60), S(440), S(720)),
                 NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_MINIMIZABLE)) {
        static const char *styles[HOLD_STYLE_MAX];
        for (int i = 0; i < lib->proc.style_count; i++) styles[i] = lib->proc.styles[i].name;
        static const char *states[HP_STATE_COUNT];
        for (int i = 0; i < HP_STATE_COUNT; i++) states[i] = holdpoint_state_name(i);
        nk_layout_row_dynamic(ctx, S(24), 2);
        E.style = nk_combo(ctx, styles, lib->proc.style_count, DG_CLAMP(E.style, 0, lib->proc.style_count - 1), S(20), nk_vec2(S(200), S(260)));
        E.state = nk_combo(ctx, states, HP_STATE_COUNT, E.state, S(20), nk_vec2(S(200), S(220)));
        nk_layout_row_dynamic(ctx, S(22), 2);
        nk_bool pv = E.preview, st = E.static_pose;
        nk_checkbox_label(ctx, "preview state", &pv);
        nk_checkbox_label(ctx, "static (no sway)", &st);
        E.preview = pv;
        E.static_pose = st;
        cvar_set_int(cvar_find("holdpoint_force_state"), E.preview ? E.state : -1);
        cvar_set_int(cvar_find("holdpoint_static"), E.static_pose ? 1 : 0);
        nk_layout_row_dynamic(ctx, S(26), 3);
        if (nk_button_label(ctx, "Save")) snprintf(E.status, sizeof(E.status), holdpoint_save_all(NULL) ? "saved holdpoint.json" : "save failed");
        if (nk_button_label(ctx, "Reload")) { cmd_execute_line("holdpoint_reload"); snprintf(E.status, sizeof(E.status), "reloaded"); }
        if (nk_button_label(ctx, "Run test")) cmd_execute_line("holdpoint_test");
        if (E.status[0]) { nk_layout_row_dynamic(ctx, S(18), 1); nk_label(ctx, E.status, NK_TEXT_LEFT); }
        HoldPointStyle *s = &lib->proc.styles[E.style].hp;
        bool ch = false;
        if (nk_tree_push(ctx, NK_TREE_TAB, "State (camera space)", NK_MAXIMIZED)) {
            if (E.state == HP_ADS) {
                ch |= frow(ctx, "eye relief (m)", &s->eye_relief, 0.05f, 0.8f, 0.005f);
                ch |= vrow(ctx, "ADS cant deg", &s->ads_rot, -45, 45, 0.25f);
                nk_layout_row_dynamic(ctx, S(18), 1);
                nk_label(ctx, "ADS position comes from each item's sight socket.", NK_TEXT_LEFT);
            } else {
                HpPose *p = &s->state[E.state];
                ch |= vrow(ctx, "pos r/u/f m", &p->pos, -1, 1, 0.002f);
                ch |= vrow(ctx, "rot p/y/r deg", &p->rot, -180, 180, 0.25f);
            }
            ch |= frow(ctx, "blend in (s)", &s->blend_in[E.state], 0.01f, 2.0f, 0.01f);
            nk_tree_pop(ctx);
        }
        if (nk_tree_push(ctx, NK_TREE_TAB, "Procedural", NK_MINIMIZED)) {
            ch |= frow(ctx, "ADS scale", &s->ads_scale, 0, 1, 0.01f);
            ch |= frow(ctx, "look lag deg/(rad/s)", &s->lag_rot, 0, 10, 0.05f);
            ch |= frow(ctx, "look lag pos", &s->lag_pos, 0, 0.05f, 0.0005f);
            ch |= frow(ctx, "look lag max deg", &s->lag_max, 0, 30, 0.1f);
            ch |= frow(ctx, "lag stiffness", &s->lag_k, 1, 1000, 1);
            ch |= frow(ctx, "lag damping", &s->lag_d, 0, 100, 0.5f);
            ch |= frow(ctx, "sway deg", &s->sway_rot, 0, 5, 0.01f);
            ch |= frow(ctx, "sway freq", &s->sway_freq, 0, 5, 0.01f);
            ch |= frow(ctx, "breath m", &s->breath_pos, 0, 0.02f, 0.0002f);
            ch |= frow(ctx, "bob m", &s->bob_pos, 0, 0.05f, 0.0005f);
            ch |= frow(ctx, "bob roll deg", &s->bob_rot, 0, 10, 0.05f);
            ch |= frow(ctx, "kick scale", &s->kick_scale, 0, 3, 0.01f);
            nk_tree_pop(ctx);
        }
        if (nk_tree_push(ctx, NK_TREE_TAB, "Collision / reach / body", NK_MINIMIZED)) {
            ch |= frow(ctx, "pull back m", &s->col_back, 0, 1, 0.005f);
            ch |= frow(ctx, "pull up m", &s->col_up, 0, 1, 0.005f);
            ch |= frow(ctx, "pull pitch deg", &s->col_pitch, -90, 90, 0.5f);
            ch |= frow(ctx, "pull curve", &s->col_curve, 0.2f, 4, 0.05f);
            ch |= frow(ctx, "reach pull m", &s->reach_pull, 0, 0.6f, 0.005f);
            ch |= frow(ctx, "pitch soft deg", &s->pitch_soft, 0, 89, 0.5f);
            ch |= frow(ctx, "pitch follow", &s->pitch_follow, 0, 1, 0.01f);
            ch |= frow(ctx, "torso twist deg", &s->twist, -45, 45, 0.25f);
            ch |= vrow(ctx, "clavicle L deg", &s->clav_l, -45, 45, 0.25f);
            ch |= vrow(ctx, "clavicle R deg", &s->clav_r, -45, 45, 0.25f);
            ch |= frow(ctx, "clavicle gain", &s->clavicle_gain, 0, 200, 0.5f);
            nk_tree_pop(ctx);
        }
        if (ch) {
            proc_params_rehash(&lib->proc);
            snprintf(E.status, sizeof(E.status), "edited (not saved)");
        }
    }
    nk_end(ctx);
}
