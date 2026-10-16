#include "mapgen.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NO_WATER (-1e9f)

static float noise2(const fnl_state *s, float x, float z) { return fnlGetNoise2D((fnl_state *)s, x, z); }

// ------------------------------------------------------------------ small geometry

// Distance from p to segment ab, *t = parameter of the closest point.
static float seg_dist(vec2 p, vec2 a, vec2 b, float *t) {
    vec2 ab = v2_sub(b, a), ap = v2_sub(p, a);
    float l2 = ab.x * ab.x + ab.y * ab.y;
    float u = l2 > 1e-9f ? clampf((ap.x * ab.x + ap.y * ab.y) / l2, 0.0f, 1.0f) : 0.0f;
    if (t) *t = u;
    vec2 c = v2_add(a, v2_scale(ab, u));
    return v2_len(v2_sub(p, c));
}

// Point in a rotated rectangle's frame: returns the outside distance (0 inside), *inside depth.
static float rect_dist(vec2 p, vec2 c, vec2 half, float yaw) {
    float cs = cosf(yaw), sn = sinf(yaw), dx = p.x - c.x, dz = p.y - c.y;
    float lx = fabsf(dx * cs - dz * sn) - half.x, lz = fabsf(dx * sn + dz * cs) - half.y;
    float ox = fmaxf(lx, 0.0f), oz = fmaxf(lz, 0.0f);
    return sqrtf(ox * ox + oz * oz);
}

static float ellipse_d(vec2 p, vec2 c, vec2 r, float yaw) {
    float cs = cosf(yaw), sn = sinf(yaw), dx = p.x - c.x, dz = p.y - c.y;
    float lx = (dx * cs - dz * sn) / fmaxf(r.x, 1e-3f), lz = (dx * sn + dz * cs) / fmaxf(r.y, 1e-3f);
    return sqrtf(lx * lx + lz * lz);
}

static float poly_signed_dist(const vec2 *poly, int n, vec2 p) {
    float dmin = 1e30f;
    bool inside = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        vec2 a = poly[j], b = poly[i];
        dmin = fminf(dmin, seg_dist(p, a, b, NULL));
        if (((b.y > p.y) != (a.y > p.y)) && (p.x < (a.x - b.x) * (p.y - b.y) / (a.y - b.y) + b.x)) inside = !inside;
    }
    return inside ? dmin : -dmin;
}

// ------------------------------------------------------------------ grids

static void grid_free(MgGrid *g) {
    free(g->start);
    free(g->items);
    memset(g, 0, sizeof(*g));
}

// Builds a grid of items with bounding boxes [lo, hi] (count items).
static void grid_build(MgGrid *g, float origin, float extent, int count, void (*bounds)(const void *ctx, int i, vec2 *lo, vec2 *hi), const void *ctx) {
    grid_free(g);
    g->origin = origin;
    g->n = (int)ceilf(extent / MG_GRID_CELL);
    int cells = g->n * g->n;
    int *cnt = (int *)calloc((size_t)cells + 1, sizeof(int));
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) {
            g->start = (int *)calloc((size_t)cells + 1, sizeof(int));
            for (int c = 0; c < cells; c++) g->start[c + 1] = g->start[c] + cnt[c];
            g->items = (int *)malloc(sizeof(int) * (size_t)(g->start[cells] + 1));
            memset(cnt, 0, sizeof(int) * (size_t)cells);
        }
        for (int i = 0; i < count; i++) {
            vec2 lo, hi;
            bounds(ctx, i, &lo, &hi);
            int x0 = (int)floorf((lo.x - origin) / MG_GRID_CELL), x1 = (int)floorf((hi.x - origin) / MG_GRID_CELL);
            int z0 = (int)floorf((lo.y - origin) / MG_GRID_CELL), z1 = (int)floorf((hi.y - origin) / MG_GRID_CELL);
            x0 = DG_MAX(x0, 0), z0 = DG_MAX(z0, 0), x1 = DG_MIN(x1, g->n - 1), z1 = DG_MIN(z1, g->n - 1);
            for (int z = z0; z <= z1; z++)
                for (int x = x0; x <= x1; x++) {
                    int c = z * g->n + x;
                    if (pass == 0) cnt[c]++;
                    else g->items[g->start[c] + cnt[c]++] = i;
                }
        }
    }
    free(cnt);
}

static int grid_cell(const MgGrid *g, float x, float z) {
    if (!g->n) return -1;
    int cx = (int)floorf((x - g->origin) / MG_GRID_CELL), cz = (int)floorf((z - g->origin) / MG_GRID_CELL);
    if (cx < 0 || cz < 0 || cx >= g->n || cz >= g->n) return -1;
    return cz * g->n + cx;
}

// ------------------------------------------------------------------ init / description

void mapgen_init(MapGen *g, u32 seed) {
    memset(g, 0, sizeof(*g));
    g->seed = seed;
    g->half_size = 1000.0f;
    g->raster_half = 1024.0f;
    g->base_height = 14.0f;
    g->hill_amp = 6.0f;
    g->hill_freq = 0.0022f;
    g->sea_floor = -14.0f;
    g->beach_width = 60.0f;
    g->sea_noise = 15.0f;
    g->n_hills = fnlCreateState();
    g->n_hills.seed = (int)seed;
    g->n_hills.noise_type = FNL_NOISE_OPENSIMPLEX2;
    g->n_hills.fractal_type = FNL_FRACTAL_FBM;
    g->n_hills.octaves = 5;
    g->n_ridge = fnlCreateState();
    g->n_ridge.seed = (int)seed + 101;
    g->n_ridge.noise_type = FNL_NOISE_OPENSIMPLEX2;
    g->n_ridge.fractal_type = FNL_FRACTAL_FBM;
    g->n_ridge.octaves = 4;
    g->n_ridge.frequency = 0.004f;
    g->n_detail = fnlCreateState();
    g->n_detail.seed = (int)seed + 202;
    g->n_detail.frequency = 0.06f;
    g->n_shore = fnlCreateState();
    g->n_shore.seed = (int)seed + 303;
    g->n_shore.fractal_type = FNL_FRACTAL_FBM;
    g->n_shore.octaves = 3;
    g->n_shore.frequency = 0.012f;
}

void mapgen_free(MapGen *g) {
    free(g->road_ctrl);
    free(g->road_ctrl_first);
    free(g->road_ctrl_count);
    free(g->road_smooth);
    free(g->rs);
    free(g->vs);
    free(g->keep);
    grid_free(&g->road_grid);
    grid_free(&g->river_grid);
    grid_free(&g->keep_grid);
    grid_free(&g->feature_grid);
    memset(g, 0, sizeof(*g));
}

static const char *const k_mat_names[MGM_COUNT] = { "grass", "meadow", "forest", "dirt", "rock", "sand", "gravel", "asphalt", "dirt_road", "concrete", "field", "crop", "plowed", "mud" };
const char *mapgen_material_name(int mat) { return mat >= 0 && mat < MGM_COUNT ? k_mat_names[mat] : "?"; }
static int material_from_name(const char *s) {
    if (!s) return -1;
    for (int i = 0; i < MGM_COUNT; i++) if (!strcmp(s, k_mat_names[i])) return i;
    return -1;
}

