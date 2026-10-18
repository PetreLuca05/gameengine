#include "vgeo_scene.h"
#include "vgeo_skin.h"
#include "foliage_support.h"
#include "core/log.h"
#include "core/platform.h"
#include <float.h>
#include <math.h>
#include <stdio.h>
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
    vg_material_ao_check(&s->materials[s->material_count]);
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

static void free_placements(VgScene *s) {
    VgPlacements *p = &s->pl;
    for (int f = 0; f < 3; f++) if (s->ctx) vgc_buffer_free(s->ctx, &p->staging[f]);
    free(p->inst);
    free(p->cells);
    free(p->cell_chunk);
    free(p->free_inst);
    free(p->free_cells);
    free(p->chunks);
    free(p->dirty_inst);
    free(p->dirty_cells);
    memset(p, 0, sizeof(*p));
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
    free(s->mesh_mat_count);
    free_placements(s);
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
    s->mesh_mat_count = (u32 *)realloc(s->mesh_mat_count, sizeof(u32) * s->mesh_cap);
    s->mesh_mat_count[mi] = DG_MAX(h->material_count, 1u);
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

// World sphere of a mesh-space sphere of a static instance (+ the mesh's max WPO, as vg_world_sphere).
static void world_sphere(const VgInstance *in, const VgMesh *m, const float c[3], float r, float out[4]) {
    float p[3], q[4], s;
    vgeo_instance_decode(in, p, q, &s);
    float v[3] = { c[0] * s, c[1] * s, c[2] * s }, w[3];
    qrot(q, v, w);
    out[0] = p[0] + w[0];
    out[1] = p[1] + w[1];
    out[2] = p[2] + w[2];
    out[3] = r * s + vg_wpo_margin(m, s);
}

// Mesh foliage fields from its materials (declared WPO bounds, fades, shadow distance, flags).
static void mesh_foliage(VgScene *s) {
    s->has_two_sided = s->has_masked = s->has_impostors = false;
    s->fol_fade_max = s->fol_shadow_max = 0;
    bool fade_inf = false, shadow_inf = false;
    for (u32 i = 0; i < s->material_count; i++) {
        VgMaterial *mt = &s->materials[i];
        mt->wpo_max = mt->wpo != WPO_NONE ? vgeo_wpo_max(mt) : 0.0f;
    }
    for (u32 mi = 0; mi < s->mesh_count; mi++) {
        VgMesh *m = &s->meshes[mi];
        m->max_wpo = m->fade_start = m->fade_end = m->shadow_max_dist = 0;
        m->fflags = 0;
        bool unlimited_shadow = false;
        for (u32 k = 0; k < s->mesh_mat_count[mi]; k++) {
            u32 idx = m->material_base + k;
            if (idx >= s->material_count) break;
            const VgMaterial *mt = &s->materials[idx];
            if (mt->flags & VGM_FOLIAGE) m->fflags |= VGMF_FOLIAGE;
            if (mt->flags & VGM_MASKED) m->fflags |= VGMF_MASKED;
            if (mt->flags & VGM_TWO_SIDED) m->fflags |= VGMF_TWO_SIDED;
            if (mt->flags & VGM_DITHER_FADE) m->fflags |= VGMF_DITHER;
            if (mt->wpo != WPO_NONE) { m->fflags |= VGMF_WPO; m->max_wpo = fmaxf(m->max_wpo, mt->wpo_max); }
            if (mt->fade_end > m->fade_end) { m->fade_end = mt->fade_end; m->fade_start = mt->fade_start; }
            if (mt->shadow_max_dist <= 0) unlimited_shadow = true;
            else m->shadow_max_dist = fmaxf(m->shadow_max_dist, mt->shadow_max_dist);
        }
        if (unlimited_shadow) m->shadow_max_dist = 0;
        if (m->fflags & VGMF_TWO_SIDED) s->has_two_sided = true;
        if (m->fflags & (VGMF_MASKED | VGMF_DITHER)) s->has_masked = true;
        if (m->impostor != ~0u) s->has_impostors = true;
        if (m->fflags & VGMF_FOLIAGE) {
            if (m->fade_end > m->fade_start) s->fol_fade_max = fmaxf(s->fol_fade_max, m->fade_end);
            else fade_inf = true;
            if (m->shadow_max_dist > 0) s->fol_shadow_max = fmaxf(s->fol_shadow_max, m->shadow_max_dist);
            else shadow_inf = true;
        }
    }
    if (fade_inf) s->fol_fade_max = 0;
    if (shadow_inf) s->fol_shadow_max = 0;
}

// ------------------------------------------------------------------ placement chunks

static const VgCell k_empty_cell = { { 1e30f, 1e30f, 1e30f }, 0, { 1e30f, 1e30f, 1e30f }, 0 };

void vgeo_scene_reserve_placements(VgScene *s, u32 instances, u32 cells) {
    VgPlacements *p = &s->pl;
    if (instances <= p->inst_cap && cells <= p->cell_cap) return;
    instances = DG_MAX(instances, p->inst_cap);
    cells = DG_MAX(cells, p->cell_cap);
    p->inst = (VgInstance *)realloc(p->inst, sizeof(VgInstance) * instances);
    memset(p->inst + p->inst_cap, 0, sizeof(VgInstance) * (instances - p->inst_cap));
    p->cells = (VgCell *)realloc(p->cells, sizeof(VgCell) * cells);
    p->cell_chunk = (u32 *)realloc(p->cell_chunk, sizeof(u32) * cells);
    p->free_cells = (u32 *)realloc(p->free_cells, sizeof(u32) * cells);
    // new cells free (pushed so the lowest index is taken first)
    u32 add = cells - p->cell_cap;
    memmove(p->free_cells + add, p->free_cells, sizeof(u32) * p->free_cells_n);
    for (u32 i = 0; i < add; i++) {
        u32 c = cells - 1 - i;
        p->cells[c] = k_empty_cell;
        p->cell_chunk[c] = ~0u;
        p->free_cells[i] = c;
    }
    p->free_cells_n += add;
    // new instances: one free range (merged with a free tail)
    if (instances > p->inst_cap) {
        if (p->free_inst_n && p->free_inst[p->free_inst_n - 1].first + p->free_inst[p->free_inst_n - 1].count == p->inst_cap)
            p->free_inst[p->free_inst_n - 1].count += instances - p->inst_cap;
        else {
            GROW(p->free_inst, p->free_inst_n, p->free_inst_cap, 1);
            p->free_inst[p->free_inst_n++] = (VgRange){ p->inst_cap, instances - p->inst_cap };
        }
    }
    p->inst_cap = instances;
    p->cell_cap = cells;
}

// The ranges vgeo_scene_placement_record uploads (a CPU-only scene, without a context, has no GPU copy).
static void dirty_range(VgScene *s, u32 first, u32 count) {
    VgPlacements *p = &s->pl;
    if (!count || !s->ctx) return;
    GROW(p->dirty_inst, p->dirty_inst_n, p->dirty_inst_cap, 1);
    p->dirty_inst[p->dirty_inst_n++] = (VgRange){ first, count };
}
static void dirty_cell(VgScene *s, u32 c) {
    VgPlacements *p = &s->pl;
    if (!s->ctx) return;
    GROW(p->dirty_cells, p->dirty_cells_n, p->dirty_cells_cap, 1);
    p->dirty_cells[p->dirty_cells_n++] = c;
}

// Cell bounds = union of its instances' mesh spheres (+ max WPO) and positions (the fade / shadow
// distances are measured from them), first relative to the region.
static void cell_bounds(const VgScene *s, VgCell *c) {
    for (int a = 0; a < 3; a++) { c->min[a] = FLT_MAX; c->max[a] = -FLT_MAX; }
    for (u32 i = c->first; i < c->first + c->count; i++) {
        const VgInstance *in = &s->pl.inst[i];
        const VgMesh *m = &s->meshes[in->mesh < s->mesh_count ? in->mesh : 0];
        float w[4];
        world_sphere(in, m, m->center, m->radius, w);
        for (int a = 0; a < 3; a++) {
            c->min[a] = fminf(c->min[a], fminf(w[a] - w[3], in->pos[a]));
            c->max[a] = fmaxf(c->max[a], fmaxf(w[a] + w[3], in->pos[a]));
        }
    }
    if (!c->count) *c = k_empty_cell;
}

u32 vgeo_scene_placement_add(VgScene *s, const VgCell *cells, u32 ncells, const VgInstance *inst, u32 n, u32 user) {
    VgPlacements *p = &s->pl;
    if (ncells > 16 || ncells > p->free_cells_n || !n) return ~0u;
    u32 fr = ~0u;   // first fit
    for (u32 i = 0; i < p->free_inst_n; i++)
        if (p->free_inst[i].count >= n) { fr = i; break; }
    if (fr == ~0u) return ~0u;
    u32 first = p->free_inst[fr].first;
    p->free_inst[fr].first += n;
    p->free_inst[fr].count -= n;
    if (!p->free_inst[fr].count) {
        memmove(&p->free_inst[fr], &p->free_inst[fr + 1], sizeof(VgRange) * (p->free_inst_n - fr - 1));
        p->free_inst_n--;
    }
    u32 h = ~0u;
    for (u32 i = 0; i < p->chunk_count; i++)
        if (!p->chunks[i].used) { h = i; break; }
    if (h == ~0u) {
        GROW(p->chunks, p->chunk_count, p->chunk_cap, 1);
        h = p->chunk_count++;
    }
    VgPlacementChunk *ch = &p->chunks[h];
    memset(ch, 0, sizeof(*ch));
    ch->used = true;
    ch->first = first;
    ch->count = n;
    ch->user = user;
    memcpy(&p->inst[first], inst, sizeof(VgInstance) * n);
    for (u32 k = 0; k < ncells; k++) {
        u32 c = p->free_cells[--p->free_cells_n];
        ch->cells[ch->cell_count++] = c;
        p->cell_chunk[c] = h;
        VgCell *cl = &p->cells[c];
        cl->first = first + DG_MIN(cells[k].first, n);
        cl->count = DG_MIN(cells[k].count, n - DG_MIN(cells[k].first, n));
        for (u32 i = cl->first; i < cl->first + cl->count; i++) p->inst[i].cell = c;
        cell_bounds(s, cl);
        dirty_cell(s, c);
    }
    dirty_range(s, first, n);
    p->live_chunks++;
    p->live_instances += n;
    return h;
}

void vgeo_scene_placement_remove(VgScene *s, u32 h) {
    VgPlacements *p = &s->pl;
    if (h >= p->chunk_count || !p->chunks[h].used) return;
    VgPlacementChunk *ch = &p->chunks[h];
    for (u32 k = 0; k < ch->cell_count; k++) {   // cells first: no frame recorded after this sees the instances
        u32 c = ch->cells[k];
        p->cells[c] = k_empty_cell;
        p->cell_chunk[c] = ~0u;
        p->free_cells[p->free_cells_n++] = c;
        dirty_cell(s, c);
    }
    // instance range back into the sorted free list, merged with its neighbours
    u32 at = 0;
    while (at < p->free_inst_n && p->free_inst[at].first < ch->first) at++;
    GROW(p->free_inst, p->free_inst_n, p->free_inst_cap, 1);
    memmove(&p->free_inst[at + 1], &p->free_inst[at], sizeof(VgRange) * (p->free_inst_n - at));
    p->free_inst[at] = (VgRange){ ch->first, ch->count };
    p->free_inst_n++;
    if (at + 1 < p->free_inst_n && p->free_inst[at].first + p->free_inst[at].count == p->free_inst[at + 1].first) {
        p->free_inst[at].count += p->free_inst[at + 1].count;
        memmove(&p->free_inst[at + 1], &p->free_inst[at + 2], sizeof(VgRange) * (p->free_inst_n - at - 2));
        p->free_inst_n--;
    }
    if (at > 0 && p->free_inst[at - 1].first + p->free_inst[at - 1].count == p->free_inst[at].first) {
        p->free_inst[at - 1].count += p->free_inst[at].count;
        memmove(&p->free_inst[at], &p->free_inst[at + 1], sizeof(VgRange) * (p->free_inst_n - at - 1));
        p->free_inst_n--;
    }
    p->live_chunks--;
    p->live_instances -= ch->count;
    memset(ch, 0, sizeof(*ch));
}

const VgInstance *vgeo_scene_instance(const VgScene *s, u32 i) {
    if (i < s->instance_count) return &s->instances[i];
    i -= s->instance_count;
    return i < s->pl.gpu_inst_cap && i < s->pl.inst_cap ? &s->pl.inst[i] : NULL;
}

const VgCell *vgeo_scene_gpu_cell(const VgScene *s, u32 i, VgCell *tmp) {
    if (i < s->cell_count) return &s->cells[i];
    i -= s->cell_count;
    if (i >= s->pl.gpu_cell_cap || i >= s->pl.cell_cap) return NULL;
    *tmp = s->pl.cells[i];
    tmp->first += s->instance_count;
    return tmp;
}

// GPU form of region cell c (first rebased past the static instances; VG_CELL_FOLIAGE when every
// instance is foliage: the cull drops it whole past the fade / shadow distances).
static VgCell gpu_cell(const VgScene *s, u32 c) {
    VgCell g = s->pl.cells[c];
    if (!g.count) return g;
    bool all = true;
    for (u32 i = g.first; i < g.first + g.count && all; i++) {
        u32 m = s->pl.inst[i].mesh;
        all = m < s->mesh_count && (s->meshes[m].fflags & VGMF_FOLIAGE);
    }
    g.first += s->instance_count;
    if (all) g.count |= VG_CELL_FOLIAGE;
    return g;
}

void vgeo_scene_placement_record(VgScene *s, VkCommandBuffer cmd, u32 frame) {
    VgPlacements *p = &s->pl;
    if ((!p->dirty_inst_n && !p->dirty_cells_n) || !s->b_instances.buffer || frame >= 3) return;
    // dirty ranges sorted and merged, cells unique (a chunk freed and its space reused in one frame)
    for (u32 i = 1; i < p->dirty_inst_n; i++)
        for (u32 j = i; j > 0 && p->dirty_inst[j - 1].first > p->dirty_inst[j].first; j--) {
            VgRange t = p->dirty_inst[j];
            p->dirty_inst[j] = p->dirty_inst[j - 1];
            p->dirty_inst[j - 1] = t;
        }
    u32 nr = 0;
    for (u32 i = 0; i < p->dirty_inst_n; i++) {
        VgRange r = p->dirty_inst[i];
        if (nr && r.first <= p->dirty_inst[nr - 1].first + p->dirty_inst[nr - 1].count) {
            VgRange *l = &p->dirty_inst[nr - 1];
            u32 end = DG_MAX(l->first + l->count, r.first + r.count);
            l->count = end - l->first;
        } else p->dirty_inst[nr++] = r;
    }
    p->dirty_inst_n = nr;
    u8 *seen = (u8 *)calloc(p->cell_cap + 1, 1);
    u32 nc0 = 0;
    for (u32 i = 0; i < p->dirty_cells_n; i++) {
        u32 c = p->dirty_cells[i];
        if (c < p->cell_cap && !seen[c]) { seen[c] = 1; p->dirty_cells[nc0++] = c; }
    }
    p->dirty_cells_n = nc0;
    free(seen);
    // only what the committed buffers hold (a region grown since waits for the commit)
    VkDeviceSize bytes = 0;
    for (u32 i = 0; i < p->dirty_inst_n; i++) bytes += sizeof(VgInstance) * (VkDeviceSize)p->dirty_inst[i].count;
    bytes += sizeof(VgCell) * (VkDeviceSize)p->dirty_cells_n;
    VgBuffer *st = &p->staging[frame];
    if (st->size < bytes) {   // this frame slot's previous use has completed
        vgc_buffer_free(s->ctx, st);
        *st = vgc_buffer(s->ctx, DG_MAX(bytes, (VkDeviceSize)65536) * 2, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VGM_UPLOAD);
        if (!st->buffer) return;
    }
    VkBufferCopy *ci = (VkBufferCopy *)malloc(sizeof(VkBufferCopy) * (p->dirty_inst_n + 1));
    VkBufferCopy *cc = (VkBufferCopy *)malloc(sizeof(VkBufferCopy) * (p->dirty_cells_n + 1));
    u32 ni = 0, nc = 0;
    VkDeviceSize at = 0;
    for (u32 i = 0; i < p->dirty_inst_n; i++) {
        VgRange r = p->dirty_inst[i];
        if (r.first >= p->gpu_inst_cap) continue;
        r.count = DG_MIN(r.count, p->gpu_inst_cap - r.first);
        memcpy((u8 *)st->mapped + at, &p->inst[r.first], sizeof(VgInstance) * r.count);
        ci[ni++] = (VkBufferCopy){ at, sizeof(VgInstance) * (VkDeviceSize)(s->instance_count + r.first), sizeof(VgInstance) * (VkDeviceSize)r.count };
        at += sizeof(VgInstance) * (VkDeviceSize)r.count;
    }
    for (u32 i = 0; i < p->dirty_cells_n; i++) {
        u32 c = p->dirty_cells[i];
        if (c >= p->gpu_cell_cap) continue;
        VgCell g = gpu_cell(s, c);
        memcpy((u8 *)st->mapped + at, &g, sizeof(VgCell));
        cc[nc++] = (VkBufferCopy){ at, sizeof(VgCell) * (VkDeviceSize)(s->cell_count + c), sizeof(VgCell) };
        at += sizeof(VgCell);
    }
    vmaFlushAllocation(s->ctx->vma, st->alloc, 0, at);
    // after every earlier read of these ranges (frames in flight included: same queue), before the culls
    VkMemoryBarrier2 mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &dep);
    if (ni) vkCmdCopyBuffer(cmd, st->buffer, s->b_instances.buffer, ni, ci);
    if (nc) vkCmdCopyBuffer(cmd, st->buffer, s->b_cells.buffer, nc, cc);
    mb.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier2(cmd, &dep);
    free(ci);
    free(cc);
    p->uploads++;
    p->upload_bytes += at;
    p->dirty_inst_n = p->dirty_cells_n = 0;
}

