#include "openworld.h"
#include "openworld_internal.h"
#include "chunks.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/jobs.h"
#include "core/log.h"
#include "core/platform.h"
#include "render/mesh_gen.h"
#include "gameplay/entities.h"
#include "gameplay/events.h"
#include "gameplay/characters.h"
#include "gameplay/zombies/horde.h"
#include "vfx/vfx_types.h"
#include "vfx/vfx_library.h"
#include "gameplay/attachments.h"
#include "cJSON.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OW_TILE 64.0f             // static geometry / collision tile (m): small enough for local-light / cascade caster culling
#define OW_DETAIL_DIST 260.0f     // MPF_DETAIL draws beyond this are skipped

static OpenWorld *ow_of(const World *w) { return w ? (OpenWorld *)w->openworld : NULL; }
bool openworld_active(const World *w) { return ow_of(w) != NULL; }
const MapGen *openworld_gen(const World *w) { return ow_of(w) ? &ow_of(w)->gen : NULL; }
const MapPois *openworld_pois(const World *w) { return ow_of(w) ? &ow_of(w)->pois : NULL; }

float openworld_water_level(const World *w, float x, float z) {
    const OpenWorld *o = ow_of(w);
    return o ? mapgen_water_level(&o->gen, x, z) : -1e9f;
}
float openworld_ground(const World *w, float x, float z) {
    const OpenWorld *o = ow_of(w);
    return o ? mapgen_raster_height(&o->raster, x, z) : 0.0f;
}

static void ow_free(World *w) {
    OpenWorld *o = ow_of(w);
    if (!o) return;
    ow_foliage_free(w, o);
    render_surface_map(NULL, 0, 0, 0, 1);
    mapgen_raster_free(&o->raster);
    mappoi_free(&o->pois);
    mapgen_free(&o->gen);
    free(o->spots);
    free(o);
    w->openworld = NULL;
    horde_set_water(NULL, NULL);
    horde_set_sense(0);
}

// ------------------------------------------------------------------ terrain provider (chunks.c)

static float hash01(int x, int z) {
    u32 h = (u32)x * 73856093u ^ (u32)z * 19349663u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return (float)(h & 0xFFFFu) / 65535.0f;
}
static float vnoise(float x, float z) {
    float fx = floorf(x), fz = floorf(z), tx = x - fx, tz = z - fz;
    tx = tx * tx * (3 - 2 * tx);
    tz = tz * tz * (3 - 2 * tz);
    int ix = (int)fx, iz = (int)fz;
    return lerpf(lerpf(hash01(ix, iz), hash01(ix + 1, iz), tx), lerpf(hash01(ix, iz + 1), hash01(ix + 1, iz + 1), tx), tz);
}

static u32 ground_color(const OpenWorld *o, float x, float z, u8 mat, float slope) {
    float v = vnoise(x * 0.11f, z * 0.11f) * 0.6f + vnoise(x * 0.5f, z * 0.5f) * 0.25f + hash01((int)x, (int)z) * 0.15f;
    // fields: rows across the parcel (crop lines)
    if (mat == MGM_FIELD || mat == MGM_CROP || mat == MGM_PLOWED) v = v * 0.6f + 0.4f * (0.5f + 0.5f * sinf(x * 2.4f));
    vec3 c = mapgen_material_color(mat, v, slope);
    (void)o;
    // rgb: the untextured colour (no surface textures); alpha: the variation the textured terrain
    // (MAT_TERRAIN) applies to the material's base colour
    return rgba8(c.x, c.y, c.z, DG_CLAMP(v, 0.0f, 1.0f));
}

static void prov_generate(void *user, ChunkCoord coord, ChunkData *out) {
    const OpenWorld *o = (const OpenWorld *)user;
    const MgRaster *r = &o->raster;
    out->coord = coord;
    int x0 = (int)lrintf(((float)coord.x * CHUNK_SIZE - r->origin) / r->spacing), z0 = (int)lrintf(((float)coord.z * CHUNK_SIZE - r->origin) / r->spacing);
    for (int z = 0; z < CHUNK_SAMPLES; z++)
        for (int x = 0; x < CHUNK_SAMPLES; x++) {
            int ix = DG_CLAMP(x0 + x, 0, r->n - 1), iz = DG_CLAMP(z0 + z, 0, r->n - 1);
            size_t i = (size_t)iz * r->n + ix;
            out->heights[z * CHUNK_SAMPLES + x] = r->h[i];
            int xl = DG_MAX(ix - 1, 0), xr = DG_MIN(ix + 1, r->n - 1), zd = DG_MAX(iz - 1, 0), zu = DG_MIN(iz + 1, r->n - 1);
            float dx = (r->h[(size_t)iz * r->n + xr] - r->h[(size_t)iz * r->n + xl]) * 0.5f, dz = (r->h[(size_t)zu * r->n + ix] - r->h[(size_t)zd * r->n + ix]) * 0.5f;
            float wx = r->origin + (float)ix * r->spacing, wz = r->origin + (float)iz * r->spacing;
            out->colors[z * CHUNK_SAMPLES + x] = ground_color(o, wx, wz, r->mat[i], sqrtf(dx * dx + dz * dz));
        }
    out->biome = 1;
    out->structure_count = 0;
}

static float prov_height(void *user, float x, float z) { return mapgen_raster_height(&((const OpenWorld *)user)->raster, x, z); }

static void prov_sample(void *user, float x, float z, float *h, u32 *color) {
    const OpenWorld *o = (const OpenWorld *)user;
    *h = mapgen_raster_height(&o->raster, x, z);
    *color = ground_color(o, x, z, mapgen_raster_mat(&o->raster, x, z), mapgen_raster_slope(&o->raster, x, z));
}

