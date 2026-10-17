#include "chunks.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/log.h"
#include "core/jobs.h"
#include "render/mesh_gen.h"
#include "render/debug_draw.h"
#include "render/lod_fade.h"
#include "gameplay/entities.h"
#include "FastNoiseLite.h"
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define MAX_CHUNKS 1024
#define LOD_COUNT 4

typedef enum ChunkState { CS_FREE, CS_LOADING, CS_READY, CS_LOADED } ChunkState;

typedef struct Chunk {
    ChunkCoord c;
    volatile int state;
    ChunkData *data;
    MeshId lod[LOD_COUNT];
    BodyId body;
    float *heights;     // kept alive for the heightfield body's debug draw
    double needed_at;
    // Foliage placement chunk (world_foliage_test; docs/VGEO.md "Foliage support"): the .vgp made by
    // the job, loaded into the renderer with the chunk (trunk capsules into the chunk's statics),
    // freed with it.
    void *foliage;
    size_t foliage_size;
    int foliage_handle;
    BodyId *fol_bodies;
    int fol_body_count;
} Chunk;

typedef struct ChunkStreamer ChunkStreamer;

typedef struct ChunkJob { ChunkStreamer *s; int index; ChunkCoord c; } ChunkJob;

#define FAR_LODS 2
#define FAR_MAX_TILES 1024

typedef struct FarTile { MeshId lod[FAR_LODS]; int cx0, cz0; } FarTile;   // first chunk of the tile

struct ChunkStreamer {
    World *w;
    ChunkProviderKind kind;
    ChunkProvider provider;
    // bounded maps: far tiles and a direct chunk lookup (index + 1, 0 = none)
    FarTile *far;
    int far_n;                    // tiles per side
    int chunk_n;                  // chunks per side
    int chunk_origin;             // chunk coordinate of the min corner
    short *lookup;
    WorldGen gen;
    fnl_state noise_base, noise_ridge, noise_detail;
    Chunk chunks[MAX_CHUNKS];
    ChunkJob jobs[MAX_CHUNKS];
    JobCounter counter;
    vec3 spawn;
    double time;
};

typedef struct ChunkGlobals { Cvar *radius, *uploads, *debug, *lod_dist, *grid, *foliage; } ChunkGlobals;
#define CG (g_engine.chunk_globals)

static void ensure_globals(void) {
    if (CG) return;
    CG = ARENA_PUSH(&g_engine.perm_arena, ChunkGlobals);
    CG->radius = cvar_reg_int("world_chunk_radius", 4, 1, 12, CVAR_ARCHIVE, "chunks loaded around each player (radius, in chunks)");
    CG->uploads = cvar_reg_int("world_chunk_uploads", 3, 1, 32, 0, "chunks finalized (mesh + collision) per frame");
    CG->debug = cvar_reg_bool("world_debug_chunks", false, 0, "draw chunk bounds and streaming state");
    CG->lod_dist = cvar_reg_float("world_lod_distance", 70.0f, 10.0f, 1000.0f, CVAR_ARCHIVE, "distance of the first terrain LOD switch (m)");
    CG->grid = cvar_reg_bool("world_terrain_grid", true, CVAR_ARCHIVE, "draw the unit grid on terrain");
    CG->foliage = cvar_reg_bool("world_foliage_test", false, 0, "streamed terrain: procedural test foliage per chunk as vgeo placement chunks (set before the map)");
}

void chunks_register_cvars(void) { ensure_globals(); }

float chunks_relevance_radius(void) {
    ensure_globals();
    return ((float)CG->radius->i + 0.5f) * CHUNK_SIZE;
}

// ------------------------------------------------------------------ WorldGen (noise height demo)

static float wg_height(const WorldGen *g, float x, float z) {
    const ChunkStreamer *s = (const ChunkStreamer *)g->noise;
    float base = fnlGetNoise2D((fnl_state *)&s->noise_base, x, z);              // rolling hills
    float ridge = 1.0f - fabsf(fnlGetNoise2D((fnl_state *)&s->noise_ridge, x, z)); // ridged mountains
    ridge = ridge * ridge;
    float detail = fnlGetNoise2D((fnl_state *)&s->noise_detail, x, z);
    float mountain_mask = smoothstepf(0.1f, 0.6f, base);
    float h = g->base_height + base * g->amplitude * 0.5f + ridge * mountain_mask * g->amplitude * 1.6f + detail * 0.6f;
    // Flatten a clearing around the origin for the spawn.
    float d = sqrtf(x * x + z * z);
    float flat = smoothstepf(10.0f, 40.0f, d);
    return lerpf(g->base_height + 0.5f, h, flat);
}

static int wg_biome(const WorldGen *g, float x, float z) {
    float h = g->height(g, x, z);
    if (h < 1.0f) return 0;      // shore
    if (h < 14.0f) return 1;     // grassland
    if (h < 30.0f) return 2;     // forest (stub)
    return 3;                    // rock / snow
}

