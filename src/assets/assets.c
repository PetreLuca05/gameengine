#include "assets.h"
#include "core/engine.h"
#include "core/log.h"
#include "core/platform.h"
#include "render/mesh_gen.h"
#include "render/vgeo/vgeo_import.h"
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

// World textures (glTF maps, the manifest; docs/TEXTURE_MIPS.md) remember their source: a higher
// r_texture_quality needs the levels the renderer dropped, decoded again (assets_reload_textures).
// image >= 0: that image of the glTF file at path (embedded or next to it), else the file itself.
// World textures live for the session (never destroyed), so an id always names its source.
typedef struct TexSource { TextureId id; int image; bool srgb; char path[512]; } TexSource;
static TexSource *g_tex_src;
static int g_tex_src_count, g_tex_src_cap;

static void tex_source_add(TextureId id, const char *path, int image, bool srgb) {
    if (!id) return;
    if (g_tex_src_count == g_tex_src_cap) {
        int cap = g_tex_src_cap ? g_tex_src_cap * 2 : 64;
        TexSource *n = (TexSource *)realloc(g_tex_src, sizeof(TexSource) * (size_t)cap);
        if (!n) return;
        g_tex_src = n;
        g_tex_src_cap = cap;
    }
    TexSource *t = &g_tex_src[g_tex_src_count++];
    t->id = id;
    t->image = image;
    t->srgb = srgb;
    dg_strcpy(t->path, sizeof(t->path), path);
}

// Decoded pixels to the renderer: a texture (world: r_texture_quality applies), or world texture
// `reload` again.
static TextureId upload_rgba(int w, int h, const u8 *px, bool srgb, bool world, TextureId reload) {
    if (reload) { render_texture_reload(reload, w, h, px); return reload; }
    return world ? render_texture_create_world(w, h, px, srgb) : render_texture_create(w, h, px, srgb, true);
}

#define KTX_VK_R8G8B8A8_UNORM 37u
#define KTX_VK_R8G8B8A8_SRGB 43u

// KTX2: header + level index, no supercompression. Any VkFormat the device supports. levelCount 0
// means "generate the chain at load": done for RGBA8; other formats keep their one level.
static TextureId ktx2_load(const char *path, bool world, TextureId reload) {
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
    bool gen_mips = levels == 0;
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
    if (gen_mips && (vk_format == KTX_VK_R8G8B8A8_UNORM || vk_format == KTX_VK_R8G8B8A8_SRGB) && level_sizes[0] == (size_t)w * h * 4) {
        u64 off;
        memcpy(&off, index, 8);
        id = upload_rgba((int)w, (int)h, data + off, vk_format == KTX_VK_R8G8B8A8_SRGB, world, reload);
        log_info("ktx2: %s (%ux%u, format %u, mips generated)", path, w, h, vk_format);
        goto done;
    }
    if (gen_mips) log_warn("ktx2: %s asks for generated mips, format %u cannot be blitted at load: one level", path, vk_format);
    u8 *packed = (u8 *)malloc(total);
    size_t at = 0;
    for (u32 l = 0; l < levels; l++) {
        u64 off;
        memcpy(&off, index + l * 24, 8);
        memcpy(packed + at, data + off, level_sizes[l]);
        at += level_sizes[l];
    }
    if (reload) {
        render_texture_reload_raw(reload, (int)w, (int)h, (int)levels, vk_format, packed, level_sizes);
        id = reload;
    } else {
        id = world ? render_texture_create_raw_world((int)w, (int)h, (int)levels, vk_format, packed, level_sizes)
                   : render_texture_create_raw((int)w, (int)h, (int)levels, vk_format, packed, level_sizes);
    }
    free(packed);
    log_info("ktx2: %s (%ux%u, format %u, %u mips)", path, w, h, vk_format, levels);
done:
    free(data);
    return id;
}

static TextureId texture_load_path(const char *path, bool srgb, bool world, TextureId reload) {
    size_t n = strlen(path);
    if (n > 5 && SDL_strcasecmp(path + n - 5, ".ktx2") == 0) return ktx2_load(path, world, reload);
    int w, h, comp;
    stbi_uc *px = stbi_load(path, &w, &h, &comp, 4);
    if (!px) { log_error("texture: cannot load %s (%s)", path, stbi_failure_reason()); return 0; }
    TextureId id = upload_rgba(w, h, px, srgb, world, reload);
    stbi_image_free(px);
    log_info("texture: %s (%dx%d)", path, w, h);
    return id;
}

