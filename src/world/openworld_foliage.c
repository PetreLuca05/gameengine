// Trees and ground cover of open-world maps (docs/ZOMBIE_MAP.md "Vegetation"), on the vgeo foliage
// path (masked cards, wind WPO, impostors, placement chunks):
//   - trees (conifer / broadleaf / dead roles, a hash-picked species of each: docs/VGEO.md "Tree
//     models") and forest underbrush (bushes) for the whole map at load, one
//     placement chunk per 32 m chunk (generated on the job system from mapgen_tree_density), their
//     trunk capsules merged into one static collision mesh per 128 m tile;
//   - grass tufts, crops, meadow bushes and daisies per streamed terrain chunk (ow_foliage_chunk,
//     thread-safe, by surface material: grass / meadow / forest floor / fields), loaded and freed with
//     the chunk by chunks.c.
#include "openworld_internal.h"
#include "chunks.h"
#include "core/jobs.h"
#include "core/log.h"
#include "render/render.h"
#include "render/vgeo/vgeo_tree.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

// Type tables, filled at load from the registered types (render_foliage_role_types: the same order
// on every machine, so the hash picks below place the same species everywhere). Read-only while the
// chunk jobs run.
//   tree chunks:  names = the conifer, broadleaf, dead and bush species, a role = a range of them
//   cover chunks: names = test_tuft, crop_wheat, the bush species, the flower species
// A species' instance scale range comes from its height range (vgeo_tree_scale_range: real mature
// heights, docs/VGEO.md "Tree models").
#define OW_MAX_TYPES 40
typedef struct OwRole { int first, count; float wsum; } OwRole;
static struct {
    char tree_names[OW_MAX_TYPES][48];
    float tree_w[OW_MAX_TYPES];
    float tree_scale[OW_MAX_TYPES][2];
    int tree_count;
    OwRole role[RENDER_TREE_ROLES];       // T_* ranges in tree_names
    char cover_names[OW_MAX_TYPES][48];
    float cover_w[OW_MAX_TYPES];
    float cover_scale[OW_MAX_TYPES][2];
    int cover_count;
    OwRole cover_bush, cover_flower;
} OT;
enum { C_TUFT, C_CROP };

static void role_add(char (*names)[48], float *w, int *count, OwRole *r, int role) {
    r->first = *count;
    r->count = render_foliage_role_types(role, names + *count, w + *count, OW_MAX_TYPES - *count);
    r->wsum = 0;
    for (int i = 0; i < r->count; i++) r->wsum += w[*count + i];
    *count += r->count;
}

// Instance scales of the types: the species height range, else lo..hi.
static void type_scales(char (*names)[48], float (*scale)[2], int count, float lo, float hi) {
    for (int i = 0; i < count; i++)
        if (!vgeo_tree_scale_range(names[i], &scale[i][0], &scale[i][1])) scale[i][0] = lo, scale[i][1] = hi;
}

static void ow_types(void) {
    memset(&OT, 0, sizeof(OT));
    for (int r = RENDER_TREE_CONIFER; r <= RENDER_TREE_BUSH; r++) role_add(OT.tree_names, OT.tree_w, &OT.tree_count, &OT.role[r], r);
    dg_strcpy(OT.cover_names[C_TUFT], 48, "test_tuft");
    dg_strcpy(OT.cover_names[C_CROP], 48, "crop_wheat");
    OT.cover_count = 2;
    role_add(OT.cover_names, OT.cover_w, &OT.cover_count, &OT.cover_bush, RENDER_TREE_BUSH);
    role_add(OT.cover_names, OT.cover_w, &OT.cover_count, &OT.cover_flower, RENDER_TREE_FLOWER);
    type_scales(OT.tree_names, OT.tree_scale, OT.tree_count, 0.72f, 1.27f);
    type_scales(OT.cover_names, OT.cover_scale, OT.cover_count, 0.75f, 1.35f);
}