// ------------------------------------------------------------------ static geometry

enum { V_PLAIN, V_GLOSS, V_METAL, V_EMISSIVE, V_GLASS, V_MARK, V_DETAIL_PLAIN, V_DETAIL_GLOSS, V_DETAIL_METAL, V_DETAIL_MARK, V_COUNT };   // water: game_water.c / vk_water.c

typedef struct GeoBucket { int tile, variant; MeshBuilder mb; } GeoBucket;
typedef struct CollBucket { int tile, surf; vec3 *v; u32 *ix; int nv, ni, cv, ci; } CollBucket;
typedef struct GeoBuild {
    GeoBucket *g;
    int gn, gcap;
    CollBucket *c;
    int cn, ccap;
    int *gmap, *cmap;            // tile * V_COUNT + variant -> index + 1; tile * SURF_COUNT + surf -> index + 1
    int tiles_side;
    float origin;
    int tris;
} GeoBuild;

static int tile_of(const GeoBuild *b, vec3 p) {
    int tx = DG_CLAMP((int)floorf((p.x - b->origin) / OW_TILE), 0, b->tiles_side - 1), tz = DG_CLAMP((int)floorf((p.z - b->origin) / OW_TILE), 0, b->tiles_side - 1);
    return tz * b->tiles_side + tx;
}

static MeshBuilder *geo_bucket(GeoBuild *b, int tile, int variant) {
    int key = tile * V_COUNT + variant;
    if (b->gmap[key]) return &b->g[b->gmap[key] - 1].mb;
    if (b->gn == b->gcap) {
        b->gcap = b->gcap ? b->gcap * 2 : 256;
        b->g = (GeoBucket *)realloc(b->g, sizeof(GeoBucket) * (size_t)b->gcap);
    }
    GeoBucket *g = &b->g[b->gn++];
    g->tile = tile;
    g->variant = variant;
    mb_init(&g->mb);
    b->gmap[key] = b->gn;
    return &g->mb;
}

static CollBucket *coll_bucket(GeoBuild *b, int tile, int surf) {
    int key = tile * SURF_COUNT + surf;
    if (b->cmap[key]) return &b->c[b->cmap[key] - 1];
    if (b->cn == b->ccap) {
        b->ccap = b->ccap ? b->ccap * 2 : 256;
        b->c = (CollBucket *)realloc(b->c, sizeof(CollBucket) * (size_t)b->ccap);
    }
    CollBucket *c = &b->c[b->cn++];
    memset(c, 0, sizeof(*c));
    c->tile = tile;
    c->surf = surf;
    b->cmap[key] = b->cn;
    return c;
}

// Convex primitive's world-space corners (box 8, wedge 6, cylinder 2 * seg).
static int prim_points(const MpPrim *p, vec3 *out, int seg) {
    mat4 m = mat4_trs(p->pos, p->rot, v3s(1));
    vec3 h = p->half;
    int n = 0;
    if (p->shape == MPS_BOX) {
        for (int i = 0; i < 8; i++) out[n++] = mat4_mul_point(m, v3(i & 1 ? h.x : -h.x, i & 2 ? h.y : -h.y, i & 4 ? h.z : -h.z));
    } else if (p->shape == MPS_WEDGE) {
        for (int i = 0; i < 4; i++) out[n++] = mat4_mul_point(m, v3(i & 1 ? h.x : -h.x, -h.y, i & 2 ? h.z : -h.z));
        out[n++] = mat4_mul_point(m, v3(-h.x, h.y, 0));
        out[n++] = mat4_mul_point(m, v3(h.x, h.y, 0));
    } else {
        float r0 = h.x, r1 = h.x * h.z;
        for (int i = 0; i < seg; i++) {
            float a = (float)i / (float)seg * 6.2831853f;
            out[n++] = mat4_mul_point(m, v3(cosf(a) * r0, -h.y, sinf(a) * r0));
        }
        for (int i = 0; i < seg; i++) {
            float a = (float)i / (float)seg * 6.2831853f;
            out[n++] = mat4_mul_point(m, v3(cosf(a) * r1, h.y, sinf(a) * r1));
        }
    }
    return n;
}

// Faces (as triangle index triples into prim_points), outward winding fixed against the centre.
static int prim_tris(const MpPrim *p, int seg, u32 *tri) {
    int n = 0;
#define T(a, b, c) (tri[n++] = (u32)(a), tri[n++] = (u32)(b), tri[n++] = (u32)(c))
    if (p->shape == MPS_BOX) {
        static const int f[6][4] = { { 0, 2, 3, 1 }, { 4, 5, 7, 6 }, { 0, 1, 5, 4 }, { 2, 6, 7, 3 }, { 0, 4, 6, 2 }, { 1, 3, 7, 5 } };
        for (int i = 0; i < 6; i++) { T(f[i][0], f[i][1], f[i][2]); T(f[i][0], f[i][2], f[i][3]); }
    } else if (p->shape == MPS_WEDGE) {
        T(0, 1, 3); T(0, 3, 2);   // bottom
        T(0, 4, 5); T(0, 5, 1);   // back slope (-z)
        T(2, 3, 5); T(2, 5, 4);   // front slope (+z)
        T(0, 2, 4); T(1, 5, 3);   // gables
    } else {
        for (int i = 0; i < seg; i++) {
            int j = (i + 1) % seg;
            T(i, j, seg + j);
            T(i, seg + j, seg + i);
        }
        for (int i = 1; i + 1 < seg; i++) { T(0, i, i + 1); T(seg, seg + i + 1, seg + i); }
    }
#undef T
    return n;
}