// Structure placement stub: deterministic candidate positions per chunk (no content yet).
static int wg_structures(const WorldGen *g, ChunkCoord c, vec3 *out, int max) {
    u32 h = dg_hash_u32((u32)c.x * 73856093u ^ (u32)c.z * 19349663u ^ g->seed);
    int n = (int)(h % 3u) == 0 ? 1 : 0;
    for (int i = 0; i < n && i < max; i++) {
        float fx = (float)((h >> 8) & 255u) / 255.0f, fz = (float)((h >> 16) & 255u) / 255.0f;
        float x = ((float)c.x + 0.2f + fx * 0.6f) * CHUNK_SIZE, z = ((float)c.z + 0.2f + fz * 0.6f) * CHUNK_SIZE;
        out[i] = v3(x, g->height(g, x, z), z);
    }
    return n;
}

// ------------------------------------------------------------------ providers

static u32 terrain_color(float h, float slope, int biome) {
    vec3 grass = v3(0.32f, 0.45f, 0.22f), dirt = v3(0.45f, 0.37f, 0.25f), rock = v3(0.45f, 0.44f, 0.42f), snow = v3(0.92f, 0.93f, 0.95f);
    vec3 sand = v3(0.72f, 0.66f, 0.48f);
    vec3 c = biome == 0 ? sand : (biome == 2 ? v3_lerp(grass, dirt, 0.35f) : grass);
    c = v3_lerp(c, rock, smoothstepf(0.45f, 0.8f, slope));
    if (biome == 3) c = v3_lerp(rock, snow, smoothstepf(34.0f, 42.0f, h) * (1.0f - smoothstepf(0.5f, 0.9f, slope)));
    return rgba8(c.x, c.y, c.z, 1);
}

static void noise_generate(void *user, ChunkCoord c, ChunkData *out) {
    ChunkStreamer *s = (ChunkStreamer *)user;
    const WorldGen *g = &s->gen;
    out->coord = c;
    float x0 = (float)c.x * CHUNK_SIZE, z0 = (float)c.z * CHUNK_SIZE;
    for (int z = 0; z < CHUNK_SAMPLES; z++)
        for (int x = 0; x < CHUNK_SAMPLES; x++)
            out->heights[z * CHUNK_SAMPLES + x] = g->height(g, x0 + (float)x, z0 + (float)z);
    for (int z = 0; z < CHUNK_SAMPLES; z++)
        for (int x = 0; x < CHUNK_SAMPLES; x++) {
            float wx = x0 + (float)x, wz = z0 + (float)z;
            float hx = g->height(g, wx + 1, wz) - g->height(g, wx - 1, wz);
            float hz = g->height(g, wx, wz + 1) - g->height(g, wx, wz - 1);
            float slope = 1.0f - 1.0f / sqrtf(1.0f + (hx * hx + hz * hz) * 0.25f);
            float h = out->heights[z * CHUNK_SAMPLES + x];
            out->colors[z * CHUNK_SAMPLES + x] = terrain_color(h, slope * 3.0f, g->biome(g, wx, wz));
        }
    out->biome = g->biome(g, x0 + CHUNK_SIZE * 0.5f, z0 + CHUNK_SIZE * 0.5f);
    out->structure_count = g->place_structures(g, c, out->structures, 4);
}

static float noise_height_at(void *user, float x, float z) {
    ChunkStreamer *s = (ChunkStreamer *)user;
    return s->gen.height(&s->gen, x, z);
}

static void flat_generate(void *user, ChunkCoord c, ChunkData *out) {
    (void)user;
    out->coord = c;
    bool odd = ((c.x + c.z) & 1) != 0;
    u32 col = odd ? rgba8(0.55f, 0.6f, 0.55f, 1) : rgba8(0.6f, 0.58f, 0.52f, 1);
    for (int i = 0; i < CHUNK_SAMPLES * CHUNK_SAMPLES; i++) { out->heights[i] = 0; out->colors[i] = col; }
    out->biome = 1;
    out->structure_count = 0;
}

static float flat_height_at(void *user, float x, float z) { (void)user; (void)x; (void)z; return 0; }

// ------------------------------------------------------------------ meshing (LOD + skirts)

// Grid normal at sample (x, z) from the neighbours `stride` samples away (clamped to the chunk).
static vec3 grid_normal(const ChunkData *d, int x, int z, int stride) {
    int xl = DG_MAX(x - stride, 0), xr = DG_MIN(x + stride, CHUNK_SAMPLES - 1);
    int zd = DG_MAX(z - stride, 0), zu = DG_MIN(z + stride, CHUNK_SAMPLES - 1);
    float dx = (d->heights[z * CHUNK_SAMPLES + xr] - d->heights[z * CHUNK_SAMPLES + xl]) / (float)(xr - xl);
    float dz = (d->heights[zu * CHUNK_SAMPLES + x] - d->heights[zd * CHUNK_SAMPLES + x]) / (float)(zu - zd);
    return v3_norm(v3(-dx, 1.0f, -dz));
}

