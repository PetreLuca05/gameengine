// Building kit pieces (docs/BUILDING_KIT.md): one .glb per piece in assets/kits/building, loaded with
// cgltf into plain arrays and checked against the kit conventions (pivot, scale, naming, material
// slots, sockets, collision, LODs, UV sets, texel density). Pure (no renderer, no physics): the game
// (src/world/kit_world.c), tools/validate_kit and tests/test_kit.c share it.
//
// Engine space = glTF space for kit files: +Y up, the piece's front faces -Z (the engine's yaw 0),
// pivot at the bottom-left-back corner: the piece spans x 0..w, y 0..h, z -d..0.
#ifndef DG_KIT_PIECE_H
#define DG_KIT_PIECE_H

#include "core/base.h"

#define KIT_ID 48
#define KIT_MAX_SLOTS 8          // material slots per LOD
#define KIT_MAX_LODS 4
#define KIT_MAX_SOCKETS 48
#define KIT_MAX_HULLS 32
#define KIT_MAX_HULL_VERTS 256

typedef struct KitSlotMesh {
    char slot[32];               // material slot name ("M_paint")
    float *pos, *nrm, *uv0, *uv1;   // 3 / 3 / 2 / 2 floats per vertex (uv1 may be NULL)
    u32 *idx;
    u32 vcount, icount;
} KitSlotMesh;

typedef struct KitLod {
    KitSlotMesh slots[KIT_MAX_SLOTS];
    int slot_n;
    u32 tris;
} KitLod;

typedef struct KitSocket {
    char name[40];               // node name ("S_hinge", "S_light_2")
    char type[24];               // "hinge", "light" (the name without "S_" and a trailing "_<n>")
    float pos[3];
    float rot[4];                // x y z w
} KitSocket;

typedef struct KitHull {
    bool box;                    // UBX_ / UCL_: an axis-aligned box (min / max)
    bool clip;                   // UCL_: blocks characters only (PL_CLIP: an open window's opening)
    float *verts;                // UCX_: piece-space vertices (3 floats each)
    u32 *idx;
    u32 vcount, icount;
    float mn[3], mx[3];
} KitHull;

typedef struct KitPiece {
    char id[KIT_ID];
    KitLod lods[KIT_MAX_LODS];
    int lod_n;
    KitSocket sockets[KIT_MAX_SOCKETS];
    int socket_n;
    KitHull hulls[KIT_MAX_HULLS];
    int hull_n;
    float mn[3], mx[3];          // LOD0 bounds
    int unnamed_nodes;           // mesh nodes that are not <id>, <id>_LODn, UCX_ / UBX_ / UCL_ (validation)
    bool missing_normals;        // a render primitive without NORMAL (validation)
    char first_unnamed[64];
} KitPiece;

// The kit's rules for one piece (assets/kits/building/kit.json, its category).
typedef struct KitRules {
    char category[24];
    char required_sockets[8][24];
    int required_socket_n;
    bool collision_required;
    int max_tris;                // LOD0 triangle budget (0 = none)
    float grid;                  // footprint sizes (x, z) must be multiples of this (m; 0 = no check)
    float grid_height;           // heights must be multiples of this (m; 0 = no check)
    float texel_density;         // px per cm on UV0 (10.24)
    float texel_tolerance;       // relative (0.15)
    bool uv1_required;
} KitRules;

// id: the piece id (file stem). Returns false with a message when the file cannot be read as glTF.
bool kit_piece_load(KitPiece *p, const char *path, const char *id, char *err, int err_size);
void kit_piece_free(KitPiece *p);
void kit_rules_default(KitRules *r);
// Known material slots: a newline-separated list ("M_paint\nM_wood\n..."); NULL = skip the check.
// Writes "<id>: <problem> - <fix>" lines; returns the number of problems.
int kit_piece_validate(const KitPiece *p, const KitRules *r, const char *known_slots, char *out, int out_size);
const KitSocket *kit_piece_socket(const KitPiece *p, const char *type, int nth);

#endif
