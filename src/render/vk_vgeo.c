// The vgeo scene content in the frame (docs/CLASSIC_RENDERER.md): the scene tables (meshes, materials,
// instances, foliage placement chunks) that the classic instanced renderer (vk_classic.c) draws, the
// level placements, the procedural / imported foliage types, the grass assets, the foliage wind
// (wind field tile, displacement buffer) and the era presets of the discrete LODs. The virtualized
// geometry renderer that used to draw the same tables (GPU culling, cluster raster, visibility
// buffer, page streaming) lives on branch `nanite` (docs/VGEO.md there).
//
// The scene is CPU only (no GPU context: vgeo_scene_commit bins and derives, uploads nothing); the
// vgeo module context still runs the displacement buffer, the wind tile and the module textures.
#include "vk_internal.h"
#include <SDL3/SDL.h>
#include "vgeo/vgeo_scene.h"
#include "vgeo/vgeo_tex.h"
#include "vgeo/vgeo_procgen.h"
#include "vgeo/vgeo_import.h"
#include "vgeo/foliage_support.h"
#include "vgeo/vgeo_procgen_foliage.h"
#include "vgeo/vgeo_impostor.h"
#include "vgeo/vgeo_tree.h"
#include "vgeo/grass_asset.h"
#include "core/cvar.h"
#include "fidelity/fidelity.h"
#include "assets/assets.h"
#include "core/platform.h"
#include "core/jobs.h"
#include "stb_image.h"
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FOLIAGE_TYPES 64       // registered foliage types: grass variants, trees, bushes, flowers, crops

static struct {
    bool ok;
    VgContext ctx;               // the module's device context (displacement buffer, wind tile, textures)
    VgScene scene;               // CPU tables only (vgeo_scene_init without a context)
    VgTextures tex;              // module textures (masks, tree maps, impostor atlases, displacement buffer)
    struct { char name[96]; u32 mesh; } named[64];   // level meshes loaded so far (by name)
    u32 named_count;
    bool applying_era;
    Cvar *era, *lod_px, *foliage_density, *shadow_error_px, *shadows;
    // Foliage (docs/FOLIAGE_WIND.md): wind of the previous frame (velocity), the displacement
    // buffer, foliage types by name (placement chunks resolve their mesh names here).
    Cvar *impostor_px, *foliage_wind, *foliage_wind_solo;
    float wind_cur[4], wind_prev[4], time_cur, time_prev;
    bool wind_init;
    VgWindTex wind;              // the wind field's tile (docs/FOLIAGE_WIND.md)
    bool wind_ok;
    VgDisplace disp;
    bool disp_ok;
    // role / weight: the scatter role of a tree type (RENDER_TREE_*, -1 none) and its pick weight
    struct FoliageType { char name[48]; u32 mesh; bool capsule; float cap_r, cap_h; int role; float weight; } ftype[FOLIAGE_TYPES];
    u32 ftype_count;
    bool test_types, map_types;
    u32 tree_roles;              // roles whose imported species were loaded (bits of VgTreeRole)
    struct { char name[64]; u32 tex; } tree_tex[80];   // imported tree textures by file (+ "#n" / "#orm" bark maps; shared by species)
    u32 tree_tex_count;
    size_t tree_tex_bytes;
    double disp_time;
    float push_test;             // foliage_push_test: seconds left
    bool need_commit;            // a placement chunk resolved a mesh the scene did not have
} G;

// Grass assets (the grass section below).
static struct {
    GrassAsset a[GRASS_MAX_ASSETS];
    int n;
    u32 mat[GRASS_MAX_ASSETS];       // scene material index (~0: the asset's atlas is missing)
    int tier;
    bool built;
    Cvar *r_grass_tex;
} GR;

static void fallback_register(u32 mesh, const VgBuilt *b, bool persistent);
static void vkg_register_era(void);
static void cmd_foliage_push_test(int argc, const char **argv);
static void cmd_foliage_stats(int argc, const char **argv);
static void cmd_foliage_query(int argc, const char **argv);
static void cmd_classic_stats(int argc, const char **argv) { (void)argc; (void)argv; vkc_stats(); }

// The material sampler changed: the classic renderer re-wraps the module texture slots.
void vkg_samplers_changed(void) { vkc_samplers_changed(); }

// ------------------------------------------------------------------ lifetime

void vkg_init(void) {
    G.shadow_error_px = cvar_reg_float("r_vgeo_shadow_error_px", 2.0f, 0.25f, 32.0f, CVAR_TIER,
                                       "classic renderer: projected LOD error of the sun shadow cascades (pixels)");
    G.shadows = cvar_reg_bool("r_vgeo_shadows", true, CVAR_TIER, "classic renderer: draw the scene content (level meshes, foliage) into the sun / light shadows");
    G.impostor_px = cvar_reg_float("r_vgeo_impostor_px", 16.0f, 0.0f, 256.0f, CVAR_LOCKED,
                                   "small cover (grass tufts, bushes) with an impostor atlas draws it below this projected size (pixels across, 0 = never)");
    G.foliage_wind = cvar_reg_int("r_foliage_wind", 3, 0, 3, CVAR_TIER,
                                  "foliage wind animation (docs/FOLIAGE_WIND.md): 0 none, 1 main bending, 2 + branch bending, 3 + leaf flutter and bent normals");
    G.foliage_wind_solo = cvar_reg_int("r_foliage_wind_solo", 0, 0, 3, CVAR_LOCKED, "foliage wind debug: 0 every layer (r_foliage_wind), 1 main bending only, 2 detail only, 3 flutter only");
    cmd_register("foliage_push_test", cmd_foliage_push_test, "foliage_push_test [seconds=10]: a ring of displacement-buffer pushes circling the camera");
    cmd_register("foliage_stats", cmd_foliage_stats, "foliage: placement chunks, region allocator audit");
    cmd_register("foliage_query", cmd_foliage_query, "foliage_query [radius=2]: placement instances around the camera (foliage_query API)");
    cmd_register("classic_stats", cmd_classic_stats, "classic instanced renderer: meshes, GPU memory, last frame's instances / batches / draws, CPU cull ms");
    vkg_register_era();
    VgContext *c = &G.ctx;
    c->gpu = R->gpu;
    c->device = R->device;
    c->queue = R->queue;
    c->queue_family = R->queue_family;
    c->vma = R->vma;
    c->frames = FRAMES_IN_FLIGHT;
    c->timestamp_period = R->gpu_props.limits.timestampPeriod;
    c->load_shader = vkp_load_shader;
    vgeo_scene_init(&G.scene, NULL);   // CPU tables: the classic renderer reads them, nothing is uploaded
    if (!vg_tex_init(&G.tex, c)) {
        log_error("vgeo: module textures unavailable (no scene content)");
        vg_tex_shutdown(&G.tex);
        return;
    }
    G.disp_ok = vg_displace_init(&G.disp, c, &G.tex);
    if (!G.disp_ok) log_warn("vgeo: foliage displacement buffer unavailable");
    G.wind_ok = vg_wind_init(&G.wind, c, &G.tex);
    if (!G.wind_ok) log_warn("vgeo: wind field tile unavailable (foliage uses the mean wind)");
    G.ok = true;
    vkc_init();
    vkc_set_textures(&G.tex);
}

void vkg_shutdown(void) {
    if (!G.ok) return;
    vkc_shutdown();
    vkDeviceWaitIdle(R->device);
    if (G.disp_ok) vg_displace_shutdown(&G.disp);
    if (G.wind_ok) vg_wind_shutdown(&G.wind);
    G.wind_ok = false;
    G.disp_ok = false;
    vg_tex_shutdown(&G.tex);
    vgeo_scene_free(&G.scene);
    vgeo_file_release_all();
    G.ok = false;
}