// Geomorph target of a vertex (docs/LOD.md): the height and normal the next coarser LOD (stride 2s)
// has at this sample: its own vertex, or the middle of the coarse edge / diagonal (b-c, the coarse
// triangulation) it lies on. Packed into uv: x = height delta, y = normal x / z (12 bit each).
static vec2 morph_target(const ChunkData *d, int x, int z, int stride, float h) {
    int S = stride * 2;
    if (S > 8) return v2(0.0f, -1.0f);   // the coarsest LOD: nothing to morph to (normal unused)
    bool ex = (x % S) != 0, ez = (z % S) != 0;
    int ax = x, az = z, bx = x, bz = z;
    if (ex && ez) { ax = x + stride, az = z - stride, bx = x - stride, bz = z + stride; }   // the coarse diagonal
    else if (ex) { ax = x - stride, bx = x + stride; }
    else if (ez) { az = z - stride, bz = z + stride; }
    float ch = 0.5f * (d->heights[az * CHUNK_SAMPLES + ax] + d->heights[bz * CHUNK_SAMPLES + bx]);
    vec3 cn = v3_norm(v3_add(grid_normal(d, ax, az, S), grid_normal(d, bx, bz, S)));
    float qx = floorf(clampf(cn.x * 0.5f + 0.5f, 0, 1) * 4095.0f + 0.5f), qz = floorf(clampf(cn.z * 0.5f + 0.5f, 0, 1) * 4095.0f + 0.5f);
    return v2(ch - h, qx + qz * 4096.0f);
}

// morph: uv carries the geomorph target (MAT_TERRAIN_MORPH draws); otherwise uv = (x, z) as before.
static MeshId build_lod_mesh(const ChunkData *d, int stride, bool morph) {
    MeshBuilder mb;
    mb_init(&mb);
    int n = (CHUNK_SAMPLES - 1) / stride + 1;
    for (int zi = 0; zi < n; zi++)
        for (int xi = 0; xi < n; xi++) {
            int x = xi * stride, z = zi * stride;
            float h = d->heights[z * CHUNK_SAMPLES + x];
            vec3 nrm = grid_normal(d, x, z, stride);
            vec2 uv = morph ? morph_target(d, x, z, stride, h) : v2((float)x, (float)z);
            mb_vertex(&mb, v3((float)x, h, (float)z), nrm, uv, d->colors[z * CHUNK_SAMPLES + x]);
        }
    for (int zi = 0; zi < n - 1; zi++)
        for (int xi = 0; xi < n - 1; xi++) {
            u32 a = (u32)(zi * n + xi), b = a + 1, c = a + (u32)n, e = c + 1;
            mb_tri(&mb, a, c, b);
            mb_tri(&mb, b, c, e);
        }
    // Skirts: vertical strips along the four edges hide cracks between different LODs.
    float drop = 1.0f + (float)stride * 1.5f;
    for (int edge = 0; edge < 4; edge++) {
        for (int k = 0; k < n - 1; k++) {
            int x0, z0, x1, z1;
            switch (edge) {
            case 0: x0 = k; z0 = 0; x1 = k + 1; z1 = 0; break;
            case 1: x0 = n - 1; z0 = k; x1 = n - 1; z1 = k + 1; break;
            case 2: x0 = k + 1; z0 = n - 1; x1 = k; z1 = n - 1; break;
            default: x0 = 0; z0 = k + 1; x1 = 0; z1 = k; break;
            }
            u32 ia = (u32)(z0 * n + x0), ib = (u32)(z1 * n + x1);
            Vertex va = mb.verts[ia], vb = mb.verts[ib];
            u32 ja = mb_vertex(&mb, v3_add(va.pos, v3(0, -drop, 0)), va.normal, va.uv, va.color);
            u32 jb = mb_vertex(&mb, v3_add(vb.pos, v3(0, -drop, 0)), vb.normal, vb.uv, vb.color);
            mb_tri(&mb, ia, ja, ib);
            mb_tri(&mb, ib, ja, jb);
        }
    }
    MeshId id = mb_upload(&mb);
    mb_free(&mb);
    return id;
}

BodyId chunks_capsule_body(World *w, const RenderCapsule *caps, int n) {
    if (n <= 0) return PHYS_INVALID_BODY;
    vec3 *v = (vec3 *)malloc(sizeof(vec3) * 12 * (size_t)n);
    u32 *ix = (u32 *)malloc(sizeof(u32) * 60 * (size_t)n), ni = 0;
    for (int i = 0; i < n; i++) {
        const RenderCapsule *c = &caps[i];
        u32 b = (u32)i * 12;
        for (int k = 0; k < 6; k++) {
            float a = (float)k * 1.0471976f;
            vec3 o = v3(cosf(a) * c->radius, 0, sinf(a) * c->radius);
            v[b + (u32)k] = v3_add(c->a, o);
            v[b + 6 + (u32)k] = v3_add(c->b, o);
        }
        for (u32 k = 0; k < 6; k++) {   // sides (outward, CCW), then the top fan
            u32 k1 = (k + 1) % 6;
            u32 q[6] = { b + k, b + 6 + k, b + 6 + k1, b + k, b + 6 + k1, b + k1 };
            for (int e = 0; e < 6; e++) ix[ni++] = q[e];
        }
        for (u32 k = 1; k + 1 < 6; k++) {
            ix[ni++] = b + 6;
            ix[ni++] = b + 6 + k + 1;
            ix[ni++] = b + 6 + k;
        }
    }
    BodyDesc bd;
    memset(&bd, 0, sizeof(bd));
    bd.shape.kind = SHAPE_MESH;
    bd.shape.verts = v;
    bd.shape.vert_count = 12u * (u32)n;
    bd.shape.indices = ix;
    bd.shape.index_count = ni;
    bd.rot = quat_identity();
    bd.motion = PM_STATIC;
    bd.layer = PL_STATIC;
    bd.friction = 0.8f;
    BodyId id = phys_add_body(w->phys, &bd);
    free(v);
    free(ix);
    return id;
}

