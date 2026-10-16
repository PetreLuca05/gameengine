#include "assets.h"
#include "core/engine.h"
#include "core/log.h"
#include "core/platform.h"
#include "render/mesh_gen.h"
#include "stb_image.h"
#include "cgltf.h"
#include "cJSON.h"
#include <stdlib.h>
#include <stdio.h>

typedef struct NamedTexture { char name[32]; char path[256]; TextureId id; bool srgb; } NamedTexture;

typedef struct AssetSystem {
    Model models[MAX_ASSET_MODELS];   // index 0 unused
    int model_count;
    NamedTexture textures[MAX_ASSET_TEXTURES];
    int texture_count;
} AssetSystem;

#define AS (g_engine.assets)

// ------------------------------------------------------------------ textures

TextureId texture_load_file(const char *path, bool srgb) {
    size_t n = strlen(path);
    if (n > 5 && SDL_strcasecmp(path + n - 5, ".ktx2") == 0) return texture_load_ktx2(path);
    int w, h, comp;
    stbi_uc *px = stbi_load(path, &w, &h, &comp, 4);
    if (!px) { log_error("texture: cannot load %s (%s)", path, stbi_failure_reason()); return 0; }
    TextureId id = render_texture_create(w, h, px, srgb, true);
    stbi_image_free(px);
    log_info("texture: %s (%dx%d)", path, w, h);
    return id;
}

