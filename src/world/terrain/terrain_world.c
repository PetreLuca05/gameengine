#include "terrain_world.h"
#include "terrain_maps.h"
#include "core/log.h"
#include "render/render.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static struct {
    TerrainParams p;
    bool loaded;
    int n;
    float xf[4];
    int level, debug;
    float grass[2];
} TW;

static void push_params(void) {
    TW.p.gpu.g.tier[0] = (float)TW.level;
    TW.p.gpu.g.tier[1] = (float)TW.debug;
    TW.p.gpu.g.grass[0] = TW.grass[0];
    TW.p.gpu.g.grass[1] = TW.grass[1];
    memcpy(TW.p.gpu.g.maps, TW.xf, sizeof(TW.xf));
    render_terrain_params(&TW.p.gpu, sizeof(TW.p.gpu));
}

// Ground colours of the grass shading and the macro variation follow the terrain's layers.
static void push_ground(void) {
    for (int s = 0; s < TER_SLOTS; s++)
        if (TW.p.slot_layer[s][0]) render_ground_set_color(s, TW.p.mean_albedo[s]);
    render_ground_set_macro(TW.p.ground_macro);
}

void terrain_world_load(const float *h, const u8 *mat, int n, float origin_x, float origin_z, float spacing) {
    u64 t0 = SDL_GetTicksNS();
    TerrainParams *np = (TerrainParams *)malloc(sizeof(TerrainParams));
    if (!terrain_params_load(np, TER_DIR)) {
        log_warn("terrain: %s (the older surface blend stays)", np->err);
        free(np);
        return;
    }
    TW.p = *np;
    free(np);
    render_terrain_load((const char(*)[48])TW.p.tex, TW.p.tex_count, TW.p.scatter_atlas);
    u64 t1 = SDL_GetTicksNS();
    TerrainMapParams mp;
    terrain_map_params_default(&mp);
    mp.spacing = spacing;
    mp.blend_radius = DG_MAX(1, (int)lrintf(TW.p.gpu.g.tier[3] / spacing));
    TW.p.gpu.g.tier[3] = (float)mp.blend_radius * spacing;   // the radius the map was baked with
    u8 *blend = (u8 *)malloc((size_t)n * n * 4), *attr = (u8 *)malloc((size_t)n * n * 4);
    terrain_maps_build(h, mat, n, &mp, blend, attr);
    render_terrain_maps(blend, attr, n, origin_x, origin_z, spacing);
    free(blend);
    free(attr);
    TW.n = n;
    TW.xf[0] = origin_x, TW.xf[1] = origin_z, TW.xf[2] = 1.0f / spacing, TW.xf[3] = (float)n;
    TW.loaded = true;
    TW.level = 4;
    TW.grass[0] = 45.0f, TW.grass[1] = 70.0f;
    push_params();
    push_ground();
    log_info("terrain: %d layers, %d textures, %d pairs; maps %d x %d baked in %.0f ms (textures %.0f ms)", 14, TW.p.tex_count, TW.p.pair_count, n, n,
             (double)(SDL_GetTicksNS() - t1) / 1e6, (double)(t1 - t0) / 1e6);
}

void terrain_world_unload(void) {
    if (!TW.loaded) return;
    TW.loaded = false;
    render_terrain_maps(NULL, NULL, 0, 0, 0, 1);
}

void terrain_world_frame(int level, int debug, float grass_start, float grass_end) {
    if (!TW.loaded) return;
    if (level == TW.level && debug == TW.debug && grass_start == TW.grass[0] && grass_end == TW.grass[1]) return;
    TW.level = level;
    TW.debug = debug;
    TW.grass[0] = grass_start;
    TW.grass[1] = grass_end;
    push_params();
}

bool terrain_world_reload(void) {
    if (!TW.loaded) return false;
    TerrainParams *np = (TerrainParams *)malloc(sizeof(TerrainParams));
    if (!terrain_params_load(np, TER_DIR)) {
        log_warn("terrain: reload failed: %s", np->err);
        free(np);
        return false;
    }
    bool tex_changed = np->tex_count != TW.p.tex_count || memcmp(np->tex, TW.p.tex, sizeof(np->tex)) || strcmp(np->scatter_atlas, TW.p.scatter_atlas);
    float radius = TW.p.gpu.g.tier[3];
    TW.p = *np;
    free(np);
    TW.p.gpu.g.tier[3] = radius;   // the maps keep the radius they were baked with
    if (tex_changed) render_terrain_load((const char(*)[48])TW.p.tex, TW.p.tex_count, TW.p.scatter_atlas);
    push_params();
    push_ground();
    log_info("terrain: reloaded (%d textures%s, %d pairs)", TW.p.tex_count, tex_changed ? ", textures reloaded" : "", TW.p.pair_count);
    return true;
}

i64 terrain_world_mtime(void) { return TW.loaded ? terrain_params_mtime(TER_DIR) : 0; }
bool terrain_world_loaded(void) { return TW.loaded; }
const TerrainParams *terrain_world_params(void) { return TW.loaded ? &TW.p : NULL; }
void terrain_world_map_xf(float out[4]) { memcpy(out, TW.xf, sizeof(TW.xf)); }