static void finalize_chunk(ChunkStreamer *s, Chunk *ch) {
    ChunkData *d = ch->data;
    // uv = the geomorph target (docs/LOD.md); the shading never reads a terrain chunk's uv (untextured)
    for (int l = 0; l < LOD_COUNT; l++) ch->lod[l] = build_lod_mesh(d, 1 << l, true);
    ch->heights = (float *)malloc(sizeof(float) * CHUNK_SAMPLES * CHUNK_SAMPLES);
    memcpy(ch->heights, d->heights, sizeof(float) * CHUNK_SAMPLES * CHUNK_SAMPLES);
    BodyDesc bd;
    memset(&bd, 0, sizeof(bd));
    if (s->provider.static_collision) goto foliage;
    bd.shape.kind = SHAPE_HEIGHTFIELD;
    bd.shape.heights = ch->heights;
    bd.shape.sample_count = CHUNK_SAMPLES;
    bd.shape.spacing = CHUNK_SIZE / (float)(CHUNK_SAMPLES - 1);
    bd.pos = v3((float)ch->c.x * CHUNK_SIZE, 0, (float)ch->c.z * CHUNK_SIZE);
    bd.rot = quat_identity();
    bd.motion = PM_STATIC;
    bd.layer = PL_STATIC;
    bd.friction = 0.9f;
    ch->body = phys_add_body(s->w->phys, &bd);
foliage:
    // foliage placements + trunk capsules
    ch->foliage_handle = -1;
    if (ch->foliage) {
        ch->foliage_handle = render_foliage_chunk_load(ch->foliage, ch->foliage_size);
        free(ch->foliage);
        ch->foliage = NULL;
        static RenderCapsule caps[4096];   // main thread
        int n = ch->foliage_handle >= 0 ? render_foliage_chunk_capsules(ch->foliage_handle, caps, 4096) : 0;
        BodyId b = chunks_capsule_body(s->w, caps, n);
        if (b != PHYS_INVALID_BODY) {
            ch->fol_bodies = (BodyId *)malloc(sizeof(BodyId));
            ch->fol_bodies[ch->fol_body_count++] = b;
        }
    }
    ch->state = CS_LOADED;
}

static int lookup_index(const ChunkStreamer *s, int cx, int cz);

static void release_chunk(ChunkStreamer *s, Chunk *ch) {
    if (s->lookup && ch->state != CS_FREE) {
        int li = lookup_index(s, ch->c.x, ch->c.z);
        if (li >= 0) s->lookup[li] = 0;
    }
    for (int l = 0; l < LOD_COUNT; l++) render_mesh_destroy(ch->lod[l]);
    if (ch->body != PHYS_INVALID_BODY) phys_remove_body(s->w->phys, ch->body);
    if (ch->state == CS_LOADED && ch->foliage_handle >= 0) render_foliage_chunk_unload(ch->foliage_handle);
    for (int i = 0; i < ch->fol_body_count; i++) phys_remove_body(s->w->phys, ch->fol_bodies[i]);
    free(ch->fol_bodies);
    free(ch->foliage);
    free(ch->heights);
    free(ch->data);
    memset(ch, 0, sizeof(*ch));
    ch->body = PHYS_INVALID_BODY;
    ch->state = CS_FREE;
}

// ------------------------------------------------------------------ jobs

static float job_height(void *user, float x, float z) {
    ChunkStreamer *s = (ChunkStreamer *)user;
    return s->provider.height_at(s->provider.user, x, z);
}

static void job_generate(void *user) {
    ChunkJob *j = (ChunkJob *)user;
    Chunk *ch = &j->s->chunks[j->index];
    j->s->provider.generate(j->s->provider.user, j->c, ch->data);
    if (j->s->provider.foliage_chunk) ch->foliage = j->s->provider.foliage_chunk(j->s->provider.user, j->c.x, j->c.z, &ch->foliage_size);
    else if (CG->foliage->i)   // the chunk's placements (thread-safe): 1.5 instances per m2 on the terrain
        ch->foliage = render_foliage_test_chunk(j->c.x, j->c.z, 1.5f, -1e9f, -1e9f, 1e9f, 1e9f, job_height, j->s, &ch->foliage_size);
}

static void job_done(void *user) {
    ChunkJob *j = (ChunkJob *)user;
    Chunk *ch = &j->s->chunks[j->index];
    if (ch->state == CS_LOADING && ch->c.x == j->c.x && ch->c.z == j->c.z) ch->state = CS_READY;
}

static int lookup_index(const ChunkStreamer *s, int cx, int cz) {
    int x = cx - s->chunk_origin, z = cz - s->chunk_origin;
    if (x < 0 || z < 0 || x >= s->chunk_n || z >= s->chunk_n) return -1;
    return z * s->chunk_n + x;
}

static Chunk *find_chunk(ChunkStreamer *s, int cx, int cz) {
    if (s->lookup) {
        int li = lookup_index(s, cx, cz);
        return li >= 0 && s->lookup[li] ? &s->chunks[s->lookup[li] - 1] : NULL;
    }
    for (int i = 0; i < MAX_CHUNKS; i++)
        if (s->chunks[i].state != CS_FREE && s->chunks[i].c.x == cx && s->chunks[i].c.z == cz) return &s->chunks[i];
    return NULL;
}