bool vgeo_scene_placement_audit(const VgScene *s, char *err, int err_size) {
    const VgPlacements *p = &s->pl;
    u8 *own = (u8 *)calloc(p->inst_cap + 1, 1);
    bool ok = true;
#define FAIL(...) do { if (ok) snprintf(err, (size_t)err_size, __VA_ARGS__); ok = false; } while (0)
    u32 live = 0, live_inst = 0;
    for (u32 h = 0; h < p->chunk_count; h++) {
        const VgPlacementChunk *ch = &p->chunks[h];
        if (!ch->used) continue;
        live++;
        live_inst += ch->count;
        for (u32 i = ch->first; i < ch->first + ch->count && i < p->inst_cap; i++) {
            if (own[i]) FAIL("instance %u owned twice (chunk %u)", i, h);
            own[i] = 1;
        }
        for (u32 k = 0; k < ch->cell_count; k++)
            if (p->cell_chunk[ch->cells[k]] != h) FAIL("cell %u of chunk %u owned by %u", ch->cells[k], h, p->cell_chunk[ch->cells[k]]);
    }
    for (u32 r = 0; r < p->free_inst_n; r++) {
        const VgRange *fr = &p->free_inst[r];
        if (r && fr->first <= p->free_inst[r - 1].first + p->free_inst[r - 1].count) FAIL("free ranges %u / %u not sorted and merged", r - 1, r);
        for (u32 i = fr->first; i < fr->first + fr->count && i < p->inst_cap; i++) {
            if (own[i]) FAIL("instance %u both free and owned", i);
            own[i] = 2;
        }
    }
    for (u32 i = 0; i < p->inst_cap; i++)
        if (!own[i]) FAIL("instance %u leaked (neither free nor owned)", i);
    u32 cells_owned = 0;
    for (u32 c = 0; c < p->cell_cap; c++) cells_owned += p->cell_chunk[c] != ~0u;
    if (cells_owned + p->free_cells_n != p->cell_cap) FAIL("cells: %u owned + %u free != %u", cells_owned, p->free_cells_n, p->cell_cap);
    for (u32 k = 0; k < p->free_cells_n; k++)
        if (p->cell_chunk[p->free_cells[k]] != ~0u || p->cells[p->free_cells[k]].count) FAIL("free cell %u in use", p->free_cells[k]);
    if (live != p->live_chunks || live_inst != p->live_instances) FAIL("live counts %u / %u vs %u / %u", live, live_inst, p->live_chunks, p->live_instances);
#undef FAIL
    free(own);
    return ok;
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
    mesh_foliage(s);
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
        world_sphere(&sorted[i], m, m->center, m->radius, w);
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
    // placement region: after the static tables, at its reserved capacity; live cells re-bounded
    // (mesh WPO bounds may have changed), uploaded whole below
    VgPlacements *pp = &s->pl;
    for (u32 cc = 0; cc < pp->cell_cap; cc++)
        if (pp->cells[cc].count) cell_bounds(s, &pp->cells[cc]);
    pp->gpu_inst_cap = pp->inst_cap;
    pp->gpu_cell_cap = pp->cell_cap;
    pp->dirty_inst_n = pp->dirty_cells_n = 0;
    u32 n_inst = s->instance_count + pp->inst_cap, n_cells = s->cell_count + pp->cell_cap;
    VgInstance *all_inst = s->instances;
    VgCell *all_cells = s->cells;
    if (pp->inst_cap || pp->cell_cap) {
        all_inst = (VgInstance *)malloc(sizeof(VgInstance) * (n_inst + 1));
        memcpy(all_inst, s->instances, sizeof(VgInstance) * s->instance_count);
        memcpy(all_inst + s->instance_count, pp->inst, sizeof(VgInstance) * pp->inst_cap);
        all_cells = (VgCell *)malloc(sizeof(VgCell) * (n_cells + 1));
        memcpy(all_cells, s->cells, sizeof(VgCell) * s->cell_count);
        for (u32 cc = 0; cc < pp->cell_cap; cc++) all_cells[s->cell_count + cc] = gpu_cell(s, cc);
    }
    assign_pool(s);
    if (!c || !c->device) {
        if (all_inst != s->instances) free(all_inst);
        if (all_cells != s->cells) free(all_cells);
        return true;
    }
    vkDeviceWaitIdle(c->device);
    free_gpu(s);
    if (!upload_pool(s)) {
        if (all_inst != s->instances) free(all_inst);
        if (all_cells != s->cells) free(all_cells);
        return false;
    }
    const VkBufferUsageFlags U = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    struct { VgBuffer *b; const void *p; VkDeviceSize n; } t[] = {
        { &s->b_meshes, s->meshes, sizeof(VgMesh) * (VkDeviceSize)s->mesh_count },
        { &s->b_nodes, s->nodes, sizeof(VgNode) * (VkDeviceSize)s->node_count },
        { &s->b_groups, s->groups, sizeof(VgGroup) * (VkDeviceSize)s->group_count },
        { &s->b_page_table, s->page_table, sizeof(u32) * (VkDeviceSize)s->page_count },
        { &s->b_instances, all_inst, sizeof(VgInstance) * (VkDeviceSize)n_inst },
        { &s->b_cells, all_cells, sizeof(VgCell) * (VkDeviceSize)n_cells },
        { &s->b_materials, s->materials, sizeof(VgMaterial) * (VkDeviceSize)s->material_count },
    };
    bool ok = true;
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]) && ok; i++) {
        *t[i].b = vgc_buffer(c, t[i].n ? t[i].n : 16, U, VGM_DEVICE);
        if (!t[i].b->buffer) ok = false;
        else if (t[i].n) vgc_upload(c, t[i].b, 0, t[i].p, t[i].n);
    }
    if (all_inst != s->instances) free(all_inst);
    if (all_cells != s->cells) free(all_cells);
    if (!ok) return false;
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
    u32 total = vgeo_scene_gpu_instances(s);
    for (u32 i = 0; i < total; i++) {
        const VgInstance *in = vgeo_scene_instance(s, i);
        if (i >= s->instance_count) {   // placement region: live only when its cell holds it
            if (in->cell >= s->pl.cell_cap) continue;
            const VgCell *cl = &s->pl.cells[in->cell];
            u32 ri = i - s->instance_count;
            if (!cl->count || ri < cl->first || ri >= cl->first + cl->count) continue;
        }
        const VgMesh *m = &s->meshes[in->mesh];
        VgXfCpu x;   // the shaders' decode (skinned: slot sphere transform and error scale)
        vgeo_xf_decode(&x, in, m, s->palettes);
        if (!x.active) continue;
        // foliage: density / fade / shadow distance, then the impostor switch (no clusters)
        if ((m->fflags & VGMF_FOLIAGE) || m->shadow_max_dist > 0) {
            float dx = in->pos[0] - v->cam_pos[0], dy = in->pos[1] - v->cam_pos[1], dz = in->pos[2] - v->cam_pos[2];
            float dist = sqrtf(dx * dx + dy * dy + dz * dz);
            if (((v->flags & VGV_ORTHO) && m->shadow_max_dist > 0 && dist > m->shadow_max_dist) || vgeo_foliage_alpha(m, in->flags_phase, dist, v->foliage[2]) <= 0) continue;
        }
        {
            float ms[4];
            vgeo_xf_sphere(&x, m->center, m->radius, ms);
            if (vgeo_use_impostor(v, m, ms, x.wpo)) continue;
        }
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
    v->slot_material_base = ~0u;
    v->foliage[2] = 1.0f;   // r_foliage_density (no wind, no impostors, no displacement buffer)
}
