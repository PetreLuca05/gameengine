// In-game VFX editor (vfx_editor / F7): pick an effect, tweak its emitters live, spawn it in front
// of the camera (once or repeating, optionally in slow motion) and save it back to its JSON file.
#include "vfx_editor.h"
#include "ui.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/log.h"
#include "core/platform.h"
#include "game/game.h"
#include "render/render.h"
#include "vfx/vfx.h"
#include "vfx/vfx_curve.h"
#include "physics/physics.h"
#include "world/world.h"
#include "nuklear_config.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    bool open;
    int effect, emitter;
    bool repeat;
    float interval, timer;
    vec3 spot, spot_normal;
    quat spot_rot;
    bool spot_valid;
    char filter[32];
    char status[160];
    bool dirty;
} E = { .interval = 0.6f };

static float S(float v) { return v * ui_scale(); }

bool vfx_editor_open(void) { return E.open; }

static void cmd_vfx_editor(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    E.open = !E.open;
}

void vfx_editor_init(void) { cmd_register("vfx_editor", cmd_vfx_editor, "toggle the in-game VFX editor"); }

void vfx_editor_toggle(void) { E.open = !E.open; }

static void pick_spot(void) {
    Player *p = game_local_player();
    World *w = game_world();
    if (!p || !w) return;
    vec3 eye = player_eye_pos(&p->sim.move);
    vec3 dir = dir_from_angles(p->sim.move.yaw, p->sim.move.pitch);
    E.spot = v3_madd(eye, dir, 2.5f);
    E.spot_normal = v3_neg(dir);
    RayHit rh;
    if (phys_raycast(w->phys, eye, v3_scale(dir, 12.0f), PL_MASK_WORLD, PHYS_INVALID_BODY, &rh)) {
        E.spot = rh.point;
        E.spot_normal = rh.normal;
    }
    E.spot_rot = quat_euler(p->sim.move.yaw + DG_PI * 0.5f, 0, 0);   // muzzle effects fire sideways (side view)
    E.spot_valid = true;
}

static void spawn_selected(void) {
    VfxLibrary *lib = vfx_game_library();
    if (!lib || E.effect < 0 || E.effect >= lib->effect_count) return;
    if (!E.spot_valid) pick_spot();
    VfxSpawnParams sp = vfx_spawn_params(E.spot);
    sp.normal = E.spot_normal;
    sp.incoming = v3_neg(E.spot_normal);
    sp.rot = E.spot_rot;
    sp.decal_ok = true;
    sp.material = SURF_CONCRETE;
    vfx_game_spawn_id(E.effect, &sp);
}

static void save_selected(void) {
    VfxLibrary *lib = vfx_game_library();
    if (!lib || E.effect < 0) return;
    const VfxEffectDef *fx = &lib->effects[E.effect];
    char *txt = vfx_lib_effect_to_json(lib, E.effect);
    if (!txt) return;
    char rel[512], path[600];
    // Save into the source tree when running from the build (assets are mirrored at build time).
    snprintf(rel, sizeof(rel), "assets/vfx/effects/%s.json", fx->name);
#ifdef DG_SOURCE_DIR
    snprintf(path, sizeof(path), "%s/%s", DG_SOURCE_DIR, rel);
#else
    platform_data_path(rel, path, sizeof(path));
#endif
    FILE *f = fopen(path, "wb");
    if (f) {
        fputs(txt, f);
        fputc('\n', f);
        fclose(f);
        snprintf(E.status, sizeof(E.status), "saved %s", path);
        E.dirty = false;
        // Mirror into the runtime copy so a vfx_reload keeps the edit.
        char rt[600];
        snprintf(rt, sizeof(rt), "%s%s", platform_base_dir(), rel);
        if (strcmp(rt, path)) {
            FILE *g = fopen(rt, "wb");
            if (g) { fputs(txt, g); fputc('\n', g); fclose(g); }
        }
    } else {
        snprintf(E.status, sizeof(E.status), "cannot write %s", path);
    }
    log_info("vfx_editor: %s", E.status);
    free(txt);
}