static Chunk *request_chunk(ChunkStreamer *s, int cx, int cz, bool sync) {
    Chunk *ch = find_chunk(s, cx, cz);
    if (ch) { ch->needed_at = s->time; return ch; }
    int idx = -1;
    for (int i = 0; i < MAX_CHUNKS; i++) if (s->chunks[i].state == CS_FREE) { idx = i; break; }
    if (idx < 0) return NULL;
    if (s->lookup && lookup_index(s, cx, cz) < 0) return NULL;   // outside the bounded map
    ch = &s->chunks[idx];
    memset(ch, 0, sizeof(*ch));
    ch->body = PHYS_INVALID_BODY;
    ch->c.x = cx;
    ch->c.z = cz;
    if (s->lookup) s->lookup[lookup_index(s, cx, cz)] = (short)(idx + 1);
    ch->needed_at = s->time;
    ch->data = (ChunkData *)calloc(1, sizeof(ChunkData));
    if (sync) {
        s->provider.generate(s->provider.user, ch->c, ch->data);
        if (s->provider.foliage_chunk) ch->foliage = s->provider.foliage_chunk(s->provider.user, cx, cz, &ch->foliage_size);
        else if (CG->foliage->i) ch->foliage = render_foliage_test_chunk(cx, cz, 1.5f, -1e9f, -1e9f, 1e9f, 1e9f, job_height, s, &ch->foliage_size);
        finalize_chunk(s, ch);
        return ch;
    }
    ch->state = CS_LOADING;
    ChunkJob *j = &s->jobs[idx];
    j->s = s;
    j->index = idx;
    j->c = ch->c;
    jobs_submit(job_generate, job_done, j, &s->counter);
    return ch;
}

// ------------------------------------------------------------------ public

bool chunks_create(World *w, ChunkProviderKind kind, u32 seed) {
    ensure_globals();
    ChunkStreamer *s = (ChunkStreamer *)calloc(1, sizeof(ChunkStreamer));
    s->w = w;
    s->kind = kind;
    for (int i = 0; i < MAX_CHUNKS; i++) s->chunks[i].body = PHYS_INVALID_BODY;
    s->gen.seed = seed;
    s->gen.height = wg_height;
    s->gen.biome = wg_biome;
    s->gen.place_structures = wg_structures;
    s->gen.noise = s;
    s->gen.amplitude = 18.0f;
    s->gen.base_height = 2.0f;
    s->noise_base = fnlCreateState();
    s->noise_base.seed = (int)seed;
    s->noise_base.noise_type = FNL_NOISE_OPENSIMPLEX2;
    s->noise_base.fractal_type = FNL_FRACTAL_FBM;
    s->noise_base.octaves = 4;
    s->noise_base.frequency = 0.0035f;
    s->noise_ridge = fnlCreateState();
    s->noise_ridge.seed = (int)seed + 101;
    s->noise_ridge.noise_type = FNL_NOISE_OPENSIMPLEX2;
    s->noise_ridge.fractal_type = FNL_FRACTAL_FBM;
    s->noise_ridge.octaves = 3;
    s->noise_ridge.frequency = 0.006f;
    s->noise_detail = fnlCreateState();
    s->noise_detail.seed = (int)seed + 202;
    s->noise_detail.frequency = 0.08f;
    if (kind == CHUNKS_FLAT) {
        s->provider = (ChunkProvider){ "flat", s, flat_generate, flat_height_at };
    } else {
        s->provider = (ChunkProvider){ "noise", s, noise_generate, noise_height_at };
    }
    w->chunks = s;
    w->streaming = true;
    if (CG->foliage->i) render_foliage_test_types();   // before the first chunk resolves its mesh names

    LevelStatic *ls = &w->level;
    memset(ls, 0, sizeof(*ls));
    dg_strcpy(ls->name, sizeof(ls->name), kind == CHUNKS_FLAT ? "Streaming (flat)" : "Streaming (noise terrain)");
    ls->sun_dir = v3_norm(v3(0.5f, 0.8f, 0.3f));
    ls->sun_color = v3(1.0f, 0.95f, 0.85f);
    ls->sun_intensity = 3.0f;
    ls->ambient_sky = v3(0.45f, 0.55f, 0.75f);
    ls->ambient_ground = v3(0.3f, 0.28f, 0.22f);
    ls->fog_color = v3(0.62f, 0.7f, 0.8f);
    ls->fog_density = 0.0035f;
    ls->seed = seed;
    float sy = s->provider.height_at(s->provider.user, 0, 0);
    s->spawn = v3(0, sy + 0.2f, 0);
    ls->spawns[0].pos = s->spawn;
    ls->spawn_count = 1;

    // Synchronously build the area around the spawn so physics exists before anyone moves.
    for (int z = -1; z <= 0; z++)
        for (int x = -1; x <= 0; x++) request_chunk(s, x, z, true);
    phys_optimize(w->phys);

    if (w->authority) {
        // A few props so replication + relevance can be seen in the streaming world.
        for (int i = 0; i < 6; i++) {
            float px = 4.0f + (float)(i % 3) * 1.1f, pz = -6.0f - (float)(i / 3) * 1.1f;
            float py = s->provider.height_at(s->provider.user, px, pz) + 0.6f + (float)(i / 3) * 0.9f;
            spawn_prop(w, v3(px, py, pz), quat_identity(), VIS_BOX, v3s(0.4f), rgba8(0.6f, 0.45f, 0.25f, 1), 20);
        }
        spawn_dummy(w, v3(-6, s->provider.height_at(s->provider.user, -6, -8), -8), 0, 200);
        spawn_light(w, v3(0, sy + 4.0f, -3), v3(1, 0.85f, 0.6f), 14, 60, 0, true);
    }
    log_info("chunks: %s provider, seed %u, radius %d (%d m), spawn height %.1f", s->provider.name, seed, CG->radius->i,
             (int)(CG->radius->i * CHUNK_SIZE), sy);
    return true;
}