TextureId texture_load_file(const char *path, bool srgb) { return texture_load_path(path, srgb, false, 0); }
TextureId texture_load_ktx2(const char *path) { return ktx2_load(path, false, 0); }

// A world texture from a file (the manifest; glTF images register with their image index instead).
static TextureId texture_load_world(const char *path, bool srgb) {
    TextureId id = texture_load_path(path, srgb, true, 0);
    tex_source_add(id, path, -1, srgb);
    return id;
}

// ------------------------------------------------------------------ glTF

static void read_accessor_vec(const cgltf_accessor *a, u32 i, float *out, int n) {
    cgltf_accessor_read_float(a, i, out, (cgltf_size)n);
}

static TextureId texture_load_memory(const void *data, size_t size, bool srgb, const char *what, TextureId reload) {
    int w, h, comp;
    stbi_uc *px = stbi_load_from_memory((const stbi_uc *)data, (int)size, &w, &h, &comp, 4);
    if (!px) { log_error("texture: cannot decode %s (%s)", what, stbi_failure_reason()); return 0; }
    TextureId id = upload_rgba(w, h, px, srgb, true, reload);
    stbi_image_free(px);
    log_info("texture: %s (%dx%d, embedded)", what, w, h);
    return id;
}

// glTF material textures (docs/PBR.md): files next to the .gltf (percent-decoded uri), embedded
// buffer views (.glb) and base64 data: uris. Each image is loaded once per file and colour space
// (one image as occlusion + metallic-roughness is common).
typedef struct GltfTexCache { const cgltf_image *img; bool srgb; TextureId id; } GltfTexCache;
typedef struct GltfTexLoad { const char *path; const cgltf_data *data; GltfTexCache c[32]; int n; } GltfTexLoad;

// A glTF image as a world texture (or world texture `reload` again): a buffer view (.glb), a base64
// data: uri, or a file next to the .gltf (percent-decoded uri).
static TextureId gltf_image_load(const char *gltf_path, const cgltf_image *img, bool srgb, const char *name, TextureId reload) {
    TextureId id = 0;
    if (img->buffer_view) {
        const u8 *data = cgltf_buffer_view_data(img->buffer_view);
        if (data) id = texture_load_memory(data, img->buffer_view->size, srgb, name, reload);
    } else if (img->uri && !strncmp(img->uri, "data:", 5)) {
        const char *comma = strchr(img->uri, ',');
        if (comma && comma - img->uri >= 7 && !strncmp(comma - 7, ";base64", 7)) {
            size_t n = strlen(comma + 1), pad = n >= 2 && comma[n] == '=' ? (comma[n - 1] == '=' ? 2 : 1) : 0;
            cgltf_options o;
            memset(&o, 0, sizeof(o));
            void *data = NULL;
            if (n >= 4 && cgltf_load_buffer_base64(&o, n / 4 * 3 - pad, comma + 1, &data) == cgltf_result_success) {
                id = texture_load_memory(data, n / 4 * 3 - pad, srgb, name, reload);
                free(data);
            }
        }
    } else if (img->uri) {
        char tpath[512];
        const char *slash = strrchr(gltf_path, '/');
        const char *bslash = strrchr(gltf_path, '\\');
        if (bslash > slash) slash = bslash;
        int dir_len = slash ? (int)(slash - gltf_path + 1) : 0;
        snprintf(tpath, sizeof(tpath), "%.*s%s", dir_len, gltf_path, img->uri);
        cgltf_decode_uri(tpath + dir_len);
        id = texture_load_path(tpath, srgb, true, reload);
    }
    return id;
}

static TextureId gltf_texture(GltfTexLoad *L, const cgltf_texture_view *tv, bool srgb, const char *what) {
    const cgltf_image *img = tv->texture ? tv->texture->image : NULL;
    if (!img) return 0;
    for (int i = 0; i < L->n; i++)
        if (L->c[i].img == img && L->c[i].srgb == srgb) return L->c[i].id;
    if (tv->texcoord != 0) log_warn("gltf: %s: %s texture uses TEXCOORD_%d, only TEXCOORD_0 is imported", L->path, what, tv->texcoord);
    char name[512];
    snprintf(name, sizeof(name), "%s %s", L->path, what);
    TextureId id = gltf_image_load(L->path, img, srgb, name, 0);
    if (!id) log_warn("gltf: %s: %s texture not loaded", L->path, what);
    else tex_source_add(id, L->path, (int)cgltf_image_index(L->data, img), srgb);
    if (L->n < (int)(sizeof(L->c) / sizeof(L->c[0]))) L->c[L->n++] = (GltfTexCache){ img, srgb, id };
    return id;
}

