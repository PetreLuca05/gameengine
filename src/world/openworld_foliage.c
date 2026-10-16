// Trees and ground cover of open-world maps (docs/ZOMBIE_MAP.md "Vegetation"), on the vgeo foliage
// path (masked cards, wind WPO, impostors, placement chunks):
//   - trees (pine / broadleaf / dead) and forest underbrush (bushes) for the whole map at load, one
//     placement chunk per 32 m chunk (generated on the job system from mapgen_tree_density), their
//     trunk capsules merged into one static collision mesh per 128 m tile;
//   - grass tufts and crops per streamed terrain chunk (ow_foliage_chunk, thread-safe, by surface
//     material: grass / meadow / forest floor / fields), loaded and freed with the chunk by chunks.c.
#include "openworld_internal.h"
#include "chunks.h"
#include "core/jobs.h"
#include "core/log.h"
#include "render/render.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char k_tree_names[4][48] = { "tree_pine", "tree_broadleaf", "tree_dead", "test_bush" };
enum { T_PINE, T_BROAD, T_DEAD, T_BUSH };
static const char k_cover_names[3][48] = { "test_tuft", "crop_wheat", "test_bush" };
enum { C_TUFT, C_CROP, C_BUSH };

static u32 hash3(int x, int z, u32 salt) {
    u32 h = (u32)x * 73856093u ^ (u32)z * 19349663u ^ salt * 83492791u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return h;
}
static float h01(u32 h) { return (float)(h & 0xFFFFFFu) / 16777216.0f; }

typedef struct TreeJob {
    const OpenWorld *o;
    int n;                       // chunks per side
    int origin;                  // chunk coordinate of the min corner
    void **blob;
    size_t *size;
    int *count;
} TreeJob;

// Candidates on a jittered 3 m grid, kept with probability density x cell area.
static int tree_chunk(const OpenWorld *o, int cx, int cz, RenderFoliageInst *out, int cap) {
    const float cell = 3.0f;
    const int per = (int)(CHUNK_SIZE / cell);
    float x0 = (float)cx * CHUNK_SIZE, z0 = (float)cz * CHUNK_SIZE;
    int n = 0;
    for (int gz = 0; gz < per; gz++)
        for (int gx = 0; gx < per && n < cap; gx++) {
            int ix = cx * per + gx, iz = cz * per + gz;
            u32 h = hash3(ix, iz, o->gen.seed);
            float x = x0 + ((float)gx + h01(h)) * cell, z = z0 + ((float)gz + h01(h >> 7 ^ 0x9e37u)) * cell;
            if (fabsf(x) > o->gen.half_size + 60.0f || fabsf(z) > o->gen.half_size + 60.0f) continue;
            float pine;
            float d = mapgen_tree_density(&o->gen, &o->raster, x, z, &pine);
            if (d <= 0) continue;
            u32 h2 = hash3(ix, iz, o->gen.seed + 17u);
            float pick = h01(h2);
            RenderFoliageInst *t = &out[n];
            t->pos = v3(x, mapgen_raster_height(&o->raster, x, z) - 0.15f, z);
            t->yaw = h01(h2 >> 3) * 6.2831853f;
            if (pick < d * cell * cell) {
                float k = h01(h2 >> 11);
                t->kind = k < 0.035f ? T_DEAD : (k < pine ? T_PINE : T_BROAD);
                t->scale = 0.72f + 0.55f * h01(h2 >> 5);
                n++;
            } else if (d > 0.006f && pick < d * cell * cell * 1.6f) {   // forest underbrush
                t->kind = T_BUSH;
                t->scale = 1.0f + 0.9f * h01(h2 >> 5);
                n++;
            }
        }
    return n;
}

