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
    cmd_register("foliage_test_scatter", cmd_foliage_test_scatter, "foliage_test_scatter [count=300000]: procedural grass + bushes over the devroom foliage yard (placement chunks)");
    cmd_register("foliage_test_clear", cmd_foliage_test_clear, "remove the foliage_test_scatter placements");
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