// AO settings of a material from a JSON object (glTF extras, the manifest's "ao" block); absent keys keep
// the material's. ao_strength 0 in data means "no screen-space AO" (MATERIAL_AO_OFF), not automatic.
static void model_parse_ao(Material *mat, const cJSON *o, const char *k_strength, const char *k_combine, const char *k_vertex) {
    if (!cJSON_IsObject(o)) return;
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k_strength);
    if (cJSON_IsNumber(v)) mat->ao_strength = v->valuedouble <= 0.0 ? MATERIAL_AO_OFF : (float)v->valuedouble;
    v = cJSON_GetObjectItemCaseSensitive(o, k_combine);
    if (cJSON_IsString(v)) mat->ao_combine = (u8)material_ao_combine_parse(v->valuestring);
    v = cJSON_GetObjectItemCaseSensitive(o, k_vertex);
    if (cJSON_IsBool(v)) mat->ao_vertex = cJSON_IsTrue(v);
}

// KHR_texture_transform: one uv for every map, so only a uniform scale without offset / rotation on
// the base colour is honoured (as tex_scale); anything else is reported and ignored.
static float gltf_uv_scale(const char *path, const cgltf_texture_view *tv) {
    if (!tv->texture || !tv->has_transform) return 1.0f;
    const cgltf_texture_transform *t = &tv->transform;
    if (t->offset[0] == 0 && t->offset[1] == 0 && t->rotation == 0 && t->scale[0] == t->scale[1] && t->scale[0] > 0) return t->scale[0];
    log_warn("gltf: %s: KHR_texture_transform with offset / rotation / non-uniform scale is not supported (ignored)", path);
    return 1.0f;
}

// The glTF material of a primitive (docs/PBR.md): factors, the base colour / metallic-roughness /
// normal / occlusion / emissive textures, alphaMode MASK + alphaCutoff, doubleSided. Missing maps
// leave the factors alone (unchanged look for content without them).
static void gltf_material(GltfTexLoad *L, const cgltf_material *gm, Material *mat) {
    // Emissive (lights): the factor's brightest channel x KHR_materials_emissive_strength. An emissive
    // texture keeps the factor's colour too; without one the emission stays albedo-tinted (as before).
    const float *ef = gm->emissive_factor;
    float e = fmaxf(ef[0], fmaxf(ef[1], ef[2]));
    float es = gm->has_emissive_strength ? gm->emissive_strength.emissive_strength : 1.0f;
    mat->emissive = e * es;
    if (gm->emissive_texture.texture && e > 0) {
        mat->emissive_tex = gltf_texture(L, &gm->emissive_texture, true, "emissive");
        if (mat->emissive_tex) {
            mat->flags |= MAT_EMISSIVE_TEX;
            mat->emissive_rgb = rgba8(ef[0] / e, ef[1] / e, ef[2] / e, 1.0f);
        }
    }
    if (gm->has_pbr_metallic_roughness) {
        const cgltf_pbr_metallic_roughness *pbr = &gm->pbr_metallic_roughness;
        mat->color = v4(pbr->base_color_factor[0], pbr->base_color_factor[1], pbr->base_color_factor[2], pbr->base_color_factor[3]);
        mat->metallic = pbr->metallic_factor;
        mat->roughness = pbr->roughness_factor;
        if (pbr->base_color_texture.texture) {
            mat->texture = gltf_texture(L, &pbr->base_color_texture, true, "base colour");
            if (mat->texture) mat->flags |= MAT_TEXTURED;
            mat->tex_scale = gltf_uv_scale(L->path, &pbr->base_color_texture);
        }
        if (pbr->metallic_roughness_texture.texture) {
            mat->mr_tex = gltf_texture(L, &pbr->metallic_roughness_texture, false, "metallic-roughness");
            if (mat->mr_tex) mat->flags |= MAT_MR_TEX;
        }
    }
    if (gm->normal_texture.texture) {
        mat->normal_tex = gltf_texture(L, &gm->normal_texture, false, "normal");
        mat->normal_scale = gm->normal_texture.scale;
        if (mat->normal_tex) mat->flags |= MAT_NORMAL_TEX;
    }
    if (gm->occlusion_texture.texture) {
        mat->occlusion_tex = gltf_texture(L, &gm->occlusion_texture, false, "occlusion");
        mat->occlusion_strength = gm->occlusion_texture.scale;   // cgltf keeps occlusionTexture.strength in scale
        if (mat->occlusion_tex) mat->flags |= MAT_OCCLUSION_TEX;
    }
    if (gm->alpha_mode == cgltf_alpha_mode_mask) {
        mat->flags |= MAT_ALPHA_MASK;
        mat->alpha_cutoff = gm->alpha_cutoff;
    } else if (gm->alpha_mode == cgltf_alpha_mode_blend && mat->color.w >= 0.999f) {
        log_warn("gltf: %s: alphaMode BLEND with an opaque base colour factor is drawn opaque", L->path);
    }
    if (gm->double_sided) mat->flags |= MAT_TWO_SIDED;
    // Ambient occlusion (render/material_ao.h): "extras": { "ao_strength", "ao_combine", "ao_vertex" }.
    if (gm->extras.data) {
        cJSON *x = cJSON_Parse(gm->extras.data);
        model_parse_ao(mat, x, "ao_strength", "ao_combine", "ao_vertex");
        cJSON_Delete(x);
    }
}