static const char *const k_road_names[MGR_CLASS_COUNT] = { "main", "secondary", "street", "dirt", "trail", "runway" };
int mapgen_road_class(const char *name) {
    for (int i = 0; i < MGR_CLASS_COUNT; i++) if (name && !strcmp(name, k_road_names[i])) return i;
    return -1;
}
float mapgen_road_default_width(int cls) {
    static const float w[MGR_CLASS_COUNT] = { 9.0f, 7.5f, 8.0f, 4.5f, 2.5f, 40.0f };
    return cls >= 0 && cls < MGR_CLASS_COUNT ? w[cls] : 6.0f;
}
static float road_shoulder(int cls) {
    static const float s[MGR_CLASS_COUNT] = { 10.0f, 9.0f, 4.0f, 6.0f, 3.0f, 25.0f };
    return s[cls];
}
static float road_grade(int cls) {   // max rise per metre
    static const float gr[MGR_CLASS_COUNT] = { 0.08f, 0.10f, 0.12f, 0.16f, 0.30f, 0.01f };
    return gr[cls];
}
static float road_smooth_m(int cls) {   // half window of the profile smoothing (m)
    static const float sm[MGR_CLASS_COUNT] = { 24.0f, 18.0f, 8.0f, 10.0f, 4.0f, 60.0f };
    return sm[cls];
}

int mapgen_add_feature(MapGen *g, const MgFeature *f) {
    if (g->feature_count >= MG_MAX_FEATURES) return -1;
    g->features[g->feature_count] = *f;
    return g->feature_count++;
}

int mapgen_add_road(MapGen *g, int cls, float width, const vec2 *pts, int n, bool smooth, bool loop, const char *name) {
    if (g->road_count >= MG_MAX_ROADS || n < 2 || cls < 0 || cls >= MGR_CLASS_COUNT) return -1;
    if (!g->road_ctrl_first) {
        g->road_ctrl_first = (int *)calloc(MG_MAX_ROADS, sizeof(int));
        g->road_ctrl_count = (int *)calloc(MG_MAX_ROADS, sizeof(int));
        g->road_smooth = (bool *)calloc(MG_MAX_ROADS, sizeof(bool));
    }
    if (g->road_ctrl_total + n > g->road_ctrl_cap) {
        g->road_ctrl_cap = DG_MAX(g->road_ctrl_cap * 2, g->road_ctrl_total + n + 256);
        g->road_ctrl = (vec2 *)realloc(g->road_ctrl, sizeof(vec2) * (size_t)g->road_ctrl_cap);
    }
    int r = g->road_count++;
    MgRoad *rd = &g->roads[r];
    memset(rd, 0, sizeof(*rd));
    rd->cls = cls;
    rd->width = width > 0 ? width : mapgen_road_default_width(cls);
    rd->shoulder = road_shoulder(cls);
    rd->loop = loop;
    rd->keep_level = cls == MGR_STREET;
    dg_strcpy(rd->name, sizeof(rd->name), name ? name : k_road_names[cls]);
    g->road_ctrl_first[r] = g->road_ctrl_total;
    g->road_ctrl_count[r] = n;
    g->road_smooth[r] = smooth;
    memcpy(g->road_ctrl + g->road_ctrl_total, pts, sizeof(vec2) * (size_t)n);
    g->road_ctrl_total += n;
    return r;
}

void mapgen_add_keepout(MapGen *g, vec2 c, vec2 half, float yaw, int kind) {
    if (g->keep_count == g->keep_cap) {
        g->keep_cap = g->keep_cap ? g->keep_cap * 2 : 512;
        g->keep = (MgKeepout *)realloc(g->keep, sizeof(MgKeepout) * (size_t)g->keep_cap);
    }
    g->keep[g->keep_count++] = (MgKeepout){ c, half, yaw, (u8)kind };
}

static vec2 jv2(const cJSON *a) {
    if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) < 2) return v2(0, 0);
    return v2((float)cJSON_GetArrayItem(a, 0)->valuedouble, (float)cJSON_GetArrayItem(a, 1)->valuedouble);
}
static float jnum(const cJSON *o, const char *k, float def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}
// "radius": r | [rx, rz]; "size": [w, d] (full) -> half extents.
static vec2 jradius(const cJSON *o, const char *k, vec2 def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsNumber(v)) return v2((float)v->valuedouble, (float)v->valuedouble);
    if (cJSON_IsArray(v)) return jv2(v);
    return def;
}
static int jpoints(const cJSON *arr, vec2 *out, int max) {
    int n = 0;
    const cJSON *p;
    cJSON_ArrayForEach(p, arr) {
        if (n >= max) break;
        out[n++] = jv2(p);
    }
    return n;
}

static int edge_mask(const char *s) {
    int m = 0;
    for (; s && *s; s++) {
        if (*s == 'W' || *s == 'w') m |= MG_EDGE_W;
        if (*s == 'E' || *s == 'e') m |= MG_EDGE_E;
        if (*s == 'S' || *s == 's') m |= MG_EDGE_S;
        if (*s == 'N' || *s == 'n') m |= MG_EDGE_N;
    }
    return m;
}

static bool mapgen_parse_feature(MapGen *g, const cJSON *f, char *err, size_t err_size) {
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(f, "type");
    const char *type = cJSON_IsString(t) ? t->valuestring : "";
    MgFeature m;
    memset(&m, 0, sizeof(m));
    m.pos = jv2(cJSON_GetObjectItemCaseSensitive(f, "pos"));
    m.yaw = jnum(f, "yaw", 0) * DG_DEG2RAD;
    m.material = material_from_name(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(f, "material")) ? cJSON_GetObjectItemCaseSensitive(f, "material")->valuestring : NULL);
    if (!strcmp(type, "mountain") || !strcmp(type, "hill")) {
        m.kind = !strcmp(type, "mountain") ? MGF_MOUNTAIN : MGF_HILL;
        m.size = jradius(f, "radius", v2(200, 200));
        m.height = jnum(f, "height", 60);
        m.param = jnum(f, "ridge", 0.6f);
    } else if (!strcmp(type, "rim")) {
        m.kind = MGF_RIM;
        const cJSON *e = cJSON_GetObjectItemCaseSensitive(f, "edges");
        m.edge = edge_mask(cJSON_IsString(e) ? e->valuestring : "");
        m.height = jnum(f, "height", 80);
        m.blend = jnum(f, "width", 150);
        const cJSON *r = cJSON_GetObjectItemCaseSensitive(f, "range");
        m.range = cJSON_IsArray(r) ? jv2(r) : v2(-1e6f, 1e6f);
    } else if (!strcmp(type, "lake")) {
        m.kind = MGF_LAKE;
        m.size = jradius(f, "radius", v2(120, 80));
        m.level = jnum(f, "level", 12);
        m.depth = jnum(f, "depth", 6);
        m.param = jnum(f, "shore_noise", 20);
    } else if (!strcmp(type, "pit")) {
        m.kind = MGF_PIT;
        m.size = jradius(f, "radius", v2(100, 100));
        m.depth = jnum(f, "depth", 30);
        m.terraces = (int)jnum(f, "terraces", 5);
    } else if (!strcmp(type, "pad") || !strcmp(type, "paint")) {
        m.kind = !strcmp(type, "pad") ? MGF_PAD : MGF_PAINT;
        vec2 sz = jradius(f, "size", v2(40, 40));
        m.size = v2_scale(sz, 0.5f);
        const cJSON *lv = cJSON_GetObjectItemCaseSensitive(f, "level");
        m.auto_level = !cJSON_IsNumber(lv);
        m.level = cJSON_IsNumber(lv) ? (float)lv->valuedouble : 0;
        m.blend = jnum(f, "blend", 25);
    } else if (!strcmp(type, "forest")) {
        m.kind = MGF_FOREST;
        m.size = jradius(f, "radius", v2(150, 150));
        m.param = jnum(f, "density", 2.5f);
        m.height = jnum(f, "pine", 0.6f);
    } else {
        snprintf(err, err_size, "unknown feature type '%s'", type);
        return false;
    }
    if (mapgen_add_feature(g, &m) < 0) { snprintf(err, err_size, "too many features"); return false; }
    return true;
}

