// Detailed item meshes (docs/ITEM_PIPELINE.md): weapons, attachments and items authored in Blender
// (assets/src/items/<id>.py -> tools/blender/item_export.py) and exported as .dgim files. A mesh is
// a tree of named parts with pivots (the moving ones: slide / bolt / pump / magazine, folding iron
// sights, parts hidden by an attachment slot), named sockets, up to ITEM_MESH_LODS levels per part
// and a convex hull per part. Item space as everywhere for held items: origin = grip_r, -Z forward,
// +Y up, metres.
//
// No textures: every vertex carries a material id and baked masks (edge wear from curvature, AO,
// dirt, grunge noise, cavity). The loader layers the material library (assets/items/materials.json)
// over them per condition (new / worn / rusty) into vertex colours (sqrt albedo, a = AO) and
// (roughness, metallic) in the uv: MAT_ROUGHNESS_VERTEX draws, one per moving part, the static parts merged.
#ifndef DG_ITEM_MESH_H
#define DG_ITEM_MESH_H

#include "core/base.h"
#include "core/dgmath.h"
#include "anim/anim.h"
#include "render/render.h"

#define ITEM_MESH_LODS 3
#define ITEM_MESH_MAX_PARTS 64
#define ITEM_MESH_MAX_SOCKETS 32
#define ITEM_MESH_MAX_MATS 24
#define ITEM_MESH_MAX_GROUPS 16
#define ITEM_MESH_MAX 96

typedef enum ItemCondition { ITEM_COND_NEW, ITEM_COND_WORN, ITEM_COND_RUSTY, ITEM_COND_COUNT } ItemCondition;

// On-disk layout (little endian, tools/blender/item_export.py writes it, tools/validate_item reads it):
// the header, then the payload zlib-compressed (packed_bytes -> payload_bytes): materials, parts, sockets,
// hulls, vertices, u16 indices (relative to the LOD's first vertex), hull points (float[3], part-local).
#define DGIM_MAGIC 0x4D494744u   // "DGIM"
#define DGIM_VERSION 2u
typedef struct DgimHeader {
    u32 magic, version;
    u32 part_count, socket_count, material_count, hull_count;
    u32 vertex_count, index_count, hull_point_count;
    u32 flags;
    u32 payload_bytes, packed_bytes;
    float lod_dist[ITEM_MESH_LODS];   // m from the camera at a 75 deg FOV where each LOD starts (lod_dist[0] = 0)
    float bmin[3], bmax[3];           // item space bounds (rest pose)
    char id[32];
} DgimHeader;
typedef struct DgimMaterial { char name[32]; } DgimMaterial;   // slot names, resolved in the library
typedef struct DgimPart {
    char name[32];
    i32 parent;                       // part index, -1 = the item root
    float pos[3], rot[4];             // pivot relative to the parent (rot x y z w)
    char mech[16];                    // "", "slide", "bolt", "pump", "mag" (PartMech) or a static role ("trigger", ...)
    char hide[16];                    // attachment slot that hides it while filled ("magazine", "stock", "optic", ...)
    char fold[16];                    // attachment slot that folds it (flip-up irons: "optic")
    float fold_axis[3], fold_angle;   // part-local hinge axis, degrees
    float bmin[3], bmax[3];           // part-local bounds (also the vertex position quantisation box)
    u32 lod_count;
    u32 lod_v0[ITEM_MESH_LODS], lod_vn[ITEM_MESH_LODS], lod_i0[ITEM_MESH_LODS], lod_in[ITEM_MESH_LODS];
} DgimPart;
typedef struct DgimSocket { char name[32]; i32 part; float pos[3], rot[4]; } DgimSocket;
typedef struct DgimHull { i32 part; u32 first, count; } DgimHull;
// Vertex (16 bytes): position quantised in the part's bmin..bmax (u16), octahedral normal (i16 x 2),
// material slot and the baked masks (0..255).
typedef struct DgimVertex { u16 pos[3]; i16 oct[2]; u8 mat, wear, dirt, ao, grunge, cavity; } DgimVertex;

typedef struct ItemMeshPart {
    char name[32];
    int parent;
    Xform local, rest;          // pivot in the parent / in item space (rest pose)
    char role[16];
    u8 mech;                    // PartMech
    i8 hide_slot, fold_slot;    // AttachSlot, -1 = none
    vec3 fold_axis;
    float fold_angle;           // radians
    vec3 bmin, bmax;
    int group;                  // draw group (0 = the merged static body)
    u32 tris[ITEM_MESH_LODS];
    int lod_count;
} ItemMeshPart;