bool model_load_gltf(const char *path, Model *out) {
    cgltf_options opt;
    memset(&opt, 0, sizeof(opt));
    cgltf_data *data = NULL;
    if (cgltf_parse_file(&opt, path, &data) != cgltf_result_success) { log_error("gltf: cannot parse %s", path); return false; }
    if (cgltf_load_buffers(&opt, data, path) != cgltf_result_success) { log_error("gltf: cannot load buffers for %s", path); cgltf_free(data); return false; }
    memset(out, 0, sizeof(*out));
    bool first_vertex = true;
    GltfTexLoad tl;
    memset(&tl, 0, sizeof(tl));
    tl.path = path;
    tl.data = data;
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
            if (prim->material) gltf_material(&tl, prim->material, &mm->mat);
            mb_free(&mb);
        }
    }
    cgltf_free(data);
    log_info("gltf: %s (%d meshes)", path, out->mesh_count);
    return out->mesh_count > 0;
}

// ------------------------------------------------------------------ virtualized geometry

// Builds (or loads the cached build of) the model's clusters + DAG. Phase 1: the data is kept on the
// model; the GPU path consumes it from Phase 2 on (docs/VGEO.md).
static void model_load_vgeo(Model *m, const char *gltf_path) {
    char cache[512];
    platform_writable_dir("assets/cache/", cache, sizeof(cache));
    VgBuildOptions opt;
    memset(&opt, 0, sizeof(opt));
    VgBuilt *b = (VgBuilt *)calloc(1, sizeof(VgBuilt));
    bool cached = false;
    char err[256] = "";
    u64 t0 = SDL_GetTicksNS();
    if (!vgeo_load_or_build(gltf_path, cache, &opt, b, &cached, err, sizeof(err))) {
        log_error("vgeo: %s: %s", gltf_path, err);
        free(b);
        return;
    }
    m->vgeo = b;
    log_info("vgeo: %s %s: %u clusters, %u groups, %u pages, %u levels, %u lods (%.1f ms)", m->name, cached ? "cached" : "built", b->hdr.cluster_count,
             b->hdr.group_count, b->hdr.page_count, b->hdr.level_count, b->hdr.lod_count, (double)(SDL_GetTicksNS() - t0) / 1e6);
}

