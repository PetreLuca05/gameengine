#include "vfx_library.h"
#include "vfx_curve.h"
#include "core/log.h"
#include "core/platform.h"
#include "cJSON.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ------------------------------------------------------------------ names

static const char *k_surface_names[SURF_COUNT] = {
    "default", "concrete", "brick", "plaster", "metal_thin", "metal_thick", "wood", "dirt",
    "sand", "gravel", "grass", "glass", "water", "flesh", "cloth", "tile", "asphalt",
};
const char *surface_name(int m) { return (m >= 0 && m < SURF_COUNT) ? k_surface_names[m] : "default"; }
int surface_from_name(const char *s) {
    if (!s) return -1;
    for (int i = 0; i < SURF_COUNT; i++) if (!strcmp(s, k_surface_names[i])) return i;
    return -1;
}

static const char *k_muzzle_types[VFX_MUZZLE_TYPE_COUNT] = { "pistol", "smg", "rifle", "lmg", "shotgun", "sniper" };
static const char *k_muzzle_devices[VFX_DEV_COUNT] = { "bare", "hider", "comp", "suppressor" };
static const char *k_calibers[VFX_CAL_COUNT] = { "pistol", "smg", "rifle", "shotgun", "sniper", "melee" };
const char *vfx_muzzle_type_name(int t) { return (t >= 0 && t < VFX_MUZZLE_TYPE_COUNT) ? k_muzzle_types[t] : "rifle"; }
const char *vfx_muzzle_device_name(int d) { return (d >= 0 && d < VFX_DEV_COUNT) ? k_muzzle_devices[d] : "bare"; }
const char *vfx_caliber_name(int c) { return (c >= 0 && c < VFX_CAL_COUNT) ? k_calibers[c] : "rifle"; }
static int name_index(const char *const *names, int n, const char *s) {
    if (!s) return -1;
    for (int i = 0; i < n; i++) if (!strcmp(names[i], s)) return i;
    return -1;
}
int vfx_muzzle_type_from_name(const char *s) { return name_index(k_muzzle_types, VFX_MUZZLE_TYPE_COUNT, s); }
int vfx_muzzle_device_from_name(const char *s) {
    if (s && (!strcmp(s, "flash_hider") || !strcmp(s, "hider"))) return VFX_DEV_HIDER;
    if (s && (!strcmp(s, "compensator") || !strcmp(s, "comp"))) return VFX_DEV_COMP;
    return name_index(k_muzzle_devices, VFX_DEV_COUNT, s);
}
int vfx_caliber_from_name(const char *s) { return name_index(k_calibers, VFX_CAL_COUNT, s); }

VfxCaliberScale vfx_caliber_scale(int c) {
    switch (c) {
    case VFX_CAL_PISTOL: return (VfxCaliberScale){ 0.65f, 0.8f, 0.75f, 0.85f };
    case VFX_CAL_SMG: return (VfxCaliberScale){ 0.7f, 0.85f, 0.8f, 0.85f };
    case VFX_CAL_SHOTGUN: return (VfxCaliberScale){ 1.6f, 1.25f, 0.8f, 0.8f };   // one merged effect per pellet cluster
    case VFX_CAL_SNIPER: return (VfxCaliberScale){ 1.7f, 1.45f, 1.5f, 1.3f };
    case VFX_CAL_MELEE: return (VfxCaliberScale){ 0.45f, 0.9f, 0.35f, 0.0f };
    default: return (VfxCaliberScale){ 1.0f, 1.0f, 1.0f, 1.0f };
    }
}

// ------------------------------------------------------------------ json helpers

static const cJSON *jget(const cJSON *o, const char *k) { return o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL; }
static float jf(const cJSON *o, const char *k, float def) {
    const cJSON *v = jget(o, k);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}
static bool jb(const cJSON *o, const char *k, bool def) {
    const cJSON *v = jget(o, k);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : def;
}
static const char *js(const cJSON *o, const char *k, const char *def) {
    const cJSON *v = jget(o, k);
    return cJSON_IsString(v) ? v->valuestring : def;
}
static vec3 jv3(const cJSON *o, const char *k, vec3 def) {
    const cJSON *v = jget(o, k);
    if (!cJSON_IsArray(v) || cJSON_GetArraySize(v) < 3) return def;
    return v3((float)cJSON_GetArrayItem(v, 0)->valuedouble, (float)cJSON_GetArrayItem(v, 1)->valuedouble, (float)cJSON_GetArrayItem(v, 2)->valuedouble);
}
static vec4 jcolor(const cJSON *o, const char *k, vec4 def) {
    const cJSON *v = jget(o, k);
    if (!cJSON_IsArray(v) || cJSON_GetArraySize(v) < 3) return def;
    float a = cJSON_GetArraySize(v) > 3 ? (float)cJSON_GetArrayItem(v, 3)->valuedouble : 1.0f;
    return v4((float)cJSON_GetArrayItem(v, 0)->valuedouble, (float)cJSON_GetArrayItem(v, 1)->valuedouble, (float)cJSON_GetArrayItem(v, 2)->valuedouble, a);
}
// Range: number (min = max) or [min, max].
static VfxRange jrange(const cJSON *o, const char *k, VfxRange def) {
    const cJSON *v = jget(o, k);
    if (cJSON_IsNumber(v)) return (VfxRange){ (float)v->valuedouble, (float)v->valuedouble };
    if (cJSON_IsArray(v) && cJSON_GetArraySize(v) >= 2)
        return (VfxRange){ (float)cJSON_GetArrayItem(v, 0)->valuedouble, (float)cJSON_GetArrayItem(v, 1)->valuedouble };
    return def;
}
static int jenum(const cJSON *o, const char *k, const char *const *names, int n, int def) {
    const char *s = js(o, k, NULL);
    if (!s) return def;
    for (int i = 0; i < n; i++) if (!strcmp(s, names[i])) return i;
    log_warn("vfx: unknown %s '%s'", k, s);
    return def;
}

