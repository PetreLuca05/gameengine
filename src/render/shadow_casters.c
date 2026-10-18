#include "shadow_casters.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *k_class_names[SCC_COUNT] = { "none", "ground", "small", "tree" };

const char *scc_class_name(int c) { return c >= 0 && c < SCC_COUNT ? k_class_names[c] : "?"; }

int scc_class_from_name(const char *s) {
    for (int c = 0; s && c < SCC_COUNT; c++)
        if (!strcmp(s, k_class_names[c])) return c;
    return -1;
}

// ground < small < tree < none (unclassified: never limited)
static int rank(int c) { return c == SCC_GROUND ? 1 : c == SCC_SMALL ? 2 : c == SCC_TREE ? 3 : 4; }
int scc_merge(int a, int b) { return rank(a) >= rank(b) ? a : b; }

void scc_defaults(SccTable *t) {
    memset(t, 0, sizeof(*t));
    t->fade = 0.3f;
    for (int m = 0; m < SCC_MODES; m++)
        for (int c = 0; c < SCC_COUNT; c++) t->lim[m][c] = (SccLimit){ -1, 0.0f };
    // 1 (High / Modern, 4 cascades): ground cover in cascade 0 (to ~10 m), small plants in 0 - 1 (~30 m)
    t->lim[1][SCC_GROUND] = (SccLimit){ 0, 0.0f };
    t->lim[1][SCC_SMALL] = (SccLimit){ 1, 0.0f };
    // 2 (Off .. Medium, 1 - 2 cascades: cascade 0 reaches 37.5 / 150 m): cascade 0 and a cap, so the
    // reach in metres stays close to the High / Modern one
    t->lim[2][SCC_GROUND] = (SccLimit){ 0, 15.0f };
    t->lim[2][SCC_SMALL] = (SccLimit){ 0, 30.0f };
}

static bool set_err(char *err, int n, const char *msg, const char *what) {
    if (err && n > 0) snprintf(err, (size_t)n, "%s%s%s", msg, what ? ": " : "", what ? what : "");
    return false;
}

bool scc_parse(SccTable *t, const char *json, char *err, int err_size) {
    scc_defaults(t);
    cJSON *root = cJSON_Parse(json);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return set_err(err, err_size, "not a JSON object", NULL);
    }
    bool ok = true;
    const cJSON *f = cJSON_GetObjectItemCaseSensitive(root, "fade");
    if (f) {
        if (!cJSON_IsNumber(f) || f->valuedouble < 0.0 || f->valuedouble > 1.0) ok = set_err(err, err_size, "fade must be 0 .. 1", NULL);
        else t->fade = (float)f->valuedouble;
    }
    const cJSON *modes = cJSON_GetObjectItemCaseSensitive(root, "modes"), *m;
    cJSON_ArrayForEach(m, modes) {
        if (!ok) break;
        char *end = NULL;
        long mi = strtol(m->string, &end, 10);
        if (!end || *end || mi < 1 || mi >= SCC_MODES || !cJSON_IsObject(m)) { ok = set_err(err, err_size, "unknown mode", m->string); break; }
        const cJSON *c;
        cJSON_ArrayForEach(c, m) {
            if (c->string[0] == '_') continue;
            int ci = scc_class_from_name(c->string);
            if (ci < 0 || !cJSON_IsObject(c)) { ok = set_err(err, err_size, "unknown class", c->string); break; }
            const cJSON *k;
            cJSON_ArrayForEach(k, c) {
                if (k->string[0] == '_') continue;
                if (!cJSON_IsNumber(k)) { ok = set_err(err, err_size, "not a number", k->string); break; }
                if (!strcmp(k->string, "cascade")) {
                    int v = (int)k->valuedouble;
                    if (v < -1 || v >= SCC_CASCADES) { ok = set_err(err, err_size, "cascade must be -1 .. 3", c->string); break; }
                    t->lim[mi][ci].cascade = v;
                } else if (!strcmp(k->string, "max_m")) {
                    if (k->valuedouble < 0.0) { ok = set_err(err, err_size, "max_m must be >= 0", c->string); break; }
                    t->lim[mi][ci].max_m = (float)k->valuedouble;
                } else {
                    ok = set_err(err, err_size, "unknown key", k->string);
                    break;
                }
            }
            if (!ok) break;
        }
    }
    cJSON_Delete(root);
    if (!ok) scc_defaults(t);
    return ok;
}

void scc_view(const SccTable *t, int mode, int k, int ncasc, const float *splits, bool wide, float border, SccView *out) {
    memset(out, 0, sizeof(*out));
    out->allow = (1u << SCC_COUNT) - 1u;
    if (mode <= 0 || mode >= SCC_MODES || ncasc < 1 || !splits) return;
    int last = ncasc - 1;
    wide = wide && ncasc >= 2;
    // Where cascade k's receivers start (lighting.glsl shadow_world: its range and the blend band from
    // k - 1; in wide mode cascade 1 also serves the near field).
    float start = (k == 0 || (wide && k == 1)) ? 0.0f : splits[k - 1] * (1.0f - border);
    for (int c = 0; c < SCC_COUNT; c++) {
        SccLimit L = t->lim[mode][c];
        int lastc = last;
        float end = 0.0f;
        if (L.cascade >= 0 && L.cascade < last) {
            lastc = L.cascade + (wide ? 1 : 0);
            if (lastc > last) lastc = last;
            end = splits[L.cascade];
        }
        if (L.max_m > 0.0f) end = end > 0.0f ? fminf(end, L.max_m) : L.max_m;
        if (k > lastc || (end > 0.0f && end <= start)) {
            out->allow &= ~(1u << c);
            continue;
        }
        out->end_m[c] = end;
        out->fade_m[c] = end * t->fade;
    }
}

float scc_alpha(const SccView *v, int c, float d) {
    if (c < 0 || c >= SCC_COUNT || !(v->allow >> c & 1u)) return 0.0f;
    float e = v->end_m[c];
    if (e <= 0.0f) return 1.0f;
    if (d >= e) return 0.0f;
    float f = v->fade_m[c];
    if (f <= 0.0f) return 1.0f;
    float a = (e - d) / f;
    return a < 1.0f ? a : 1.0f;
}

float scc_mask_end(const SccView *v, u32 mask) {
    float best = -1.0f;
    for (int c = 0; c < SCC_COUNT; c++) {
        if (!(mask >> c & 1u) || !(v->allow >> c & 1u)) continue;
        if (v->end_m[c] <= 0.0f) return 0.0f;
        best = fmaxf(best, v->end_m[c]);
    }
    return best;
}