// "foliage": { "masked", "two_sided", "alpha_cutoff", "wpo", "displace", "wpo_params", "fade",
// "shadow_max_dist" } and "colliders": "capsule" + "capsule": [radius, height].
static void model_parse_foliage(Model *m, const cJSON *it) {
    ModelFoliage *f = &m->foliage;
    memset(f, 0, sizeof(*f));
    const cJSON *col = cJSON_GetObjectItemCaseSensitive(it, "colliders"), *cap = cJSON_GetObjectItemCaseSensitive(it, "capsule");
    f->capsule = cJSON_IsString(col) && !strcmp(col->valuestring, "capsule");
    f->capsule_radius = 0.15f;
    f->capsule_height = 2.0f;
    if (cJSON_IsArray(cap) && cJSON_GetArraySize(cap) >= 2) {
        f->capsule_radius = (float)cJSON_GetArrayItem(cap, 0)->valuedouble;
        f->capsule_height = (float)cJSON_GetArrayItem(cap, 1)->valuedouble;
    }
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(it, "foliage");
    if (!cJSON_IsObject(o)) return;
    f->enabled = true;
    f->masked = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "masked"));
    f->two_sided = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "two_sided"));
    f->dither = !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(o, "dither"));
    f->displace = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "displace"));
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, "alpha_cutoff");
    f->alpha_cutoff = cJSON_IsNumber(v) ? (float)v->valuedouble : 0.5f;
    v = cJSON_GetObjectItemCaseSensitive(o, "wpo");
    f->wpo = cJSON_IsString(v) ? (!strcmp(v->valuestring, "grass") ? 1 : !strcmp(v->valuestring, "tree") ? 2 : 0) : 0;
    v = cJSON_GetObjectItemCaseSensitive(o, "wpo_params");
    for (int k = 0; k < 4 && cJSON_IsArray(v) && k < cJSON_GetArraySize(v); k++) f->wpo_params[k] = (float)cJSON_GetArrayItem(v, k)->valuedouble;
    v = cJSON_GetObjectItemCaseSensitive(o, "fade");
    if (cJSON_IsArray(v) && cJSON_GetArraySize(v) >= 2) {
        f->fade_start = (float)cJSON_GetArrayItem(v, 0)->valuedouble;
        f->fade_end = (float)cJSON_GetArrayItem(v, 1)->valuedouble;
    }
    v = cJSON_GetObjectItemCaseSensitive(o, "shadow_max_dist");
    f->shadow_max_dist = cJSON_IsNumber(v) ? (float)v->valuedouble : 0.0f;
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
        t->id = texture_load_world(platform_data_path(t->path, buf, sizeof(buf)), t->srgb);
    }
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "models")) {
        if (AS->model_count >= MAX_ASSET_MODELS) break;
        const cJSON *n = cJSON_GetObjectItemCaseSensitive(it, "name"), *p = cJSON_GetObjectItemCaseSensitive(it, "path");
        Model *m = &AS->models[AS->model_count];
        if (model_load_gltf(platform_data_path(cJSON_IsString(p) ? p->valuestring : "", buf, sizeof(buf)), m)) {
            dg_strcpy(m->name, sizeof(m->name), cJSON_IsString(n) ? n->valuestring : "");
            // "ao": { "strength", "combine", "vertex" } on every material of the model (render/material_ao.h).
            const cJSON *ao = cJSON_GetObjectItemCaseSensitive(it, "ao");
            for (int k = 0; k < m->mesh_count; k++) model_parse_ao(&m->meshes[k].mat, ao, "strength", "combine", "vertex");
            if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(it, "vgeo"))) model_load_vgeo(m, buf);
            model_parse_foliage(m, it);
        }
        AS->model_count++; // keep indices stable even if a model failed to load
    }
    cJSON_Delete(root);
    return true;
}

void assets_shutdown(void) {
    if (!AS) return;
    for (int i = 1; i < AS->model_count; i++)
        if (AS->models[i].vgeo) { vgeo_free(AS->models[i].vgeo); free(AS->models[i].vgeo); }
    free(AS);
    AS = NULL;
    free(g_tex_src);
    g_tex_src = NULL;
    g_tex_src_count = g_tex_src_cap = 0;
}

void assets_reload_textures(void) {
    if (!render_textures_reload_wanted()) return;
    u64 t0 = SDL_GetTicksNS();
    render_textures_reload_begin();
    cgltf_data *g = NULL;   // the glTF of the previous source (a model's maps are registered together)
    const char *g_path = NULL;
    bool g_buffers = false;
    int n = 0;
    for (int i = 0; i < g_tex_src_count; i++) {
        const TexSource *t = &g_tex_src[i];
        if (!render_texture_reload_needed(t->id)) continue;
        if (t->image < 0) {
            texture_load_path(t->path, t->srgb, true, t->id);
            n++;
            continue;
        }
        cgltf_options opt;
        memset(&opt, 0, sizeof(opt));
        if (!g_path || strcmp(g_path, t->path) != 0) {
            if (g) cgltf_free(g);
            g = NULL;
            g_path = t->path;
            g_buffers = false;
            if (cgltf_parse_file(&opt, t->path, &g) != cgltf_result_success) { log_error("gltf: cannot parse %s (texture reload)", t->path); g = NULL; }
        }
        if (!g || t->image >= (int)g->images_count) continue;
        const cgltf_image *img = &g->images[t->image];
        if (img->buffer_view && !g_buffers) {   // geometry buffers only when an image lives in one
            if (cgltf_load_buffers(&opt, g, t->path) != cgltf_result_success) { log_error("gltf: cannot load buffers for %s (texture reload)", t->path); continue; }
            g_buffers = true;
        }
        char name[560];
        snprintf(name, sizeof(name), "%s image %d", t->path, t->image);
        gltf_image_load(t->path, img, t->srgb, name, t->id);
        n++;
    }
    if (g) cgltf_free(g);
    render_textures_reload_end();
    log_info("texture quality: %d world textures reloaded in %.0f ms", n, (double)(SDL_GetTicksNS() - t0) / 1e6);
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