// Curve: number (constant), [[t, v...], ...] (linear) or { "type": "bezier", "keys": [...] }.
// Keys may carry tangents as extra values when channels are given: [t, v0..vc-1, in0.., out0..].
static void jcurve(const cJSON *o, const char *k, VfxCurve *c, vec4 def, int channels) {
    vfx_curve_const(c, def, channels);
    const cJSON *v = jget(o, k);
    if (!v) return;
    if (cJSON_IsNumber(v)) { vfx_curve_const(c, v4((float)v->valuedouble, (float)v->valuedouble, (float)v->valuedouble, (float)v->valuedouble), channels); return; }
    const cJSON *keys = v;
    int type = VFX_CURVE_LINEAR;
    if (cJSON_IsObject(v)) {
        keys = jget(v, "keys");
        if (!strcmp(js(v, "type", "linear"), "bezier")) type = VFX_CURVE_BEZIER;
    }
    if (!cJSON_IsArray(keys)) return;
    c->type = (u8)type;
    c->key_count = 0;
    bool explicit_tan = false;
    const cJSON *key;
    cJSON_ArrayForEach(key, keys) {
        if (!cJSON_IsArray(key) || c->key_count >= VFX_CURVE_KEYS) continue;
        int n = cJSON_GetArraySize(key);
        int i = c->key_count++;
        c->t[i] = n > 0 ? (float)cJSON_GetArrayItem(key, 0)->valuedouble : 0;
        float vals[4] = { def.x, def.y, def.z, def.w };
        for (int ch = 0; ch < channels && ch + 1 < n; ch++) vals[ch] = (float)cJSON_GetArrayItem(key, ch + 1)->valuedouble;
        // Scalar curves on multi-channel defaults broadcast (alpha curves keep rgb).
        if (channels == 1) vals[1] = vals[2] = vals[3] = vals[0];
        c->v[i] = v4(vals[0], vals[1], vals[2], vals[3]);
        if (type == VFX_CURVE_BEZIER && n >= 1 + channels * 3) {
            float ti[4] = { 0 }, to[4] = { 0 };
            for (int ch = 0; ch < channels; ch++) {
                ti[ch] = (float)cJSON_GetArrayItem(key, 1 + channels + ch)->valuedouble;
                to[ch] = (float)cJSON_GetArrayItem(key, 1 + channels * 2 + ch)->valuedouble;
            }
            c->tan_in[i] = v4(ti[0], ti[1], ti[2], ti[3]);
            c->tan_out[i] = v4(to[0], to[1], to[2], to[3]);
            explicit_tan = true;
        }
    }
    if (c->key_count == 0) vfx_curve_const(c, def, channels);
    if (type == VFX_CURVE_BEZIER && !explicit_tan) vfx_curve_auto_tangents(c);
}

static const char *k_tiers[] = { "gpu", "cpu" };
static const char *k_spawn_modes[] = { "burst", "rate", "distance" };
static const char *k_shapes[] = { "point", "sphere", "box", "cone", "disc", "surface" };
static const char *k_dirs[] = { "socket", "normal", "reflect", "incoming", "up", "random", "side", "surface" };
static const char *k_render_modes[] = { "add", "alpha", "distort", "mesh", "ribbon", "none" };
static const char *k_orients[] = { "billboard", "stretch", "cross", "axis", "normal", "segment" };
static const char *k_lighting[] = { "none", "simple", "sixway" };
static const char *k_collide[] = { "none", "depth", "physics" };
static const char *k_sub_on[] = { "none", "death", "collision" };

