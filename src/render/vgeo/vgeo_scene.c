#include "vgeo_scene.h"
#include "vgeo_skin.h"
#include "core/log.h"
#include "core/platform.h"
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define GROW(p, n, cap, add)                                                     \
    do {                                                                         \
        if ((n) + (add) > (cap)) {                                               \
            while ((n) + (add) > (cap)) (cap) = (cap) ? (cap) * 2 : 64;          \
            *(void **)&(p) = realloc((p), sizeof(*(p)) * (size_t)(cap));         \
        }                                                                        \
    } while (0)

void vgeo_scene_init(VgScene *s, const VgContext *ctx) {
    memset(s, 0, sizeof(*s));
    s->ctx = ctx;
    VgMaterial def;
    memset(&def, 0, sizeof(def));
    def.color[0] = def.color[1] = def.color[2] = 0.6f;
    def.color[3] = 1.0f;
    def.roughness = 0.8f;
    def.tex_scale = 1.0f;
    vgeo_scene_add_material(s, &def);
}

u32 vgeo_scene_add_material(VgScene *s, const VgMaterial *m) {
    GROW(s->materials, s->material_count, s->material_cap, 1);
    s->materials[s->material_count] = *m;
    return s->material_count++;
}

void vgeo_scene_set_mesh_materials(VgScene *s, u32 mesh, u32 material_base) {
    if (mesh < s->mesh_count) s->meshes[mesh].material_base = material_base;
}

static void free_gpu(VgScene *s) {
    if (!s->ctx) return;
    vgc_buffer_free(s->ctx, &s->b_meshes);
    vgc_buffer_free(s->ctx, &s->b_nodes);
    vgc_buffer_free(s->ctx, &s->b_groups);
    vgc_buffer_free(s->ctx, &s->b_page_table);
    vgc_buffer_free(s->ctx, &s->b_pool);
    vgc_buffer_free(s->ctx, &s->b_instances);
    vgc_buffer_free(s->ctx, &s->b_cells);
    vgc_buffer_free(s->ctx, &s->b_materials);
    vgc_buffer_free(s->ctx, &s->b_stamps);
}

void vgeo_scene_free(VgScene *s) {
    if (s->ctx && s->ctx->device) vkDeviceWaitIdle(s->ctx->device);
    free_gpu(s);
    free(s->meshes);
    free(s->nodes);
    free(s->groups);
    free(s->page_table);
    free(s->page_desc);
    free(s->page_mesh);
    free(s->page_root);
    free(s->meta_off);
    free(s->meta);
    for (u32 m = 0; m < s->mesh_count; m++) { free(s->sources[m].data); free(s->sources[m].path); }
    free(s->sources);
    free(s->slot_page);
    free(s->mesh_depth);
    free(s->instances);
    free(s->cells);
    free(s->materials);
    memset(s, 0, sizeof(*s));
}

static u32 bvh_depth(const VgNode *nodes, u32 n) {
    const VgNode *nd = &nodes[n];
    if (nd->flags & VGN_LEAF) return 1;
    u32 d = 0;
    for (u32 k = 0; k < nd->count; k++) {
        u32 c = bvh_depth(nodes, nd->first + k);
        if (c > d) d = c;
    }
    return d + 1;
}