// ------------------------------------------------------------------ foliage wind (docs/FOLIAGE_WIND.md)

// The wind outside the field's tile: instant x, z, settled bend x, z.
static void foliage_wind_frame(void) {
    const RenderWeather *wx = &R->weather;
    float w[4] = { wx->wind_global.x, wx->wind_global.y, wx->wind_global.z, wx->wind_global.w };
    memcpy(G.wind_prev, G.wind_init ? G.wind_cur : w, 16);
    G.time_prev = G.wind_init ? G.time_cur : wx->time;
    memcpy(G.wind_cur, w, 16);
    G.time_cur = wx->time;
    G.wind_init = true;
}

// The foliage fields of a view (WPO wind / time this frame and last, density, impostor switch,
// displacement buffer, dither rotation).
static void view_foliage(VgViewGpu *v) {
    memcpy(v->wind, G.wind_cur, 16);
    memcpy(v->wind_prev, G.wind_prev, 16);
    v->foliage[0] = G.time_cur;
    v->foliage[1] = G.time_prev;
    v->foliage[2] = G.foliage_density->f;
    v->foliage[3] = G.impostor_px->f;
    v->frame_index = R->frame_index;
    if (G.disp_ok) vg_displace_view(&G.disp, v);
    if (G.wind_ok) vg_wind_view(&G.wind, v);
    // layers: the fidelity tier (r_foliage_wind), or one layer solo (tuning); the debug view
    int solo = R->weather.wind_solo ? R->weather.wind_solo : G.foliage_wind_solo->i;
    v->wind_layers = solo == 1 ? VGW_MAIN : solo == 2 ? VGW_DETAIL : solo == 3 ? VGW_FLUTTER : vgeo_wind_tier_layers(G.foliage_wind->i);
    v->wind_debug = (u32)DG_CLAMP(R->weather.wind_view, 0, 2);
}

// The frame's wind for the classic renderer (after the tile upload).
static void classic_wind(void) {
    VgViewGpu v;
    memset(&v, 0, sizeof(v));
    view_foliage(&v);
    vkc_wind_frame(&v, G.wind_ok ? &G.wind : NULL);
}

// The foliage displacement buffer around the camera (WPO_DISPLACE reads it).
static void displace_frame(VkCommandBuffer cmd) {
    u32 frame = R->frame_slot;
    if (G.disp_ok) {
        double now = (double)SDL_GetTicksNS() * 1e-9;
        float dt = G.disp_time > 0 ? (float)fmin(now - G.disp_time, 0.1) : 0.0f;
        G.disp_time = now;
        if (G.push_test > 0) {   // foliage_push_test: 6 pushes on a 4 m ring circling the camera
            G.push_test -= dt;
            for (int k = 0; k < 6; k++) {
                float a = (float)now * 1.3f + (float)k * 1.0471976f;
                float p[3] = { R->cam.pos.x + cosf(a) * 4.0f, R->cam.pos.y, R->cam.pos.z + sinf(a) * 4.0f };
                vg_displace_push(&G.disp, p, 1.5f, 3.0f);
            }
        }
        float cam[3] = { R->cam.pos.x, R->cam.pos.y, R->cam.pos.z };
        vg_displace_record(&G.disp, cmd, frame, cam, dt);
    }
    // the wind field's tile (the game updated it this frame)
    if (G.wind_ok) vg_wind_record(&G.wind, cmd, frame, R->weather.wind_texels, R->weather.wind_ox, R->weather.wind_oz, R->weather.wind_texel);
}

// Before the shadow pass (outside rendering): the frame's wind, the displacement buffer around the
// camera, the wind tile upload, then the classic renderer's wind set.
void vkg_record_foliage(VkCommandBuffer cmd) {
    if (!G.ok) return;
    foliage_wind_frame();
    displace_frame(cmd);
    classic_wind();
}

// ------------------------------------------------------------------ level placements

static bool name_is_file(const char *n) {
    size_t l = strlen(n);
    return l > 5 && !strcmp(n + l - 5, ".vgeo");
}

// Mesh index of a placement's mesh (loaded on first use), ~0 when unavailable.
static u32 level_mesh(const RenderVgeoPlacement *pl) {
    for (u32 i = 0; i < G.named_count; i++)
        if (!strcmp(G.named[i].name, pl->mesh)) return G.named[i].mesh;
    if (G.named_count >= 64) return ~0u;
    const VgBuilt *src = NULL;
    char file_path[512] = "";
    VgMaterial mat;
    memset(&mat, 0, sizeof(mat));
    mat.color[0] = pl->color.x;
    mat.color[1] = pl->color.y;
    mat.color[2] = pl->color.z;
    mat.color[3] = 1.0f;
    mat.roughness = pl->roughness;
    mat.tex_scale = 1.0f;
    const Model *model = NULL;
    if (name_is_file(pl->mesh)) {
        src = vgeo_file_get(platform_data_path(pl->mesh, file_path, sizeof(file_path)));
        if (!src) { log_warn("vgeo: %s missing or invalid (generated at build time by tools/build_vgeo)", pl->mesh); return ~0u; }
    } else {
        model = assets_model(assets_model_id(pl->mesh));
        if (!model || !model->vgeo) { log_warn("vgeo: model '%s' has no vgeo data (\"vgeo\": true in data/assets.json)", pl->mesh); return ~0u; }
        src = model->vgeo;
    }
    // .vgeo files stream their pages from the file (the pool holds what the views need)
    u32 mesh = file_path[0] ? vgeo_scene_add_mesh_file(&G.scene, src, file_path) : vgeo_scene_add_mesh(&G.scene, src);
    fallback_register(mesh, src, true);
    if (file_path[0]) vgeo_file_drop_pages(file_path);   // pages stream from the file from now on
    u32 nm = src->hdr.material_count ? src->hdr.material_count : 1, base = 0;
    for (u32 k = 0; k < nm; k++) {
        VgMaterial m = mat;
        if (model && model->mesh_count) {
            const Material *mm = &model->meshes[k < (u32)model->mesh_count ? k : (u32)model->mesh_count - 1].mat;
            m.color[0] = mm->color.x;
            m.color[1] = mm->color.y;
            m.color[2] = mm->color.z;
            m.roughness = mm->roughness;
            m.metallic = mm->metallic;
            m.emissive = mm->emissive;
            m.flags = mm->flags & (MAT_TEXTURED | MAT_TRIPLANAR | MAT_GRID | MAT_CHECKER);
            m.tex_scale = mm->tex_scale > 0 ? mm->tex_scale : 1.0f;
            if ((mm->flags & MAT_TEXTURED) && mm->texture) { m.albedo_tex = mm->texture; }
            // glTF maps (docs/PBR.md): the resolve samples them through the texture table.
            const u32 map_flag[4] = { MAT_MR_TEX, MAT_NORMAL_TEX, MAT_OCCLUSION_TEX, MAT_EMISSIVE_TEX };
            const TextureId map_tex[4] = { mm->mr_tex, mm->normal_tex, mm->occlusion_tex, mm->emissive_tex };
            u32 *map_dst[4] = { &m.mr_tex, &m.normal_tex, &m.occlusion_tex, &m.emissive_tex };
            for (int t = 0; t < 4; t++)
                if ((mm->flags & map_flag[t]) && map_tex[t]) { m.flags |= map_flag[t]; *map_dst[t] = map_tex[t]; }
            m.normal_scale = mm->normal_scale;
            m.occlusion_strength = mm->occlusion_strength;
            m.emissive_rgb = mm->emissive_rgb;
            // alphaMode MASK / doubleSided: the vgeo masked / two-sided paths (every raster path and the shadows)
            if (mm->flags & MAT_ALPHA_MASK) { m.flags |= VGM_MASKED; m.mask_tex = m.albedo_tex; m.alpha_cutoff = mm->alpha_cutoff; }
            if (mm->flags & (MAT_TWO_SIDED | MAT_ALPHA_MASK)) m.flags |= VGM_TWO_SIDED | MAT_TWO_SIDED;
            const ModelFoliage *fo = &model->foliage;
            if (fo->enabled) {   // manifest "foliage": the vgeo material flags and WPO parameters
                m.flags |= VGM_FOLIAGE | (fo->masked ? VGM_MASKED : 0) | (fo->two_sided ? VGM_TWO_SIDED : 0) | (fo->dither ? VGM_DITHER_FADE : 0);
                m.mask_tex = m.albedo_tex;
                m.alpha_cutoff = fo->alpha_cutoff;
                m.wpo = (u32)fo->wpo | (fo->displace ? WPO_DISPLACE : 0);
                memcpy(m.wpo_params, fo->wpo_params, 16);
                m.fade_start = fo->fade_start;
                m.fade_end = fo->fade_end;
                m.shadow_max_dist = fo->shadow_max_dist;
            }
            vg_material_set_ao(&m, mm->ao_strength, mm->ao_combine, mm->ao_vertex);   // with the final VGM_MASKED
        }
        u32 idx = vgeo_scene_add_material(&G.scene, &m);
        if (k == 0) base = idx;
    }
    vgeo_scene_set_mesh_materials(&G.scene, mesh, base);
    log_info("vgeo: level mesh %s: %u clusters, %u pages, %u tris", pl->mesh, src->hdr.cluster_count, src->hdr.page_count, src->stats.input_tris);
    dg_strcpy(G.named[G.named_count].name, sizeof(G.named[0].name), pl->mesh);
    G.named[G.named_count++].mesh = mesh;
    return mesh;
}

