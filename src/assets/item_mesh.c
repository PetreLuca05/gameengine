// Detailed item meshes: .dgim loading, material layering per condition, draw groups, LODs
// (docs/ITEM_PIPELINE.md).
#include "item_mesh.h"
#include "core/log.h"
#include "core/platform.h"
#include "core/cvar.h"
#include "gameplay/items.h"
#include "cJSON.h"
#include "stb_image.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

_Static_assert(sizeof(DgimHeader) == 116, "DgimHeader layout (tools/blender/item_export.py)");
_Static_assert(sizeof(DgimPart) == 204, "DgimPart layout");
_Static_assert(sizeof(DgimSocket) == 64, "DgimSocket layout");
_Static_assert(sizeof(DgimHull) == 12, "DgimHull layout");
_Static_assert(sizeof(DgimVertex) == 16, "DgimVertex layout");

// ------------------------------------------------------------------ material library

typedef struct ItemMaterial {
    char name[32];
    vec3 color;
    float rough, metal;
    vec3 edge_color;            // the substrate edge wear exposes (bare metal under paint / bluing)
    float edge_rough, edge_metal;
    float wear;                 // how readily it wears (x the condition's wear)
    vec3 dirt_color;
    float dirt_rough;
    float grunge;               // albedo / roughness variation from the grunge mask
    bool rusts;                 // ferrous: the rusty condition corrodes it
    vec3 rust_color;
    float rust_rough;
} ItemMaterial;

typedef struct ItemConditionDef { float wear, dirt, rust; } ItemConditionDef;

#define ITEM_LIB_MAX 64
static struct {
    bool loaded;
    ItemMaterial mats[ITEM_LIB_MAX];
    int count;
    ItemConditionDef cond[ITEM_COND_COUNT];
} L;

static ItemMesh s_meshes[ITEM_MESH_MAX];
static int s_mesh_count;
static int s_cond_override = -1;   // item_condition: -1 = per item
static int s_debug_view;           // item_mesh_set_debug_view

static vec3 jv3(const cJSON *o, const char *k, vec3 def) {
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) < 3) return def;
    return v3((float)cJSON_GetArrayItem(a, 0)->valuedouble, (float)cJSON_GetArrayItem(a, 1)->valuedouble, (float)cJSON_GetArrayItem(a, 2)->valuedouble);
}
static float jf(const cJSON *o, const char *k, float def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}

static void lib_load(void) {
    memset(&L, 0, sizeof(L));
    L.loaded = true;
    L.cond[ITEM_COND_NEW] = (ItemConditionDef){ 0.25f, 0.1f, 0.0f };
    L.cond[ITEM_COND_WORN] = (ItemConditionDef){ 1.0f, 0.55f, 0.0f };
    L.cond[ITEM_COND_RUSTY] = (ItemConditionDef){ 1.3f, 0.9f, 1.0f };
    char buf[512];
    char *text = platform_read_file(platform_data_path("assets/items/materials.json", buf, sizeof(buf)), NULL);
    if (!text) { log_warn("item_mesh: no assets/items/materials.json (flat grey materials)"); return; }
    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) { log_error("item_mesh: materials.json parse error"); return; }
    const cJSON *c;
    cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(root, "conditions")) {
        int k = item_condition_from(c->string);
        if (k < 0) continue;
        L.cond[k].wear = jf(c, "wear", L.cond[k].wear);
        L.cond[k].dirt = jf(c, "dirt", L.cond[k].dirt);
        L.cond[k].rust = jf(c, "rust", L.cond[k].rust);
    }
    const cJSON *m;
    cJSON_ArrayForEach(m, cJSON_GetObjectItemCaseSensitive(root, "materials")) {
        if (L.count >= ITEM_LIB_MAX || m->string[0] == '_') continue;
        ItemMaterial *d = &L.mats[L.count++];
        dg_strcpy(d->name, sizeof(d->name), m->string);
        d->color = jv3(m, "color", v3s(0.3f));
        d->rough = jf(m, "rough", 0.6f);
        d->metal = jf(m, "metal", 0.0f);
        const cJSON *e = cJSON_GetObjectItemCaseSensitive(m, "edge");
        d->edge_color = jv3(e, "color", d->color);
        d->edge_rough = jf(e, "rough", d->rough);
        d->edge_metal = jf(e, "metal", d->metal);
        d->wear = jf(m, "wear", e ? 1.0f : 0.0f);
        const cJSON *dt = cJSON_GetObjectItemCaseSensitive(m, "dirt");
        d->dirt_color = jv3(dt, "color", v3(0.09f, 0.075f, 0.055f));
        d->dirt_rough = jf(dt, "rough", 0.9f);
        d->grunge = jf(m, "grunge", 0.12f);
        const cJSON *r = cJSON_GetObjectItemCaseSensitive(m, "rust");
        d->rusts = r != NULL;
        d->rust_color = jv3(r, "color", v3(0.17f, 0.06f, 0.025f));
        d->rust_rough = jf(r, "rough", 0.92f);
    }
    cJSON_Delete(root);
}

