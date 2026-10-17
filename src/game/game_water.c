// Water game glue (docs/WATER.md): the level's water bodies (level JSON "water", the open world's
// sea / lakes / rivers from mapgen), presets (assets/world/water/*.json), the ground function for
// depth queries, the wind and wave time every machine derives from the replicated weather and the
// server tick, buoyancy for the host's physics props, and the console commands / debug view.
//
// Nothing here is replicated: every machine builds the same bodies from the same level and seed and
// evaluates the same swell (water_waves.c) from the same (wind, tick).
#include "game_internal.h"
#include "game_water.h"
#include "core/cmd_registry.h"
#include "core/engine.h"
#include "render/render.h"
#include "core/cvar.h"
#include "core/log.h"
#include "core/platform.h"
#include "gameplay/entities.h"
#include "gameplay/health.h"
#include "player/player.h"
#include "net/net.h"
#include "physics/buoyancy.h"
#include "render/debug_draw.h"
#include "world/mapgen.h"
#include "world/openworld.h"
#include "world/weather.h"
#include "cJSON.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define G (g_engine.game)

#define GW_MAX_PRESETS 32
#define GW_GROUND_MAX 1024          // texels per side of the baked ground grid (non open-world levels)
#define GW_GROUND_TEXEL 0.5f        // m

static struct {
    WaterWorld world;
    bool loaded;
    // presets
    WaterParams presets[GW_MAX_PRESETS];
    int preset_count;
    bool presets_loaded;
    // ground (levels without a terrain raster): a downward raycast bake over the bodies' bounds
    float *ground;
    int gw, gh;
    float gx0, gz0;
    float ground_default;           // outside the grid (an infinite ocean in a room level: deep)
    // buoyancy (host)
    BuoySystem buoy;
    EntityId buoy_ent[BUOY_MAX_BODIES];
    bool buoy_ext[BUOY_MAX_BODIES]; // owned by another system (vehicles): not a prop, attach / detach
    i16 buoy_of[MAX_ENTITIES];      // entity index -> buoy item + 1
    u32 wind_tick;                  // tick the wind was last sampled at (quantized to 1 s)
    Cvar *debug, *wave_scale, *drown_dps;
    float drown_acc[MAX_PLAYERS];   // host: s until the next drowning hit
    // water_test (in-game checks, logged as "water_test <mode> -> ok / FAIL")
    struct {
        int mode, ticks, frame, fails, checks;
        bool quit;
        UserCmd cmd;      // scripted input (swim)
        bool drive;
        float hp0, hp_prev;
        int above, total;
    } t;
} GW;

enum { WT_NONE, WT_PROPS, WT_SWIM, WT_WAVES };

const WaterWorld *game_water(void) { return &GW.world; }
WaterWorld *game_water_mut(void) { return &GW.world; }
BuoySystem *game_water_buoyancy(void) { return &GW.buoy; }

float game_water_time(double tick) {
    if (tick < 0.0) tick = 0.0;
    u32 t = (u32)tick;
    return waves_time(t, (float)(tick - (double)t), SIM_HZ);
}

float game_water_height(float x, float z, double tick) { return water_height_at(&GW.world, x, z, game_water_time(tick)); }
float game_water_depth(float x, float z, double tick) { return water_depth_at(&GW.world, x, z, game_water_time(tick)); }
bool game_water_sample(float x, float z, double tick, WaterSample *s) { return water_sample(&GW.world, x, z, game_water_time(tick), s); }

// ------------------------------------------------------------------ presets

static void load_presets(void) {
    if (GW.presets_loaded) return;
    GW.presets_loaded = true;
    char dir[512];
    platform_data_path("assets/world/water", dir, sizeof(dir));
    int n = 0;
    char **names = SDL_GlobDirectory(dir, "*.json", 0, &n);
    for (int i = 0; names && i < n && GW.preset_count < GW_MAX_PRESETS; i++) {
        char path[640];
        snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        char *text = platform_read_file(path, NULL);
        if (!text) continue;
        // The preset's base kind ("kind": "ocean") picks the defaults it overrides.
        cJSON *root = cJSON_Parse(text);
        const cJSON *kind = root ? cJSON_GetObjectItemCaseSensitive(root, "kind") : NULL;
        int k = cJSON_IsString(kind) ? water_kind_parse(kind->valuestring) : WATER_LAKE;
        cJSON_Delete(root);
        WaterParams p = water_params_default(k < 0 ? WATER_LAKE : (WaterKind)k);
        char err[128];
        char base[64];
        dg_strcpy(base, sizeof(base), names[i]);
        char *dot = strrchr(base, '.');
        if (dot) *dot = 0;
        dg_strcpy(p.preset, sizeof(p.preset), base);
        if (water_params_parse(&p, text, err, sizeof(err))) {
            dg_strcpy(p.preset, sizeof(p.preset), base);   // the file name is the preset's name
            GW.presets[GW.preset_count++] = p;
        } else {
            log_warn("water: %s: %s", names[i], err);
        }
        free(text);
    }
    SDL_free(names);
    log_info("water: %d presets", GW.preset_count);
}

const WaterParams *game_water_preset(const char *name) {
    load_presets();
    for (int i = 0; i < GW.preset_count; i++)
        if (!strcmp(GW.presets[i].preset, name)) return &GW.presets[i];
    return NULL;
}

// ------------------------------------------------------------------ ground

static float ground_open(void *user, float x, float z) { return openworld_ground((const World *)user, x, z); }

static float ground_baked(void *user, float x, float z) {
    (void)user;
    if (!GW.ground) return GW.ground_default;
    float fx = (x - GW.gx0) / GW_GROUND_TEXEL - 0.5f, fz = (z - GW.gz0) / GW_GROUND_TEXEL - 0.5f;
    int ix = (int)floorf(fx), iz = (int)floorf(fz);
    if (ix < 0 || iz < 0 || ix >= GW.gw - 1 || iz >= GW.gh - 1) return GW.ground_default;
    float tx = fx - (float)ix, tz = fz - (float)iz;
    const float *r0 = GW.ground + iz * GW.gw + ix, *r1 = r0 + GW.gw;
    return lerpf(lerpf(r0[0], r0[1], tx), lerpf(r1[0], r1[1], tx), tz);
}