static u32 add_mesh(VgScene *s, const VgBuilt *b, const char *path) {
    const VgFileHeader *h = &b->hdr;
    u32 mi = s->mesh_count;
    GROW(s->meshes, s->mesh_count, s->mesh_cap, 1);
    s->mesh_depth = (u32 *)realloc(s->mesh_depth, sizeof(u32) * s->mesh_cap);
    s->sources = (VgPageSource *)realloc(s->sources, sizeof(VgPageSource) * s->mesh_cap);
    VgPageSource *src = &s->sources[mi];
    memset(src, 0, sizeof(*src));
    if (path) {
        size_t n = strlen(path) + 1;
        src->path = (char *)malloc(n);
        memcpy(src->path, path, n);
        src->base = h->sections[VGS_PAGE_DATA].offset;
    } else {
        src->data = (u8 *)malloc(b->page_data_size ? b->page_data_size : 1);
        memcpy(src->data, b->page_data, b->page_data_size);
    }
    VgMesh *m = &s->meshes[s->mesh_count++];
    memset(m, 0, sizeof(*m));
    const VgNode *root = &b->nodes[h->root_node];
    memcpy(m->center, root->center, 12);
    m->radius = root->radius;
    m->root_node = h->root_node;
    m->node_base = s->node_count;
    m->group_base = s->group_count;
    m->page_base = s->page_count;
    m->page_count = h->page_count;
    m->flags = h->flags;
    m->impostor = ~0u;
    m->q_step = h->q_step;
    GROW(s->nodes, s->node_count, s->node_cap, h->node_count);
    memcpy(&s->nodes[s->node_count], b->nodes, sizeof(VgNode) * h->node_count);
    s->node_count += h->node_count;
    GROW(s->groups, s->group_count, s->group_cap, h->group_count);
    memcpy(&s->groups[s->group_count], b->groups, sizeof(VgGroup) * h->group_count);
    s->group_count += h->group_count;
    if (s->page_count + h->page_count > s->page_cap) {
        while (s->page_count + h->page_count > s->page_cap) s->page_cap = s->page_cap ? s->page_cap * 2 : 64;
        s->page_table = (u32 *)realloc(s->page_table, sizeof(u32) * s->page_cap);
        s->page_desc = (VgPageDesc *)realloc(s->page_desc, sizeof(VgPageDesc) * s->page_cap);
        s->page_mesh = (u32 *)realloc(s->page_mesh, sizeof(u32) * s->page_cap);
        s->page_root = (u8 *)realloc(s->page_root, s->page_cap);
        s->meta_off = (u32 *)realloc(s->meta_off, sizeof(u32) * s->page_cap);
    }
    // Page meta (header + cluster records) stays on the CPU for every page.
    for (u32 p = 0; p < h->page_count; p++) {
        u32 g = s->page_count + p;
        const VgPageHeader *ph = vgeo_page(b, p);
        size_t n = sizeof(VgPageHeader) + sizeof(VgCluster) * (size_t)ph->cluster_count;
        if (s->meta_size + n > s->meta_cap) {
            while (s->meta_size + n > s->meta_cap) s->meta_cap = s->meta_cap ? s->meta_cap * 2 : 65536;
            s->meta = (u8 *)realloc(s->meta, s->meta_cap);
        }
        memcpy(s->meta + s->meta_size, ph, n);
        s->meta_off[g] = (u32)s->meta_size;
        s->meta_size += n;
        s->page_desc[g] = b->pages[p];
        s->page_mesh[g] = mi;
        s->page_root[g] = p < h->root_page_count;
        s->page_table[g] = ~0u;
        s->root_count += p < h->root_page_count;
    }
    s->page_count += h->page_count;
    s->mesh_depth[mi] = bvh_depth(b->nodes, h->root_node);
    if (s->mesh_depth[mi] > s->max_depth) s->max_depth = s->mesh_depth[mi];
    s->cluster_count += h->cluster_count;
    for (u32 g = 0; g < h->group_count; g++)
        if (b->groups[g].cluster_count > 16) log_warn("vgeo: mesh %u group %u has %u clusters (> 16 lanes)", mi, g, b->groups[g].cluster_count);
    return mi;
}

u32 vgeo_scene_add_mesh(VgScene *s, const VgBuilt *b) { return add_mesh(s, b, NULL); }
u32 vgeo_scene_add_mesh_file(VgScene *s, const VgBuilt *b, const char *path) { return add_mesh(s, b, path); }

void vgeo_scene_set_pool(VgScene *s, u32 slots) { s->pool_budget = slots; }

bool vgeo_scene_read_page(const VgScene *s, u32 page, void *dst) {
    if (page >= s->page_count) return false;
    const VgPageDesc *d = &s->page_desc[page];
    const VgPageSource *src = &s->sources[s->page_mesh[page]];
    if (src->data) { memcpy(dst, src->data + d->file_offset, d->size); return true; }
    return platform_read_file_range(src->path, src->base + d->file_offset, dst, d->size);
}

