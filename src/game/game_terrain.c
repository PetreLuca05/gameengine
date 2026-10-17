// Terrain shading, game side (docs/TERRAIN_SHADING.md): the level (r_terrain_quality, bounded by Engine
// Fidelity), the debug views (r_terrain_debug), the grass term's distances (the grass assets' fade at
// this level), hot reload of assets/terrain JSON (r_terrain_watch), the terrain tool (terrain_paint: tint /
// variant / wear / moisture brush at the camera; terrain_paint_save) and live parameter edits (terrain_set).
#include "game_internal.h"
#include "world/terrain/terrain_world.h"
#include "render/render.h"
#include "core/log.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "stb_image_write.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    Cvar *quality, *debug, *watch;
    i64 mtime;
    double next_poll;
    vec3 cam;
} TG;

static void cmd_reload(int argc, const char **argv) {
    (void)argc, (void)argv;
    if (!terrain_world_reload()) log_info("terrain_reload: no terrain loaded (open-world maps) or a parse error");
    TG.mtime = terrain_world_mtime();
}

static void cmd_status(int argc, const char **argv) {
    (void)argc, (void)argv;
    const TerrainParams *p = terrain_world_params();
    if (!p) { log_info("terrain: not loaded (open-world maps only)"); return; }
    log_info("terrain: level %d (r_terrain_quality), debug %d, %d textures, %d pairs, active %d", TG.quality ? TG.quality->i : -1, TG.debug ? TG.debug->i : 0, p->tex_count,
             p->pair_count, render_terrain_active());
    for (int s = 0; s < TER_SLOTS; s++)
        if (p->slot_layer[s][0])
            log_info("  slot %2d %-14s tex %2.0f macro %2.0f tile %.2f m  mean %.3f %.3f %.3f", s, p->slot_layer[s], p->gpu.layer[s].tex[0], p->gpu.layer[s].tex[1],
                     1.0f / p->gpu.layer[s].tex[2], p->mean_albedo[s][0], p->mean_albedo[s][1], p->mean_albedo[s][2]);
}

// terrain_paint <tint_r|tint_g|tint_b|tint|variant|wear|moisture> <value 0..1> [radius m] [strength 0..1]
static void cmd_paint(int argc, const char **argv) {
    if (argc < 3) { log_info("usage: terrain_paint <tint_r|tint_g|tint_b|tint|variant|wear|moisture> <value 0..1> [radius m] [strength]"); return; }
    float xf[4];
    terrain_world_map_xf(xf);
    if (!terrain_world_loaded()) { log_info("terrain_paint: no terrain loaded"); return; }
    float v = (float)atof(argv[2]), r = argc > 3 ? (float)atof(argv[3]) : 3.0f, k = argc > 4 ? (float)atof(argv[4]) : 0.5f;
    static const char *const names[] = { "tint_r", "tint_g", "tint_b", "variant", "wear", "moisture" };
    if (!strcmp(argv[1], "tint")) {   // a grey tint: all three channels
        for (int c = 0; c < 3; c++) render_terrain_paint(TG.cam.x, TG.cam.z, r, c, v, k, xf);
        return;
    }
    for (int c = 0; c < 6; c++)
        if (!strcmp(argv[1], names[c])) {
            render_terrain_paint(TG.cam.x, TG.cam.z, r, c, v, k, xf);
            return;
        }
    log_info("terrain_paint: unknown channel %s", argv[1]);
}

static void cmd_paint_save(int argc, const char **argv) {
    int n = 0;
    const u8 *px = render_terrain_paint_data(&n);
    if (!px) { log_info("terrain_paint_save: no terrain loaded"); return; }
    char path[512];
    snprintf(path, sizeof(path), "%s", argc > 1 ? argv[1] : "terrain_paint.png");
    if (stbi_write_png(path, n, n, 4, px, n * 4)) log_info("terrain_paint_save: %s (%d x %d: tint rgb, variant mask)", path, n, n);
    else log_info("terrain_paint_save: cannot write %s", path);
}

// terrain_set <slot> <field> <index 0..3> <value>: one float of a layer's GPU block, live (not saved).
static void cmd_set(int argc, const char **argv) {
    static const char *const fields[] = { "tex", "tiling", "multi", "hsv", "blend", "curv", "moist", "slope", "ranges", "alt", "aspect", "var0", "var1", "varn", "detail",
                                          "snow", "scatter", "mean" };
    const TerrainParams *p = terrain_world_params();
    if (!p || argc < 5) { log_info("usage: terrain_set <slot 0..15> <field> <0..3> <value> (fields: tex tiling multi hsv blend curv moist slope ranges alt aspect var0 var1 varn detail snow scatter mean)"); return; }
    int slot = atoi(argv[1]), idx = atoi(argv[3]);
    for (int f = 0; f < (int)(sizeof(fields) / sizeof(fields[0])); f++) {
        if (strcmp(fields[f], argv[2])) continue;
        if (slot < 0 || slot >= TER_SLOTS || idx < 0 || idx > 3) break;
        TerrainParams *mp = (TerrainParams *)p;
        ((float *)&mp->gpu.layer[slot])[f * 4 + idx] = (float)atof(argv[4]);
        render_terrain_params(&mp->gpu, sizeof(mp->gpu));
        log_info("terrain_set: slot %d %s[%d] = %s (live; edit assets/terrain/layers to keep it)", slot, fields[f], idx, argv[4]);
        return;
    }
    log_info("terrain_set: unknown field / index");
}

void terrain_game_register(void) {
    TG.debug = cvar_reg_int("r_terrain_debug", 0, 0, 11, 0,
                            "terrain debug view: 1 weights, 2 bombing cells, 3 macro field, 4 curvature, 5 moisture, 6 variants, 7 transition band, 8 tiling frequency, 9 scatter, 10 grass term, 11 border distance");
    TG.watch = cvar_reg_int("r_terrain_watch", 1, 0, 1, 0, "reload assets/terrain JSON when it changes (checked once a second)");
    cmd_register("terrain_reload", cmd_reload, "reload assets/terrain (terrain.json, layers, transitions; textures only when their list changed)");
    cmd_register("terrain_status", cmd_status, "terrain shading: level, slots, textures, pairs");
    cmd_register("terrain_paint", cmd_paint, "terrain_paint <tint_r|tint_g|tint_b|tint|variant|wear|moisture> <value> [radius] [strength]: paint brush at the camera");
    cmd_register("terrain_paint_save", cmd_paint_save, "terrain_paint_save [file.png]: write the painted map (tint rgb, variant mask)");
    cmd_register("terrain_set", cmd_set, "terrain_set <slot> <field> <index> <value>: live edit of a terrain layer parameter");
}

void terrain_game_frame(vec3 cam) {
    TG.cam = cam;
    if (!terrain_world_loaded()) return;
    if (!TG.quality) TG.quality = cvar_find("r_terrain_quality");
    int level = TG.quality ? DG_CLAMP(TG.quality->i, 0, 4) : 4;
    float gs = 45.0f, ge = 70.0f;
    render_grass_fade(level, &gs, &ge);
    terrain_world_frame(level, TG.debug ? TG.debug->i : 0, gs, ge);
    if (TG.watch && TG.watch->i && g_engine.real_time >= TG.next_poll) {
        TG.next_poll = g_engine.real_time + 1.0;
        i64 t = terrain_world_mtime();
        if (!TG.mtime) TG.mtime = t;
        else if (t && t != TG.mtime) {
            TG.mtime = t;
            terrain_world_reload();
        }
    }
}
