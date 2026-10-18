// Kit piece loading + validation (docs/BUILDING_KIT.md "Conventions"). Pure; see kit_piece.h.
#include "assets/kit_piece.h"
#include "cgltf.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static void xform_point(const float m[16], const float *in, float *out) {
    for (int r = 0; r < 3; r++) out[r] = m[r] * in[0] + m[4 + r] * in[1] + m[8 + r] * in[2] + m[12 + r];
}

static void xform_dir(const float m[16], const float *in, float *out) {
    float v[3];
    for (int r = 0; r < 3; r++) v[r] = m[r] * in[0] + m[4 + r] * in[1] + m[8 + r] * in[2];
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    for (int r = 0; r < 3; r++) out[r] = l > 1e-12f ? v[r] / l : 0.0f;
}

// Rotation of a (rigid) column-major matrix as a quaternion.
static void mat_quat(const float m[16], float q[4]) {
    float c0[3] = { m[0], m[1], m[2] }, c1[3] = { m[4], m[5], m[6] }, c2[3] = { m[8], m[9], m[10] };
    float s0 = sqrtf(c0[0] * c0[0] + c0[1] * c0[1] + c0[2] * c0[2]), s1 = sqrtf(c1[0] * c1[0] + c1[1] * c1[1] + c1[2] * c1[2]);
    float s2 = sqrtf(c2[0] * c2[0] + c2[1] * c2[1] + c2[2] * c2[2]);
    for (int i = 0; i < 3; i++) { c0[i] /= s0 > 0 ? s0 : 1; c1[i] /= s1 > 0 ? s1 : 1; c2[i] /= s2 > 0 ? s2 : 1; }
    float tr = c0[0] + c1[1] + c2[2];
    if (tr > 0) {
        float s = sqrtf(tr + 1.0f) * 2.0f;
        q[3] = 0.25f * s; q[0] = (c1[2] - c2[1]) / s; q[1] = (c2[0] - c0[2]) / s; q[2] = (c0[1] - c1[0]) / s;
    } else if (c0[0] > c1[1] && c0[0] > c2[2]) {
        float s = sqrtf(1.0f + c0[0] - c1[1] - c2[2]) * 2.0f;
        q[3] = (c1[2] - c2[1]) / s; q[0] = 0.25f * s; q[1] = (c1[0] + c0[1]) / s; q[2] = (c2[0] + c0[2]) / s;
    } else if (c1[1] > c2[2]) {
        float s = sqrtf(1.0f + c1[1] - c0[0] - c2[2]) * 2.0f;
        q[3] = (c2[0] - c0[2]) / s; q[0] = (c1[0] + c0[1]) / s; q[1] = 0.25f * s; q[2] = (c2[1] + c1[2]) / s;
    } else {
        float s = sqrtf(1.0f + c2[2] - c0[0] - c1[1]) * 2.0f;
        q[3] = (c0[1] - c1[0]) / s; q[0] = (c2[0] + c0[2]) / s; q[1] = (c2[1] + c1[2]) / s; q[2] = 0.25f * s;
    }
}

static const cgltf_accessor *attr(const cgltf_primitive *pr, cgltf_attribute_type t, int index) {
    for (cgltf_size i = 0; i < pr->attributes_count; i++)
        if (pr->attributes[i].type == t && pr->attributes[i].index == index) return pr->attributes[i].data;
    return NULL;
}

static KitSlotMesh *slot_for(KitLod *lod, const char *slot) {
    for (int i = 0; i < lod->slot_n; i++)
        if (!strcmp(lod->slots[i].slot, slot)) return &lod->slots[i];
    if (lod->slot_n >= KIT_MAX_SLOTS) return NULL;
    KitSlotMesh *s = &lod->slots[lod->slot_n++];
    memset(s, 0, sizeof(*s));
    dg_strcpy(s->slot, sizeof(s->slot), slot);
    return s;
}