// Initial residency: every page, or the root pages with a budget.
static void assign_pool(VgScene *s) {
    u32 slots = s->page_count;
    if (s->pool_budget) slots = DG_MAX(s->root_count, DG_MIN(s->pool_budget, s->page_count));
    if (s->pool_budget && s->pool_budget < s->root_count) log_warn("vgeo: pool of %u slots < %u root pages, grown", s->pool_budget, s->root_count);
    s->pool_slots = slots;
    free(s->slot_page);
    s->slot_page = (u32 *)malloc(sizeof(u32) * (slots + 1));
    memset(s->slot_page, 0xFF, sizeof(u32) * (slots + 1));
    u32 used = 0;
    for (u32 p = 0; p < s->page_count; p++) {
        s->page_table[p] = ~0u;
        if (s->pool_budget && !s->page_root[p]) continue;
        s->page_table[p] = used;
        s->slot_page[used++] = p;
    }
}

static bool upload_pool(VgScene *s) {
    const VgContext *c = s->ctx;
    u32 slots = s->pool_slots, used = 0;
    while (used < slots && s->slot_page[used] != ~0u) used++;
    const VkBufferUsageFlags U = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkDeviceSize n_slots = slots ? slots : 1;
    s->b_pool = vgc_buffer(c, n_slots * VGEO_PAGE_SIZE, U, VGM_DEVICE);
    s->b_stamps = vgc_buffer(c, sizeof(u32) * n_slots, U, VGM_DEVICE);
    if (!s->b_pool.buffer || !s->b_stamps.buffer) return false;
    // Resident pages in chunks of 256 slots (32 MB).
    const u32 chunk = 256;
    u8 *tmp = (u8 *)malloc((size_t)DG_MIN(chunk, DG_MAX(used, 1u)) * VGEO_PAGE_SIZE);
    bool ok = true;
    for (u32 first = 0; first < used; first += chunk) {
        u32 n = DG_MIN(chunk, used - first);
        memset(tmp, 0, (size_t)n * VGEO_PAGE_SIZE);
        for (u32 k = 0; k < n; k++) ok &= vgeo_scene_read_page(s, s->slot_page[first + k], tmp + (size_t)k * VGEO_PAGE_SIZE);
        vgc_upload(c, &s->b_pool, (VkDeviceSize)first * VGEO_PAGE_SIZE, tmp, (VkDeviceSize)n * VGEO_PAGE_SIZE);
    }
    free(tmp);
    u32 *zero = (u32 *)calloc((size_t)n_slots, sizeof(u32));
    vgc_upload(c, &s->b_stamps, 0, zero, sizeof(u32) * n_slots);
    free(zero);
    if (!ok) log_error("vgeo: cannot read the resident pages");
    return ok;
}

void vgeo_scene_add_instance(VgScene *s, u32 mesh, const float pos[3], const float rot[4], float scale) {
    GROW(s->instances, s->instance_count, s->instance_cap, 1);
    VgInstance *in = &s->instances[s->instance_count++];
    memset(in, 0, sizeof(*in));
    memcpy(in->pos, pos, 12);
    in->rot = vg_quat_encode(rot);
    in->scale_h = vgeo_half(scale);
    in->mesh = (u16)mesh;
}

void vgeo_scene_add_skinned_instance(VgScene *s, u32 mesh, const float pos[3], const float rot[4], float scale, u32 slot) {
    vgeo_scene_add_instance(s, mesh, pos, rot, scale);
    VgInstance *in = &s->instances[s->instance_count - 1];
    in->flags_phase |= (u32)VGI_SKINNED << 16;
    in->extra = slot;
}

void vgeo_scene_clear_instances(VgScene *s) { s->instance_count = 0; }

void vgeo_instance_decode(const VgInstance *in, float pos[3], float rot[4], float *scale) {
    memcpy(pos, in->pos, 12);
    vg_quat_decode(in->rot, rot);
    *scale = vgeo_half_to_float(in->scale_h);
}

