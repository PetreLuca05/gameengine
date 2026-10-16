// Foliage, game side (docs/VGEO.md "Foliage support"): the devroom test scatter (foliage_test_scatter:
// placement chunks of the procedural test types over the foliage yard, their trunk capsules in the
// level's static physics), players flattening grass (the displacement buffer), clearing on map load.
// Streamed worlds load / free their chunks' placements in src/world/chunks.c.
#include "game_internal.h"
#include "physics/physics.h"
#include "render/render.h"
#include "world/chunks.h"
#include "core/log.h"
#include "core/engine.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FOL_MAX_CHUNKS 512
#define FOL_MAX_BODIES 65536

static struct {
    int handles[FOL_MAX_CHUNKS];
    int count;
    BodyId *bodies;
    int nbodies, ncapsules;
    World *world;                // the world the bodies were added to
} FG;

// Static trunk capsules of a loaded chunk into the world's physics (one body per chunk).
static int foliage_add_capsules(World *w, int handle, BodyId *bodies, int max) {
    static RenderCapsule caps[4096];
    int n = render_foliage_chunk_capsules(handle, caps, 4096);
    BodyId b = max > 0 ? chunks_capsule_body(w, caps, n) : PHYS_INVALID_BODY;
    if (b == PHYS_INVALID_BODY) return 0;
    bodies[0] = b;
    return n;
}

void foliage_clear(void) {
    for (int i = 0; i < FG.count; i++) render_foliage_chunk_unload(FG.handles[i]);
    FG.count = 0;
    if (FG.world && FG.world->phys)
        for (int i = 0; i < FG.nbodies; i++) phys_remove_body(FG.world->phys, FG.bodies[i]);
    FG.nbodies = 0;
    FG.world = NULL;
}

// foliage_test_scatter [count=300000]: the procedural tuft (90 %) and bush (10 %) over the devroom
// foliage yard (x 60..380, z 80..400, behind the 8 m wall at z = 76), as placement chunks.
static void cmd_foliage_test_scatter(int argc, const char **argv) {
    World *w = game_world();
    if (!w || !w->phys) { log_warn("foliage_test_scatter: no world"); return; }
    u32 count = argc > 1 ? (u32)atoi(argv[1]) : 300000u;
    if (!render_foliage_test_types()) { log_warn("foliage_test_scatter: vgeo unavailable"); return; }
    foliage_clear();
    if (!FG.bodies) FG.bodies = (BodyId *)malloc(sizeof(BodyId) * FOL_MAX_BODIES);
    FG.world = w;
    const float x0 = 60, z0 = 80, x1 = 380, z1 = 400;
    float density = (float)count / ((x1 - x0) * (z1 - z0));
    u32 total = 0;
    for (int cz = (int)floorf(z0 / 32.0f); cz < (int)ceilf(z1 / 32.0f); cz++)
        for (int cx = (int)floorf(x0 / 32.0f); cx < (int)ceilf(x1 / 32.0f); cx++) {
            size_t size = 0;
            void *blob = render_foliage_test_chunk(cx, cz, density, x0, z0, x1, z1, NULL, NULL, &size);
            if (!blob) continue;
            int h = render_foliage_chunk_load(blob, size);
            free(blob);
            if (h < 0 || FG.count >= FOL_MAX_CHUNKS) continue;
            FG.handles[FG.count++] = h;
            int nc = foliage_add_capsules(w, h, FG.bodies + FG.nbodies, FOL_MAX_BODIES - FG.nbodies);
            if (nc) FG.nbodies++;
            FG.ncapsules += nc;
            total += (u32)size;
        }
    phys_optimize(w->phys);
    log_info("foliage_test_scatter: %u instances requested (%.2f per m2) in %d placement chunks (%.1f MB of .vgp), %d trunk capsules in %d static bodies", count,
             (double)density, FG.count, (double)total / (1024.0 * 1024.0), FG.ncapsules, FG.nbodies);
}