static void fix_winding(const vec3 *pts, int np, u32 *tri, int ni) {
    vec3 c = v3(0, 0, 0);
    for (int i = 0; i < np; i++) c = v3_add(c, pts[i]);
    c = v3_scale(c, 1.0f / (float)np);
    for (int t = 0; t < ni; t += 3) {
        vec3 a = pts[tri[t]], b = pts[tri[t + 1]], d = pts[tri[t + 2]];
        vec3 nrm = v3_cross(v3_sub(b, a), v3_sub(d, a));
        vec3 mid = v3_scale(v3_add(v3_add(a, b), d), 1.0f / 3.0f);
        if (v3_dot(nrm, v3_sub(mid, c)) < 0) { u32 s = tri[t + 1]; tri[t + 1] = tri[t + 2]; tri[t + 2] = s; }
    }
}

// Surface texture layer of a prim (render.h SurfTexLayer, MAT_LAYERED draws) by shape and surface:
// pitched roofs shingles, walls brick / plaster / wood, corrugated sheet metal, rusty steel,
// concrete. Glass, lamps and glossy paint (car bodies) stay plain.
static u32 prim_layer(const MpPrim *p) {
    if (p->flags & (MPF_GLASS | MPF_EMISSIVE | MPF_GLOSS)) return STEX_NONE;
    const char *s = p->surf ? p->surf : "";
    if (p->shape == MPS_WEDGE && !strcmp(s, "wood")) return STEX_SHINGLES;
    if (!strcmp(s, "brick")) return STEX_BRICK;
    if (!strcmp(s, "plaster")) return STEX_PLASTER;
    if (!strcmp(s, "wood")) return STEX_WOOD;
    if (!strcmp(s, "metal_thin")) return STEX_METAL;
    if (!strcmp(s, "metal_thick")) return STEX_RUST;
    if (!strcmp(s, "asphalt")) return STEX_ASPHALT;
    if (!strcmp(s, "gravel")) return STEX_GRAVEL;
    if (!strcmp(s, "concrete") || !s[0]) return STEX_CONCRETE;
    return STEX_NONE;   // cloth, glass, ...
}

// Face uv in metres (MAT_LAYERED): u along the face, v up it (walls: world height, so courses and
// boards continue across wall pieces), along the prim's x on horizontal faces.
static vec2 face_uv_m(vec3 wp, vec3 n, quat rot) {
    vec3 t = fabsf(n.y) > 0.98f ? quat_rotate(rot, v3(1, 0, 0)) : v3_norm(v3_cross(v3(0, 1, 0), n));
    vec3 bt = v3_cross(n, t);
    return v2(v3_dot(wp, t), v3_dot(wp, bt));
}

static void emit_prim(GeoBuild *b, const MpPrim *p) {
    if (p->flags & MPF_FOOTPRINT) return;
    int seg = (p->flags & MPF_DETAIL) || p->half.x < 0.5f ? 8 : 14;
    vec3 pts[32];
    u32 tri[32 * 6 * 3];
    int np = prim_points(p, pts, seg);
    int ni = prim_tris(p, seg, tri);
    fix_winding(pts, np, tri, ni);
    int tile = tile_of(b, p->pos);
    if (!(p->flags & MPF_INVISIBLE)) {
        bool detail = (p->flags & MPF_DETAIL) != 0;
        int v = V_PLAIN;
        if (p->flags & MPF_GLASS) v = V_GLASS;
        else if (p->flags & MPF_EMISSIVE) v = V_EMISSIVE;
        else if (p->flags & MPF_METAL) v = detail ? V_DETAIL_METAL : V_METAL;
        else if (p->flags & MPF_GLOSS) v = detail ? V_DETAIL_GLOSS : V_GLOSS;
        else if (p->flags & MPF_NO_SHADOW) v = detail ? V_DETAIL_MARK : V_MARK;
        else v = detail ? V_DETAIL_PLAIN : V_PLAIN;
        MeshBuilder *mb = geo_bucket(b, tile, v);
        u32 col = (p->color & 0x00FFFFFFu) | prim_layer(p) << 24;   // alpha: the surface layer
        if (p->shape == MPS_BOX) {
            u32 v0 = mb->vcount;
            mb_box_transformed(mb, mat4_trs(p->pos, p->rot, v3s(1)), p->half, col);
            for (u32 i = v0; i < mb->vcount; i++) mb->verts[i].uv = face_uv_m(mb->verts[i].pos, mb->verts[i].normal, p->rot);
        } else {
            // flat-shaded faces (cylinders: smooth sides)
            vec3 c = v3(0, 0, 0);
            for (int i = 0; i < np; i++) c = v3_add(c, pts[i]);
            c = v3_scale(c, 1.0f / (float)np);
            vec3 axis = quat_rotate(p->rot, v3(0, 1, 0));
            for (int t = 0; t < ni; t += 3) {
                vec3 a = pts[tri[t]], bb = pts[tri[t + 1]], d = pts[tri[t + 2]];
                vec3 fn = v3_norm(v3_cross(v3_sub(bb, a), v3_sub(d, a)));
                vec3 nn[3] = { fn, fn, fn };
                if (p->shape == MPS_CYL && fabsf(v3_dot(fn, axis)) < 0.9f) {
                    for (int k = 0; k < 3; k++) {
                        vec3 q = pts[tri[t + k]], rel = v3_sub(q, p->pos);
                        vec3 radial = v3_sub(rel, v3_scale(axis, v3_dot(rel, axis)));
                        nn[k] = v3_norm(radial);
                    }
                }
                u32 i0 = mb_vertex(mb, a, nn[0], face_uv_m(a, fn, p->rot), col);
                u32 i1 = mb_vertex(mb, bb, nn[1], face_uv_m(bb, fn, p->rot), col);
                u32 i2 = mb_vertex(mb, d, nn[2], face_uv_m(d, fn, p->rot), col);
                mb_tri(mb, i0, i1, i2);
            }
            (void)c;
        }
        b->tris += p->shape == MPS_BOX ? 12 : ni / 3;
    }
    if (p->flags & MPF_COLLIDE) {
        int surf = surface_from_name(p->surf);
        if (surf < 0) surf = SURF_CONCRETE;
        CollBucket *c = coll_bucket(b, tile, surf);
        if (c->nv + np > c->cv) { c->cv = DG_MAX(c->cv * 2, c->nv + np + 1024); c->v = (vec3 *)realloc(c->v, sizeof(vec3) * (size_t)c->cv); }
        if (c->ni + ni > c->ci) { c->ci = DG_MAX(c->ci * 2, c->ni + ni + 4096); c->ix = (u32 *)realloc(c->ix, sizeof(u32) * (size_t)c->ci); }
        for (int i = 0; i < ni; i++) c->ix[c->ni++] = (u32)c->nv + tri[i];
        memcpy(c->v + c->nv, pts, sizeof(vec3) * (size_t)np);
        c->nv += np;
    }
}