// Downward casts from just under each body's surface (a dock or a roof above the water is not its
// bottom) over the union of the finite bodies' bounds.
static void bake_ground(World *w) {
    free(GW.ground);
    GW.ground = NULL;
    GW.ground_default = -1000.0f;
    float lo[2] = { 1e30f, 1e30f }, hi[2] = { -1e30f, -1e30f }, top = -1e30f;
    for (int i = 0; i < GW.world.count; i++) {
        const WaterBody *b = &GW.world.body[i];
        if (b->shape == WSHAPE_INFINITE) continue;
        for (int k = 0; k < 2; k++) { lo[k] = fminf(lo[k], b->bmin[k] - 2.0f); hi[k] = fmaxf(hi[k], b->bmax[k] + 2.0f); }
        top = fmaxf(top, b->level);
    }
    if (lo[0] > hi[0] || !w->phys) return;
    GW.gw = DG_MIN(GW_GROUND_MAX, (int)ceilf((hi[0] - lo[0]) / GW_GROUND_TEXEL) + 1);
    GW.gh = DG_MIN(GW_GROUND_MAX, (int)ceilf((hi[1] - lo[1]) / GW_GROUND_TEXEL) + 1);
    GW.gx0 = lo[0];
    GW.gz0 = lo[1];
    GW.ground = (float *)malloc(sizeof(float) * (size_t)GW.gw * (size_t)GW.gh);
    double t0 = platform_time();
    for (int z = 0; z < GW.gh; z++)
        for (int x = 0; x < GW.gw; x++) {
            float px = GW.gx0 + ((float)x + 0.5f) * GW_GROUND_TEXEL, pz = GW.gz0 + ((float)z + 0.5f) * GW_GROUND_TEXEL;
            float lv, fl[2];
            float start = water_body_at(&GW.world, px, pz, &lv, fl) >= 0 ? lv - 0.02f : top - 0.02f;
            RayHit rh;
            float g = start - 200.0f;
            if (phys_raycast(w->phys, v3(px, start, pz), v3(0, -200.0f, 0), PL_MASK(PL_STATIC), PHYS_INVALID_BODY, &rh)) g = rh.point.y;
            GW.ground[z * GW.gw + x] = g;
        }
    log_info("water: ground baked %dx%d over (%.0f, %.0f)..(%.0f, %.0f) in %.0f ms", GW.gw, GW.gh, lo[0], lo[1], hi[0], hi[1],
             (platform_time() - t0) * 1000.0);
}

// ------------------------------------------------------------------ open-world bodies

static bool ow_sea(void *user, const WaterBody *b, float x, float z, float *level, float flow[2]) {
    (void)b; (void)flow;
    if (!mapgen_in_sea((const MapGen *)user, x, z)) return false;
    *level = 0.0f;
    return true;
}
static bool ow_lake(void *user, const WaterBody *b, float x, float z, float *level, float flow[2]) {
    (void)flow;
    if (!mapgen_in_lake((const MapGen *)user, b->user_index, x, z)) return false;
    *level = b->level;
    return true;
}
static bool ow_river(void *user, const WaterBody *b, float x, float z, float *level, float flow[2]) {
    vec2 dir;
    if (!mapgen_river_water((const MapGen *)user, x, z, level, &dir)) return false;
    flow[0] = dir.x * b->p.flow_speed;
    flow[1] = dir.y * b->p.flow_speed;
    return true;
}

static void add_mapgen_bodies(World *w) {
    const MapGen *g = openworld_gen(w);
    if (!g) return;
    const WaterParams *sea_p = game_water_preset("ocean"), *lake_p = game_water_preset("lake"), *river_p = game_water_preset("river");
    float half = g->half_size;
    if (g->sea_count >= 3) {
        WaterBody b;
        memset(&b, 0, sizeof(b));
        dg_strcpy(b.name, sizeof(b.name), "sea");
        b.kind = WATER_OCEAN;
        b.shape = WSHAPE_CALLBACK;
        b.p = sea_p ? *sea_p : water_params_default(WATER_OCEAN);
        b.fn = ow_sea;
        b.user = (void *)g;
        b.bmin[0] = b.bmin[1] = -half - 2000.0f;   // the sea continues past the map edge
        b.bmax[0] = b.bmax[1] = half + 2000.0f;
        water_add_body(&GW.world, &b);
    }
    for (int i = 0; i < g->feature_count; i++) {
        const MgFeature *f = &g->features[i];
        if (f->kind != MGF_LAKE) continue;
        WaterBody b;
        memset(&b, 0, sizeof(b));
        snprintf(b.name, sizeof(b.name), "lake%d", i);
        b.kind = WATER_LAKE;
        b.shape = WSHAPE_CALLBACK;
        b.p = lake_p ? *lake_p : water_params_default(WATER_LAKE);
        b.level = f->level;
        b.fn = ow_lake;
        b.user = (void *)g;
        b.user_index = i;
        float r = fmaxf(f->size.x, f->size.y) * 1.4f + f->param;
        b.bmin[0] = f->pos.x - r; b.bmax[0] = f->pos.x + r;
        b.bmin[1] = f->pos.y - r; b.bmax[1] = f->pos.y + r;
        water_add_body(&GW.world, &b);
    }
    if (g->vs_count) {
        WaterBody b;
        memset(&b, 0, sizeof(b));
        dg_strcpy(b.name, sizeof(b.name), "rivers");
        b.kind = WATER_RIVER;
        b.shape = WSHAPE_CALLBACK;
        b.p = river_p ? *river_p : water_params_default(WATER_RIVER);
        b.level = 1e6f;          // per point from the callback
        b.fn = ow_river;
        b.user = (void *)g;
        b.bmin[0] = b.bmin[1] = -half;
        b.bmax[0] = b.bmax[1] = half;
        water_add_body(&GW.world, &b);
    }
}

// ------------------------------------------------------------------ surface meshes (renderer)

typedef struct MeshOut { RenderWaterVertex *v; u32 *idx; u32 vn, in, vcap, icap; } MeshOut;

static u32 mo_vertex(MeshOut *m, float x, float y, float z, u32 body) {
    if (m->vn == m->vcap) {
        m->vcap = m->vcap ? m->vcap * 2 : 4096;
        m->v = (RenderWaterVertex *)realloc(m->v, sizeof(RenderWaterVertex) * m->vcap);
    }
    RenderWaterVertex *o = &m->v[m->vn];
    o->pos[0] = x; o->pos[1] = y; o->pos[2] = z;
    o->body = body;
    return m->vn++;
}
static void mo_tri(MeshOut *m, u32 a, u32 b, u32 c) {
    if (m->in + 3 > m->icap) {
        m->icap = m->icap ? m->icap * 2 : 8192;
        m->idx = (u32 *)realloc(m->idx, sizeof(u32) * m->icap);
    }
    m->idx[m->in++] = a; m->idx[m->in++] = b; m->idx[m->in++] = c;
}