void chunks_destroy(World *w) {
    ChunkStreamer *s = w->chunks;
    if (!s) return;
    jobs_wait(&s->counter);
    jobs_pump_completions(0);
    for (int i = 0; i < MAX_CHUNKS; i++) if (s->chunks[i].state != CS_FREE) release_chunk(s, &s->chunks[i]);
    for (int t = 0; s->far && t < s->far_n * s->far_n; t++)
        for (int l = 0; l < FAR_LODS; l++) render_mesh_destroy(s->far[t].lod[l]);
    free(s->far);
    free(s->lookup);
    free(s);
    w->chunks = NULL;
    w->streaming = false;
}

void chunks_update(World *w, float dt) {
    ChunkStreamer *s = w->chunks;
    if (!s) return;
    s->time += dt;
    int r = CG->radius->i;
    // Anchors: every player on the host, the local player (and anything replicated) on clients.
    for (int p = 0; p < MAX_PLAYERS; p++) {
        Player *pl = &w->players[p];
        if (!pl->active) continue;
        vec3 pos = pl->sim.move.pos;
        if (s->far) {
            // bounded maps stream whole far tiles (a far tile is drawn until all its chunks are in)
            float rr = ((float)r + 0.5f) * CHUNK_SIZE;
            int t0x = (int)floorf((pos.x - rr) / CHUNK_FAR_TILE), t1x = (int)floorf((pos.x + rr) / CHUNK_FAR_TILE);
            int t0z = (int)floorf((pos.z - rr) / CHUNK_FAR_TILE), t1z = (int)floorf((pos.z + rr) / CHUNK_FAR_TILE);
            int per = (int)(CHUNK_FAR_TILE / CHUNK_SIZE);
            for (int tz = t0z; tz <= t1z; tz++)
                for (int tx = t0x; tx <= t1x; tx++) {
                    float nx = clampf(pos.x, (float)tx * CHUNK_FAR_TILE, (float)(tx + 1) * CHUNK_FAR_TILE), nz = clampf(pos.z, (float)tz * CHUNK_FAR_TILE, (float)(tz + 1) * CHUNK_FAR_TILE);
                    if ((nx - pos.x) * (nx - pos.x) + (nz - pos.z) * (nz - pos.z) > rr * rr) continue;
                    for (int k = 0; k < per * per; k++) request_chunk(s, tx * per + k % per, tz * per + k / per, false);
                }
            continue;
        }
        int cx = (int)floorf(pos.x / CHUNK_SIZE), cz = (int)floorf(pos.z / CHUNK_SIZE);
        // Request nearest-first so the area under the player streams in first.
        for (int ring = 0; ring <= r; ring++)
            for (int z = cz - ring; z <= cz + ring; z++)
                for (int x = cx - ring; x <= cx + ring; x++) {
                    if (DG_MAX(abs(x - cx), abs(z - cz)) != ring) continue;
                    if ((x - cx) * (x - cx) + (z - cz) * (z - cz) > (r + 1) * (r + 1)) continue;
                    request_chunk(s, x, z, false);
                }
    }
    // Finalize generated chunks (GPU upload + collision) with a per-frame budget.
    int budget = CG->uploads->i;
    for (int i = 0; i < MAX_CHUNKS && budget > 0; i++) {
        if (s->chunks[i].state == CS_READY) { finalize_chunk(s, &s->chunks[i]); budget--; }
    }
    // Unload chunks nobody needed for a while (hysteresis avoids thrashing at the border). A budget
    // per frame: a teleport drops hundreds at once, and each frees 8 buffers through the renderer's
    // per-frame deferred queue (an overflow there would stall and free buffers still in flight).
    int unloads = 32;
    for (int i = 0; i < MAX_CHUNKS && unloads > 0; i++) {
        Chunk *ch = &s->chunks[i];
        if (ch->state != CS_LOADED && ch->state != CS_READY) continue;
        if (s->time - ch->needed_at > 2.0) { release_chunk(s, ch); unloads--; }
    }
}

// A far tile is complete when all its chunks are loaded (then they draw instead of it).
static bool far_complete(ChunkStreamer *s, const FarTile *t) {
    int per = (int)(CHUNK_FAR_TILE / CHUNK_SIZE);
    for (int k = 0; k < per * per; k++) {
        Chunk *ch = find_chunk(s, t->cx0 + k % per, t->cz0 + k / per);
        if (!ch || ch->state != CS_LOADED) return false;
    }
    return true;
}

