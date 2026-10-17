// Point and spot light shadows (docs/LIGHT_SHADOWS.md, docs/SHADOWS_UNITY_MODEL.md). Every light
// casts shadows unless it opts out with LIGHT_NO_SHADOW (muzzle flashes) or its Shadow Type is None.
// Each frame the lights with the biggest screen size get squares of one D32 atlas at their
// Resolution (URP tiers 256 .. 2048, or the quality's; a light that does not fit is retried one
// size smaller): 6 cube faces per point light (90 degree frusta, slightly widened so the filter
// footprint never leaves the face's square), 1 square per spot light. The tile matrices map
// straight into the atlas (as the sun cascades do): the casters draw with a full-atlas viewport,
// scissored to the tile, culled against the light sphere and the face frustum. The lit shaders pick
// the cube face from the light-to-point direction (common.glsl point_light_shadow).
#include "vk_internal.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define ATLAS_TILES_X 8
#define MAX_SHADOW_POINTS 10   // 60 tiles + 4 spots = the 64-tile atlas
#define SHADOW_NEAR 0.05f
#define SPOT_NEAR 0.6f         // flashlights / headlights: the holder's gun / own bodywork never shadows the beam

// The atlas edge / 8 ("tile" kept for the cvar's meaning): 512 -> a 4096 atlas, 256 -> 2048.
static u32 tile_size_for(int quality) { return quality >= 2 ? 512u : 256u; }

static void write_binding(void) {
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) {
        VkDescriptorSet sets[FRAME_SET_COUNT];   // main, scope PiP and the phone camera views
        frame_sets_of(&R->frames[i], sets);
        for (int k = 0; k < FRAME_SET_COUNT; k++) {
            if (!sets[k]) continue;
            VkDescriptorImageInfo ii = { R->sampler_shadow, R->lshadow.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w.dstSet = sets[k];
            w.dstBinding = 11;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.pImageInfo = &ii;
            vkUpdateDescriptorSets(R->device, 1, &w, 0, NULL);
        }
    }
}

// (Re)creates the atlas for a tile size; the image starts in SHADER_READ_ONLY (cleared to far).
static void create_atlas(u32 tile) {
    if (R->lshadow.image) {
        vkDeviceWaitIdle(R->device);
        vkr_target_destroy(&R->lshadow);
    }
    u32 size = tile * ATLAS_TILES_X;
    R->lshadow = vkr_target_create(size, size, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                   VK_IMAGE_ASPECT_DEPTH_BIT);
    if (!R->lshadow.image || !R->lshadow.view) {
        // Binding 11 is statically used by the lit shaders: keep a 1 x 1 atlas (shadows off).
        log_error("light shadows: cannot create a %ux%u atlas, light shadows off", size, size);
        tile = 1;
        size = ATLAS_TILES_X;
        R->lshadow = vkr_target_create(size, size, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                       VK_IMAGE_ASPECT_DEPTH_BIT);
    }
    R->lshadow.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    R->lshadow_tile = tile;
    write_binding();
    log_info("light shadows: %ux%u atlas (%u px tiles)", size, size, tile);
}

void vkls_init(void) {
    R->r_light_shadows = cvar_reg_bool("r_light_shadows", true, CVAR_ARCHIVE, "point / spot light shadows (every light but muzzle flashes)");
    R->r_light_shadow_count = cvar_reg_int("r_light_shadow_count", MAX_SHADOW_POINTS, 0, MAX_SHADOW_POINTS, CVAR_ARCHIVE,
                                           "shadowed point lights per frame (the most important visible ones)");
    R->r_light_shadow_quality = cvar_reg_int("r_light_shadow_quality", 2, 1, 2, CVAR_ARCHIVE, "light shadow tiles: 1 = 256 px, 2 = 512 px");
    create_atlas(tile_size_for(R->r_light_shadow_quality->i));
}

void vkls_shutdown(void) {
    if (R->lshadow.image) vkr_target_destroy(&R->lshadow);
}

static void planes_from(mat4 m, vec4 out[6]) {
    float row[4][4];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) row[r][c] = m.m[c * 4 + r];
    for (int c = 0; c < 4; c++) {
        (&out[0].x)[c] = row[3][c] + row[0][c];
        (&out[1].x)[c] = row[3][c] - row[0][c];
        (&out[2].x)[c] = row[3][c] + row[1][c];
        (&out[3].x)[c] = row[3][c] - row[1][c];
        (&out[4].x)[c] = row[2][c];
        (&out[5].x)[c] = row[3][c] - row[2][c];
    }
    for (int p = 0; p < 6; p++) {
        float l = sqrtf(out[p].x * out[p].x + out[p].y * out[p].y + out[p].z * out[p].z);
        if (l > 0) { out[p].x /= l; out[p].y /= l; out[p].z /= l; out[p].w /= l; }
    }
}