// Pools / lakes with an explicit footprint: a grid whose cells are kept when their centre is in the
// body (the walls / banks hide the cell-sized edge).
static void mesh_grid_body(MeshOut *m, const WaterWorld *ww, int bi) {
    const WaterBody *b = &ww->body[bi];
    float ex = b->bmax[0] - b->bmin[0], ez = b->bmax[1] - b->bmin[1];
    float cell = fmaxf(0.5f, fmaxf(ex, ez) / 256.0f);
    int nx = (int)ceilf(ex / cell), nz = (int)ceilf(ez / cell);
    u32 *row = (u32 *)malloc(sizeof(u32) * (size_t)(nx + 1) * (size_t)(nz + 1));
    for (size_t i = 0; i < (size_t)(nx + 1) * (size_t)(nz + 1); i++) row[i] = 0xFFFFFFFFu;
    for (int z = 0; z < nz; z++)
        for (int x = 0; x < nx; x++) {
            float cx = b->bmin[0] + ((float)x + 0.5f) * cell, cz = b->bmin[1] + ((float)z + 0.5f) * cell;
            float lv, fl[2];
            if (water_body_at(ww, cx, cz, &lv, fl) != bi) continue;
            u32 q[4];
            for (int k = 0; k < 4; k++) {
                int vx = x + (k & 1), vz = z + (k >> 1);
                u32 *slot = &row[vz * (nx + 1) + vx];
                if (*slot == 0xFFFFFFFFu) *slot = mo_vertex(m, b->bmin[0] + (float)vx * cell, b->level, b->bmin[1] + (float)vz * cell, (u32)bi);
                q[k] = *slot;
            }
            mo_tri(m, q[0], q[2], q[1]);
            mo_tri(m, q[1], q[2], q[3]);
        }
    free(row);
}

// Open-world lake: a disc over 1.4 x its radii (the terrain hides what lies past the shore).
static void mesh_lake_disc(MeshOut *m, const MgFeature *f, int bi) {
    const int rings = 12, segs = 96;
    u32 c0 = mo_vertex(m, f->pos.x, f->level, f->pos.y, (u32)bi);
    float cs = cosf(f->yaw), sn = sinf(f->yaw);
    for (int r = 1; r <= rings; r++)
        for (int s = 0; s < segs; s++) {
            float a = (float)s / (float)segs * 6.2831853f, rr = 1.4f * (float)r / (float)rings;
            vec2 l = v2(cosf(a) * f->size.x * rr, sinf(a) * f->size.y * rr);
            mo_vertex(m, f->pos.x + l.x * cs + l.y * sn, f->level, f->pos.y - l.x * sn + l.y * cs, (u32)bi);
        }
    for (int s = 0; s < segs; s++) mo_tri(m, c0, c0 + 1 + (u32)((s + 1) % segs), c0 + 1 + (u32)s);
    for (int r = 1; r < rings; r++)
        for (int s = 0; s < segs; s++) {
            u32 a = c0 + 1 + (u32)((r - 1) * segs + s), b = c0 + 1 + (u32)((r - 1) * segs + (s + 1) % segs);
            u32 c = a + (u32)segs, d = b + (u32)segs;
            mo_tri(m, a, b, d);
            mo_tri(m, a, d, c);
        }
}

// Open-world rivers: ribbons along the samples at their water profile.
static void mesh_rivers(MeshOut *m, const MapGen *g, int bi) {
    for (int r = 0; r < g->river_count; r++) {
        const MgRiver *rv = &g->rivers[r];
        float hw = rv->width * 0.5f + rv->bank * 0.6f;
        u32 base = m->vn;
        for (int i = 0; i < rv->count; i++) {
            const MgRiverSample *s = &g->vs[rv->first + i];
            vec2 nrm = v2(s->dir.y, -s->dir.x);
            mo_vertex(m, s->p.x + nrm.x * hw, s->water, s->p.y + nrm.y * hw, (u32)bi);
            mo_vertex(m, s->p.x - nrm.x * hw, s->water, s->p.y - nrm.y * hw, (u32)bi);
        }
        for (int i = 0; i + 1 < rv->count; i++) {
            u32 a = base + (u32)(i * 2), b = a + 1, c = a + 2, d = a + 3;
            mo_tri(m, a, c, b);
            mo_tri(m, b, c, d);
        }
    }
}

static void build_meshes(World *w) {
    MeshOut m;
    memset(&m, 0, sizeof(m));
    const MapGen *g = openworld_active(w) ? openworld_gen(w) : NULL;
    for (int i = 0; i < GW.world.count; i++) {
        const WaterBody *b = &GW.world.body[i];
        if (b->kind == WATER_OCEAN) continue;   // the renderer's camera grid
        if (b->shape == WSHAPE_POLYGON || b->shape == WSHAPE_ELLIPSE) mesh_grid_body(&m, &GW.world, i);
        else if (b->shape == WSHAPE_CALLBACK && g && b->kind == WATER_LAKE) mesh_lake_disc(&m, &g->features[b->user_index], i);
        else if (b->shape == WSHAPE_CALLBACK && g && b->kind == WATER_RIVER) mesh_rivers(&m, g, i);
    }
    render_water_mesh(m.v, m.vn, m.idx, m.in);
    if (m.vn) log_info("water: surface meshes %u vertices, %u triangles", m.vn, m.in / 3);
    free(m.v);
    free(m.idx);
}

// ------------------------------------------------------------------ level

void water_level_loaded(World *w, const char *map) {
    load_presets();
    water_world_init(&GW.world, 0x5EA5EAu ^ (u32)cvar_find("world_seed")->i);
    GW.loaded = true;
    free(GW.ground);
    GW.ground = NULL;
    buoy_system_init(&GW.buoy);
    memset(GW.buoy_of, 0, sizeof(GW.buoy_of));
    GW.wind_tick = 0xFFFFFFFFu;
    if (!w) return;
    // Level JSON "water": [{type, name, preset, level, polygon | box | ellipse, ...}]
    char rel[160], buf[512];
    snprintf(rel, sizeof(rel), "data/levels/%s.json", map ? map : "");
    char *text = map && map[0] ? platform_read_file(platform_data_path(rel, buf, sizeof(buf)), NULL) : NULL;
    cJSON *root = text ? cJSON_Parse(text) : NULL;
    free(text);
    const cJSON *arr = root ? cJSON_GetObjectItemCaseSensitive(root, "water") : NULL;
    const cJSON *e;
    cJSON_ArrayForEach(e, arr) {
        const cJSON *pn = cJSON_GetObjectItemCaseSensitive(e, "preset");
        const cJSON *ty = cJSON_GetObjectItemCaseSensitive(e, "type");
        const WaterParams *pre = cJSON_IsString(pn) ? game_water_preset(pn->valuestring) : NULL;
        if (!pre && cJSON_IsString(ty)) pre = game_water_preset(ty->valuestring);
        if (cJSON_IsString(pn) && !game_water_preset(pn->valuestring)) log_warn("water: unknown preset '%s'", pn->valuestring);
        WaterBody b;
        char err[160];
        if (!water_body_from_json(&b, e, pre, err, sizeof(err))) { log_warn("water: %s: %s", map, err); continue; }
        if (water_add_body(&GW.world, &b) < 0) { log_warn("water: more than %d bodies", WATER_MAX_BODIES); break; }
    }
    cJSON_Delete(root);
    if (openworld_active(w)) {
        add_mapgen_bodies(w);
        water_set_ground(&GW.world, ground_open, w);
    } else {
        bake_ground(w);
        water_set_ground(&GW.world, ground_baked, NULL);
    }
    if (GW.world.count) log_info("water: %d bodies on '%s'", GW.world.count, map ? map : "");
    build_meshes(w);
}