static bool range_row(struct nk_context *ctx, const char *label, VfxRange *r, float lo, float hi, float step) {
    VfxRange old = *r;
    nk_layout_row_dynamic(ctx, S(20), 3);
    nk_label(ctx, label, NK_TEXT_LEFT);
    nk_property_float(ctx, "#min", lo, &r->min, hi, step, step * 0.2f);
    nk_property_float(ctx, "#max", lo, &r->max, hi, step, step * 0.2f);
    return old.min != r->min || old.max != r->max;
}

static bool float_row(struct nk_context *ctx, const char *label, float *v, float lo, float hi, float step) {
    float old = *v;
    nk_layout_row_dynamic(ctx, S(20), 2);
    nk_label(ctx, label, NK_TEXT_LEFT);
    nk_property_float(ctx, "#", lo, v, hi, step, step * 0.2f);
    return old != *v;
}

static bool color_row(struct nk_context *ctx, const char *label, vec4 *c) {
    vec4 old = *c;
    nk_layout_row_dynamic(ctx, S(20), 5);
    nk_label(ctx, label, NK_TEXT_LEFT);
    nk_property_float(ctx, "#r", 0, &c->x, 16, 0.02f, 0.005f);
    nk_property_float(ctx, "#g", 0, &c->y, 16, 0.02f, 0.005f);
    nk_property_float(ctx, "#b", 0, &c->z, 16, 0.02f, 0.005f);
    nk_property_float(ctx, "#a", 0, &c->w, 1, 0.02f, 0.005f);
    return memcmp(&old, c, sizeof(old)) != 0;
}

static int combo(struct nk_context *ctx, const char *label, const char **items, int n, int cur) {
    nk_layout_row_dynamic(ctx, S(22), 2);
    nk_label(ctx, label, NK_TEXT_LEFT);
    return nk_combo(ctx, items, n, cur, S(20), nk_vec2(S(200), S(220)));
}

