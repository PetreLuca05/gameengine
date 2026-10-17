// Ground colour tables for the lit shaders (docs/FOLIAGE_GRASS.md, docs/TERRAIN_SHADING.md): the ground
// cover table (one entry per grass asset, MAT_COVER_ID), the mean albedo and the grass tops' colour /
// coverage per terrain material (raster id), the shared macro variation. Filled by vk_vgeo.c (grass
// assets) and the terrain (its layers' means, terrain.json "macro"); copied into FrameData every frame.
#include "vk_internal.h"
#include "core/cvar.h"
#include <string.h>

static struct {
    vec4 cover[24];
    int cover_count;
    vec4 color[16], top[16];
    float macro[6];
    Cvar *r_grass_sss, *r_grass_debug, *r_ground_macro;
} GD;

void vkgr_init(void) {
    memset(&GD, 0, sizeof(GD));
    // mapgen_material_color's palette (surfaces.glsl SURF_BASE) until the terrain sets its layers' means
    static const float base[14][3] = { { 0.17f, 0.25f, 0.09f }, { 0.33f, 0.31f, 0.16f }, { 0.13f, 0.12f, 0.07f }, { 0.29f, 0.22f, 0.14f }, { 0.25f, 0.24f, 0.22f },
                                       { 0.58f, 0.52f, 0.38f }, { 0.38f, 0.36f, 0.33f }, { 0.10f, 0.10f, 0.11f }, { 0.34f, 0.27f, 0.18f }, { 0.40f, 0.39f, 0.37f },
                                       { 0.50f, 0.41f, 0.20f }, { 0.20f, 0.29f, 0.09f }, { 0.22f, 0.16f, 0.10f }, { 0.18f, 0.16f, 0.12f } };
    for (int i = 0; i < 16; i++) {
        const float *c = base[i < 14 ? i : 3];
        GD.color[i] = v4(c[0], c[1], c[2], 0.0f);
    }
    const float macro[6] = { 1.0f / 220.0f, 1.0f / 65.0f, 0.12f, 0.08f, 0.1f, 1.0f };
    memcpy(GD.macro, macro, sizeof(macro));
    GD.r_grass_sss = cvar_reg_int("r_grass_sss", 1, 0, 1, CVAR_ARCHIVE, "ground cover translucency (light through backlit blades)");
    GD.r_grass_debug = cvar_reg_int("r_grass_debug", 0, 0, 5, 0, "ground cover debug view: 1 root -> tip, 2 matched ground colour, 3 clump / card variation, 4 thickness, 5 macro field");
    GD.r_ground_macro = cvar_reg_int("r_ground_macro", 1, 0, 1, 0, "shared ground macro variation (grass + terrain hue / saturation / value fields)");
}

void render_ground_set_cover(int id, const float p[12]) {
    if (id < 0 || id >= 8) return;
    for (int k = 0; k < 3; k++) GD.cover[id * 3 + k] = v4(p[k * 4], p[k * 4 + 1], p[k * 4 + 2], p[k * 4 + 3]);
}
void render_ground_set_cover_count(int n) { GD.cover_count = DG_CLAMP(n, 0, 8); }

void render_ground_set_top(int mat, const float rgb[3], float coverage) {
    if (mat < 0 || mat >= 16) return;
    GD.top[mat] = rgb ? v4(rgb[0], rgb[1], rgb[2], 0.0f) : v4(0, 0, 0, 0);
    GD.color[mat].w = rgb ? coverage : 0.0f;
}

void render_ground_set_color(int mat, const float rgb[3]) {
    if (mat < 0 || mat >= 16 || !rgb) return;
    GD.color[mat] = v4(rgb[0], rgb[1], rgb[2], GD.color[mat].w);
}

void render_ground_set_macro(const float m[6]) { memcpy(GD.macro, m, sizeof(GD.macro)); }

void vkgr_frame_data(FrameData *fd) {
    memcpy(fd->cover, GD.cover, sizeof(GD.cover));
    fd->cover_info = v4((float)GD.cover_count, 0.0f, GD.r_grass_sss && GD.r_grass_sss->i ? 1.0f : 0.0f, GD.r_grass_debug ? (float)GD.r_grass_debug->i : 0.0f);
    memcpy(fd->ground_color, GD.color, sizeof(GD.color));
    memcpy(fd->ground_top, GD.top, sizeof(GD.top));
    fd->ground_macro = v4(GD.macro[0], GD.macro[1], GD.macro[2], GD.macro[3]);
    fd->ground_macro2 = v4(GD.macro[4], GD.r_ground_macro && GD.r_ground_macro->i ? GD.macro[5] : 0.0f, 0.0f, 0.0f);
}