bool mapgen_parse(MapGen *g, const cJSON *world, char *err, size_t err_size) {
    if (!world) { snprintf(err, err_size, "no world block"); return false; }
    g->half_size = jnum(world, "half_size", g->half_size);
    g->raster_half = fmaxf(jnum(world, "raster_half", g->raster_half), g->half_size);
    const cJSON *ter = cJSON_GetObjectItemCaseSensitive(world, "terrain");
    if (ter) {
        g->base_height = jnum(ter, "base", g->base_height);
        g->hill_amp = jnum(ter, "hill_amp", g->hill_amp);
        g->hill_freq = jnum(ter, "hill_freq", g->hill_freq);
    }
    g->n_hills.frequency = g->hill_freq;
    const cJSON *sea = cJSON_GetObjectItemCaseSensitive(world, "sea");
    if (sea) {
        g->sea_floor = jnum(sea, "floor", g->sea_floor);
        g->beach_width = jnum(sea, "beach", g->beach_width);
        g->sea_noise = jnum(sea, "noise", g->sea_noise);
        g->sea_count = jpoints(cJSON_GetObjectItemCaseSensitive(sea, "polygon"), g->sea, MG_MAX_POLY);
    }
    const cJSON *f;
    cJSON_ArrayForEach(f, cJSON_GetObjectItemCaseSensitive(world, "features"))
        if (!mapgen_parse_feature(g, f, err, err_size)) return false;
    cJSON_ArrayForEach(f, cJSON_GetObjectItemCaseSensitive(world, "rivers")) {
        if (g->river_count >= MG_MAX_RIVERS) break;
        MgRiver *r = &g->rivers[g->river_count];
        memset(r, 0, sizeof(*r));
        r->ctrl_count = jpoints(cJSON_GetObjectItemCaseSensitive(f, "points"), r->ctrl, MG_MAX_POLY);
        r->width = jnum(f, "width", 12);
        r->depth = jnum(f, "depth", 2.5f);
        r->bank = jnum(f, "bank", 10);
        r->start_level = jnum(f, "start_level", NO_WATER);
        r->end_level = jnum(f, "end_level", 0.0f);
        if (r->ctrl_count < 2) { snprintf(err, err_size, "river with < 2 points"); return false; }
        g->river_count++;
    }
    cJSON_ArrayForEach(f, cJSON_GetObjectItemCaseSensitive(world, "roads")) {
        const cJSON *c = cJSON_GetObjectItemCaseSensitive(f, "class");
        int cls = mapgen_road_class(cJSON_IsString(c) ? c->valuestring : "secondary");
        if (cls < 0) { snprintf(err, err_size, "unknown road class '%s'", cJSON_IsString(c) ? c->valuestring : "?"); return false; }
        vec2 pts[MG_MAX_POLY];
        int n = jpoints(cJSON_GetObjectItemCaseSensitive(f, "points"), pts, MG_MAX_POLY);
        const cJSON *sm = cJSON_GetObjectItemCaseSensitive(f, "smooth"), *nm = cJSON_GetObjectItemCaseSensitive(f, "name");
        const cJSON *lp = cJSON_GetObjectItemCaseSensitive(f, "loop");
        if (mapgen_add_road(g, cls, jnum(f, "width", 0), pts, n, !cJSON_IsFalse(sm), cJSON_IsTrue(lp), cJSON_IsString(nm) ? nm->valuestring : NULL) < 0) {
            snprintf(err, err_size, "bad road (%d points)", n);
            return false;
        }
    }
    return true;
}

// ------------------------------------------------------------------ terrain

// Features whose influence can reach (x, z) (the cell's list, ascending: application order kept).
static int feature_cell(const MapGen *g, float x, float z, const int **list) {
    if (!g->feature_grid.n) { *list = NULL; return g->feature_count; }
    int c = grid_cell(&g->feature_grid, x, z);
    if (c < 0) { *list = NULL; return 0; }
    *list = g->feature_grid.items + g->feature_grid.start[c];
    return g->feature_grid.start[c + 1] - g->feature_grid.start[c];
}

static void feature_bounds(const void *ctx, int i, vec2 *lo, vec2 *hi) {
    const MapGen *g = (const MapGen *)ctx;
    const MgFeature *f = &g->features[i];
    float r;
    switch (f->kind) {
    case MGF_RIM: *lo = v2(-1e6f, -1e6f); *hi = v2(1e6f, 1e6f); return;
    case MGF_LAKE: r = fmaxf(f->size.x, f->size.y) * 2.0f + f->param; break;
    case MGF_PAD: r = sqrtf(f->size.x * f->size.x + f->size.y * f->size.y) + f->blend; break;
    case MGF_PAINT: r = sqrtf(f->size.x * f->size.x + f->size.y * f->size.y); break;
    case MGF_FOREST: r = fmaxf(f->size.x, f->size.y) * 1.3f; break;
    default: r = fmaxf(f->size.x, f->size.y); break;
    }
    r += 2.0f;
    *lo = v2(f->pos.x - r, f->pos.y - r);
    *hi = v2(f->pos.x + r, f->pos.y + r);
}

float mapgen_sea_distance(const MapGen *g, float x, float z) {
    if (g->sea_count < 3) return -1e9f;
    return poly_signed_dist(g->sea, g->sea_count, v2(x, z)) + noise2(&g->n_shore, x, z) * g->sea_noise;
}

// Lake ellipse distance with shoreline noise (1 = shore).
static float lake_d(const MapGen *g, const MgFeature *f, float x, float z) {
    float d = ellipse_d(v2(x, z), f->pos, f->size, f->yaw);
    float ravg = (f->size.x + f->size.y) * 0.5f;
    return d + noise2(&g->n_shore, x * 1.3f + 500.0f, z * 1.3f) * f->param / ravg;
}

