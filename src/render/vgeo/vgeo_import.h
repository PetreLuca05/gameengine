// Virtualized geometry import (docs/VGEO.md "Import"): glTF triangles -> the builder, cached as
// assets/cache/<stem>.vgeo.<key %08x>.dgc (key = FNV-1a over the file bytes, VGEO_BUILD_VERSION and the
// build options; same naming and versioned-header convention as the animation cache).
#ifndef DG_VGEO_IMPORT_H
#define DG_VGEO_IMPORT_H

#include "vgeo_build.h"

// Reads every triangle primitive of a glTF (node transforms applied) into owned arrays.
typedef struct VgImported {
    float *pos, *normal, *uv;
    u32 *indices;
    u16 *tri_material;
    u32 vertex_count, index_count, material_count;
    char (*material_names)[48];
} VgImported;
bool vgeo_import_gltf(const char *path, VgImported *out, char *err, int err_size);
void vgeo_imported_free(VgImported *m);
void vgeo_imported_source(const VgImported *m, VgSource *src);

// Loads the cached build of a glTF or builds (and caches) it. cache_dir ends with a separator (NULL:
// no cache). *from_cache tells which happened.
bool vgeo_load_or_build(const char *gltf_path, const char *cache_dir, const VgBuildOptions *opt, VgBuilt *out, bool *from_cache, char *err, int err_size);
u32 vgeo_cache_key(const void *file, size_t size, const VgBuildOptions *opt);

// .vgeo files loaded once per process (level collision and the renderer share them): NULL when the
// file is missing / invalid. Kept until vgeo_file_release_all (shutdown).
const VgBuilt *vgeo_file_get(const char *path);
void vgeo_file_release_all(void);

// Instances of a level placement (LevelVgeo / RenderVgeoPlacement): nx x nz grid around pos at spacing,
// a deterministic random yaw per grid instance (seed) and scale * (1 +- jitter). Returns the count.
typedef struct VgPlacementXf { float pos[3], rot[4], scale; } VgPlacementXf;
u32 vgeo_placement_expand(const float pos[3], float yaw, float scale, float jitter, float spacing, u32 nx, u32 nz, u32 seed, VgPlacementXf *out, u32 cap);

#endif