// Road surfaces: ribbons draped on the terrain (crisp edges where the 1 m vertex colours blur), a few
// cm above it, per tile; asphalt with worn patches, dirt, runway concrete. Bridge decks cover the
// water spans.
static void build_road_ribbons(GeoBuild *b, const OpenWorld *o) {
    const MapGen *g = &o->gen;
    for (int r = 0; r < g->road_count; r++) {
        const MgRoad *rd = &g->roads[r];
        bool dirt = rd->cls == MGR_DIRT || rd->cls == MGR_TRAIL, runway = rd->cls == MGR_RUNWAY;
        u32 layer = dirt ? STEX_DIRT_ROAD : (runway ? STEX_CONCRETE : STEX_ASPHALT);
        float lift = dirt ? 0.035f : (runway ? 0.045f : 0.05f);
        float hw = rd->width * 0.5f;
        MeshBuilder *mb = NULL;
        int tile = -1;
        u32 prev[3] = { 0, 0, 0 };
        bool have_prev = false;
        for (int i = 0; i < rd->count; i++) {
            const MgRoadSample *s = &g->rs[rd->first + i];
            if (s->bridge) { have_prev = false; continue; }
            int t = tile_of(b, v3(s->p.x, 0, s->p.y));
            if (t != tile) {   // a new tile: restart the strip there (duplicating the joint row)
                tile = t;
                mb = geo_bucket(b, t, V_MARK);
                if (have_prev && i > 0) {
                    const MgRoadSample *q = &g->rs[rd->first + i - 1];
                    vec2 qn = v2(q->dir.y, -q->dir.x);
                    for (int k = 0; k < 3; k++) {
                        float off = (float)(k - 1) * hw;
                        vec2 p = v2_add(q->p, v2_scale(qn, off));
                        prev[k] = mb_vertex(mb, v3(p.x, mapgen_raster_height(&o->raster, p.x, p.y) + lift, p.y), v3(0, 1, 0), v2(p.x, p.y), 0);
                    }
                }
            }
            vec2 nrm = v2(s->dir.y, -s->dir.x);
            u32 cur[3];
            for (int k = 0; k < 3; k++) {
                float off = (float)(k - 1) * hw;
                vec2 p = v2_add(s->p, v2_scale(nrm, off));
                float v = vnoise(p.x * 0.17f, p.y * 0.17f) * 0.7f + vnoise(p.x * 0.9f, p.y * 0.9f) * 0.3f;
                vec3 c;
                if (dirt) c = v3_scale(v3(0.31f, 0.245f, 0.165f), 0.85f + 0.3f * v);
                else if (runway) c = v3_scale(v3(0.31f, 0.31f, 0.29f), 0.85f + 0.25f * v);
                else c = v3_scale(v3(0.075f, 0.075f, 0.08f), 0.8f + 0.5f * v + (v > 0.72f ? 0.6f : 0.0f));   // worn / patched spots
                u32 rc = (rgba8(c.x, c.y, c.z, 1) & 0x00FFFFFFu) | layer << 24;
                cur[k] = mb_vertex(mb, v3(p.x, mapgen_raster_height(&o->raster, p.x, p.y) + lift, p.y), v3(0, 1, 0), v2(p.x, p.y), rc);
            }
            // fix the colour of a joint row created above (it was written black)
            if (have_prev && mb->verts[prev[0]].color == 0)
                for (int k = 0; k < 3; k++) mb->verts[prev[k]].color = mb->verts[cur[k]].color;
            if (have_prev)
                for (int k = 0; k < 2; k++) {
                    mb_tri(mb, prev[k], cur[k], cur[k + 1]);
                    mb_tri(mb, prev[k], cur[k + 1], prev[k + 1]);
                }
            memcpy(prev, cur, sizeof(prev));
            have_prev = true;
            b->tris += 4;
        }
    }
}

static void add_body(World *w, BodyDesc *d) {
    LevelStatic *ls = &w->level;
    if (ls->body_count >= MAX_STATIC_BODIES) { log_warn("openworld: too many static bodies"); return; }
    BodyId id = phys_add_body(w->phys, d);
    if (id != PHYS_INVALID_BODY) ls->bodies[ls->body_count++] = id;
}