// Natural terrain + sea + lakes + pits + pads (the pre-road height); *mat its surface.
static float height_pre(const MapGen *g, float x, float z, u8 *mat_out) {
    const int *fl;
    int fn = feature_cell(g, x, z, &fl);
    float h = g->base_height + noise2(&g->n_hills, x, z) * g->hill_amp + noise2(&g->n_detail, x, z) * 0.35f;
    int mat = MGM_GRASS;
    float meadow = noise2(&g->n_hills, x * 2.7f + 911.0f, z * 2.7f);
    if (meadow > 0.25f) mat = MGM_MEADOW;
    vec2 p = v2(x, z);
    float ridge = 1.0f - fabsf(noise2(&g->n_ridge, x, z));
    ridge *= ridge;
    // additive relief
    for (int k = 0; k < fn; k++) {
        const MgFeature *f = &g->features[fl ? fl[k] : k];
        if (f->kind == MGF_MOUNTAIN || f->kind == MGF_HILL) {
            float d = ellipse_d(p, f->pos, f->size, f->yaw);
            if (d >= 1.0f) continue;
            float t = 1.0f - d, bump = t * t * (3.0f - 2.0f * t);
            float shape = f->kind == MGF_MOUNTAIN ? lerpf(1.0f, 0.6f + 0.55f * ridge, f->param) : 1.0f;
            h += f->height * bump * shape;
        } else if (f->kind == MGF_RIM) {
            float best = 0;
            for (int e = 0; e < 4; e++) {
                if (!(f->edge & (1 << e))) continue;
                float de, along;
                switch (1 << e) {
                case MG_EDGE_W: de = x + g->half_size; along = z; break;
                case MG_EDGE_E: de = g->half_size - x; along = z; break;
                case MG_EDGE_S: de = g->half_size - z; along = x; break;   // south = +Z (engine compass)
                default: de = z + g->half_size; along = x; break;          // north = -Z
                }
                float t = 1.0f - saturate(de / f->blend);
                float rm = smoothstepf(f->range.x - 120.0f, f->range.x, along) * (1.0f - smoothstepf(f->range.y, f->range.y + 120.0f, along));
                best = fmaxf(best, t * t * (3.0f - 2.0f * t) * rm);
            }
            h += f->height * best * (0.82f + 0.3f * ridge);
        }
    }
    // sea: beaches down to the floor
    if (g->sea_count >= 3) {
        float sd = mapgen_sea_distance(g, x, z);
        if (sd > -g->beach_width) {
            if (sd >= 0) {
                h = fminf(h, fmaxf(g->sea_floor, 0.25f - sd * 0.07f));
            } else {
                float t = smoothstepf(-g->beach_width, 0.0f, sd);
                h = lerpf(h, fminf(h, 0.25f - sd * 0.05f), t);
            }
            if (h < 2.2f) mat = MGM_SAND;
        }
    }
    for (int k = 0; k < fn; k++) {
        const MgFeature *f = &g->features[fl ? fl[k] : k];
        if (f->kind == MGF_LAKE) {
            float d = lake_d(g, f, x, z);
            if (d >= 2.0f) continue;
            float pulled = lerpf(f->level + 1.6f, h, smoothstepf(1.15f, 2.0f, d));
            if (d < 1.15f) {
                float hb = d < 1.0f ? f->level - 0.4f - f->depth * powf(1.0f - d * d, 0.7f) : lerpf(f->level - 0.4f, f->level + 1.6f, (d - 1.0f) / 0.15f);
                h = fminf(pulled, hb);
                mat = d < 1.0f ? MGM_MUD : (noise2(&g->n_detail, x * 0.3f, z * 0.3f) > 0.1f ? MGM_SAND : MGM_DIRT);
            } else {
                h = pulled;
            }
        } else if (f->kind == MGF_PIT) {
            float d = ellipse_d(p, f->pos, f->size, f->yaw);
            if (d >= 1.0f) continue;
            float kt = (float)DG_MAX(f->terraces, 1);
            float u = saturate((d - 0.22f) / 0.78f) * kt;   // 0 at the floor's edge, k at the rim
            float idx = floorf(u), frac = u - idx;
            float stepped = (idx + smoothstepf(0.72f, 1.0f, frac)) / kt;
            float rim = h;
            h = rim - f->depth * (1.0f - stepped);
            mat = frac > 0.72f && u < kt ? MGM_ROCK : MGM_GRAVEL;
        }
    }
    for (int k = 0; k < fn; k++) {   // forest floor under the woods (before pads)
        const MgFeature *f = &g->features[fl ? fl[k] : k];
        if (f->kind != MGF_FOREST || (mat != MGM_GRASS && mat != MGM_MEADOW)) continue;
        if (ellipse_d(p, f->pos, f->size, f->yaw) + noise2(&g->n_shore, x + 3000.0f, z) * 0.25f < 0.85f) mat = MGM_FOREST;
    }
    for (int k = 0; k < fn; k++) {
        const MgFeature *f = &g->features[fl ? fl[k] : k];
        if (f->kind != MGF_PAD) continue;
        float dd = rect_dist(p, f->pos, f->size, f->yaw);
        if (dd >= f->blend) continue;
        float w = 1.0f - smoothstepf(0.0f, f->blend, dd);
        h = lerpf(h, f->level, w);
        if (dd <= 0.0f && f->material >= 0) mat = f->material;
    }
    if (mat_out) *mat_out = (u8)mat;
    return h;
}

float mapgen_height_base(const MapGen *g, float x, float z) { return height_pre(g, x, z, NULL); }

typedef struct RoadHit { int sample; float dist, h, t; } RoadHit;

// Best road influence at (x, z): the sample whose weight (1 on the carriageway, falling off over the
// shoulder) is highest (higher classes win ties).
static bool road_query(const MapGen *g, float x, float z, float extra, RoadHit *out, float *weight) {
    int c = grid_cell(&g->road_grid, x, z);
    if (c < 0) return false;
    vec2 p = v2(x, z);
    float best_w = -1;
    bool found = false;
    for (int k = g->road_grid.start[c]; k < g->road_grid.start[c + 1]; k++) {
        int i = g->road_grid.items[k];
        const MgRoadSample *a = &g->rs[i], *b = &g->rs[i + 1];
        const MgRoad *rd = &g->roads[a->road];
        float t, d = seg_dist(p, a->p, b->p, &t);
        float hw = rd->width * 0.5f;
        if (d > hw + rd->shoulder + extra) continue;
        // on the carriageway the higher class wins, then the nearest segment (its own profile)
        float w = d <= hw ? 2.0f - (float)rd->cls * 0.01f - d * 0.001f : 1.0f - smoothstepf(hw, hw + rd->shoulder, d);
        if (w > best_w) {
            best_w = w;
            out->sample = i;
            out->dist = d;
            out->t = t;
            out->h = lerpf(a->h, b->h, t);
            found = true;
        }
    }
    if (weight) *weight = best_w > 1.0f ? 1.0f : best_w;
    return found;
}

typedef struct RiverHit { int sample; float dist, water; } RiverHit;
static bool river_query(const MapGen *g, float x, float z, RiverHit *out) {
    int c = grid_cell(&g->river_grid, x, z);
    if (c < 0) return false;
    vec2 p = v2(x, z);
    float best = 1e30f;
    for (int k = g->river_grid.start[c]; k < g->river_grid.start[c + 1]; k++) {
        int i = g->river_grid.items[k];
        const MgRiverSample *a = &g->vs[i], *b = &g->vs[i + 1];
        float t, d = seg_dist(p, a->p, b->p, &t);
        const MgRiver *rv = &g->rivers[a->river];
        if (d > rv->width * 0.5f + rv->bank || d >= best) continue;
        best = d;
        out->sample = i;
        out->dist = d;
        out->water = lerpf(a->water, b->water, t);
    }
    return best < 1e29f;
}

