// Pure first-person camera math (no cvars / engine): the nominal eye table, the glide springs, the
// anchor statistics. Unit tested in tests/test_anim.c.
#include "camera_fp.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *k_posture[EYE_POSTURES] = { "stand", "crouch", "swim", "hang", "downed", "mantle" };
const char *eye_posture_name(int p) { return p >= 0 && p < EYE_POSTURES ? k_posture[p] : "stand"; }

void eye_table_defaults(EyeNominalTable *t) {
    memset(t, 0, sizeof(*t));
    static const float pitch[EYE_PITCH_N] = { -85, -60, -30, 0, 30, 60, 85 };
    memcpy(t->pitch, pitch, sizeof(pitch));
    // Speeds: the locomotion blend's samples (assets/human/controller.json: walk 1.6, run 4.2, sprint 6.8).
    static const float sp[EYE_POSTURES][EYE_SPEED_N] = { { 0, 1.6f, 4.2f, 6.8f }, { 0, 1.2f }, { 0, 1.5f }, { 0 }, { 0, 0.8f }, { 0, 0.33f, 0.67f, 1.0f } };
    static const int n[EYE_POSTURES] = { 4, 2, 2, 1, 2, 4 };
    memcpy(t->speed, sp, sizeof(sp));
    memcpy(t->speed_n, n, sizeof(n));
}

static const cJSON *jget(const cJSON *o, const char *k) { return o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL; }

void eye_table_parse(EyeNominalTable *t, const cJSON *root) {
    eye_table_defaults(t);
    if (!root) return;
    const cJSON *ah = jget(root, "anim_hash");
    if (cJSON_IsString(ah)) t->anim_hash = (u32)strtoul(ah->valuestring, NULL, 16);
    const cJSON *pa = jget(root, "pitch");
    if (cJSON_IsArray(pa) && cJSON_GetArraySize(pa) == EYE_PITCH_N)
        for (int i = 0; i < EYE_PITCH_N; i++) t->pitch[i] = (float)cJSON_GetArrayItem(pa, i)->valuedouble;
    const cJSON *ps = jget(root, "postures");
    for (int p = 0; p < EYE_POSTURES; p++) {
        const cJSON *po = jget(ps, k_posture[p]);
        if (!po) continue;
        const cJSON *sa = jget(po, "speed");
        if (cJSON_IsArray(sa) && cJSON_GetArraySize(sa) >= 1) {
            t->speed_n[p] = DG_MIN(cJSON_GetArraySize(sa), EYE_SPEED_N);
            for (int i = 0; i < t->speed_n[p]; i++) t->speed[p][i] = (float)cJSON_GetArrayItem(sa, i)->valuedouble;
        }
        const cJSON *r = jget(po, "residual");
        if (cJSON_IsNumber(r)) t->residual[p] = (float)r->valuedouble;
        const cJSON *st = jget(po, "styles"), *e;
        cJSON_ArrayForEach(e, st) {
            int s = e->string ? atoi(e->string) : -1;
            if (s < 0 || s >= EYE_STYLES || !cJSON_IsArray(e)) continue;
            for (int i = 0; i < t->speed_n[p] && i < cJSON_GetArraySize(e); i++) {
                const cJSON *row = cJSON_GetArrayItem(e, i);
                for (int k = 0; k < EYE_PITCH_N && k < cJSON_GetArraySize(row); k++) {
                    const cJSON *v = cJSON_GetArrayItem(row, k);
                    if (cJSON_GetArraySize(v) < 3) continue;
                    t->off[p][s][i][k] = v3((float)cJSON_GetArrayItem(v, 0)->valuedouble, (float)cJSON_GetArrayItem(v, 1)->valuedouble,
                                            (float)cJSON_GetArrayItem(v, 2)->valuedouble);
                }
            }
            if (p == EYE_STAND) t->style_valid[s] = true;
        }
    }
    t->valid = t->style_valid[0];
}

static double r4(float v) { return round((double)v * 10000.0) / 10000.0; }