// NDC -> the atlas rectangle (x, y, size) in texels of an atlas of `atlas` (x right, y down).
static mat4 rect_xform(u32 x, u32 y, u32 size, u32 atlas) {
    float s = (float)size / (float)atlas;
    mat4 m = mat4_identity();
    m.m[0] = s;
    m.m[5] = s;
    m.m[12] = -1.0f + (2.0f * (float)x + (float)size) / (float)atlas;
    m.m[13] = -1.0f + (2.0f * (float)y + (float)size) / (float)atlas;
    return m;
}

static void add_tile(mat4 vp_untiled, vec4 sphere, ShsAtlasRect r, u32 atlas, const LightShadowSettings *ls) {
    u32 t = R->local_tile_count++;
    R->local_vps[t] = mat4_mul(rect_xform(r.x, r.y, r.size, atlas), vp_untiled);
    R->local_rect[t] = v4((float)r.x / (float)atlas, (float)r.y / (float)atlas, (float)r.size / (float)atlas, 0.0f);
    R->local_px[t][0] = r.x;
    R->local_px[t][1] = r.y;
    R->local_px[t][2] = r.size;
    R->local_bias[t] = ls->bias;
    R->local_layers[t] = ls->culling_mask;
    planes_from(vp_untiled, R->local_planes[t]);
    R->local_sphere[t] = sphere;
}

typedef struct Cand { int light; float score; bool spot; } Cand;
static int cand_cmp(const void *a, const void *b) {
    const Cand *x = (const Cand *)a, *y = (const Cand *)b;
    if (x->score != y->score) return x->score > y->score ? -1 : 1;
    if (x->spot != y->spot) return x->spot ? 1 : -1;
    return x->light - y->light;
}

static bool sphere_in_view(vec3 c, float r) {
    for (int p = 0; p < 6; p++)
        if (v3_dot(v3(R->frustum[p].x, R->frustum[p].y, R->frustum[p].z), c) + R->frustum[p].w < -r) return false;
    return true;
}

// Screen-size priority (Unity / URP: the most visible lights get the atlas): the light sphere's
// angular size squared, a bonus for last frame's picks (no flicker between equals).
static float screen_score(vec3 p, float r) {
    float d = v3_dist(p, R->cam.pos);
    float a = r / fmaxf(d, r);
    float s = a * a;
    for (u32 k = 0; k < R->lshadow_prev_count; k++)
        if (v3_len2(v3_sub(R->lshadow_prev[k], p)) < 0.01f) s *= 1.5f;
    return s;
}

// The light's packed filter for the shader: 1 hard, 2 tent (the quality's soft tent), 0 none.
static int light_mode(const LightShadowSettings *s) {
    ShadowType t = shs_effective_type(s->type, R->shq.mode);
    return t == SHADOW_HARD ? 1 : (t == SHADOW_SOFT ? 2 : 0);
}