static void add_draw(World *w, MeshBuilder *mb, int variant, vec3 center, float cull) {
    LevelStatic *ls = &w->level;
    if (!mb->icount) return;
    if (ls->draw_count >= MAX_STATIC_DRAWS) { log_warn("openworld: too many static draws"); return; }
    StaticDraw *d = &ls->draws[ls->draw_count++];
    d->mesh = mb_upload(mb);
    d->mat = material_color(v4(1, 1, 1, 1));
    d->mat.roughness = 0.85f;
    switch (variant) {
    case V_GLOSS: case V_DETAIL_GLOSS: d->mat.roughness = 0.22f; break;
    case V_METAL: case V_DETAIL_METAL: d->mat.roughness = 0.5f; d->mat.metallic = 0.55f; break;
    case V_EMISSIVE: d->mat.emissive = 2.5f; d->mat.flags = MAT_NO_SHADOW; break;
    case V_GLASS: d->mat.color.w = 0.55f; d->mat.roughness = 0.15f; d->mat.flags = MAT_NO_SHADOW; break;
    case V_MARK: case V_DETAIL_MARK: d->mat.roughness = 0.9f; d->mat.flags = MAT_NO_SHADOW; break;
    default: break;
    }
    if (variant != V_EMISSIVE && variant != V_GLASS) d->mat.flags |= MAT_LAYERED;   // vertex alpha = surface layer
    d->model = mat4_identity();
    d->cull_center = center;
    d->cull_dist = cull;
}

static void build_geometry(World *w, OpenWorld *o) {
    GeoBuild b;
    memset(&b, 0, sizeof(b));
    b.origin = -o->gen.raster_half;
    b.tiles_side = (int)ceilf(2.0f * o->gen.raster_half / OW_TILE);
    int tiles = b.tiles_side * b.tiles_side;
    b.gmap = (int *)calloc((size_t)tiles * V_COUNT, sizeof(int));
    b.cmap = (int *)calloc((size_t)tiles * SURF_COUNT, sizeof(int));
    for (int i = 0; i < o->pois.prim_count; i++) emit_prim(&b, &o->pois.prims[i]);
    build_road_ribbons(&b, o);
    int draws0 = w->level.draw_count;
    for (int i = 0; i < b.gn; i++) {
        GeoBucket *g = &b.g[i];
        int tx = g->tile % b.tiles_side, tz = g->tile / b.tiles_side;
        vec3 c = v3(b.origin + ((float)tx + 0.5f) * OW_TILE, 0, b.origin + ((float)tz + 0.5f) * OW_TILE);
        bool detail = g->variant >= V_DETAIL_PLAIN && g->variant <= V_DETAIL_MARK;
        add_draw(w, &g->mb, g->variant, c, detail ? OW_DETAIL_DIST + OW_TILE * 0.71f : 0);
        mb_free(&g->mb);
    }
    int bodies0 = w->level.body_count;
    for (int i = 0; i < b.cn; i++) {
        CollBucket *c = &b.c[i];
        BodyDesc d;
        memset(&d, 0, sizeof(d));
        d.shape.kind = SHAPE_MESH;
        d.shape.verts = c->v;
        d.shape.vert_count = (u32)c->nv;
        d.shape.indices = c->ix;
        d.shape.index_count = (u32)c->ni;
        d.rot = quat_identity();
        d.motion = PM_STATIC;
        d.layer = PL_STATIC;
        d.friction = 0.8f;
        d.material = (u8)c->surf;
        add_body(w, &d);
        free(c->v);
        free(c->ix);
    }
    log_info("openworld: %d prims -> %d draws (%d tris), %d collision meshes", o->pois.prim_count, w->level.draw_count - draws0, b.tris, w->level.body_count - bodies0);
    free(b.g);
    free(b.c);
    free(b.gmap);
    free(b.cmap);
}

static void build_terrain_collision(World *w, OpenWorld *o) {
    const MgRaster *r = &o->raster;
    int per = (int)OW_TILE, n = per + 1;
    int tiles = (r->n - 1) / per;
    float *hb = (float *)malloc(sizeof(float) * (size_t)n * n);
    for (int tz = 0; tz < tiles; tz++)
        for (int tx = 0; tx < tiles; tx++) {
            for (int z = 0; z < n; z++)
                for (int x = 0; x < n; x++) hb[z * n + x] = r->h[(size_t)(tz * per + z) * r->n + (size_t)(tx * per + x)];
            BodyDesc d;
            memset(&d, 0, sizeof(d));
            d.shape.kind = SHAPE_HEIGHTFIELD;
            d.shape.heights = hb;
            d.shape.sample_count = (u32)n;
            d.shape.spacing = r->spacing;
            d.pos = v3(r->origin + (float)(tx * per) * r->spacing, 0, r->origin + (float)(tz * per) * r->spacing);
            d.rot = quat_identity();
            d.motion = PM_STATIC;
            d.layer = PL_STATIC;
            d.friction = 0.9f;
            d.material = SURF_DIRT;
            add_body(w, &d);
        }
    free(hb);
    // the map edge: tall invisible walls
    float hs = o->gen.half_size;
    for (int e = 0; e < 4; e++) {
        BodyDesc d;
        memset(&d, 0, sizeof(d));
        d.shape.kind = SHAPE_BOX;
        d.shape.half = e < 2 ? v3(2, 300, hs + 4) : v3(hs + 4, 300, 2);
        d.pos = e == 0 ? v3(-hs - 2, 100, 0) : e == 1 ? v3(hs + 2, 100, 0) : e == 2 ? v3(0, 100, -hs - 2) : v3(0, 100, hs + 2);
        d.rot = quat_identity();
        d.motion = PM_STATIC;
        d.layer = PL_STATIC;
        d.friction = 0.1f;
        add_body(w, &d);
    }
}

// ------------------------------------------------------------------ entities

