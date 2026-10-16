// Virtualized geometry foliage support tests (Phase 7: src/render/vgeo/foliage_support.c, the masked /
// two-sided / WPO / impostor paths of shaders/vgeo, docs/VGEO.md "Foliage support"), on the window-less
// harness, with the procedural test content (grass tuft, bush: vgeo_procgen_foliage.h):
//   - masked visibility: all-HW vs all-SW (and mesh shader vs vertex pulling) on a masked, two-sided,
//     wind-displaced grass patch within the edge tolerance of test_vgeo_raster.c; plus a CPU
//     alpha-tested reference on sample pixels (the masked winner passes the CPU alpha test, a pixel
//     whose unmasked winner fails it shows another surface, masking never brings a surface nearer)
//   - WPO bounds: every displaced vertex of every cluster of every level lies inside its cluster's and
//     its instance's culling spheres at and beyond the declared max wind, with the displacement
//     buffer saturated; the GPU snippet equals the CPU mirror
//   - density hash: the kept instance set of a static camera is identical over 600 frames, equals the
//     CPU mirror, and shrinks monotonically with r_foliage_density
//   - cell culling: GPU visible instances vs a brute-force frustum + fade test over every instance
//   - impostor switch: every instance draws its impostor xor its DAG cut, exactly where the CPU
//     comparison says (no gap, no overlap)
//   - placement streaming: a 2000 m flythrough loading / unloading .vgp chunks: allocator audit every
//     step, GPU tables = CPU mirror, GPU instance counts = CPU, nothing leaked at the end
//   - Hi-Z: >= 90 % of the in-frustum instances behind a wall are rejected (counters)
//   - --validation: zero validation messages
#include "world/wind.h"
#include "vgeo_gpu_harness.h"
#include "render/vgeo/vgeo_raster.h"
#include "render/vgeo/foliage_support.h"
#include "render/vgeo/vgeo_impostor.h"
#include "render/vgeo/vgeo_skin.h"
#include "render/vgeo/vgeo_procgen_foliage.h"
#include "core/engine.h"
#include "core/dgmath.h"
#include <SDL3/SDL.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Engine g_engine;
static int g_fail, g_checks;
#define CHECK(cond, ...)                                        \
    do {                                                        \
        g_checks++;                                             \
        if (!(cond)) {                                          \
            g_fail++;                                           \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);         \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
        }                                                       \
    } while (0)

#define W 1920
#define H 1080
#define ZNEAR 0.1f
#define FOV (60.0f * DG_DEG2RAD)
#define SW_ALL 1e9f
#define MASK_SIZE 256

static VgHarness g_h;
static VgScene g_scene;
static VgCuller g_cull;
static VgRaster g_rast;
static VgBuilt g_built[VG_FOLIAGE_KINDS];
static u32 g_mesh[VG_FOLIAGE_KINDS];
static u8 *g_mask_mips[VG_FOLIAGE_KINDS][2];   // CPU copies of the mask textures (all mips)
static u32 g_mask_levels;
static u32 g_mask_tex[VG_FOLIAGE_KINDS][2];
static VgMaterial g_mats[VG_FOLIAGE_KINDS][2];
static u32 g_mat_base[VG_FOLIAGE_KINDS];

// ------------------------------------------------------------------ views

typedef struct Cam { vec3 eye, target; } Cam;
static float g_wind[4] = { 4.8f, 3.6f, 4.8f, 3.6f }, g_time = 3.7f, g_density = 1.0f, g_impostor_px = 0.0f;

static void view_for(const Cam *c, u32 flags, float sw_px, VgViewGpu *v) {
    mat4 view = mat4_look_at(c->eye, c->target, v3(0, 1, 0));
    mat4 proj = mat4_perspective(FOV, (float)W / (float)H, ZNEAR, 4000.0f);
    mat4 vp = mat4_mul(proj, view);
    float ps = (float)H / (2.0f * tanf(FOV * 0.5f));
    float eye[3] = { c->eye.x, c->eye.y, c->eye.z };
    vgeo_view_setup(v, vp.m, vp.m, eye, ps, ZNEAR, W, H, 1.0f, sw_px, flags);
    memcpy(v->wind, g_wind, 16);   // the field outside its tile: instant x, z, settled x, z
    memcpy(v->wind_prev, g_wind, 16);
    v->foliage[0] = v->foliage[1] = g_time;
    v->wind_layers = VGW_ALL;
    v->foliage[2] = g_density;
    v->foliage[3] = g_impostor_px;
}

static void upload_materials(void) { vgc_upload(&g_h.ctx, &g_scene.b_materials, 0, g_scene.materials, sizeof(VgMaterial) * g_scene.material_count); }

typedef struct Px { u32 inst, packed, tri; float depth; } Px;   // inst ~0: empty
typedef struct Img { Px *px; u32 hw, sw, covered; VgCounters ctr; } Img;
static void img_free(Img *m) { free(m->px); memset(m, 0, sizeof(*m)); }

// Placement changes, cull (both passes), raster, read back (image optional).
static void render(const VgViewGpu *v, const VgHizRef *prev, bool mesh, bool raster, Img *out) {
    memset(out, 0, sizeof(*out));
    g_rast.use_mesh = mesh && g_rast.p_mesh;
    VkCommandPool pool;
    VkCommandBuffer cmd = vgc_begin_once(&g_h.ctx, &pool);
    vgeo_scene_placement_record(&g_scene, cmd, 0);
    vg_cull_record(&g_cull, cmd, 0, 0, v, prev, NULL);
    if (raster) {
        vg_raster_clear(&g_rast, cmd);
        vg_raster_record(&g_rast, cmd, 0, 0, 1);
        vg_raster_record(&g_rast, cmd, 0, 0, 2);
    }
    vgc_submit_wait(&g_h.ctx, pool, cmd);
    vgh_read(&g_h, &g_cull.v[0].counters, 0, sizeof(VgCounters), &out->ctr);
    u32 cap = g_cull.caps.visible;
    out->hw = DG_MIN(out->ctr.n[VGN_HW], cap);
    out->sw = DG_MIN(out->ctr.n[VGN_SW], cap);
    CHECK(out->ctr.n[VGN_OVERFLOW] == 0, "%u overflows", out->ctr.n[VGN_OVERFLOW]);
    if (!raster) return;
    u32 *hw = (u32 *)malloc(8ull * (out->hw + 1)), *sw = (u32 *)malloc(8ull * (out->sw + 1));
    vgh_read(&g_h, &g_cull.v[0].visible, 0, 8ull * out->hw, hw);
    vgh_read(&g_h, &g_cull.v[0].visible, 8ull * cap, 8ull * out->sw, sw);
    u64 *vis = (u64 *)malloc(8ull * W * H);
    vgh_read(&g_h, &g_rast.vis, 0, 8ull * W * H, vis);
    out->px = (Px *)malloc(sizeof(Px) * W * H);
    u32 bad = 0;
    for (u32 i = 0; i < W * H; i++) {
        Px *p = &out->px[i];
        p->inst = ~0u;
        if (!vis[i]) continue;
        u32 lo = (u32)vis[i], hi = (u32)(vis[i] >> 32), idx = lo >> VG_VIS_TRI_BITS;
        const u32 *e = idx < cap ? (idx < out->hw ? &hw[2 * idx] : NULL) : (idx - cap < out->sw ? &sw[2 * (idx - cap)] : NULL);
        if (!e) { bad++; continue; }
        float f;
        memcpy(&f, &hi, 4);
        p->inst = e[0];
        p->packed = e[1];
        p->tri = lo & ((1u << VG_VIS_TRI_BITS) - 1);
        p->depth = 1.0f - f;
        out->covered++;
    }
    CHECK(bad == 0, "%u pixels reference a visible index outside the lists", bad);
    free(vis);
    free(hw);
    free(sw);
}

// Visible list entries of the last render (HW then SW): (instance, packed) pairs.
static u32 *visible_entries(const Img *m) {
    u32 cap = g_cull.caps.visible;
    u32 *e = (u32 *)malloc(8ull * (m->hw + m->sw + 1));
    vgh_read(&g_h, &g_cull.v[0].visible, 0, 8ull * m->hw, e);
    vgh_read(&g_h, &g_cull.v[0].visible, 8ull * cap, 8ull * m->sw, e + 2 * m->hw);
    return e;
}

static bool same(const Px *a, const Px *b) { return a->inst == b->inst && (a->inst == ~0u || (a->packed == b->packed && a->tri == b->tri)); }

// As test_vgeo_raster.c: differing pixels must be on a triangle edge (a 3x3 neighbour of another triangle).
static void compare(const char *what, const Img *a, const Img *b, double max_frac) {
    u32 covered = 0, diff = 0, inner = 0;
    for (u32 y = 0; y < H; y++)
        for (u32 x = 0; x < W; x++) {
            u32 i = y * W + x;
            const Px *pa = &a->px[i], *pb = &b->px[i];
            if (pa->inst != ~0u || pb->inst != ~0u) covered++;
            if (same(pa, pb)) continue;
            diff++;
            bool edge = false;
            for (int dy = -1; dy <= 1 && !edge; dy++)
                for (int dx = -1; dx <= 1 && !edge; dx++) {
                    int nx = (int)x + dx, ny = (int)y + dy;
                    if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
                    u32 n = (u32)ny * W + (u32)nx;
                    edge = !same(&a->px[n], pa) || !same(&b->px[n], pb);
                }
            if (!edge) {
                if (inner < 3) printf("  interior difference at %u,%u: %u/%x/%u (%.7f) vs %u/%x/%u (%.7f)\n", x, y, pa->inst, pa->packed, pa->tri, pa->depth, pb->inst,
                                      pb->packed, pb->tri, pb->depth);
                inner++;
            }
        }
    double frac = covered ? (double)diff / covered : 0.0;
    printf("  %s: %u of %u covered pixels differ (%.3f %%), %u not on an edge\n", what, diff, covered, frac * 100.0, inner);
    CHECK(covered > 10000, "%s: only %u pixels covered", what, covered);
    CHECK(frac <= max_frac, "%s: %.3f %% of covered pixels differ", what, frac * 100.0);
    CHECK(inner == 0, "%s: %u differing pixels not on a triangle edge", what, inner);
}