static void tree_range(void *ctx, int begin, int end) {
    TreeJob *j = (TreeJob *)ctx;
    RenderFoliageInst *tmp = (RenderFoliageInst *)malloc(sizeof(RenderFoliageInst) * 256);
    for (int i = begin; i < end; i++) {
        int cx = j->origin + i % j->n, cz = j->origin + i / j->n;
        int n = tree_chunk(j->o, cx, cz, tmp, 256);
        j->count[i] = n;
        j->blob[i] = n ? render_foliage_make_chunk(cx, cz, k_tree_names, 4, tmp, n, &j->size[i]) : NULL;
    }
    free(tmp);
}

void ow_foliage_load(World *w, OpenWorld *o) {
    if (!render_foliage_map_types()) { log_warn("openworld: vgeo foliage unavailable (no trees)"); return; }
    u64 t0 = SDL_GetTicksNS();
    TreeJob j;
    j.o = o;
    j.n = (int)ceilf(2.0f * o->gen.raster_half / CHUNK_SIZE);
    j.origin = (int)floorf(-o->gen.raster_half / CHUNK_SIZE);
    int total = j.n * j.n;
    j.blob = (void **)calloc((size_t)total, sizeof(void *));
    j.size = (size_t *)calloc((size_t)total, sizeof(size_t));
    j.count = (int *)calloc((size_t)total, sizeof(int));
    jobs_parallel_for(total, 16, tree_range, &j);
    u64 t1 = SDL_GetTicksNS();
    // load the chunks; trunk capsules into one static body per 128 m tile
    int tiles = (int)ceilf(2.0f * o->gen.raster_half / CHUNK_FAR_TILE), per = (int)(CHUNK_FAR_TILE / CHUNK_SIZE);
    RenderCapsule **caps = (RenderCapsule **)calloc((size_t)tiles * tiles, sizeof(RenderCapsule *));
    int *ncap = (int *)calloc((size_t)tiles * tiles, sizeof(int)), *ccap = (int *)calloc((size_t)tiles * tiles, sizeof(int));
    o->fol = (OwFolChunk *)calloc((size_t)total, sizeof(OwFolChunk));
    o->fol_count = 0;
    static RenderCapsule tmp[512];
    for (int i = 0; i < total; i++) {
        if (!j.blob[i]) continue;
        int h = render_foliage_chunk_load(j.blob[i], j.size[i]);
        free(j.blob[i]);
        if (h < 0) continue;
        int cx = j.origin + i % j.n, cz = j.origin + i / j.n;
        o->fol[o->fol_count++] = (OwFolChunk){ h, cx, cz };
        o->tree_instances += j.count[i];
        int nc = render_foliage_chunk_capsules(h, tmp, 512);
        int t = ((cz - j.origin) / per) * tiles + (cx - j.origin) / per;
        if (ncap[t] + nc > ccap[t]) {
            ccap[t] = DG_MAX(ccap[t] * 2, ncap[t] + nc + 256);
            caps[t] = (RenderCapsule *)realloc(caps[t], sizeof(RenderCapsule) * (size_t)ccap[t]);
        }
        memcpy(caps[t] + ncap[t], tmp, sizeof(RenderCapsule) * (size_t)nc);
        ncap[t] += nc;
    }
    int bodies = 0, capsules = 0;
    LevelStatic *ls = &w->level;
    for (int t = 0; t < tiles * tiles; t++) {
        if (ncap[t] && ls->body_count < MAX_STATIC_BODIES) {
            BodyId b = chunks_capsule_body(w, caps[t], ncap[t]);
            if (b != PHYS_INVALID_BODY) { ls->bodies[ls->body_count++] = b; bodies++; }
            capsules += ncap[t];
        }
        free(caps[t]);
    }
    free(caps);
    free(ncap);
    free(ccap);
    free(j.blob);
    free(j.size);
    free(j.count);
    o->fol_loaded = o->fol_count;
    o->fol_types = true;
    log_info("openworld: %d trees / bushes in %d placement chunks (scatter %.0f ms, load %.0f ms), %d trunk capsules in %d bodies", o->tree_instances, o->fol_count,
             (double)(t1 - t0) / 1e6, (double)(SDL_GetTicksNS() - t1) / 1e6, capsules, bodies);
}