static const ItemMaterial *lib_find(const char *name) {
    if (!L.loaded) lib_load();
    for (int i = 0; i < L.count; i++)
        if (!strcmp(L.mats[i].name, name)) return &L.mats[i];
    return NULL;
}

const char *item_condition_name(int c) {
    static const char *n[ITEM_COND_COUNT] = { "new", "worn", "rusty" };
    return c >= 0 && c < ITEM_COND_COUNT ? n[c] : "?";
}

int item_mesh_condition(int item_default) {
    if (s_cond_override >= 0) return s_cond_override;
    return item_default >= 0 && item_default < ITEM_COND_COUNT ? item_default : ITEM_COND_WORN;
}

// ------------------------------------------------------------------ layering

static float sat(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
static float smooth(float a, float b, float x) { float t = sat((x - a) / (b - a)); return t * t * (3 - 2 * t); }

static vec3 debug_id_color(int id) {
    static const vec3 k[8] = { { 0.9f, 0.2f, 0.2f }, { 0.2f, 0.8f, 0.3f }, { 0.25f, 0.4f, 0.95f }, { 0.95f, 0.8f, 0.2f },
                               { 0.8f, 0.3f, 0.9f }, { 0.2f, 0.85f, 0.85f }, { 0.95f, 0.55f, 0.2f }, { 0.6f, 0.6f, 0.6f } };
    vec3 c = k[id & 7];
    return id >= 8 ? v3_scale(c, 0.55f) : c;
}

// One vertex of the composed mesh: the slot's library material, then edge wear, grunge, dirt, rust.
static vec3 oct_decode(i16 ox, i16 oy) {
    float x = ox / 32767.0f, y = oy / 32767.0f;
    vec3 n = v3(x, y, 1.0f - fabsf(x) - fabsf(y));
    if (n.z < 0) {
        float tx = (1.0f - fabsf(n.y)) * (n.x >= 0 ? 1.0f : -1.0f), ty = (1.0f - fabsf(n.x)) * (n.y >= 0 ? 1.0f : -1.0f);
        n.x = tx, n.y = ty;
    }
    return v3_norm(n);
}

vec3 item_mesh_vertex_pos(const DgimPart *dp, const DgimVertex *s) {
    return v3(dp->bmin[0] + (dp->bmax[0] - dp->bmin[0]) * (s->pos[0] / 65535.0f), dp->bmin[1] + (dp->bmax[1] - dp->bmin[1]) * (s->pos[1] / 65535.0f),
              dp->bmin[2] + (dp->bmax[2] - dp->bmin[2]) * (s->pos[2] / 65535.0f));
}

static Vertex compose(const DgimPart *dp, const DgimVertex *s, const ItemMaterial *m, const ItemConditionDef *c, int mat_id, Xform x) {
    Vertex v;
    v.pos = xf_point(x, item_mesh_vertex_pos(dp, s));
    v.normal = v3_norm(quat_rotate(x.r, oct_decode(s->oct[0], s->oct[1])));
    float wear = s->wear / 255.0f, dirt = s->dirt / 255.0f, ao = s->ao / 255.0f, grunge = s->grunge / 255.0f, cav = s->cavity / 255.0f;
    vec3 col = m->color;
    float rough = m->rough, metal = m->metal;
    // Edge wear: the curvature mask, gated by the grunge noise so edges chip in patches (handled spots,
    // corners) instead of every edge glowing evenly (rails, serrations are all edges).
    float patch = smooth(0.3f, 0.75f, grunge);
    float w = smooth(0.45f, 0.78f, wear * m->wear * c->wear) * (0.12f + 0.88f * patch);
    col = v3_lerp(col, m->edge_color, w);
    rough = lerpf(rough, m->edge_rough, w);
    metal = lerpf(metal, m->edge_metal, w);
    // Grunge: low-frequency albedo / roughness variation.
    float g = (grunge - 0.5f) * 2.0f * m->grunge;
    col = v3_scale(col, 1.0f + g);
    rough = sat(rough + g * 0.35f);
    // Dirt: cavities and occluded areas (baked mask), more in a worn item.
    float d = sat(dirt * c->dirt * (0.7f + 0.6f * grunge));
    col = v3_lerp(col, m->dirt_color, d * 0.75f);
    rough = lerpf(rough, m->dirt_rough, d);
    metal *= 1.0f - d * 0.7f;
    // Rust: ferrous parts in the rusty condition, from the cavities outwards.
    if (m->rusts && c->rust > 0) {
        float r = smooth(0.35f, 0.75f, (dirt * 0.55f + cav * 0.35f + grunge * 0.5f) * c->rust);
        col = v3_lerp(col, v3_scale(m->rust_color, 0.8f + 0.4f * grunge), r);
        rough = lerpf(rough, m->rust_rough, r);
        metal = lerpf(metal, 0.0f, r);
    }
    switch (s_debug_view) {
    case 1: col = debug_id_color(mat_id); rough = 0.6f; metal = 0; break;
    case 2: col = v3s(0.03f + 0.9f * w); rough = 0.7f; metal = 0; break;
    case 3: col = v3s(0.7f); ao = ao; rough = 0.7f; metal = 0; break;
    case 4: col = v3_lerp(v3s(0.6f), v3(0.35f, 0.18f, 0.05f), d); rough = 0.7f; metal = 0; break;
    default: break;
    }
    if (s_debug_view && s_debug_view != 3) ao = 1.0f;
    u32 r8 = (u32)(sqrtf(sat(col.x)) * 255.0f + 0.5f), g8 = (u32)(sqrtf(sat(col.y)) * 255.0f + 0.5f), b8 = (u32)(sqrtf(sat(col.z)) * 255.0f + 0.5f);
    u32 a8 = (u32)(sat(ao) * 255.0f + 0.5f);
    v.color = r8 | g8 << 8 | b8 << 16 | a8 << 24;
    v.uv = v2(sat(rough), sat(metal));
    return v;
}

static const ItemMaterial *slot_material(const ItemMesh *m, int slot) {
    static ItemMaterial fallback = { "fallback", { 0.3f, 0.3f, 0.3f }, 0.6f, 0.0f, { 0.3f, 0.3f, 0.3f }, 0.6f, 0.0f, 0.0f, { 0.1f, 0.08f, 0.06f }, 0.9f, 0.1f, false, { 0 }, 0 };
    if (slot < 0 || slot >= m->mat_count) return &fallback;
    const ItemMaterial *lm = lib_find(m->mats[slot]);
    return lm ? lm : &fallback;
}

// Builds the GPU meshes of one condition (every group, every LOD).
static void build_condition(ItemMesh *m, int cond) {
    const ItemConditionDef *c = &L.cond[cond];
    m->built[cond] = true;
    for (int lod = 0; lod < m->lod_count; lod++) {
        for (int g = 0; g < m->group_count; g++) {
            ItemMeshGroup *gr = &m->groups[g];
            u32 vn = 0, in = 0;
            for (int p = 0; p < m->part_count; p++) {
                if (m->parts[p].group != g) continue;
                int l = lod;   // a part without this LOD (a small detail) is dropped from it
                if (l >= (int)m->dparts[p].lod_count) continue;
                vn += m->dparts[p].lod_vn[l];
                in += m->dparts[p].lod_in[l];
            }
            if (!vn || !in) continue;
            Vertex *vb = malloc(sizeof(Vertex) * vn);
            u32 *ib = malloc(sizeof(u32) * in);
            u32 vo = 0, io = 0;
            Xform inv = gr->part >= 0 ? xf_inverse(m->parts[gr->part].rest) : xf_identity();
            for (int p = 0; p < m->part_count; p++) {
                if (m->parts[p].group != g) continue;
                const DgimPart *dp = &m->dparts[p];
                int l = lod;
                if (l >= (int)dp->lod_count) continue;
                // Static parts are baked into their group's frame (item space for the body).
                Xform x = xf_mul(inv, m->parts[p].rest);
                for (u32 i = 0; i < dp->lod_vn[l]; i++) {
                    const DgimVertex *s = &m->verts[dp->lod_v0[l] + i];
                    vb[vo + i] = compose(dp, s, slot_material(m, s->mat), c, s->mat, x);
                }
                for (u32 i = 0; i < dp->lod_in[l]; i++) ib[io + i] = m->indices[dp->lod_i0[l] + i] + vo;
                vo += dp->lod_vn[l];
                io += dp->lod_in[l];
            }
            if (gr->mesh[cond][lod]) render_mesh_destroy(gr->mesh[cond][lod]);
            gr->mesh[cond][lod] = render_mesh_create(vb, vo, ib, io);
            gr->tris[lod] = io / 3;
            free(vb);
            free(ib);
        }
    }
}

static void free_gpu(ItemMesh *m) {
    memset(m->built, 0, sizeof(m->built));
    for (int g = 0; g < m->group_count; g++)
        for (int c = 0; c < ITEM_COND_COUNT; c++)
            for (int l = 0; l < ITEM_MESH_LODS; l++)
                if (m->groups[g].mesh[c][l]) { render_mesh_destroy(m->groups[g].mesh[c][l]); m->groups[g].mesh[c][l] = 0; }
}

// ------------------------------------------------------------------ loading

static u8 mech_of(const char *s) {
    if (!strcmp(s, "slide")) return MECH_SLIDE;
    if (!strcmp(s, "bolt")) return MECH_BOLT;
    if (!strcmp(s, "pump")) return MECH_PUMP;
    if (!strcmp(s, "mag")) return MECH_MAG;
    return MECH_NONE;
}

static i8 slot_of(const char *s) {
    if (!s[0]) return -1;
    if (!strcmp(s, "optic")) return ATT_OPTIC;
    if (!strcmp(s, "muzzle")) return ATT_MUZZLE;
    if (!strcmp(s, "underbarrel")) return ATT_UNDERBARREL;
    if (!strcmp(s, "magazine")) return ATT_MAGAZINE;
    if (!strcmp(s, "stock")) return ATT_STOCK;
    return -1;
}

// The file's header + its decompressed payload (*payload, malloc'd).
static bool parse(ItemMesh *m, const DgimHeader *h, void *blob, size_t size) {
    if (h->part_count == 0 || h->part_count > ITEM_MESH_MAX_PARTS || h->socket_count > ITEM_MESH_MAX_SOCKETS || h->material_count > ITEM_MESH_MAX_MATS) {
        log_error("item_mesh: %s: %u parts / %u sockets / %u materials exceed the limits", m->path, h->part_count, h->socket_count, h->material_count);
        return false;
    }
    size_t need = h->material_count * sizeof(DgimMaterial) + h->part_count * sizeof(DgimPart) + h->socket_count * sizeof(DgimSocket) +
                  h->hull_count * sizeof(DgimHull) + (size_t)h->vertex_count * sizeof(DgimVertex) + (size_t)h->index_count * 2 + (size_t)h->hull_point_count * 12;
    if (size != need) { log_error("item_mesh: %s: payload %zu bytes, the header says %zu", m->path, size, need); return false; }
    const u8 *p = (const u8 *)blob;
    const DgimMaterial *mats = (const DgimMaterial *)p;
    p += h->material_count * sizeof(DgimMaterial);
    m->dparts = (const DgimPart *)p;
    p += h->part_count * sizeof(DgimPart);
    const DgimSocket *socks = (const DgimSocket *)p;
    p += h->socket_count * sizeof(DgimSocket);
    m->hulls = (const DgimHull *)p;
    p += h->hull_count * sizeof(DgimHull);
    m->verts = (const DgimVertex *)p;
    p += (size_t)h->vertex_count * sizeof(DgimVertex);
    m->indices = (const u16 *)p;
    p += (size_t)h->index_count * 2;
    m->hull_points = (const float *)p;

    dg_strcpy(m->id, sizeof(m->id), h->id);
    m->mat_count = (int)h->material_count;
    for (int i = 0; i < m->mat_count; i++) {
        memcpy(m->mats[i], mats[i].name, 31);
        m->mats[i][31] = 0;
        if (!lib_find(m->mats[i])) log_warn("item_mesh: %s: material slot '%s' is not in assets/items/materials.json", m->path, m->mats[i]);
    }
    m->part_count = (int)h->part_count;
    m->socket_count = (int)h->socket_count;
    m->hull_count = (int)h->hull_count;
    m->lod_count = 1;
    for (int l = 0; l < ITEM_MESH_LODS; l++) m->lod_dist[l] = h->lod_dist[l];
    m->bmin = v3(h->bmin[0], h->bmin[1], h->bmin[2]);
    m->bmax = v3(h->bmax[0], h->bmax[1], h->bmax[2]);
    memset(m->tris, 0, sizeof(m->tris));
    for (int i = 0; i < m->part_count; i++) {
        const DgimPart *d = &m->dparts[i];
        ItemMeshPart *q = &m->parts[i];
        memset(q, 0, sizeof(*q));
        memcpy(q->name, d->name, 31);
        q->parent = d->parent >= 0 && d->parent < i ? d->parent : -1;   // parents come first
        q->local = xf_make(v3(d->pos[0], d->pos[1], d->pos[2]), quat_norm((quat){ d->rot[0], d->rot[1], d->rot[2], d->rot[3] }));
        q->rest = q->parent >= 0 ? xf_mul(m->parts[q->parent].rest, q->local) : q->local;
        memcpy(q->role, d->mech, 15);
        char hide[17] = { 0 }, fold[17] = { 0 };
        memcpy(hide, d->hide, 16);
        memcpy(fold, d->fold, 16);
        q->mech = mech_of(q->role);
        q->hide_slot = slot_of(hide);
        q->fold_slot = slot_of(fold);
        q->fold_axis = v3_norm(v3(d->fold_axis[0], d->fold_axis[1], d->fold_axis[2]));
        q->fold_angle = d->fold_angle * DG_DEG2RAD;
        q->bmin = v3(d->bmin[0], d->bmin[1], d->bmin[2]);
        q->bmax = v3(d->bmax[0], d->bmax[1], d->bmax[2]);
        q->lod_count = (int)DG_MIN(d->lod_count, ITEM_MESH_LODS);
        m->lod_count = DG_MAX(m->lod_count, q->lod_count);
        for (int l = 0; l < q->lod_count; l++) {
            if ((size_t)d->lod_v0[l] + d->lod_vn[l] > h->vertex_count || (size_t)d->lod_i0[l] + d->lod_in[l] > h->index_count) {
                log_error("item_mesh: %s: part %s LOD %d out of range", m->path, q->name, l);
                return false;
            }
            for (u32 k = 0; k < d->lod_in[l]; k++)
                if (m->indices[d->lod_i0[l] + k] >= d->lod_vn[l]) { log_error("item_mesh: %s: part %s: bad index", m->path, q->name); return false; }
            q->tris[l] = d->lod_in[l] / 3;
        }
    }
    for (int l = 0; l < ITEM_MESH_LODS; l++)
        for (int i = 0; i < m->part_count; i++) m->tris[l] += l < m->parts[i].lod_count ? m->parts[i].tris[l] : 0;
    for (int i = 0; i < m->socket_count; i++) {
        ItemMeshSocket *s = &m->sockets[i];
        memcpy(s->name, socks[i].name, 31);
        s->name[31] = 0;
        s->part = socks[i].part >= 0 && socks[i].part < m->part_count ? socks[i].part : -1;
        s->local = xf_make(v3(socks[i].pos[0], socks[i].pos[1], socks[i].pos[2]), quat_norm((quat){ socks[i].rot[0], socks[i].rot[1], socks[i].rot[2], socks[i].rot[3] }));
    }
    // Draw groups: the static body, then one per moving part (mech / hidden / folding); static children
    // ride in their nearest moving ancestor's group.
    m->group_count = 1;
    m->groups[0].part = -1;
    for (int i = 0; i < m->part_count; i++) {
        ItemMeshPart *q = &m->parts[i];
        bool moving = q->mech != MECH_NONE || q->hide_slot >= 0 || q->fold_slot >= 0;
        if (moving && m->group_count < ITEM_MESH_MAX_GROUPS) {
            q->group = m->group_count;
            m->groups[m->group_count].part = i;
            m->group_count++;
        } else {
            q->group = q->parent >= 0 ? m->parts[q->parent].group : 0;
        }
    }
    return true;
}

static bool load(ItemMesh *m) {
    char buf[512];
    const char *full = platform_data_path(m->path, buf, sizeof(buf));
    size_t size = 0;
    u8 *file = (u8 *)platform_read_file(full, &size);
    if (!file) { log_warn("item_mesh: cannot read %s (box placeholder)", m->path); return false; }
    DgimHeader h;
    if (size < sizeof(h)) { free(file); return false; }
    memcpy(&h, file, sizeof(h));
    if (h.magic != DGIM_MAGIC || h.version != DGIM_VERSION || sizeof(h) + h.packed_bytes > size) {
        log_error("item_mesh: %s: not a DGIM v%u file (tools/blender/build_items.py rebuilds it)", m->path, DGIM_VERSION);
        free(file);
        return false;
    }
    int out = 0;
    void *blob = stbi_zlib_decode_malloc_guesssize_headerflag((const char *)file + sizeof(h), (int)h.packed_bytes, (int)h.payload_bytes, &out, 1);
    free(file);
    if (!blob || (u32)out != h.payload_bytes) { log_error("item_mesh: %s: payload does not inflate", m->path); free(blob); return false; }
    ItemMesh keep = *m;
    if (!parse(m, &h, blob, (size_t)out)) {
        free(blob);
        *m = keep;
        return false;
    }
    if (keep.blob && keep.blob != blob) free(keep.blob);
    m->blob = blob;
    m->mtime = platform_file_mtime(full);
    m->loaded = true;
    log_info("item_mesh: %s: %d parts, %d groups, %d sockets, %d LODs, %u / %u / %u tris", m->path, m->part_count, m->group_count, m->socket_count,
             m->lod_count, m->tris[0], m->tris[1], m->tris[2]);
    return true;
}

static bool s_disabled;   // item_meshes 0: every draw falls back to the box placeholders (before / after shots)

ItemMesh *item_mesh_get(const char *path) {
    if (!path || !path[0] || s_disabled) return NULL;
    for (int i = 0; i < s_mesh_count; i++)
        if (!strcmp(s_meshes[i].path, path)) return s_meshes[i].loaded ? &s_meshes[i] : NULL;
    if (s_mesh_count >= ITEM_MESH_MAX) return NULL;
    ItemMesh *m = &s_meshes[s_mesh_count++];
    memset(m, 0, sizeof(*m));
    dg_strcpy(m->path, sizeof(m->path), path);
    if (!L.loaded) lib_load();
    if (!load(m)) { m->failed = true; return NULL; }
    return m;
}

// ------------------------------------------------------------------ posing / drawing

Xform item_mesh_part_xform(const ItemMesh *m, int part, const ItemMeshPose *pose) {
    if (part < 0 || part >= m->part_count) return xf_identity();
    const ItemMeshPart *q = &m->parts[part];
    Xform x = q->parent >= 0 ? item_mesh_part_xform(m, q->parent, pose) : xf_identity();
    x = xf_mul(x, q->local);
    if (pose && q->fold_slot >= 0 && (pose->fold_slots >> q->fold_slot & 1u)) x = xf_mul(x, xf_make(v3(0, 0, 0), quat_axis_angle(q->fold_axis, q->fold_angle)));
    if (pose && (q->mech == MECH_SLIDE || q->mech == MECH_BOLT || q->mech == MECH_PUMP)) x.t.z += pose->slide;
    if (pose && q->mech == MECH_MAG && fabsf(pose->wobble.w) < 0.999999f) {
        x.t = quat_rotate(pose->wobble, x.t);
        x.r = quat_mul(pose->wobble, x.r);
    }
    return x;
}

static bool group_hidden(const ItemMesh *m, int g, const ItemMeshPose *pose) {
    if (!pose || !pose->hide_slots) return false;
    for (int p = m->groups[g].part; p >= 0; p = m->parts[p].parent)
        if (m->parts[p].hide_slot >= 0 && (pose->hide_slots >> m->parts[p].hide_slot & 1u)) return true;
    return false;
}

static MeshId group_mesh(ItemMesh *m, int g, int cond, int lod) {
    cond = DG_CLAMP(cond, 0, ITEM_COND_COUNT - 1);
    lod = DG_CLAMP(lod, 0, m->lod_count - 1);
    if (!m->built[cond]) build_condition(m, cond);
    return m->groups[g].mesh[cond][lod];
}

int item_mesh_lod_for(const ItemMesh *m, float dist) {
    int lod = 0;
    for (int l = 1; l < m->lod_count; l++)
        if (m->lod_dist[l] > 0 && dist >= m->lod_dist[l]) lod = l;
    return lod;
}

void item_mesh_draw(ItemMesh *m, int condition, vec3 pos, quat rot, float scale, const ItemMeshPose *pose, u32 flags, bool viewmodel) {
    if (!m) return;
    int lod = 0;
    if (!viewmodel) {
        vec3 cp;
        float fov;
        render_camera_info(&cp, &fov);
        float k = tanf(fmaxf(fov, 1.0f) * 0.5f * DG_DEG2RAD) / tanf(37.5f * DG_DEG2RAD);   // narrow FOVs (scopes) keep detail
        lod = item_mesh_lod_for(m, v3_len(v3_sub(pos, cp)) * k / fmaxf(scale, 1e-3f));
    }
    for (int g = 0; g < m->group_count; g++) {
        if (group_hidden(m, g, pose)) continue;
        MeshId mesh = group_mesh(m, g, condition, lod);
        if (!mesh) continue;
        Xform x = m->groups[g].part >= 0 ? item_mesh_part_xform(m, m->groups[g].part, pose) : xf_identity();
        DrawItem d;
        memset(&d, 0, sizeof(d));
        d.mesh = mesh;
        d.model = mat4_trs(v3_add(pos, quat_rotate(rot, v3_scale(x.t, scale))), quat_mul(rot, x.r), v3s(scale));
        d.mat = material_color(v4(1, 1, 1, 1));
        d.mat.flags = flags;
        d.mat.roughness = MAT_ROUGHNESS_VERTEX;   // per-vertex (roughness, metallic) + sqrt albedo
        d.mat.ao_vertex = true;
        if (viewmodel) render_draw_viewmodel(&d);
        else render_draw(&d);
    }
}

int item_mesh_ui_parts(ItemMesh *m, int condition, Xform root, const ItemMeshPose *pose, float highlight, RenderUi3dPart *out, int max, vec3 *mn,
                       vec3 *mx) {
    if (!m) return 0;
    int n = 0;
    for (int g = 0; g < m->group_count && n < max; g++) {
        if (group_hidden(m, g, pose)) continue;
        MeshId mesh = group_mesh(m, g, condition, 0);
        if (!mesh) continue;
        Xform x = xf_mul(root, m->groups[g].part >= 0 ? item_mesh_part_xform(m, m->groups[g].part, pose) : xf_identity());
        RenderUi3dPart *p = &out[n++];
        memset(p, 0, sizeof(*p));
        p->mesh = mesh;
        p->center = x.t;
        p->rot = x.r;
        p->half = v3s(root.s.x);
        p->color = v4(1, 1, 1, 1);
        p->highlight = highlight;
        // Bounds: the group's parts' boxes.
        for (int q = 0; q < m->part_count; q++) {
            if (m->parts[q].group != g) continue;
            Xform px = xf_mul(root, item_mesh_part_xform(m, q, pose));
            for (int c = 0; c < 8; c++) {
                vec3 b = v3(c & 1 ? m->parts[q].bmax.x : m->parts[q].bmin.x, c & 2 ? m->parts[q].bmax.y : m->parts[q].bmin.y, c & 4 ? m->parts[q].bmax.z : m->parts[q].bmin.z);
                vec3 w = xf_point(px, b);
                *mn = v3_min(*mn, w);
                *mx = v3_max(*mx, w);
            }
        }
    }
    return n;
}

int item_mesh_find_part(const ItemMesh *m, const char *name) {
    for (int i = 0; m && i < m->part_count; i++)
        if (!strcmp(m->parts[i].name, name)) return i;
    return -1;
}

bool item_mesh_socket(const ItemMesh *m, const char *name, Xform *out) {
    for (int i = 0; m && i < m->socket_count; i++) {
        if (strcmp(m->sockets[i].name, name)) continue;
        Xform base = m->sockets[i].part >= 0 ? m->parts[m->sockets[i].part].rest : xf_identity();
        *out = xf_mul(base, m->sockets[i].local);
        return true;
    }
    return false;
}

int item_mesh_reload_all(bool force) {
    lib_load();
    int n = 0;
    for (int i = 0; i < s_mesh_count; i++) {
        ItemMesh *m = &s_meshes[i];
        char buf[512];
        i64 t = platform_file_mtime(platform_data_path(m->path, buf, sizeof(buf)));
        bool changed = force || t != m->mtime || m->failed;
        free_gpu(m);   // the library may have changed: every condition is re-composed on next use
        if (!changed) continue;
        if (load(m)) { m->failed = false; n++; }
    }
    return n;
}

void item_mesh_set_debug_view(int view) {
    if (view == s_debug_view) return;
    s_debug_view = view;
    for (int i = 0; i < s_mesh_count; i++) free_gpu(&s_meshes[i]);
}

int item_mesh_debug_view(void) { return s_debug_view; }

void item_mesh_shutdown(void) {
    for (int i = 0; i < s_mesh_count; i++) {
        free_gpu(&s_meshes[i]);
        free(s_meshes[i].blob);
    }
    memset(s_meshes, 0, sizeof(s_meshes));
    s_mesh_count = 0;
}

int item_mesh_count(void) { return s_mesh_count; }
ItemMesh *item_mesh_at(int i) { return i >= 0 && i < s_mesh_count ? &s_meshes[i] : NULL; }

// ------------------------------------------------------------------ commands

static void cmd_item_condition(int argc, const char **argv) {
    if (argc < 2) { log_info("item_condition: %s (new | worn | rusty | auto)", s_cond_override < 0 ? "auto" : item_condition_name(s_cond_override)); return; }
    s_cond_override = item_condition_from(argv[1]);
    log_info("item_condition: %s", s_cond_override < 0 ? "auto (per item)" : item_condition_name(s_cond_override));
}

static void cmd_item_mesh_list(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    for (int i = 0; i < s_mesh_count; i++) {
        const ItemMesh *m = &s_meshes[i];
        log_info("%-40s %s parts %2d groups %2d sockets %2d tris %6u / %6u / %6u", m->path, m->loaded ? "ok  " : "FAIL", m->part_count, m->group_count, m->socket_count,
                 m->tris[0], m->tris[1], m->tris[2]);
    }
}

static void cmd_item_meshes(int argc, const char **argv) {
    if (argc > 1) s_disabled = atoi(argv[1]) == 0;
    log_info("item_meshes: %s", s_disabled ? "0 (box placeholders)" : "1 (detailed meshes)");
}

void item_mesh_register(void) {
    cmd_register("item_meshes", cmd_item_meshes, "item_meshes [0|1]: draw the detailed item meshes (1) or the old box placeholders (0, before / after shots)");
    cmd_register("item_condition", cmd_item_condition, "item_condition [new|worn|rusty|auto]: item mesh wear condition override (docs/ITEM_PIPELINE.md)");
    cmd_register("item_mesh_list", cmd_item_mesh_list, "list the loaded item meshes: parts, draw groups, sockets, triangles per LOD");
}