// ------------------------------------------------------------------ content

static u32 find_kind(u32 mesh) {
    for (u32 k = 0; k < VG_FOLIAGE_KINDS; k++)
        if (g_mesh[k] == mesh) return k;
    return ~0u;
}

static void build_content(void) {
    size_t mb = vg_tex_mip_bytes(MASK_SIZE, MASK_SIZE, &g_mask_levels);
    u8 *base = (u8 *)malloc((size_t)MASK_SIZE * MASK_SIZE * 4);
    for (int k = 0; k < VG_FOLIAGE_KINDS; k++) {
        VgFoliageMesh fm;
        vgeo_gen_foliage(&fm, k, 11 + (u32)k);
        VgSource src;
        vgeo_foliage_source(&fm, &src);
        VgBuildOptions opt;
        memset(&opt, 0, sizeof(opt));
        char err[256];
        if (!vgeo_build(&src, &opt, &g_built[k], err, sizeof(err))) { printf("build failed: %s\n", err); exit(1); }
        printf("content: %s %u tris, %u clusters, %u levels, colour %d\n", vgeo_foliage_name(k), fm.index_count / 3, g_built[k].hdr.cluster_count,
               g_built[k].hdr.level_count, (g_built[k].hdr.flags & VGF_HAS_COLOR) != 0);
        CHECK(k == VG_FOLIAGE_TUFT ? (fm.index_count / 3 >= 50 && fm.index_count / 3 <= 70) : (fm.index_count / 3 >= 550 && fm.index_count / 3 <= 650),
              "%s: %u triangles", vgeo_foliage_name(k), fm.index_count / 3);
        vgeo_foliage_mesh_free(&fm);
        // masks (CPU mips kept for the reference), impostor bake
        const u8 *masks[2] = { NULL, NULL };
        for (u32 m = 0; m < 2; m++) {
            if (!vgeo_gen_foliage_mask(base, MASK_SIZE, k, m, 5)) continue;
            g_mask_mips[k][m] = (u8 *)malloc(mb);
            vg_tex_build_mips(base, MASK_SIZE, MASK_SIZE, g_mask_mips[k][m]);
            masks[m] = g_mask_mips[k][m];
            g_mask_tex[k][m] = vg_tex_create(&g_cull.tex, MASK_SIZE, MASK_SIZE, g_mask_levels, g_mask_mips[k][m], true, true);
            CHECK(g_mask_tex[k][m] >= VG_TEX_GAME, "mask texture");
        }
        VgImpostorMaterial im[2];
        vgeo_foliage_impostor_materials(k, masks, MASK_SIZE, im);
        VgImpostorOptions io = { 512, 8, im, 2 };
        u64 t0 = SDL_GetPerformanceCounter();
        CHECK(vgeo_impostor_bake(&g_built[k], &io), "impostor bake");
        const u8 *alb;
        const VgImpostorHeader *ih = vgeo_impostor(&g_built[k], &alb, NULL);
        u32 empty = 0;   // every frame sees something
        for (u32 f = 0; f < 64; f++) {
            u32 cov = 0;
            for (u32 y = 0; y < 64; y++)
                for (u32 x = 0; x < 64; x++) cov += alb[(((f / 8) * 64 + y) * 512 + (f % 8) * 64 + x) * 4 + 3] > 127;
            empty += cov < 20;
        }
        CHECK(ih && empty == 0, "%s impostor: %u empty frames", vgeo_foliage_name(k), empty);
        printf("content: %s impostor %u x %u frames, %u px, %.0f ms\n", vgeo_foliage_name(k), ih ? ih->grid : 0, ih ? ih->grid : 0, ih ? ih->size : 0,
               (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency());
        // serialise round trip keeps the atlas
        u8 *data;
        size_t size;
        vgeo_serialize(&g_built[k], 1, &data, &size);
        VgBuilt back;
        CHECK(vgeo_deserialize(data, size, &back) && back.impostor_size == g_built[k].impostor_size && !memcmp(back.impostor, g_built[k].impostor, back.impostor_size),
              "impostor section round trip");
        vgeo_free(&back);
        free(data);
        g_mesh[k] = vgeo_scene_add_mesh(&g_scene, &g_built[k]);
        u32 nm = vgeo_foliage_materials(k, g_mats[k]);
        for (u32 m = 0; m < nm; m++) {
            if (g_mask_tex[k][m]) g_mats[k][m].mask_tex = g_mats[k][m].albedo_tex = g_mask_tex[k][m];
            u32 idx = vgeo_scene_add_material(&g_scene, &g_mats[k][m]);
            if (m == 0) g_mat_base[k] = idx;
        }
        vgeo_scene_set_mesh_materials(&g_scene, g_mesh[k], g_mat_base[k]);
        CHECK(vgeo_foliage_impostor_upload(&g_scene, &g_cull.tex, g_mesh[k], &g_built[k]), "impostor upload");
    }
    free(base);
    // hemi-octahedral directions round trip
    double worst = 0;
    for (int i = 0; i < 1000; i++) {
        float d[3] = { sinf((float)i * 0.37f) * cosf((float)i * 1.1f), fabsf(cosf((float)i * 0.37f)), sinf((float)i * 0.37f) * sinf((float)i * 1.1f) }, u, v, o[3];
        float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        for (int k = 0; k < 3; k++) d[k] /= l;
        vgeo_hemi_oct_encode(d, &u, &v);
        vgeo_hemi_oct_decode(u, v, o);
        worst = fmax(worst, fabs(o[0] - d[0]) + fabs(o[1] - d[1]) + fabs(o[2] - d[2]));
    }
    CHECK(worst < 1e-5, "hemi-octahedral round trip %.2e", worst);
}

static u32 resolve_name(const char *name, void *u);

// Loads every 32 m chunk of an area through the .vgp format (write, parse, load). Returns instances.
static u32 load_area(const VgScatter *sc, int cx0, int cz0, int cx1, int cz1) {
    static const char names[2][48] = { "test_tuft", "test_bush" };
    VgInstance *tmp = (VgInstance *)malloc(sizeof(VgInstance) * 65536);
    u32 total = 0;
    for (int cz = cz0; cz <= cz1; cz++)
        for (int cx = cx0; cx <= cx1; cx++) {
            u32 n = vgeo_foliage_scatter_chunk(sc, cx, cz, tmp, 65536);
            if (!n) continue;
            size_t size;
            u8 *blob = vgp_write(cx, cz, names, 2, tmp, n, &size);
            VgpView v;
            CHECK(blob && vgp_parse(blob, size, &v), "vgp write / parse");
            if (blob && v.hdr) {
                u32 h = vgeo_foliage_chunk_load(&g_scene, &v, resolve_name, NULL, 0);
                CHECK(h != ~0u, "chunk %d %d load", cx, cz);
                total += n;
            }
            free(blob);
        }
    free(tmp);
    return total;
}

static void unload_all(void) {
    for (u32 h = 0; h < g_scene.pl.chunk_count; h++)
        if (g_scene.pl.chunks[h].used) vgeo_scene_placement_remove(&g_scene, h);
}

static u32 resolve_name(const char *name, void *u) {
    (void)u;
    for (int k = 0; k < VG_FOLIAGE_KINDS; k++)
        if (!strcmp(name, vgeo_foliage_name(k))) return g_mesh[k];
    return ~0u;
}

static VgScatter scatter(float x0, float z0, float x1, float z1, float density, u32 seed) {
    VgScatter s;
    memset(&s, 0, sizeof(s));
    s.x0 = x0, s.z0 = z0, s.x1 = x1, s.z1 = z1;
    s.density = density;
    s.kinds = 2;
    s.weight[0] = 9.0f;
    s.weight[1] = 1.0f;
    s.scale[0][0] = 0.8f, s.scale[0][1] = 1.3f;
    s.scale[1][0] = 0.8f, s.scale[1][1] = 1.5f;
    s.seed = seed;
    return s;
}

// ------------------------------------------------------------------ CPU references

static void qrot(const float q[4], const float v[3], float o[3]) {
    float t[3] = { 2.0f * (q[1] * v[2] - q[2] * v[1]), 2.0f * (q[2] * v[0] - q[0] * v[2]), 2.0f * (q[0] * v[1] - q[1] * v[0]) };
    o[0] = v[0] + q[3] * t[0] + (q[1] * t[2] - q[2] * t[1]);
    o[1] = v[1] + q[3] * t[1] + (q[2] * t[0] - q[0] * t[2]);
    o[2] = v[2] + q[3] * t[2] + (q[0] * t[1] - q[1] * t[0]);
}

// Trilinear, wrapping sample of the alpha of a CPU mip chain (the masks' sampler).
static float alpha_level(const u8 *mips, u32 level, float u, float v) {
    u32 w = MASK_SIZE >> level, off = 0;
    for (u32 l = 0; l < level; l++) off += (MASK_SIZE >> l) * (MASK_SIZE >> l) * 4;
    if (!w) w = 1;
    float x = u * (float)w - 0.5f, y = v * (float)w - 0.5f, fx = floorf(x), fy = floorf(y), tx = x - fx, ty = y - fy;
    float a = 0;
    for (int k = 0; k < 4; k++) {
        int sx = (((int)fx + (k & 1)) % (int)w + (int)w) % (int)w, sy = (((int)fy + (k >> 1)) % (int)w + (int)w) % (int)w;
        a += (float)mips[off + ((u32)sy * w + (u32)sx) * 4 + 3] / 255.0f * ((k & 1) ? tx : 1 - tx) * ((k >> 1) ? ty : 1 - ty);
    }
    return a;
}
static float alpha_tri(const u8 *mips, float u, float v, float lod) {
    lod = fminf(fmaxf(lod, 0.0f), (float)(g_mask_levels - 1));
    u32 l0 = (u32)floorf(lod), l1 = DG_MIN(l0 + 1, g_mask_levels - 1);
    float f = lod - (float)l0;
    return alpha_level(mips, l0, u, v) * (1 - f) + alpha_level(mips, l1, u, v) * f;
}

static void bary(const float c[3][4], const float pn[2], float b[3]) {
    float s[3][2];
    for (int k = 0; k < 3; k++) { s[k][0] = c[k][0] / c[k][3]; s[k][1] = c[k][1] / c[k][3]; }
    float area = (s[1][0] - s[0][0]) * (s[2][1] - s[0][1]) - (s[2][0] - s[0][0]) * (s[1][1] - s[0][1]);
    b[1] = ((pn[0] - s[0][0]) * (s[2][1] - s[0][1]) - (s[2][0] - s[0][0]) * (pn[1] - s[0][1])) / area;
    b[2] = ((s[1][0] - s[0][0]) * (pn[1] - s[0][1]) - (pn[0] - s[0][0]) * (s[1][1] - s[0][1])) / area;
    b[0] = 1 - b[1] - b[2];
    float sum = 0;
    for (int k = 0; k < 3; k++) { b[k] /= c[k][3]; sum += b[k]; }
    for (int k = 0; k < 3; k++) b[k] /= sum;
}

// CPU alpha of the triangle a pixel shows (wind 0: no WPO offset), -1 when the material is not masked.
static float cpu_alpha(const VgViewGpu *v, const Px *p, u32 x, u32 y) {
    const VgInstance *in = vgeo_scene_instance(&g_scene, p->inst);
    u32 k = find_kind(in->mesh);
    if (k == ~0u || p->packed == VG_IMPOSTOR_ENTRY) return -1;
    u32 slot = p->packed >> VG_CLUSTER_BITS, ci = p->packed & ((1u << VG_CLUSTER_BITS) - 1);
    u32 page = slot - g_scene.meshes[in->mesh].page_base;   // every page resident: slot = global page
    static VgDecoded dec;
    vgeo_decode_cluster(&g_built[k], page, ci, &dec);
    const VgMaterial *mt = &g_mats[k][dec.material];
    if (!(mt->flags & VGM_MASKED)) return -1;
    float pos[3], q[4], sc;
    vgeo_instance_decode(in, pos, q, &sc);
    float c[3][4], uv[3][2];
    const VgPageHeader *ph = vgeo_page(&g_built[k], page);
    const VgCluster *cl = vgeo_page_cluster(&g_built[k], page, ci);
    for (int j = 0; j < 3; j++) {
        u32 vi = dec.tri[p->tri][j];
        float l[3] = { dec.pos[vi][0] * sc, dec.pos[vi][1] * sc, dec.pos[vi][2] * sc }, w[3];
        qrot(q, l, w);
        for (int a = 0; a < 3; a++) w[a] += pos[a];
        // the material's WPO (CPU mirror; no wind: no offset)
        const u8 *vp = (const u8 *)ph + (cl->offsets & 0x7FFFu) * 4u + vi * vgeo_vertex_bytes(g_built[k].hdr.flags) + 12;
        VgWindVertex wv;
        memset(&wv, 0, sizeof(wv));
        memcpy(wv.p, w, 12);
        memcpy(wv.ip, pos, 12);
        wv.pscale = sc;
        wv.n[1] = 1.0f;
        for (int a = 0; a < 4; a++) wv.col[a] = vp[a] / 255.0f;
        wv.phase = (float)VG_INST_PHASE(in->flags_phase) / 256.0f;
        float off[3];
        vgeo_wpo_eval(mt, &wv, v->wind, v->foliage[0], v->wind_layers, NULL, off, NULL);
        for (int a = 0; a < 3; a++) w[a] += off[a];
        for (int r = 0; r < 4; r++) c[j][r] = v->raster_view_proj[r] * w[0] + v->raster_view_proj[4 + r] * w[1] + v->raster_view_proj[8 + r] * w[2] + v->raster_view_proj[12 + r];
        uv[j][0] = dec.uv[vi][0] * mt->tex_scale;
        uv[j][1] = dec.uv[vi][1] * mt->tex_scale;
    }
    // the mip LOD of the triangle: the pixel footprint at its screen centroid (vg_uv_plane_lod)
    float cx = (c[0][0] / c[0][3] + c[1][0] / c[1][3] + c[2][0] / c[2][3]) / 3.0f, cy = (c[0][1] / c[0][3] + c[1][1] / c[1][3] + c[2][1] / c[2][3]) / 3.0f;
    float pn[4][2] = { { cx, cy }, { cx + 2.0f / W, cy }, { cx, cy + 2.0f / H }, { ((float)x + 0.5f) / W * 2 - 1, ((float)y + 0.5f) / H * 2 - 1 } };
    float u3[4][2];
    for (int s = 0; s < 4; s++) {
        float b[3];
        bary(c, pn[s], b);
        u3[s][0] = uv[0][0] * b[0] + uv[1][0] * b[1] + uv[2][0] * b[2];
        u3[s][1] = uv[0][1] * b[0] + uv[1][1] * b[1] + uv[2][1] * b[2];
    }
    float dx = hypotf((u3[1][0] - u3[0][0]) * MASK_SIZE, (u3[1][1] - u3[0][1]) * MASK_SIZE), dy = hypotf((u3[2][0] - u3[0][0]) * MASK_SIZE, (u3[2][1] - u3[0][1]) * MASK_SIZE);
    float lod = log2f(fmaxf(fmaxf(dx, dy), 1e-6f));
    return alpha_tri(g_mask_mips[k][dec.material], u3[3][0], u3[3][1], lod);
}

// Instance world sphere as the shaders compute it (WPO-inflated).
static void inst_sphere(const VgInstance *in, float s[4]) {
    VgXfCpu x;
    const VgMesh *m = &g_scene.meshes[in->mesh];
    vgeo_xf_decode(&x, in, m, NULL);
    vgeo_xf_sphere(&x, m->center, m->radius, s);
}

static float inst_dist(const VgInstance *in, const VgViewGpu *v) {
    float dx = in->pos[0] - v->cam_pos[0], dy = in->pos[1] - v->cam_pos[1], dz = in->pos[2] - v->cam_pos[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

// Brute force over every live placement instance: in frustum (1), out (0), within the plane band (-1).
static int frustum_class(const VgViewGpu *v, const float s[4]) {
    if (v->flags & VGV_NO_FRUSTUM) return 1;
    int r = 1;
    for (int p = 0; p < 6; p++) {
        float d = v->planes[p][0] * s[0] + v->planes[p][1] * s[1] + v->planes[p][2] * s[2] + v->planes[p][3];
        float band = 1e-3f * (1.0f + s[3]);
        if (d < -s[3] - band) return 0;
        if (d < -s[3] + band) r = -1;
    }
    return r;
}

typedef void (*InstFn)(u32 gpu_index, const VgInstance *in, void *u);
static void each_live(InstFn fn, void *u) {
    const VgPlacements *p = &g_scene.pl;
    for (u32 c = 0; c < p->cell_cap; c++)
        for (u32 i = p->cells[c].first; i < p->cells[c].first + p->cells[c].count; i++) fn(g_scene.instance_count + i, &p->inst[i], u);
}

// ------------------------------------------------------------------ tests

static void test_masked(void) {
    printf("masked visibility\n");
    VgScatter sc = scatter(-12, -14, 12, 6, 6.0f, 21);
    u32 n = load_area(&sc, -1, -1, 0, 0);
    printf("  patch: %u instances in %u chunks\n", n, g_scene.pl.live_chunks);
    const Cam cams[3] = { { { 0.3f, 1.1f, 4.0f }, { 0.0f, 0.3f, -6.0f } }, { { -3.0f, 0.5f, 1.0f }, { 2.0f, 0.2f, -5.0f } }, { { 1.0f, 2.5f, -12.0f }, { 0.0f, 0.0f, 0.0f } } };
    for (int ci = 0; ci < 3; ci++) {
        printf(" cam %d\n", ci);
        VgViewGpu vh, vs;
        view_for(&cams[ci], VGV_NO_OCCLUSION, 0.0f, &vh);
        view_for(&cams[ci], VGV_NO_OCCLUSION, SW_ALL, &vs);
        Img a, b, c;
        render(&vh, NULL, true, true, &a);
        render(&vs, NULL, true, true, &b);
        printf("  all-HW %u clusters, all-SW %u SW + %u HW (near plane)\n", a.hw, b.sw, b.hw);
        compare("HW vs SW (masked, two-sided, wind)", &a, &b, 0.005);
        if (g_rast.p_mesh) {
            render(&vh, NULL, false, true, &c);
            compare("mesh shader vs vertex pulling", &a, &c, 0.005);
            img_free(&c);
        }
        img_free(&b);
        // CPU alpha-tested reference (no wind: positions without WPO), against the unmasked image
        float wsave[4];
        memcpy(wsave, g_wind, 16);
        memset(g_wind, 0, 16);
        view_for(&cams[ci], VGV_NO_OCCLUSION, 32.0f, &vh);
        Img m, u;
        render(&vh, NULL, true, true, &m);
        for (int k = 0; k < VG_FOLIAGE_KINDS; k++)
            for (int j = 0; j < 2; j++) g_scene.materials[g_mat_base[k] + (u32)j].flags &= ~VGM_MASKED;
        upload_materials();
        render(&vh, NULL, true, true, &u);
        for (int k = 0; k < VG_FOLIAGE_KINDS; k++)
            for (int j = 0; j < 2; j++) g_scene.materials[g_mat_base[k] + (u32)j].flags = g_mats[k][j].flags;
        upload_materials();
        memcpy(g_wind, wsave, 16);
        const float band = 0.06f;
        u32 kept = 0, kept_bad = 0, removed = 0, removed_bad = 0, nearer = 0, border = 0;
        for (u32 y = 2; y < H; y += 5)
            for (u32 x = 2; x < W; x += 5) {
                const Px *pm = &m.px[y * W + x], *pu = &u.px[y * W + x];
                if (pm->inst != ~0u) {
                    float al = cpu_alpha(&vh, pm, x, y);
                    if (al >= 0) {
                        if (fabsf(al - 0.5f) < band) border++;
                        else { kept++; kept_bad += al < 0.5f; }
                    }
                }
                if (pu->inst != ~0u) {
                    float al = cpu_alpha(&vh, pu, x, y);
                    if (al >= 0 && al < 0.5f - band) {
                        removed++;
                        removed_bad += same(pm, pu);
                    }
                    if (pm->inst != ~0u && pm->depth < pu->depth - 1e-6f) nearer++;
                }
            }
        printf("  CPU reference: %u kept samples (%u fail the CPU alpha test), %u masked-out samples (%u still drawn), %u borderline, %u nearer\n", kept, kept_bad,
               removed, removed_bad, border, nearer);
        CHECK(kept > 2000 && kept_bad == 0, "masked winners failing the CPU alpha test: %u of %u", kept_bad, kept);
        CHECK(removed > 500 && removed_bad == 0, "masked-out triangles still drawn: %u of %u", removed_bad, removed);
        CHECK(nearer == 0, "%u masked pixels nearer than the unmasked surface", nearer);
        img_free(&m);
        img_free(&u);
        img_free(&a);
    }
    unload_all();
}

static void test_wpo_bounds(void) {
    printf("WPO bounds\n");
    VgScatter sc = scatter(0, 0, 8, 8, 0.5f, 33);
    load_area(&sc, 0, 0, 0, 0);
    // items: every vertex of every cluster (all levels) of a few instances of each kind
    u32 pick[8], np = 0;
    const VgPlacements *p = &g_scene.pl;
    for (u32 c = 0; c < p->cell_cap && np < 8; c++)
        for (u32 i = p->cells[c].first; i < p->cells[c].first + p->cells[c].count && np < 8; i++) {
            u32 kind = find_kind(p->inst[i].mesh);
            u32 have = 0;
            for (u32 j = 0; j < np; j++) have += find_kind(vgeo_scene_instance(&g_scene, pick[j])->mesh) == kind;
            if (have < 4) pick[np++] = g_scene.instance_count + i;
        }
    u32 cap = 0;
    for (u32 j = 0; j < np; j++) {
        const VgBuilt *b = &g_built[find_kind(vgeo_scene_instance(&g_scene, pick[j])->mesh)];
        cap += b->hdr.cluster_count * 128;
    }
    u32 *items = (u32 *)malloc(16ull * cap), n = 0;
    for (u32 j = 0; j < np; j++) {
        const VgInstance *in = vgeo_scene_instance(&g_scene, pick[j]);
        u32 k = find_kind(in->mesh);
        const VgMesh *me = &g_scene.meshes[in->mesh];
        for (u32 pg = 0; pg < g_built[k].hdr.page_count; pg++) {
            const VgPageHeader *ph = vgeo_page(&g_built[k], pg);
            for (u32 ci = 0; ci < ph->cluster_count; ci++) {
                u32 vc = vgeo_page_cluster(&g_built[k], pg, ci)->counts & 0xFFu;
                for (u32 v = 0; v < vc; v++) {
                    items[n * 4] = pick[j];
                    items[n * 4 + 1] = g_scene.page_table[me->page_base + pg] << VG_CLUSTER_BITS | ci;
                    items[n * 4 + 2] = v;
                    items[n * 4 + 3] = pg << 16 | ci;
                    n++;
                }
            }
        }
    }
    const VgContext *c = &g_h.ctx;
    VgBuffer bin = vgc_buffer(c, 16ull * n, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VGM_DEVICE);
    VgBuffer bout = vgc_buffer(c, 112ull * n, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VGM_DEVICE);
    vgc_upload(c, &bin, 0, items, 16ull * n);
    VkDescriptorSetLayoutBinding lb[2];
    memset(lb, 0, sizeof(lb));
    for (u32 i = 0; i < 2; i++) {
        lb[i].binding = i;
        lb[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        lb[i].descriptorCount = 1;
        lb[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = 2;
    li.pBindings = lb;
    VkDescriptorSetLayout tl;
    vkCreateDescriptorSetLayout(c->device, &li, NULL, &tl);
    VkDescriptorSetLayout sets[2] = { g_cull.layout, tl };
    VkPushConstantRange pr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VgPush) };
    VkPipelineLayoutCreateInfo pi = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pi.setLayoutCount = 2;
    pi.pSetLayouts = sets;
    pi.pushConstantRangeCount = 1;
    pi.pPushConstantRanges = &pr;
    VkPipelineLayout pl;
    vkCreatePipelineLayout(c->device, &pi, NULL, &pl);
    VkPipeline pipe = vgc_compute_pipeline(c, pl, "vgeo/vg_test_wpo.comp");
    CHECK(pipe != VK_NULL_HANDLE, "vg_test_wpo.comp pipeline");
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 };
    VkDescriptorPoolCreateInfo dp = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dp.maxSets = 1;
    dp.poolSizeCount = 1;
    dp.pPoolSizes = &ps;
    VkDescriptorPool dpool;
    vkCreateDescriptorPool(c->device, &dp, NULL, &dpool);
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = dpool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &tl;
    VkDescriptorSet ts;
    vkAllocateDescriptorSets(c->device, &ai, &ts);
    VkDescriptorBufferInfo bi[2] = { { bin.buffer, 0, VK_WHOLE_SIZE }, { bout.buffer, 0, VK_WHOLE_SIZE } };
    VkWriteDescriptorSet wr[2];
    memset(wr, 0, sizeof(wr));
    for (u32 i = 0; i < 2; i++) {
        wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[i].dstSet = ts;
        wr[i].dstBinding = i;
        wr[i].descriptorCount = 1;
        wr[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        wr[i].pBufferInfo = &bi[i];
    }
    vkUpdateDescriptorSets(c->device, 2, wr, 0, NULL);
    // displacement buffer saturated around the instances (WPO_DISPLACE of the grass)
    VgDisplace disp;
    CHECK(vg_displace_init(&disp, c, &g_cull.tex), "displacement buffer");
    float cam[3] = { 4, 1, 4 };
    for (int f = 0; f < 8; f++) {
        for (int k = 0; k < 9; k++) {
            float pp[3] = { (float)(k % 3) * 3.0f + 1.0f, 0, (float)(k / 3) * 3.0f + 1.0f };
            vg_displace_push(&disp, pp, 2.5f, 20.0f);
        }
        VkCommandPool pool;
        VkCommandBuffer cmd = vgc_begin_once(c, &pool);
        vg_displace_record(&disp, cmd, 0, cam, 0.1f);
        vgc_submit_wait(c, pool, cmd);
    }
    // the wind field's tile on the GPU (pass 3): two consecutive frames of a gusty field
    static WindField wf, wf_prev;
    VgWindTex wt;
    CHECK(vg_wind_init(&wt, c, &g_cull.tex), "wind tile");
    wind_init(&wf, 21);
    float *o = (float *)malloc(112ull * n);
    u32 outside_cl = 0, outside_in = 0, mirror_bad = 0, samples = 0, mv_bad = 0, n_bad = 0, wind_bad = 0, tile_samples = 0, tile_varied = 0;
    double worst_excess = -1e9, worst_mirror = 0, max_disp = 0, worst_mv = 0, worst_n = 0, worst_wind = 0, max_mv = 0;
    const Cam cv = { { 4, 1.5f, 14 }, { 4, 0, 4 } };
    for (int s = 0; s < 4 * 8 * 4; s++) {
        // 0: full wind, no displacement; 1: 3x the largest full wind; 2: full wind + displacement buffer;
        // 3: the wind field's tile (gusts, turbulence), last frame's tile and time for the velocity
        int pass = s / 32;
        float ang = (float)(s % 4) * 1.5707963f + 0.3f, t = (float)(s % 8) * 1.37f + 0.5f;
        float wmax = pass == 1 ? 66.0f : 22.0f;
        float wnd[4] = { cosf(ang) * wmax, sinf(ang) * wmax, cosf(ang + 0.2f) * wmax * 1.1f, sinf(ang + 0.2f) * wmax * 1.1f };
        VgViewGpu v;
        memcpy(g_wind, wnd, 16);
        g_time = t;
        view_for(&cv, VGV_NO_OCCLUSION, 32.0f, &v);
        if (pass == 2) vg_displace_view(&disp, &v);
        VkCommandPool pool;
        VkCommandBuffer cmd = vgc_begin_once(c, &pool);
        if (pass == 3) {
            WindBase b = { ang * 57.29578f, 8.0f + (float)(s % 8) * 3.0f, 0.9f };
            float wcam[3] = { 4.0f + (float)(s % 5) * 9.0f, 1.0f, 4.0f - (float)(s % 3) * 7.0f };
            wind_set(&wf, &b, t - 1.0 / 60.0);
            wind_tile_update(&wf, wcam, 1.0f / 60.0f);
            VkCommandPool p0;   // last frame's upload: its own submit (one staging buffer per frame slot)
            VkCommandBuffer c0 = vgc_begin_once(c, &p0);
            vg_wind_record(&wt, c0, 0, wf.texels, wf.ox, wf.oz, WIND_TEXEL);
            vgc_submit_wait(c, p0, c0);
            memcpy(&wf_prev, &wf, sizeof(wf));
            wcam[0] += 6.0f;   // the window moves between the frames
            wind_set(&wf, &b, t);
            wind_tile_update(&wf, wcam, 1.0f / 60.0f);
            vg_wind_record(&wt, cmd, 0, wf.texels, wf.ox, wf.oz, WIND_TEXEL);
            vg_wind_view(&wt, &v);
            float g[4] = { wf.dir[0] * wf.base.speed, wf.dir[1] * wf.base.speed, wf.global_bend[0], wf.global_bend[1] };
            float gp[4] = { wf_prev.dir[0] * wf_prev.base.speed, wf_prev.dir[1] * wf_prev.base.speed, wf_prev.global_bend[0], wf_prev.global_bend[1] };
            memcpy(v.wind, g, 16);
            memcpy(v.wind_prev, gp, 16);
            v.foliage[1] = t - 1.0f / 60.0f;
        }
        vgeo_scene_placement_record(&g_scene, cmd, 0);
        vg_cull_record(&g_cull, cmd, 0, 0, &v, NULL, NULL);   // uploads the view, writes the set
        VkDescriptorSet cs = vg_cull_set(&g_cull, 0, 0);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &cs, 0, NULL);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 1, 1, &ts, 0, NULL);
        VgPush pp;
        memset(&pp, 0, sizeof(pp));
        pp.a[0] = n;
        vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pp), &pp);
        vkCmdDispatch(cmd, (n + 63) / 64, 1, 1);
        vgc_submit_wait(c, pool, cmd);
        vgh_read(&g_h, &bout, 0, 112ull * n, o);
        for (u32 i = 0; i < n; i++) {
            const float *w = &o[i * 28], *cs4 = w + 4, *is = w + 8, *pw = w + 12, *gw = w + 16, *gn = w + 20, *gwp = w + 24;
            double dc = sqrt((double)(w[0] - cs4[0]) * (w[0] - cs4[0]) + (double)(w[1] - cs4[1]) * (w[1] - cs4[1]) + (double)(w[2] - cs4[2]) * (w[2] - cs4[2]));
            double di = sqrt((double)(w[0] - is[0]) * (w[0] - is[0]) + (double)(w[1] - is[1]) * (w[1] - is[1]) + (double)(w[2] - is[2]) * (w[2] - is[2]));
            outside_cl += dc > cs4[3];
            outside_in += di > is[3];
            worst_excess = fmax(worst_excess, dc - cs4[3]);
            samples++;
            if (pass == 2) continue;   // CPU mirror without the displacement buffer
            const VgInstance *in = vgeo_scene_instance(&g_scene, items[i * 4]);
            u32 k = find_kind(in->mesh);
            static VgDecoded dec;
            static u32 dec_key = ~0u, dec_kind = ~0u;
            if (dec_key != items[i * 4 + 3] || dec_kind != k) {
                vgeo_decode_cluster(&g_built[k], items[i * 4 + 3] >> 16, items[i * 4 + 3] & 0xFFFFu, &dec);
                dec_key = items[i * 4 + 3];
                dec_kind = k;
            }
            u32 vi = items[i * 4 + 2];
            float pos[3], q[4], sc, l[3], wp[3], nw[3];
            vgeo_instance_decode(in, pos, q, &sc);
            for (int a = 0; a < 3; a++) l[a] = dec.pos[vi][a] * sc;
            qrot(q, l, wp);
            qrot(q, dec.normal[vi], nw);
            for (int a = 0; a < 3; a++) wp[a] += pos[a];
            // the vertex colour: decode it from the page (the CPU decode has none)
            const VgPageHeader *ph = vgeo_page(&g_built[k], items[i * 4 + 3] >> 16);
            const VgCluster *cl = vgeo_page_cluster(&g_built[k], items[i * 4 + 3] >> 16, items[i * 4 + 3] & 0xFFFFu);
            const u8 *vp = (const u8 *)ph + (cl->offsets & 0x7FFFu) * 4u + vi * vgeo_vertex_bytes(g_built[k].hdr.flags) + 12;
            VgWindVertex wv;
            memset(&wv, 0, sizeof(wv));
            memcpy(wv.p, wp, 12);
            memcpy(wv.ip, pos, 12);
            memcpy(wv.n, nw, 12);
            wv.pscale = sc;
            for (int a = 0; a < 4; a++) wv.col[a] = vp[a] / 255.0f;
            wv.phase = (float)VG_INST_PHASE(in->flags_phase) / 256.0f;
            const VgMaterial *mt = &g_mats[k][dec.material];
            // the wind at the pivot: the view's (passes 0-1); pass 3: the GPU's tile sample feeds the
            // mirror (the sample itself is checked against wind_tile_sample below)
            float wcur[4], wprev[4], off[3], offp[3], nn[3];
            memcpy(wcur, pass == 3 ? gw : wnd, 16);
            memcpy(wprev, pass == 3 ? gwp : wnd, 16);
            vgeo_wpo_eval(mt, &wv, wcur, v.foliage[0], VGW_ALL, NULL, off, nn);
            vgeo_wpo_eval(mt, &wv, wprev, v.foliage[1], VGW_ALL, NULL, offp, NULL);
            double e = 0, em = 0, en = 0, mv = 0;
            for (int a = 0; a < 3; a++) {
                e += (double)(wp[a] + off[a] - w[a]) * (wp[a] + off[a] - w[a]);
                em += (double)(wp[a] + offp[a] - pw[a]) * (wp[a] + offp[a] - pw[a]);
                en += (double)(nn[a] - gn[a]) * (nn[a] - gn[a]);
                mv += (double)(w[a] - pw[a]) * (w[a] - pw[a]);
            }
            e = sqrt(e), em = sqrt(em), en = sqrt(en);
            worst_mirror = fmax(worst_mirror, e);
            mirror_bad += e > 2e-4;
            worst_mv = fmax(worst_mv, em);
            mv_bad += em > 2e-4;
            max_mv = fmax(max_mv, sqrt(mv));
            if (mt->wpo != WPO_NONE) {
                worst_n = fmax(worst_n, en);
                n_bad += en > 2e-3;
            }
            max_disp = fmax(max_disp, sqrt((double)off[0] * off[0] + (double)off[1] * off[1] + (double)off[2] * off[2]));
            if (pass == 3) {   // shader tile sample vs the CPU's (half texels, 8-bit filter weights)
                float cw[4], cwp[4];
                wind_tile_sample(&wf, pos[0], pos[2], cw);
                wind_tile_sample(&wf_prev, pos[0], pos[2], cwp);
                for (int a = 0; a < 4; a++) {
                    double d = fmax(fabs(cw[a] - gw[a]), fabs(cwp[a] - gwp[a]));
                    worst_wind = fmax(worst_wind, d);
                    wind_bad += d > 0.02 + 4e-3 * fabs(cw[a]);
                }
                tile_samples++;
                // the tile, not the fallback: gusts / turbulence differ from the mean wind
                tile_varied += fabs(gw[0] - v.wind[0]) + fabs(gw[1] - v.wind[1]) > 0.1;
            }
        }
    }
    printf("  %u vertex samples (8 instances, all clusters of all levels, 128 wind / time / displacement / tile states): %u outside the cluster sphere, %u outside "
           "the instance sphere (worst %.4f m inside), CPU mirror worst %.2e m (%u over 2e-4), largest wind offset %.3f m\n",
           samples, outside_cl, outside_in, -worst_excess, worst_mirror, mirror_bad, max_disp);
    printf("  velocity: previous-frame position vs the CPU mirror with last frame's wind / time / tile: worst %.2e m (%u over 2e-4, largest motion %.3f m); "
           "bent normals worst %.2e (%u over 2e-3); tile sample at the pivot vs wind_tile_sample: worst %.4f m/s over %u samples (%u out of tolerance)\n",
           worst_mv, mv_bad, max_mv, worst_n, n_bad, worst_wind, tile_samples, wind_bad);
    CHECK(outside_cl == 0 && outside_in == 0, "WPO: %u / %u displaced vertices outside their bounds", outside_cl, outside_in);
    CHECK(mirror_bad == 0, "WPO: GPU snippet vs CPU mirror %.2e m", worst_mirror);
    CHECK(mv_bad == 0, "WPO velocity: GPU previous position vs CPU %.2e m", worst_mv);
    CHECK(n_bad == 0, "WPO normals: GPU vs CPU %.2e", worst_n);
    CHECK(wind_bad == 0 && tile_samples > 0, "wind tile: GPU sample vs wind_tile_sample %.4f m/s", worst_wind);
    CHECK(tile_varied > tile_samples / 4, "wind tile: %u / %u pivot samples differ from the mean wind (the tile was sampled)", tile_varied, tile_samples);
    CHECK(max_disp > 0.05, "WPO: the wind displaced at most %.3f m", max_disp);
    CHECK(max_mv > 1e-3, "WPO velocity: the wind moved vertices by at most %.4f m between frames", max_mv);
    vg_wind_shutdown(&wt);
    vg_displace_shutdown(&disp);
    g_wind[0] = 4.8f, g_wind[1] = 3.6f, g_wind[2] = 4.8f, g_wind[3] = 3.6f;
    g_time = 3.7f;
    vkDestroyPipeline(c->device, pipe, NULL);
    vkDestroyPipelineLayout(c->device, pl, NULL);
    vkDestroyDescriptorSetLayout(c->device, tl, NULL);
    vkDestroyDescriptorPool(c->device, dpool, NULL);
    vgc_buffer_free(c, &bin);
    vgc_buffer_free(c, &bout);
    free(items);
    free(o);
    unload_all();
}

