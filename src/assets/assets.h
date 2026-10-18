// Asset loading: glTF 2.0 models (cgltf), PNG/JPG (stb_image) and KTX2 textures.
// data/assets.json is the manifest: model/texture ids are manifest indices, so they are
// identical on every machine and safe to replicate. (WAV/OGG audio: see audio/audio.c.)
#ifndef DG_ASSETS_H
#define DG_ASSETS_H

#include "core/base.h"
#include "core/dgmath.h"
#include "render/render.h"

#define MAX_MODEL_MESHES 16
#define MAX_ASSET_MODELS 64
#define MAX_ASSET_TEXTURES 64

typedef struct ModelMesh {
    MeshId mesh;
    Material mat;
    mat4 local;          // node transform within the model
    bool paint;          // material named "paint*": tinted with the owner's colour (vehicles)
} ModelMesh;

// Manifest "foliage": { ... } of a "vgeo": true model (docs/VGEO.md "Foliage support"): its vgeo
// materials' flags and parameters; "colliders": "capsule" (on the model entry) gives every placement
// instance a static trunk capsule.
typedef struct ModelFoliage {
    bool enabled;
    bool masked, two_sided, dither;   // "masked", "two_sided", "dither" (default true)
    float alpha_cutoff;               // "alpha_cutoff" (0.5)
    int wpo;                          // "wpo": "none" | "grass" | "tree" (VgWpo)
    bool displace;                    // "displace": the displacement buffer bends it (WPO_DISPLACE)
    float wpo_params[4];              // "wpo_params"
    float fade_start, fade_end;       // "fade": [start, end] (m)
    float shadow_max_dist;            // "shadow_max_dist" (m, 0 = always)
    int shadow_class;                 // "shadow_class": "none" (default) | "ground" | "small" | "tree" (render/shadow_casters.h)
    bool capsule;                     // "colliders": "capsule"
    float capsule_radius, capsule_height;   // "capsule": [radius, height] (m, at scale 1)
} ModelFoliage;

typedef struct Model {
    char name[32];
    ModelMesh meshes[MAX_MODEL_MESHES];
    int mesh_count;
    AABB bounds;
    struct VgBuilt *vgeo;    // manifest "vgeo": true: the virtualized-geometry build (docs/VGEO.md), else NULL
    ModelFoliage foliage;
} Model;

bool assets_init(const char *manifest_path);
void assets_shutdown(void);

u16 assets_model_id(const char *name);          // 0 = none
const Model *assets_model(u16 id);
TextureId assets_texture(const char *name);     // 0 if unknown

TextureId texture_load_file(const char *path, bool srgb);   // PNG/JPG/TGA/BMP or .ktx2
TextureId texture_load_ktx2(const char *path);
// r_texture_quality went up: the world textures (glTF maps, manifest textures) decode their sources
// again for the levels the renderer dropped (docs/TEXTURE_MIPS.md). Cheap when there is nothing to do;
// the main loop calls it every frame, between frames.
void assets_reload_textures(void);
bool model_load_gltf(const char *path, Model *out);

#endif