void ow_foliage_free(World *w, OpenWorld *o) {
    (void)w;
    for (int i = 0; i < o->fol_count; i++) render_foliage_chunk_unload(o->fol[i].handle);
    free(o->fol);
    o->fol = NULL;
    o->fol_count = o->fol_loaded = 0;
}

void ow_foliage_frame(World *w, OpenWorld *o, vec3 cam) {
    (void)w;
    (void)o;
    (void)cam;
}

// Ground cover of a streamed chunk: tufts on grass / meadow / forest floor, wheat on fields, a few
// bushes on meadows; nothing on roads, lots, sand, rock, water or inside keep-outs.
void *ow_foliage_chunk(void *user, int cx, int cz, size_t *size) {
    const OpenWorld *o = (const OpenWorld *)user;
    *size = 0;
    if (!o->fol_types) return NULL;
    const float cell = 1.1f;
    const int per = (int)(CHUNK_SIZE / cell);
    RenderFoliageInst *tmp = (RenderFoliageInst *)malloc(sizeof(RenderFoliageInst) * (size_t)(per * per * 2 + 16));
    int n = 0;
    float x0 = (float)cx * CHUNK_SIZE, z0 = (float)cz * CHUNK_SIZE;
    for (int gz = 0; gz < per; gz++)
        for (int gx = 0; gx < per; gx++) {
            int ix = cx * per + gx, iz = cz * per + gz;
            u32 h = hash3(ix, iz, o->gen.seed + 101u);
            float x = x0 + ((float)gx + h01(h)) * cell, z = z0 + ((float)gz + h01(h >> 9 ^ 0x51u)) * cell;
            u8 mat = mapgen_raster_mat(&o->raster, x, z);
            float dens;
            int kind = C_TUFT;
            switch (mat) {
            case MGM_GRASS: dens = 0.75f; break;
            case MGM_MEADOW: dens = 0.9f; break;
            case MGM_FOREST: dens = 0.3f; break;
            case MGM_DIRT: dens = 0.12f; break;
            case MGM_FIELD: dens = 1.0f; kind = C_CROP; break;
            case MGM_CROP: dens = 0.8f; kind = C_CROP; break;
            case MGM_ASPHALT: dens = 0.015f; break;   // weeds through the cracks
            default: dens = 0; break;
            }
            float pick = h01(hash3(ix, iz, o->gen.seed + 202u));
            if (pick >= dens) continue;
            if (mat == MGM_MEADOW && pick < 0.012f) kind = C_BUSH;
            float y = mapgen_raster_height(&o->raster, x, z);
            if (mapgen_raster_slope(&o->raster, x, z) > 1.0f) continue;
            if (mapgen_water_level(&o->gen, x, z) > y - 0.15f) continue;
            if (mapgen_in_keepout(&o->gen, x, z, 0.2f)) continue;
            RenderFoliageInst *t = &tmp[n++];
            t->pos = v3(x, y - 0.03f, z);
            t->yaw = h01(h >> 5) * 6.2831853f;
            t->kind = (u16)kind;
            t->scale = kind == C_CROP ? 0.85f + 0.3f * h01(h >> 3) : (kind == C_BUSH ? 1.2f + 0.8f * h01(h >> 3) : 0.75f + 0.6f * h01(h >> 3));
            // crops twice as dense (a second stalk clump in the cell)
            if (kind == C_CROP) {
                RenderFoliageInst *t2 = &tmp[n++];
                *t2 = *t;
                t2->pos.x = x0 + ((float)gx + h01(h >> 13)) * cell;
                t2->pos.z = z0 + ((float)gz + h01(h >> 17)) * cell;
                t2->pos.y = mapgen_raster_height(&o->raster, t2->pos.x, t2->pos.z) - 0.03f;
                t2->yaw += 1.7f;
            }
        }
    void *blob = n ? render_foliage_make_chunk(cx, cz, k_cover_names, 3, tmp, n, size) : NULL;
    free(tmp);
    return blob;
}
