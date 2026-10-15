#include "hitboxes.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>

bool hitbox_table_load(HitboxTable *t, const RagdollDef *rd, const cJSON *json, char *err, int err_size) {
    memset(t, 0, sizeof(*t));
    for (int i = 0; i < rd->body_count; i++) {
        t->body_part[i] = rd->bodies[i].part;
        t->radius_scale[i] = 1.0f;
    }
    const cJSON *bodies = json ? cJSON_GetObjectItemCaseSensitive(json, "bodies") : NULL;
    const cJSON *e;
    cJSON_ArrayForEach(e, bodies) {
        int b = -1;
        for (int i = 0; i < rd->body_count; i++) if (!strcmp(rd->bodies[i].name, e->string)) b = i;
        if (b < 0) { if (err) snprintf(err, (size_t)err_size, "hitboxes: unknown ragdoll body '%s'", e->string); return false; }
        const char *part = cJSON_IsString(e) ? e->valuestring : NULL;
        if (cJSON_IsObject(e)) {
            const cJSON *p = cJSON_GetObjectItemCaseSensitive(e, "part");
            part = cJSON_IsString(p) ? p->valuestring : NULL;
            const cJSON *rs = cJSON_GetObjectItemCaseSensitive(e, "radius_scale");
            if (cJSON_IsNumber(rs)) t->radius_scale[b] = (float)rs->valuedouble;
        }
        if (part) {
            int bp = body_part_from_name(part);
            if (bp < 0) { if (err) snprintf(err, (size_t)err_size, "hitboxes: unknown body part '%s'", part); return false; }
            t->body_part[b] = (u8)bp;
        }
    }
    t->hash = anim_hash(t, sizeof(*t), ANIM_HASH_SEED);
    return true;
}

void hitbox_build(const HitboxTable *t, const RagdollDef *rd, const Xform *bone_world, float scale, HitboxSet *out) {
    out->count = 0;
    for (int i = 0; i < rd->body_count && out->count < HB_MAX; i++) {
        HitCapsule *c = &out->c[out->count++];
        ragdoll_body_segment(&rd->bodies[i], bone_world[rd->bodies[i].bone], scale, &c->a, &c->b, &c->r);
        c->r *= t->radius_scale[i];
        c->part = t->body_part[i];
        c->body = (u8)i;
    }
}

void hitbox_record(HitboxHistory *h, const HitboxSet *s) {
    h->ring[h->head] = *s;
    h->head = (h->head + 1) % HB_HISTORY;
    if (h->count < HB_HISTORY) h->count++;
}

static const HitboxSet *find_tick(const HitboxHistory *h, u32 tick) {
    for (int k = 0; k < h->count; k++) {
        const HitboxSet *s = &h->ring[(h->head - 1 - k + HB_HISTORY * 2) % HB_HISTORY];
        if (s->tick == tick) return s;
    }
    return NULL;
}

bool hitbox_rewind(const HitboxHistory *h, u32 tick256, HitboxSet *out) {
    if (!h->count) { out->count = 0; return false; }
    const HitboxSet *newest = &h->ring[(h->head - 1 + HB_HISTORY) % HB_HISTORY];
    u32 t0 = tick256 >> 8;
    u32 frac = tick256 & 0xFF;
    const HitboxSet *a = find_tick(h, t0);
    if (!a) { *out = *newest; return false; }
    if (!frac) { *out = *a; return true; }
    const HitboxSet *b = find_tick(h, t0 + 1);
    if (!b || b->count != a->count) { *out = *a; return true; }
    float f = (float)frac / 256.0f;
    *out = *a;
    for (int i = 0; i < a->count; i++) {
        out->c[i].a = v3_lerp(a->c[i].a, b->c[i].a, f);
        out->c[i].b = v3_lerp(a->c[i].b, b->c[i].b, f);
    }
    return true;
}