static void qrot(const float q[4], const float v[3], float o[3]) {
    float t[3] = { 2.0f * (q[1] * v[2] - q[2] * v[1]), 2.0f * (q[2] * v[0] - q[0] * v[2]), 2.0f * (q[0] * v[1] - q[1] * v[0]) };
    o[0] = v[0] + q[3] * t[0] + (q[1] * t[2] - q[2] * t[1]);
    o[1] = v[1] + q[3] * t[1] + (q[2] * t[0] - q[0] * t[2]);
    o[2] = v[2] + q[3] * t[2] + (q[0] * t[1] - q[1] * t[0]);
}

static void world_sphere(const VgInstance *in, const float c[3], float r, float out[4]) {
    float p[3], q[4], s;
    vgeo_instance_decode(in, p, q, &s);
    float v[3] = { c[0] * s, c[1] * s, c[2] * s }, w[3];
    qrot(q, v, w);
    out[0] = p[0] + w[0];
    out[1] = p[1] + w[1];
    out[2] = p[2] + w[2];
    out[3] = r * s;
}

typedef struct CellKey { i32 x, z; u32 index; } CellKey;
static int cell_cmp(const void *a, const void *b) {
    const CellKey *x = (const CellKey *)a, *y = (const CellKey *)b;
    if (x->z != y->z) return x->z < y->z ? -1 : 1;
    if (x->x != y->x) return x->x < y->x ? -1 : 1;
    return x->index < y->index ? -1 : (x->index > y->index);   // stable
}

bool vgeo_scene_commit(VgScene *s) {
    const VgContext *c = s->ctx;
    // Bin instances into 8 m cells (x, z), stable within a cell.
    CellKey *k = (CellKey *)malloc(sizeof(CellKey) * (s->instance_count + 1));
    for (u32 i = 0; i < s->instance_count; i++) {
        k[i].x = (i32)floorf(s->instances[i].pos[0] / VGEO_CELL_SIZE);
        k[i].z = (i32)floorf(s->instances[i].pos[2] / VGEO_CELL_SIZE);
        if ((s->instances[i].flags_phase >> 16) & VGI_SKINNED) k[i].x = k[i].z = INT32_MAX;   // moving: cells of their own, last
        k[i].index = i;
    }
    qsort(k, s->instance_count, sizeof(CellKey), cell_cmp);
    VgInstance *sorted = (VgInstance *)malloc(sizeof(VgInstance) * (s->instance_count + 1));
    free(s->cells);
    s->cells = (VgCell *)malloc(sizeof(VgCell) * (s->instance_count + 1));
    s->cell_count = 0;
    s->tri_count = 0;
    for (u32 i = 0; i < s->instance_count; i++) {
        sorted[i] = s->instances[k[i].index];
        bool skinned = k[i].x == INT32_MAX && k[i].z == INT32_MAX;
        if (i == 0 || k[i].x != k[i - 1].x || k[i].z != k[i - 1].z || (skinned && s->cells[s->cell_count - 1].count == 32)) {
            VgCell *cl = &s->cells[s->cell_count++];
            for (int a = 0; a < 3; a++) { cl->min[a] = FLT_MAX; cl->max[a] = -FLT_MAX; }
            cl->first = i;
            cl->count = 0;
        }
        VgCell *cl = &s->cells[s->cell_count - 1];
        sorted[i].cell = s->cell_count - 1;
        const VgMesh *m = &s->meshes[sorted[i].mesh];
        float w[4];
        world_sphere(&sorted[i], m->center, m->radius, w);
        if (skinned) w[0] = w[1] = w[2] = 0, w[3] = 1e18f;   // unbounded: the instance test uses the slot's bounds
        for (int a = 0; a < 3; a++) {
            cl->min[a] = fminf(cl->min[a], w[a] - w[3]);
            cl->max[a] = fmaxf(cl->max[a], w[a] + w[3]);
        }
        cl->count++;
    }
    free(k);
    memcpy(s->instances, sorted, sizeof(VgInstance) * s->instance_count);
    free(sorted);
    assign_pool(s);
    if (!c || !c->device) return true;
    vkDeviceWaitIdle(c->device);
    free_gpu(s);
    if (!upload_pool(s)) return false;
    const VkBufferUsageFlags U = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    struct { VgBuffer *b; const void *p; VkDeviceSize n; } t[] = {
        { &s->b_meshes, s->meshes, sizeof(VgMesh) * (VkDeviceSize)s->mesh_count },
        { &s->b_nodes, s->nodes, sizeof(VgNode) * (VkDeviceSize)s->node_count },
        { &s->b_groups, s->groups, sizeof(VgGroup) * (VkDeviceSize)s->group_count },
        { &s->b_page_table, s->page_table, sizeof(u32) * (VkDeviceSize)s->page_count },
        { &s->b_instances, s->instances, sizeof(VgInstance) * (VkDeviceSize)s->instance_count },
        { &s->b_cells, s->cells, sizeof(VgCell) * (VkDeviceSize)s->cell_count },
        { &s->b_materials, s->materials, sizeof(VgMaterial) * (VkDeviceSize)s->material_count },
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        *t[i].b = vgc_buffer(c, t[i].n ? t[i].n : 16, U, VGM_DEVICE);
        if (!t[i].b->buffer) return false;
        if (t[i].n) vgc_upload(c, t[i].b, 0, t[i].p, t[i].n);
    }
    s->generation++;
    return true;
}

