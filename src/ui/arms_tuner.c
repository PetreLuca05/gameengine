// Arms layer tuner (arms_tuner, docs/CAMERA_HOLDPOINT.md "The arms layer"): pick a hold point state
// and the held item (or every item), edit the layer's offset / rotation / scale / FOV and the blend
// live, the equip raise and the shoulder seam; preview a state, draw the layer in wireframe, hide
// the body or the arms to check that nothing is drawn twice; save assets/player/arms_layer.json.
// No bone moves: the layer only re-projects the arms + item (src/render/arms_layer.c).
#include "arms_tuner.h"
#include "ui.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/log.h"
#include "render/arms_layer.h"
#include "game/game_internal.h"
#include "nuklear_config.h"
#include <stdio.h>
#include <string.h>

static struct {
    bool open;
    int state;
    bool per_item;
    char status[160];
} T;

static float S(float v) { return v * ui_scale(); }

bool arms_tuner_open(void) { return T.open; }
static void cmd_tuner(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    // A dev tool: it writes locked cvars and the shared arms layer data (never under the release locks).
    if (cvar_release_locks()) { log_info("arms_tuner: dev builds only"); return; }
    T.open = !T.open;
}
void arms_tuner_init(void) {
    cmd_register("arms_tuner", cmd_tuner, "toggle the first-person arms layer tuner (offset / rotation / scale / FOV per hold state and item, equip raise, seam; wireframe, layer toggles, save)");
}

static bool frow(struct nk_context *ctx, const char *label, float *v, float lo, float hi, float step) {
    float o = *v;
    nk_layout_row_dynamic(ctx, S(20), 2);
    nk_label(ctx, label, NK_TEXT_LEFT);
    nk_property_float(ctx, "#", lo, v, hi, step, step * 0.2f);
    return o != *v;
}

static void toggle_cvar(struct nk_context *ctx, const char *label, const char *name, int on_value) {
    Cvar *c = cvar_find(name);
    if (!c) return;
    nk_bool v = c->i == on_value;
    if (nk_checkbox_label(ctx, label, &v)) {
        CvarSource src = cvar_source_push(CVAR_SRC_HOST);   // dev tools may set locked cvars
        cvar_set_int(c, v ? on_value : (on_value ? 0 : 1));
        cvar_source_pop(src);
    }
}