static void parse_emitter(VfxEmitterDef *e, const cJSON *j) {
    memset(e, 0, sizeof(*e));
    dg_strcpy(e->name, sizeof(e->name), js(j, "name", "emitter"));
    e->tier = (u8)jenum(j, "tier", k_tiers, 2, VFX_TIER_GPU);
    e->deterministic = jb(j, "deterministic", false);
    if (e->deterministic) e->tier = VFX_TIER_CPU;   // deterministic emitters run on the CPU tier (exact on every peer)
    e->enabled = jb(j, "enabled", true);
    e->lod_cull = jf(j, "lod_cull", 0);
    const cJSON *sp = jget(j, "spawn");
    VfxSpawnDef *s = &e->spawn;
    s->mode = (u8)jenum(sp, "mode", k_spawn_modes, 3, VFX_SPAWN_BURST);
    s->count = jrange(sp, "count", (VfxRange){ 1, 1 });
    s->rate = jf(sp, "rate", 0);
    s->per_meter = jf(sp, "per_meter", 0);
    s->delay = jf(sp, "delay", 0);
    s->duration = jf(sp, "duration", 0);
    s->bursts = (int)jf(sp, "bursts", 1);
    if (s->bursts < 1) s->bursts = 1;
    s->burst_interval = jf(sp, "burst_interval", 0.05f);
    s->shape = (u8)jenum(sp, "shape", k_shapes, 6, VFX_SHAPE_POINT);
    s->radius = jf(sp, "radius", 0);
    s->box = jv3(sp, "box", v3(0, 0, 0));
    s->angle = jf(sp, "angle", 0);
    s->dir = (u8)jenum(sp, "dir", k_dirs, 8, VFX_DIR_SOCKET);
    s->dir_bias = jf(sp, "dir_bias", 0.5f);
    s->speed = jrange(sp, "speed", (VfxRange){ 0, 0 });
    s->inherit = jf(sp, "inherit", 0);
    s->life = jrange(sp, "life", (VfxRange){ 1, 1 });
    s->size = jrange(sp, "size", (VfxRange){ 0.1f, 0.1f });
    s->rotation = jrange(sp, "rotation", (VfxRange){ 0, 360 });
    s->spin = jrange(sp, "spin", (VfxRange){ 0, 0 });
    s->color_a = jcolor(sp, "color", v4(1, 1, 1, 1));
    s->color_b = jcolor(sp, "color2", s->color_a);
    s->offset = jf(sp, "offset", 0);
    s->local = jb(sp, "local", false);
    s->frame_random = jb(sp, "frame_random", false);
    s->variant_no_repeat = jb(sp, "variant_no_repeat", false);

    const cJSON *up = jget(j, "update");
    VfxUpdateDef *u = &e->update;
    u->gravity = jf(up, "gravity", 0);
    u->drag = jf(up, "drag", 0);
    u->wind = jf(up, "wind", 0);
    const cJSON *tb = jget(up, "turbulence");
    u->turbulence = cJSON_IsNumber(tb) ? (float)tb->valuedouble : jf(tb, "strength", 0);
    u->turb_freq = jf(tb, "freq", 1.0f);
    u->turb_speed = jf(tb, "speed", 0.5f);
    jcurve(up, "size_curve", &u->size_curve, v4(1, 1, 1, 1), 1);
    jcurve(up, "color_curve", &u->color_curve, v4(1, 1, 1, 1), 4);
    // "alpha_curve": scalar curve applied to alpha only (rgb stays 1) - merged into color_curve.
    const cJSON *ac = jget(up, "alpha_curve");
    if (ac && !jget(up, "color_curve")) {
        VfxCurve a;
        jcurve(up, "alpha_curve", &a, v4(1, 1, 1, 1), 1);
        u->color_curve = a;
        u->color_curve.channels = 4;
        for (int i = 0; i < a.key_count; i++) {
            u->color_curve.v[i] = v4(1, 1, 1, a.v[i].x);
            u->color_curve.tan_in[i] = v4(0, 0, 0, a.tan_in[i].x);
            u->color_curve.tan_out[i] = v4(0, 0, 0, a.tan_out[i].x);
        }
    }
    jcurve(up, "velocity_curve", &u->velocity_curve, v4(1, 1, 1, 1), 1);
    jcurve(up, "spin_curve", &u->spin_curve, v4(1, 1, 1, 1), 1);
    u->frame_over_life = jb(up, "frame_over_life", false);
    u->fps = jf(up, "fps", 0);
    u->motion_vectors = jb(up, "motion_vectors", false);
    u->collide = (u8)jenum(up, "collide", k_collide, 3, VFX_COLLIDE_NONE);
    u->bounce = jf(up, "bounce", 0.3f);
    u->friction = jf(up, "friction", 0.4f);
    u->kill_on_hit = jb(up, "kill_on_hit", false);
    const cJSON *at = jget(up, "attractor");
    u->attractor = jv3(at, "pos", v3(0, 0, 0));
    u->attract = jf(at, "strength", 0);
    u->vortex = jf(up, "vortex", 0);
    const cJSON *sub = jget(up, "sub_emitter");
    u->sub_on = (u8)jenum(sub, "on", k_sub_on, 3, VFX_SUB_NONE);
    dg_strcpy(u->sub_effect, sizeof(u->sub_effect), js(sub, "effect", ""));
    u->sub_count = (int)jf(sub, "count", 1);
    u->sub_effect_id = -1;

    const cJSON *rd = jget(j, "render");
    VfxRenderDef *r = &e->render;
    r->mode = (u8)jenum(rd, "mode", k_render_modes, 6, VFX_RENDER_ALPHA);
    r->orient = (u8)jenum(rd, "orient", k_orients, 6, VFX_ORIENT_BILLBOARD);
    r->lighting = (u8)jenum(rd, "lighting", k_lighting, 3, VFX_LIGHT_NONE);
    dg_strcpy(r->texture, sizeof(r->texture), js(rd, "texture", ""));
    dg_strcpy(r->normal_tex, sizeof(r->normal_tex), js(rd, "normal", ""));
    dg_strcpy(r->mesh, sizeof(r->mesh), js(rd, "mesh", ""));
    r->texture_id = r->normal_tex_id = -1;
    r->stretch = jf(rd, "stretch", 0.02f);
    r->stretch_max = jf(rd, "stretch_max", 8.0f);
    r->soft = jf(rd, "soft", 0.15f);
    r->emissive = jf(rd, "emissive", r->mode == VFX_RENDER_ADD ? 1.0f : 0.0f);
    r->exposure_aware = jb(rd, "exposure_aware", false);
    r->near_fade = jf(rd, "near_fade", 0.25f);
    r->distortion = jf(rd, "distortion", 0.02f);
    r->scatter = jf(rd, "scatter", 0.0f);
    r->aspect = jf(rd, "aspect", 1.0f);
    const cJSON *pl = jget(rd, "light");
    r->light_radius = jf(pl, "radius", 0);
    r->light_intensity = jf(pl, "intensity", 0);
    r->light_color = jv3(pl, "color", v3(1, 0.7f, 0.4f));
    char *txt = cJSON_PrintUnformatted(j);
    if (txt) { e->hash = vfx_str_hash(txt); free(txt); }
}