void render_vgeo_set_level(const RenderVgeoPlacement *p, int count) {
    if (!G.ok) return;
    vgeo_scene_clear_instances(&G.scene);
    u32 total = 0;
    for (int i = 0; i < count; i++) {
        const RenderVgeoPlacement *pl = &p[i];
        u32 mesh = level_mesh(pl);
        if (mesh == ~0u) continue;
        u32 cap = (pl->grid[0] ? pl->grid[0] : 1) * (pl->grid[1] ? pl->grid[1] : 1);
        VgPlacementXf *xf = (VgPlacementXf *)malloc(sizeof(VgPlacementXf) * cap);
        float pp[3] = { pl->pos.x, pl->pos.y, pl->pos.z };
        u32 n = vgeo_placement_expand(pp, pl->yaw, pl->scale, pl->scale_jitter, pl->spacing, pl->grid[0], pl->grid[1], pl->seed, xf, cap);
        for (u32 k = 0; k < n; k++) {
            vgeo_scene_add_instance(&G.scene, mesh, xf[k].pos, xf[k].rot, xf[k].scale);
            G.scene.instances[G.scene.instance_count - 1].flags_phase |= (pl->shadow_flags & 0xFFFFu) << 16;   // Cast / Receive / layer
        }
        total += n;
        free(xf);
    }
    if (!vgeo_scene_commit(&G.scene)) log_error("vgeo: scene commit failed");
    if (count) log_info("vgeo: level: %u instances of %u meshes, %u cells", total, G.named_count, G.scene.cell_count);
}

// ------------------------------------------------------------------ era presets

// r_era: the shadow-view geometry error per era; any other combination reads back as Custom (4)
// (the era owning the current value wins a tie: 2015 shares modern's). r_era is locked (modern)
// outside dev builds; the era's old foliage density and LOD columns are gone: one locked value for
// every era and level (assets/render/consistency.json, docs/SETTINGS_AUDIT.md). What else an era
// changes is its capability matrix (assets/render/fidelity/eras.json) and, for 2010, the dropped
// finest LOD (vk_classic.c era2010).
typedef struct EraDef { float shadow_px; } EraDef;
static const EraDef k_eras[4] = {
    { 2.0f },   // modern
    { 4.0f },   // 2020: coarser shadow geometry
    { 2.0f },   // 2015: 4 discrete LODs
    { 3.0f },   // 2010: 3 LODs
};

// Compared after the Engine Fidelity bounds (docs/ENGINE_FIDELITY.md): a lower level's own value of
// the era still reads back as the era.
static bool era_eq(int f, float v, float era_v) {
    if (fabsf(v - fid_clamp_current(f, era_v)) < 1e-4f) return true;
    for (int l = 0; l < fid_level(); l++) {
        FidBounds b = fid_bounds(l, fid_era(), f);
        if (fabsf(v - fid_clamp_current(f, fid_clamp(&b, fid_feature(f), era_v))) < 1e-4f) return true;
    }
    return false;
}

static int era_detect(void) {
    // The current era first: 2015 and modern share their value.
    int cur = G.era ? G.era->i : -1;
    if (cur >= 0 && cur <= 3 && era_eq(FF_VGEO_SHADOW_ERROR_PX, G.shadow_error_px->f, k_eras[cur].shadow_px)) return cur;
    for (int e = 0; e < 4; e++) {
        const EraDef *d = &k_eras[e];
        if (era_eq(FF_VGEO_SHADOW_ERROR_PX, G.shadow_error_px->f, d->shadow_px)) return e;
    }
    return 4;
}

static void era_apply(int e) {
    const EraDef *d = &k_eras[DG_CLAMP(e, 0, 3)];
    G.applying_era = true;
    CvarSource src = cvar_source_push(CVAR_SRC_FIDELITY);   // tier cvars
    cvar_set_float(G.shadow_error_px, d->shadow_px);
    cvar_source_pop(src);
    G.applying_era = false;
}

static void on_era(Cvar *c) {
    if (G.applying_era) return;
    if (c->i >= 0 && c->i <= 3 && era_detect() != c->i) era_apply(c->i);
    fid_enforce_all();   // the era's capability matrix (what it lacks is unavailable at every level)
}

static void on_era_value(Cvar *c) {
    (void)c;
    if (G.applying_era || !G.era) return;
    int e = era_detect();
    if (G.era->i != e) {
        G.applying_era = true;
        CvarSource src = cvar_source_push(CVAR_SRC_HOST);   // r_era is locked: the read-back is the game's
        cvar_set_int(G.era, e);
        cvar_source_pop(src);
        G.applying_era = false;
    }
}

static void vkg_register_era(void) {
    G.lod_px = cvar_reg_float("r_vgeo_lod_px", 2.0f, 0.25f, 64.0f, CVAR_LOCKED, "classic renderer: projected error of the discrete LOD pick (pixels)");
    G.foliage_density = cvar_reg_float("r_foliage_density", 1.0f, 0.0f, 1.0f, CVAR_ARCHIVE | CVAR_LOCKED | CVAR_RULE, "foliage instance density");
    GR.r_grass_tex = cvar_reg_int("r_grass_tex", 0, 0, 1, CVAR_TIER, "grass atlases at full size (1: 2048 px, about 4x the memory; 0: half, at the next map load)");
    G.era = cvar_reg_int("r_era", 0, 0, 4, CVAR_LOCKED, "geometry era preset: 0 modern, 1 2020 (coarser shadow geometry), 2 2015 (4 discrete LODs), 3 2010 (3 LODs), 4 custom");
    G.era->on_change = on_era;
    // Era-owned Engine Fidelity features take their defaults from this table.
    for (int e = 0; e < 4; e++) {
        fid_set_era_value(e, FF_VGEO_SHADOW_ERROR_PX, k_eras[e].shadow_px);
    }
    G.shadow_error_px->on_change = on_era_value;
    on_era_value(NULL);
}

