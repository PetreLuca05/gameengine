// Procedural skinned test content for virtualized geometry (docs/VGEO.md "Skinned meshes"): a
// "creature" of a body sphere (bone 0, the root) and K tapered tentacles (closed capsules) of S bones
// each, smooth weights over up to 4 bones, and an analytic animation (root motion, per-bone swing and
// a pulsing uniform scale) that produces the palettes (bone_world * inverse_bind, column-major mat4).
// Deterministic for a seed; generated at test / bench time, never committed.
#ifndef DG_VGEO_PROCGEN_SKINNED_H
#define DG_VGEO_PROCGEN_SKINNED_H

#include "vgeo_procgen.h"

#define VGEO_GEN_MAX_BONES 128

typedef struct VgGenSkinned {
    VgGenMesh mesh;
    u8 *joints;                  // 4 per vertex
    float *weights;              // 4 per vertex (sum 1)
    u32 bone_count, tentacles, bones_per;
    u32 shells;                  // closed surfaces (body + tentacles): Euler characteristic 2 x shells
    i32 parent[VGEO_GEN_MAX_BONES];
    float head[VGEO_GEN_MAX_BONES][3];   // bind-space bone origins (bind rotations are identity)
    float dir[VGEO_GEN_MAX_BONES][3];    // the bone's tentacle axis
    u32 seed;
} VgGenSkinned;

// About `tris` triangles; 1 + tentacles * bones_per <= 128 bones.
void vgeo_gen_creature(VgGenSkinned *s, u32 seed, u32 tentacles, u32 bones_per, u64 tris);
void vgeo_gen_skinned_free(VgGenSkinned *s);
// The palettes at time t (seconds): bone_count mat4 (16 floats each, column-major). phase offsets the
// animation (several instances out of step).
void vgeo_gen_creature_pose(const VgGenSkinned *s, float t, float phase, float *palette);

#endif
