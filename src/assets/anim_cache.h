// Binary cache for baked FBX data (assets/cache/). Entries are keyed by the source file's
// hash + bake version + sample rate, so editing an FBX re-bakes automatically.
#ifndef DG_ANIM_CACHE_H
#define DG_ANIM_CACHE_H

#include "anim/anim.h"
#include "anim/anim_mesh.h"

#define ANIM_BAKE_VERSION 5

void anim_cache_set_dir(const char *dir);        // absolute directory (created on demand)
void anim_cache_set_rebuild(bool rebuild);       // --rebuild-cache: ignore existing entries
const char *anim_cache_dir(void);

// Loading through the cache: bake from FBX on a miss, then store.
bool anim_cache_skeleton(const char *fbx_path, Skeleton *out, char *err, int err_size);
bool anim_cache_mesh(const char *fbx_path, const Skeleton *canon, SkinMeshData *out, char *err, int err_size);
int anim_cache_clips(const char *fbx_path, const Skeleton *canon, float rate, Clip *out, int max, char *err, int err_size);
// Stats for the log / tools.
typedef struct AnimCacheStats { int hits, misses, writes; } AnimCacheStats;
AnimCacheStats anim_cache_stats(void);

#endif
