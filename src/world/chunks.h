// Chunk streaming: the world is a grid of chunks loaded around players on the job system.
// ChunkProvider is the plug-in point for content (flat test provider, noise terrain via WorldGen,
// later: real terrain + structures + zombie spawners).
#ifndef DG_CHUNKS_H
#define DG_CHUNKS_H

#include "world.h"

#define CHUNK_SIZE 32.0f          // meters per chunk side
#define CHUNK_SAMPLES 33          // heightfield samples per side (1 m spacing, shared edges)

typedef enum ChunkProviderKind { CHUNKS_FLAT, CHUNKS_NOISE } ChunkProviderKind;

typedef struct ChunkCoord { int x, z; } ChunkCoord;

// Output of a provider: generated on a worker thread, consumed on the main thread.
typedef struct ChunkData {
    ChunkCoord coord;
    float heights[CHUNK_SAMPLES * CHUNK_SAMPLES];
    u32 colors[CHUNK_SAMPLES * CHUNK_SAMPLES];
    int biome;                    // WorldGen biome stub
    int structure_count;          // WorldGen structure placement stub
    vec3 structures[4];
} ChunkData;

typedef struct ChunkProvider {
    const char *name;
    void *user;
    // Must be thread-safe: called from job threads.
    void (*generate)(void *user, ChunkCoord coord, ChunkData *out);
    float (*height_at)(void *user, float x, float z);
} ChunkProvider;

// WorldGen: seeded procedural generation interface (noise height implemented; biomes/structures stubbed).
typedef struct WorldGen {
    u32 seed;
    float (*height)(const struct WorldGen *g, float x, float z);
    int (*biome)(const struct WorldGen *g, float x, float z);
    int (*place_structures)(const struct WorldGen *g, ChunkCoord c, vec3 *out, int max);
    void *noise;                  // FastNoiseLite state
    float amplitude, base_height;
} WorldGen;

// Foliage trunk capsules (render_foliage_chunk_capsules) as one static body: hexagonal prisms in a
// triangle mesh (a body per capsule would run out of bodies at 300k foliage instances).
BodyId chunks_capsule_body(World *w, const RenderCapsule *caps, int n);

bool chunks_create(World *w, ChunkProviderKind kind, u32 seed);
void chunks_destroy(World *w);
void chunks_update(World *w, float dt);
void chunks_render(World *w, vec3 cam_pos);
vec3 chunks_spawn_point(World *w);
int chunks_loaded_count(const World *w);
bool chunks_is_loaded(const World *w, vec3 pos);
float chunks_relevance_radius(void);
void chunks_register_cvars(void);   // the world_* chunk cvars (before the first streamed map: world_foliage_test)

#endif
