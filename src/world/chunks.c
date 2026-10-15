// TEMPORARY STUB - replaced by the real implementation.
#include "chunks.h"
bool chunks_create(World *w, ChunkProviderKind kind, u32 seed) { return false; }
void chunks_destroy(World *w) {}
void chunks_update(World *w, float dt) {}
void chunks_render(World *w, vec3 cam_pos) {}
vec3 chunks_spawn_point(World *w) { return v3(0, 2, 0); }
int chunks_loaded_count(const World *w) { return 0; }
bool chunks_is_loaded(const World *w, vec3 pos) { return false; }
float chunks_relevance_radius(void) { return 100.0f; }