void vfx_editor_draw(void) {
    if (!E.open) return;
    VfxLibrary *lib = vfx_game_library();
    if (!lib) return;
    struct nk_context *ctx = ui_nk();
    float dt = (float)g_engine.frame_dt;
    if (E.repeat) {
        E.timer -= dt;
        if (E.timer <= 0) { spawn_selected(); E.timer = E.interval; }
    }
    if (nk_begin(ctx, "VFX EDITOR", nk_rect(S(10), S(60), S(470), S(760)),
                 NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_MINIMIZABLE)) {
        // ---- effect selection
        nk_layout_row_dynamic(ctx, S(22), 2);
        nk_label(ctx, "filter", NK_TEXT_LEFT);
        nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, E.filter, sizeof(E.filter), nk_filter_default);
        static const char *names[VFX_MAX_EFFECTS];
        static int map[VFX_MAX_EFFECTS];
        int n = 0, cur = 0;
        for (int i = 0; i < lib->effect_count; i++) {
            if (E.filter[0] && !strstr(lib->effects[i].name, E.filter)) continue;
            if (i == E.effect) cur = n;
            map[n] = i;
            names[n++] = lib->effects[i].name;
        }
        if (n > 0) {
            nk_layout_row_dynamic(ctx, S(24), 1);
            int sel = nk_combo(ctx, names, n, cur, S(20), nk_vec2(S(440), S(400)));
            if (map[sel] != E.effect) { E.effect = map[sel]; E.emitter = 0; }
        }
        if (E.effect >= lib->effect_count) E.effect = 0;
        VfxEffectDef *fx = &lib->effects[E.effect];
        bool changed = false;

        nk_layout_row_dynamic(ctx, S(26), 4);
        if (nk_button_label(ctx, "Spawn")) { pick_spot(); spawn_selected(); }
        if (nk_button_label(ctx, E.repeat ? "Stop" : "Repeat")) { E.repeat = !E.repeat; if (E.repeat) pick_spot(); E.timer = 0; }
        if (nk_button_label(ctx, E.dirty ? "Save *" : "Save")) save_selected();
        if (nk_button_label(ctx, "Reload all")) { vfx_game_reload(); E.dirty = false; snprintf(E.status, sizeof(E.status), "reloaded"); }
        Cvar *ts = cvar_find("vfx_timescale");
        nk_layout_row_dynamic(ctx, S(20), 2);
        float tsv = ts ? ts->f : 1.0f;
        nk_property_float(ctx, "#timescale", 0.0f, &tsv, 4.0f, 0.05f, 0.01f);
        if (ts && tsv != ts->f) cvar_set_float(ts, tsv);
        nk_property_float(ctx, "#repeat s", 0.05f, &E.interval, 5.0f, 0.05f, 0.01f);
        nk_layout_row_dynamic(ctx, S(18), 1);
        nk_label(ctx, fx->source, NK_TEXT_LEFT);
        if (E.status[0]) nk_label(ctx, E.status, NK_TEXT_LEFT);

        // ---- effect level
        if (nk_tree_push(ctx, NK_TREE_TAB, "Effect", NK_MAXIMIZED)) {
            changed |= float_row(ctx, "duration (s)", &fx->duration, 0, 30, 0.05f);
            float pr = (float)fx->priority, mi = (float)fx->max_instances;
            changed |= float_row(ctx, "priority 0..3", &pr, 0, 3, 1);
            changed |= float_row(ctx, "max instances", &mi, 1, 512, 1);
            fx->priority = (int)pr;
            fx->max_instances = (int)mi;
            changed |= float_row(ctx, "cull distance", &fx->cull_distance, 1, 2000, 5);
            if (fx->light.enabled) {
                changed |= float_row(ctx, "light intensity", &fx->light.intensity, 0, 200, 0.5f);
                changed |= float_row(ctx, "light radius", &fx->light.radius, 0, 50, 0.1f);
                changed |= float_row(ctx, "light duration", &fx->light.duration, 0, 2, 0.005f);
            }
            if (fx->decal.enabled) {
                changed |= range_row(ctx, "decal size", &fx->decal.size, 0, 4, 0.005f);
                changed |= float_row(ctx, "decal life", &fx->decal.lifetime, 0, 600, 1);
            }
            nk_tree_pop(ctx);
        }
        // ---- emitters
        if (fx->emitter_count > 0) {
            static const char *em_names[VFX_MAX_EMITTERS_PER_EFFECT];
            for (int k = 0; k < fx->emitter_count; k++) em_names[k] = lib->emitters[fx->emitters[k]].name;
            if (E.emitter >= fx->emitter_count) E.emitter = 0;
            E.emitter = combo(ctx, "emitter", em_names, fx->emitter_count, E.emitter);
            VfxEmitterDef *e = &lib->emitters[fx->emitters[E.emitter]];
            nk_layout_row_dynamic(ctx, S(22), 2);
            nk_bool en = e->enabled;
            nk_checkbox_label(ctx, "enabled", &en);
            if ((bool)en != e->enabled) { e->enabled = en; changed = true; }
            nk_label(ctx, e->tier == VFX_TIER_GPU ? "tier: GPU" : "tier: CPU", NK_TEXT_RIGHT);
            if (nk_tree_push(ctx, NK_TREE_TAB, "Spawn", NK_MAXIMIZED)) {
                VfxSpawnDef *s = &e->spawn;
                changed |= range_row(ctx, "count", &s->count, 0, 100000, 1);
                changed |= float_row(ctx, "rate /s", &s->rate, 0, 1e6f, 1);
                changed |= range_row(ctx, "life", &s->life, 0, 30, 0.01f);
                changed |= range_row(ctx, "size", &s->size, 0, 20, 0.002f);
                changed |= range_row(ctx, "speed", &s->speed, 0, 200, 0.05f);
                changed |= float_row(ctx, "cone angle", &s->angle, 0, 180, 0.5f);
                changed |= float_row(ctx, "radius", &s->radius, 0, 20, 0.01f);
                changed |= float_row(ctx, "inherit vel", &s->inherit, 0, 2, 0.05f);
                changed |= range_row(ctx, "spin deg/s", &s->spin, -3600, 3600, 5);
                changed |= color_row(ctx, "color", &s->color_a);
                changed |= color_row(ctx, "color 2", &s->color_b);
                static const char *dirs[] = { "socket", "normal", "reflect", "incoming", "up", "random", "side", "surface" };
                int d = combo(ctx, "direction", dirs, 8, s->dir);
                if (d != s->dir) { s->dir = (u8)d; changed = true; }
                nk_tree_pop(ctx);
            }
            if (nk_tree_push(ctx, NK_TREE_TAB, "Update", NK_MINIMIZED)) {
                VfxUpdateDef *u = &e->update;
                changed |= float_row(ctx, "gravity", &u->gravity, -5, 5, 0.01f);
                changed |= float_row(ctx, "drag", &u->drag, 0, 50, 0.05f);
                changed |= float_row(ctx, "wind", &u->wind, 0, 5, 0.05f);
                changed |= float_row(ctx, "turbulence", &u->turbulence, 0, 50, 0.05f);
                changed |= float_row(ctx, "turb freq", &u->turb_freq, 0, 20, 0.05f);
                changed |= float_row(ctx, "bounce", &u->bounce, 0, 1, 0.02f);
                changed |= float_row(ctx, "friction", &u->friction, 0, 1, 0.02f);
                changed |= float_row(ctx, "flipbook fps", &u->fps, 0, 120, 1);
                // Curve end values (keys keep their times; full curve editing lives in the JSON).
                if (u->size_curve.key_count >= 2) {
                    float *a = &u->size_curve.v[0].x, *b = &u->size_curve.v[u->size_curve.key_count - 1].x;
                    bool c1 = float_row(ctx, "size start", a, 0, 20, 0.02f), c2 = float_row(ctx, "size end", b, 0, 20, 0.02f);
                    if (c1 || c2) { if (u->size_curve.type == VFX_CURVE_BEZIER) vfx_curve_auto_tangents(&u->size_curve); changed = true; }
                }
                static const char *col[] = { "none", "depth", "physics" };
                int c = combo(ctx, "collision", col, 3, u->collide);
                if (c != u->collide) { u->collide = (u8)c; changed = true; }
                nk_tree_pop(ctx);
            }
            if (nk_tree_push(ctx, NK_TREE_TAB, "Render", NK_MINIMIZED)) {
                VfxRenderDef *r = &e->render;
                static const char *modes[] = { "add", "alpha", "distort", "mesh", "ribbon", "none" };
                static const char *orients[] = { "billboard", "stretch", "cross", "axis", "normal", "segment" };
                static const char *lights[] = { "none", "simple", "sixway" };
                int m = combo(ctx, "mode", modes, 6, r->mode);
                int o = combo(ctx, "orient", orients, 6, r->orient);
                int l = combo(ctx, "lighting", lights, 3, r->lighting);
                if (m != r->mode || o != r->orient || l != r->lighting) { r->mode = (u8)m; r->orient = (u8)o; r->lighting = (u8)l; changed = true; }
                static const char *tex[VFX_MAX_TEXTURES + 1];
                tex[0] = "(soft sprite)";
                for (int i = 0; i < lib->texture_count; i++) tex[i + 1] = lib->textures[i].name;
                int t = combo(ctx, "texture", tex, lib->texture_count + 1, r->texture_id + 1);
                if (t - 1 != r->texture_id) {
                    r->texture_id = t - 1;
                    dg_strcpy(r->texture, sizeof(r->texture), t > 0 ? lib->textures[t - 1].name : "");
                    changed = true;
                }
                changed |= float_row(ctx, "emissive", &r->emissive, 0, 100, 0.1f);
                changed |= float_row(ctx, "soft (m)", &r->soft, 0, 5, 0.01f);
                changed |= float_row(ctx, "near fade (m)", &r->near_fade, 0, 5, 0.01f);
                changed |= float_row(ctx, "stretch", &r->stretch, 0, 1, 0.001f);
                changed |= float_row(ctx, "stretch max", &r->stretch_max, 1, 50, 0.1f);
                changed |= float_row(ctx, "aspect", &r->aspect, 0.05f, 20, 0.02f);
                changed |= float_row(ctx, "distortion", &r->distortion, 0, 0.2f, 0.001f);
                nk_bool ex = r->exposure_aware;
                nk_layout_row_dynamic(ctx, S(22), 1);
                nk_checkbox_label(ctx, "exposure aware (fades in daylight)", &ex);
                if ((bool)ex != r->exposure_aware) { r->exposure_aware = ex; changed = true; }
                nk_tree_pop(ctx);
            }
        }
        if (changed) {
            E.dirty = true;
            render_vfx_update_emitters(lib);
        }
    }
    nk_end(ctx);
}