static void fill_container(World *w, OpenWorld *o, EntityId id, const char *tier, u32 seed) {
    LootItem items[CONTAINER_SLOTS];
    int n = loot_roll(&o->loot, tier, seed, items, CONTAINER_SLOTS);
    int slot = 0;
    for (int i = 0; i < n && slot < CONTAINER_SLOTS; i++) {
        u16 item = item_find(w->items, items[i].item);
        if (!item) continue;
        const WeaponDef *wd = item_weapon(w->items, item);
        InvSlot *s = &w->cont[ENT_INDEX(id)].slots[slot++];
        s->item = item;
        int stack = w->items->items[item].max_stack > 0 ? w->items->items[item].max_stack : 1;
        s->count = (u16)DG_CLAMP(items[i].count, 1, stack);
        s->ammo = wd ? (i16)wd->mag_size : 0;
        attach_defaults(wd, s->att);
    }
}

static void spawn_entities(World *w, OpenWorld *o) {
    const MapPois *p = &o->pois;
    int counts[MPE_KIND_COUNT] = { 0 };
    LevelStatic *ls = &w->level;
    // indoor zombie spots per POI (the spawner's indoor share)
    o->spots = (OwSpot *)calloc((size_t)p->ent_count + 1, sizeof(OwSpot));
    o->spot_count = 0;
    for (int i = 0; i < p->ent_count; i++) {
        const MpEnt *e = &p->ents[i];
        counts[e->kind]++;
        if (e->kind == MPE_ZOMBIE_SPOT) { o->spots[o->spot_count++] = (OwSpot){ e->pos, e->poi }; continue; }
        if (e->kind == MPE_SPAWN) {
            if (ls->spawn_count < MAX_SPAWNS) ls->spawns[ls->spawn_count++] = (SpawnPoint){ e->pos, e->yaw };
            continue;
        }
        if (!w->authority) continue;
        EntityId id = ENT_NONE;
        switch (e->kind) {
        case MPE_CONTAINER:
            id = spawn_container(w, e->pos, e->yaw);
            if (id) fill_container(w, o, id, e->tier, (u32)i * 2654435761u ^ o->gen.seed);
            break;
        case MPE_PICKUP: {
            u16 item = item_find(w->items, e->item);
            if (!item) { log_warn("openworld: unknown pickup item '%s'", e->item); break; }
            const WeaponDef *wd = item_weapon(w->items, item);
            id = spawn_pickup(w, e->pos, item, DG_MAX(e->count, 1), wd ? (i16)wd->mag_size : 0, 0);
            break;
        }
        case MPE_DOOR: {
            u16 key = item_find(w->items, e->item);
            id = spawn_door(w, e->pos, e->yaw, e->w, e->h, e->locked, key, rgba8(0.4f, 0.32f, 0.22f, 1));
            break;
        }
        case MPE_LIGHT:
            id = spawn_light(w, e->pos, e->color, e->radius, e->intensity, e->shadow ? 0 : LIGHT_GROUP_NO_SHADOW, true);
            if (id && e->scheduled) {
                LightComp *lc = &w->light[ENT_INDEX(id)];
                lc->scheduled = true;
                lc->sched_state = false;
                w->flags[ENT_INDEX(id)] &= ~(u32)EF_ON;
            } else if (id) {
                w->light[ENT_INDEX(id)].flicker = 0.25f;   // fires
            }
            break;
        default: break;   // vehicles / pumps: game_vehicles.c after its library loads
        }
        (void)id;
    }
    log_info("openworld: %d containers, %d pickups, %d doors, %d lights, %d vehicles, %d pumps, %d zombie spots, %d spawns", counts[MPE_CONTAINER], counts[MPE_PICKUP],
             counts[MPE_DOOR], counts[MPE_LIGHT], counts[MPE_VEHICLE], counts[MPE_PUMP], counts[MPE_ZOMBIE_SPOT], counts[MPE_SPAWN]);
}

// ------------------------------------------------------------------ load

typedef struct RasterJob { const MapGen *g; MgRaster *r; bool slopes; } RasterJob;
static void raster_range(void *ctx, int begin, int end) {
    RasterJob *j = (RasterJob *)ctx;
    if (j->slopes) mapgen_raster_slopes(j->g, j->r, begin, end);
    else mapgen_raster_rows(j->g, j->r, begin, end);
}

static float jnum(const cJSON *o, const char *k, float def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}

