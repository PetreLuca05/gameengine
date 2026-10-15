#include "vfx_flesh.h"
#include "assets/character.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

u32 flesh_hash(u32 x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

float flesh_rand01(u32 *s) {
    *s = flesh_hash(*s + 0x9e3779b9u);
    return (float)(*s >> 8) / 16777216.0f;
}

static void cal(FleshCaliberRule *c, float chance, float thick, float max_dist, float scale, float mist, float drops, int rays, float range, float cone) {
    c->exit_chance = chance;
    c->max_thickness = thick;
    c->max_dist = max_dist;
    c->scale = scale;
    c->mist = mist;
    c->droplets = drops;
    c->splatter_rays = rays;
    c->splatter_range = range;
    c->cone_deg = cone;
}

void flesh_rules_defaults(FleshRules *r) {
    memset(r, 0, sizeof(*r));
    // Rifles always exit, pistols rarely, shotgun pellets never at range.
    cal(&r->cal[VFX_CAL_PISTOL], 0.2f, 0.22f, 0, 0.8f, 0.8f, 0.8f, 3, 2.5f, 16);
    cal(&r->cal[VFX_CAL_SMG], 0.3f, 0.25f, 0, 0.85f, 0.85f, 0.85f, 3, 2.5f, 16);
    cal(&r->cal[VFX_CAL_RIFLE], 1.0f, 0.6f, 0, 1.15f, 1.1f, 1.2f, 6, 4.0f, 14);
    cal(&r->cal[VFX_CAL_SHOTGUN], 0.15f, 0.12f, 6.0f, 0.65f, 0.6f, 0.6f, 1, 2.0f, 20);
    cal(&r->cal[VFX_CAL_SNIPER], 1.0f, 0.9f, 0, 1.4f, 1.3f, 1.5f, 8, 5.0f, 12);
    cal(&r->cal[VFX_CAL_MELEE], 0.0f, 0.0f, 0, 0.9f, 0.6f, 1.3f, 3, 1.6f, 35);
    for (int i = 0; i < BP_COUNT; i++) { r->part_thickness[i] = 1.0f; r->part_exit[i] = 1.0f; }
    r->part_thickness[BP_HEAD] = 1.1f;   // skull
    for (int i = BP_UPPER_ARM_L; i <= BP_FOOT_R; i++) r->part_exit[i] = 1.6f;   // thin: exits easily
    r->dist_near = 5.0f;
    r->dist_far = 40.0f;
    r->dist_far_scale = 0.65f;
    r->dmg_ref = 35.0f;
    r->dmg_min_scale = 0.75f;
    r->dmg_max_scale = 1.5f;
    r->splat_size[0] = 0.16f;
    r->splat_size[1] = 0.42f;
    r->splat_stretch_max = 3.5f;
    r->drip_chance = 0.65f;
    r->drip_len[0] = 0.25f;
    r->drip_len[1] = 0.7f;
    r->drip_time = 6.0f;
    r->pool_size[0] = 0.25f;
    r->pool_size[1] = 1.15f;
    r->pool_grow_time = 28.0f;
    r->pool_delay = 1.2f;
    r->stain_size[0] = 0.05f;
    r->stain_size[1] = 0.08f;
    r->stain_grow = 0.24f;
    r->stain_grow_time = 35.0f;
    r->wound_entry = 0.035f;
    r->wound_exit = 0.075f;
    r->bleed_rate = 5.0f;
    r->bleed_decay = 0.12f;
    r->bleed_time = 30.0f;
    r->kill_scale = 1.4f;
    r->head_scale = 1.5f;
    r->limb_droplets = 1.6f;
    r->skinned_max = 16;
}

static float jf(const cJSON *o, const char *k, float def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}
static void jrange(const cJSON *o, const char *k, float *out) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsArray(v) && cJSON_GetArraySize(v) >= 2) {
        out[0] = (float)cJSON_GetArrayItem(v, 0)->valuedouble;
        out[1] = (float)cJSON_GetArrayItem(v, 1)->valuedouble;
    } else if (cJSON_IsNumber(v)) {
        out[0] = out[1] = (float)v->valuedouble;
    }
}

