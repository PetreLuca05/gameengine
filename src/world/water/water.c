// Water bodies, presets and the world-wide queries (water.h).
#include "water.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define SHORE_DEPTH 3.0f            // m of still depth over which the swell fades out towards the beach

static const char *k_kind_names[WATER_KIND_COUNT] = { "puddle", "lake", "ocean", "river" };

const char *water_kind_name(WaterKind k) { return (unsigned)k < WATER_KIND_COUNT ? k_kind_names[k] : "?"; }

int water_kind_parse(const char *s) {
    if (!s) return -1;
    for (int i = 0; i < WATER_KIND_COUNT; i++)
        if (strcmp(s, k_kind_names[i]) == 0) return i;
    return -1;
}

static void set3(float *d, float a, float b, float c) { d[0] = a; d[1] = b; d[2] = c; }

WaterParams water_params_default(WaterKind kind) {
    WaterParams p;
    memset(&p, 0, sizeof(p));
    snprintf(p.preset, sizeof(p.preset), "%s", water_kind_name(kind));
    // Clear fresh water: red goes in a few metres, blue lasts tens.
    set3(p.absorb, 0.45f, 0.09f, 0.06f);
    set3(p.scatter, 0.004f, 0.012f, 0.016f);
    p.turbidity = 0.15f;
    p.roughness = 0.04f;
    p.wave_scale = 1.0f;
    p.wind_coupling = 1.0f;
    p.choppiness = 0.5f;
    p.fetch_km = 30.0f;
    p.foam = 0.5f;
    p.temperature = 14.0f;
    p.density = WATER_DENSITY_FRESH;
    switch (kind) {
    case WATER_OCEAN:
        set3(p.absorb, 0.40f, 0.07f, 0.045f);
        set3(p.scatter, 0.003f, 0.016f, 0.018f);
        p.roughness = 0.06f;
        p.choppiness = 0.65f;
        p.foam = 1.0f;
        p.density = WATER_DENSITY_SEA;
        break;
    case WATER_LAKE:
        p.wave_scale = 0.35f;        // ripple strength (lakes have no swell)
        p.turbidity = 0.25f;
        break;
    case WATER_RIVER:
        set3(p.absorb, 0.5f, 0.16f, 0.14f);
        p.turbidity = 0.45f;
        p.flow_speed = 1.2f;
        break;
    case WATER_PUDDLE:
        set3(p.absorb, 1.2f, 0.9f, 0.8f);
        p.roughness = 0.03f;
        p.wave_scale = 0.0f;
        p.foam = 0.0f;
        break;
    default: break;
    }
    return p;
}

static void jf(const cJSON *o, const char *k, float *v) {
    const cJSON *e = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsNumber(e)) *v = (float)e->valuedouble;
}
static void jf3(const cJSON *o, const char *k, float *v) {
    const cJSON *e = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsArray(e) && cJSON_GetArraySize(e) == 3)
        for (int i = 0; i < 3; i++) v[i] = (float)cJSON_GetArrayItem(e, i)->valuedouble;
    else if (cJSON_IsNumber(e)) v[0] = v[1] = v[2] = (float)e->valuedouble;
}

static void params_from_obj(WaterParams *p, const cJSON *o) {
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(o, "preset");
    if (cJSON_IsString(name)) snprintf(p->preset, sizeof(p->preset), "%s", name->valuestring);
    jf3(o, "absorb", p->absorb);
    jf3(o, "scatter", p->scatter);
    jf(o, "turbidity", &p->turbidity);
    jf(o, "roughness", &p->roughness);
    jf(o, "wave_scale", &p->wave_scale);
    jf(o, "wind_coupling", &p->wind_coupling);
    jf(o, "choppiness", &p->choppiness);
    jf(o, "fetch_km", &p->fetch_km);
    jf(o, "foam", &p->foam);
    jf(o, "flow_speed", &p->flow_speed);
    jf(o, "temperature", &p->temperature);
    jf(o, "density", &p->density);
}

bool water_params_parse(WaterParams *p, const char *json, char *err, int err_len) {
    cJSON *root = cJSON_Parse(json);
    if (!cJSON_IsObject(root)) {
        if (err) snprintf(err, (size_t)err_len, "water preset: invalid JSON");
        cJSON_Delete(root);
        return false;
    }
    params_from_obj(p, root);
    cJSON_Delete(root);
    if (p->density < 100.0f) p->density = WATER_DENSITY_FRESH;
    return true;
}