// ------------------------------------------------------------------ tick

// Wind for the swell: the replicated weather evaluated at whole seconds of server time, so host and
// clients feed the identical value (the swell barely changes within a second).
static void update_wind(World *w) {
    u32 qt = w->tick - w->tick % (u32)SIM_HZ;
    if (qt == GW.wind_tick) return;
    GW.wind_tick = qt;
    float wind = 0.0f, dir = 0.0f;
    if (w->level.sky && w->weather.version) {
        WeatherParams p;
        weather_eval(&w->weather, qt, SIM_DT, &p);
        wind = p.v[WP_WIND];
        dir = p.v[WP_WIND_DIR];
    }
    water_set_wind(&GW.world, wind * GW.wave_scale->f, dir);
}

static bool buoy_plane(void *user, float x, float z, float extent, BuoyWaterPlane *o) {
    float t = *(const float *)user;
    WaterPatch p;
    if (!water_patch(&GW.world, x, z, t, extent, &p)) return false;
    o->density = p.density;
    return water_patch_plane(&p, x, z, &o->height, &o->gx, &o->gz, o->vel);
}

static void buoy_forget(int k) {
    int last = GW.buoy.count - 1;
    EntityId e = GW.buoy_ent[k];
    if (e && !GW.buoy_ext[k]) GW.buoy_of[ENT_INDEX(e)] = 0;
    if (k != last) {
        GW.buoy.item[k] = GW.buoy.item[last];
        GW.buoy_ent[k] = GW.buoy_ent[last];
        GW.buoy_ext[k] = GW.buoy_ext[last];
        if (GW.buoy_ent[k] && !GW.buoy_ext[k]) GW.buoy_of[ENT_INDEX(GW.buoy_ent[k])] = (i16)(k + 1);
    }
    GW.buoy.count--;
}

// ---- externally owned buoyant bodies (vehicles: game_vehicles.c)

Buoyancy *game_water_buoy_of(BodyId body) { return buoy_find(&GW.buoy, body); }

Buoyancy *game_water_buoy_attach(World *w, BodyId body, const BuoyProbe *probes, int n, const BuoyDesc *d) {
    int k = buoy_add_probes(&GW.buoy, w->phys, body, probes, n, d);
    if (k < 0) return NULL;
    GW.buoy_ent[k] = (EntityId)phys_body_user(w->phys, body);
    GW.buoy_ext[k] = true;
    return &GW.buoy.item[k];
}

void game_water_buoy_detach(BodyId body) {
    for (int k = 0; k < GW.buoy.count; k++)
        if (GW.buoy.item[k].body == body && GW.buoy_ext[k]) { buoy_forget(k); return; }
}

// One body's buoyancy now (clients: the vehicle they predict, before their physics step).
void game_water_buoy_step_one(World *w, BodyId body) {
    Buoyancy *b = buoy_find(&GW.buoy, body);
    if (!b || !w->phys || !GW.world.count) return;
    BuoyBodyState st;
    phys_get_transform(w->phys, body, &st.pos, &st.rot);
    st.com = phys_body_com(w->phys, body);
    st.v = phys_get_velocity(w->phys, body);
    st.w = phys_get_angular_velocity(w->phys, body);
    float t = waves_time(w->tick, 0.0f, SIM_HZ);
    BuoyWater water = { buoy_plane, &t, 9.81f };
    BuoyResult r;
    buoy_compute(b, &st, &water, SIM_DT, &r);
    b->prev_submerged = b->submerged;
    b->submerged = r.submerged;
    b->water_h = r.water_h;
    b->in_water = r.in_water;
    b->impulse = r.impulse;
    b->ang_impulse = r.ang_impulse;
    if (r.in_water) phys_add_impulse_com(w->phys, body, r.impulse, r.ang_impulse);
}

// Every dynamic prop is buoyant (its density decides whether it floats). New props register on the
// tick they appear; destroyed ones leave.
static void sync_buoyant(World *w) {
    for (int k = GW.buoy.count - 1; k >= 0; k--) {
        if (GW.buoy_ext[k]) continue;
        EntityId e = GW.buoy_ent[k];
        int i = (int)ENT_INDEX(e);
        if (!e || !ent_valid(w, e) || !(w->comps[i] & C_BODY) || w->body[i].body != GW.buoy.item[k].body || w->body[i].motion != PM_DYNAMIC)
            buoy_forget(k);
    }
    ENT_FOREACH(w, i) {
        if (w->type[i] != ET_PROP || !(w->comps[i] & C_BODY) || w->body[i].motion != PM_DYNAMIC || GW.buoy_of[i]) continue;
        BuoyDesc d = buoy_desc_default();
        d.density = w->body[i].density;
        int k = buoy_add(&GW.buoy, w->phys, w->body[i].body, &d);
        if (k < 0) continue;
        GW.buoy_ent[k] = ENT_MAKE(i, w->gen[i]);
        GW.buoy_of[i] = (i16)(k + 1);
    }
}

static void test_tick(World *w);

// Movement's water query (movement.c): the surface, the column and the water velocity (flow +
// orbital) at the command's tick.
static bool move_water(World *w, float x, float z, double tick, float *surface, float *column, vec3 *flow) {
    (void)w;
    if (!GW.world.count) return false;
    WaterSample s;
    if (!water_sample(&GW.world, x, z, game_water_time(tick), &s)) return false;
    *surface = s.height;
    *column = s.depth;
    *flow = v3(s.vel[0], s.vel[1], s.vel[2]);   // y: the surface's own vertical motion (waves)
    return true;
}

// Host: out of breath -> water_drown_dps damage, the first hit at once, then every second.
static void drowning(World *w) {
    for (int sl = 0; sl < MAX_PLAYERS; sl++) {
        Player *p = &w->players[sl];
        if (!p->active || !p->character || p->sim.move.mode == MOVE_DEAD || p->sim.move.oxygen > 0.0f) {
            GW.drown_acc[sl] = 0.0f;
            continue;
        }
        GW.drown_acc[sl] -= SIM_DT;
        if (GW.drown_acc[sl] > 0.0f) continue;
        GW.drown_acc[sl] += 1.0f;
        apply_damage(w, p->ent, GW.drown_dps->f, DMG_DROWN, p->ent, p->sim.move.pos, v3(0, 1, 0), false);
    }
}

void water_tick(World *w) {
    if (!GW.loaded || !w) return;
    update_wind(w);
    if (!w->authority || !GW.world.count || !w->phys) return;
    drowning(w);
    sync_buoyant(w);
    float t = waves_time(w->tick, 0.0f, SIM_HZ);
    BuoyWater water = { buoy_plane, &t, 9.81f };
    buoy_step(&GW.buoy, w->phys, &water, SIM_DT);
    if (GW.t.mode) test_tick(w);
}