static MeshId build_far_mesh(ChunkStreamer *s, float x0, float z0, float spacing) {
    int n = (int)(CHUNK_FAR_TILE / spacing) + 1;
    MeshBuilder mb;
    mb_init(&mb);
    float *h = (float *)malloc(sizeof(float) * (size_t)(n + 2) * (size_t)(n + 2));
    u32 *col = (u32 *)malloc(sizeof(u32) * (size_t)n * (size_t)n);
    for (int z = -1; z <= n; z++)
        for (int x = -1; x <= n; x++) {
            u32 c;
            float hh;
            s->provider.sample(s->provider.user, x0 + (float)x * spacing, z0 + (float)z * spacing, &hh, &c);
            h[(z + 1) * (n + 2) + x + 1] = hh;
            if (x >= 0 && z >= 0 && x < n && z < n) col[z * n + x] = c;
        }
    for (int z = 0; z < n; z++)
        for (int x = 0; x < n; x++) {
            float hc = h[(z + 1) * (n + 2) + x + 1];
            float dx = (h[(z + 1) * (n + 2) + x + 2] - h[(z + 1) * (n + 2) + x]) / (2 * spacing);
            float dz = (h[(z + 2) * (n + 2) + x + 1] - h[z * (n + 2) + x + 1]) / (2 * spacing);
            mb_vertex(&mb, v3((float)x * spacing, hc, (float)z * spacing), v3_norm(v3(-dx, 1.0f, -dz)), v2((float)x * spacing, (float)z * spacing), col[z * n + x]);
        }
    for (int z = 0; z < n - 1; z++)
        for (int x = 0; x < n - 1; x++) {
            u32 a = (u32)(z * n + x), b = a + 1, c = a + (u32)n, e = c + 1;
            mb_tri(&mb, a, c, b);
            mb_tri(&mb, b, c, e);
        }
    // skirts (below the neighbouring surfaces at tile borders)
    float drop = 2.0f + spacing;
    for (int edge = 0; edge < 4; edge++)
        for (int k = 0; k < n - 1; k++) {
            int xa, za, xb, zb;
            switch (edge) {
            case 0: xa = k; za = 0; xb = k + 1; zb = 0; break;
            case 1: xa = n - 1; za = k; xb = n - 1; zb = k + 1; break;
            case 2: xa = k + 1; za = n - 1; xb = k; zb = n - 1; break;
            default: xa = 0; za = k + 1; xb = 0; zb = k; break;
            }
            u32 ia = (u32)(za * n + xa), ib = (u32)(zb * n + xb);
            Vertex va = mb.verts[ia], vb = mb.verts[ib];
            u32 ja = mb_vertex(&mb, v3_add(va.pos, v3(0, -drop, 0)), va.normal, va.uv, va.color);
            u32 jb = mb_vertex(&mb, v3_add(vb.pos, v3(0, -drop, 0)), vb.normal, vb.uv, vb.color);
            mb_tri(&mb, ia, ja, ib);
            mb_tri(&mb, ib, ja, jb);
        }
    MeshId id = mb_upload(&mb);
    mb_free(&mb);
    free(h);
    free(col);
    return id;
}

bool chunks_create_custom(World *w, const ChunkProvider *provider) {
    ensure_globals();
    ChunkStreamer *s = (ChunkStreamer *)calloc(1, sizeof(ChunkStreamer));
    s->w = w;
    s->kind = CHUNKS_NOISE;
    s->provider = *provider;
    for (int i = 0; i < MAX_CHUNKS; i++) s->chunks[i].body = PHYS_INVALID_BODY;
    w->chunks = s;
    w->streaming = true;
    if (provider->bounds > 0) {
        s->chunk_n = (int)ceilf(2.0f * provider->bounds / CHUNK_SIZE);
        s->chunk_origin = (int)floorf(-provider->bounds / CHUNK_SIZE);
        s->lookup = (short *)calloc((size_t)s->chunk_n * (size_t)s->chunk_n, sizeof(short));
    }
    if (provider->bounds > 0 && provider->sample) {
        u64 t0 = SDL_GetTicksNS();
        s->far_n = DG_MIN((int)ceilf(2.0f * provider->bounds / CHUNK_FAR_TILE), 32);
        s->far = (FarTile *)calloc((size_t)s->far_n * (size_t)s->far_n, sizeof(FarTile));
        int per = (int)(CHUNK_FAR_TILE / CHUNK_SIZE);
        for (int tz = 0; tz < s->far_n; tz++)
            for (int tx = 0; tx < s->far_n; tx++) {
                FarTile *t = &s->far[tz * s->far_n + tx];
                float x0 = -provider->bounds + (float)tx * CHUNK_FAR_TILE, z0 = -provider->bounds + (float)tz * CHUNK_FAR_TILE;
                t->cx0 = s->chunk_origin + tx * per;
                t->cz0 = s->chunk_origin + tz * per;
                t->lod[0] = build_far_mesh(s, x0, z0, 4.0f);
                t->lod[1] = build_far_mesh(s, x0, z0, 16.0f);
            }
        log_info("chunks: %s: %d far tiles in %.0f ms", provider->name, s->far_n * s->far_n, (double)(SDL_GetTicksNS() - t0) / 1e6);
    }
    return true;
}