bool water_body_from_json(WaterBody *b, const void *obj_v, const WaterParams *preset, char *err, int err_len) {
    const cJSON *o = (const cJSON *)obj_v;
    memset(b, 0, sizeof(*b));
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(o, "type");
    int kind = cJSON_IsString(type) ? water_kind_parse(type->valuestring) : -1;
    if (kind < 0 || kind == WATER_PUDDLE) {
        if (err) snprintf(err, (size_t)err_len, "water body: type must be lake / ocean / river");
        return false;
    }
    b->kind = (u8)kind;
    b->p = preset ? *preset : water_params_default((WaterKind)kind);
    params_from_obj(&b->p, o);
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(o, "name");
    snprintf(b->name, sizeof(b->name), "%s", cJSON_IsString(name) ? name->valuestring : water_kind_name((WaterKind)kind));
    jf(o, "level", &b->level);
    const cJSON *flow = cJSON_GetObjectItemCaseSensitive(o, "flow");
    if (cJSON_IsArray(flow) && cJSON_GetArraySize(flow) == 2) {
        b->flow[0] = (float)cJSON_GetArrayItem(flow, 0)->valuedouble;
        b->flow[1] = (float)cJSON_GetArrayItem(flow, 1)->valuedouble;
    }
    const cJSON *poly = cJSON_GetObjectItemCaseSensitive(o, "polygon");
    const cJSON *ell = cJSON_GetObjectItemCaseSensitive(o, "ellipse");
    const cJSON *box = cJSON_GetObjectItemCaseSensitive(o, "box");
    if (kind == WATER_OCEAN && !poly && !ell && !box) {
        b->shape = WSHAPE_INFINITE;
    } else if (cJSON_IsArray(poly)) {
        b->shape = WSHAPE_POLYGON;
        int n = cJSON_GetArraySize(poly);
        if (n < 3 || n > WATER_POLY_MAX) {
            if (err) snprintf(err, (size_t)err_len, "water body '%s': polygon needs 3..%d points", b->name, WATER_POLY_MAX);
            return false;
        }
        for (int i = 0; i < n; i++) {
            const cJSON *pt = cJSON_GetArrayItem(poly, i);
            if (!cJSON_IsArray(pt) || cJSON_GetArraySize(pt) != 2) {
                if (err) snprintf(err, (size_t)err_len, "water body '%s': polygon point %d is not [x, z]", b->name, i);
                return false;
            }
            b->poly[i][0] = (float)cJSON_GetArrayItem(pt, 0)->valuedouble;
            b->poly[i][1] = (float)cJSON_GetArrayItem(pt, 1)->valuedouble;
        }
        b->poly_n = n;
    } else if (cJSON_IsObject(box)) {
        // {"center":[x,z],"half":[hx,hz]}: an axis-aligned rectangle (pools, tanks)
        float c[2] = { 0, 0 }, h[2] = { 1, 1 };
        const cJSON *cc = cJSON_GetObjectItemCaseSensitive(box, "center"), *hh = cJSON_GetObjectItemCaseSensitive(box, "half");
        if (cJSON_IsArray(cc) && cJSON_GetArraySize(cc) == 2) { c[0] = (float)cJSON_GetArrayItem(cc, 0)->valuedouble; c[1] = (float)cJSON_GetArrayItem(cc, 1)->valuedouble; }
        if (cJSON_IsArray(hh) && cJSON_GetArraySize(hh) == 2) { h[0] = (float)cJSON_GetArrayItem(hh, 0)->valuedouble; h[1] = (float)cJSON_GetArrayItem(hh, 1)->valuedouble; }
        b->shape = WSHAPE_POLYGON;
        b->poly_n = 4;
        float sx[4] = { -1, 1, 1, -1 }, sz[4] = { -1, -1, 1, 1 };
        for (int i = 0; i < 4; i++) { b->poly[i][0] = c[0] + sx[i] * h[0]; b->poly[i][1] = c[1] + sz[i] * h[1]; }
    } else if (cJSON_IsObject(ell)) {
        b->shape = WSHAPE_ELLIPSE;
        const cJSON *cc = cJSON_GetObjectItemCaseSensitive(ell, "center"), *rr = cJSON_GetObjectItemCaseSensitive(ell, "radius");
        if (cJSON_IsArray(cc) && cJSON_GetArraySize(cc) == 2) { b->center[0] = (float)cJSON_GetArrayItem(cc, 0)->valuedouble; b->center[1] = (float)cJSON_GetArrayItem(cc, 1)->valuedouble; }
        if (cJSON_IsArray(rr) && cJSON_GetArraySize(rr) == 2) { b->radius[0] = (float)cJSON_GetArrayItem(rr, 0)->valuedouble; b->radius[1] = (float)cJSON_GetArrayItem(rr, 1)->valuedouble; }
        else if (cJSON_IsNumber(rr)) b->radius[0] = b->radius[1] = (float)rr->valuedouble;
        jf(ell, "yaw", &b->yaw);
        if (b->radius[0] <= 0.0f || b->radius[1] <= 0.0f) {
            if (err) snprintf(err, (size_t)err_len, "water body '%s': ellipse radius must be > 0", b->name);
            return false;
        }
    } else {
        if (err) snprintf(err, (size_t)err_len, "water body '%s': needs polygon, box or ellipse", b->name);
        return false;
    }
    if (b->p.density < 100.0f) b->p.density = kind == WATER_OCEAN ? WATER_DENSITY_SEA : WATER_DENSITY_FRESH;
    water_body_bounds(b);
    return true;
}