// The kept set of a view: sorted unique instances of the visible lists (clusters + impostors).
static u32 kept_set(const Img *m, u32 **out) {
    u32 *e = visible_entries(m), n = m->hw + m->sw;
    u32 *s = (u32 *)malloc(sizeof(u32) * (n + 1));
    for (u32 i = 0; i < n; i++) s[i] = e[2 * i];
    free(e);
    // sort + unique
    for (u32 gap = n / 2; gap > 0; gap /= 2)
        for (u32 i = gap; i < n; i++)
            for (u32 j = i; j >= gap && s[j - gap] > s[j]; j -= gap) { u32 t = s[j]; s[j] = s[j - gap]; s[j - gap] = t; }
    u32 k = 0;
    for (u32 i = 0; i < n; i++)
        if (!k || s[k - 1] != s[i]) s[k++] = s[i];
    *out = s;
    return k;
}

typedef struct DensityRef { const VgViewGpu *v; u32 kept, border; u8 *keep; } DensityRef;
static void density_fn(u32 gi, const VgInstance *in, void *u) {
    DensityRef *r = (DensityRef *)u;
    const VgMesh *m = &g_scene.meshes[in->mesh];
    float d = inst_dist(in, r->v), a = vgeo_foliage_alpha(m, in->flags_phase, d, r->v->foliage[2]);
    // borderline: the threshold within f32 noise of the hash
    float t = r->v->foliage[2] * (m->fade_end > m->fade_start ? fminf(fmaxf((m->fade_end - d) / (m->fade_end - m->fade_start), 0), 1) : 1);
    float h = ((float)VG_INST_HASH(in->flags_phase) + 0.5f) * (0.9375f / 256.0f);
    if (fabsf(t - h) < 1e-5f) { r->border++; r->keep[gi] = 2; return; }
    r->keep[gi] = a > 0;
    r->kept += a > 0;
}