bool openworld_load(World *w, const cJSON *root) {
    const cJSON *world = cJSON_GetObjectItemCaseSensitive(root, "world");
    if (!world) return false;
    u64 t0 = SDL_GetTicksNS();
    OpenWorld *o = (OpenWorld *)calloc(1, sizeof(OpenWorld));
    w->openworld = o;
    w->openworld_free = ow_free;
    const cJSON *seed = cJSON_GetObjectItemCaseSensitive(world, "seed");
    mapgen_init(&o->gen, cJSON_IsNumber(seed) ? (u32)seed->valueint : 1u);
    char err[256];
    if (!mapgen_parse(&o->gen, world, err, sizeof(err))) { log_error("openworld: %s", err); return false; }
    if (!mappoi_plan(&o->pois, &o->gen, cJSON_GetObjectItemCaseSensitive(root, "pois"), err, sizeof(err))) { log_error("openworld: %s", err); return false; }
    mapgen_finalize(&o->gen);
    u64 t1 = SDL_GetTicksNS();
    if (!mapgen_raster_alloc(&o->raster, o->gen.raster_half, 1.0f)) { log_error("openworld: out of memory (raster)"); return false; }
    RasterJob rj = { &o->gen, &o->raster, false };
    jobs_parallel_for(o->raster.n, 16, raster_range, &rj);
    rj.slopes = true;
    jobs_parallel_for(o->raster.n, 32, raster_range, &rj);
    u64 t2 = SDL_GetTicksNS();
    mappoi_build(&o->pois, &o->gen, &o->raster);
    u64 t3 = SDL_GetTicksNS();
    // gameplay tuning
    const cJSON *z = cJSON_GetObjectItemCaseSensitive(world, "zombies");
    o->wild_density = jnum(z, "wild_density", 0.6f);
    o->wild_max = (int)jnum(z, "wild_max", 4);
    o->zombie_cap = (int)jnum(z, "cap", 120);
    o->despawn_dist = jnum(z, "despawn", 190);
    o->spawn_min = jnum(z, "spawn_min", 40);
    o->spawn_max = jnum(z, "spawn_max", 95);
    o->sense = jnum(z, "sense", 38);
    char path[512], lerr[128];
    char *lt = platform_read_file(platform_data_path("data/loot.json", path, sizeof(path)), NULL);
    if (!loot_parse(&o->loot, lt, lerr, sizeof(lerr))) log_warn("openworld: %s (containers stay empty)", lerr);
    free(lt);

    build_terrain_collision(w, o);
    build_geometry(w, o);
    spawn_entities(w, o);
    ow_foliage_load(w, o);
    u64 t4 = SDL_GetTicksNS();
    ChunkProvider prov;
    memset(&prov, 0, sizeof(prov));
    prov.name = "openworld";
    prov.user = o;
    prov.generate = prov_generate;
    prov.height_at = prov_height;
    prov.sample = prov_sample;
    prov.bounds = o->gen.raster_half;
    prov.static_collision = true;
    prov.no_grid = true;
    prov.surface_textures = true;
    prov.foliage_chunk = ow_foliage_chunk;
    // terrain surface textures: the layers + this map's material raster (MgMaterial = SurfTexLayer ids)
    render_surfaces_load();
    render_surface_map(o->raster.mat, o->raster.n, o->raster.origin, o->raster.origin, o->raster.spacing);
    chunks_create_custom(w, &prov);
    horde_set_water(ow_water_fn, o);
    horde_set_sense(o->sense);
    u64 t5 = SDL_GetTicksNS();
    log_info("openworld: %d POIs (%d enterable buildings), %d roads, %d bridges; plan %.0f ms, raster %.0f ms, build %.0f ms, upload %.0f ms, chunks %.0f ms",
             o->pois.poi_count, o->pois.enterable_count, o->gen.road_count, o->gen.bridge_count, (double)(t1 - t0) / 1e6, (double)(t2 - t1) / 1e6, (double)(t3 - t2) / 1e6,
             (double)(t4 - t3) / 1e6, (double)(t5 - t4) / 1e6);
    return true;
}

// ------------------------------------------------------------------ host rules: zombies (water: game_water.c, swimming)

float ow_water_fn(void *user, float x, float z) { return mapgen_water_level(&((OpenWorld *)user)->gen, x, z); }

static float poi_density(const OpenWorld *o, int poi, float *indoor, int *max) {
    if (poi < 0) { *indoor = 0; *max = o->wild_max; return o->wild_density; }
    const MpPoi *pp = &o->pois.pois[poi];
    *indoor = pp->indoor;
    *max = pp->zombie_max;
    return pp->zombie_density;
}

static void zombie_spawner(World *w, OpenWorld *o) {
    vec3 ppos[MAX_PLAYERS];
    int np = 0;
    for (int s = 0; s < MAX_PLAYERS; s++) {
        Player *p = &w->players[s];
        if (p->active && p->sim.move.mode != MOVE_DEAD) ppos[np++] = p->sim.move.pos;
    }
    if (!np) return;
    // count / cull
    int total = 0, near[MAX_PLAYERS] = { 0 };
    ENT_FOREACH(w, i) {
        if (w->type[i] != ET_ZOMBIE) continue;
        vec3 zp = w->xf[i].pos;
        float best = 1e9f;
        for (int k = 0; k < np; k++) {
            float d = v3_len(v3_flat(v3_sub(zp, ppos[k])));
            best = fminf(best, d);
            if (d < 110.0f && !(w->flags[i] & EF_DEAD)) near[k]++;
        }
        if (best > o->despawn_dist) { ent_destroy(w, ENT_MAKE(i, w->gen[i])); o->despawned++; continue; }
        total++;
    }
    for (int k = 0; k < np && total < o->zombie_cap; k++) {
        int poi = mappoi_find(&o->pois, ppos[k].x, ppos[k].z);
        float indoor;
        int max;
        float dens = poi_density(o, poi, &indoor, &max);
        int target = DG_MIN(max, (int)(dens * 3.8f + 0.5f));
        int deficit = DG_MIN(target - near[k], 3);
        for (int n = 0; n < deficit && total < o->zombie_cap; n++) {
            o->rng = o->rng * 1664525u + 1013904223u;
            float r0 = (float)(o->rng >> 8) / 16777216.0f;
            bool spawned = false;
            if (r0 < indoor && o->spot_count) {
                // an indoor spot of this POI at a fair distance (try a few)
                for (int t = 0; t < 12 && !spawned; t++) {
                    o->rng = o->rng * 1664525u + 1013904223u;
                    const OwSpot *sp = &o->spots[(o->rng >> 8) % (u32)o->spot_count];
                    if (sp->poi != poi + 1) continue;
                    float d = v3_len(v3_sub(sp->pos, ppos[k]));
                    if (d < 18.0f || d > 100.0f) continue;
                    spawned = horde_spawn_at(w, sp->pos, (float)(o->rng & 1023) * 0.006f, "mixed");
                }
            }
            // outdoors: a ring point in the open (streets, yards, fields: not in a building
            // footprint, where the drop from above would land on the roof, and not in water)
            for (int t = 0; t < 6 && !spawned; t++) {
                o->rng = o->rng * 1664525u + 1013904223u;
                float a = (float)(o->rng >> 8) / 16777216.0f * 6.2831853f;
                o->rng = o->rng * 1664525u + 1013904223u;
                float dist = lerpf(o->spawn_min, o->spawn_max, (float)(o->rng >> 8) / 16777216.0f);
                float x = ppos[k].x + cosf(a) * dist, zz = ppos[k].z + sinf(a) * dist;
                if (fabsf(x) > o->gen.half_size - 5 || fabsf(zz) > o->gen.half_size - 5) continue;
                float gy = mapgen_raster_height(&o->raster, x, zz);
                if (mapgen_water_level(&o->gen, x, zz) > gy - 0.3f || mapgen_in_keepout(&o->gen, x, zz, 1.0f)) continue;
                spawned = horde_spawn(w, v3(x, gy, zz), 1, 2.0f, HSPAWN_RANDOM, "mixed") > 0;
            }
            if (spawned) { total++; o->spawned++; }
        }
    }
}