// Every scene mesh goes to the classic renderer (its LOD chain, impostor textures).
static void fallback_register(u32 mesh, const VgBuilt *b, bool persistent) { vkc_register_mesh(mesh, b, persistent); }

// Once per frame after the camera is set (render.c): the classic renderer's culling, LOD selection
// and instance batches for the main view, the sun cascades and the scope PiP.
void vkg_classic_frame(void) {
    if (!G.ok) return;
    vkc_frame(&G.scene, G.disp_ok ? &G.disp : NULL, G.foliage_density->f, G.lod_px->f, G.shadow_error_px->f, G.impostor_px->f, G.era && G.era->i == 3, G.shadows->i != 0);
}

// ------------------------------------------------------------------ point / spot light shadows

// Local light shadow tile (vk_light_shadow.c): the classic renderer's instances near the light.
// Inside the atlas pass, PIPE_SHADOW bound by the caller.
u32 vkg_draw_local_shadow(VkCommandBuffer cmd, vec4 sphere, const vec4 planes[6], u32 cascade, u32 tile_px, u32 layers) {
    if (!G.ok || !G.shadows->i) return 0;
    return vkc_draw_local_shadow(cmd, sphere, planes, cascade, tile_px, layers);
}

// ------------------------------------------------------------------ foliage (docs/VGEO.md "Foliage support")

static int foliage_type_of_mesh(u32 mesh) {
    for (u32 i = 0; i < G.ftype_count; i++)
        if (G.ftype[i].mesh == mesh) return (int)i;
    return -1;
}

// ------------------------------------------------------------------ wind tuning (docs/FOLIAGE_WIND.md)

int render_foliage_type_count(void) { return G.ok ? (int)G.ftype_count : 0; }
const char *render_foliage_type_name(int i) { return G.ok && i >= 0 && (u32)i < G.ftype_count ? G.ftype[i].name : NULL; }

static void foliage_commit(void);

// The wind materials of a type's mesh: [*first, *first + count).
static bool type_wind_materials(const char *type, u32 *mesh, u32 *first, u32 *count) {
    if (!G.ok || !type) return false;
    for (u32 i = 0; i < G.ftype_count; i++)
        if (!strcmp(G.ftype[i].name, type)) {
            *mesh = G.ftype[i].mesh;
            *first = G.scene.meshes[*mesh].material_base;
            *count = G.scene.mesh_mat_count[*mesh];
            return true;
        }
    return false;
}

bool render_foliage_wind_get(const char *type, RenderFoliageWind *out) {
    u32 mesh, first, count;
    if (!type_wind_materials(type, &mesh, &first, &count)) return false;
    for (u32 k = first; k < first + count && k < G.scene.material_count; k++) {
        const VgMaterial *mt = &G.scene.materials[k];
        if (mt->wpo == WPO_NONE) continue;
        VgWindParams p;
        vgeo_wind_unpack(mt->wpo_params, mt->wind_params, &p);
        _Static_assert(sizeof(RenderFoliageWind) == sizeof(VgWindParams), "RenderFoliageWind mirrors VgWindParams");
        memcpy(out, &p, sizeof(p));
        return true;
    }
    return false;
}

// Live tuning: every wind material of the type takes the parameters (the classic draws read them from
// the next frame on). A larger bound than the mesh carries re-commits the scene (the culling spheres).
bool render_foliage_wind_set(const char *type, const RenderFoliageWind *w) {
    u32 mesh, first, count;
    if (!type_wind_materials(type, &mesh, &first, &count)) return false;
    VgWindParams p;
    memcpy(&p, w, sizeof(p));
    bool grow = false;
    for (u32 k = first; k < first + count && k < G.scene.material_count; k++) {
        VgMaterial *mt = &G.scene.materials[k];
        if (mt->wpo == WPO_NONE) continue;
        vgeo_wind_pack(&p, mt->wpo_params, mt->wind_params);
        mt->wpo_max = vgeo_wpo_max(mt);
        grow |= mt->wpo_max > G.scene.meshes[mesh].max_wpo;
    }
    if (grow) {
        G.need_commit = true;
        foliage_commit();
    }
    return true;
}

// The placement region (1M instances = 32 MB, 65536 cells: an open-world map's trees fill thousands
// of chunks), reserved on first use.
static void foliage_region(void) {
    if (G.scene.pl.inst_cap) return;
    vgeo_scene_reserve_placements(&G.scene, 1u << 20, 65536);
    G.need_commit = true;
}

static void foliage_commit(void) {
    if (!G.need_commit) return;
    G.need_commit = false;
    if (!vgeo_scene_commit(&G.scene)) log_error("vgeo: scene commit failed");
}

// Builds procedural foliage kinds [k0, k1) (vgeo_procgen_foliage.c): mesh, masks, impostor atlas,
// materials, the type table (trunk capsules).
// One kind's CPU work: the mesh / masks (generated serially: the generators share a seed state),
// then the cluster build and the impostor bake (the slow parts) in parallel on the job system.
typedef struct FoliageKindBuild {
    int k;
    VgFoliageMesh fm;
    VgSource src;
    VgBuilt b;
    bool ok, impostor;
    char err[256];
    VgMaterial mats[2];
    u32 nm;
    u8 *masks[2];
    u32 msz[2];             // mask size (px): the card textures load at their own size
    const u8 *mk[2];
    VgImpostorMaterial im[2];
    VgImpostorOptions io;
} FoliageKindBuild;

static void foliage_kind_job(void *ctx, int begin, int end) {
    FoliageKindBuild *kb = (FoliageKindBuild *)ctx;
    for (int i = begin; i < end; i++) {
        VgBuildOptions opt;
        memset(&opt, 0, sizeof(opt));
        kb[i].ok = vgeo_build(&kb[i].src, &opt, &kb[i].b, kb[i].err, sizeof(kb[i].err));
        if (kb[i].ok && kb[i].impostor) vgeo_impostor_bake(&kb[i].b, &kb[i].io);
    }
}

// Card textures drawn by tools/vfxgen/foliage_atlas.py (docs/TEXTURES.md) for the grass tuft and the
// wheat clump: RGBA (sRGB colour, coverage alpha), square. NULL (procedural mask) when missing.
static u8 *foliage_card_file(int kind, u32 material, u32 *size) {
    const char *name = material ? NULL : (kind == VG_FOLIAGE_TUFT ? "grass_blades" : (kind == VG_FOLIAGE_CROP ? "wheat" : NULL));
    if (!name) return NULL;
    char rel[96], path[512];
    snprintf(rel, sizeof(rel), "assets/textures/foliage/%s.png", name);
    int w, h, comp;
    u8 *px = stbi_load(platform_data_path(rel, path, sizeof(path)), &w, &h, &comp, 4);
    if (!px) return NULL;
    if (w != h || w < 16 || w > 2048) { stbi_image_free(px); return NULL; }
    u8 *out = (u8 *)malloc((size_t)w * (size_t)h * 4);
    memcpy(out, px, (size_t)w * (size_t)h * 4);
    stbi_image_free(px);
    *size = (u32)w;
    return out;
}