bool hitbox_raycast(const HitboxSet *s, vec3 o, vec3 d, float max_dist, HitboxRayHit *out) {
    float best = max_dist;
    int bi = -1;
    for (int i = 0; i < s->count; i++) {
        const HitCapsule *c = &s->c[i];
        float t = ray_capsule(o, d, c->a, c->b, c->r);
        if (t >= 0.0f && t < best) { best = t; bi = i; }
    }
    if (bi < 0) return false;
    out->t = best;
    out->index = bi;
    out->part = s->c[bi].part;
    out->point = v3_madd(o, d, best);
    return true;
}

static float seg_seg_dist(vec3 p1, vec3 q1, vec3 p2, vec3 q2, float *s_out) {
    vec3 d1 = v3_sub(q1, p1), d2 = v3_sub(q2, p2), r = v3_sub(p1, p2);
    float a = v3_dot(d1, d1), e = v3_dot(d2, d2), f = v3_dot(d2, r);
    float s, t;
    if (a <= 1e-8f && e <= 1e-8f) { *s_out = 0; return v3_len(r); }
    if (a <= 1e-8f) { s = 0; t = saturate(f / e); }
    else {
        float c = v3_dot(d1, r);
        if (e <= 1e-8f) { t = 0; s = saturate(-c / a); }
        else {
            float b = v3_dot(d1, d2), den = a * e - b * b;
            s = den != 0 ? saturate((b * f - c * e) / den) : 0;
            t = (b * s + f) / e;
            if (t < 0) { t = 0; s = saturate(-c / a); }
            else if (t > 1) { t = 1; s = saturate((b - c) / a); }
        }
    }
    *s_out = s;
    return v3_dist(v3_madd(p1, d1, s), v3_madd(p2, d2, t));
}

bool hitbox_sweep(const HitboxSet *s, vec3 a, vec3 b, float radius, HitboxRayHit *out) {
    float best = 1e9f;
    int bi = -1;
    float bs = 0;
    for (int i = 0; i < s->count; i++) {
        float sp;
        float d = seg_seg_dist(a, b, s->c[i].a, s->c[i].b, &sp);
        if (d <= s->c[i].r + radius && sp < best) { best = sp; bi = i; bs = sp; }
    }
    if (bi < 0) return false;
    out->index = bi;
    out->part = s->c[bi].part;
    out->t = bs * v3_dist(a, b);
    out->point = v3_lerp(a, b, bs);
    return true;
}

void part_mult_defaults(float *m) { for (int i = 0; i < BP_COUNT; i++) m[i] = 1.0f; }

void part_mult_parse(float *m, const cJSON *obj) {
    const cJSON *e;
    cJSON_ArrayForEach(e, obj) {
        if (!cJSON_IsNumber(e)) continue;
        int p = body_part_from_name(e->string);
        if (p >= 0) { m[p] = (float)e->valuedouble; continue; }
        // Group shortcuts: "arms", "hands", "legs", "feet", "torso".
        float v = (float)e->valuedouble;
        if (!strcmp(e->string, "arms")) { m[BP_UPPER_ARM_L] = m[BP_UPPER_ARM_R] = m[BP_LOWER_ARM_L] = m[BP_LOWER_ARM_R] = v; }
        else if (!strcmp(e->string, "hands")) { m[BP_HAND_L] = m[BP_HAND_R] = v; }
        else if (!strcmp(e->string, "legs")) { m[BP_UPPER_LEG_L] = m[BP_UPPER_LEG_R] = m[BP_LOWER_LEG_L] = m[BP_LOWER_LEG_R] = v; }
        else if (!strcmp(e->string, "feet")) { m[BP_FOOT_L] = m[BP_FOOT_R] = v; }
        else if (!strcmp(e->string, "limbs")) {
            for (int i = BP_UPPER_ARM_L; i <= BP_FOOT_R; i++) m[i] = v;
        } else if (!strcmp(e->string, "torso")) { m[BP_CHEST] = m[BP_STOMACH] = m[BP_PELVIS] = v; }
    }
}

float damage_falloff(float dist, float start, float end, float min_mult) {
    if (end <= start || dist <= start) return 1.0f;
    if (dist >= end) return min_mult;
    return lerpf(1.0f, min_mult, (dist - start) / (end - start));
}