static void test_density(void) {
    printf("density hash\n");
    VgScatter sc = scatter(-100, -100, 100, 100, 1.5f, 44);
    u32 n = load_area(&sc, -4, -4, 3, 3);
    printf("  scene: %u instances in %u chunks, %u cells\n", n, g_scene.pl.live_chunks, g_scene.pl.cell_cap - g_scene.pl.free_cells_n);
    const Cam cam = { { 5.0f, 1.7f, 60.0f }, { 0.0f, 0.5f, 0.0f } };
    u32 total = vgeo_scene_gpu_instances(&g_scene);
    u8 *keep = (u8 *)calloc(total, 1);
    float dens[3] = { 0.7f, 0.4f, 1.0f };
    u32 *sets[3], ns[3];
    for (int di = 0; di < 3; di++) {
        g_density = dens[di];
        VgViewGpu v;
        view_for(&cam, VGV_NO_FRUSTUM | VGV_NO_OCCLUSION | VGV_NO_CONE, 32.0f, &v);   // LOD only: every kept instance draws
        Img m;
        render(&v, NULL, true, false, &m);
        ns[di] = kept_set(&m, &sets[di]);
        memset(keep, 0, total);
        DensityRef r = { &v, 0, 0, keep };
        each_live(density_fn, &r);
        u32 miss = 0, extra = 0;
        for (u32 i = 0, j = 0; i < total; i++) {
            while (j < ns[di] && sets[di][j] < i) j++;
            bool gpu = j < ns[di] && sets[di][j] == i;
            if (keep[i] == 2) continue;
            miss += keep[i] && !gpu;
            extra += !keep[i] && gpu;
        }
        printf("  density %.1f: GPU keeps %u (faded %u, impostors %u), CPU mirror %u (%u borderline), %u missing, %u extra\n", dens[di], ns[di], m.ctr.n[VGN_INST_FADED],
               m.ctr.n[VGN_IMPOSTORS], r.kept, r.border, miss, extra);
        CHECK(miss == 0 && extra == 0, "density %.1f: GPU kept set vs CPU mirror: %u missing, %u extra", dens[di], miss, extra);
        CHECK(m.ctr.n[VGN_INST_FADED] > 0 && ns[di] > 1000, "density %.1f: nothing faded / kept", dens[di]);
    }
    // monotonic: kept(0.4) c kept(0.7) c kept(1.0)
    u32 not_sub = 0;
    const int pairs[2][2] = { { 1, 0 }, { 0, 2 } };
    for (int q = 0; q < 2; q++) {
        const u32 *sa = sets[pairs[q][0]], *sb = sets[pairs[q][1]];
        u32 na = ns[pairs[q][0]], nb = ns[pairs[q][1]];
        for (u32 i = 0, j = 0; i < na; i++) {
            while (j < nb && sb[j] < sa[i]) j++;
            not_sub += !(j < nb && sb[j] == sa[i]);
        }
    }
    CHECK(not_sub == 0, "density: %u instances kept at a lower density but not at a higher one", not_sub);
    printf("  monotonic: kept(0.4) c kept(0.7) c kept(1.0): %u violations\n", not_sub);
    // 600 frames of a static camera (frame slots alternating, as the renderer): the identical set
    g_density = 0.7f;
    VgViewGpu v;
    view_for(&cam, VGV_NO_OCCLUSION, 32.0f, &v);
    u32 h0 = 0, changed = 0, n0 = 0;
    u64 t0 = SDL_GetPerformanceCounter();
    for (int f = 0; f < 600; f++) {
        u32 slot = (u32)f & 1u;
        VkCommandPool pool;
        VkCommandBuffer cmd = vgc_begin_once(&g_h.ctx, &pool);
        v.frame_index = (u32)f;
        vg_cull_record(&g_cull, cmd, slot, 0, &v, NULL, NULL);
        vgc_submit_wait(&g_h.ctx, pool, cmd);
        VgCounters ctr;
        vgh_read(&g_h, &g_cull.v[0].counters, 0, sizeof(ctr), &ctr);
        Img m;
        memset(&m, 0, sizeof(m));
        m.hw = DG_MIN(ctr.n[VGN_HW], g_cull.caps.visible);
        m.sw = DG_MIN(ctr.n[VGN_SW], g_cull.caps.visible);
        u32 *s, k = kept_set(&m, &s);
        u32 h = vgeo_fnv1a(s, sizeof(u32) * k, 2166136261u) ^ ctr.n[VGN_INST_VIS] * 31u ^ ctr.n[VGN_INST_FADED];
        free(s);
        if (!f) { h0 = h; n0 = k; }
        else changed += h != h0;
    }
    printf("  600 frames, static camera: %u instances kept every frame, %u frames differ (%.2f s)\n", n0, changed,
           (double)(SDL_GetPerformanceCounter() - t0) / (double)SDL_GetPerformanceFrequency());
    CHECK(changed == 0 && n0 > 100, "density hash: the kept set changed in %u of 600 frames", changed);
    g_density = 1.0f;
    for (int i = 0; i < 3; i++) free(sets[i]);
    free(keep);
    unload_all();
}