bool flesh_rules_parse(FleshRules *r, const char *json, char *err, int err_size) {
    flesh_rules_defaults(r);
    cJSON *root = cJSON_Parse(json);
    if (!root) { if (err) snprintf(err, (size_t)err_size, "flesh_rules: JSON parse error"); return false; }
    static const char *const cal_names[VFX_CAL_COUNT] = { "pistol", "smg", "rifle", "shotgun", "sniper", "melee" };
    const cJSON *cals = cJSON_GetObjectItemCaseSensitive(root, "calibers");
    for (int i = 0; i < VFX_CAL_COUNT; i++) {
        const cJSON *c = cJSON_GetObjectItemCaseSensitive(cals, cal_names[i]);
        if (!cJSON_IsObject(c)) continue;
        FleshCaliberRule *k = &r->cal[i];
        k->exit_chance = jf(c, "exit_chance", k->exit_chance);
        k->max_thickness = jf(c, "max_thickness", k->max_thickness);
        k->max_dist = jf(c, "exit_max_dist", k->max_dist);
        k->scale = jf(c, "scale", k->scale);
        k->mist = jf(c, "mist", k->mist);
        k->droplets = jf(c, "droplets", k->droplets);
        k->splatter_rays = (int)jf(c, "splatter_rays", (float)k->splatter_rays);
        k->splatter_range = jf(c, "splatter_range", k->splatter_range);
        k->cone_deg = jf(c, "cone", k->cone_deg);
    }
    const cJSON *e;
    bool m[BP_COUNT];
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(root, "parts")) {
        if (!cJSON_IsObject(e) || !character_part_group(e->string, m)) continue;
        for (int i = 0; i < BP_COUNT; i++) {
            if (!m[i]) continue;
            r->part_thickness[i] = jf(e, "thickness", r->part_thickness[i]);
            r->part_exit[i] = jf(e, "exit", r->part_exit[i]);
        }
    }
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(root, "distance");
    r->dist_near = jf(o, "near", r->dist_near);
    r->dist_far = jf(o, "far", r->dist_far);
    r->dist_far_scale = jf(o, "far_scale", r->dist_far_scale);
    o = cJSON_GetObjectItemCaseSensitive(root, "damage");
    r->dmg_ref = jf(o, "ref", r->dmg_ref);
    r->dmg_min_scale = jf(o, "min", r->dmg_min_scale);
    r->dmg_max_scale = jf(o, "max", r->dmg_max_scale);
    o = cJSON_GetObjectItemCaseSensitive(root, "splatter");
    jrange(o, "size", r->splat_size);
    r->splat_stretch_max = jf(o, "stretch_max", r->splat_stretch_max);
    r->drip_chance = jf(o, "drip_chance", r->drip_chance);
    jrange(o, "drip_len", r->drip_len);
    r->drip_time = jf(o, "drip_time", r->drip_time);
    o = cJSON_GetObjectItemCaseSensitive(root, "pool");
    jrange(o, "size", r->pool_size);
    r->pool_grow_time = jf(o, "grow_time", r->pool_grow_time);
    r->pool_delay = jf(o, "delay", r->pool_delay);
    o = cJSON_GetObjectItemCaseSensitive(root, "stain");
    jrange(o, "size", r->stain_size);
    r->stain_grow = jf(o, "grow", r->stain_grow);
    r->stain_grow_time = jf(o, "grow_time", r->stain_grow_time);
    o = cJSON_GetObjectItemCaseSensitive(root, "wound");
    r->wound_entry = jf(o, "entry", r->wound_entry);
    r->wound_exit = jf(o, "exit", r->wound_exit);
    o = cJSON_GetObjectItemCaseSensitive(root, "bleed");
    r->bleed_rate = jf(o, "rate", r->bleed_rate);
    r->bleed_decay = jf(o, "decay", r->bleed_decay);
    r->bleed_time = jf(o, "time", r->bleed_time);
    r->kill_scale = jf(root, "kill_scale", r->kill_scale);
    r->head_scale = jf(root, "head_scale", r->head_scale);
    r->limb_droplets = jf(root, "limb_droplets", r->limb_droplets);
    r->skinned_max = DG_CLAMP((int)jf(root, "skinned_max", (float)r->skinned_max), 0, 16);
    cJSON_Delete(root);
    return true;
}

float flesh_hit_scale(const FleshRules *r, int caliber, float damage, float dist) {
    const FleshCaliberRule *c = &r->cal[DG_CLAMP(caliber, 0, VFX_CAL_COUNT - 1)];
    float d = r->dist_far > r->dist_near ? clampf((dist - r->dist_near) / (r->dist_far - r->dist_near), 0, 1) : 0;
    float ds = 1.0f + (r->dist_far_scale - 1.0f) * d;
    float dm = r->dmg_ref > 0 && damage > 0 ? clampf(sqrtf(damage / r->dmg_ref), r->dmg_min_scale, r->dmg_max_scale) : 1.0f;
    return c->scale * ds * dm;
}

