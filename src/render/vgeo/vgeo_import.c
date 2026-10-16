#include "vgeo_import.h"
#include "core/log.h"
#include "core/platform.h"
#include "cgltf.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void read_vec(const cgltf_accessor *a, u32 i, float *out, int n) {
    cgltf_accessor_read_float(a, i, out, (cgltf_size)n);
}

bool vgeo_import_gltf(const char *path, VgImported *out, char *err, int err_size) {
    memset(out, 0, sizeof(*out));
    cgltf_options o;
    memset(&o, 0, sizeof(o));
    cgltf_data *d = NULL;
    if (cgltf_parse_file(&o, path, &d) != cgltf_result_success) { snprintf(err, (size_t)err_size, "%s: cannot parse", path); return false; }
    if (cgltf_load_buffers(&o, d, path) != cgltf_result_success) { snprintf(err, (size_t)err_size, "%s: cannot load buffers", path); cgltf_free(d); return false; }
    out->material_count = (u32)(d->materials_count ? d->materials_count : 1);
    out->material_names = calloc(out->material_count, 48);
    for (u32 m = 0; m < (u32)d->materials_count; m++)
        if (d->materials[m].name) dg_strcpy(out->material_names[m], 48, d->materials[m].name);
    u32 vcap = 0, icap = 0;
    for (cgltf_size ni = 0; ni < d->nodes_count; ni++) {
        cgltf_node *node = &d->nodes[ni];
        if (!node->mesh) continue;
        float world[16];
        cgltf_node_transform_world(node, world);
        for (cgltf_size pi = 0; pi < node->mesh->primitives_count; pi++) {
            cgltf_primitive *prim = &node->mesh->primitives[pi];
            if (prim->type != cgltf_primitive_type_triangles) continue;
            const cgltf_accessor *ap = NULL, *an = NULL, *at = NULL;
            for (cgltf_size k = 0; k < prim->attributes_count; k++) {
                if (prim->attributes[k].type == cgltf_attribute_type_position) ap = prim->attributes[k].data;
                else if (prim->attributes[k].type == cgltf_attribute_type_normal) an = prim->attributes[k].data;
                else if (prim->attributes[k].type == cgltf_attribute_type_texcoord && prim->attributes[k].index == 0) at = prim->attributes[k].data;
            }
            if (!ap) continue;
            u32 vbase = out->vertex_count, vn = (u32)ap->count;
            u32 in = prim->indices ? (u32)prim->indices->count : vn;
            if (out->vertex_count + vn > vcap) {
                while (out->vertex_count + vn > vcap) vcap = vcap ? vcap * 2 : 4096;
                out->pos = realloc(out->pos, sizeof(float) * 3 * vcap);
                out->normal = realloc(out->normal, sizeof(float) * 3 * vcap);
                out->uv = realloc(out->uv, sizeof(float) * 2 * vcap);
            }
            if (out->index_count + in > icap) {
                while (out->index_count + in > icap) icap = icap ? icap * 2 : 12288;
                out->indices = realloc(out->indices, sizeof(u32) * icap);
                out->tri_material = realloc(out->tri_material, sizeof(u16) * (icap / 3 + 1));
            }
            for (u32 v = 0; v < vn; v++) {
                float p[3] = { 0 }, n[3] = { 0, 1, 0 }, t[2] = { 0 };
                read_vec(ap, v, p, 3);
                if (an) read_vec(an, v, n, 3);
                if (at) read_vec(at, v, t, 2);
                float *wp = &out->pos[(vbase + v) * 3], *wn = &out->normal[(vbase + v) * 3];
                for (int r = 0; r < 3; r++) {
                    wp[r] = world[0 * 4 + r] * p[0] + world[1 * 4 + r] * p[1] + world[2 * 4 + r] * p[2] + world[12 + r];
                    wn[r] = world[0 * 4 + r] * n[0] + world[1 * 4 + r] * n[1] + world[2 * 4 + r] * n[2];
                }
                memcpy(&out->uv[(vbase + v) * 2], t, 8);
            }
            u32 mat = prim->material ? (u32)(prim->material - d->materials) : 0;
            for (u32 i = 0; i + 2 < in; i += 3) {
                for (int k = 0; k < 3; k++) out->indices[out->index_count + k] = vbase + (prim->indices ? (u32)cgltf_accessor_read_index(prim->indices, i + (u32)k) : i + (u32)k);
                out->tri_material[out->index_count / 3] = (u16)mat;
                out->index_count += 3;
            }
            out->vertex_count += vn;
        }
    }
    cgltf_free(d);
    if (!out->index_count) { snprintf(err, (size_t)err_size, "%s: no triangles", path); vgeo_imported_free(out); return false; }
    return true;
}

void vgeo_imported_free(VgImported *m) {
    free(m->pos);
    free(m->normal);
    free(m->uv);
    free(m->indices);
    free(m->tri_material);
    free(m->material_names);
    memset(m, 0, sizeof(*m));
}

void vgeo_imported_source(const VgImported *m, VgSource *src) {
    memset(src, 0, sizeof(*src));
    src->vertex_count = m->vertex_count;
    src->pos = m->pos;
    src->normal = m->normal;
    src->uv = m->uv;
    src->index_count = m->index_count;
    src->indices = m->indices;
    src->tri_material = m->tri_material;
    src->material_count = m->material_count;
    src->material_names = (const char (*)[48])m->material_names;
}