// Builds procedural foliage kinds [k0, k1) (vgeo_procgen_foliage.c): mesh, masks, impostor atlas
// (atlas px, 8 x 8 frames), materials, the type table (trunk capsules).
static void foliage_build_kinds(int k0, int k1, u32 atlas) {
    const u32 msize = 256;
    u32 levels;
    size_t mb = vg_tex_mip_bytes(msize, msize, &levels);
    u8 *mips = (u8 *)malloc(mb);
    int n = k1 - k0;
    FoliageKindBuild *kb = (FoliageKindBuild *)calloc((size_t)n, sizeof(FoliageKindBuild));
    for (int i = 0; i < n; i++) {
        FoliageKindBuild *q = &kb[i];
        q->k = k0 + i;
        vgeo_gen_foliage(&q->fm, q->k, 11 + (u32)q->k);
        vgeo_foliage_source(&q->fm, &q->src);
        q->nm = vgeo_foliage_materials(q->k, q->mats);
        for (u32 m = 0; m < q->nm; m++) {
            u32 fs = 0;
            u8 *card = foliage_card_file(q->k, m, &fs);
            if (card) {   // grass blades / wheat drawn offline
                q->masks[m] = card;
                q->msz[m] = fs;
                q->mk[m] = card;
                continue;
            }
            q->masks[m] = (u8 *)malloc((size_t)msize * msize * 4);
            q->msz[m] = msize;
            if (vgeo_gen_foliage_mask(q->masks[m], msize, q->k, m, 5)) q->mk[m] = q->masks[m];
        }
        vgeo_foliage_impostor_materials(q->k, q->mk, msize, q->im);
        for (u32 m = 0; m < q->nm; m++)
            if (q->mk[m]) q->im[m].w = q->im[m].h = q->msz[m];
        q->io = (VgImpostorOptions){ atlas, 8, q->im, 2 };
        q->impostor = q->k != VG_FOLIAGE_CROP;   // crops fade out before an impostor would be used
    }
    jobs_parallel_for(n, 1, foliage_kind_job, kb);
    for (int i = 0; i < n && G.ftype_count < FOLIAGE_TYPES; i++) {
        FoliageKindBuild *q = &kb[i];
        vgeo_foliage_mesh_free(&q->fm);
        if (!q->ok) { log_error("vgeo: %s build failed: %s", vgeo_foliage_name(q->k), q->err); continue; }
        // alpha masks (module textures)
        for (u32 m = 0; m < q->nm; m++) {
            if (!q->mk[m]) continue;
            u32 sz = q->msz[m], lv = levels;
            u8 *mp = mips;
            if (sz != msize) mp = (u8 *)malloc(vg_tex_mip_bytes(sz, sz, &lv));
            vg_tex_build_mips(q->masks[m], sz, sz, mp);
            u32 t = vg_tex_create(&G.tex, sz, sz, lv, mp, true, true);
            if (mp != mips) free(mp);
            q->mats[m].mask_tex = q->mats[m].albedo_tex = t;
        }
        u32 mesh = vgeo_scene_add_mesh(&G.scene, &q->b);
        fallback_register(mesh, &q->b, false);
        u32 base = 0;
        for (u32 m = 0; m < q->nm; m++) {
            u32 idx = vgeo_scene_add_material(&G.scene, &q->mats[m]);
            if (!m) base = idx;
        }
        vgeo_scene_set_mesh_materials(&G.scene, mesh, base);
        if (q->impostor) vgeo_foliage_impostor_upload(&G.scene, &G.tex, mesh, &q->b);
        struct FoliageType *ft = &G.ftype[G.ftype_count++];
        dg_strcpy(ft->name, sizeof(ft->name), vgeo_foliage_name(q->k));
        ft->mesh = mesh;
        ft->capsule = vgeo_foliage_capsule(q->k, &ft->cap_r, &ft->cap_h);   // the bush / tree trunks
        ft->role = -1;   // ground cover, not a tree role
        ft->weight = 0;
        log_info("vgeo: foliage type %s: %u tris, %u clusters, %u levels, impostor %s", ft->name, q->b.stats.input_tris, q->b.hdr.cluster_count, q->b.hdr.level_count,
                 q->impostor ? "8x8 frames" : "none");
        vgeo_free(&q->b);
    }
    for (int i = 0; i < n; i++) {
        free(kb[i].masks[0]);
        free(kb[i].masks[1]);
    }
    free(kb);
    free(mips);
}

// ------------------------------------------------------------------ grass assets (grass_asset.h, docs/FOLIAGE_GRASS.md)

// Every assets/foliage/grass asset: its atlas + nta map (module textures), one material (shared by its
// mesh variants; MAT_COVER with the asset's cover id), `variants` clump meshes registered as foliage types
// "<asset>_v<k>" (the placement uses the same names), an impostor each (not for the fields' wheat).
// The material's fades / shadow distance follow the fidelity level (grass_update_tier).

typedef struct GrassBuild {
    int asset;
    u32 variant;
    VgFoliageMesh fm;
    VgSource src;
    VgBuilt b;
    bool ok, impostor;
    char err[256];
    VgImpostorMaterial im;
    VgImpostorOptions io;
} GrassBuild;

static void grass_job(void *ctx, int begin, int end) {
    GrassBuild *gb = (GrassBuild *)ctx;
    for (int i = begin; i < end; i++) {
        VgBuildOptions opt;
        memset(&opt, 0, sizeof(opt));
        gb[i].ok = vgeo_build(&gb[i].src, &opt, &gb[i].b, gb[i].err, sizeof(gb[i].err));
        if (gb[i].ok && gb[i].impostor) vgeo_impostor_bake(&gb[i].b, &gb[i].io);
    }
}

// assets/foliage/tex/<atlas>_<suffix>.png, box-downsampled by div (a power of two). NULL when missing.
static u8 *grass_tex_load(const char *atlas, const char *suffix, u32 div, u32 *w, u32 *h) {
    char rel[160], path[512];
    snprintf(rel, sizeof(rel), "assets/foliage/tex/%s_%s.png", atlas, suffix);
    int iw, ih, comp;
    u8 *px = stbi_load(platform_data_path(rel, path, sizeof(path)), &iw, &ih, &comp, 4);
    if (!px) return NULL;
    u32 ow = DG_MAX((u32)iw / div, 1u), oh = DG_MAX((u32)ih / div, 1u);
    u8 *out = (u8 *)malloc((size_t)ow * oh * 4);
    for (u32 y = 0; y < oh; y++)
        for (u32 x = 0; x < ow; x++)
            for (int c = 0; c < 4; c++) {
                u32 acc = 0;
                for (u32 dy = 0; dy < div; dy++)
                    for (u32 dx = 0; dx < div; dx++) acc += px[(((size_t)(y * div + dy)) * (u32)iw + x * div + dx) * 4 + c];
                out[((size_t)y * ow + x) * 4 + c] = (u8)((acc + div * div / 2) / (div * div));
            }
    stbi_image_free(px);
    *w = ow;
    *h = oh;
    return out;
}

static u32 grass_tex_upload(const u8 *px, u32 w, u32 h, bool srgb) {
    u32 levels;
    u8 *mips = (u8 *)malloc(vg_tex_mip_bytes(w, h, &levels));
    vg_tex_build_mips(px, w, h, mips);
    u32 t = vg_tex_create(&G.tex, w, h, levels, mips, srgb, true);
    free(mips);
    return t;
}

static int grass_fidelity_tier(void) {
    Cvar *c = cvar_find("r_fidelity");
    return c ? DG_CLAMP(c->i, 0, GRASS_TIERS - 1) : GRASS_TIERS - 1;
}

// Ground colour tables (render_ground_*): the grass tops' colour and coverage per terrain material,
// density-weighted over the assets that grow there.
static void grass_ground_tops(void) {
    for (int m = 0; m < GRASS_MAX_MATERIALS; m++) {
        float w = 0, c[3] = { 0, 0, 0 }, cov = 0;
        for (int i = 0; i < GR.n; i++) {
            float d = GR.a[i].density[m];
            if (d <= 0 || GR.mat[i] == ~0u) continue;
            for (int k = 0; k < 3; k++) c[k] += GR.a[i].top_color[k] * d;
            cov += d * GR.a[i].coverage;
            w += d;
        }
        if (w > 0) {
            for (int k = 0; k < 3; k++) c[k] /= w;
            render_ground_set_top(m, c, DG_MIN(cov / 2.5f, 1.0f));   // 2.5 clumps / m2 of an asset cover its share fully
        } else render_ground_set_top(m, NULL, 0.0f);
    }
}