float mapgen_height(const MapGen *g, float x, float z, u8 *mat_out) {
    u8 mat;
    float h = height_pre(g, x, z, &mat);
    const int *fl;
    int fn = feature_cell(g, x, z, &fl);
    for (int k = 0; k < fn; k++) {   // surface paint (fields, lots)
        const MgFeature *f = &g->features[fl ? fl[k] : k];
        if (f->kind == MGF_PAINT && f->material >= 0 && rect_dist(v2(x, z), f->pos, f->size, f->yaw) <= 0.0f) mat = (u8)f->material;
    }
    RoadHit rh;
    float w;
    if (g->rs_count && road_query(g, x, z, 0.0f, &rh, &w)) {
        const MgRoad *rd = &g->roads[g->rs[rh.sample].road];
        h = lerpf(h, rh.h, w);
        float hw = rd->width * 0.5f;
        if (rh.dist <= hw) {
            mat = rd->cls == MGR_DIRT || rd->cls == MGR_TRAIL ? MGM_DIRT_ROAD : (rd->cls == MGR_RUNWAY ? MGM_CONCRETE : MGM_ASPHALT);
        } else if (rh.dist <= hw + 1.2f && (rd->cls == MGR_MAIN || rd->cls == MGR_SECONDARY)) {
            mat = MGM_GRAVEL;
        } else if (rh.dist <= hw + 1.0f && rd->cls == MGR_DIRT && mat != MGM_CONCRETE) {
            mat = MGM_DIRT;
        }
    }
    RiverHit vh;
    if (g->vs_count && river_query(g, x, z, &vh)) {
        const MgRiver *rv = &g->rivers[g->vs[vh.sample].river];
        float hw = rv->width * 0.5f, hb;
        if (vh.dist < hw) {
            float u = vh.dist / hw;
            hb = vh.water - rv->depth * sqrtf(fmaxf(1.0f - u * u, 0.0f)) - 0.3f;
        } else {
            hb = lerpf(vh.water - 0.3f, h, smoothstepf(0.0f, 1.0f, (vh.dist - hw) / rv->bank));
        }
        if (hb < h) {
            h = hb;
            mat = h < vh.water ? MGM_MUD : (h < vh.water + 1.0f ? MGM_SAND : MGM_DIRT);
        }
    }
    if (mat_out) *mat_out = mat;
    return h;
}

float mapgen_water_level(const MapGen *g, float x, float z) {
    float best = NO_WATER;
    if (g->sea_count >= 3 && mapgen_sea_distance(g, x, z) > -g->beach_width) best = 0.0f;
    const int *fl;
    int fn = feature_cell(g, x, z, &fl);
    for (int k = 0; k < fn; k++) {
        const MgFeature *f = &g->features[fl ? fl[k] : k];
        if (f->kind == MGF_LAKE && lake_d(g, f, x, z) < 1.15f) best = fmaxf(best, f->level);
    }
    RiverHit vh;
    if (g->vs_count && river_query(g, x, z, &vh)) {
        const MgRiver *rv = &g->rivers[g->vs[vh.sample].river];
        if (vh.dist < rv->width * 0.5f + rv->bank * 0.5f) best = fmaxf(best, vh.water);
    }
    return best;
}

int mapgen_nearest_road(const MapGen *g, float x, float z, float max_dist, float *dist) {
    int c = grid_cell(&g->road_grid, x, z);
    if (c < 0 || !g->rs_count) return -1;
    int best = -1;
    float bd = max_dist;
    for (int k = g->road_grid.start[c]; k < g->road_grid.start[c + 1]; k++) {
        int i = g->road_grid.items[k];
        float d = seg_dist(v2(x, z), g->rs[i].p, g->rs[i + 1].p, NULL) - g->roads[g->rs[i].road].width * 0.5f;
        if (d < bd) { bd = d; best = i; }
    }
    if (dist) *dist = bd;
    return best;
}

bool mapgen_in_keepout(const MapGen *g, float x, float z, float margin) {
    int c = grid_cell(&g->keep_grid, x, z);
    if (c < 0) return false;
    for (int k = g->keep_grid.start[c]; k < g->keep_grid.start[c + 1]; k++) {
        const MgKeepout *o = &g->keep[g->keep_grid.items[k]];
        if (rect_dist(v2(x, z), o->c, o->half, o->yaw) <= margin) return true;
    }
    return false;
}

float mapgen_tree_density(const MapGen *g, const MgRaster *r, float x, float z, float *pine) {
    float dens = 0, pf = 0.7f;
    const int *fl;
    int fn = feature_cell(g, x, z, &fl);
    for (int k = 0; k < fn; k++) {
        const MgFeature *f = &g->features[fl ? fl[k] : k];
        if (f->kind != MGF_FOREST) continue;
        float d = ellipse_d(v2(x, z), f->pos, f->size, f->yaw) + noise2(&g->n_shore, x + 3000.0f, z) * 0.25f;
        if (d >= 1.0f) continue;
        float w = 1.0f - smoothstepf(0.75f, 1.0f, d);
        float clearing = smoothstepf(-0.55f, -0.35f, noise2(&g->n_hills, x * 3.1f, z * 3.1f + 777.0f));   // open patches
        float dd = f->param * 0.01f * w * clearing;
        if (dd > dens) { dens = dd; pf = f->height; }
    }
    float h = mapgen_raster_height(r, x, z), slope = mapgen_raster_slope(r, x, z);
    u8 mat = mapgen_raster_mat(r, x, z);
    // pads (camp, farmsteads, cabins, lots) only keep trees an explicit forest gives them
    bool pad = false;
    for (int k = 0; k < fn && !pad; k++) {
        const MgFeature *f = &g->features[fl ? fl[k] : k];
        pad = f->kind == MGF_PAD && rect_dist(v2(x, z), f->pos, f->size, f->yaw) <= 2.0f;
    }
    if (pad && mat != MGM_GRASS) return 0;   // yards / lots stay clear (parks are painted grass)
    if (!pad) {
        // scattered pines on the mountains, sparse trees on open grass
        if (h > 34.0f && slope < 0.9f) { float m = 0.006f * (1.0f - smoothstepf(150.0f, 220.0f, h)); if (m > dens) { dens = m; pf = 0.95f; } }
        if (mat == MGM_GRASS && dens < 0.0007f) dens = 0.0007f * saturate(noise2(&g->n_hills, x * 4.0f, z * 4.0f) * 2.0f + 0.4f);
    }
    if (pine) *pine = pf;
    if (dens <= 0) return 0;
    if (mat == MGM_ASPHALT || mat == MGM_CONCRETE || mat == MGM_DIRT_ROAD || mat == MGM_FIELD || mat == MGM_CROP || mat == MGM_PLOWED || mat == MGM_SAND ||
        mat == MGM_GRAVEL || mat == MGM_MUD || slope > 1.1f)
        return 0;
    if (mapgen_water_level(g, x, z) > h - 0.6f) return 0;
    float rd;
    if (mapgen_nearest_road(g, x, z, 6.0f, &rd) >= 0 && rd < 4.0f) return 0;
    if (mapgen_in_keepout(g, x, z, 3.0f)) return 0;
    return dens;
}