char *eye_table_to_json(const EyeNominalTable *t) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "_comment",
                            "Nominal first-person eye (docs/CAMERA_HOLDPOINT.md \"Rule 2\"): the head eye's mean over each clip cycle, (right, up, forward) m "
                            "from the capsule's feet in the view's yaw frame, per posture x hold style (weapon type) x speed sample (mantle: progress) x "
                            "view pitch sample (deg). residual: how far the animated head eye strayed from the mean while measured (m). Written by "
                            "cam_calibrate (src/anim/eye_nominal.c); anim_hash: the animation library measured (the game recalibrates its own copy at "
                            "startup when it differs; tests/test_anim.c fails until cam_calibrate updates this file).");
    char hash[16];
    snprintf(hash, sizeof(hash), "%08x", t->anim_hash);
    cJSON_AddStringToObject(o, "anim_hash", hash);
    cJSON *pa = cJSON_AddArrayToObject(o, "pitch");
    for (int i = 0; i < EYE_PITCH_N; i++) cJSON_AddItemToArray(pa, cJSON_CreateNumber(t->pitch[i]));
    cJSON *ps = cJSON_AddObjectToObject(o, "postures");
    for (int p = 0; p < EYE_POSTURES; p++) {
        cJSON *po = cJSON_AddObjectToObject(ps, k_posture[p]);
        cJSON *sa = cJSON_AddArrayToObject(po, "speed");
        for (int i = 0; i < t->speed_n[p]; i++) cJSON_AddItemToArray(sa, cJSON_CreateNumber(r4(t->speed[p][i])));
        cJSON_AddNumberToObject(po, "residual", r4(t->residual[p]));
        cJSON *st = cJSON_AddObjectToObject(po, "styles");
        for (int s = 0; s < EYE_STYLES; s++) {
            if (!t->style_valid[s]) continue;
            char key[8];
            snprintf(key, sizeof(key), "%d", s);
            cJSON *rows = cJSON_AddArrayToObject(st, key);
            for (int i = 0; i < t->speed_n[p]; i++) {
                cJSON *row = cJSON_CreateArray();
                for (int k = 0; k < EYE_PITCH_N; k++) {
                    vec3 v = t->off[p][s][i][k];
                    cJSON *a = cJSON_CreateArray();
                    cJSON_AddItemToArray(a, cJSON_CreateNumber(r4(v.x)));
                    cJSON_AddItemToArray(a, cJSON_CreateNumber(r4(v.y)));
                    cJSON_AddItemToArray(a, cJSON_CreateNumber(r4(v.z)));
                    cJSON_AddItemToArray(row, a);
                }
                cJSON_AddItemToArray(rows, row);
            }
        }
    }
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}

// Piecewise linear sample index / weight over an ascending list.
static void seg(const float *x, int n, float v, int *i0, float *u) {
    if (n <= 1 || v <= x[0]) { *i0 = 0; *u = 0; return; }
    for (int i = 0; i + 1 < n; i++)
        if (v <= x[i + 1]) {
            float d = x[i + 1] - x[i];
            *i0 = i;
            *u = d > 1e-6f ? (v - x[i]) / d : 0.0f;
            return;
        }
    *i0 = n - 2;
    *u = 1.0f;
}

vec3 eye_table_sample(const EyeNominalTable *t, int posture, int style, float speed, float pitch_rad) {
    posture = DG_CLAMP(posture, 0, EYE_POSTURES - 1);
    if (style < 0 || style >= EYE_STYLES || !t->style_valid[style]) style = 0;
    int si, pi;
    float su, pu;
    seg(t->speed[posture], t->speed_n[posture], speed, &si, &su);
    seg(t->pitch, EYE_PITCH_N, pitch_rad * DG_RAD2DEG, &pi, &pu);
    int s1 = t->speed_n[posture] > 1 ? si + 1 : si;
    const vec3 (*o)[EYE_PITCH_N] = t->off[posture][style];
    vec3 a = v3_lerp(o[si][pi], o[si][pi + 1], pu), b = v3_lerp(o[s1][pi], o[s1][pi + 1], pu);
    return v3_lerp(a, b, su);
}

CamGlideParams cam_glide_params_default(void) {
    CamGlideParams p = { 150.0f, 24.5f };   // critically damped: damping = 2 sqrt(stiffness), ~0.4 s to settle
    return p;
}