// Appends a primitive's triangles (piece space) to a slot mesh.
static bool append_prim(KitSlotMesh *s, const cgltf_primitive *pr, const float m[16], bool *has_uv1, bool *has_nrm) {
    const cgltf_accessor *ap = attr(pr, cgltf_attribute_type_position, 0);
    if (!ap || pr->type != cgltf_primitive_type_triangles) return false;
    const cgltf_accessor *an = attr(pr, cgltf_attribute_type_normal, 0), *a0 = attr(pr, cgltf_attribute_type_texcoord, 0);
    const cgltf_accessor *a1 = attr(pr, cgltf_attribute_type_texcoord, 1);
    if (!an) *has_nrm = false;
    if (!a1) *has_uv1 = false;
    u32 vn = (u32)ap->count, base = s->vcount;
    u32 in = pr->indices ? (u32)pr->indices->count : vn;
    s->pos = realloc(s->pos, sizeof(float) * 3 * (base + vn));
    s->nrm = realloc(s->nrm, sizeof(float) * 3 * (base + vn));
    s->uv0 = realloc(s->uv0, sizeof(float) * 2 * (base + vn));
    s->uv1 = realloc(s->uv1, sizeof(float) * 2 * (base + vn));
    s->idx = realloc(s->idx, sizeof(u32) * (s->icount + in));
    for (u32 v = 0; v < vn; v++) {
        float p[3] = { 0 }, n[3] = { 0, 1, 0 }, t0[2] = { 0 }, t1[2] = { 0 };
        cgltf_accessor_read_float(ap, v, p, 3);
        if (an) cgltf_accessor_read_float(an, v, n, 3);
        if (a0) cgltf_accessor_read_float(a0, v, t0, 2);
        if (a1) cgltf_accessor_read_float(a1, v, t1, 2);
        xform_point(m, p, &s->pos[3 * (base + v)]);
        xform_dir(m, n, &s->nrm[3 * (base + v)]);
        s->uv0[2 * (base + v)] = t0[0];
        s->uv0[2 * (base + v) + 1] = t0[1];
        s->uv1[2 * (base + v)] = t1[0];
        s->uv1[2 * (base + v) + 1] = t1[1];
    }
    for (u32 k = 0; k < in; k++) s->idx[s->icount + k] = base + (pr->indices ? (u32)cgltf_accessor_read_index(pr->indices, k) : k);
    s->vcount += vn;
    s->icount += in;
    return true;
}