int vfx_lib_add_effect_json(VfxLibrary *lib, const char *json_text, const char *source) {
    cJSON *root = cJSON_Parse(json_text);
    if (!root) { log_error("vfx: %s: JSON parse error", source); lib->load_errors++; return -1; }
    const char *name = js(root, "name", NULL);
    char stem[VFX_NAME];
    if (!name) {
        const char *b = strrchr(source, '/');
        dg_strcpy(stem, sizeof(stem), b ? b + 1 : source);
        char *dot = strrchr(stem, '.');
        if (dot) *dot = 0;
        name = stem;
    }
    // Replace an existing effect of the same name (hot reload / editor), else append.
    int idx = vfx_lib_find_effect(lib, name);
    if (idx < 0) {
        if (lib->effect_count >= VFX_MAX_EFFECTS) { cJSON_Delete(root); log_error("vfx: too many effects"); return -1; }
        idx = lib->effect_count++;
    }
    VfxEffectDef *fx = &lib->effects[idx];
    // Emitters of a replaced effect are reused in place when the count fits, else appended.
    int old_count = fx->emitter_count;
    int old_emitters[VFX_MAX_EMITTERS_PER_EFFECT];
    memcpy(old_emitters, fx->emitters, sizeof(old_emitters));
    memset(fx, 0, sizeof(*fx));
    dg_strcpy(fx->name, sizeof(fx->name), name);
    dg_strcpy(fx->source, sizeof(fx->source), source);
    fx->hash = vfx_str_hash(json_text);
    fx->duration = jf(root, "duration", 0);
    fx->loop = jb(root, "loop", false);
    fx->priority = (int)jf(root, "priority", 1);
    fx->max_instances = (int)jf(root, "max_instances", 32);
    fx->cull_distance = jf(root, "cull_distance", 150);
    const cJSON *lod = jget(root, "lod");
    for (int i = 0; i < 3; i++) { fx->lod_dist[i] = 1e9f; fx->lod_mul[i] = 1.0f; }
    if (cJSON_IsArray(lod)) {
        int i = 0;
        const cJSON *e;
        cJSON_ArrayForEach(e, lod) {
            if (i >= 3 || !cJSON_IsArray(e) || cJSON_GetArraySize(e) < 2) continue;
            fx->lod_dist[i] = (float)cJSON_GetArrayItem(e, 0)->valuedouble;
            fx->lod_mul[i] = (float)cJSON_GetArrayItem(e, 1)->valuedouble;
            i++;
        }
    } else {
        fx->lod_dist[0] = 25; fx->lod_mul[0] = 0.7f;
        fx->lod_dist[1] = 60; fx->lod_mul[1] = 0.4f;
        fx->lod_dist[2] = 110; fx->lod_mul[2] = 0.2f;
    }
    const cJSON *li = jget(root, "light");
    fx->light.enabled = li != NULL;
    fx->light.color = jv3(li, "color", v3(1.0f, 0.72f, 0.42f));
    fx->light.intensity = jf(li, "intensity", 6.0f);
    fx->light.radius = jf(li, "radius", 5.0f);
    fx->light.duration = jf(li, "duration", 1.5f / 60.0f);
    fx->light.exposure_aware = jb(li, "exposure_aware", true);
    fx->light.shadow = jb(li, "shadow", true);   // muzzle flashes: false (too short-lived to shadow)
    const cJSON *dc = jget(root, "decal");
    fx->decal.enabled = dc != NULL;
    dg_strcpy(fx->decal.texture, sizeof(fx->decal.texture), js(dc, "texture", ""));
    dg_strcpy(fx->decal.normal, sizeof(fx->decal.normal), js(dc, "normal", ""));
    fx->decal.texture_id = fx->decal.normal_id = -1;
    fx->decal.size = jrange(dc, "size", (VfxRange){ 0.06f, 0.09f });
    fx->decal.lifetime = jf(dc, "lifetime", 60.0f);
    fx->decal.depth = jf(dc, "depth", 0.05f);
    fx->decal.variants = (int)jf(dc, "variants", 1);
    fx->decal.tint = jcolor(dc, "tint", v4(1, 1, 1, 1));
    fx->decal.roughness = jf(dc, "roughness", 0.0f);
    fx->decal.dry_time = jf(dc, "dry_time", 0.0f);
    fx->decal.dry_darken = jf(dc, "dry_darken", 1.0f);
    fx->decal.pool = !strcmp(js(dc, "pool", "impact"), "blood") ? VFX_DECAL_POOL_BLOOD : VFX_DECAL_POOL_IMPACT;
    dg_strcpy(fx->sound, sizeof(fx->sound), js(root, "sound", ""));
    const cJSON *ems = jget(root, "emitters");
    const cJSON *ej;
    cJSON_ArrayForEach(ej, ems) {
        if (fx->emitter_count >= VFX_MAX_EMITTERS_PER_EFFECT) { log_warn("vfx: %s: too many emitters", name); break; }
        int ei;
        if (fx->emitter_count < old_count) ei = old_emitters[fx->emitter_count];
        else {
            if (lib->emitter_count >= VFX_MAX_EMITTERS) { log_error("vfx: emitter table full"); break; }
            ei = lib->emitter_count++;
        }
        parse_emitter(&lib->emitters[ei], ej);
        lib->emitters[ei].gpu_index = ei;
        fx->emitters[fx->emitter_count++] = ei;
    }
    cJSON_Delete(root);
    return idx;
}