static void foliage_build_grass(void) {
    if (GR.built) return;
    GR.built = true;
    u64 t0 = SDL_GetTicksNS();
    GR.n = grass_assets_load_dir(GR.a, GRASS_MAX_ASSETS);
    GR.tier = grass_fidelity_tier();
    u32 div = GR.r_grass_tex && GR.r_grass_tex->i ? 1u : 2u;
    int total = 0;
    for (int i = 0; i < GR.n; i++) total += GR.a[i].variants;
    GrassBuild *gb = (GrassBuild *)calloc((size_t)DG_MAX(total, 1), sizeof(GrassBuild));
    u8 *alb[GRASS_MAX_ASSETS] = { 0 };
    u32 aw[GRASS_MAX_ASSETS] = { 0 }, ah[GRASS_MAX_ASSETS] = { 0 };
    int nb = 0;
    for (int i = 0; i < GR.n; i++) {
        const GrassAsset *a = &GR.a[i];
        GR.mat[i] = ~0u;
        u32 nw = 0, nh = 0;
        alb[i] = grass_tex_load(a->atlas, "albedo", div, &aw[i], &ah[i]);
        u8 *nta = grass_tex_load(a->atlas, "nta", div * 2, &nw, &nh);
        if (!alb[i]) { log_warn("grass: %s: atlas %s missing (tools/vfxgen/groundgen.py)", a->name, a->atlas); free(nta); continue; }
        VgMaterial mt;
        grass_material(a, GR.tier, (u32)i, &mt);
        mt.albedo_tex = mt.mask_tex = grass_tex_upload(alb[i], aw[i], ah[i], true);
        if (nta) mt.mr_tex = grass_tex_upload(nta, nw, nh, false);   // the cover nta map rides in the mr slot (no MAT_MR_TEX)
        free(nta);
        GR.mat[i] = vgeo_scene_add_material(&G.scene, &mt);
        float p[12];
        grass_cover_params(a, p);
        render_ground_set_cover(i, p);
        for (int v = 0; v < a->variants; v++, nb++) {
            GrassBuild *q = &gb[nb];
            q->asset = i;
            q->variant = (u32)v;
            grass_gen_mesh(&q->fm, a, (u32)v);
            vgeo_foliage_source(&q->fm, &q->src);
            q->im = (VgImpostorMaterial){ { 1, 1, 1, 1 }, alb[i], aw[i], ah[i], 1.0f, true, a->alpha_cutoff };
            q->io = (VgImpostorOptions){ 512, 8, &q->im, 1 };
            q->impostor = a->tier[GRASS_TIERS - 1].end_m < 80.0f;   // tall crops fade before an impostor would show
        }
    }
    render_ground_set_cover_count(GR.n);
    jobs_parallel_for(nb, 1, grass_job, gb);
    for (int k = 0; k < nb; k++) {
        GrassBuild *q = &gb[k];
        vgeo_foliage_mesh_free(&q->fm);
        if (!q->ok) { log_error("grass: %s variant %u: build failed: %s", GR.a[q->asset].name, q->variant, q->err); continue; }
        if (G.ftype_count < FOLIAGE_TYPES) {
            u32 mesh = vgeo_scene_add_mesh(&G.scene, &q->b);
            fallback_register(mesh, &q->b, false);
            vgeo_scene_set_mesh_materials(&G.scene, mesh, GR.mat[q->asset]);
            if (q->impostor) vgeo_foliage_impostor_upload(&G.scene, &G.tex, mesh, &q->b);
            struct FoliageType *ft = &G.ftype[G.ftype_count++];
            snprintf(ft->name, sizeof(ft->name), "%s_v%u", GR.a[q->asset].name, q->variant);
            ft->mesh = mesh;
            ft->capsule = false;
            ft->role = -1;
            ft->weight = 0;
        }
        vgeo_free(&q->b);
    }
    for (int i = 0; i < GR.n; i++) free(alb[i]);
    free(gb);
    grass_ground_tops();
    log_info("grass: %d assets, %d meshes, level %d, atlases %s in %.0f ms", GR.n, nb, GR.tier, div == 1 ? "full" : "half", (double)(SDL_GetTicksNS() - t0) / 1e6);
}

bool render_grass_fade(int level, float *start, float *end) {
    float fs = 0, fe = 0;
    for (int i = 0; i < GR.n; i++) {
        const GrassAsset *a = &GR.a[i];
        if (GR.mat[i] == ~0u || (a->density[0] <= 0 && a->density[1] <= 0)) continue;   // grass / meadow cover only
        const GrassTier *t = &a->tier[DG_CLAMP(level, 0, GRASS_TIERS - 1)];
        fs = fmaxf(fs, t->full_m + 0.4f * (t->end_m - t->full_m));
        fe = fmaxf(fe, t->end_m);
    }
    if (fe <= 0) return false;
    *start = fs;
    *end = fe;
    return true;
}

static void grass_update_tier(void);
void vkg_frame_update(void) {
    if (G.ok) grass_update_tier();
}

// The fidelity level changed: every grass material's fades and shadow distance (mesh bounds and the
// cells' fade limits follow at the commit).
static void grass_update_tier(void) {
    if (!GR.built || !GR.n) return;
    int t = grass_fidelity_tier();
    if (t == GR.tier) return;
    GR.tier = t;
    for (int i = 0; i < GR.n; i++) {
        if (GR.mat[i] == ~0u || GR.mat[i] >= G.scene.material_count) continue;
        VgMaterial *mt = &G.scene.materials[GR.mat[i]], nm;
        grass_material(&GR.a[i], t, (u32)i, &nm);
        mt->fade_start = nm.fade_start;
        mt->fade_end = nm.fade_end;
        mt->shadow_max_dist = nm.shadow_max_dist;
    }
    G.need_commit = true;
    foliage_commit();
    log_info("grass: fidelity level %d (fades / shadows)", t);
}

// Imported trees (vgeo_tree.h, docs/VGEO.md "Tree models"): the species of the roles in role_mask not
// loaded yet, from the build's assets/vgeo/trees (else built into assets/cache) on the job system;
// textures shared by file, masks = the albedo alpha. Returns the species registered.
typedef struct TreeBuild { const VgTreeSpecies *sp; VgTree t; bool ok; char err[256]; const char *dir, *built, *cache; } TreeBuild;

static void tree_job(void *ctx, int begin, int end) {
    TreeBuild *tb = (TreeBuild *)ctx;
    for (int i = begin; i < end; i++) tb[i].ok = vgeo_tree_load_or_build(tb[i].sp, tb[i].dir, tb[i].built, tb[i].cache, &tb[i].t, tb[i].err, sizeof(tb[i].err));
}

// A tree texture of material k (map: 0 albedo, 1 bark normal, 2 bark ORM), shared by file name.
static u32 tree_texture(const VgTree *t, u32 k, int map) {
    static const char *const suffix[3] = { "", "#n", "#orm" };
    char name[64];
    snprintf(name, sizeof(name), "%s%s", t->tex[k], suffix[map]);
    for (u32 i = 0; i < G.tree_tex_count; i++)
        if (!strcmp(G.tree_tex[i].name, name)) return G.tree_tex[i].tex;
    const VgTreeTexture *x = map == 0 ? &t->texture[k] : map == 1 ? &t->normal[k] : &t->orm[k];
    if (!x->rgba || G.tree_tex_count >= sizeof(G.tree_tex) / sizeof(G.tree_tex[0])) return 0;
    u32 levels;
    u8 *mips = (u8 *)malloc(vg_tex_mip_bytes(x->w, x->h, &levels));
    vg_tex_build_mips(x->rgba, x->w, x->h, mips);
    if (map == 0 && t->masked[k]) vgeo_tree_mip_coverage(mips, x->w, x->h, levels, 0.5f);
    u32 id = vg_tex_create(&G.tex, x->w, x->h, levels, mips, map == 0, true);   // the maps are linear
    free(mips);
    dg_strcpy(G.tree_tex[G.tree_tex_count].name, sizeof(G.tree_tex[0].name), name);
    G.tree_tex[G.tree_tex_count].tex = id;
    G.tree_tex_count++;
    G.tree_tex_bytes += vg_tex_mip_bytes(x->w, x->h, &levels);
    return id;
}