bool kit_piece_load(KitPiece *p, const char *path, const char *id, char *err, int err_size) {
    memset(p, 0, sizeof(*p));
    dg_strcpy(p->id, sizeof(p->id), id);
    if (err_size > 0) err[0] = 0;
    cgltf_options opt;
    memset(&opt, 0, sizeof(opt));
    cgltf_data *g = NULL;
    if (cgltf_parse_file(&opt, path, &g) != cgltf_result_success) { snprintf(err, (size_t)err_size, "%s: not a glTF file", path); return false; }
    if (cgltf_load_buffers(&opt, g, path) != cgltf_result_success) { snprintf(err, (size_t)err_size, "%s: cannot load its buffers", path); cgltf_free(g); return false; }
    size_t idl = strlen(id);
    bool uv1 = true, nrm = true;
    for (cgltf_size ni = 0; ni < g->nodes_count; ni++) {
        const cgltf_node *nd = &g->nodes[ni];
        const char *name = nd->name ? nd->name : "";
        float m[16];
        cgltf_node_transform_world(nd, m);
        if (!nd->mesh) {
            if (!strncmp(name, "S_", 2) && p->socket_n < KIT_MAX_SOCKETS) {
                KitSocket *s = &p->sockets[p->socket_n++];
                dg_strcpy(s->name, sizeof(s->name), name);
                dg_strcpy(s->type, sizeof(s->type), name + 2);
                char *us = strrchr(s->type, '_');   // "light_2" -> "light"
                if (us && us[1] >= '0' && us[1] <= '9') *us = 0;
                s->pos[0] = m[12]; s->pos[1] = m[13]; s->pos[2] = m[14];
                mat_quat(m, s->rot);
            }
            continue;
        }
        bool ucx = !strncmp(name, "UCX_", 4), ubx = !strncmp(name, "UBX_", 4);
        if (ucx || ubx) {
            if (p->hull_n >= KIT_MAX_HULLS) continue;
            KitHull *h = &p->hulls[p->hull_n++];
            h->box = ubx;
            KitSlotMesh tmp;
            memset(&tmp, 0, sizeof(tmp));
            bool a = true, b = true;
            for (cgltf_size pi = 0; pi < nd->mesh->primitives_count; pi++) append_prim(&tmp, &nd->mesh->primitives[pi], m, &a, &b);
            h->verts = tmp.pos;
            h->idx = tmp.idx;
            h->vcount = tmp.vcount;
            h->icount = tmp.icount;
            free(tmp.nrm);
            free(tmp.uv0);
            free(tmp.uv1);
            for (int k = 0; k < 3; k++) { h->mn[k] = 1e30f; h->mx[k] = -1e30f; }
            for (u32 v = 0; v < h->vcount; v++)
                for (int k = 0; k < 3; k++) {
                    h->mn[k] = fminf(h->mn[k], h->verts[3 * v + k]);
                    h->mx[k] = fmaxf(h->mx[k], h->verts[3 * v + k]);
                }
            continue;
        }
        int lod = -1;
        if (!strcmp(name, id)) lod = 0;
        else if (!strncmp(name, id, idl) && !strncmp(name + idl, "_LOD", 4) && name[idl + 4] >= '1' && name[idl + 4] <= '3' && !name[idl + 5]) lod = name[idl + 4] - '0';
        if (lod < 0) {
            if (!p->unnamed_nodes++) dg_strcpy(p->first_unnamed, sizeof(p->first_unnamed), name);
            continue;
        }
        if (lod + 1 > p->lod_n) p->lod_n = lod + 1;
        KitLod *L = &p->lods[lod];
        for (cgltf_size pi = 0; pi < nd->mesh->primitives_count; pi++) {
            const cgltf_primitive *pr = &nd->mesh->primitives[pi];
            const char *slot = pr->material && pr->material->name ? pr->material->name : "(none)";
            KitSlotMesh *s = slot_for(L, slot);
            if (s && append_prim(s, pr, m, &uv1, &nrm)) L->tris += (u32)((pr->indices ? pr->indices->count : attr(pr, cgltf_attribute_type_position, 0)->count) / 3);
        }
    }
    cgltf_free(g);
    if (!uv1)   // a primitive without TEXCOORD_1: drop the (zero-filled) set so validation reports it
        for (int l = 0; l < p->lod_n; l++)
            for (int s = 0; s < p->lods[l].slot_n; s++) { free(p->lods[l].slots[s].uv1); p->lods[l].slots[s].uv1 = NULL; }
    p->missing_normals = !nrm;
    for (int k = 0; k < 3; k++) { p->mn[k] = 1e30f; p->mx[k] = -1e30f; }
    const KitLod *L0 = &p->lods[0];
    for (int s = 0; s < L0->slot_n; s++)
        for (u32 v = 0; v < L0->slots[s].vcount; v++)
            for (int k = 0; k < 3; k++) {
                p->mn[k] = fminf(p->mn[k], L0->slots[s].pos[3 * v + k]);
                p->mx[k] = fmaxf(p->mx[k], L0->slots[s].pos[3 * v + k]);
            }
    return true;
}

void kit_piece_free(KitPiece *p) {
    for (int l = 0; l < KIT_MAX_LODS; l++)
        for (int s = 0; s < p->lods[l].slot_n; s++) {
            KitSlotMesh *m = &p->lods[l].slots[s];
            free(m->pos); free(m->nrm); free(m->uv0); free(m->uv1); free(m->idx);
        }
    for (int h = 0; h < p->hull_n; h++) { free(p->hulls[h].verts); free(p->hulls[h].idx); }
    memset(p, 0, sizeof(*p));
}

void kit_rules_default(KitRules *r) {
    memset(r, 0, sizeof(*r));
    r->texel_density = 10.24f;
    r->texel_tolerance = 0.15f;
    r->uv1_required = true;
}

const KitSocket *kit_piece_socket(const KitPiece *p, const char *type, int nth) {
    for (int i = 0; i < p->socket_n; i++)
        if (!strcmp(p->sockets[i].type, type) && nth-- == 0) return &p->sockets[i];
    return NULL;
}

// ------------------------------------------------------------------ validation

typedef struct Report { char *out; int size, len, n; const char *id; } Report;

static void problem(Report *r, const char *fmt, ...) {
    r->n++;
    if (r->len >= r->size - 1) return;
    int w = snprintf(r->out + r->len, (size_t)(r->size - r->len), "%s: ", r->id);
    if (w > 0) r->len = DG_MIN(r->len + w, r->size - 1);
    va_list ap;
    va_start(ap, fmt);
    w = vsnprintf(r->out + r->len, (size_t)(r->size - r->len), fmt, ap);
    va_end(ap);
    if (w > 0) r->len = DG_MIN(r->len + w, r->size - 1);
    if (r->len < r->size - 1) { r->out[r->len++] = '\n'; r->out[r->len] = 0; }
}

