// Procedural mesh building (boxes, ramps, cylinders, capsules, spheres...).
#ifndef DG_MESH_GEN_H
#define DG_MESH_GEN_H

#include "render.h"

typedef struct MeshBuilder {
    Vertex *verts;
    u32 vcount, vcap;
    u32 *indices;
    u32 icount, icap;
} MeshBuilder;

void mb_init(MeshBuilder *mb);
void mb_free(MeshBuilder *mb);
void mb_reset(MeshBuilder *mb);
u32 mb_vertex(MeshBuilder *mb, vec3 p, vec3 n, vec2 uv, u32 color);
void mb_tri(MeshBuilder *mb, u32 a, u32 b, u32 c);
void mb_quad(MeshBuilder *mb, vec3 a, vec3 b, vec3 c, vec3 d, vec3 n, u32 color); // CCW a,b,c,d
// Axis-aligned box centered at c with half extents h; UVs are in meters (world scale).
void mb_box(MeshBuilder *mb, vec3 c, vec3 h, u32 color);
void mb_box_transformed(MeshBuilder *mb, mat4 xf, vec3 h, u32 color);
// Ramp: rises along +Z from (y=0 at z=-hz) to (y=height at z=+hz), width along X.
void mb_wedge(MeshBuilder *mb, vec3 base_center, float hx, float hz, float height, u32 color);
void mb_cylinder(MeshBuilder *mb, vec3 c, float radius, float half_height, int segments, u32 color);
void mb_sphere(MeshBuilder *mb, vec3 c, float radius, int rings, int segments, u32 color);
void mb_capsule(MeshBuilder *mb, vec3 c, float radius, float half_height, int rings, int segments, u32 color);
MeshId mb_upload(MeshBuilder *mb);

// Common unit meshes (created once at startup).
typedef struct BasicMeshes {
    MeshId cube;      // unit cube, half extent 0.5
    MeshId sphere;    // radius 0.5
    MeshId cylinder;  // radius 0.5, height 1
    MeshId capsule;   // radius 0.5, cylinder half height 0.5 (total 2.0)
    MeshId quad;      // XY plane, 1x1, facing +Z
} BasicMeshes;

void basic_meshes_create(BasicMeshes *out);

#endif
