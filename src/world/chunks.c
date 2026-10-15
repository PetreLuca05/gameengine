#include "chunks.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/log.h"
#include "core/jobs.h"
#include "render/mesh_gen.h"
#include "render/debug_draw.h"
#include "gameplay/entities.h"
#include "FastNoiseLite.h"
#include <stdlib.h>
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
} Chunk;

typedef struct ChunkStreamer ChunkStreamer;

typedef struct ChunkJob { ChunkStreamer *s; int index; ChunkCoord c; } ChunkJob;

struct ChunkStreamer {
    World *w;
    ChunkProviderKind kind;
    ChunkProvider provider;
    WorldGen gen;
    fnl_state noise_base, noise_ridge, noise_detail;
    Chunk chunks[MAX_CHUNKS];
    ChunkJob jobs[MAX_CHUNKS];
    JobCounter counter;
    vec3 spawn;
    double time;
};

typedef struct ChunkGlobals { Cvar *radius, *uploads, *debug, *lod_dist, *grid; } ChunkGlobals;
#define CG (g_engine.chunk_globals)

static void ensure_globals(void) {
    if (CG) return;
    CG = ARENA_PUSH(&g_engine.perm_arena, ChunkGlobals);
    CG->radius = cvar_reg_int("world_chunk_radius", 4, 1, 12, CVAR_ARCHIVE, "chunks loaded around each player (radius, in chunks)");
    CG->uploads = cvar_reg_int("world_chunk_uploads", 3, 1, 32, 0, "chunks finalized (mesh + collision) per frame");
    CG->debug = cvar_reg_bool("world_debug_chunks", false, 0, "draw chunk bounds and streaming state");
    CG->lod_dist = cvar_reg_float("world_lod_distance", 70.0f, 10.0f, 1000.0f, CVAR_ARCHIVE, "distance of the first terrain LOD switch (m)");
    CG->grid = cvar_reg_bool("world_terrain_grid", true, CVAR_ARCHIVE, "draw the unit grid on terrain");
}

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

static MeshId build_lod_mesh(const ChunkData *d, int stride) {
    MeshBuilder mb;
    mb_init(&mb);
    int n = (CHUNK_SAMPLES - 1) / stride + 1;
    for (int zi = 0; zi < n; zi++)
        for (int xi = 0; xi < n; xi++) {
            int x = xi * stride, z = zi * stride;
            float h = d->heights[z * CHUNK_SAMPLES + x];
            int xl = DG_MAX(x - stride, 0), xr = DG_MIN(x + stride, CHUNK_SAMPLES - 1);
            int zd = DG_MAX(z - stride, 0), zu = DG_MIN(z + stride, CHUNK_SAMPLES - 1);
            float dx = (d->heights[z * CHUNK_SAMPLES + xr] - d->heights[z * CHUNK_SAMPLES + xl]) / (float)(xr - xl);
            float dz = (d->heights[zu * CHUNK_SAMPLES + x] - d->heights[zd * CHUNK_SAMPLES + x]) / (float)(zu - zd);
            vec3 nrm = v3_norm(v3(-dx, 1.0f, -dz));
            mb_vertex(&mb, v3((float)x, h, (float)z), nrm, v2((float)x, (float)z), d->colors[z * CHUNK_SAMPLES + x]);
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

static void finalize_chunk(ChunkStreamer *s, Chunk *ch) {
    ChunkData *d = ch->data;
    for (int l = 0; l < LOD_COUNT; l++) ch->lod[l] = build_lod_mesh(d, 1 << l);
    ch->heights = (float *)malloc(sizeof(float) * CHUNK_SAMPLES * CHUNK_SAMPLES);
    memcpy(ch->heights, d->heights, sizeof(float) * CHUNK_SAMPLES * CHUNK_SAMPLES);
    BodyDesc bd;
    memset(&bd, 0, sizeof(bd));
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
    ch->state = CS_LOADED;
}

static void release_chunk(ChunkStreamer *s, Chunk *ch) {
    for (int l = 0; l < LOD_COUNT; l++) render_mesh_destroy(ch->lod[l]);
    if (ch->body != PHYS_INVALID_BODY) phys_remove_body(s->w->phys, ch->body);
    free(ch->heights);
    free(ch->data);
    memset(ch, 0, sizeof(*ch));
    ch->body = PHYS_INVALID_BODY;
    ch->state = CS_FREE;
}

// ------------------------------------------------------------------ jobs

static void job_generate(void *user) {
    ChunkJob *j = (ChunkJob *)user;
    Chunk *ch = &j->s->chunks[j->index];
    j->s->provider.generate(j->s->provider.user, j->c, ch->data);
}

static void job_done(void *user) {
    ChunkJob *j = (ChunkJob *)user;
    Chunk *ch = &j->s->chunks[j->index];
    if (ch->state == CS_LOADING && ch->c.x == j->c.x && ch->c.z == j->c.z) ch->state = CS_READY;
}

static Chunk *find_chunk(ChunkStreamer *s, int cx, int cz) {
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
    ch = &s->chunks[idx];
    memset(ch, 0, sizeof(*ch));
    ch->body = PHYS_INVALID_BODY;
    ch->c.x = cx;
    ch->c.z = cz;
    ch->needed_at = s->time;
    ch->data = (ChunkData *)calloc(1, sizeof(ChunkData));
    if (sync) {
        s->provider.generate(s->provider.user, ch->c, ch->data);
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
    // Unload chunks nobody needed for a while (hysteresis avoids thrashing at the border).
    for (int i = 0; i < MAX_CHUNKS; i++) {
        Chunk *ch = &s->chunks[i];
        if (ch->state != CS_LOADED && ch->state != CS_READY) continue;
        if (s->time - ch->needed_at > 2.0) release_chunk(s, ch);
    }
}

void chunks_render(World *w, vec3 cam) {
    ChunkStreamer *s = w->chunks;
    if (!s) return;
    float lod_d = CG->lod_dist->f;
    for (int i = 0; i < MAX_CHUNKS; i++) {
        Chunk *ch = &s->chunks[i];
        if (ch->state == CS_FREE) continue;
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
        DrawItem di;
        memset(&di, 0, sizeof(di));
        di.mesh = ch->lod[lod];
        di.model = mat4_translate(origin);
        di.mat = material_color(v4(1, 1, 1, 1));
        di.mat.roughness = 0.95f;
        di.mat.flags = CG->grid->i ? MAT_GRID : 0;
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
    if (!w->chunks) return true;
    Chunk *ch = find_chunk(w->chunks, (int)floorf(pos.x / CHUNK_SIZE), (int)floorf(pos.z / CHUNK_SIZE));
    return ch && ch->state == CS_LOADED;
}