typedef struct CellRef { const VgViewGpu *v; u32 in, border; u8 *mark; } CellRef;
static void cell_fn(u32 gi, const VgInstance *in, void *u) {
    CellRef *r = (CellRef *)u;
    float s[4];
    inst_sphere(in, s);
    int fc = frustum_class(r->v, s);
    const VgMesh *m = &g_scene.meshes[in->mesh];
    if (vgeo_foliage_alpha(m, in->flags_phase, inst_dist(in, r->v), r->v->foliage[2]) <= 0) fc = 0;
    if (fc < 0) { r->border++; r->mark[gi] = 2; return; }
    r->mark[gi] = (u8)fc;
    r->in += fc;
}

static void test_cells(void) {
    printf("cell culling vs brute force\n");
    VgScatter sc = scatter(-120, -120, 120, 120, 1.2f, 55);
    u32 n = load_area(&sc, -4, -4, 3, 3);
    printf("  scene: %u instances, %u chunks\n", n, g_scene.pl.live_chunks);
    const Cam cams[4] = { { { 0, 1.7f, 130 }, { 0, 0, 0 } }, { { -30, 6, -20 }, { 40, 0, 30 } }, { { 0, 80, 0 }, { 1, 0, 1 } }, { { 110, 2, -110 }, { 0, 0, 0 } } };
    u32 total = vgeo_scene_gpu_instances(&g_scene);
    u8 *mark = (u8 *)calloc(total, 1);
    for (int ci = 0; ci < 4; ci++) {
        VgViewGpu v;
        view_for(&cams[ci], VGV_NO_OCCLUSION, 32.0f, &v);
        Img m;
        render(&v, NULL, true, false, &m);
        memset(mark, 0, total);
        CellRef r = { &v, 0, 0, mark };
        each_live(cell_fn, &r);
        u32 *s, k = kept_set(&m, &s), false_pos = 0;
        for (u32 i = 0; i < k; i++) false_pos += mark[s[i]] == 0;
        free(s);
        u32 gpu = m.ctr.n[VGN_INST_VIS];
        u32 diff = gpu > r.in ? gpu - r.in : r.in - gpu;
        printf("  cam %d: GPU %u visible instances (cells %u + %u large), brute force %u (+%u borderline), %u drawn outside\n", ci, gpu, m.ctr.n[VGN_CELLS_VIS],
               m.ctr.n[VGN_CELLS_OCC], r.in, r.border, false_pos);
        CHECK(diff <= r.border, "cam %d: GPU %u visible instances vs %u brute force (%u borderline)", ci, gpu, r.in, r.border);
        CHECK(false_pos == 0, "cam %d: %u drawn instances fail the brute-force test", ci, false_pos);
    }
    free(mark);
    unload_all();
}