// water_test props: every buoyant prop in a lake floats at its density's submersion (density /
// water density, +-5 %), or rests on the bottom when denser than the water.
static void test_props(World *w) {
    int n = 0, bad = 0;
    for (int k = 0; k < GW.buoy.count; k++) {
        const Buoyancy *b = &GW.buoy.item[k];
        if (!b->in_water && b->water_h < -1e8f) continue;
        vec3 pos;
        phys_get_transform(w->phys, b->body, &pos, NULL);
        float rho = b->mass / b->volume;
        WaterSample s;
        if (!water_sample(&GW.world, pos.x, pos.z, waves_time(w->tick, 0, SIM_HZ), &s)) continue;
        float wd = GW.world.body[s.body].p.density;
        float expect = rho < wd ? rho / wd : 1.0f;
        bool ok = fabsf(b->submerged - expect) < 0.05f;
        if (rho >= wd) ok = b->submerged > 0.95f && pos.y < s.height - 0.3f;
        log_info("  prop %d: density %.0f -> submerged %.3f (expected %.3f) at y %.2f: %s", k, rho, b->submerged, expect, pos.y, ok ? "ok" : "FAIL");
        n++;
        bad += !ok;
    }
    log_info("water_test props -> %s (%d bodies, %d off)", n > 0 && !bad ? "ok" : "FAIL", n, bad);
}

static void test_check(bool ok, const char *fmt, ...) {
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    GW.t.checks++;
    if (!ok) GW.t.fails++;
    log_info("  %s %s", ok ? "ok  " : "FAIL", b);
}

static void test_end(void) {
    if (GW.t.mode == WT_SWIM || GW.t.mode == WT_WAVES)
        log_info("water_test %s -> %s (%d checks, %d failed)", GW.t.mode == WT_SWIM ? "swim" : "waves", GW.t.fails ? "FAIL" : "ok", GW.t.checks, GW.t.fails);
    GW.t.mode = WT_NONE;
    GW.t.drive = false;
    if (GW.t.quit) cmd_execute_line("quit");
}

void water_test_cmd(UserCmd *c) {
    if (!GW.t.drive) return;
    c->move_x = GW.t.cmd.move_x;
    c->move_y = GW.t.cmd.move_y;
    c->buttons = (u16)((c->buttons & (u16)~(BTN_JUMP | BTN_CROUCH | BTN_SPRINT | BTN_AIM | BTN_ATTACK)) | GW.t.cmd.buttons);
    c->yaw = GW.t.cmd.yaw;
    c->pitch = GW.t.cmd.pitch;
    G->view.yaw = c->yaw;
    G->view.pitch = c->pitch;
}

static void teleport_local(World *w, vec3 feet, float yaw) {
    Player *p = game_local_player();
    if (!p || !p->character) return;
    p->sim.move.pos = feet;
    p->sim.move.vel = v3(0, 0, 0);
    p->sim.move.mode = MOVE_WALK;
    p->sim.move.yaw = yaw;
    phys_character_set_pos(p->character, feet);
    phys_character_set_vel(p->character, v3(0, 0, 0));
    (void)w;
}

static void set_input(float fwd, u16 buttons, float yaw, float pitch) {
    GW.t.cmd.move_x = 0;
    GW.t.cmd.move_y = (i8)(fwd * 127.0f);
    GW.t.cmd.buttons = buttons;
    GW.t.cmd.yaw = yaw;
    GW.t.cmd.pitch = pitch;
}

// water_test swim (dev room pool, water at 2.6 m on a floor at 0, south wall inner face at z -76):
// float up from the bottom, dive, resurface, breath / drowning damage timing, swim to the wall and
// climb out onto the rim.
static void test_swim(World *w) {
    Player *p = game_local_player();
    if (!p) { test_check(false, "local player"); test_end(); return; }
    PlayerMove *m = &p->sim.move;
    int f = ++GW.t.frame;
    const float yaw_south = DG_PI;     // yaw 0 faces -Z: pi faces +Z (the south wall)
    float surface = 2.6f;
    int hi = (int)ENT_INDEX(p->ent);
    if (f == 1) {
        GW.t.drive = true;
        set_input(0, 0, yaw_south, 0);
        teleport_local(w, v3(80.0f, 0.05f, -82.0f), yaw_south);
        m->oxygen = player_breath_max();
        w->health[hi].hp = w->health[hi].max_hp;
    }
    if (f == 6 * SIM_HZ) {
        float eye = m->pos.y + player_eye_height(m);
        test_check(m->mode == MOVE_SWIM && m->water == WATER_SURFACE, "deep water: swimming at the surface (mode %d, water %d)", m->mode, m->water);
        test_check(fabsf(eye - (surface + 0.16f)) < 0.12f, "eye held above the surface: %.3f (surface %.2f)", eye, surface);
        set_input(0, BTN_CROUCH, yaw_south, 0);   // dive
    }
    if (f == 8 * SIM_HZ) {
        test_check(m->water == WATER_UNDER && m->head_under, "crouch dives (water %d, head under %d)", m->water, m->head_under);
        test_check(m->oxygen < player_breath_max() - 1.0f, "breath runs down under water (%.2f s left)", m->oxygen);
        set_input(0, 0, yaw_south, 0);            // let go: rise
    }
    if (f == 14 * SIM_HZ) {
        test_check(m->water == WATER_SURFACE && !m->head_under, "idle diver rises back to the surface (water %d)", m->water);
        test_check(m->oxygen > player_breath_max() - 0.01f, "breath restored at the surface (%.2f)", m->oxygen);
        // Drowning: 2 s of breath left, hold under.
        m->oxygen = 2.0f;
        GW.t.hp0 = w->health[hi].hp;
        GW.t.ticks = 0;
        set_input(0, BTN_CROUCH, yaw_south, 0);
    }
    // Drowning timing, from the tick the breath ran out (the dive takes a moment to get the head under).
    if (f > 14 * SIM_HZ && f < 24 * SIM_HZ) {
        if (!GW.t.ticks && m->oxygen <= 0.0f) {
            GW.t.ticks = f;
            test_check(GW.t.hp_prev == GW.t.hp0, "no damage while the breath lasted (hp %.0f the tick before)", GW.t.hp_prev);
        }
        GW.t.hp_prev = w->health[hi].hp;
        int since = GW.t.ticks ? f - GW.t.ticks : -1;
        if (since == SIM_HZ / 2) {
            float lost = GW.t.hp0 - w->health[hi].hp;
            test_check(fabsf(lost - GW.drown_dps->f) < 0.01f, "first drowning hit at once (%.0f hp lost, expected %.0f)", lost, GW.drown_dps->f);
        }
        if (since == 2 * SIM_HZ + SIM_HZ / 2) {
            float lost = GW.t.hp0 - w->health[hi].hp;
            test_check(fabsf(lost - 3.0f * GW.drown_dps->f) < 0.01f, "then one hit per second (%.0f hp lost after 2.5 s, expected %.0f)", lost, 3.0f * GW.drown_dps->f);
            w->health[hi].hp = w->health[hi].max_hp;
            set_input(0, 0, yaw_south, 0);
        }
    }
    if (f == 24 * SIM_HZ) {
        test_check(m->water == WATER_SURFACE, "back at the surface");
        set_input(1.0f, 0, yaw_south, 0);         // swim to the south wall
    }
    if (f > 24 * SIM_HZ && f < 34 * SIM_HZ && m->mode == MOVE_SWIM && m->pos.z > -77.0f) set_input(1.0f, BTN_JUMP, yaw_south, 0);
    if (f > 24 * SIM_HZ && m->mode == MOVE_MANTLE) set_input(0, 0, yaw_south, 0);   // stay on the rim
    if (f == 34 * SIM_HZ) {
        test_check(m->mode == MOVE_WALK && m->pos.y > 2.9f && m->water == WATER_DRY, "climbed out onto the rim (mode %d, feet %.2f)", m->mode, m->pos.y);
        test_check(m->wet > 0.9f, "wet after swimming (%.2f)", m->wet);
        test_end();
    }
}

