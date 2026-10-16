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

typedef struct Model {
    char name[32];
    ModelMesh meshes[MAX_MODEL_MESHES];
    int mesh_count;
    AABB bounds;
} Model;

bool assets_init(const char *manifest_path);
void assets_shutdown(void);

u16 assets_model_id(const char *name);          // 0 = none
const Model *assets_model(u16 id);
TextureId assets_texture(const char *name);     // 0 if unknown

TextureId texture_load_file(const char *path, bool srgb);   // PNG/JPG/TGA/BMP or .ktx2
TextureId texture_load_ktx2(const char *path);
bool model_load_gltf(const char *path, Model *out);

#endif