bool vfx_lib_parse_texture_meta(VfxTextureMeta *m, const char *json_text, const char *name) {
    memset(m, 0, sizeof(*m));
    dg_strcpy(m->name, sizeof(m->name), name);
    cJSON *root = cJSON_Parse(json_text);
    if (!root) return false;
    dg_strcpy(m->file, sizeof(m->file), js(root, "file", ""));
    m->w = (int)jf(root, "width", 0);
    m->h = (int)jf(root, "height", 0);
    const cJSON *g = jget(root, "grid");
    m->cols = cJSON_IsArray(g) && cJSON_GetArraySize(g) >= 2 ? cJSON_GetArrayItem(g, 0)->valueint : 1;
    m->rows = cJSON_IsArray(g) && cJSON_GetArraySize(g) >= 2 ? cJSON_GetArrayItem(g, 1)->valueint : 1;
    if (m->cols < 1) m->cols = 1;
    if (m->rows < 1) m->rows = 1;
    m->frames = (int)jf(root, "frames", (float)(m->cols * m->rows));
    m->variants = (int)jf(root, "variants", 1);
    m->fps = jf(root, "fps", 30);
    m->loop = jb(root, "loop", false);
    m->motion_vectors = jb(root, "motion_vectors", false);
    const char *lt = js(root, "lighting", "none");
    m->lighting = !strcmp(lt, "sixway") ? VFX_LIGHT_SIXWAY : (!strcmp(lt, "simple") ? VFX_LIGHT_SIMPLE : VFX_LIGHT_NONE);
    m->premultiplied = jb(root, "premultiplied", true);
    m->placeholder = jb(root, "placeholder", false);
    cJSON_Delete(root);
    if (!m->file[0]) snprintf(m->file, sizeof(m->file), "%s.png", name);
    return true;
}

int vfx_lib_find_effect(const VfxLibrary *lib, const char *name) {
    if (!name || !name[0]) return -1;
    for (int i = 0; i < lib->effect_count; i++) if (!strcmp(lib->effects[i].name, name)) return i;
    return -1;
}
int vfx_lib_find_texture(const VfxLibrary *lib, const char *name) {
    if (!name || !name[0]) return -1;
    for (int i = 0; i < lib->texture_count; i++) if (!strcmp(lib->textures[i].name, name)) return i;
    return -1;
}

void vfx_lib_resolve(VfxLibrary *lib) {
    for (int i = 0; i < lib->emitter_count; i++) {
        VfxEmitterDef *e = &lib->emitters[i];
        e->render.texture_id = vfx_lib_find_texture(lib, e->render.texture);
        e->render.normal_tex_id = vfx_lib_find_texture(lib, e->render.normal_tex);
        if (e->render.texture[0] && e->render.texture_id < 0) log_warn("vfx: emitter %s: texture '%s' not found (soft sprite used)", e->name, e->render.texture);
        e->update.sub_effect_id = vfx_lib_find_effect(lib, e->update.sub_effect);
    }
    for (int i = 0; i < lib->effect_count; i++) {
        VfxDecalDef *d = &lib->effects[i].decal;
        d->texture_id = vfx_lib_find_texture(lib, d->texture);
        d->normal_id = vfx_lib_find_texture(lib, d->normal);
    }
    int def = vfx_lib_find_effect(lib, "impact_default");
    for (int m = 0; m < SURF_COUNT; m++) {
        char n[64];
        snprintf(n, sizeof(n), "impact_%s", surface_name(m));
        int e = vfx_lib_find_effect(lib, n);
        if (e < 0 && m == SURF_DEFAULT) e = vfx_lib_find_effect(lib, "impact_concrete");
        lib->impact[m] = e >= 0 ? e : def;
    }
    if (lib->impact[SURF_DEFAULT] < 0) lib->impact[SURF_DEFAULT] = lib->impact[SURF_CONCRETE];
    for (int m = 0; m < SURF_COUNT; m++) if (lib->impact[m] < 0) lib->impact[m] = lib->impact[SURF_DEFAULT];
    lib->impact_head = vfx_lib_find_effect(lib, "impact_flesh_head");
    if (lib->impact_head < 0) lib->impact_head = lib->impact[SURF_FLESH];
    for (int t = 0; t < VFX_MUZZLE_TYPE_COUNT; t++)
        for (int d = 0; d < VFX_DEV_COUNT; d++) {
            char n[64];
            snprintf(n, sizeof(n), "muzzle_%s_%s", k_muzzle_types[t], k_muzzle_devices[d]);
            int e = vfx_lib_find_effect(lib, n);
            if (e < 0) { snprintf(n, sizeof(n), "muzzle_%s_bare", k_muzzle_types[t]); e = vfx_lib_find_effect(lib, n); }
            if (e < 0) { snprintf(n, sizeof(n), "muzzle_rifle_%s", k_muzzle_devices[d]); e = vfx_lib_find_effect(lib, n); }
            if (e < 0) e = vfx_lib_find_effect(lib, "muzzle_rifle_bare");
            lib->muzzle[t][d] = e;
        }
}