static int foliage_build_trees(u32 role_mask) {
    role_mask &= ~G.tree_roles;
    if (!role_mask) return 0;
    char dir[512], built[512], cache[512];
    platform_data_path("assets/treefbx/", dir, sizeof(dir));
    platform_data_path("assets/vgeo/trees/", built, sizeof(built));
    platform_data_path("assets/cache/", cache, sizeof(cache));
    u32 ns = vgeo_tree_species_count();
    TreeBuild *tb = (TreeBuild *)calloc(ns, sizeof(TreeBuild));
    int n = 0;
    for (u32 i = 0; i < ns; i++) {
        const VgTreeSpecies *sp = vgeo_tree_species(i);
        if (!(role_mask & (1u << sp->role))) continue;
        tb[n].sp = sp;
        tb[n].dir = dir, tb[n].built = built, tb[n].cache = cache;
        n++;
    }
    u64 t0 = SDL_GetTicksNS();
    jobs_parallel_for(n, 1, tree_job, tb);
    int reg = 0, cached = 0;
    for (int i = 0; i < n; i++) {
        TreeBuild *q = &tb[i];
        if (!q->ok) { log_error("vgeo: tree %s: %s", q->sp->name, q->err); continue; }
        if (G.ftype_count >= FOLIAGE_TYPES) { vgeo_tree_free(&q->t); continue; }
        VgMaterial mats[VG_TREE_MAX_MATERIALS];
        vgeo_tree_materials(&q->t, mats);
        u32 mesh = vgeo_scene_add_mesh(&G.scene, &q->t.b);
        fallback_register(mesh, &q->t.b, false);
        u32 base = 0;
        for (u32 k = 0; k < q->t.material_count; k++) {
            mats[k].albedo_tex = mats[k].mask_tex = tree_texture(&q->t, k, 0);
            if (q->t.normal[k].rgba) {   // bark maps (vgeo_tree_materials set the flags)
                mats[k].normal_tex = tree_texture(&q->t, k, 1);
                mats[k].mr_tex = mats[k].occlusion_tex = tree_texture(&q->t, k, 2);
            }
            u32 idx = vgeo_scene_add_material(&G.scene, &mats[k]);
            if (!k) base = idx;
        }
        vgeo_scene_set_mesh_materials(&G.scene, mesh, base);
        bool imp = vgeo_impostor(&q->t.b, NULL, NULL) != NULL;
        if (imp && vgeo_foliage_impostor_upload(&G.scene, &G.tex, mesh, &q->t.b)) vgeo_impostor_scale(&G.scene, mesh, q->sp->impostor_scale);
        struct FoliageType *ft = &G.ftype[G.ftype_count++];
        dg_strcpy(ft->name, sizeof(ft->name), q->sp->name);
        ft->mesh = mesh;
        ft->capsule = q->sp->capsule;
        ft->cap_r = q->t.cap_r;
        ft->cap_h = q->t.cap_h;
        ft->role = q->sp->role;
        ft->weight = q->sp->weight;
        G.tree_roles |= 1u << q->sp->role;
        cached += q->t.from_cache;
        reg++;
        log_info("vgeo: tree %s: %u tris, %u clusters, %u levels, %u materials, capsule r %.2f m, impostor %s%s", ft->name, q->t.b.stats.input_tris, q->t.b.hdr.cluster_count,
                 q->t.b.hdr.level_count, q->t.material_count, (double)ft->cap_r, imp ? "8x8 frames" : "none", q->t.from_cache ? "" : " (built)");
        vgeo_tree_free(&q->t);
    }
    free(tb);
    u32 own = 0;
    for (u32 i = 0; i < VG_TEX_OWN; i++) own += G.tex.own[i].used;
    log_info("vgeo: %d tree species (%d cached) in %.0f ms, %u tree textures (%.1f MB, albedo + bark maps), %u / %u module textures", reg, cached,
             (double)(SDL_GetTicksNS() - t0) / 1e6, G.tree_tex_count, (double)G.tree_tex_bytes / (1024.0 * 1024.0), own, VG_TEX_OWN);
    return reg;
}

int render_foliage_map_types(void) {
    if (!G.ok) return 0;
    if (G.map_types) return (int)G.ftype_count;
    render_foliage_test_types();   // grass tufts, bushes and flowers are part of the maps' cover
    u64 t0 = SDL_GetTicksNS();
    foliage_build_trees((1u << VG_TREE_CONIFER) | (1u << VG_TREE_BROADLEAF) | (1u << VG_TREE_DEAD));
    foliage_build_kinds(VG_FOLIAGE_CROP, VG_FOLIAGE_CROP + 1, 512);
    foliage_build_grass();
    G.map_types = true;
    foliage_region();
    G.need_commit = true;
    foliage_commit();
    log_info("vgeo: foliage map types ready in %.0f ms", (double)(SDL_GetTicksNS() - t0) / 1e6);
    return (int)G.ftype_count;
}

int render_foliage_role_types(int role, char (*names)[48], float *weights, int max) {
    int n = 0;
    for (u32 i = 0; i < G.ftype_count && n < max; i++)
        if (G.ftype[i].role == role) {
            dg_strcpy(names[n], 48, G.ftype[i].name);
            if (weights) weights[n] = G.ftype[i].weight;
            n++;
        }
    return n;
}

int render_foliage_test_types(void) {
    if (!G.ok) return 0;
    if (G.test_types) return (int)G.ftype_count;
    u64 t0 = SDL_GetTicksNS();
    foliage_build_kinds(VG_FOLIAGE_TUFT, VG_FOLIAGE_TUFT + 1, 1024);   // the procedural bush is GPU test content only
    foliage_build_trees((1u << VG_TREE_BUSH) | (1u << VG_TREE_FLOWER));   // the imported bushes and flowers
    G.test_types = true;
    foliage_region();
    G.need_commit = true;
    foliage_commit();
    log_info("vgeo: foliage test types ready in %.0f ms", (double)(SDL_GetTicksNS() - t0) / 1e6);
    return (int)G.ftype_count;
}

void *render_foliage_test_chunk(int cx, int cz, float density, float x0, float z0, float x1, float z1, float (*height)(void *user, float x, float z), void *user,
                                size_t *size) {
    // tufts, the imported bushes and daisies (dropped at load when not registered)
    static const char names[5][48] = { "test_tuft", "bush_juniper", "bush_bamboo", "bush_acacia", "flower_daisy" };
    static const float weight[5] = { 9.0f, 0.45f, 0.15f, 0.1f, 0.3f }, scale[5][2] = { { 0.8f, 1.3f }, { 0.6f, 1.1f }, { 0.6f, 1.0f }, { 0.6f, 1.0f }, { 0.8f, 1.3f } };
    VgScatter sc;
    memset(&sc, 0, sizeof(sc));
    sc.x0 = x0, sc.z0 = z0, sc.x1 = x1, sc.z1 = z1;
    sc.density = density;
    sc.kinds = 5;
    for (int k = 0; k < 5; k++) {
        sc.weight[k] = weight[k];
        sc.scale[k][0] = scale[k][0], sc.scale[k][1] = scale[k][1];
    }
    sc.height = height;
    sc.user = user;
    sc.seed = 1234;
    u32 cap = (u32)(density * VGP_CHUNK_SIZE * VGP_CHUNK_SIZE * 1.3f) + 64;
    VgInstance *tmp = (VgInstance *)malloc(sizeof(VgInstance) * cap);
    u32 n = vgeo_foliage_scatter_chunk(&sc, cx, cz, tmp, cap);
    u8 *blob = n ? vgp_write(cx, cz, names, 5, tmp, n, size) : NULL;
    free(tmp);
    return blob;
}