void cam_glide_reset(CamGlide *g) { memset(g, 0, sizeof(*g)); }

// Critically damped (with the defaults) spring towards `target`, sub-stepped at 240 Hz.
static void spring(float *x, float *v, float target, float dt, const CamGlideParams *p) {
    int n = (int)ceilf(dt * 240.0f);
    if (n < 1) n = 1;
    float h = dt / (float)n;
    for (int i = 0; i < n; i++) {
        float a = (target - *x) * p->stiffness - *v * p->damping;
        *v += a * h;
        *x += *v * h;
    }
}

vec3 cam_glide_offset(CamGlide *g, const EyeNominalTable *t, const CamNominalIn *in, float dt, const CamGlideParams *p) {
    dt = clampf(dt, 0.0f, 0.1f);
    float tw[EYE_POSTURES] = { 0 };
    if (in->posture < 0 || in->posture >= EYE_POSTURES) {
        float c = saturate(in->crouch);
        tw[EYE_STAND] = 1.0f - c;
        tw[EYE_CROUCH] = c;
    } else {
        tw[in->posture] = 1.0f;
    }
    int style = DG_CLAMP(in->style, 0, EYE_STYLES - 1);
    if (!g->init) {
        memcpy(g->w, tw, sizeof(tw));
        memset(g->wv, 0, sizeof(g->wv));
        g->speed = in->speed;
        g->speed_v = 0;
        g->style_from = g->style_to = style;
        g->style_w = 1.0f;
        g->style_wv = 0;
        g->init = true;
    }
    if (style != g->style_to) {
        g->style_from = g->style_w >= 0.5f ? g->style_to : g->style_from;
        g->style_to = style;
        g->style_w = 0.0f;
        g->style_wv = 0.0f;
    }
    for (int k = 0; k < EYE_POSTURES; k++) spring(&g->w[k], &g->wv[k], tw[k], dt, p);
    spring(&g->speed, &g->speed_v, in->speed, dt, p);
    spring(&g->style_w, &g->style_wv, 1.0f, dt, p);
    vec3 o = v3(0, 0, 0);
    float sum = 0.0f, sw = saturate(g->style_w);
    for (int k = 0; k < EYE_POSTURES; k++) {
        float w = saturate(g->w[k]);
        if (w < 1e-4f) continue;
        float sv = k == EYE_MANTLE ? in->progress : g->speed;   // the traversal's own progress (continuous)
        vec3 a = eye_table_sample(t, k, g->style_from, sv, in->pitch), b = eye_table_sample(t, k, g->style_to, sv, in->pitch);
        o = v3_madd(o, v3_lerp(a, b, sw), w);
        sum += w;
    }
    return sum > 1e-4f ? v3_scale(o, 1.0f / sum) : eye_table_sample(t, EYE_STAND, style, in->speed, in->pitch);
}

float cam_glide_step(CamGlide *g, float y, float speed_h, bool grounded, float dt, const CamGlideParams *p) {
    dt = clampf(dt, 0.0f, 0.1f);
    float dy = y - g->last_y;
    // A grounded snap - more than a slope can climb in this frame - is a step: absorbed here and
    // glided out. Everything else (slopes, jumps, falls, lifts) follows the capsule.
    if (g->step_init && g->last_grounded && grounded && fabsf(dy) > speed_h * dt + 0.02f && fabsf(dy) < 0.8f) g->step -= dy;
    if (fabsf(g->step) > 0.8f || fabsf(dy) >= 0.8f) g->step = g->step_v = 0.0f;   // a teleport
    spring(&g->step, &g->step_v, 0.0f, dt, p);
    g->last_y = y;
    g->last_grounded = grounded;
    g->step_init = true;
    return g->step;
}

void cam_anchor_add(CamAnchorStats *s, float d, float radius, int posture) {
    s->frames++;
    s->sum_d += d;
    s->max_d = fmaxf(s->max_d, d);
    if (d > radius) s->over++;
    posture = DG_CLAMP(posture, 0, EYE_POSTURES - 1);
    s->max_posture[posture] = fmaxf(s->max_posture[posture], d);
}