typedef struct ItemMeshSocket { char name[32]; int part; Xform local; } ItemMeshSocket;

typedef struct ItemMeshGroup {
    int part;                   // the moving part the group follows, -1 = the static body (item space)
    MeshId mesh[ITEM_COND_COUNT][ITEM_MESH_LODS];
    u32 tris[ITEM_MESH_LODS];
} ItemMeshGroup;

typedef struct ItemMesh {
    char path[96];
    char id[32];
    bool loaded, failed;
    int part_count, socket_count, group_count, lod_count;
    ItemMeshPart parts[ITEM_MESH_MAX_PARTS];
    ItemMeshSocket sockets[ITEM_MESH_MAX_SOCKETS];
    ItemMeshGroup groups[ITEM_MESH_MAX_GROUPS];
    bool built[ITEM_COND_COUNT];   // the condition's GPU meshes exist
    char mats[ITEM_MESH_MAX_MATS][32];
    int mat_count;
    float lod_dist[ITEM_MESH_LODS];
    vec3 bmin, bmax;
    u32 tris[ITEM_MESH_LODS];   // whole item per LOD
    int hull_count;
    // CPU copy of the file (vertices / indices / hulls): conditions are composed lazily, item_reload re-reads it.
    void *blob;                 // the decompressed payload
    const DgimPart *dparts;
    const DgimVertex *verts;
    const u16 *indices;
    const DgimHull *hulls;
    const float *hull_points;
    i64 mtime;
} ItemMesh;

// Per-draw pose: the fire animation's slide travel (+z = back) and magazine wobble, the attachment slots
// that hide / fold parts (bit = AttachSlot).
typedef struct ItemMeshPose {
    float slide;
    quat wobble;
    u8 hide_slots, fold_slots;
} ItemMeshPose;

// The mesh for a path ("assets/items/meshes/rifle.dgim"), loaded on first use (NULL when missing / bad:
// callers keep their box placeholder). Cached for the session.
ItemMesh *item_mesh_get(const char *path);
// Condition for draws without an instance condition: the item's default, or the item_condition override.
int item_mesh_condition(int item_default);
// "new" | "worn" | "rusty"; -1 = unknown (inline: gameplay parses it without the renderer)
DG_INLINE int item_condition_from(const char *s) {
    if (!s) return -1;
    if (!strcmp(s, "new")) return ITEM_COND_NEW;
    if (!strcmp(s, "worn")) return ITEM_COND_WORN;
    if (!strcmp(s, "rusty")) return ITEM_COND_RUSTY;
    return -1;
}
const char *item_condition_name(int c);
// Draw at pos / rot (item space -> world, uniform scale). viewmodel: the first-person pass (always LOD 0).
void item_mesh_draw(ItemMesh *m, int condition, vec3 pos, quat rot, float scale, const ItemMeshPose *pose, u32 flags, bool viewmodel);
// UI item views (render_ui3d): one part per draw group, placed by root (item space -> view model space).
int item_mesh_ui_parts(ItemMesh *m, int condition, Xform root, const ItemMeshPose *pose, float highlight, RenderUi3dPart *out, int max, vec3 *mn,
                       vec3 *mx);
// Posed transform of a part in item space.
Xform item_mesh_part_xform(const ItemMesh *m, int part, const ItemMeshPose *pose);
int item_mesh_find_part(const ItemMesh *m, const char *name);
vec3 item_mesh_vertex_pos(const DgimPart *dp, const DgimVertex *v);   // part-local position of a file vertex
// A socket in item space (rest pose); false when the mesh has none of that name.
bool item_mesh_socket(const ItemMesh *m, const char *name, Xform *out);
// LOD a world draw at this distance (m) uses (FOV-corrected by the caller's tan ratio).
int item_mesh_lod_for(const ItemMesh *m, float dist);
// Re-reads the material library and every loaded mesh whose file changed (item_reload); returns meshes reloaded.
int item_mesh_reload_all(bool force);
// Checks one loaded mesh's file per call, round robin, and re-reads it when it changed on disk (an export
// from Blender, docs/ITEM_PIPELINE.md "Edit items in Blender"); returns the mesh re-read, or NULL.
ItemMesh *item_mesh_poll(void);
void item_mesh_shutdown(void);
// Stats for item_inspect / tests.
int item_mesh_count(void);
ItemMesh *item_mesh_at(int i);
// Inspector views: 0 shaded, 1 material id colours, 2 wear mask, 3 AO, 4 dirt (re-composes the meshes).
void item_mesh_set_debug_view(int view);
int item_mesh_debug_view(void);
void item_mesh_register(void);

#endif
