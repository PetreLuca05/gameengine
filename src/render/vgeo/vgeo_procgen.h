// Procedural test meshes for virtualized geometry (docs/VGEO.md "Test content"): displaced rocks
// (subdivided icosahedra, closed genus 0), tori (closed genus 1), a rock field and a statue built
// from them. Deterministic for a seed: vertices shared by faces are computed in a canonical order so
// the builder welds them into closed surfaces. Generated at build / test time, never committed.
#ifndef DG_VGEO_PROCGEN_H
#define DG_VGEO_PROCGEN_H

#include "core/base.h"

typedef struct VgGenMesh {
    float *pos;                  // xyz
    u32 *indices;
    u32 vertex_count, index_count;
    u32 vcap, icap;
} VgGenMesh;

void vgeo_gen_free(VgGenMesh *m);
// Displaced rock: an icosahedron with `n` subdivisions per edge (20 n^2 triangles), radius r at c.
void vgeo_gen_rock(VgGenMesh *m, u32 seed, u32 n, float r, const float c[3], float displace);
// Torus (seg_u x seg_v x 2 triangles), lying in the xz plane at c.
void vgeo_gen_torus(VgGenMesh *m, u32 seed, float R, float r, u32 seg_u, u32 seg_v, const float c[3], float displace);
// Many rocks on a grid (about `tris` triangles in all, disconnected closed shells).
void vgeo_gen_rock_field(VgGenMesh *m, u32 seed, u64 tris);
// A statue: a body sphere, a head and torus rings (about `tris` triangles).
void vgeo_gen_statue(VgGenMesh *m, u32 seed, u64 tris);

#endif