static void test_impostor_switch(void) {
    printf("impostor switch\n");
    // bushes on a line away from the camera, some exactly at the switch distance
    g_impostor_px = 48.0f;   // the switch at ~65 m, inside the bush fade start
    const Cam cam = { { 0, 1.0f, 2.0f }, { 0, 1.0f, -100.0f } };
    VgViewGpu v;
    view_for(&cam, VGV_NO_FRUSTUM | VGV_NO_OCCLUSION, 32.0f, &v);
    const VgMesh *bm = &g_scene.meshes[g_mesh[VG_FOLIAGE_BUSH]];
    VgInstance inst[400];
    u32 n = 0;
    for (u32 i = 0; i < 190; i++) {   // 0 .. 95 m (inside the bush fade start)
        float p[3] = { 0.0f, 0.0f, -(float)i * 0.5f };
        vgeo_foliage_instance(&inst[n++], g_mesh[VG_FOLIAGE_BUSH], p, (float)i * 0.7f, 1.0f);
    }
    // the exact switch distance: size = px  <=>  |c - eye| - r = 2 r ps / px; instances (yaw 0) whose
    // sphere centre sits there within +-1e-5 relative
    VgInstance probe;
    float p0[3] = { 0, 0, 0 }, s0[4];
    vgeo_foliage_instance(&probe, g_mesh[VG_FOLIAGE_BUSH], p0, 0.0f, 1.0f);
    inst_sphere(&probe, s0);
    float rr = s0[3] - g_scene.meshes[g_mesh[VG_FOLIAGE_BUSH]].max_wpo, dsw = 2.0f * rr * v.proj_scale / g_impostor_px + rr;
    for (int k = -10; k <= 10; k++) {
        float d = dsw * (1.0f + (float)k * 1e-6f);
        float ox = s0[0], oy = s0[1] - cam.eye.y;
        float zc = cam.eye.z - sqrtf(d * d - ox * ox - oy * oy);
        float p[3] = { 0, 0, zc - s0[2] };
        vgeo_foliage_instance(&inst[n++], g_mesh[VG_FOLIAGE_BUSH], p, 0.0f, 1.0f);
    }
    // placement chunk without the 32 m grid: one cell list over everything (cells only need the bounds)
    VgCell cells[16];
    u32 per = (n + 15) / 16;
    for (u32 c = 0; c < 16; c++) { cells[c].first = c * per; cells[c].count = c * per < n ? DG_MIN(per, n - c * per) : 0; }
    u32 h = vgeo_scene_placement_add(&g_scene, cells, 16, inst, n, 0);
    CHECK(h != ~0u, "impostor line chunk");
    Img m;
    render(&v, NULL, true, false, &m);
    u32 *e = visible_entries(&m);
    u32 total = vgeo_scene_gpu_instances(&g_scene);
    u8 *imp = (u8 *)calloc(total, 1), *dag = (u8 *)calloc(total, 1);
    for (u32 i = 0; i < m.hw + m.sw; i++) {
        if (e[2 * i + 1] == VG_IMPOSTOR_ENTRY) imp[e[2 * i]]++;
        else dag[e[2 * i]] = 1;
    }
    u32 both = 0, neither = 0, wrong = 0, border = 0, nimp = 0, ndag = 0, exact = 0;
    for (u32 i = 0; i < n; i++) {
        u32 gi = g_scene.instance_count + g_scene.pl.chunks[h].first + i;
        const VgInstance *in = vgeo_scene_instance(&g_scene, gi);
        float s[4];
        inst_sphere(in, s);
        s[3] -= vg_wpo_margin(&g_scene.meshes[in->mesh], vgeo_half_to_float(in->scale_h));
        float size = vgeo_project_size(&v, s);
        s[3] += vg_wpo_margin(&g_scene.meshes[in->mesh], vgeo_half_to_float(in->scale_h));
        bool cpu = vgeo_use_impostor(&v, &g_scene.meshes[in->mesh], s, vg_wpo_margin(&g_scene.meshes[in->mesh], vgeo_half_to_float(in->scale_h)));
        both += imp[gi] && dag[gi];
        if (!imp[gi] && !dag[gi] && neither < 4)
            printf("  neither: instance %u at %.2f m, size %.3f px, cpu impostor %d, alpha %.3f\n", i, (double)inst_dist(in, &v), (double)vgeo_project_size(&v, s), cpu,
                   (double)vgeo_foliage_alpha(&g_scene.meshes[in->mesh], in->flags_phase, inst_dist(in, &v), v.foliage[2]));
        neither += !imp[gi] && !dag[gi];
        if (fabsf(size / g_impostor_px - 1.0f) < 2e-6f) { border++; continue; }
        exact += fabsf(size / g_impostor_px - 1.0f) < 1e-4f;
        wrong += (imp[gi] != 0) != cpu;
        nimp += imp[gi] != 0;
        ndag += dag[gi];
    }
    printf("  %u instances: %u impostors, %u DAG cuts (%u within 1e-4 of the switch), %u both, %u neither, %u decided unlike the CPU, %u within f32 noise\n", n, nimp,
           ndag, exact, both, neither, wrong, border);
    CHECK(both == 0 && neither == 0, "impostor switch: %u instances drawn twice, %u not at all", both, neither);
    CHECK(wrong == 0 && nimp > 20 && ndag > 50 && exact >= 4, "impostor switch: %u instances unlike the CPU comparison", wrong);
    CHECK(m.ctr.n[VGN_IMPOSTORS] >= nimp && m.ctr.n[VGN_IMPOSTORS] <= nimp + border, "impostor counter %u vs %u", m.ctr.n[VGN_IMPOSTORS], nimp);
    // rasterised: the impostor quads cover pixels through the SW path (atlas alpha test)
    Img r;
    const Cam above = { { 0, 25.0f, 2.0f }, { 0, 0, -120.0f } };   // looking down the line: the far bushes are not behind the near ones
    view_for(&above, VGV_NO_OCCLUSION, 32.0f, &v);
    render(&v, NULL, true, true, &r);
    u32 imp_px = 0;
    for (u32 i = 0; i < W * H; i++) imp_px += r.px[i].inst != ~0u && r.px[i].packed == VG_IMPOSTOR_ENTRY;
    printf("  rasterised: %u impostor pixels, %u covered\n", imp_px, r.covered);
    CHECK(imp_px > 100, "impostors: only %u pixels", imp_px);
    img_free(&r);
    free(e);
    free(imp);
    free(dag);
    vgeo_scene_placement_remove(&g_scene, h);
    g_impostor_px = 0.0f;
}

