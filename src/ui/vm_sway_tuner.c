// Viewmodel sway tuner (viewmodel_sway_tuner, docs/FIRE_ANIM.md "Viewmodel sway"): every value of the
// held weapon's resolved config, edited live (the next frame uses it), each layer on / off, this
// frame's layers read out, and a save into assets/weapons/viewmodel_sway.json at one level: the
// defaults, the weapon's class or the weapon itself (only what differs from the level above).
#include "vm_sway_tuner.h"
#include "ui.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/log.h"
#include "game/game_internal.h"
#include "player/weapon_sway.h"
#include "nuklear_config.h"
#include <stdio.h>
#include <string.h>

static struct {
    bool open;
    char status[200];
} T;

static float S(float v) { return v * ui_scale(); }

bool vm_sway_tuner_open(void) { return T.open; }

static void cmd_tuner(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    T.open = !T.open;
}

void vm_sway_tuner_init(void) {
    cmd_register("viewmodel_sway_tuner", cmd_tuner, "toggle the viewmodel sway tuner (every value live, layers on / off, save per default / class / weapon)");
}

static const char *k_section_title(const char *sec) {
    if (!sec) return "Composition";
    if (!strcmp(sec, "look")) return "1 Look sway";
    if (!strcmp(sec, "move")) return "2 Movement sway";
    if (!strcmp(sec, "bob")) return "3 Walk / sprint bob";
    if (!strcmp(sec, "sprint_pose")) return "3 Sprint pose";
    if (!strcmp(sec, "air")) return "4 Jump / fall / land / stance";
    if (!strcmp(sec, "camera")) return "4 Camera landing dip";
    if (!strcmp(sec, "idle")) return "5 Idle / breathing";
    if (!strcmp(sec, "ads_transition")) return "7 ADS transition";
    if (!strcmp(sec, "ads")) return "ADS multipliers [rotation, position]";
    return sec;
}

static void field_row(struct nk_context *ctx, WeaponSwayConfig *c, const WeaponSwayField *f) {
    char *base = (char *)c;
    float inc = f->step * 0.2f;
    switch (f->type) {
    case WSF_FLOAT:
        nk_layout_row_dynamic(ctx, S(20), 2);
        nk_label(ctx, f->key, NK_TEXT_LEFT);
        nk_property_float(ctx, "#", f->lo, (float *)(base + f->off), f->hi, f->step, inc);
        break;
    case WSF_VEC3: {
        vec3 *v = (vec3 *)(base + f->off);
        nk_layout_row_dynamic(ctx, S(20), 1);
        nk_label(ctx, f->key, NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, S(20), 3);
        nk_property_float(ctx, "#x", f->lo, &v->x, f->hi, f->step, inc);
        nk_property_float(ctx, "#y", f->lo, &v->y, f->hi, f->step, inc);
        nk_property_float(ctx, "#z", f->lo, &v->z, f->hi, f->step, inc);
        break;
    }
    case WSF_ADS:
        nk_layout_row_dynamic(ctx, S(20), 3);
        nk_label(ctx, f->key, NK_TEXT_LEFT);
        nk_property_float(ctx, "#rot", f->lo, &c->ads_rot[f->off], f->hi, f->step, inc);
        nk_property_float(ctx, "#pos", f->lo, &c->ads_pos[f->off], f->hi, f->step, inc);
        break;
    case WSF_BOOL: {
        bool *b = (bool *)(base + f->off);
        nk_layout_row_dynamic(ctx, S(20), 1);
        nk_bool v = *b;
        nk_checkbox_label(ctx, f->key, &v);
        *b = v != 0;
        break;
    }
    default: break;   // WSF_LAYER: the toggles at the top
    }
}