void vkls_select(FrameData *fd) {
    R->local_tile_count = 0;
    for (u32 i = 0; i < MAX_POINT_LIGHTS; i++) R->light_shadow[i] = v4(-1, 0, 0, 0);
    for (u32 i = 0; i < R->spot_count; i++) R->spots[i].params.y = R->spots[i].params.w = 0.0f;
    u32 want_tile = tile_size_for(R->r_light_shadow_quality->i);
    if (want_tile != R->lshadow_tile) create_atlas(want_tile);
    bool on = R->r_light_shadows->i && R->lshadow.image && R->lshadow_tile > 1 && R->shq.mode != SHADOWS_DISABLE;
    if (!on) R->lshadow_prev_count = 0;
    u32 atlas = R->lshadow.w;
    if (on) {
        // Candidates: visible, shadow-casting (type not None under the quality cap), within their
        // distance override; ranked by screen size. Point lights take 6 faces, spots 1 tile.
        Cand c[MAX_POINT_LIGHTS + MAX_SPOT_LIGHTS];
        u32 n = 0, npoints = 0;
        vec3 cam = R->cam.pos;
        for (u32 i = 0; i < R->light_count; i++) {
            const LightShadowSettings *ls = &R->light_set[i];
            if ((R->light_flags[i] & LIGHT_NO_SHADOW) || !light_mode(ls)) continue;
            vec4 pr = R->lights[i].pos_radius;
            vec3 p = v3(pr.x, pr.y, pr.z);
            if (pr.w < 0.5f || R->lights[i].color_intensity.w <= 0.0f || !sphere_in_view(p, pr.w)) continue;
            if (ls->distance > 0.0f && v3_dist(p, cam) - pr.w > ls->distance) continue;
            c[n++] = (Cand){ (int)i, screen_score(p, pr.w), false };
        }
        for (u32 i = 0; i < R->spot_count; i++) {
            const LightShadowSettings *ls = &R->spot_set[i];
            if ((R->spot_flags[i] & LIGHT_NO_SHADOW) || !light_mode(ls)) continue;
            GpuSpotLight *s = &R->spots[i];
            vec3 p = v3(s->pos_range.x, s->pos_range.y, s->pos_range.z);
            if (acosf(clampf(s->dir_cos.w, -1.0f, 1.0f)) > 1.2f) continue;   // > ~70 degree half angle: too wide for one tile, unshadowed
            if (ls->distance > 0.0f && v3_dist(p, cam) > ls->distance) continue;
            // a spot at the eye (flashlight) fills the screen: always first
            c[n++] = (Cand){ (int)i, screen_score(p, fmaxf(s->pos_range.w * 0.25f, 0.5f)) * 4.0f, true };
        }
        qsort(c, n, sizeof(Cand), cand_cmp);
        // Atlas requests in priority order: the light's resolution (or the quality's), URP tiers.
        ShsAtlasReq req[LOCAL_SHADOW_TILES];
        ShsAtlasRect rect[LOCAL_SHADOW_TILES];
        int nreq = 0, owner[LOCAL_SHADOW_TILES];
        u32 max_points = (u32)DG_CLAMP(R->r_light_shadow_count->i, 0, MAX_SHADOW_POINTS);
        for (u32 k = 0; k < n; k++) {
            bool spot = c[k].spot;
            const LightShadowSettings *ls = spot ? &R->spot_set[c[k].light] : &R->light_set[c[k].light];
            int faces = spot ? 1 : 6;
            if (!spot && npoints >= max_points) continue;
            if (nreq + faces > LOCAL_SHADOW_TILES) continue;
            u32 size = DG_MIN(shs_light_texels(ls, &R->shq, spot ? SHADOW_LIGHT_SPOT : SHADOW_LIGHT_POINT), atlas / 2u);
            for (int f = 0; f < faces; f++) {
                req[nreq] = (ShsAtlasReq){ size, (int)k };
                owner[nreq++] = (int)k;
            }
            if (!spot) npoints++;
        }
        shs_atlas_pack(atlas, 64u, req, nreq, rect);
        static const vec3 dirs[6] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
        static const vec3 ups[6] = { { 0, 1, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { 0, 0, 1 }, { 0, 1, 0 }, { 0, 1, 0 } };
        R->lshadow_prev_count = 0;
        for (int r0 = 0; r0 < nreq;) {
            int k = owner[r0], faces = c[k].spot ? 1 : 6;
            if (!rect[r0].size) { r0 += faces; continue; }   // did not fit even at the smallest size: unshadowed this frame
            bool spot = c[k].spot;
            int li = c[k].light;
            const LightShadowSettings *ls = spot ? &R->spot_set[li] : &R->light_set[li];
            bool given = spot ? R->spot_set_given[li] : R->light_set_given[li];
            u32 px = rect[r0].size;
            float nb = 1.5f * ls->normal_bias;   // texels of normal offset
            if (spot) {
                GpuSpotLight *s = &R->spots[li];
                vec3 p = v3(s->pos_range.x, s->pos_range.y, s->pos_range.z), d = v3(s->dir_cos.x, s->dir_cos.y, s->dir_cos.z);
                float outer = acosf(clampf(s->dir_cos.w, -1.0f, 1.0f));
                float sfov = 2.0f * outer * 1.04f + 0.02f;
                float near = given ? fmaxf(ls->near_plane, 0.01f) : SPOT_NEAR;
                vec3 up = fabsf(d.y) > 0.95f ? v3(1, 0, 0) : v3(0, 1, 0);
                mat4 vp = mat4_mul(mat4_perspective(sfov, 1.0f, near, fmaxf(s->pos_range.w, near + 0.5f)), mat4_look_at(p, v3_add(p, d), up));
                u32 t = R->local_tile_count;
                s->params.y = (float)t + 1.0f;
                s->params.z = nb * 2.0f * tanf(sfov * 0.5f) / (float)px;   // normal offset per metre
                s->params.w = (float)light_mode(ls) * 2.0f + ls->strength;
                add_tile(vp, v4(p.x, p.y, p.z, s->pos_range.w), rect[r0], atlas, ls);
            } else {
                vec4 pr = R->lights[li].pos_radius;
                vec3 p = v3(pr.x, pr.y, pr.z);
                float near = given ? fmaxf(ls->near_plane, 0.01f) : SHADOW_NEAR;
                // A few texels of margin around the 90 degree face: filter taps at a face edge stay in the tile.
                float fov = 2.0f * atanf(1.0f + 8.0f / (float)px);
                mat4 proj = mat4_perspective(fov, 1.0f, near, fmaxf(pr.w, near + 0.5f));
                R->light_shadow[li] = v4((float)R->local_tile_count, nb * 2.0f / (float)px, (float)light_mode(ls), ls->strength);
                for (int f = 0; f < 6; f++) add_tile(mat4_mul(proj, mat4_look_at(p, v3_add(p, dirs[f]), ups[f])), pr, rect[r0 + f], atlas, ls);
                if (R->lshadow_prev_count < 16) R->lshadow_prev[R->lshadow_prev_count++] = p;
            }
            r0 += faces;
        }
    }
    memcpy(fd->local_vps, R->local_vps, sizeof(mat4) * R->local_tile_count);
    memcpy(fd->local_rect, R->local_rect, sizeof(vec4) * R->local_tile_count);
    memcpy(fd->light_shadow, R->light_shadow, sizeof(fd->light_shadow));
    fd->local_shadow_info = v4(1.0f / (float)DG_MAX(atlas, 1u), (float)R->lshadow_prev_count, 0, 0);
    fd->local_cfg = v4(shs_tent_texels(R->shq.soft_quality), R->shq.mode == SHADOWS_HARD_ONLY ? 1.0f : 0.0f, 0, 0);
}

void vkls_record(VkCommandBuffer cmd, FrameResources *f) {
    if (!R->lshadow.image) return;
    // Nothing shadowed and the atlas already readable: skip the clear (unused this frame).
    if (!R->local_tile_count && R->lshadow.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) { R->lshadow_draws = 0; R->lshadow_ms = 0; return; }
    u64 t0 = SDL_GetPerformanceCounter();
    vkr_image_barrier(cmd, R->lshadow.image, VK_IMAGE_ASPECT_DEPTH_BIT, R->lshadow.layout, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, 0,
                      VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, 1);
    VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    da.imageView = R->lshadow.view;
    da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    da.clearValue.depthStencil.depth = 1.0f;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea.extent.width = R->lshadow.w;
    ri.renderArea.extent.height = R->lshadow.h;
    ri.layerCount = 1;
    ri.pDepthAttachment = &da;
    vkCmdBeginRendering(cmd, &ri);
    R->lshadow_draws = 0;
    if (R->local_tile_count) {
        VkViewport vp = { 0.0f, 0.0f, (float)R->lshadow.w, (float)R->lshadow.h, 0.0f, 1.0f };
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[PIPE_SHADOW]);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->layout_lit, 0, 1, &f->frame_set, 0, NULL);
        for (u32 t = 0; t < R->local_tile_count; t++) {
            VkRect2D sc = { { (int)R->local_px[t][0], (int)R->local_px[t][1] }, { R->local_px[t][2], R->local_px[t][2] } };
            vkCmdSetScissor(cmd, 0, 1, &sc);
            vkCmdSetDepthBias(cmd, 1.5f * R->local_bias[t], 0.0f, 2.0f * R->local_bias[t]);   // the light's Bias
            R->lshadow_draws += render_draw_shadow_casters(cmd, f, R->local_sphere[t], R->local_planes[t], 4u + t, R->local_layers[t]);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[PIPE_SHADOW]);
            R->lshadow_draws += vkg_draw_local_shadow(cmd, R->local_sphere[t], R->local_planes[t], 4u + t, R->local_px[t][2], R->local_layers[t]);
        }
    }
    vkCmdEndRendering(cmd);
    vkr_image_barrier(cmd, R->lshadow.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 1);
    R->lshadow.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    R->lshadow_ms = (float)((double)(SDL_GetPerformanceCounter() - t0) / (double)SDL_GetPerformanceFrequency() * 1000.0);
}