void chunks_render(World *w, vec3 cam) {
    ChunkStreamer *s = w->chunks;
    if (!s) return;
    float lod_d = CG->lod_dist->f;
    // docs/LOD.md: LOD distances x lod.json x tier (at least 32 m: one chunk diagonal fits a level), the
    // level from the chunk's nearest point and a per-vertex geomorph (no pops, no visible skirts)
    bool morph = lod_active();
    float far_d = 700.0f;
    if (morph) {
        lod_d = floorf(fmaxf(lod_d * lod_scale(LODC_TERRAIN), 32.0f));
        far_d *= lod_scale(LODC_TERRAIN);
    }
    u32 grid = CG->grid->i && !s->provider.no_grid ? MAT_GRID : 0;
    if (s->provider.surface_textures) grid |= MAT_TERRAIN;
    static u8 complete[FAR_MAX_TILES];
    for (int t = 0; s->far && t < s->far_n * s->far_n; t++) {
        FarTile *ft = &s->far[t];
        complete[t] = far_complete(s, ft);
        if (complete[t]) continue;
        vec3 origin = v3((float)ft->cx0 * CHUNK_SIZE, 0, (float)ft->cz0 * CHUNK_SIZE);
        vec3 center = v3_add(origin, v3(CHUNK_FAR_TILE * 0.5f, 0, CHUNK_FAR_TILE * 0.5f));
        float d = v3_len(v3_flat(v3_sub(center, cam)));
        DrawItem di;
        memset(&di, 0, sizeof(di));
        di.mesh = ft->lod[d < far_d ? 0 : 1];
        di.model = mat4_translate(origin);
        di.mat = material_color(v4(1, 1, 1, 1));
        di.mat.roughness = 0.95f;
        di.mat.flags = grid;
        render_draw(&di);
    }
    for (int i = 0; i < MAX_CHUNKS; i++) {
        Chunk *ch = &s->chunks[i];
        if (ch->state == CS_FREE) continue;
        if (s->far) {   // inside an incomplete far tile: the tile draws
            int per = (int)(CHUNK_FAR_TILE / CHUNK_SIZE);
            int tx = (ch->c.x - s->chunk_origin) / per, tz = (ch->c.z - s->chunk_origin) / per;
            if (tx >= 0 && tz >= 0 && tx < s->far_n && tz < s->far_n && !complete[tz * s->far_n + tx]) continue;
        }
        vec3 origin = v3((float)ch->c.x * CHUNK_SIZE, 0, (float)ch->c.z * CHUNK_SIZE);
        if (CG->debug->i) {
            u32 col = ch->state == CS_LOADED ? DBG_GREEN : (ch->state == CS_READY ? DBG_CYAN : DBG_YELLOW);
            float y = cam.y - 1.5f;
            dbg_aabb(v3(origin.x + 0.1f, y, origin.z + 0.1f), v3(origin.x + CHUNK_SIZE - 0.1f, y + 0.05f, origin.z + CHUNK_SIZE - 0.1f), col, 0);
            if (ch->data) for (int k = 0; k < ch->data->structure_count; k++) dbg_cross(ch->data->structures[k], 2.0f, DBG_MAGENTA, 0);
        }
        if (ch->state != CS_LOADED) continue;
        vec3 center = v3_add(origin, v3(CHUNK_SIZE * 0.5f, 0, CHUNK_SIZE * 0.5f));
        float d = v3_len(v3_flat(v3_sub(center, cam)));
        int lod = d < lod_d ? 0 : d < lod_d * 2 ? 1 : d < lod_d * 4 ? 2 : 3;
        if (morph) {
            float nx = clampf(cam.x, origin.x, origin.x + CHUNK_SIZE) - cam.x, nz = clampf(cam.z, origin.z, origin.z + CHUNK_SIZE) - cam.z;
            lod = lod_terrain_level(sqrtf(nx * nx + nz * nz), lod_d);
        }
        DrawItem di;
        memset(&di, 0, sizeof(di));
        di.mesh = ch->lod[lod];
        di.model = mat4_translate(origin);
        di.mat = material_color(v4(1, 1, 1, 1));
        di.mat.roughness = 0.95f;
        di.mat.flags = grid;
        if (morph) {
            di.mat.flags |= MAT_TERRAIN_MORPH;
            di.mat.tex_scale = (float)lod + 8.0f * lod_d;   // lod_terrain_vertex
        }
        render_draw(&di);
    }
}

vec3 chunks_spawn_point(World *w) { return w->chunks ? w->chunks->spawn : v3(0, 1, 0); }

int chunks_loaded_count(const World *w) {
    if (!w->chunks) return 0;
    int n = 0;
    for (int i = 0; i < MAX_CHUNKS; i++) if (w->chunks->chunks[i].state == CS_LOADED) n++;
    return n;
}

bool chunks_is_loaded(const World *w, vec3 pos) {
    if (!w->chunks || w->chunks->provider.static_collision) return true;   // static collision everywhere
    Chunk *ch = find_chunk(w->chunks, (int)floorf(pos.x / CHUNK_SIZE), (int)floorf(pos.z / CHUNK_SIZE));
    return ch && ch->state == CS_LOADED;
}