// ------------------------------------------------------------------ CPU reference

const VgPageHeader *vgeo_scene_page_header(const VgScene *s, u32 page) { return (const VgPageHeader *)(s->meta + s->meta_off[page]); }

const VgCluster *vgeo_scene_page_cluster(const VgScene *s, u32 page, u32 index) {
    return (const VgCluster *)(s->meta + s->meta_off[page] + sizeof(VgPageHeader) + (size_t)index * sizeof(VgCluster));
}

const VgCluster *vgeo_scene_cluster(const VgScene *s, u32 slot, u32 index) {
    static const VgCluster none;
    u32 page = slot < s->pool_slots ? s->slot_page[slot] : ~0u;
    if (page == ~0u || index >= vgeo_scene_page_header(s, page)->cluster_count) return &none;
    return vgeo_scene_page_cluster(s, page, index);
}

// Same formula as vg_project_error in shaders/vgeo/vgeo_common.glsl.
static float project_error(const VgViewGpu *v, const float s[4], float err) {
    if (v->flags & VGV_ORTHO) return err * v->proj_scale;
    float dx = s[0] - v->cam_pos[0], dy = s[1] - v->cam_pos[1], dz = s[2] - v->cam_pos[2];
    float d = sqrtf(dx * dx + dy * dy + dz * dz) - s[3];
    if (d < v->znear) d = v->znear;
    return err * v->proj_scale / d;
}

static float margin(float px, float t) {
    if (!isfinite(px)) return FLT_MAX;
    return fabsf(px - t) / t;
}

u32 vgeo_scene_cpu_select(const VgScene *s, const VgViewGpu *v, VgRefCluster *out, u32 cap) { return vgeo_scene_cpu_select_ex(s, v, false, out, cap); }