typedef struct LiveRef { const VgViewGpu *v; u32 kept; } LiveRef;
static void live_fn(u32 gi, const VgInstance *in, void *u) {
    (void)gi;
    LiveRef *r = (LiveRef *)u;
    r->kept += vgeo_foliage_alpha(&g_scene.meshes[in->mesh], in->flags_phase, inst_dist(in, r->v), r->v->foliage[2]) > 0;
}

static void test_streaming(void) {
    printf("placement streaming flythrough\n");
    VgScatter sc = scatter(-1e6f, -1e6f, 1e6f, 1e6f, 1.0f, 66);
    const int R = 3;
    char err[256] = "";
    u32 loads = 0, unloads = 0, audits_bad = 0, max_live = 0, gpu_bad = 0, count_bad = 0, checks = 0;
    u64 up0 = g_scene.pl.uploads;
    float travelled = 0, px = 0, pz = 0;
    for (int step = 0; step <= 520; step++) {
        float t = (float)step * 4.0f;   // 2080 m along a winding path
        float x = t * 0.8f + 60.0f * sinf(t * 0.004f), z = t * 0.6f + 80.0f * cosf(t * 0.003f);
        if (step) travelled += sqrtf((x - px) * (x - px) + (z - pz) * (z - pz));
        px = x, pz = z;
        int cx = (int)floorf(x / VGP_CHUNK_SIZE), cz = (int)floorf(z / VGP_CHUNK_SIZE);
        // unload chunks out of range, load the missing ones
        for (u32 h = 0; h < g_scene.pl.chunk_count; h++) {
            VgPlacementChunk *ch = &g_scene.pl.chunks[h];
            if (!ch->used) continue;
            int ux = (int)(i16)(ch->user & 0xFFFFu), uz = (int)(i16)(ch->user >> 16);
            if (abs(ux - cx) > R || abs(uz - cz) > R) { vgeo_scene_placement_remove(&g_scene, h); unloads++; }
        }
        for (int dz = -R; dz <= R; dz++)
            for (int dx = -R; dx <= R; dx++) {
                u32 tag = (u32)(u16)(i16)(cx + dx) | (u32)(u16)(i16)(cz + dz) << 16;
                bool have = false;
                for (u32 h = 0; h < g_scene.pl.chunk_count && !have; h++) have = g_scene.pl.chunks[h].used && g_scene.pl.chunks[h].user == tag;
                if (have) continue;
                static VgInstance tmp[8192];
                static const char names[2][48] = { "test_tuft", "test_bush" };
                u32 n = vgeo_foliage_scatter_chunk(&sc, cx + dx, cz + dz, tmp, 8192);
                size_t size;
                u8 *blob = vgp_write(cx + dx, cz + dz, names, 2, tmp, n, &size);
                VgpView v;
                if (blob && vgp_parse(blob, size, &v)) {
                    u32 h = vgeo_foliage_chunk_load(&g_scene, &v, resolve_name, NULL, tag);
                    if (h != ~0u) loads++;
                    else count_bad++;
                }
                free(blob);
            }
        if (!vgeo_scene_placement_audit(&g_scene, err, sizeof(err))) {
            if (!audits_bad) printf("  step %d: %s\n", step, err);
            audits_bad++;
        }
        max_live = DG_MAX(max_live, g_scene.pl.live_instances);
        if (step % 20 == 0) {   // GPU: the culled instance count, the region's contents
            const Cam cam = { { x, 2, z }, { x + 1, 1.5f, z - 1 } };
            VgViewGpu v;
            view_for(&cam, VGV_NO_FRUSTUM | VGV_NO_OCCLUSION, 32.0f, &v);
            VkCommandPool pool;
            VkCommandBuffer cmd = vgc_begin_once(&g_h.ctx, &pool);
            vgeo_scene_placement_record(&g_scene, cmd, (u32)step & 1u);
            vg_cull_record(&g_cull, cmd, (u32)step & 1u, 0, &v, NULL, NULL);
            vgc_submit_wait(&g_h.ctx, pool, cmd);
            VgCounters ctr;
            vgh_read(&g_h, &g_cull.v[0].counters, 0, sizeof(ctr), &ctr);
            LiveRef lr = { &v, 0 };
            each_live(live_fn, &lr);
            count_bad += ctr.n[VGN_INST_VIS] != lr.kept;
            if (ctr.n[VGN_INST_VIS] != lr.kept && count_bad < 3) printf("  step %d: GPU %u instances vs CPU %u\n", step, ctr.n[VGN_INST_VIS], lr.kept);
            const VgPlacements *p = &g_scene.pl;
            VgInstance *gi = (VgInstance *)malloc(sizeof(VgInstance) * p->inst_cap);
            VgCell *gc = (VgCell *)malloc(sizeof(VgCell) * p->cell_cap);
            vgh_read(&g_h, &g_scene.b_instances, sizeof(VgInstance) * g_scene.instance_count, sizeof(VgInstance) * p->inst_cap, gi);
            vgh_read(&g_h, &g_scene.b_cells, sizeof(VgCell) * g_scene.cell_count, sizeof(VgCell) * p->cell_cap, gc);
            for (u32 c = 0; c < p->cell_cap; c++) {
                VgCell e = p->cells[c];
                if (e.count) e.first += g_scene.instance_count;
                gc[c].count &= VG_CELL_COUNT;   // the GPU copy flags all-foliage cells
                gpu_bad += memcmp(&e, &gc[c], sizeof(VgCell)) != 0;
                for (u32 i = p->cells[c].first; i < p->cells[c].first + p->cells[c].count; i++) gpu_bad += memcmp(&p->inst[i], &gi[i], sizeof(VgInstance)) != 0;
            }
            free(gi);
            free(gc);
            checks++;
        }
    }
    unload_all();
    {   // the last removals reach the GPU too
        VkCommandPool pool;
        VkCommandBuffer cmd = vgc_begin_once(&g_h.ctx, &pool);
        vgeo_scene_placement_record(&g_scene, cmd, 0);
        vgc_submit_wait(&g_h.ctx, pool, cmd);
    }
    bool final_ok = vgeo_scene_placement_audit(&g_scene, err, sizeof(err));
    const VgPlacements *p = &g_scene.pl;
    printf("  %.0f m, %u loads / %u unloads, up to %u live instances (%u chunks max %u), %u audits failed, %u GPU checks: %u table mismatches, %u count / load failures, "
           "%llu uploads (%.1f MB); end: %u free ranges, %u / %u cells free, %u live chunks\n",
           travelled, loads, unloads, max_live, (2 * R + 1) * (2 * R + 1), p->chunk_count, audits_bad, checks, gpu_bad, count_bad,
           (unsigned long long)(p->uploads - up0), (double)p->upload_bytes / (1024.0 * 1024.0), p->free_inst_n, p->free_cells_n, p->cell_cap, p->live_chunks);
    CHECK(travelled >= 2000.0f, "flythrough %.0f m", travelled);
    CHECK(audits_bad == 0 && final_ok, "placement allocator audit failed: %s", err);
    CHECK(gpu_bad == 0 && count_bad == 0, "placement GPU tables: %u mismatches, %u count / load failures", gpu_bad, count_bad);
    CHECK(p->free_inst_n == 1 && p->free_inst[0].first == 0 && p->free_inst[0].count == p->inst_cap && p->free_cells_n == p->cell_cap && p->live_chunks == 0 &&
              p->live_instances == 0,
          "placement region leaked: %u free ranges, %u / %u cells free, %u live", p->free_inst_n, p->free_cells_n, p->cell_cap, p->live_chunks);
    CHECK(p->chunk_count <= (u32)((2 * R + 2) * (2 * R + 2) + 8), "chunk records grew to %u", p->chunk_count);
}