bool flesh_capsule_exit(vec3 a, vec3 b, float radius, vec3 entry, vec3 dir, vec3 *exit, float *thickness) {
    if (v3_len2(dir) < 1e-8f || radius <= 0) return false;
    dir = v3_norm(dir);
    // March back from beyond the far side: the first hit is the exit.
    float L = 2.0f * radius + v3_dist(a, b) + 0.05f;
    vec3 start = v3_madd(entry, dir, L);
    float t = ray_capsule(start, v3_neg(dir), a, b, radius);
    if (t < 0 || t > L) return false;
    float th = L - t;
    if (th < 0.01f) return false;
    *exit = v3_madd(start, dir, -t);
    *thickness = th;
    return true;
}

bool flesh_should_exit(const FleshRules *r, int caliber, int part, float thickness, float dist, u32 seed) {
    const FleshCaliberRule *c = &r->cal[DG_CLAMP(caliber, 0, VFX_CAL_COUNT - 1)];
    if (c->exit_chance <= 0) return false;
    if (c->max_dist > 0 && dist > c->max_dist) return false;
    float tm = part >= 0 && part < BP_COUNT ? r->part_thickness[part] : 1.0f;
    if (thickness * tm > c->max_thickness) return false;
    float chance = c->exit_chance * (part >= 0 && part < BP_COUNT ? r->part_exit[part] : 1.0f);
    if (chance >= 1.0f) return true;
    u32 s = seed;
    return flesh_rand01(&s) < chance;
}

int flesh_plan_splatter(const FleshRules *r, vec3 origin, vec3 dir, float scale, int rays, float range, float cone_deg,
                        FleshRayFn fn, void *ctx, u32 seed, FleshSplat *out, int max) {
    if (!fn || rays <= 0 || max <= 0 || v3_len2(dir) < 1e-8f) return 0;
    dir = v3_norm(dir);
    vec3 up = fabsf(dir.y) > 0.9f ? v3(1, 0, 0) : v3(0, 1, 0);
    vec3 bx = v3_norm(v3_cross(up, dir));
    vec3 by = v3_cross(dir, bx);
    float cmax = cosf(cone_deg * DG_DEG2RAD);
    int n = 0;
    u32 s = flesh_hash(seed ^ 0x51a7u);
    for (int i = 0; i < rays && n < max; i++) {
        // Uniform in the cone, with the rays' length jittered (droplets fly different distances).
        float u = flesh_rand01(&s), v = flesh_rand01(&s);
        float ct = 1.0f - u * (1.0f - cmax), st = sqrtf(fmaxf(0, 1 - ct * ct));
        float ph = v * 6.2831853f;
        vec3 rd = v3_add(v3_scale(dir, ct), v3_add(v3_scale(bx, st * cosf(ph)), v3_scale(by, st * sinf(ph))));
        // Gravity: the heavier drops sag, longer rays hit lower.
        rd = v3_norm(v3_add(rd, v3(0, -0.12f * flesh_rand01(&s), 0)));
        float len = range * (0.6f + 0.4f * flesh_rand01(&s));
        FleshSplat *o = &out[n++];
        memset(o, 0, sizeof(*o));
        o->from = v3_madd(origin, rd, 0.02f);
        o->to = v3_madd(o->from, rd, len);
        vec3 hp, hn;
        if (!fn(ctx, o->from, v3_scale(rd, len), &hp, &hn)) { flesh_rand01(&s); flesh_rand01(&s); continue; }
        o->hit = true;
        o->pos = hp;
        o->normal = v3_len2(hn) > 1e-8f ? v3_norm(hn) : v3_neg(rd);
        o->to = hp;
        float t = v3_dist(o->from, hp);
        float f = clampf(t / fmaxf(range, 1e-3f), 0, 1);
        o->dist = t;
        o->size = (r->splat_size[0] + (r->splat_size[1] - r->splat_size[0]) * flesh_rand01(&s)) * scale * (0.6f + 0.8f * f);
        o->alpha = 1.0f - 0.55f * f;
        // Incidence: a grazing ray smears the splat along its direction.
        float ci = fabsf(v3_dot(o->normal, rd));
        o->aspect = clampf(1.0f / fmaxf(ci, 0.15f), 1.0f, r->splat_stretch_max);
        vec3 tg = v3_sub(rd, v3_scale(o->normal, v3_dot(rd, o->normal)));
        o->tangent = v3_len2(tg) > 1e-6f ? v3_norm(tg) : v3(0, 0, 0);
        if (fabsf(o->normal.y) < 0.4f && flesh_rand01(&s) < r->drip_chance) {
            o->drip = true;
            o->drip_len = (r->drip_len[0] + (r->drip_len[1] - r->drip_len[0]) * flesh_rand01(&s)) * fminf(scale, 1.5f);
        }
    }
    return n;
}