int vfx_lib_impact_effect(const VfxLibrary *lib, int material, bool headshot) {
    if (material < 0 || material >= SURF_COUNT) material = SURF_DEFAULT;
    if (material == SURF_FLESH && headshot && lib->impact_head >= 0) return lib->impact_head;
    return lib->impact[material];
}

int vfx_lib_muzzle_effect(const VfxLibrary *lib, int type, int device) {
    if (type < 0 || type >= VFX_MUZZLE_TYPE_COUNT) type = VFX_MUZZLE_RIFLE;
    if (device < 0 || device >= VFX_DEV_COUNT) device = VFX_DEV_BARE;
    return lib->muzzle[type][device];
}

// ------------------------------------------------------------------ loading

static char *read_text(const char *path) {
    size_t n = 0;
    char buf[512];
    char *t = platform_read_file(platform_data_path(path, buf, sizeof(buf)), &n);
    return t;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

bool vfx_lib_load(VfxLibrary *lib, const char *dir) {
    u32 rev = lib->revision;
    memset(lib, 0, sizeof(*lib));
    lib->revision = rev + 1;
    dg_strcpy(lib->dir, sizeof(lib->dir), dir);
    char buf[512], path[512];
    // Textures: every *.meta.json in <dir>/tex (sorted for stable indices).
    snprintf(path, sizeof(path), "%s/tex", dir);
    int count = 0;
    char **files = SDL_GlobDirectory(platform_data_path(path, buf, sizeof(buf)), "*.meta.json", 0, &count);
    if (files) {
        qsort(files, (size_t)count, sizeof(char *), cmp_str);
        for (int i = 0; i < count && lib->texture_count < VFX_MAX_TEXTURES; i++) {
            char name[VFX_NAME];
            dg_strcpy(name, sizeof(name), files[i]);
            char *dot = strstr(name, ".meta.json");
            if (dot) *dot = 0;
            char fp[600];
            snprintf(fp, sizeof(fp), "%s/tex/%s", dir, files[i]);
            char *text = read_text(fp);
            if (!text) continue;
            if (vfx_lib_parse_texture_meta(&lib->textures[lib->texture_count], text, name)) lib->texture_count++;
            else { log_warn("vfx: bad texture meta %s", fp); lib->load_errors++; }
            free(text);
        }
        SDL_free(files);
    }
    // Effects.
    snprintf(path, sizeof(path), "%s/effects", dir);
    files = SDL_GlobDirectory(platform_data_path(path, buf, sizeof(buf)), "*.json", 0, &count);
    if (files) {
        qsort(files, (size_t)count, sizeof(char *), cmp_str);
        for (int i = 0; i < count; i++) {
            char fp[600];
            snprintf(fp, sizeof(fp), "%s/effects/%s", dir, files[i]);
            char *text = read_text(fp);
            if (!text) continue;
            vfx_lib_add_effect_json(lib, text, fp);
            free(text);
        }
        SDL_free(files);
    }
    vfx_lib_resolve(lib);
    log_info("vfx: %d effects, %d emitters, %d textures (%d errors)", lib->effect_count, lib->emitter_count, lib->texture_count, lib->load_errors);
    return lib->effect_count > 0;
}

// ------------------------------------------------------------------ save (editor)

static cJSON *range_json(VfxRange r) {
    if (r.min == r.max) return cJSON_CreateNumber(r.min);
    cJSON *a = cJSON_CreateArray();
    cJSON_AddItemToArray(a, cJSON_CreateNumber(r.min));
    cJSON_AddItemToArray(a, cJSON_CreateNumber(r.max));
    return a;
}
static cJSON *vec_json(const float *v, int n) {
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < n; i++) cJSON_AddItemToArray(a, cJSON_CreateNumber(v[i]));
    return a;
}
static cJSON *curve_json(const VfxCurve *c, int channels) {
    if (c->type == VFX_CURVE_CONST || c->key_count <= 1) {
        if (channels == 1) return cJSON_CreateNumber(c->v[0].x);
    }
    cJSON *keys = cJSON_CreateArray();
    for (int i = 0; i < c->key_count; i++) {
        float k[5] = { c->t[i], c->v[i].x, c->v[i].y, c->v[i].z, c->v[i].w };
        cJSON_AddItemToArray(keys, vec_json(k, 1 + channels));
    }
    if (c->type != VFX_CURVE_BEZIER) return keys;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "bezier");
    cJSON_AddItemToObject(o, "keys", keys);
    return o;
}