void *render_foliage_make_chunk(int cx, int cz, const char (*names)[48], int name_count, const RenderFoliageInst *inst, int n, size_t *size) {
    *size = 0;
    if (n <= 0) return NULL;
    VgInstance *tmp = (VgInstance *)malloc(sizeof(VgInstance) * (size_t)n);
    float x0 = (float)cx * VGP_CHUNK_SIZE, z0 = (float)cz * VGP_CHUNK_SIZE;
    int m = 0;
    for (int i = 0; i < n; i++) {
        const RenderFoliageInst *s = &inst[i];
        if (s->pos.x < x0 || s->pos.z < z0 || s->pos.x >= x0 + VGP_CHUNK_SIZE || s->pos.z >= z0 + VGP_CHUNK_SIZE || s->kind >= name_count) continue;
        float p[3] = { s->pos.x, s->pos.y, s->pos.z };
        vgeo_foliage_instance(&tmp[m++], s->kind, p, s->yaw, s->scale);
    }
    u8 *blob = m ? vgp_write(cx, cz, names, (u32)name_count, tmp, (u32)m, size) : NULL;
    free(tmp);
    return blob;
}

// A placement chunk's mesh name: a registered foliage type, else a model with "vgeo": true (its
// "foliage" block sets the materials; registered as a type on first use).
static u32 foliage_resolve(const char *name, void *u) {
    (void)u;
    for (u32 i = 0; i < G.ftype_count; i++)
        if (!strcmp(G.ftype[i].name, name)) return G.ftype[i].mesh;
    if (G.ftype_count >= FOLIAGE_TYPES) return ~0u;
    const Model *model = assets_model(assets_model_id(name));
    if (!model || !model->vgeo) return ~0u;
    RenderVgeoPlacement pl;
    memset(&pl, 0, sizeof(pl));
    pl.mesh = name;
    pl.color = v3(1, 1, 1);
    pl.roughness = 0.8f;
    u32 before = G.scene.mesh_count, mesh = level_mesh(&pl);
    if (mesh == ~0u) return ~0u;
    if (G.scene.mesh_count != before) G.need_commit = true;
    struct FoliageType *ft = &G.ftype[G.ftype_count++];
    dg_strcpy(ft->name, sizeof(ft->name), name);
    ft->mesh = mesh;
    ft->capsule = model->foliage.capsule;
    ft->cap_r = model->foliage.capsule_radius;
    ft->cap_h = model->foliage.capsule_height;
    ft->role = -1;
    ft->weight = 0;
    return mesh;
}

int render_foliage_chunk_load(const void *data, size_t size) {
    if (!G.ok || !data) return -1;
    VgpView v;
    if (!vgp_parse((const u8 *)data, size, &v)) { log_warn("vgeo: invalid placement chunk (.vgp)"); return -1; }
    foliage_region();
    u32 h = vgeo_foliage_chunk_load(&G.scene, &v, foliage_resolve, NULL, (u32)(u16)(i16)v.hdr->chunk_x | (u32)(u16)(i16)v.hdr->chunk_z << 16);
    foliage_commit();   // a new mesh / the region: load time
    if (h != ~0u) vkc_chunk_loaded(&G.scene, h);
    return h == ~0u ? -1 : (int)h;
}

void render_foliage_chunk_unload(int handle) {
    if (G.ok && handle >= 0) vgeo_scene_placement_remove(&G.scene, (u32)handle);
}

int render_foliage_chunk_capsules(int handle, RenderCapsule *out, int max) {
    if (!G.ok || handle < 0 || (u32)handle >= G.scene.pl.chunk_count || !G.scene.pl.chunks[handle].used) return 0;
    const VgPlacementChunk *ch = &G.scene.pl.chunks[handle];
    int n = 0;
    for (u32 i = ch->first; i < ch->first + ch->count && n < max; i++) {
        const VgInstance *in = &G.scene.pl.inst[i];
        int t = foliage_type_of_mesh(in->mesh);
        if (t < 0 || !G.ftype[t].capsule) continue;
        float s = vgeo_half_to_float(in->scale_h);
        out[n].a = v3(in->pos[0], in->pos[1], in->pos[2]);
        out[n].b = v3(in->pos[0], in->pos[1] + G.ftype[t].cap_h * s, in->pos[2]);
        out[n].radius = G.ftype[t].cap_r * s;
        n++;
    }
    return n;
}

int render_foliage_query(vec3 pos, float radius, RenderFoliageHit *out, int max) {
    if (!G.ok || max <= 0) return 0;
    VgFoliageHit hits[256];
    float p[3] = { pos.x, pos.y, pos.z };
    u32 n = vgeo_foliage_query(&G.scene, p, radius, hits, (u32)DG_MIN(max, 256));
    for (u32 i = 0; i < n; i++) {
        out[i].pos = v3(hits[i].pos[0], hits[i].pos[1], hits[i].pos[2]);
        out[i].scale = hits[i].scale;
        out[i].dist = hits[i].dist;
        out[i].instance = hits[i].instance;
        int t = foliage_type_of_mesh(hits[i].mesh);
        dg_strcpy(out[i].type, sizeof(out[i].type), t >= 0 ? G.ftype[t].name : "?");
    }
    return (int)n;
}

void render_foliage_push(vec3 pos, float radius, float strength) {
    if (!G.disp_ok) return;
    float p[3] = { pos.x, pos.y, pos.z };
    vg_displace_push(&G.disp, p, radius, strength);
}

void render_foliage_clear(void) {
    if (!G.ok) return;
    for (u32 h = 0; h < G.scene.pl.chunk_count; h++)
        if (G.scene.pl.chunks[h].used) vgeo_scene_placement_remove(&G.scene, h);
}

static void cmd_foliage_push_test(int argc, const char **argv) {
    G.push_test = argc > 1 ? (float)atof(argv[1]) : 10.0f;
    log_info("foliage_push_test: %.0f s of pushes circling the camera", (double)G.push_test);
}

static void cmd_foliage_stats(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    if (!G.ok) return;
    const VgPlacements *p = &G.scene.pl;
    char err[256] = "";
    bool ok = vgeo_scene_placement_audit(&G.scene, err, sizeof(err));
    log_info("foliage: %u types, %u placement chunks, %u instances (region %u instances / %u cells, %u cells free, %u free ranges), allocator audit %s%s",
             G.ftype_count, p->live_chunks, p->live_instances, p->inst_cap, p->cell_cap, p->free_cells_n, p->free_inst_n, ok ? "PASS" : "FAIL: ", ok ? "" : err);
}

static void cmd_foliage_query(int argc, const char **argv) {
    float r = argc > 1 ? (float)atof(argv[1]) : 2.0f;
    RenderFoliageHit h[16];
    int n = render_foliage_query(R->cam.pos, r, h, 16);
    log_info("foliage_query: %d instances within %.1f m of (%.1f, %.1f)", n, (double)r, (double)R->cam.pos.x, (double)R->cam.pos.z);
    for (int i = 0; i < n; i++) log_info("  %s at (%.2f, %.2f, %.2f) scale %.2f, %.2f m", h[i].type, (double)h[i].pos.x, (double)h[i].pos.y, (double)h[i].pos.z, (double)h[i].scale, (double)h[i].dist);
}