// ------------------------------------------------------------------ world

void water_world_init(WaterWorld *w, u32 seed) {
    memset(w, 0, sizeof(*w));
    waves_init(&w->waves, seed);
    w->ocean = -1;
    w->wind_dir_deg = 0.0f;
    WaveParams p = wave_params_default();
    waves_set_params(&w->waves, &p);
}

void water_world_clear(WaterWorld *w) {
    w->count = 0;
    w->ocean = -1;
}

void water_set_ground(WaterWorld *w, WaterGroundFn fn, void *user) {
    w->ground = fn;
    w->ground_user = user;
}

WaveParams water_ocean_wave_params(const WaterWorld *w) {
    WaveParams p = wave_params_default();
    p.wind_dir_deg = w->wind_dir_deg;
    if (w->ocean >= 0) {
        const WaterParams *bp = &w->body[w->ocean].p;
        p.wind = w->wind * bp->wind_coupling;
        p.scale = bp->wave_scale;
        p.choppiness = bp->choppiness;
        p.fetch_km = bp->fetch_km;
    } else {
        p.wind = w->wind;
        p.scale = 0.0f;
    }
    return p;
}

void water_set_wind(WaterWorld *w, float wind, float dir_deg) {
    w->wind = wind;
    w->wind_dir_deg = dir_deg;
    WaveParams p = water_ocean_wave_params(w);
    waves_set_params(&w->waves, &p);
}

void water_body_bounds(WaterBody *b) {
    switch (b->shape) {
    case WSHAPE_POLYGON:
        b->bmin[0] = b->bmax[0] = b->poly[0][0];
        b->bmin[1] = b->bmax[1] = b->poly[0][1];
        for (int i = 1; i < b->poly_n; i++) {
            b->bmin[0] = fminf(b->bmin[0], b->poly[i][0]); b->bmax[0] = fmaxf(b->bmax[0], b->poly[i][0]);
            b->bmin[1] = fminf(b->bmin[1], b->poly[i][1]); b->bmax[1] = fmaxf(b->bmax[1], b->poly[i][1]);
        }
        break;
    case WSHAPE_ELLIPSE: {
        float r = fmaxf(b->radius[0], b->radius[1]);
        b->bmin[0] = b->center[0] - r; b->bmax[0] = b->center[0] + r;
        b->bmin[1] = b->center[1] - r; b->bmax[1] = b->center[1] + r;
        break;
    }
    case WSHAPE_CALLBACK: break;      // the creator sets the bounds
    default:
        b->bmin[0] = b->bmin[1] = -1e30f;
        b->bmax[0] = b->bmax[1] = 1e30f;
        break;
    }
}

int water_add_body(WaterWorld *w, const WaterBody *b) {
    if (w->count >= WATER_MAX_BODIES) return -1;
    int i = w->count++;
    w->body[i] = *b;
    if (b->shape != WSHAPE_CALLBACK) water_body_bounds(&w->body[i]);
    if (b->kind == WATER_OCEAN && w->ocean < 0) {
        w->ocean = i;
        water_set_wind(w, w->wind, w->wind_dir_deg);
    }
    return i;
}

bool water_poly_inside(const float (*poly)[2], int n, float x, float z) {
    bool in = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        float xi = poly[i][0], zi = poly[i][1], xj = poly[j][0], zj = poly[j][1];
        if ((zi > z) != (zj > z) && x < (xj - xi) * (z - zi) / (zj - zi) + xi) in = !in;
    }
    return in;
}