// ------------------------------------------------------------------ finalize: sampling and profiles

typedef struct PolySampler { vec2 *pts; int n, cap; } PolySampler;
static void ps_push(PolySampler *s, vec2 p) {
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 1024;
        s->pts = (vec2 *)realloc(s->pts, sizeof(vec2) * (size_t)s->cap);
    }
    s->pts[s->n++] = p;
}

static vec2 catmull(vec2 p0, vec2 p1, vec2 p2, vec2 p3, float t) {
    float t2 = t * t, t3 = t2 * t;
    return v2(0.5f * (2 * p1.x + (-p0.x + p2.x) * t + (2 * p0.x - 5 * p1.x + 4 * p2.x - p3.x) * t2 + (-p0.x + 3 * p1.x - 3 * p2.x + p3.x) * t3),
              0.5f * (2 * p1.y + (-p0.y + p2.y) * t + (2 * p0.y - 5 * p1.y + 4 * p2.y - p3.y) * t2 + (-p0.y + 3 * p1.y - 3 * p2.y + p3.y) * t3));
}

// Resamples a control polyline (Catmull-Rom or straight) at `step` along its arc length.
static int resample(const vec2 *ctrl, int n, bool smooth, bool loop, float step, vec2 **out) {
    PolySampler dense = { 0 }, res = { 0 };
    int segs = loop ? n : n - 1;
    for (int i = 0; i < segs; i++) {
        vec2 p1 = ctrl[i], p2 = ctrl[(i + 1) % n];
        vec2 p0 = i > 0 ? ctrl[i - 1] : (loop ? ctrl[n - 1] : v2_sub(v2_scale(p1, 2), p2));
        vec2 p3 = (i + 2 < n) ? ctrl[i + 2] : (loop ? ctrl[(i + 2) % n] : v2_sub(v2_scale(p2, 2), p1));
        int sub = (int)fmaxf(4.0f, v2_len(v2_sub(p2, p1)) / 1.0f);
        for (int k = 0; k < sub; k++) {
            float t = (float)k / (float)sub;
            ps_push(&dense, smooth ? catmull(p0, p1, p2, p3, t) : v2_add(p1, v2_scale(v2_sub(p2, p1), t)));
        }
    }
    ps_push(&dense, loop ? ctrl[0] : ctrl[n - 1]);
    float acc = 0, next = 0;
    ps_push(&res, dense.pts[0]);
    next = step;
    for (int i = 1; i < dense.n; i++) {
        vec2 a = dense.pts[i - 1], b = dense.pts[i];
        float l = v2_len(v2_sub(b, a));
        while (acc + l >= next && l > 1e-6f) {
            float t = (next - acc) / l;
            ps_push(&res, v2_add(a, v2_scale(v2_sub(b, a), t)));
            next += step;
        }
        acc += l;
    }
    vec2 last = dense.pts[dense.n - 1];
    if (v2_len(v2_sub(last, res.pts[res.n - 1])) > step * 0.25f) ps_push(&res, last);
    free(dense.pts);
    *out = res.pts;
    return res.n;
}

static void road_bounds(const void *ctx, int i, vec2 *lo, vec2 *hi) {
    const MapGen *g = (const MapGen *)ctx;
    const MgRoadSample *a = &g->rs[i], *b = &g->rs[i + 1];
    float r = (a->road == b->road) ? g->roads[a->road].width * 0.5f + g->roads[a->road].shoulder + 1.0f : -1e9f;
    if (r < 0) { *lo = v2(1e9f, 1e9f); *hi = v2(-1e9f, -1e9f); return; }   // across two roads: not a segment
    *lo = v2(fminf(a->p.x, b->p.x) - r, fminf(a->p.y, b->p.y) - r);
    *hi = v2(fmaxf(a->p.x, b->p.x) + r, fmaxf(a->p.y, b->p.y) + r);
}
static void river_bounds(const void *ctx, int i, vec2 *lo, vec2 *hi) {
    const MapGen *g = (const MapGen *)ctx;
    const MgRiverSample *a = &g->vs[i], *b = &g->vs[i + 1];
    if (a->river != b->river) { *lo = v2(1e9f, 1e9f); *hi = v2(-1e9f, -1e9f); return; }
    float r = g->rivers[a->river].width * 0.5f + g->rivers[a->river].bank + 1.0f;
    *lo = v2(fminf(a->p.x, b->p.x) - r, fminf(a->p.y, b->p.y) - r);
    *hi = v2(fmaxf(a->p.x, b->p.x) + r, fmaxf(a->p.y, b->p.y) + r);
}
static void keep_bounds(const void *ctx, int i, vec2 *lo, vec2 *hi) {
    const MgKeepout *o = &((const MapGen *)ctx)->keep[i];
    float r = sqrtf(o->half.x * o->half.x + o->half.y * o->half.y) + 4.0f;
    *lo = v2(o->c.x - r, o->c.y - r);
    *hi = v2(o->c.x + r, o->c.y + r);
}

static void smooth_profile(float *h, int n, int half, float grade, float step, bool keep_ends) {
    if (n < 2) return;
    float *tmp = (float *)malloc(sizeof(float) * (size_t)n);
    if (half > 0) {
        for (int i = 0; i < n; i++) {
            float s = 0;
            int c = 0;
            for (int k = i - half; k <= i + half; k++) {
                int j = DG_CLAMP(k, 0, n - 1);
                s += h[j];
                c++;
            }
            tmp[i] = s / (float)c;
        }
        if (keep_ends) { tmp[0] = h[0]; tmp[n - 1] = h[n - 1]; }
        memcpy(h, tmp, sizeof(float) * (size_t)n);
    }
    float gs = grade * step;
    for (int pass = 0; pass < 4; pass++) {
        for (int i = 1; i < n; i++) h[i] = clampf(h[i], h[i - 1] - gs, h[i - 1] + gs);
        for (int i = n - 2; i >= 0; i--) h[i] = clampf(h[i], h[i + 1] - gs, h[i + 1] + gs);
    }
    free(tmp);
}

static void build_rivers(MapGen *g) {
    PolySampler all = { 0 };
    int total = 0;
    vec2 *pts[MG_MAX_RIVERS];
    int cnt[MG_MAX_RIVERS];
    for (int r = 0; r < g->river_count; r++) {
        cnt[r] = resample(g->rivers[r].ctrl, g->rivers[r].ctrl_count, true, false, MG_ROAD_STEP, &pts[r]);
        total += cnt[r];
    }
    (void)all;
    g->vs = (MgRiverSample *)calloc((size_t)total + 1, sizeof(MgRiverSample));
    g->vs_count = 0;
    for (int r = 0; r < g->river_count; r++) {
        MgRiver *rv = &g->rivers[r];
        rv->first = g->vs_count;
        rv->count = cnt[r];
        float level = rv->start_level;
        for (int i = 0; i < cnt[r]; i++) {
            MgRiverSample *s = &g->vs[g->vs_count++];
            s->p = pts[r][i];
            vec2 d = v2_sub(pts[r][DG_MIN(i + 1, cnt[r] - 1)], pts[r][DG_MAX(i - 1, 0)]);
            float l = v2_len(d);
            s->dir = l > 1e-6f ? v2_scale(d, 1.0f / l) : v2(1, 0);
            s->river = (u16)r;
            float land = height_pre(g, s->p.x, s->p.y, NULL);
            if (level < -1e8f) level = land - 1.2f;
            level = fminf(level, land - 1.2f);
            level = fmaxf(level, rv->end_level);
            s->water = level;
        }
        free(pts[r]);
    }
    float extent = g->raster_half * 2.0f;
    if (g->vs_count > 1) grid_build(&g->river_grid, -g->raster_half, extent, g->vs_count - 1, river_bounds, g);
}