// water_test waves (wateryard): wind 10 m/s; an idle swimmer in the open sea keeps the head above the
// waves on >= 95 % of the ticks; then the GPU swell (water_waves.glsl) against the CPU one
// (water_waves.c) at 512 points: within 1 cm.
static void test_waves(World *w) {
    Player *p = game_local_player();
    if (!p || GW.world.ocean < 0) { test_check(false, "an ocean and a local player (map wateryard)"); test_end(); return; }
    PlayerMove *m = &p->sim.move;
    int f = ++GW.t.frame;
    if (f == 1) {
        GW.t.drive = true;
        set_input(0, 0, 0, 0);
        cmd_execute_line("weather_params wind 10 time 0.1");
        teleport_local(w, v3(0.0f, -1.6f, 240.0f), 0.0f);
        GW.t.above = GW.t.total = GW.t.ticks = 0;
    }
    // Wait for the weather's wind transition, then measure 30 s.
    if (!GW.t.ticks) {
        if (fabsf(GW.world.wind - 10.0f * GW.wave_scale->f) < 0.3f) {
            GW.t.ticks = f;
            test_check(GW.world.waves.hs > 0.5f, "swell driven by the 10 m/s wind after %.1f s: Hs %.2f m, peak %.0f m", (float)f / SIM_HZ, GW.world.waves.hs,
                       GW.world.waves.peak_len);
        } else if (f > 90 * SIM_HZ) {
            test_check(false, "the wind reached 10 m/s (%.1f)", GW.world.wind);
            test_end();
        }
        return;
    }
    int since = f - GW.t.ticks;
    if (since > SIM_HZ && since <= 31 * SIM_HZ) {
        GW.t.total++;
        if (m->mode == MOVE_SWIM && !m->head_under) GW.t.above++;
    }
    if (since == 31 * SIM_HZ) {
        float ratio = (float)GW.t.above / (float)DG_MAX(GW.t.total, 1);
        test_check(ratio >= 0.95f, "swimmer's head above the waves on %.1f %% of %d ticks (>= 95 %%)", ratio * 100.0f, GW.t.total);
        // CPU == GPU swell.
        enum { N = 512 };
        static float xzt[N * 3], gpu[N];
        u32 st = 0x1234567u;
        float t = waves_time(w->tick, 0.0f, SIM_HZ);
        for (int i = 0; i < N; i++) {
            st = st * 1664525u + 1013904223u;
            float a = (float)(st >> 8) / 16777216.0f * 6.2831853f;
            st = st * 1664525u + 1013904223u;
            float r = (float)(st >> 8) / 16777216.0f * 300.0f;
            xzt[3 * i] = m->pos.x + cosf(a) * r;
            xzt[3 * i + 1] = m->pos.z + sinf(a) * r;
            xzt[3 * i + 2] = t + (float)(i % 7) * 1.37f;
        }
        if (render_water_gpu_heights(xzt, N, gpu)) {
            float worst = 0.0f, mean = 0.0f;
            for (int i = 0; i < N; i++) {
                float c = waves_height(&GW.world.waves, xzt[3 * i], xzt[3 * i + 1], xzt[3 * i + 2], 2);
                float e = fabsf(c - gpu[i]);
                worst = fmaxf(worst, e);
                mean += e / (float)N;
            }
            test_check(worst < 0.01f, "GPU swell == CPU swell at %d points: worst %.5f m, mean %.6f m (< 1 cm)", N, worst, mean);
        } else {
            test_check(false, "GPU probe ran");
        }
        test_end();
    }
}

static void test_tick(World *w) {
    if (GW.t.mode == WT_SWIM) { test_swim(w); return; }
    if (GW.t.mode == WT_WAVES) { test_waves(w); return; }
    if (--GW.t.ticks > 0) return;
    if (GW.t.mode == WT_PROPS) test_props(w);
    log_info("water_test done");
    GW.t.mode = WT_NONE;
    if (GW.t.quit) cmd_execute_line("quit");
}

// ------------------------------------------------------------------ debug view

static void debug_frame(World *w, vec3 cam) {
    int mode = GW.debug ? GW.debug->i : 0;
    if (mode <= 0) return;
    float t = game_water_time(w->authority ? (double)w->tick + (double)G->alpha : net_client_render_tick());
    // Bodies: outlines at their level.
    for (int i = 0; i < GW.world.count; i++) {
        const WaterBody *b = &GW.world.body[i];
        u32 col = b->kind == WATER_OCEAN ? 0xFFFF9020u : (b->kind == WATER_RIVER ? 0xFF40C0FFu : 0xFFFFC040u);
        if (b->shape == WSHAPE_POLYGON)
            for (int k = 0; k < b->poly_n; k++) {
                int j = (k + 1) % b->poly_n;
                dbg_line(v3(b->poly[k][0], b->level + 0.02f, b->poly[k][1]), v3(b->poly[j][0], b->level + 0.02f, b->poly[j][1]), col, 0, true);
            }
        else if (b->shape == WSHAPE_ELLIPSE)
            dbg_circle(v3(b->center[0], b->level + 0.02f, b->center[1]), v3(0, 1, 0), fmaxf(b->radius[0], b->radius[1]), col, 0);
    }
    // Surface samples around the camera.
    if (mode >= 2) {
        for (int z = -12; z <= 12; z++)
            for (int x = -12; x <= 12; x++) {
                float px = floorf(cam.x) + (float)x, pz = floorf(cam.z) + (float)z;
                WaterSample s;
                if (!water_sample(&GW.world, px, pz, t, &s)) continue;
                vec3 p = v3(px, s.height, pz);
                dbg_line(p, v3_add(p, v3(s.normal[0] * 0.4f, s.normal[1] * 0.4f, s.normal[2] * 0.4f)), 0xFFFFFF00u, 0, true);
                dbg_cross(p, 0.06f, s.jacobian < 0.4f ? 0xFFFFFFFFu : 0xFFFF8000u, 0);
            }
    }
    // Buoyancy (host): probes coloured by submersion, the net force.
    if (!w->authority) return;
    for (int k = 0; k < GW.buoy.count; k++) {
        const Buoyancy *b = &GW.buoy.item[k];
        vec3 pos;
        quat rot;
        phys_get_transform(w->phys, b->body, &pos, &rot);
        if (v3_dist(pos, cam) > 60.0f) continue;
        for (int i = 0; i < b->n; i++) {
            vec3 pw = v3_add(pos, quat_rotate(rot, b->probe[i].local));
            bool wet = b->in_water && pw.y < b->water_h;
            dbg_sphere(pw, 0.04f, wet ? 0xFFFF6020u : 0xFF808080u, 0);
        }
        if (b->in_water) {
            vec3 com = phys_body_com(w->phys, b->body);
            dbg_arrow(com, v3_add(com, v3_scale(b->impulse, 60.0f / (b->mass * 9.81f))), 0xFF20FF20u, 0);
        }
    }
}