void openworld_tick(World *w, float dt) {
    OpenWorld *o = ow_of(w);
    if (!o || !w->authority) return;
    o->spawn_timer -= dt;
    if (o->spawn_timer <= 0) {
        o->spawn_timer = 1.0f;
        Cvar *cv = cvar_find("ow_zombies");
        if (!cv || cv->i) zombie_spawner(w, o);
    }
}

void openworld_frame(World *w, vec3 cam_pos) {
    OpenWorld *o = ow_of(w);
    if (o) ow_foliage_frame(w, o, cam_pos);
}

// ------------------------------------------------------------------ commands

#include "game/game.h"

static void cmd_poi_list(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    const World *w = game()->world;
    const OpenWorld *o = ow_of(w);
    if (!o) { log_info("poi_list: not an open-world map"); return; }
    for (int i = 0; i < o->pois.poi_count; i++) {
        const MpPoi *p = &o->pois.pois[i];
        log_info("  %-10s %-16s (%6.0f, %6.0f) %4.0f x %4.0f m  zombies %.1f (max %d, indoor %.0f%%)  loot %-10s enterable %d", p->id, p->name, p->center.x, p->center.y,
                 p->half.x * 2, p->half.y * 2, p->zombie_density, p->zombie_max, p->indoor * 100.0f, p->loot, p->enterable);
    }
}

static void cmd_poi_goto(int argc, const char **argv) {
    const World *w = game()->world;
    const OpenWorld *o = ow_of(w);
    if (!o || argc < 2) { log_info("usage: poi_goto <id> [height above ground]"); return; }
    for (int i = 0; i < o->pois.poi_count; i++) {
        const MpPoi *p = &o->pois.pois[i];
        if (strcmp(p->id, argv[1])) continue;
        float up = argc > 2 ? (float)atof(argv[2]) : 2.0f;
        char line[128];
        snprintf(line, sizeof(line), "setpos %.1f %.1f %.1f", p->center.x, mapgen_raster_height(&o->raster, p->center.x, p->center.y) + up, p->center.y);
        cmd_execute_line(line);
        return;
    }
    log_warn("poi_goto: no POI '%s'", argv[1]);
}

static void cmd_ow_stats(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    const World *w = game()->world;
    const OpenWorld *o = ow_of(w);
    if (!o) { log_info("ow_stats: not an open-world map"); return; }
    int zombies = 0;
    ENT_FOREACH(w, i) if (w->type[i] == ET_ZOMBIE) zombies++;
    int aggro = horde_aggro_count();
    log_info("openworld: %d zombies (%d chasing; spawned %d, culled %d), %d chunks loaded, %d static draws, %d bodies, %d foliage chunks (%d trees), frame %.2f ms",
             zombies, aggro, o->spawned, o->despawned, chunks_loaded_count(w), w->level.draw_count, w->level.body_count, o->fol_loaded, o->tree_instances,
             g_engine.fps_smoothed > 0 ? 1000.0 / g_engine.fps_smoothed : 0.0);
}

static void cmd_ow_where(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    const World *w = game()->world;
    const OpenWorld *o = ow_of(w);
    Player *p = game_local_player();
    if (!o || !p) return;
    vec3 pos = p->sim.move.pos;
    int poi = mappoi_find(&o->pois, pos.x, pos.z);
    log_info("at (%.0f, %.1f, %.0f): ground %.1f, water %.1f, material %s, POI %s", pos.x, pos.y, pos.z, mapgen_raster_height(&o->raster, pos.x, pos.z),
             mapgen_water_level(&o->gen, pos.x, pos.z), mapgen_material_name(mapgen_raster_mat(&o->raster, pos.x, pos.z)), poi >= 0 ? o->pois.pois[poi].name : "-");
}

static void cmd_ow_zombies(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    World *w = game()->world;
    Player *p = game_local_player();
    if (!w || !p) return;
    int n = 0;
    ENT_FOREACH(w, i) {
        if (w->type[i] != ET_ZOMBIE || n >= 40) continue;
        vec3 z = w->xf[i].pos;
        log_info("  zombie %4d at (%7.1f, %5.1f, %7.1f) %5.1f m%s", i, z.x, z.y, z.z, v3_len(v3_sub(z, p->sim.move.pos)), (w->flags[i] & EF_DEAD) ? " dead" : "");
        n++;
    }
}

void openworld_register_commands(void) {
    cmd_register("ow_zombie_list", cmd_ow_zombies, "open-world map: the zombies near the player (debug)");
    cvar_reg_bool("ow_zombies", true, 0, "open-world maps: the POI zombie spawner keeps zombies around the players");
    cmd_register("poi_list", cmd_poi_list, "open-world map: list the points of interest");
    cmd_register("poi_goto", cmd_poi_goto, "open-world map: teleport to a POI (poi_goto <id> [height])");
    cmd_register("ow_stats", cmd_ow_stats, "open-world map: zombies, chunks, draws, bodies");
    cmd_register("ow_where", cmd_ow_where, "open-world map: ground, water, material and POI at the player");
}