u32 vgeo_cache_key(const void *file, size_t size, const VgBuildOptions *opt) {
    u32 h = vgeo_fnv1a(file, size, 0);
    u32 v = VGEO_BUILD_VERSION;
    h = vgeo_fnv1a(&v, sizeof(v), h);
    // Options that change the output (threads never do).
    i32 lc = opt ? opt->lod_count : 0;
    h = vgeo_fnv1a(&lc, sizeof(lc), h);
    if (opt) h = vgeo_fnv1a(opt->lod_error_rel, sizeof(opt->lod_error_rel), h);
    return h ? h : 1;
}

static void file_stem(const char *path, char *out, size_t n) {
    const char *s = strrchr(path, '/'), *b = strrchr(path, '\\');
    if (b > s) s = b;
    s = s ? s + 1 : path;
    dg_strcpy(out, n, s);
    char *dot = strrchr(out, '.');
    if (dot) *dot = 0;
    for (char *c = out; *c; c++) if (*c == ' ') *c = '_';
}

bool vgeo_load_or_build(const char *gltf_path, const char *cache_dir, const VgBuildOptions *opt, VgBuilt *out, bool *from_cache, char *err, int err_size) {
    if (from_cache) *from_cache = false;
    size_t fsize = 0;
    char *file = platform_read_file(gltf_path, &fsize);
    if (!file) { snprintf(err, (size_t)err_size, "%s: cannot read", gltf_path); return false; }
    u32 key = vgeo_cache_key(file, fsize, opt);
    free(file);
    char stem[96], cpath[700] = "";
    file_stem(gltf_path, stem, sizeof(stem));
    if (cache_dir && cache_dir[0]) {
        snprintf(cpath, sizeof(cpath), "%s%s.vgeo.%08x.dgc", cache_dir, stem, key);
        size_t cs = 0;
        u8 *cd = (u8 *)platform_read_file(cpath, &cs);
        if (cd) {
            bool ok = vgeo_deserialize(cd, cs, out) && out->hdr.key == key;
            free(cd);
            if (ok) {
                if (from_cache) *from_cache = true;
                return true;
            }
            vgeo_free(out);
        }
    }
    VgImported im;
    if (!vgeo_import_gltf(gltf_path, &im, err, err_size)) return false;
    VgSource src;
    vgeo_imported_source(&im, &src);
    bool ok = vgeo_build(&src, opt, out, err, err_size);
    vgeo_imported_free(&im);
    if (!ok) return false;
    out->hdr.key = key;
    if (cpath[0]) {
        SDL_CreateDirectory(cache_dir);
        u8 *data = NULL;
        size_t size = 0;
        if (vgeo_serialize(out, key, &data, &size)) {
            if (!platform_write_file(cpath, data, size)) log_warn("vgeo: cannot write cache %s", cpath);
            free(data);
        }
    }
    return true;
}

// ------------------------------------------------------------------ shared file cache, placements

static struct { char path[512]; VgBuilt *b; } g_files[32];
static int g_file_count;

const VgBuilt *vgeo_file_get(const char *path) {
    for (int i = 0; i < g_file_count; i++)
        if (!strcmp(g_files[i].path, path)) return g_files[i].b;
    if (g_file_count >= 32) return NULL;
    size_t size = 0;
    u8 *data = (u8 *)platform_read_file(path, &size);
    VgBuilt *b = NULL;
    if (data) {
        b = (VgBuilt *)calloc(1, sizeof(VgBuilt));
        if (!vgeo_deserialize(data, size, b)) { log_error("vgeo: %s is not a valid .vgeo file (version %d)", path, VGEO_FILE_VERSION); free(b); b = NULL; }
        free(data);
    }
    dg_strcpy(g_files[g_file_count].path, sizeof(g_files[0].path), path);
    g_files[g_file_count++].b = b;   // also caches a miss
    return b;
}

void vgeo_file_release_all(void) {
    for (int i = 0; i < g_file_count; i++)
        if (g_files[i].b) { vgeo_free(g_files[i].b); free(g_files[i].b); }
    g_file_count = 0;
}

u32 vgeo_placement_expand(const float pos[3], float yaw, float scale, float jitter, float spacing, u32 nx, u32 nz, u32 seed, VgPlacementXf *out, u32 cap) {
    nx = nx ? nx : 1;
    nz = nz ? nz : 1;
    u32 h = seed * 2654435761u + 1u, n = 0;
    bool grid = nx * nz > 1;
    for (u32 z = 0; z < nz; z++)
        for (u32 x = 0; x < nx; x++) {
            h ^= h << 13;
            h ^= h >> 17;
            h ^= h << 5;
            if (n >= cap) continue;
            float r0 = (float)(h & 0xFFFF) / 65535.0f, r1 = (float)(h >> 16) / 65535.0f;
            VgPlacementXf *o = &out[n++];
            o->pos[0] = pos[0] + ((float)x - (float)(nx - 1) * 0.5f) * spacing;
            o->pos[1] = pos[1];
            o->pos[2] = pos[2] + ((float)z - (float)(nz - 1) * 0.5f) * spacing;
            float a = (yaw + (grid ? r0 * 6.2831853f : 0.0f)) * 0.5f;
            o->rot[0] = 0.0f;
            o->rot[1] = sinf(a);
            o->rot[2] = 0.0f;
            o->rot[3] = cosf(a);
            o->scale = scale * (1.0f + jitter * (r1 * 2.0f - 1.0f));
        }
    return n;
}