// The renderer's water for this frame: the bodies' look, the swell, wind, rain, ripple sources.
static void render_frame(World *w, vec3 cam) {
    static RenderWater rw;
    memset(&rw, 0, sizeof(rw));
    rw.enabled = GW.world.count > 0;
    rw.ocean_body = GW.world.ocean;
    rw.tier = -1;
    if (!rw.enabled) { render_set_water(&rw); return; }
    double tick = w->authority ? (double)w->tick + (double)G->alpha : net_client_render_tick();
    rw.wave_time = game_water_time(tick);
    rw.body_count = DG_MIN(GW.world.count, RENDER_WATER_BODIES);
    for (int i = 0; i < rw.body_count; i++) {
        const WaterBody *b = &GW.world.body[i];
        RenderWaterBody *o = &rw.body[i];
        o->absorb = v4(b->p.absorb[0], b->p.absorb[1], b->p.absorb[2], b->p.turbidity);
        o->scatter = v4(b->p.scatter[0], b->p.scatter[1], b->p.scatter[2], b->p.roughness);
        o->params = v4(b->level, (float)b->kind, b->p.wave_scale, b->p.foam);
        o->flow = v4(b->flow[0], b->flow[1], b->p.wind_coupling, 0.0f);
    }
    if (GW.world.ocean >= 0) rw.ocean_level = GW.world.body[GW.world.ocean].level;
    const WaveModel *m = &GW.world.waves;
    rw.wave_count = GW.world.ocean >= 0 ? DG_MIN(m->active_n, RENDER_WATER_WAVES) : 0;
    for (int i = 0; i < rw.wave_count; i++) {
        const WaveActive *a = &m->act[i];
        rw.waves[2 * i] = v4(a->dx, a->dz, a->k, a->omega);
        rw.waves[2 * i + 1] = v4(a->amp, a->qa, a->phase, 0.0f);
    }
    float th = GW.world.wind_dir_deg * DG_DEG2RAD;
    rw.wind_dir = v2(sinf(th), -cosf(th));
    rw.wind = GW.world.wind;
    WeatherParams wp;
    if (weather_current(w, &wp)) rw.rain = wp.v[WP_PRECIP] * (1.0f - weather_precip_type(wp.v[WP_TEMPERATURE]));
    rw.ripple_center = cam;
    // Ripple sources: players in water (wading legs, swimmers), buoyant props at the surface.
    for (int s = 0; s < MAX_PLAYERS && rw.ripple_count < RENDER_WATER_RIPPLES; s++) {
        const Player *p = &w->players[s];
        if (!p->active || p->sim.move.water == WATER_DRY || p->sim.move.mode == MOVE_DEAD) continue;
        const PlayerMove *pm = &p->sim.move;
        if (pm->head_under && pm->water == WATER_UNDER && pm->water_surface - (pm->pos.y + 1.8f) > 0.6f) continue;   // deep diver
        float speed = v3_len(v3_flat(pm->vel));
        float push = 0.0015f + speed * 0.0025f;
        RenderWaterRipple *r = &rw.ripples[rw.ripple_count++];
        r->pos_radius_strength = v4(pm->pos.x, pm->pos.z, pm->mode == MOVE_SWIM ? 0.55f : 0.35f, push);
        r->vel = v4(pm->vel.x, pm->vel.z, 0, 0);
    }
    if (w->authority)
        for (int k = 0; k < GW.buoy.count && rw.ripple_count < RENDER_WATER_RIPPLES; k++) {
            const Buoyancy *b = &GW.buoy.item[k];
            if (!b->in_water || b->submerged > 0.98f) continue;
            vec3 pos;
            phys_get_transform(w->phys, b->body, &pos, NULL);
            if (v3_dist(pos, cam) > 40.0f) continue;
            vec3 vel = phys_get_velocity(w->phys, b->body);
            float push = fminf(0.02f, fabsf(vel.y) * 0.01f + v3_len(v3_flat(vel)) * 0.003f);
            if (push < 2e-4f) continue;
            RenderWaterRipple *r = &rw.ripples[rw.ripple_count++];
            r->pos_radius_strength = v4(pos.x, pos.z, fminf(b->extent, 1.5f), push);
            r->vel = v4(vel.x, vel.z, 0, 0);
        }
    render_set_water(&rw);
}

void water_frame(World *w, vec3 cam) {
    if (!GW.loaded || !w) return;
    if (!w->authority) update_wind(w);
    render_frame(w, cam);
    debug_frame(w, cam);
}

// ------------------------------------------------------------------ commands

static World *host_world(const char *cmd) {
    World *w = G ? G->world : NULL;
    if (!w || !w->authority) { log_info("%s: host only", cmd); return NULL; }
    return w;
}

// water_level <body|index> <level>: moves a body's surface (dev; the host's bodies only).
static void cmd_water_level(int argc, const char **argv) {
    if (argc < 3) {
        for (int i = 0; i < GW.world.count; i++) {
            const WaterBody *b = &GW.world.body[i];
            log_info("  %d %-12s %-6s level %.2f preset %s", i, b->name, water_kind_name((WaterKind)b->kind), b->level, b->p.preset);
        }
        log_info("usage: water_level <body|index> <level>");
        return;
    }
    if (!host_world("water_level")) return;
    int bi = -1;
    for (int i = 0; i < GW.world.count; i++)
        if (!strcmp(GW.world.body[i].name, argv[1])) bi = i;
    if (bi < 0 && argv[1][0] >= '0' && argv[1][0] <= '9') bi = atoi(argv[1]);
    if (bi < 0 || bi >= GW.world.count) { log_info("water_level: no body '%s'", argv[1]); return; }
    game_water_set_level(bi, (float)atof(argv[2]));
    log_info("water_level: %s at %.2f m", GW.world.body[bi].name, GW.world.body[bi].level);
}