// KTX2: header + level index, no supercompression. Any VkFormat the device supports.
TextureId texture_load_ktx2(const char *path) {
    size_t size = 0;
    u8 *data = (u8 *)platform_read_file(path, &size);
    if (!data) { log_error("ktx2: cannot read %s", path); return 0; }
    static const u8 ident[12] = { 0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n' };
    TextureId id = 0;
    if (size < 80 || memcmp(data, ident, 12) != 0) { log_error("ktx2: %s is not a KTX2 file", path); goto done; }
    u32 vk_format, type_size, w, h, depth, layers, faces, levels, supercomp;
    memcpy(&vk_format, data + 12, 4);
    memcpy(&type_size, data + 16, 4);
    memcpy(&w, data + 20, 4);
    memcpy(&h, data + 24, 4);
    memcpy(&depth, data + 28, 4);
    memcpy(&layers, data + 32, 4);
    memcpy(&faces, data + 36, 4);
    memcpy(&levels, data + 40, 4);
    memcpy(&supercomp, data + 44, 4);
    (void)type_size;
    if (supercomp != 0 || depth > 1 || layers > 1 || faces != 1) {
        log_error("ktx2: %s uses unsupported features (supercompression %u, depth %u, layers %u, faces %u)", path, supercomp, depth, layers, faces);
        goto done;
    }
    if (levels == 0) levels = 1;
    levels = DG_MIN(levels, 16u);
    const u8 *index = data + 80;
    if (80 + (size_t)levels * 24 > size) { log_error("ktx2: truncated level index"); goto done; }
    // Repack levels contiguously, largest first.
    size_t total = 0, level_sizes[16];
    for (u32 l = 0; l < levels; l++) {
        u64 off, len;
        memcpy(&off, index + l * 24, 8);
        memcpy(&len, index + l * 24 + 8, 8);
        if (off + len > size) { log_error("ktx2: level %u out of range", l); goto done; }
        level_sizes[l] = (size_t)len;
        total += (size_t)len;
    }
    u8 *packed = (u8 *)malloc(total);
    size_t at = 0;
    for (u32 l = 0; l < levels; l++) {
        u64 off;
        memcpy(&off, index + l * 24, 8);
        memcpy(packed + at, data + off, level_sizes[l]);
        at += level_sizes[l];
    }
    id = render_texture_create_raw((int)w, (int)h, (int)levels, vk_format, packed, level_sizes);
    free(packed);
    log_info("ktx2: %s (%ux%u, format %u, %u mips)", path, w, h, vk_format, levels);
done:
    free(data);
    return id;
}

// ------------------------------------------------------------------ glTF

static void read_accessor_vec(const cgltf_accessor *a, u32 i, float *out, int n) {
    cgltf_accessor_read_float(a, i, out, (cgltf_size)n);
}

bool model_load_gltf(const char *path, Model *out) {
    cgltf_options opt;
    memset(&opt, 0, sizeof(opt));
    cgltf_data *data = NULL;
    if (cgltf_parse_file(&opt, path, &data) != cgltf_result_success) { log_error("gltf: cannot parse %s", path); return false; }
    if (cgltf_load_buffers(&opt, data, path) != cgltf_result_success) { log_error("gltf: cannot load buffers for %s", path); cgltf_free(data); return false; }
    memset(out, 0, sizeof(*out));
    bool first_vertex = true;
    for (cgltf_size ni = 0; ni < data->nodes_count; ni++) {
        cgltf_node *node = &data->nodes[ni];
        if (!node->mesh) continue;
        float world[16];
        cgltf_node_transform_world(node, world);
        mat4 xf;
        memcpy(xf.m, world, sizeof(world));
        for (cgltf_size pi = 0; pi < node->mesh->primitives_count && out->mesh_count < MAX_MODEL_MESHES; pi++) {
            cgltf_primitive *prim = &node->mesh->primitives[pi];
            if (prim->type != cgltf_primitive_type_triangles) continue;
            const cgltf_accessor *apos = NULL, *anrm = NULL, *auv = NULL, *acol = NULL;
            for (cgltf_size k = 0; k < prim->attributes_count; k++) {
                cgltf_attribute *at = &prim->attributes[k];
                if (at->type == cgltf_attribute_type_position) apos = at->data;
                else if (at->type == cgltf_attribute_type_normal) anrm = at->data;
                else if (at->type == cgltf_attribute_type_texcoord && at->index == 0) auv = at->data;
                else if (at->type == cgltf_attribute_type_color && at->index == 0) acol = at->data;
            }
            if (!apos) continue;
            MeshBuilder mb;
            mb_init(&mb);
            for (cgltf_size v = 0; v < apos->count; v++) {
                float p[3] = { 0 }, n[3] = { 0, 1, 0 }, t[2] = { 0 }, c[4] = { 1, 1, 1, 1 };
                read_accessor_vec(apos, (u32)v, p, 3);
                if (anrm) read_accessor_vec(anrm, (u32)v, n, 3);
                if (auv) read_accessor_vec(auv, (u32)v, t, 2);
                if (acol) read_accessor_vec(acol, (u32)v, c, acol->type == cgltf_type_vec3 ? 3 : 4);
                vec3 wp = mat4_mul_point(xf, v3(p[0], p[1], p[2]));
                if (first_vertex) { out->bounds.min = out->bounds.max = wp; first_vertex = false; }
                out->bounds.min = v3_min(out->bounds.min, wp);
                out->bounds.max = v3_max(out->bounds.max, wp);
                mb_vertex(&mb, v3(p[0], p[1], p[2]), v3(n[0], n[1], n[2]), v2(t[0], t[1]), rgba8(c[0], c[1], c[2], c[3]));
            }
            if (prim->indices) {
                for (cgltf_size k = 0; k + 2 < prim->indices->count; k += 3)
                    mb_tri(&mb, (u32)cgltf_accessor_read_index(prim->indices, k), (u32)cgltf_accessor_read_index(prim->indices, k + 1),
                           (u32)cgltf_accessor_read_index(prim->indices, k + 2));
            } else {
                for (u32 k = 0; k + 2 < (u32)apos->count; k += 3) mb_tri(&mb, k, k + 1, k + 2);
            }
            ModelMesh *mm = &out->meshes[out->mesh_count++];
            mm->mesh = mb_upload(&mb);
            mm->local = xf;
            mm->mat = material_color(v4(1, 1, 1, 1));
            mm->paint = prim->material && prim->material->name && !strncmp(prim->material->name, "paint", 5);
            if (prim->material) {
                // Emissive (lights): the factor's brightest channel x KHR_materials_emissive_strength.
                const float *ef = prim->material->emissive_factor;
                float e = fmaxf(ef[0], fmaxf(ef[1], ef[2]));
                if (prim->material->has_emissive_strength) e *= prim->material->emissive_strength.emissive_strength;
                mm->mat.emissive = e;
            }
            if (prim->material && prim->material->has_pbr_metallic_roughness) {
                const cgltf_pbr_metallic_roughness *pbr = &prim->material->pbr_metallic_roughness;
                mm->mat.color = v4(pbr->base_color_factor[0], pbr->base_color_factor[1], pbr->base_color_factor[2], pbr->base_color_factor[3]);
                mm->mat.metallic = pbr->metallic_factor;
                mm->mat.roughness = pbr->roughness_factor;
                const cgltf_texture *tex = pbr->base_color_texture.texture;
                if (tex && tex->image && tex->image->uri && strncmp(tex->image->uri, "data:", 5) != 0) {
                    char tpath[512];
                    const char *slash = strrchr(path, '/');
                    if (!slash) slash = strrchr(path, '\\');
                    int dir_len = slash ? (int)(slash - path + 1) : 0;
                    snprintf(tpath, sizeof(tpath), "%.*s%s", dir_len, path, tex->image->uri);
                    mm->mat.texture = texture_load_file(tpath, true);
                    if (mm->mat.texture) mm->mat.flags |= MAT_TEXTURED;
                }
            }
            mb_free(&mb);
        }
    }
    cgltf_free(data);
    log_info("gltf: %s (%d meshes)", path, out->mesh_count);
    return out->mesh_count > 0;
}

// ------------------------------------------------------------------ manifest

bool assets_init(const char *manifest_path) {
    AS = (AssetSystem *)calloc(1, sizeof(AssetSystem));
    AS->model_count = 1;
    char buf[512];
    char *text = platform_read_file(platform_data_path(manifest_path, buf, sizeof(buf)), NULL);
    if (!text) { log_warn("assets: no manifest at %s", manifest_path); return false; }
    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) { log_error("assets: manifest parse error"); return false; }
    const cJSON *it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "textures")) {
        if (AS->texture_count >= MAX_ASSET_TEXTURES) break;
        NamedTexture *t = &AS->textures[AS->texture_count++];
        const cJSON *n = cJSON_GetObjectItemCaseSensitive(it, "name"), *p = cJSON_GetObjectItemCaseSensitive(it, "path");
        dg_strcpy(t->name, sizeof(t->name), cJSON_IsString(n) ? n->valuestring : "");
        dg_strcpy(t->path, sizeof(t->path), cJSON_IsString(p) ? p->valuestring : "");
        t->srgb = !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(it, "srgb"));
        t->id = texture_load_file(platform_data_path(t->path, buf, sizeof(buf)), t->srgb);
    }
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "models")) {
        if (AS->model_count >= MAX_ASSET_MODELS) break;
        const cJSON *n = cJSON_GetObjectItemCaseSensitive(it, "name"), *p = cJSON_GetObjectItemCaseSensitive(it, "path");
        Model *m = &AS->models[AS->model_count];
        if (model_load_gltf(platform_data_path(cJSON_IsString(p) ? p->valuestring : "", buf, sizeof(buf)), m)) {
            dg_strcpy(m->name, sizeof(m->name), cJSON_IsString(n) ? n->valuestring : "");
        }
        AS->model_count++; // keep indices stable even if a model failed to load
    }
    cJSON_Delete(root);
    return true;
}

void assets_shutdown(void) {
    free(AS);
    AS = NULL;
}

u16 assets_model_id(const char *name) {
    if (!AS || !name || !name[0]) return 0;
    for (int i = 1; i < AS->model_count; i++) if (!strcmp(AS->models[i].name, name)) return (u16)i;
    return 0;
}

const Model *assets_model(u16 id) {
    if (!AS || id == 0 || id >= AS->model_count || !AS->models[id].mesh_count) return NULL;
    return &AS->models[id];
}

TextureId assets_texture(const char *name) {
    if (!AS) return 0;
    for (int i = 0; i < AS->texture_count; i++) if (!strcmp(AS->textures[i].name, name)) return AS->textures[i].id;
    return 0;
}