char *vfx_lib_effect_to_json(const VfxLibrary *lib, int idx) {
    if (idx < 0 || idx >= lib->effect_count) return NULL;
    const VfxEffectDef *fx = &lib->effects[idx];
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", fx->name);
    cJSON_AddNumberToObject(root, "duration", fx->duration);
    if (fx->loop) cJSON_AddBoolToObject(root, "loop", true);
    cJSON_AddNumberToObject(root, "priority", fx->priority);
    cJSON_AddNumberToObject(root, "max_instances", fx->max_instances);
    cJSON_AddNumberToObject(root, "cull_distance", fx->cull_distance);
    cJSON *lod = cJSON_AddArrayToObject(root, "lod");
    for (int i = 0; i < 3; i++) {
        if (fx->lod_dist[i] >= 1e8f) break;
        float k[2] = { fx->lod_dist[i], fx->lod_mul[i] };
        cJSON_AddItemToArray(lod, vec_json(k, 2));
    }
    if (fx->sound[0]) cJSON_AddStringToObject(root, "sound", fx->sound);
    if (fx->light.enabled) {
        cJSON *l = cJSON_AddObjectToObject(root, "light");
        cJSON_AddItemToObject(l, "color", vec_json(&fx->light.color.x, 3));
        cJSON_AddNumberToObject(l, "intensity", fx->light.intensity);
        cJSON_AddNumberToObject(l, "radius", fx->light.radius);
        cJSON_AddNumberToObject(l, "duration", fx->light.duration);
        cJSON_AddBoolToObject(l, "exposure_aware", fx->light.exposure_aware);
        if (!fx->light.shadow) cJSON_AddBoolToObject(l, "shadow", false);
    }
    if (fx->decal.enabled) {
        cJSON *d = cJSON_AddObjectToObject(root, "decal");
        cJSON_AddStringToObject(d, "texture", fx->decal.texture);
        if (fx->decal.normal[0]) cJSON_AddStringToObject(d, "normal", fx->decal.normal);
        cJSON_AddItemToObject(d, "size", range_json(fx->decal.size));
        cJSON_AddNumberToObject(d, "lifetime", fx->decal.lifetime);
        cJSON_AddNumberToObject(d, "depth", fx->decal.depth);
        cJSON_AddNumberToObject(d, "variants", fx->decal.variants);
        cJSON_AddItemToObject(d, "tint", vec_json(&fx->decal.tint.x, 4));
    }
    cJSON *ems = cJSON_AddArrayToObject(root, "emitters");
    for (int k = 0; k < fx->emitter_count; k++) {
        const VfxEmitterDef *e = &lib->emitters[fx->emitters[k]];
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "name", e->name);
        cJSON_AddStringToObject(j, "tier", k_tiers[e->tier]);
        if (e->deterministic) cJSON_AddBoolToObject(j, "deterministic", true);
        if (!e->enabled) cJSON_AddBoolToObject(j, "enabled", false);
        if (e->lod_cull > 0) cJSON_AddNumberToObject(j, "lod_cull", e->lod_cull);
        const VfxSpawnDef *s = &e->spawn;
        cJSON *sp = cJSON_AddObjectToObject(j, "spawn");
        cJSON_AddStringToObject(sp, "mode", k_spawn_modes[s->mode]);
        cJSON_AddItemToObject(sp, "count", range_json(s->count));
        if (s->rate) cJSON_AddNumberToObject(sp, "rate", s->rate);
        if (s->per_meter) cJSON_AddNumberToObject(sp, "per_meter", s->per_meter);
        if (s->delay) cJSON_AddNumberToObject(sp, "delay", s->delay);
        if (s->duration) cJSON_AddNumberToObject(sp, "duration", s->duration);
        if (s->bursts > 1) { cJSON_AddNumberToObject(sp, "bursts", s->bursts); cJSON_AddNumberToObject(sp, "burst_interval", s->burst_interval); }
        cJSON_AddStringToObject(sp, "shape", k_shapes[s->shape]);
        if (s->radius) cJSON_AddNumberToObject(sp, "radius", s->radius);
        if (s->shape == VFX_SHAPE_BOX) cJSON_AddItemToObject(sp, "box", vec_json(&s->box.x, 3));
        if (s->angle) cJSON_AddNumberToObject(sp, "angle", s->angle);
        cJSON_AddStringToObject(sp, "dir", k_dirs[s->dir]);
        cJSON_AddNumberToObject(sp, "dir_bias", s->dir_bias);
        cJSON_AddItemToObject(sp, "speed", range_json(s->speed));
        if (s->inherit) cJSON_AddNumberToObject(sp, "inherit", s->inherit);
        cJSON_AddItemToObject(sp, "life", range_json(s->life));
        cJSON_AddItemToObject(sp, "size", range_json(s->size));
        cJSON_AddItemToObject(sp, "rotation", range_json(s->rotation));
        cJSON_AddItemToObject(sp, "spin", range_json(s->spin));
        cJSON_AddItemToObject(sp, "color", vec_json(&s->color_a.x, 4));
        cJSON_AddItemToObject(sp, "color2", vec_json(&s->color_b.x, 4));
        if (s->offset) cJSON_AddNumberToObject(sp, "offset", s->offset);
        if (s->local) cJSON_AddBoolToObject(sp, "local", true);
        if (s->frame_random) cJSON_AddBoolToObject(sp, "frame_random", true);
        if (s->variant_no_repeat) cJSON_AddBoolToObject(sp, "variant_no_repeat", true);
        const VfxUpdateDef *u = &e->update;
        cJSON *up = cJSON_AddObjectToObject(j, "update");
        cJSON_AddNumberToObject(up, "gravity", u->gravity);
        cJSON_AddNumberToObject(up, "drag", u->drag);
        if (u->wind) cJSON_AddNumberToObject(up, "wind", u->wind);
        if (u->turbulence) {
            cJSON *t = cJSON_AddObjectToObject(up, "turbulence");
            cJSON_AddNumberToObject(t, "strength", u->turbulence);
            cJSON_AddNumberToObject(t, "freq", u->turb_freq);
            cJSON_AddNumberToObject(t, "speed", u->turb_speed);
        }
        cJSON_AddItemToObject(up, "size_curve", curve_json(&u->size_curve, 1));
        cJSON_AddItemToObject(up, "color_curve", curve_json(&u->color_curve, 4));
        cJSON_AddItemToObject(up, "velocity_curve", curve_json(&u->velocity_curve, 1));
        if (u->spin_curve.key_count > 1) cJSON_AddItemToObject(up, "spin_curve", curve_json(&u->spin_curve, 1));
        if (u->frame_over_life) cJSON_AddBoolToObject(up, "frame_over_life", true);
        if (u->fps) cJSON_AddNumberToObject(up, "fps", u->fps);
        if (u->motion_vectors) cJSON_AddBoolToObject(up, "motion_vectors", true);
        if (u->collide) {
            cJSON_AddStringToObject(up, "collide", k_collide[u->collide]);
            cJSON_AddNumberToObject(up, "bounce", u->bounce);
            cJSON_AddNumberToObject(up, "friction", u->friction);
            if (u->kill_on_hit) cJSON_AddBoolToObject(up, "kill_on_hit", true);
        }
        if (u->attract) {
            cJSON *a = cJSON_AddObjectToObject(up, "attractor");
            cJSON_AddItemToObject(a, "pos", vec_json(&u->attractor.x, 3));
            cJSON_AddNumberToObject(a, "strength", u->attract);
        }
        if (u->vortex) cJSON_AddNumberToObject(up, "vortex", u->vortex);
        if (u->sub_on) {
            cJSON *s2 = cJSON_AddObjectToObject(up, "sub_emitter");
            cJSON_AddStringToObject(s2, "on", k_sub_on[u->sub_on]);
            cJSON_AddStringToObject(s2, "effect", u->sub_effect);
            cJSON_AddNumberToObject(s2, "count", u->sub_count);
        }
        const VfxRenderDef *r = &e->render;
        cJSON *rd = cJSON_AddObjectToObject(j, "render");
        cJSON_AddStringToObject(rd, "mode", k_render_modes[r->mode]);
        cJSON_AddStringToObject(rd, "orient", k_orients[r->orient]);
        if (r->lighting) cJSON_AddStringToObject(rd, "lighting", k_lighting[r->lighting]);
        if (r->texture[0]) cJSON_AddStringToObject(rd, "texture", r->texture);
        if (r->normal_tex[0]) cJSON_AddStringToObject(rd, "normal", r->normal_tex);
        if (r->mesh[0]) cJSON_AddStringToObject(rd, "mesh", r->mesh);
        if (r->orient == VFX_ORIENT_STRETCH) { cJSON_AddNumberToObject(rd, "stretch", r->stretch); cJSON_AddNumberToObject(rd, "stretch_max", r->stretch_max); }
        cJSON_AddNumberToObject(rd, "soft", r->soft);
        cJSON_AddNumberToObject(rd, "emissive", r->emissive);
        if (r->exposure_aware) cJSON_AddBoolToObject(rd, "exposure_aware", true);
        cJSON_AddNumberToObject(rd, "near_fade", r->near_fade);
        if (r->mode == VFX_RENDER_DISTORT) cJSON_AddNumberToObject(rd, "distortion", r->distortion);
        if (r->aspect != 1.0f) cJSON_AddNumberToObject(rd, "aspect", r->aspect);
        if (r->light_intensity > 0) {
            cJSON *l = cJSON_AddObjectToObject(rd, "light");
            cJSON_AddNumberToObject(l, "radius", r->light_radius);
            cJSON_AddNumberToObject(l, "intensity", r->light_intensity);
            cJSON_AddItemToObject(l, "color", vec_json(&r->light_color.x, 3));
        }
        cJSON_AddItemToArray(ems, j);
    }
    char *out = cJSON_Print(root);
    cJSON_Delete(root);
    return out;
}