void game_water_set_level(int body, float level) {
    if (body < 0 || body >= GW.world.count) return;
    GW.world.body[body].level = level;
    if (G && G->world && !openworld_active(G->world)) bake_ground(G->world);
}

// wind_set <m/s> [dir_deg] [seconds]: the weather wind (drives waves, foliage, rain).
static void cmd_wind_set(int argc, const char **argv) {
    if (argc < 2) { log_info("usage: wind_set <m/s> [dir_deg] [seconds]"); return; }
    if (!host_world("wind_set")) return;
    char line[160];
    snprintf(line, sizeof(line), "weather_params wind %s%s%s time %s", argv[1], argc > 2 ? " wind_dir " : "", argc > 2 ? argv[2] : "", argc > 3 ? argv[3] : "2");
    cmd_execute_line(line);
}

// spawn_float <density> [size] [box|plank|sphere|barrel]: a buoyant prop 3 m in front of the player.
static void cmd_spawn_float(int argc, const char **argv) {
    World *w = host_world("spawn_float");
    if (!w) return;
    Player *p = game_local_player();
    if (!p) return;
    float density = argc > 1 ? (float)atof(argv[1]) : 500.0f;
    float size = argc > 2 ? (float)atof(argv[2]) : 0.8f;
    const char *shape = argc > 3 ? argv[3] : "box";
    if (density <= 1.0f || size <= 0.05f) { log_info("usage: spawn_float <density kg/m3> [size m] [box|plank|sphere|barrel]"); return; }
    vec3 half = v3s(size * 0.5f);
    VisShape vs = VIS_BOX;
    float vol;
    if (!strcmp(shape, "plank")) half = v3(size, size * 0.08f, size * 0.25f);
    if (!strcmp(shape, "sphere")) vs = VIS_SPHERE;
    if (!strcmp(shape, "barrel")) { vs = VIS_CYLINDER; half = v3(size * 0.35f, size * 0.5f, size * 0.35f); }
    if (vs == VIS_SPHERE) vol = 4.18879f * half.x * half.x * half.x;
    else if (vs == VIS_CYLINDER) vol = DG_PI * half.x * half.x * 2.0f * half.y;
    else vol = 8.0f * half.x * half.y * half.z;
    vec3 fwd = forward_from_yaw(p->sim.move.yaw);
    vec3 pos = v3_add(v3_add(p->sim.move.pos, v3_scale(fwd, 3.0f)), v3(0, 1.5f, 0));
    u32 color = density < 1000.0f ? rgba8(0.75f, 0.55f, 0.3f, 1) : rgba8(0.45f, 0.45f, 0.5f, 1);
    EntityId e = spawn_prop(w, pos, quat_yaw(p->sim.move.yaw), vs, half, color, density * vol);
    w->body[ENT_INDEX(e)].density = density;
    log_info("spawn_float: %s %.2f m, %.0f kg/m3 (%.1f kg) -> %s", shape, size, density, density * vol, density < 1000.0f ? "floats" : "sinks");
}

static void cmd_water_info(int argc, const char **argv) {
    (void)argc; (void)argv;
    const WaveModel *m = &GW.world.waves;
    log_info("water: %d bodies, wind %.1f m/s towards %.0f deg, swell Hs %.2f m peak %.0f m (%d components, steepness %.2f)", GW.world.count,
             GW.world.wind, GW.world.wind_dir_deg, m->hs, m->peak_len, m->active_n, m->steepness);
    log_info("water: %d buoyant bodies, last step %.3f ms (read %.3f compute %.3f apply %.3f)", GW.buoy.count, GW.buoy.last_ms, GW.buoy.read_ms,
             GW.buoy.compute_ms, GW.buoy.apply_ms);
    Player *p = game_local_player();
    if (p) {
        WaterSample s;
        World *w = G->world;
        double tick = w->authority ? (double)w->tick : net_client_render_tick();
        if (game_water_sample(p->sim.move.pos.x, p->sim.move.pos.z, tick, &s))
            log_info("water: at the player: %s '%s' surface %.2f depth %.2f", water_kind_name((WaterKind)s.kind), GW.world.body[s.body].name, s.height, s.depth);
        else log_info("water: no water at the player");
    }
}

// water_test props [seconds] [quit]
static void cmd_water_test(int argc, const char **argv) {
    if (argc < 2) { log_info("usage: water_test props|swim|waves [seconds] [quit]"); return; }
    if (!host_world("water_test")) return;
    GW.t.quit = argc > 2 && !strcmp(argv[argc - 1], "quit");
    float sec = argc > 2 && strcmp(argv[2], "quit") ? (float)atof(argv[2]) : 15.0f;
    GW.t.frame = GW.t.fails = GW.t.checks = 0;
    if (!strcmp(argv[1], "props")) GW.t.mode = WT_PROPS;
    else if (!strcmp(argv[1], "swim")) GW.t.mode = WT_SWIM;
    else if (!strcmp(argv[1], "waves")) GW.t.mode = WT_WAVES;
    else { log_info("water_test: unknown mode '%s'", argv[1]); return; }
    GW.t.ticks = (int)(sec * SIM_HZ);
    log_info("water_test %s: checking in %.0f s", argv[1], sec);
}

void water_register(void) {
    GW.debug = cvar_reg_int("water_debug", 0, 0, 2, 0, "Water debug view: 1 bodies + buoyancy probes / forces, 2 + surface samples");
    GW.drown_dps = cvar_reg_float("water_drown_damage", 10.0f, 0.0f, 1000.0f, CVAR_SERVER, "Drowning damage per second once a player's breath runs out");
    movement_set_water(move_water);
    GW.wave_scale = cvar_reg_float("water_wave_wind_scale", 1.0f, 0.0f, 4.0f, CVAR_CHEAT, "Multiplier on the weather wind fed to the ocean swell (tests)");
    cmd_register("water_level", cmd_water_level, "water_level [<body|index> <level>]: list the water bodies / move one's surface (host)");
    cmd_register("wind_set", cmd_wind_set, "wind_set <m/s> [dir_deg] [seconds]: set the weather wind (waves, foliage, rain) (host)");
    cmd_register("spawn_float", cmd_spawn_float, "spawn_float <density> [size] [box|plank|sphere|barrel]: buoyant prop in front of you (host)");
    cmd_register("water_test", cmd_water_test, "water_test props|swim|waves [seconds] [quit]: in-game water checks (logs ok / FAIL)");
    cmd_register("water_info", cmd_water_info, "water_info: bodies, wind, swell, buoyancy cost, the water at the player");
}