static bool near_multiple(float v, float step) {
    float k = v / step;
    return fabsf(k - roundf(k)) * step < 0.002f;
}

// Every vertex of a convex hull lies on or behind every face plane.
static bool hull_convex(const KitHull *h) {
    for (u32 t = 0; t + 2 < h->icount; t += 3) {
        const float *a = &h->verts[3 * h->idx[t]], *b = &h->verts[3 * h->idx[t + 1]], *c = &h->verts[3 * h->idx[t + 2]];
        float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
        float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
        float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (l < 1e-9f) continue;
        for (u32 v = 0; v < h->vcount; v++) {
            const float *p = &h->verts[3 * v];
            float d = ((p[0] - a[0]) * n[0] + (p[1] - a[1]) * n[1] + (p[2] - a[2]) * n[2]) / l;
            if (d > 0.002f) return false;
        }
    }
    return true;
}

static bool valid_id(const char *s) {
    if (!*s) return false;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_')) return false;
    return true;
}

static bool slot_known(const char *list, const char *slot) {
    size_t l = strlen(slot);
    for (const char *p = list; p && *p;) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n == l && !strncmp(p, slot, l)) return true;
        p = e ? e + 1 : NULL;
    }
    return false;
}

int kit_piece_validate(const KitPiece *p, const KitRules *r, const char *known_slots, char *out, int out_size) {
    Report rep = { out, out_size, 0, 0, p->id };
    if (out_size > 0) out[0] = 0;
    if (!valid_id(p->id)) problem(&rep, "name '%s' - piece ids are lower case [a-z0-9_] (the file stem)", p->id);
    if (p->unnamed_nodes) problem(&rep, "%d mesh object(s) with an unknown name ('%s') - use %s, %s_LOD1..3, UCX_%s_NN or UBX_%s_NN", p->unnamed_nodes, p->first_unnamed, p->id, p->id, p->id, p->id);
    if (!p->lod_n || !p->lods[0].slot_n) {
        problem(&rep, "no render mesh named '%s' - name the LOD0 object exactly like the file", p->id);
        return rep.n;
    }
    if (p->missing_normals) problem(&rep, "a render mesh has no normals - export with normals (glTF: Normals on)");
    // Pivot: bottom-left-back corner on the grid.
    if (fabsf(p->mn[0]) > 0.001f || fabsf(p->mn[1]) > 0.001f || fabsf(p->mx[2]) > 0.001f)
        problem(&rep, "pivot off grid: bbox min x %.3f, min y %.3f, max z %.3f (want 0, 0, 0) - move the origin to the bottom-left-back corner (Blender: Set Origin, then snap to 0)", p->mn[0], p->mn[1], p->mx[2]);
    // Scale: real metres, sane extents (catches cm / inch exports and unapplied scale).
    float ext[3] = { p->mx[0] - p->mn[0], p->mx[1] - p->mn[1], p->mx[2] - p->mn[2] };
    float big = fmaxf(ext[0], fmaxf(ext[1], ext[2]));
    if (big > 60.0f || big < 0.004f) problem(&rep, "scale: largest extent %.3f m - export in metres with the scale applied (Ctrl+A > Scale)", big);
    if (r && r->grid > 0 && (!near_multiple(ext[0], r->grid) || !near_multiple(ext[2], r->grid)))
        problem(&rep, "footprint %.3f x %.3f m off the %s grid (%.2f m) - snap the vertices", ext[0], ext[2], r->category, r->grid);
    if (r && r->grid_height > 0 && !near_multiple(ext[1], r->grid_height))
        problem(&rep, "height %.3f m off the %s grid (%.2f m) - snap the vertices", ext[1], r->category, r->grid_height);
    // Material slots.
    for (int l = 0; l < p->lod_n; l++)
        for (int s = 0; s < p->lods[l].slot_n; s++) {
            const char *sl = p->lods[l].slots[s].slot;
            if (strncmp(sl, "M_", 2)) problem(&rep, "material slot '%s' unmapped - rename it to M_<set> (assets/kits/building/materials)", sl);
            else if (known_slots && !slot_known(known_slots, sl)) problem(&rep, "material slot '%s' has no material set - add assets/kits/building/materials/%s.json or rename the slot", sl, sl + 2);
        }
    // Sockets.
    for (int i = 0; r && i < r->required_socket_n; i++)
        if (!kit_piece_socket(p, r->required_sockets[i], 0)) problem(&rep, "socket S_%s missing - %s pieces need it (an empty named S_%s)", r->required_sockets[i], r->category, r->required_sockets[i]);
    // Collision.
    if (r && r->collision_required && !p->hull_n) problem(&rep, "no collision - add UCX_%s_01 (convex) or UBX_%s_01 (box) meshes", p->id, p->id);
    for (int h = 0; h < p->hull_n; h++) {
        const KitHull *hu = &p->hulls[h];
        if (hu->vcount > KIT_MAX_HULL_VERTS) problem(&rep, "collision hull %d has %u vertices (max %d) - simplify it", h + 1, hu->vcount, KIT_MAX_HULL_VERTS);
        if (!hu->box && !hull_convex(hu)) problem(&rep, "collision hull %d is not convex - split it into convex UCX_ parts", h + 1);
    }
    // LODs: each coarser than the one before.
    for (int l = 1; l < p->lod_n; l++)
        if (p->lods[l].slot_n && p->lods[l].tris >= p->lods[l - 1].tris)
            problem(&rep, "LOD%d has %u triangles, not fewer than LOD%d (%u)", l, p->lods[l].tris, l - 1, p->lods[l - 1].tris);
    if (r && r->max_tris && p->lods[0].tris > (u32)r->max_tris) problem(&rep, "LOD0 has %u triangles (budget %d for %s)", p->lods[0].tris, r->max_tris, r->category);
    // UV sets, degenerate triangles, texel density (world-scaled UV0: 1 unit = 1 m at 1024 px).
    double uv_len = 0, world_len = 0;
    int degenerate = 0;
    bool uv1_bad = false, uv1_missing = false;
    const KitLod *L0 = &p->lods[0];
    for (int s = 0; s < L0->slot_n; s++) {
        const KitSlotMesh *m = &L0->slots[s];
        if (!m->uv1) uv1_missing = true;
        for (u32 t = 0; t + 2 < m->icount; t += 3) {
            u32 i0 = m->idx[t], i1 = m->idx[t + 1], i2 = m->idx[t + 2];
            const float *a = &m->pos[3 * i0], *b = &m->pos[3 * i1], *c = &m->pos[3 * i2];
            float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
            float cr[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
            if (sqrtf(cr[0] * cr[0] + cr[1] * cr[1] + cr[2] * cr[2]) < 1e-9f) { degenerate++; continue; }
            float wl = sqrtf(e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2]);
            float du = m->uv0[2 * i1] - m->uv0[2 * i0], dv = m->uv0[2 * i1 + 1] - m->uv0[2 * i0 + 1];
            world_len += wl;
            uv_len += sqrtf(du * du + dv * dv);
            if (m->uv1)
                for (int k = 0; k < 3; k++) {
                    float u = m->uv1[2 * m->idx[t + k]], v = m->uv1[2 * m->idx[t + k] + 1];
                    if (u < -0.001f || u > 1.001f || v < -0.001f || v > 1.001f) uv1_bad = true;
                }
        }
    }
    if (degenerate) problem(&rep, "%d degenerate triangle(s) - merge by distance / delete loose geometry", degenerate);
    if (r && r->uv1_required && uv1_missing) problem(&rep, "no second UV set (lightmaps) - add a UV map named UVMap.001, unwrapped without overlaps");
    if (uv1_bad) problem(&rep, "second UV set outside 0..1 - pack it into the unit square");
    if (r && r->texel_density > 0 && world_len > 0.01) {
        double density = uv_len / world_len * 1024.0 / 100.0;   // px per cm
        if (fabs(density / r->texel_density - 1.0) > r->texel_tolerance)
            problem(&rep, "texel density %.2f px/cm (kit: %.2f +-%.0f%%) - scale UV0 to world size (1 UV unit = 1 m)", density, r->texel_density, r->texel_tolerance * 100.0f);
    }
    return rep.n;
}
