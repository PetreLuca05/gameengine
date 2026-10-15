// Skinned mesh data (CPU side), shared by the FBX loader, the asset cache, the renderer and tools.
#ifndef DG_ANIM_MESH_H
#define DG_ANIM_MESH_H

#include "anim.h"

#define SKIN_MAX_LODS 4
#define SKIN_MAX_MATERIALS 8

// 44 bytes. joints index the canonical skeleton directly (validated at load).
typedef struct SkinVertex {
    vec3 pos;
    vec3 normal;
    vec2 uv;
    u32 color;
    u8 joints[4];
    u8 weights[4];   // unorm8, sum = 255
} SkinVertex;

typedef struct SkinLod {
    SkinVertex *verts;
    u32 vcount;
    u32 *indices;
    u32 icount;
    u8 *vmat;          // material slot per vertex
    bool generated;    // produced by meshoptimizer (not authored)
} SkinLod;

typedef struct SkinMeshData {
    char source[128];
    u32 file_hash;
    u32 skel_hash;      // canonical skeleton this mesh is bound to
    int lod_count;
    SkinLod lods[SKIN_MAX_LODS];
    int material_count;
    char material_names[SKIN_MAX_MATERIALS][48];
    vec4 material_colors[SKIN_MAX_MATERIALS];
    AABB bounds;
    // Skinning remap info (rig validation): source bones that were folded onto a canonical ancestor.
    int remapped_bones;
} SkinMeshData;

void skin_mesh_free(SkinMeshData *m);
// Fits per-bone capsule radii from the vertices dominantly weighted to each bone
// (used by ragdoll + hitboxes). radius[] gets the 75th percentile distance to the bone segment.
void skin_fit_bone_radii(const SkinMeshData *m, int lod, const Skeleton *s, float *radius);
// Generates missing LODs (target ratios 1, .5, .25, .1) with meshoptimizer, preserving skin data.
void skin_generate_lods(SkinMeshData *m);

#endif