static bool body_contains(const WaterBody *b, float x, float z, float *level, float flow[2]) {
    if (b->shape != WSHAPE_INFINITE && (x < b->bmin[0] || x > b->bmax[0] || z < b->bmin[1] || z > b->bmax[1])) return false;
    *level = b->level;
    flow[0] = b->flow[0];
    flow[1] = b->flow[1];
    switch (b->shape) {
    case WSHAPE_INFINITE: return true;
    case WSHAPE_POLYGON: return water_poly_inside((const float (*)[2])b->poly, b->poly_n, x, z);
    case WSHAPE_ELLIPSE: {
        float c, s;
        wsincos(-b->yaw * 0.0174532925f, &s, &c);
        float dx = x - b->center[0], dz = z - b->center[1];
        float lx = dx * c - dz * s, lz = dx * s + dz * c;
        float ex = lx / b->radius[0], ez = lz / b->radius[1];
        return ex * ex + ez * ez <= 1.0f;
    }
    case WSHAPE_CALLBACK: return b->fn && b->fn(b->user, b, x, z, level, flow);
    default: return false;
    }
}

int water_body_at(const WaterWorld *w, float x, float z, float *level_out, float flow_out[2]) {
    int best = -1;
    float best_level = WATER_NONE, best_flow[2] = { 0, 0 };
    for (int i = 0; i < w->count; i++) {
        const WaterBody *b = &w->body[i];
        if (b->kind == WATER_PUDDLE) continue;
        float lv, fl[2] = { 0, 0 };
        if (!body_contains(b, x, z, &lv, fl)) continue;
        if (lv > best_level) { best_level = lv; best = i; best_flow[0] = fl[0]; best_flow[1] = fl[1]; }
    }
    if (level_out) *level_out = best_level;
    if (flow_out) { flow_out[0] = best_flow[0]; flow_out[1] = best_flow[1]; }
    return best;
}

// Swell fade towards the beach (identical on the GPU: the water tile holds the same ground height).
static float shore_fade(float still_depth) {
    float t = still_depth / SHORE_DEPTH;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
}

bool water_sample(const WaterWorld *w, float x, float z, float t, WaterSample *s) {
    memset(s, 0, sizeof(*s));
    s->normal[1] = 1.0f;
    s->jacobian = 1.0f;
    s->ground = w->ground ? w->ground(w->ground_user, x, z) : WATER_NONE;
    float level, flow[2];
    int bi = water_body_at(w, x, z, &level, flow);
    s->body = bi;
    if (bi < 0) {
        s->height = WATER_NONE;
        s->level = WATER_NONE;
        return false;
    }
    const WaterBody *b = &w->body[bi];
    s->kind = b->kind;
    s->level = level;
    s->height = level;
    s->vel[0] = flow[0];
    s->vel[2] = flow[1];
    if (b->kind == WATER_OCEAN && w->waves.active_n > 0) {
        float fade = w->ground ? shore_fade(level - s->ground) : 1.0f;
        if (fade > 0.0f) {
            WaveSample ws;
            waves_sample(&w->waves, x, z, t, &ws);
            s->height += ws.height * fade;
            for (int k = 0; k < 3; k++) s->vel[k] += ws.vel[k] * fade;
            s->normal[0] = ws.normal[0] * fade;
            s->normal[1] = 1.0f + (ws.normal[1] - 1.0f) * fade;
            s->normal[2] = ws.normal[2] * fade;
            float l = sqrtf(s->normal[0] * s->normal[0] + s->normal[1] * s->normal[1] + s->normal[2] * s->normal[2]);
            for (int k = 0; k < 3; k++) s->normal[k] /= l;
            s->jacobian = 1.0f + (ws.jacobian - 1.0f) * fade;
        }
    }
    s->depth = w->ground ? s->height - s->ground : 1e9f;
    if (s->depth <= 0.0f) {
        // Dry: the body's footprint reaches over a beach / bank above the surface.
        s->depth = 0.0f;
        s->height = WATER_NONE;
        s->body = -1;
        return false;
    }
    return true;
}