void mapgen_finalize(MapGen *g) {
    if (g->finalized) return;
    grid_build(&g->feature_grid, -g->raster_half, g->raster_half * 2.0f, g->feature_count, feature_bounds, g);
    // pad levels: the mean of the natural terrain under the pad (before other pads)
    for (int i = 0; i < g->feature_count; i++) {
        MgFeature *f = &g->features[i];
        if (f->kind != MGF_PAD || !f->auto_level) continue;
        float s = 0;
        int c = 0;
        float cs = cosf(f->yaw), sn = sinf(f->yaw);
        f->kind = MGF_PAINT;   // evaluated without itself
        for (int a = -4; a <= 4; a++)
            for (int b = -4; b <= 4; b++) {
                float lx = f->size.x * (float)a / 4.0f, lz = f->size.y * (float)b / 4.0f;
                s += height_pre(g, f->pos.x + lx * cs + lz * sn, f->pos.y - lx * sn + lz * cs, NULL);
                c++;
            }
        f->kind = MGF_PAD;
        f->level = s / (float)c;
        f->auto_level = false;
    }
    build_rivers(g);
    // roads: samples, raw profile, smoothing / grade, junction snaps, bridges
    int total = 0;
    vec2 **pts = (vec2 **)calloc((size_t)g->road_count + 1, sizeof(vec2 *));
    int *cnt = (int *)calloc((size_t)g->road_count + 1, sizeof(int));
    for (int r = 0; r < g->road_count; r++) {
        cnt[r] = resample(g->road_ctrl + g->road_ctrl_first[r], g->road_ctrl_count[r], g->road_smooth[r], g->roads[r].loop, MG_ROAD_STEP, &pts[r]);
        total += cnt[r];
    }
    g->rs = (MgRoadSample *)calloc((size_t)total + 1, sizeof(MgRoadSample));
    g->rs_count = 0;
    float *hbuf = (float *)malloc(sizeof(float) * (size_t)(total + 1));
    // process in class order so lower classes can snap their ends onto finished higher ones
    int order[MG_MAX_ROADS], no = 0;
    for (int c = 0; c < MGR_CLASS_COUNT; c++)
        for (int r = 0; r < g->road_count; r++) if (g->roads[r].cls == c) order[no++] = r;
    // samples are stored in road index order
    for (int r = 0; r < g->road_count; r++) {
        MgRoad *rd = &g->roads[r];
        rd->first = g->rs_count;
        rd->count = cnt[r];
        for (int i = 0; i < cnt[r]; i++) {
            MgRoadSample *s = &g->rs[g->rs_count++];
            s->p = pts[r][i];
            vec2 d = v2_sub(pts[r][DG_MIN(i + 1, cnt[r] - 1)], pts[r][DG_MAX(i - 1, 0)]);
            float l = v2_len(d);
            s->dir = l > 1e-6f ? v2_scale(d, 1.0f / l) : v2(1, 0);
            s->road = (u16)r;
            s->h = height_pre(g, s->p.x, s->p.y, NULL);
        }
        free(pts[r]);
    }
    free(pts);
    free(cnt);
    // the road grid now (snapping queries finished roads through it; heights are filled per road)
    float extent = g->raster_half * 2.0f;
    bool *done = (bool *)calloc((size_t)g->road_count + 1, sizeof(bool));
    for (int oi = 0; oi < no; oi++) {
        int r = order[oi];
        MgRoad *rd = &g->roads[r];
        float *h = hbuf;
        for (int i = 0; i < rd->count; i++) h[i] = g->rs[rd->first + i].h;
        // bridges: samples over a river (wet) get a deck above the water, approaches ramp to it
        u8 *wet = (u8 *)calloc((size_t)rd->count + 1, 1);
        for (int i = 0; i < rd->count; i++) {
            MgRoadSample *s = &g->rs[rd->first + i];
            RiverHit vh;
            if (g->vs_count && river_query(g, s->p.x, s->p.y, &vh) && vh.dist < g->rivers[g->vs[vh.sample].river].width * 0.5f + 2.0f) wet[i] = 1;
        }
        // ends meet finished roads of higher classes (junctions): the end heights are theirs
        int snap0 = -1, snap1 = -1;
        float hs0 = 0, hs1 = 0;
        for (int e = 0; e < 2 && !rd->loop; e++) {
            const MgRoadSample *s = &g->rs[rd->first + (e ? rd->count - 1 : 0)];
            float best = rd->width + 6.0f;
            for (int oj = 0; oj < oi; oj++) {
                int q = order[oj];
                if (!done[q]) continue;
                const MgRoad *rq = &g->roads[q];
                for (int k = 0; k + 1 < rq->count; k++) {
                    const MgRoadSample *a = &g->rs[rq->first + k], *b = a + 1;
                    float t, d = seg_dist(s->p, a->p, b->p, &t) - rq->width * 0.5f;
                    if (d < best) {
                        best = d;
                        if (e) { snap1 = q; hs1 = lerpf(a->h, b->h, t); }
                        else { snap0 = q; hs0 = lerpf(a->h, b->h, t); }
                    }
                }
            }
        }
        int half = rd->keep_level ? 0 : (int)(road_smooth_m(rd->cls) / MG_ROAD_STEP);
        if (snap0 >= 0) h[0] = hs0;
        if (snap1 >= 0) h[rd->count - 1] = hs1;
        smooth_profile(h, rd->count, half, road_grade(rd->cls), MG_ROAD_STEP, snap0 >= 0 || snap1 >= 0);
        // re-impose the junction heights, blended over 30 m
        for (int e = 0; e < 2; e++) {
            int sn = e ? snap1 : snap0;
            if (sn < 0) continue;
            float hj = e ? hs1 : hs0;
            int span = (int)(30.0f / MG_ROAD_STEP);
            for (int k = 0; k <= span && k < rd->count; k++) {
                int i = e ? rd->count - 1 - k : k;
                float w = 1.0f - smoothstepf(0.0f, (float)span, (float)k);
                h[i] = lerpf(h[i], hj, w);
            }
        }
        // bridges: group the flagged samples, deck above the highest water + 2.2 m, ramps of 24 m
        for (int i = 0; i < rd->count; i++) {
            if (!wet[i]) continue;
            int j = i;
            while (j + 1 < rd->count && wet[j + 1]) j++;
            int a = DG_MAX(i - 3, 0), b = DG_MIN(j + 3, rd->count - 1);
            float water = -1e9f;
            for (int k = i; k <= j; k++) {
                RiverHit vh;
                const MgRoadSample *s = &g->rs[rd->first + k];
                if (river_query(g, s->p.x, s->p.y, &vh)) water = fmaxf(water, vh.water);
            }
            float deck = fmaxf(fmaxf(h[a], h[b]), water + 2.6f);
            int ramp = (int)(24.0f / MG_ROAD_STEP);
            for (int k = a - ramp; k <= b + ramp; k++) {
                if (k < 0 || k >= rd->count) continue;
                float w = k < a ? smoothstepf((float)(a - ramp), (float)a, (float)k) : (k > b ? 1.0f - smoothstepf((float)b, (float)(b + ramp), (float)k) : 1.0f);
                h[k] = fmaxf(h[k], lerpf(h[k], deck, w));
            }
            for (int k = a; k <= b; k++) g->rs[rd->first + k].bridge = 1;
            if (g->bridge_count < MG_MAX_BRIDGES) {
                MgBridge *br = &g->bridges[g->bridge_count++];
                br->road = r;
                br->s0 = rd->first + a;
                br->s1 = rd->first + b;
                br->a = g->rs[br->s0].p;
                br->b = g->rs[br->s1].p;
                br->h = deck;
                br->width = rd->width + 1.0f;
            }
            i = j;
        }
        free(wet);
        for (int i = 0; i < rd->count; i++) g->rs[rd->first + i].h = h[i];
        done[r] = true;
    }
    free(done);
    free(hbuf);
    if (g->rs_count > 1) grid_build(&g->road_grid, -g->raster_half, extent, g->rs_count - 1, road_bounds, g);
    if (g->keep_count) grid_build(&g->keep_grid, -g->raster_half, extent, g->keep_count, keep_bounds, g);
    g->finalized = true;
}