u32 vgeo_scene_cpu_select_ex(const VgScene *s, const VgViewGpu *v, bool all_resident, VgRefCluster *out, u32 cap) {
    u32 n = 0;
    float t = v->error_px;
    for (u32 i = 0; i < s->instance_count; i++) {
        const VgInstance *in = &s->instances[i];
        const VgMesh *m = &s->meshes[in->mesh];
        VgXfCpu x;   // the shaders' decode (skinned: slot sphere transform and error scale)
        vgeo_xf_decode(&x, in, m, s->palettes);
        if (!x.active) continue;
        float sc = x.scale;
        u32 ng = (u32)((in->mesh + 1 < s->mesh_count ? s->meshes[in->mesh + 1].group_base : s->group_count) - m->group_base);
        for (u32 gi = 0; gi < ng; gi++) {
            const VgGroup *g = &s->groups[m->group_base + gi];
            float gs[4];
            vgeo_xf_sphere(&x, g->lod_center, g->lod_radius, gs);
            float gpx = project_error(v, gs, g->parent_error * sc);
            if (!(gpx > t)) continue;
            u32 page = m->page_base + g->page, slot = s->page_table[page];
            if (slot == ~0u && !all_resident) continue;
            for (u32 k = 0; k < g->cluster_count; k++) {
                const VgCluster *cl = vgeo_scene_page_cluster(s, page, g->first_cluster + k);
                float cs[4];
                vgeo_xf_sphere(&x, cl->lod_center, cl->lod_radius, cs);
                float cpx = project_error(v, cs, cl->error * sc);
                bool child_missing = false;
                if (cl->child_group != ~0u && !all_resident) child_missing = s->page_table[m->page_base + s->groups[m->group_base + cl->child_group].page] == ~0u;
                if (!(cpx <= t || child_missing)) continue;
                if (out && n < cap) {
                    VgRefCluster *r = &out[n];
                    r->instance = i;
                    r->packed = slot << VG_CLUSTER_BITS | (g->first_cluster + k);
                    r->page = page;
                    r->lod_margin = fminf(margin(gpx, t), child_missing ? FLT_MAX : margin(cpx, t));
                    float b[4];
                    if (x.skinned) {   // animated bone-box sphere (vg_skin_bounds)
                        VgBoneBox bx[VGEO_MAX_CLUSTER_BONES];
                        static u8 pbuf[VGEO_PAGE_SIZE];   // the CPU reference reads the page from its source
                        static u32 pbuf_page = ~0u;
                        static const VgScene *pbuf_scene;
                        if (pbuf_page != page || pbuf_scene != s) {
                            vgeo_scene_read_page(s, page, pbuf);
                            pbuf_page = page;
                            pbuf_scene = s;
                        }
                        u32 nb = vgeo_cluster_bones(pbuf, cl, bx);
                        float is[4];
                        vgeo_skin_cluster_sphere(&s->palettes->cpu_bones[(size_t)x.first * 16], bx, nb, is);
                        vgeo_xf_instance_sphere(&x, is, is[3], b);
                    } else vgeo_xf_sphere(&x, cl->center, cl->radius, b);
                    r->frustum = 1;
                    for (int p = 0; p < 6; p++) {
                        float d = v->planes[p][0] * b[0] + v->planes[p][1] * b[1] + v->planes[p][2] * b[2] + v->planes[p][3];
                        float band = 1e-3f * (1.0f + b[3]);
                        if (d < -b[3] - band) { r->frustum = 0; break; }
                        if (d < -b[3] + band) r->frustum = -1;
                    }
                }
                n++;
            }
        }
    }
    return n;
}

void vgeo_view_setup(VgViewGpu *v, const float vp[16], const float prev_vp[16], const float cam_pos[3], float proj_scale, float znear, u32 w, u32 h,
                     float error_px, float sw_px, u32 flags) {
    memset(v, 0, sizeof(*v));
    memcpy(v->view_proj, vp, 64);
    memcpy(v->prev_view_proj, prev_vp ? prev_vp : vp, 64);
    // Gribb-Hartmann on a column-major matrix, Vulkan clip z in [0, w].
    float row[4][4];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) row[r][c] = vp[c * 4 + r];
    for (int c = 0; c < 4; c++) {
        v->planes[0][c] = row[3][c] + row[0][c];
        v->planes[1][c] = row[3][c] - row[0][c];
        v->planes[2][c] = row[3][c] + row[1][c];
        v->planes[3][c] = row[3][c] - row[1][c];
        v->planes[4][c] = row[2][c];
        v->planes[5][c] = row[3][c] - row[2][c];
    }
    for (int p = 0; p < 6; p++) {
        float l = sqrtf(v->planes[p][0] * v->planes[p][0] + v->planes[p][1] * v->planes[p][1] + v->planes[p][2] * v->planes[p][2]);
        if (l > 0)
            for (int c = 0; c < 4; c++) v->planes[p][c] /= l;
    }
    memcpy(v->cam_pos, cam_pos, 12);
    v->proj_scale = proj_scale;
    v->znear = znear;
    v->error_px = error_px;
    v->sw_px = sw_px;
    v->flags = flags;
    v->viewport[0] = (float)w;
    v->viewport[1] = (float)h;
    v->viewport[2] = 1.0f / (float)w;
    v->viewport[3] = 1.0f / (float)h;
    memcpy(v->raster_view_proj, vp, 64);
}