// foliage_grove [spacing=16]: every imported species (data/foliage/species.json: the Tree It trees)
// in rows across the foliage yard (x 80..360, z 130..230, facing the foliage_check camera at z 120),
// 3 instances each with scale jitter, daisies scattered between them; placement chunks + trunk capsules.
static void cmd_foliage_grove(int argc, const char **argv) {
    World *w = game_world();
    if (!w || !w->phys) { log_warn("foliage_grove: no world"); return; }
    float spacing = argc > 1 ? (float)atof(argv[1]) : 16.0f;
    int ns = render_foliage_species_types();
    if (!ns) { log_warn("foliage_grove: no species (vgeo unavailable or data/foliage/species.json missing)"); return; }
    if (!FG.bodies) FG.bodies = (BodyId *)malloc(sizeof(BodyId) * FOL_MAX_BODIES);
    FG.world = w;
    static char names[32][48];
    int trees = 0, daisy = -1;
    for (int i = 0; i < ns && i < 32; i++) {
        dg_strcpy(names[i], 48, render_foliage_species_name(i));
        if (strstr(names[i], "daisy")) daisy = i;
        else trees++;
    }
    static RenderFoliageInst inst[4096];
    int n = 0, row = 0, col = 0, per_row = (int)(280.0f / spacing);
    u32 h = 0x9E3779B9u;
    for (int i = 0; i < ns && i < 32; i++) {
        if (i == daisy) continue;
        for (int k = 0; k < 3 && n < 4096; k++) {
            h = h * 1664525u + 1013904223u;
            RenderFoliageInst *t = &inst[n++];
            t->pos = v3(80.0f + ((float)col + 0.5f) * spacing + (float)((h >> 8) & 255) / 255.0f * 3.0f - 1.5f, 0.0f, 130.0f + ((float)row + 0.5f) * spacing);
            t->yaw = (float)((h >> 16) & 1023) / 1023.0f * 6.2831853f;
            t->scale = 0.85f + 0.3f * (float)((h >> 4) & 255) / 255.0f;
            t->kind = (u16)i;
            if (++col >= per_row) { col = 0; row++; }
        }
    }
    float z1 = 130.0f + (float)(row + 1) * spacing;
    for (int k = 0; daisy >= 0 && k < 600 && n < 4096; k++) {
        h = h * 1664525u + 1013904223u;
        RenderFoliageInst *t = &inst[n++];
        t->pos = v3(80.0f + (float)(h >> 8) / 16777216.0f * 280.0f, 0.0f, 125.0f + (float)((h * 2654435761u) >> 8) / 16777216.0f * (z1 - 125.0f));
        t->yaw = (float)(h & 1023) / 1023.0f * 6.2831853f;
        t->scale = 0.7f + 0.5f * (float)((h >> 3) & 255) / 255.0f;
        t->kind = (u16)daisy;
    }
    int chunks = 0, nc_total = 0;
    for (int cz = (int)floorf(120.0f / 32.0f); cz <= (int)floorf(z1 / 32.0f); cz++)
        for (int cx = (int)floorf(80.0f / 32.0f); cx <= (int)floorf(360.0f / 32.0f); cx++) {
            size_t size = 0;
            void *blob = render_foliage_make_chunk(cx, cz, (const char (*)[48])names, DG_MIN(ns, 32), inst, n, &size);
            if (!blob) continue;
            int hd = render_foliage_chunk_load(blob, size);
            free(blob);
            if (hd < 0 || FG.count >= FOL_MAX_CHUNKS) continue;
            FG.handles[FG.count++] = hd;
            chunks++;
            int nc = foliage_add_capsules(w, hd, FG.bodies + FG.nbodies, FOL_MAX_BODIES - FG.nbodies);
            if (nc) FG.nbodies++;
            FG.ncapsules += nc;
            nc_total += nc;
        }
    phys_optimize(w->phys);
    log_info("foliage_grove: %d species (%d trees x 3, daisies), %d instances in %d placement chunks, %d trunk capsules", ns, trees, n, chunks, nc_total);
}

void foliage_level_reset(void) {
    for (int i = 0; i < FG.count; i++) render_foliage_chunk_unload(FG.handles[i]);
    FG.count = 0;
    FG.nbodies = 0;
    FG.world = NULL;
}

static void cmd_foliage_test_clear(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    foliage_clear();
    log_info("foliage: test placements cleared");
}

void foliage_register(void) {
    chunks_register_cvars();   // world_foliage_test must exist before a streamed map loads
    cmd_register("foliage_test_scatter", cmd_foliage_test_scatter, "foliage_test_scatter [count=300000]: procedural grass + bushes over the devroom foliage yard (placement chunks)");
    cmd_register("foliage_test_clear", cmd_foliage_test_clear, "remove the foliage_test_scatter placements");
    cmd_register("foliage_grove", cmd_foliage_grove, "foliage_grove [spacing=16]: every imported species (Tree It trees, data/foliage/species.json) in the devroom foliage yard");
}

// Per frame: every player on the ground flattens the grass around its feet (moving harder).
void foliage_frame(World *w) {
    if (!w) return;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        const Player *p = &w->players[i];
        if (!p->active || !p->sim.move.on_ground) continue;
        float speed = sqrtf(p->sim.move.vel.x * p->sim.move.vel.x + p->sim.move.vel.z * p->sim.move.vel.z);
        render_foliage_push(p->sim.move.pos, 0.7f, 0.6f + 0.4f * speed);
    }
}