// ------------------------------------------------------------------ raster

bool mapgen_raster_alloc(MgRaster *r, float half, float spacing) {
    memset(r, 0, sizeof(*r));
    r->spacing = spacing;
    r->origin = -half;
    r->n = (int)lrintf(2.0f * half / spacing) + 1;
    r->h = (float *)malloc(sizeof(float) * (size_t)r->n * (size_t)r->n);
    r->mat = (u8 *)malloc((size_t)r->n * (size_t)r->n);
    return r->h && r->mat;
}

void mapgen_raster_free(MgRaster *r) {
    free(r->h);
    free(r->mat);
    memset(r, 0, sizeof(*r));
}

void mapgen_raster_rows(const MapGen *g, MgRaster *r, int row0, int row1) {
    for (int z = row0; z < row1 && z < r->n; z++)
        for (int x = 0; x < r->n; x++) {
            size_t i = (size_t)z * (size_t)r->n + (size_t)x;
            r->h[i] = mapgen_height(g, r->origin + (float)x * r->spacing, r->origin + (float)z * r->spacing, &r->mat[i]);
        }
}

void mapgen_raster_slopes(const MapGen *g, MgRaster *r, int row0, int row1) {
    (void)g;
    for (int z = row0; z < row1 && z < r->n; z++)
        for (int x = 0; x < r->n; x++) {
            size_t i = (size_t)z * (size_t)r->n + (size_t)x;
            u8 m = r->mat[i];
            if (m != MGM_GRASS && m != MGM_MEADOW && m != MGM_FOREST && m != MGM_DIRT) continue;
            int xl = DG_MAX(x - 1, 0), xr = DG_MIN(x + 1, r->n - 1), zd = DG_MAX(z - 1, 0), zu = DG_MIN(z + 1, r->n - 1);
            float dx = (r->h[(size_t)z * r->n + xr] - r->h[(size_t)z * r->n + xl]) / ((float)(xr - xl) * r->spacing);
            float dz = (r->h[(size_t)zu * r->n + x] - r->h[(size_t)zd * r->n + x]) / ((float)(zu - zd) * r->spacing);
            float s = sqrtf(dx * dx + dz * dz);
            if (s > 0.85f) r->mat[i] = MGM_ROCK;
            else if (s > 0.55f && m != MGM_DIRT) r->mat[i] = MGM_DIRT;
        }
}

float mapgen_raster_height(const MgRaster *r, float x, float z) {
    float fx = clampf((x - r->origin) / r->spacing, 0.0f, (float)(r->n - 1) - 1e-3f);
    float fz = clampf((z - r->origin) / r->spacing, 0.0f, (float)(r->n - 1) - 1e-3f);
    int ix = (int)fx, iz = (int)fz;
    float tx = fx - (float)ix, tz = fz - (float)iz;
    const float *h = r->h + (size_t)iz * r->n + ix;
    return lerpf(lerpf(h[0], h[1], tx), lerpf(h[r->n], h[r->n + 1], tx), tz);
}

u8 mapgen_raster_mat(const MgRaster *r, float x, float z) {
    int ix = DG_CLAMP((int)lrintf((x - r->origin) / r->spacing), 0, r->n - 1), iz = DG_CLAMP((int)lrintf((z - r->origin) / r->spacing), 0, r->n - 1);
    return r->mat[(size_t)iz * r->n + ix];
}

float mapgen_raster_slope(const MgRaster *r, float x, float z) {
    float d = r->spacing;
    float dx = (mapgen_raster_height(r, x + d, z) - mapgen_raster_height(r, x - d, z)) / (2 * d);
    float dz = (mapgen_raster_height(r, x, z + d) - mapgen_raster_height(r, x, z - d)) / (2 * d);
    return sqrtf(dx * dx + dz * dz);
}

vec3 mapgen_material_color(int mat, float v, float slope) {
    static const vec3 base[MGM_COUNT] = {
        { 0.17f, 0.25f, 0.09f },   // grass
        { 0.33f, 0.31f, 0.16f },   // meadow (dry)
        { 0.13f, 0.12f, 0.07f },   // forest floor
        { 0.29f, 0.22f, 0.14f },   // dirt
        { 0.25f, 0.24f, 0.22f },   // rock
        { 0.58f, 0.52f, 0.38f },   // sand
        { 0.38f, 0.36f, 0.33f },   // gravel
        { 0.10f, 0.10f, 0.11f },   // asphalt
        { 0.34f, 0.27f, 0.18f },   // dirt road
        { 0.40f, 0.39f, 0.37f },   // concrete
        { 0.50f, 0.41f, 0.20f },   // field (wheat)
        { 0.20f, 0.29f, 0.09f },   // crop (green)
        { 0.22f, 0.16f, 0.10f },   // plowed
        { 0.18f, 0.16f, 0.12f },   // mud
    };
    vec3 c = base[DG_CLAMP(mat, 0, MGM_COUNT - 1)];
    float k = 0.82f + 0.36f * v;
    c = v3_scale(c, k);
    if (mat == MGM_GRASS || mat == MGM_MEADOW || mat == MGM_FOREST) c = v3_lerp(c, base[MGM_DIRT], smoothstepf(0.35f, 0.7f, slope) * 0.6f);
    if (mat == MGM_ASPHALT) c = v3_lerp(c, v3(0.17f, 0.17f, 0.16f), smoothstepf(0.55f, 0.9f, v));   // worn patches
    return c;
}