static float type_scale(const float s[2], float u) { return s[0] + (s[1] - s[0]) * u; }

// The weighted type of a role for u in [0, 1); -1 when the role has none.
static int role_pick(const OwRole *r, const float *w, float u) {
    if (!r->count) return -1;
    float t = u * r->wsum;
    for (int i = 0; i < r->count - 1; i++) {
        t -= w[r->first + i];
        if (t < 0) return r->first + i;
    }
    return r->first + r->count - 1;
}

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
            float pine, dead;
            float d = mapgen_tree_density(&o->gen, &o->raster, x, z, &pine, &dead);
            if (d <= 0) continue;
            u32 h2 = hash3(ix, iz, o->gen.seed + 17u);
            float pick = h01(h2);
            RenderFoliageInst *t = &out[n];
            t->pos = v3(x, mapgen_raster_height(&o->raster, x, z) - 0.15f, z);
            t->yaw = h01(h2 >> 3) * 6.2831853f;
            float species = h01(hash3(ix, iz, o->gen.seed + 29u));
            if (pick < d * cell * cell) {
                float k = h01(h2 >> 11);
                int role = k < dead ? RENDER_TREE_DEAD : (k < dead + pine * (1.0f - dead) ? RENDER_TREE_CONIFER : RENDER_TREE_BROADLEAF);   // dead first, the rest split by the pine fraction
                int kind = role_pick(&OT.role[role], OT.tree_w, species);
                if (kind < 0) continue;
                t->kind = (u16)kind;
                t->scale = type_scale(OT.tree_scale[kind], h01(h2 >> 5));
                n++;
            } else if (d > 0.006f && pick < d * cell * cell * 1.6f) {   // forest underbrush
                int kind = role_pick(&OT.role[RENDER_TREE_BUSH], OT.tree_w, species);
                if (kind < 0) continue;
                t->kind = (u16)kind;
                t->scale = type_scale(OT.tree_scale[kind], h01(h2 >> 5));
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
        j->blob[i] = n ? render_foliage_make_chunk(cx, cz, (const char(*)[48])OT.tree_names, OT.tree_count, tmp, n, &j->size[i]) : NULL;
    }
    free(tmp);
}

void ow_foliage_load(World *w, OpenWorld *o) {
    if (!render_foliage_map_types()) { log_warn("openworld: vgeo foliage unavailable (no trees)"); return; }
    ow_types();
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
// bushes on meadows, daisies on meadows and grass; nothing on roads, lots, sand, rock, water or inside
// keep-outs.
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
            float species = h01(hash3(ix, iz, o->gen.seed + 303u));
            if (mat == MGM_MEADOW && pick < 0.012f) {
                int k = role_pick(&OT.cover_bush, OT.cover_w, species);
                if (k >= 0) kind = k;
            } else if ((mat == MGM_MEADOW && pick < 0.05f) || (mat == MGM_GRASS && pick < 0.012f)) {
                int k = role_pick(&OT.cover_flower, OT.cover_w, species);
                if (k >= 0) kind = k;
            }
            float y = mapgen_raster_height(&o->raster, x, z);
            if (mapgen_raster_slope(&o->raster, x, z) > 1.0f) continue;
            if (mapgen_water_level(&o->gen, x, z) > y - 0.15f) continue;
            if (mapgen_in_keepout(&o->gen, x, z, 0.2f)) continue;
            RenderFoliageInst *t = &tmp[n++];
            t->pos = v3(x, y - 0.03f, z);
            t->yaw = h01(h >> 5) * 6.2831853f;
            t->kind = (u16)kind;
            t->scale = kind == C_CROP ? 0.85f + 0.3f * h01(h >> 3) : type_scale(OT.cover_scale[kind], h01(h >> 3));
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
    void *blob = n ? render_foliage_make_chunk(cx, cz, (const char(*)[48])OT.cover_names, OT.cover_count, tmp, n, size) : NULL;
    free(tmp);
    return blob;
}