static void test_hiz(void) {
    printf("Hi-Z rejection behind a wall\n");
    VgScatter sc = scatter(-100, -200, 100, -2, 1.5f, 77);
    u32 n = load_area(&sc, -4, -7, 3, -1);
    printf("  scene: %u instances\n", n);
    const Cam cam = { { 0, 1.7f, 0.5f }, { 0, 1.2f, -50 } };
    VgViewGpu v;
    view_for(&cam, 0, 32.0f, &v);
    // a wall 3 m in front of the camera over the whole screen
    mat4 proj = mat4_perspective(FOV, (float)W / (float)H, ZNEAR, 4000.0f);
    float D = 3.0f, wall_z = (proj.m[10] * -D + proj.m[14]) / D;
    float *depth = (float *)malloc(sizeof(float) * W * H);
    for (u32 i = 0; i < W * H; i++) depth[i] = wall_z;
    VgHizImage z;
    vgh_hiz_create(&g_h, &z, W, H);
    vgh_hiz_build(&g_h, &g_cull, &z, depth);
    free(depth);
    VgHizRef ref = vgh_hiz_ref(&z);
    Img m;
    render(&v, &ref, true, false, &m);
    u32 total = vgeo_scene_gpu_instances(&g_scene);
    u8 *mark = (u8 *)calloc(total, 1);
    CellRef r = { &v, 0, 0, mark };
    each_live(cell_fn, &r);
    // instances in frustum the culler tested (cells passing) or not: the rejection over all of them
    u32 vis = m.ctr.n[VGN_INST_VIS];
    double rej = r.in ? 1.0 - (double)vis / (double)r.in : 0.0;
    printf("  %u instances in the frustum, %u visible after Hi-Z (%u cells, %u instances occluded): %.1f %% rejected\n", r.in, vis, m.ctr.n[VGN_CELLS_OCC],
           m.ctr.n[VGN_INST_OCC], rej * 100.0);
    CHECK(r.in > 3000 && rej >= 0.90, "Hi-Z rejected %.1f %% of %u instances behind the wall", rej * 100.0, r.in);
    free(mark);
    vgh_hiz_destroy(&g_h, &z);
    unload_all();
}

int main(int argc, char **argv) {
    bool validation = false;
    for (int i = 1; i < argc; i++) validation |= !strcmp(argv[i], "--validation");
    setvbuf(stdout, NULL, _IONBF, 0);
    if (!vgh_init(&g_h, validation)) { printf("vgeo_foliage: no device, skipped\n"); return 0; }
    u64 t0 = SDL_GetPerformanceCounter();
    vgeo_scene_init(&g_scene, &g_h.ctx);
    VgCullCaps caps = { 1u << 20, 1u << 20, 1u << 21, 1u << 20, 4096 };
    if (!vg_cull_init(&g_cull, &g_h.ctx, &g_scene, &caps)) { printf("FAIL: cull init\n"); return 1; }
    if (!vg_raster_init(&g_rast, &g_h.ctx, &g_cull)) { printf("FAIL: raster init\n"); return 1; }
    g_rast.serialize = true;
    vg_raster_resize(&g_rast, W, H);
    build_content();
    vgeo_scene_reserve_placements(&g_scene, 400000, 4096);
    CHECK(vgeo_scene_commit(&g_scene), "commit");
    for (int k = 0; k < VG_FOLIAGE_KINDS; k++) {
        const VgMesh *m = &g_scene.meshes[g_mesh[k]];
        printf("mesh %s: max WPO %.3f m, fade %.0f-%.0f m, shadow %.0f m, flags %x, impostor %x\n", vgeo_foliage_name(k), m->max_wpo, m->fade_start, m->fade_end,
               m->shadow_max_dist, m->fflags, m->impostor);
    }
    CHECK(g_scene.has_two_sided, "two-sided materials registered");
    test_masked();
    test_wpo_bounds();
    test_density();
    test_cells();
    test_impostor_switch();
    test_streaming();
    test_hiz();
    vg_raster_shutdown(&g_rast);
    vg_cull_shutdown(&g_cull);
    vgeo_scene_free(&g_scene);
    for (int k = 0; k < VG_FOLIAGE_KINDS; k++) {
        vgeo_free(&g_built[k]);
        free(g_mask_mips[k][0]);
        free(g_mask_mips[k][1]);
    }
    vgh_shutdown(&g_h);
    if (validation) CHECK(g_vgh_vk_messages == 0, "%d validation messages", g_vgh_vk_messages);
    printf("vgeo_foliage: %d/%d checks passed (%.1f s)\n", g_checks - g_fail, g_checks, (double)(SDL_GetPerformanceCounter() - t0) / (double)SDL_GetPerformanceFrequency());
    return g_fail ? 1 : 0;
}