void arms_tuner_draw(void) {
    if (!T.open || cvar_release_locks()) return;
    ArmsLayerDef *d = game_arms_def();
    ArmsLayerIn in;
    ArmsLayerOut out;
    bool live = game_arms_last(&in, &out);
    struct nk_context *ctx = ui_nk();
    if (nk_begin(ctx, "ARMS LAYER TUNER", nk_rect(S(20), S(60), S(420), S(760)),
                 NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_MINIMIZABLE)) {
        // What is held and shown now.
        nk_layout_row_dynamic(ctx, S(18), 1);
        char info[200];
        if (live)
            snprintf(info, sizeof(info), "now: %s, item %s, ADS %.2f, raise %.2f, arms FOV %.1f (world %.1f)", holdpoint_state_name(in.hp_state),
                     in.item_id ? in.item_id : "-", in.ads_w, out.raise, in.fov_hands, in.fov_world);
        else
            snprintf(info, sizeof(info), "no arms layer this frame (first person, a held item)");
        nk_label(ctx, info, NK_TEXT_LEFT);
        // The state edited (and previewed).
        static const char *states[HP_STATE_COUNT];
        for (int s = 0; s < HP_STATE_COUNT; s++) states[s] = holdpoint_state_name(s);
        nk_layout_row_dynamic(ctx, S(24), 2);
        T.state = nk_combo(ctx, states, HP_STATE_COUNT, T.state, S(20), nk_vec2(S(200), S(260)));
        if (nk_button_label(ctx, "Preview state")) {
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "holdpoint_force_state %d\n", T.state);
            cbuf_add(cmd);
        }
        nk_layout_row_dynamic(ctx, S(24), 2);
        if (nk_button_label(ctx, "Auto state")) cbuf_add("holdpoint_force_state -1\n");
        nk_bool pi = T.per_item;
        if (nk_checkbox_label(ctx, live && in.item_id ? "this item only" : "this item only (none held)", &pi)) T.per_item = pi;
        const char *item = T.per_item && live && in.item_id ? in.item_id : NULL;
        // Edit a copy: the item override is only created once a value actually changes.
        ArmsPose cur = arms_layer_target(d, T.state, item), *p = &cur;
        bool ads = T.state == HP_ADS;
        bool changed = false;
        if (nk_tree_push(ctx, NK_TREE_TAB, ads ? "Layer (ADS: push, roll, scale about the sight)" : "Layer (about the hold point)", NK_MAXIMIZED)) {
            if (!ads) {
                changed |= frow(ctx, "offset right (m)", &p->offset.x, -0.3f, 0.3f, 0.001f);
                changed |= frow(ctx, "offset up (m)", &p->offset.y, -0.3f, 0.3f, 0.001f);
            }
            changed |= frow(ctx, ads ? "push forward (m)" : "offset forward (m)", &p->offset.z, -0.3f, 0.3f, 0.001f);
            if (!ads) {
                changed |= frow(ctx, "pitch (deg)", &p->rotation.x, -45, 45, 0.1f);
                changed |= frow(ctx, "yaw (deg)", &p->rotation.y, -45, 45, 0.1f);
            }
            changed |= frow(ctx, "roll (deg)", &p->rotation.z, -45, 45, 0.1f);
            changed |= frow(ctx, "scale", &p->scale, 0.9f, 1.1f, 0.001f);
            if (!ads) changed |= frow(ctx, "FOV add (deg)", &p->fov_add, -20, 20, 0.1f);
            if (!ads) {
                // The arms camera's own position (the hip framing: the camera moves, never turns; not the arms).
                changed |= frow(ctx, "camera right (m)", &p->cam_pos.x, -0.8f, 0.8f, 0.001f);
                changed |= frow(ctx, "camera up (m)", &p->cam_pos.y, -0.8f, 0.8f, 0.001f);
                changed |= frow(ctx, "camera forward (m)", &p->cam_pos.z, -0.8f, 0.8f, 0.001f);
                changed |= frow(ctx, "camera pitch up (deg)", &p->cam_rot.x, -15.0f, 15.0f, 0.1f);
                changed |= frow(ctx, "camera yaw right (deg)", &p->cam_rot.y, -15.0f, 15.0f, 0.1f);
            }
            frow(ctx, "blend into (s)", &d->blend[T.state], 0, 1, 0.01f);
            nk_layout_row_dynamic(ctx, S(24), 1);
            if (nk_button_label(ctx, "Reset this state")) {
                p->offset = p->rotation = p->cam_pos = p->cam_rot = v3(0, 0, 0);
                p->scale = 1.0f;
                p->fov_add = 0.0f;
                changed = true;
            }
            if (changed) {
                ArmsPose *dst = arms_layer_edit(d, T.state, item);
                if (item && dst == &d->state[T.state]) snprintf(T.status, sizeof(T.status), "item table full: edited the state default");
                *dst = cur;
            }
            nk_tree_pop(ctx);
        }
        if (nk_tree_push(ctx, NK_TREE_TAB, "Equip raise", NK_MINIMIZED)) {
            frow(ctx, "time (s)", &d->equip_time, 0, 1, 0.01f);
            frow(ctx, "start pitch (deg)", &d->equip_pitch, 0, 90, 0.5f);
            frow(ctx, "margin below the view (m)", &d->equip_margin, 0, 0.3f, 0.005f);
            nk_tree_pop(ctx);
        }
        if (nk_tree_push(ctx, NK_TREE_TAB, "Shoulder seam / near plane", NK_MINIMIZED)) {
            nk_layout_row_dynamic(ctx, S(20), 1);
            nk_bool sm = d->seam;
            if (nk_checkbox_label(ctx, "shoulder seam (off: the arms camera draws only the arms + item)", &sm)) d->seam = sm;
            frow(ctx, "world camera within (m)", &d->seam_r0, 0, 0.3f, 0.005f);
            frow(ctx, "arms camera past (m)", &d->seam_r1, 0.01f, 0.5f, 0.005f);
            frow(ctx, "near plane (m)", &d->znear, 0.002f, 0.05f, 0.001f);
            nk_tree_pop(ctx);
        }
        if (nk_tree_push(ctx, NK_TREE_TAB, "Weapon Field of View presets (h16)", NK_MINIMIZED)) {
            frow(ctx, "Narrow", &d->weapon_fov[WEAPON_FOV_NARROW], 30, 130, 0.5f);
            frow(ctx, "Default", &d->weapon_fov[WEAPON_FOV_DEFAULT], 30, 130, 0.5f);
            frow(ctx, "Wide", &d->weapon_fov[WEAPON_FOV_WIDE], 30, 130, 0.5f);
            nk_tree_pop(ctx);
        }
        fp_framing_tuner(ctx);
        if (nk_tree_push(ctx, NK_TREE_TAB, "View", NK_MAXIMIZED)) {
            nk_layout_row_dynamic(ctx, S(20), 1);
            toggle_cvar(ctx, "arms layer wireframe", "r_arms_wireframe", 1);
            Cvar *fl = cvar_find("cl_fp_layers");
            if (fl) {
                nk_bool body = (fl->i & 1) != 0, arms = (fl->i & 2) != 0;
                nk_layout_row_dynamic(ctx, S(20), 2);
                bool ch = nk_checkbox_label(ctx, "body", &body);
                ch |= nk_checkbox_label(ctx, "arms + item", &arms);
                if (ch) {
                    CvarSource src = cvar_source_push(CVAR_SRC_HOST);
                    cvar_set_int(fl, (body ? 1 : 0) | (arms ? 2 : 0));
                    cvar_source_pop(src);
                }
            }
            nk_layout_row_dynamic(ctx, S(20), 1);
            toggle_cvar(ctx, "arms layer on (off: the world camera, no ADS presentation)", "arms_layer", 1);
            nk_tree_pop(ctx);
        }
        nk_layout_row_dynamic(ctx, S(24), 2);
        if (nk_button_label(ctx, "Save")) snprintf(T.status, sizeof(T.status), game_arms_save(NULL) ? "saved assets/player/arms_layer.json" : "save failed");
        if (nk_button_label(ctx, "Reload")) snprintf(T.status, sizeof(T.status), game_arms_load() ? "reloaded" : "reload failed (defaults)");
        if (T.status[0]) {
            nk_layout_row_dynamic(ctx, S(18), 1);
            nk_label(ctx, T.status, NK_TEXT_LEFT);
        }
    }
    nk_end(ctx);
}