void vm_sway_tuner_draw(void) {
    if (!T.open) return;
    struct nk_context *ctx = ui_nk();
    WeaponSwayConfig *c = game_vms_config();
    const SwayOutput *o = game_vms_output();
    if (!ctx || !c || !o) return;
    float w = S(520);
    if (nk_begin(ctx, "VIEWMODEL SWAY", nk_rect(S(8), S(8), w, ui_height() - S(16)), NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE | NK_WINDOW_CLOSABLE)) {
        const char *item = game_vms_item(), *cls = game_vms_class();
        Cvar *on = cvar_find("cl_viewmodel_sway"), *us = cvar_find("cl_viewmodel_sway_scale");
        nk_layout_row_dynamic(ctx, S(18), 1);
        nk_labelf(ctx, NK_TEXT_LEFT, "held: %s  class: %s  %s", item ? item : "-", cls ? cls : "-", on && on->i ? "" : "(cl_viewmodel_sway 0: off)");
        if (us) {
            nk_layout_row_dynamic(ctx, S(20), 2);
            nk_label(ctx, "Weapon motion (player)", NK_TEXT_LEFT);
            float v = us->f;
            nk_property_float(ctx, "#", 0, &v, 1, 0.05f, 0.01f);
            if (v != us->f) cvar_set_float(us, v);
        }
        // Layers: on / off and this frame's output (deg / mm).
        nk_layout_row_dynamic(ctx, S(18), 1);
        nk_labelf(ctx, NK_TEXT_LEFT, "total rot %.2f %.2f %.2f deg  pos %.1f %.1f %.1f mm%s", o->rot_deg.x, o->rot_deg.y, o->rot_deg.z, o->pos_offset.x * 1000,
                  o->pos_offset.y * 1000, o->pos_offset.z * 1000, o->clamped ? "  CLAMPED" : "");
        for (int l = 0; l < WSW_LAYERS; l++) {
            nk_layout_row_template_begin(ctx, S(18));
            nk_layout_row_template_push_static(ctx, S(70));
            nk_layout_row_template_push_dynamic(ctx);
            nk_layout_row_template_end(ctx);
            nk_bool v = c->layer_on[l];
            nk_checkbox_label(ctx, weapon_sway_layer_name(l), &v);
            c->layer_on[l] = v != 0;
            nk_labelf(ctx, NK_TEXT_LEFT, "rot %5.2f %5.2f %5.2f  pos %5.1f %5.1f %5.1f", o->layer_rot[l].x, o->layer_rot[l].y, o->layer_rot[l].z, o->layer_pos[l].x * 1000,
                      o->layer_pos[l].y * 1000, o->layer_pos[l].z * 1000);
        }
        nk_layout_row_dynamic(ctx, S(18), 1);
        nk_labelf(ctx, NK_TEXT_LEFT, "sprint %.2f  idle %.2f  ads %.2f  bob phase %.2f  camera dip %.2f deg", o->sprint_w, o->idle_w, o->ads,
                  weapon_sway_bob_phase(game_vms_state()), weapon_sway_camera_offset(game_vms_state()).x);
        // Save at a level / reload the file.
        nk_layout_row_dynamic(ctx, S(22), 4);
        if (nk_button_label(ctx, "save default")) game_vms_save(0, T.status, sizeof(T.status));
        if (nk_button_label(ctx, cls ? "save class" : "(no class)") && cls) game_vms_save(1, T.status, sizeof(T.status));
        if (nk_button_label(ctx, item ? "save weapon" : "(no item)") && item) game_vms_save(2, T.status, sizeof(T.status));
        if (nk_button_label(ctx, "reload file")) {
            cbuf_add("viewmodel_sway_reload\n");
            snprintf(T.status, sizeof(T.status), "reloaded (edits not saved are gone)");
        }
        if (T.status[0]) {
            nk_layout_row_dynamic(ctx, S(18), 1);
            nk_label_wrap(ctx, T.status);
        }
        // Every value, by section (the file's layout).
        int n = 0;
        const WeaponSwayField *fs = weapon_sway_fields(&n);
        for (int i = 0; i < n;) {
            const char *sec = fs[i].section;
            int j = i;
            while (j < n && ((!sec && !fs[j].section) || (sec && fs[j].section && !strcmp(sec, fs[j].section)))) j++;
            if (!(sec && !strcmp(sec, "layers")) && nk_tree_push_id(ctx, NK_TREE_TAB, k_section_title(sec), NK_MINIMIZED, i)) {
                for (int k = i; k < j; k++) field_row(ctx, c, &fs[k]);
                nk_tree_pop(ctx);
            }
            i = j;
        }
    } else {
        T.open = false;
    }
    nk_end(ctx);
}