bool water_patch(const WaterWorld *w, float x, float z, float t, float e, WaterPatch *p) {
    memset(p, 0, sizeof(*p));
    float level, flow[2];
    int bi = water_body_at(w, x, z, &level, flow);
    p->body = bi;
    const float ux[5] = { 0, 1, -1, 0, 0 }, uz[5] = { 0, 0, 0, 1, -1 };
    if (bi >= 0 && w->body[bi].kind == WATER_OCEAN && w->waves.active_n > 0) {
        waves_patch(&w->waves, x, z, t, e, p->x, p->z, p->h, p->vel);
        p->density = w->body[bi].p.density;
        int wet = 0;
        for (int j = 0; j < 5; j++) {
            float g = w->ground ? w->ground(w->ground_user, p->x[j], p->z[j]) : WATER_NONE;
            float fade = w->ground ? shore_fade(level - g) : 1.0f;
            p->h[j] = level + p->h[j] * fade;
            for (int k = 0; k < 3; k++) p->vel[j][k] *= fade;
            p->vel[j][0] += flow[0];
            p->vel[j][2] += flow[1];
            if (w->ground && p->h[j] <= g) p->h[j] = WATER_NONE;
            else wet++;
        }
        return wet > 0;
    }
    // Flat bodies (and the edges of every body): each point asks its own body.
    int wet = 0;
    p->density = bi >= 0 ? w->body[bi].p.density : WATER_DENSITY_FRESH;
    for (int j = 0; j < 5; j++) {
        p->x[j] = x + ux[j] * e;
        p->z[j] = z + uz[j] * e;
        float lv, fl[2];
        int b = j == 0 ? bi : water_body_at(w, p->x[j], p->z[j], &lv, fl);
        if (j == 0) { lv = level; fl[0] = flow[0]; fl[1] = flow[1]; }
        float g = w->ground ? w->ground(w->ground_user, p->x[j], p->z[j]) : WATER_NONE;
        if (b < 0 || (w->ground && lv <= g)) { p->h[j] = WATER_NONE; continue; }
        if (p->body < 0) { p->body = b; p->density = w->body[b].p.density; }
        p->h[j] = lv;
        p->vel[j][0] = fl[0];
        p->vel[j][2] = fl[1];
        wet++;
    }
    return wet > 0;
}

bool water_patch_plane(const WaterPatch *p, float x, float z, float *h, float *gx, float *gz, float vel[3]) {
    // Least squares h = a + b (x - mx) + c (z - mz) over the wet points.
    float mx = 0, mz = 0, mh = 0;
    int n = 0;
    vel[0] = vel[1] = vel[2] = 0.0f;
    for (int j = 0; j < 5; j++) {
        if (p->h[j] == WATER_NONE) continue;
        mx += p->x[j]; mz += p->z[j]; mh += p->h[j];
        for (int k = 0; k < 3; k++) vel[k] += p->vel[j][k];
        n++;
    }
    if (!n) return false;
    float inv = 1.0f / (float)n;
    mx *= inv; mz *= inv; mh *= inv;
    for (int k = 0; k < 3; k++) vel[k] *= inv;
    float sxx = 0, sxz = 0, szz = 0, sxh = 0, szh = 0;
    for (int j = 0; j < 5; j++) {
        if (p->h[j] == WATER_NONE) continue;
        float dx = p->x[j] - mx, dz = p->z[j] - mz, dh = p->h[j] - mh;
        sxx += dx * dx; sxz += dx * dz; szz += dz * dz; sxh += dx * dh; szh += dz * dh;
    }
    float det = sxx * szz - sxz * sxz;
    float b = 0.0f, c = 0.0f;
    if (fabsf(det) > 1e-6f) {
        b = (sxh * szz - szh * sxz) / det;
        c = (szh * sxx - sxh * sxz) / det;
    } else if (sxx > 1e-6f) {
        b = sxh / sxx;
    } else if (szz > 1e-6f) {
        c = szh / szz;
    }
    *gx = b;
    *gz = c;
    *h = mh + b * (x - mx) + c * (z - mz);
    return true;
}

float water_height_at(const WaterWorld *w, float x, float z, float t) {
    float level, flow[2];
    int bi = water_body_at(w, x, z, &level, flow);
    if (bi < 0) return WATER_NONE;
    const WaterBody *b = &w->body[bi];
    float ground = w->ground ? w->ground(w->ground_user, x, z) : WATER_NONE;
    float h = level;
    if (b->kind == WATER_OCEAN && w->waves.active_n > 0) {
        float fade = w->ground ? shore_fade(level - ground) : 1.0f;
        if (fade > 0.0f) h += waves_height(&w->waves, x, z, t, 2) * fade;
    }
    if (w->ground && h <= ground) return WATER_NONE;
    return h;
}

float water_depth_at(const WaterWorld *w, float x, float z, float t) {
    float h = water_height_at(w, x, z, t);
    if (h == WATER_NONE) return 0.0f;
    if (!w->ground) return 1e9f;
    float d = h - w->ground(w->ground_user, x, z);
    return d > 0.0f ? d : 0.0f;
}
