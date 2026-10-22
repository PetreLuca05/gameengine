// Water surfaces (docs/WATER.md "Rendering"): the level's static water meshes (lakes, rivers, pools,
// flat at their level) and the ocean grid around the camera (displaced by the swell in the vertex
// shader, shaders/water/water_waves.glsl), drawn after the opaque scene is lit and fogged and before
// the scene copy, so particles, TAA and the next frame's SSR treat the water like any opaque
// surface. Passes:
//   water_prep     snapshot of the lit scene and its depth (refraction / SSR source: the surface
//                  pass writes the depth attachment, so it cannot sample it)
//   water_ripple   the interactive ripple sim (2D wave equation, toroidal window, r_water >= 2)
//   water          the surface: hdr + depth + velocity (shaders/water/water.vert, water_surface.glsl)
#include "vk_internal.h"
#include "graph/fg.h"
#include "post/post_internal.h"
#include "core/profiler.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define RIPPLE_TEXELS 256
#define RIPPLE_TEXEL 0.125f          // m: a 32 m window
#define OCEAN_RINGS 7                // clipmap levels (cells 0.5 m .. 32 m: ~8 km across)
#define OCEAN_CELLS 64               // cells per ring side (level 0: 32 m)

// Must match shaders/water/water_common.glsl (std430).
typedef struct GpuWaterData {
    vec4 info, wind, ripple, ripple2, ocean;
    vec4 waves[RENDER_WATER_WAVES * 2];
    RenderWaterBody body[RENDER_WATER_BODIES];
    vec4 rip[RENDER_WATER_RIPPLES * 2];
} GpuWaterData;

typedef struct WaterPush {
    vec4 grid;
    mat4 inv_vp;
    vec4 misc;
    vec4 misc2;
} WaterPush;
_Static_assert(sizeof(WaterPush) <= 128, "WaterPush: the guaranteed push constant budget");

static struct {
    bool ok;
    VkPipelineLayout layout;
    VkPipeline pipe, pipe_vel;
    GpuBuffer data[FRAMES_IN_FLIGHT];
    // static meshes (lakes / rivers / pools)
    GpuBuffer vb, ib;
    u32 icount;
    vec3 bmin, bmax;   // their bounds (flat at the body levels: exact)
    // ocean grid (one ring per clipmap level, drawn with a per-level snapped origin)
    GpuBuffer ovb, oib;
    u32 oicount;
    u32 ofirst[OCEAN_RINGS], ocount[OCEAN_RINGS];
    RenderWater w;
    float prev_wave_time;
    vec2 ripple_origin, ripple_prev;
    bool ripple_valid;
    u32 frame;
    // frame graph
    FgRes sim_prev, sim_next;
    Cvar *r_water, *r_water_debug;
} W;

// Where a water surface pass draws: the main view, or the scope picture-in-picture (the same water
// through the scope camera: its own scene snapshot, frame set and depth reconstruction).
typedef struct WaterTarget {
    FgRes hdr, depth, velocity, snap_color, snap_depth;
    bool pip;
    mat4 inv_vp;   // pip: inverse view-projection of the PiP camera (main: this frame's jittered one)
} WaterTarget;
static WaterTarget g_wt_main, g_wt_pip;

void render_water_mesh(const RenderWaterVertex *v, u32 vcount, const u32 *idx, u32 icount) {
    if (W.vb.buffer) vkr_defer_buffer(&W.vb);
    if (W.ib.buffer) vkr_defer_buffer(&W.ib);
    memset(&W.vb, 0, sizeof(W.vb));
    memset(&W.ib, 0, sizeof(W.ib));
    W.icount = 0;
    if (!v || !vcount || !idx || !icount) return;
    W.vb = vkr_buffer_create(sizeof(RenderWaterVertex) * vcount, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
    W.ib = vkr_buffer_create(sizeof(u32) * icount, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true);
    memcpy(W.vb.mapped, v, sizeof(RenderWaterVertex) * vcount);
    memcpy(W.ib.mapped, idx, sizeof(u32) * icount);
    W.icount = icount;
    W.bmin = W.bmax = v3(v[0].pos[0], v[0].pos[1], v[0].pos[2]);
    for (u32 i = 1; i < vcount; i++) {
        vec3 p = v3(v[i].pos[0], v[i].pos[1], v[i].pos[2]);
        W.bmin = v3(fminf(W.bmin.x, p.x), fminf(W.bmin.y, p.y), fminf(W.bmin.z, p.z));
        W.bmax = v3(fmaxf(W.bmax.x, p.x), fmaxf(W.bmax.y, p.y), fmaxf(W.bmax.z, p.z));
    }
}

// docs/perf/FINDINGS.md PF-0047: the lake / river / pool meshes' bounds (grown by 1 m) against the
// side and near planes of this frame's view-projection: outside one of them, the surface pass would
// draw nothing (the meshes are not displaced; the ocean is always drawn).
static bool meshes_in_view(mat4 vp) {
    const float *a = vp.m;
    vec4 r0 = v4(a[0], a[4], a[8], a[12]), r1 = v4(a[1], a[5], a[9], a[13]), r2 = v4(a[2], a[6], a[10], a[14]), r3 = v4(a[3], a[7], a[11], a[15]);
    vec4 pl[5] = { v4(r3.x + r0.x, r3.y + r0.y, r3.z + r0.z, r3.w + r0.w), v4(r3.x - r0.x, r3.y - r0.y, r3.z - r0.z, r3.w - r0.w),
                   v4(r3.x + r1.x, r3.y + r1.y, r3.z + r1.z, r3.w + r1.w), v4(r3.x - r1.x, r3.y - r1.y, r3.z - r1.z, r3.w - r1.w), r2 };
    vec3 lo = v3_sub(W.bmin, v3(1, 1, 1)), hi = v3_add(W.bmax, v3(1, 1, 1));
    for (int k = 0; k < 5; k++) {
        vec3 q = v3(pl[k].x >= 0 ? hi.x : lo.x, pl[k].y >= 0 ? hi.y : lo.y, pl[k].z >= 0 ? hi.z : lo.z);   // the corner furthest inside
        if (pl[k].x * q.x + pl[k].y * q.y + pl[k].z * q.z + pl[k].w < 0) return false;
    }
    return true;
}

void render_set_water(const RenderWater *w) { W.w = *w; }

// Clipmap rings: level 0 a full OCEAN_CELLS^2 grid of 0.5 m cells, each next level the same count of
// cells twice the size with the inner half cut out. Vertices are relative to the camera's snapped
// cell (the vertex shader adds the snapped origin, so the grid never swims).
static void build_ocean_grid(void) {
    int n = OCEAN_CELLS;
    u32 vmax = (u32)(OCEAN_RINGS * (n + 1) * (n + 1)), imax = (u32)(OCEAN_RINGS * n * n * 6);
    RenderWaterVertex *v = (RenderWaterVertex *)malloc(sizeof(RenderWaterVertex) * vmax);
    u32 *idx = (u32 *)malloc(sizeof(u32) * imax);
    u32 vc = 0, ic = 0;
    for (int lv = 0; lv < OCEAN_RINGS; lv++) {
        float cell = 0.5f * (float)(1 << lv);
        float half = cell * (float)n * 0.5f;
        u32 base = vc;
        for (int z = 0; z <= n; z++)
            for (int x = 0; x <= n; x++) {
                RenderWaterVertex *o = &v[vc++];
                o->pos[0] = -half + (float)x * cell;
                o->pos[1] = 0.0f;
                o->pos[2] = -half + (float)z * cell;
                o->body = 0;
            }
        W.ofirst[lv] = ic;
        for (int z = 0; z < n; z++)
            for (int x = 0; x < n; x++) {
                // The middle (covered by the finer level) is skipped, one cell short on every side:
                // the finer level is snapped to half this level's step, so it can sit one cell off
                // centre; the overlap leaves no gap.
                if (lv > 0 && x >= n / 4 + 1 && x < 3 * n / 4 - 1 && z >= n / 4 + 1 && z < 3 * n / 4 - 1) continue;
                u32 a = base + (u32)(z * (n + 1) + x), b = a + 1, c = a + (u32)(n + 1), d = c + 1;
                idx[ic++] = a; idx[ic++] = c; idx[ic++] = b;
                idx[ic++] = b; idx[ic++] = c; idx[ic++] = d;
            }
        W.ocount[lv] = ic - W.ofirst[lv];
    }
    W.ovb = vkr_buffer_create(sizeof(RenderWaterVertex) * vc, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
    W.oib = vkr_buffer_create(sizeof(u32) * ic, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true);
    memcpy(W.ovb.mapped, v, sizeof(RenderWaterVertex) * vc);
    memcpy(W.oib.mapped, idx, sizeof(u32) * ic);
    W.oicount = ic;
    free(v);
    free(idx);
}

// ------------------------------------------------------------------ pipelines

static VkPipeline surface_pipe(const char *frag, bool velocity) {
    VkShaderModule vs = vkp_load_shader("water/water.vert"), fs = vkp_load_shader(frag);
    if (!vs || !fs) {
        if (vs) vkDestroyShaderModule(R->device, vs, NULL);
        if (fs) vkDestroyShaderModule(R->device, fs, NULL);
        return VK_NULL_HANDLE;
    }
    VkPipelineShaderStageCreateInfo st[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO }, { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = vs;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = fs;
    st[1].pName = "main";
    VkVertexInputBindingDescription bind = { 0, sizeof(RenderWaterVertex), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attrs[2] = {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(RenderWaterVertex, pos) },
        { 1, 0, VK_FORMAT_R32_UINT, offsetof(RenderWaterVertex, body) },
    };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &bind;
    vi.vertexAttributeDescriptionCount = 2;
    vi.pVertexAttributeDescriptions = attrs;
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;          // seen from below too (underwater)
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState cba[2];
    memset(cba, 0, sizeof(cba));
    cba[0].colorWriteMask = 0xF;
    cba[1].colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = velocity ? 2 : 1;
    cb.pAttachments = cba;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkFormat colors[2] = { VK_FORMAT_R16G16B16A16_SFLOAT, GB_VELOCITY_FORMAT };
    VkPipelineRenderingCreateInfo rci = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    rci.colorAttachmentCount = velocity ? 2 : 1;
    rci.pColorAttachmentFormats = colors;
    rci.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
    VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gp.pNext = &rci;
    gp.stageCount = 2;
    gp.pStages = st;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dy;
    gp.layout = W.layout;
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = vkCreateGraphicsPipelines(R->device, R->pipeline_cache, 1, &gp, NULL, &p);
    vkDestroyShaderModule(R->device, vs, NULL);
    vkDestroyShaderModule(R->device, fs, NULL);
    if (r != VK_SUCCESS) { log_error("water: pipeline %s failed (%d)", frag, (int)r); return VK_NULL_HANDLE; }
    return p;
}

bool vkw_create_pipelines(void) {
    if (!W.layout) return true;
    if (W.pipe) vkDestroyPipeline(R->device, W.pipe, NULL);
    if (W.pipe_vel) vkDestroyPipeline(R->device, W.pipe_vel, NULL);
    W.pipe = surface_pipe("water/water.frag", false);
    W.pipe_vel = surface_pipe("water/water_vel.frag", true);
    W.ok = W.pipe && W.pipe_vel;
    if (!W.ok) log_error("water: surface pipelines failed (no water rendering until the shaders are fixed)");
    return true;
}

void vkw_init(void) {
    memset(&W, 0, sizeof(W));
    W.r_water = cvar_reg_int("r_water", 3, 0, 3, CVAR_TIER,
                             "Water quality: 0 flat + sky reflection, 1 + SSR + refraction, 2 + ripple sim + caustics, 3 + every detail (Engine Fidelity bounds it)");
    W.r_water_debug = cvar_reg_int("r_water_debug", 0, 0, 1, CVAR_LOCKED, "Water render debug: 1 ripple sim heights as the surface colour");
    if (!P.ok || !P.layout) { log_warn("water: no post stack (no water rendering)"); return; }
    VkDescriptorSetLayout sets[2] = { R->frame_layout, P.layout };
    VkPushConstantRange pr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(WaterPush) };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 2;
    pli.pSetLayouts = sets;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pr;
    VK_CHECK(vkCreatePipelineLayout(R->device, &pli, NULL, &W.layout));
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) {
        W.data[i] = vkr_buffer_create(sizeof(GpuWaterData), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        memset(W.data[i].mapped, 0, sizeof(GpuWaterData));
    }
    build_ocean_grid();
    vkp_register_shader("water/water.vert");
    vkp_register_shader("water/water.frag");
    vkp_register_shader("water/water_vel.frag");
    vkp_register_shader("water/water_prep.comp");
    vkp_register_shader("water/water_ripple.comp");
    vkp_register_shader("water/water_probe.comp");
    vkw_create_pipelines();
}

void vkw_shutdown(void) {
    if (!R || !R->device) return;
    if (W.pipe) vkDestroyPipeline(R->device, W.pipe, NULL);
    if (W.pipe_vel) vkDestroyPipeline(R->device, W.pipe_vel, NULL);
    if (W.layout) vkDestroyPipelineLayout(R->device, W.layout, NULL);
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) vkr_buffer_destroy_now(&W.data[i]);
    vkr_buffer_destroy_now(&W.vb);
    vkr_buffer_destroy_now(&W.ib);
    vkr_buffer_destroy_now(&W.ovb);
    vkr_buffer_destroy_now(&W.oib);
    memset(&W, 0, sizeof(W));
}

// ------------------------------------------------------------------ frame

static int tier(void) { return DG_CLAMP(DG_MIN(W.r_water->i, W.w.tier >= 0 ? W.w.tier : 3), 0, 3); }

static void upload(void) {
    GpuWaterData *g = (GpuWaterData *)W.data[R->frame_slot].mapped;
    const RenderWater *w = &W.w;
    int nb = DG_CLAMP(w->body_count, 0, RENDER_WATER_BODIES), nw = DG_CLAMP(w->wave_count, 0, RENDER_WATER_WAVES);
    int nr = DG_CLAMP(w->ripple_count, 0, RENDER_WATER_RIPPLES);
    g->info = v4(w->wave_time, (float)nb, (float)nw, w->rain);
    g->wind = v4(w->wind_dir.x, w->wind_dir.y, w->wind, (float)tier());
    g->ripple = v4(W.ripple_origin.x, W.ripple_origin.y, RIPPLE_TEXEL, (float)RIPPLE_TEXELS);
    g->ripple2 = v4(W.ripple_prev.x, W.ripple_prev.y, (float)nr, (float)(W.frame & 0xFFFFFF));
    g->ocean = v4((float)w->ocean_body, w->ocean_level, W.prev_wave_time, 0.0f);
    memcpy(g->waves, w->waves, sizeof(vec4) * 2 * (size_t)nw);
    memcpy(g->body, w->body, sizeof(RenderWaterBody) * (size_t)nb);
    for (int i = 0; i < nr; i++) {
        g->rip[2 * i] = w->ripples[i].pos_radius_strength;
        g->rip[2 * i + 1] = w->ripples[i].vel;
    }
}

static void bind_data(PostBind *b) { pb_buf(b, 13, W.data[R->frame_slot].buffer, sizeof(GpuWaterData), false); }

static void exec_prep(VkCommandBuffer cmd, void *u) {
    const WaterTarget *t = (const WaterTarget *)u;
    PostBind b = { 0 };
    pb_ubo(&b);
    pb_tex_fg(&b, 0, t->hdr, P.point);
    pb_tex_fg(&b, 1, t->depth, P.point);
    pb_img_fg(&b, 8, t->snap_color);
    pb_img_fg(&b, 9, t->snap_depth);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
    u32 w, h;
    fg_extent(t->hdr, &w, &h);
    post_dispatch(cmd, "water/water_prep", w, h, 1, 8);
}

static void exec_ripple(VkCommandBuffer cmd, void *u) {
    (void)u;
    PostBind b = { 0 };
    pb_ubo(&b);
    pb_tex_fg(&b, 0, fg_history_valid(W.sim_prev) ? W.sim_prev : FG_NONE, P.point);
    pb_img_fg(&b, 8, W.sim_next);
    bind_data(&b);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
    struct { vec4 p0, p1, p2, p3; u32 u[16]; } pc;
    memset(&pc, 0, sizeof(pc));
    pc.p0 = v4(0.9f, 1.0f / 60.0f, 0.985f, 0.0f);   // wave speed (m/s), step (s), damping per step
    post_push_constants(cmd, &pc, 128);
    post_dispatch(cmd, "water/water_ripple", RIPPLE_TEXELS, RIPPLE_TEXELS, 1, 8);
}

static void draw_surface(VkCommandBuffer cmd, VkBuffer vb, VkBuffer ib, u32 icount, const WaterPush *pc) {
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &off);
    vkCmdBindIndexBuffer(cmd, ib, 0, VK_INDEX_TYPE_UINT32);
    vkCmdPushConstants(cmd, W.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(*pc), pc);
    vkCmdDrawIndexed(cmd, icount, 1, 0, 0, 0);
    R->stats.draw_calls++;
}

static void exec_surface(VkCommandBuffer cmd, void *u) {
    const WaterTarget *wt = (const WaterTarget *)u;
    u32 w, h;
    fg_extent(wt->hdr, &w, &h);
    bool vel = wt->velocity != FG_NONE;
    VkRenderingAttachmentInfo ca[2] = { { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO }, { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO } };
    ca[0].imageView = fg_view(wt->hdr);
    ca[0].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ca[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    if (vel) {
        ca[1] = ca[0];
        ca[1].imageView = fg_view(wt->velocity);
    }
    VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    da.imageView = fg_view(wt->depth);
    da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea.extent = (VkExtent2D){ w, h };
    ri.layerCount = 1;
    ri.colorAttachmentCount = vel ? 2 : 1;
    ri.pColorAttachments = ca;
    ri.pDepthAttachment = &da;
    vkCmdBeginRendering(cmd, &ri);
    VkViewport vp = { 0, 0, (float)w, (float)h, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { w, h } };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vel ? W.pipe_vel : W.pipe);
    VkDescriptorSet fs = wt->pip ? R->frames[R->frame_slot].frame_set_pip : R->frames[R->frame_slot].frame_set;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, W.layout, 0, 1, &fs, 0, NULL);
    PostBind b = { 0 };
    pb_tex_fg(&b, 0, wt->snap_color, P.linear);
    pb_tex_fg(&b, 1, wt->snap_depth, P.point);
    if (W.sim_next != FG_NONE) pb_tex_fg(&b, 2, W.sim_next, P.linear_repeat);
    else pb_tex(&b, 2, P.black, P.linear_repeat);
    VkDescriptorImageInfo fog;
    post_froxel_image(true, &fog);
    pb_tex(&b, 3, fog.imageView, fog.sampler);
    bind_data(&b);
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, W.layout, 1, (u32)b.n, b.w);

    int t = tier();
    WaterPush pc;
    memset(&pc, 0, sizeof(pc));
    // The depth buffer was rendered with the jittered projection: reconstruct with the same.
    pc.inv_vp = wt->pip ? wt->inv_vp : mat4_inverse(mat4_mul(R->proj_j, R->view));
    pc.misc = v4(t >= 1 ? 1.0f : 0.0f, W.sim_next != FG_NONE ? 1.0f : 0.0f, t >= 2 ? 1.0f : 0.0f, t >= 1 ? 1.0f : 0.0f);
    pc.misc2 = v4(1.0f, 0.06f, 0.0f, 0.0f);
    if (W.icount) draw_surface(cmd, W.vb.buffer, W.ib.buffer, W.icount, &pc);
    if (W.w.ocean_body >= 0 && W.oicount) {
        // One draw per clipmap level, each snapped to twice its own cell (no vertex swimming, the
        // finer level's hole stays aligned).
        vec3 c = R->cam.pos;
        for (int lv = 0; lv < OCEAN_RINGS; lv++) {
            float snap = 0.5f * (float)(1 << lv) * 2.0f;
            pc.grid = v4(floorf(c.x / snap) * snap, floorf(c.z / snap) * snap, 1.0f, 0.0f);
            u32 first = W.ofirst[lv], count = W.ocount[lv];
            VkDeviceSize off = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &W.ovb.buffer, &off);
            vkCmdBindIndexBuffer(cmd, W.oib.buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdPushConstants(cmd, W.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
            vkCmdDrawIndexed(cmd, count, 1, first, 0, 0);
            R->stats.draw_calls++;
        }
    }
    vkCmdEndRendering(cmd);
}

bool render_water_gpu_heights(const float *xzt, int n, float *out) {
    if (!W.ok || n <= 0) return false;
    VkPipeline pipe = post_compute_pipe("water/water_probe");
    if (!pipe) return false;
    GpuBuffer data = vkr_buffer_create(sizeof(GpuWaterData), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    GpuBuffer pts = vkr_buffer_create(sizeof(vec4) * (VkDeviceSize)n, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    // The probe's own copy of the water data (the frame's buffer may be in flight).
    GpuWaterData *g = (GpuWaterData *)data.mapped;
    memset(g, 0, sizeof(*g));
    int nw = DG_CLAMP(W.w.wave_count, 0, RENDER_WATER_WAVES);
    g->info = v4(0, 0, (float)nw, 0);
    memcpy(g->waves, W.w.waves, sizeof(vec4) * 2 * (size_t)nw);
    vec4 *p = (vec4 *)pts.mapped;
    for (int i = 0; i < n; i++) p[i] = v4(xzt[3 * i], xzt[3 * i + 1], xzt[3 * i + 2], 0.0f);
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pci.queueFamilyIndex = R->queue_family;
    VkCommandPool pool;
    VK_CHECK(vkCreateCommandPool(R->device, &pci, NULL, &pool));
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    VK_CHECK(vkAllocateCommandBuffers(R->device, &ai, &cmd));
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
    PostBind b = { 0 };
    pb_ubo(&b);
    pb_buf(&b, 13, data.buffer, sizeof(GpuWaterData), false);
    pb_buf(&b, 14, pts.buffer, sizeof(vec4) * (VkDeviceSize)n, false);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
    struct { vec4 p[4]; u32 u[16]; } pc;
    memset(&pc, 0, sizeof(pc));
    pc.u[0] = (u32)n;
    post_push_constants(cmd, &pc, 128);
    vkCmdDispatch(cmd, (u32)(n + 63) / 64, 1, 1);
    VkMemoryBarrier2 mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo di = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &di);
    VK_CHECK(vkEndCommandBuffer(cmd));
    VkCommandBufferSubmitInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
    cbi.commandBuffer = cmd;
    VkSubmitInfo2 si = { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    VK_CHECK(vkQueueSubmit2(R->queue, 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(R->queue));
    vkDestroyCommandPool(R->device, pool, NULL);
    vmaInvalidateAllocation(R->vma, pts.alloc, 0, VK_WHOLE_SIZE);
    for (int i = 0; i < n; i++) out[i] = p[i].w;
    vkr_buffer_destroy_now(&data);
    vkr_buffer_destroy_now(&pts);
    return true;
}

void vkw_build_graph(int hdr, int depth, int velocity) {
    WaterTarget *wt = &g_wt_main;
    memset(wt, 0, sizeof(*wt));
    wt->hdr = hdr;
    wt->depth = depth;
    wt->velocity = velocity;
    W.sim_next = FG_NONE;
    bool any = W.w.enabled && (W.icount > 0 || W.w.ocean_body >= 0);
    if (!W.ok || !any || hdr < 0 || depth < 0) {
        W.ripple_valid = false;
        return;
    }
    W.frame++;
    int t = tier();
    upload();
    int p;
    bool seen = W.w.ocean_body >= 0 || !prof_fix_enabled(PFX_PASS_SKIPS) || meshes_in_view(R->view_proj);   // PF-0047
    if (seen) {
        FgTexDesc dc = { "water_snap_color", VK_FORMAT_R16G16B16A16_SFLOAT, FG_SIZE_RENDER };
        FgTexDesc dd = { "water_snap_depth", VK_FORMAT_R32_SFLOAT, FG_SIZE_RENDER };
        wt->snap_color = fg_texture(&dc);
        wt->snap_depth = fg_texture(&dd);
        p = fg_pass("water_prep", exec_prep, wt);
        fg_read(p, hdr, FG_SAMPLED);
        fg_read(p, depth, FG_SAMPLED);
        fg_write(p, wt->snap_color, FG_STORAGE);
        fg_write(p, wt->snap_depth, FG_STORAGE);
    }

    // Ripple sim: ping-pong histories, the window following the ripple centre in whole texels.
    if (t >= 2) {
        float span = RIPPLE_TEXEL * (float)RIPPLE_TEXELS;
        vec2 o = v2(floorf((W.w.ripple_center.x - span * 0.5f) / RIPPLE_TEXEL) * RIPPLE_TEXEL,
                    floorf((W.w.ripple_center.z - span * 0.5f) / RIPPLE_TEXEL) * RIPPLE_TEXEL);
        W.ripple_prev = W.ripple_valid ? W.ripple_origin : o;
        W.ripple_origin = o;
        W.ripple_valid = true;
        upload();
        FgTexDesc ra = { (W.frame & 1) ? "water_ripple_a" : "water_ripple_b", VK_FORMAT_R16G16_SFLOAT, FG_SIZE_ABS, RIPPLE_TEXELS, RIPPLE_TEXELS, 1 };
        FgTexDesc rb = { (W.frame & 1) ? "water_ripple_b" : "water_ripple_a", VK_FORMAT_R16G16_SFLOAT, FG_SIZE_ABS, RIPPLE_TEXELS, RIPPLE_TEXELS, 1 };
        W.sim_prev = fg_history(&ra);
        W.sim_next = fg_history(&rb);
        p = fg_pass("water_ripple", exec_ripple, NULL);
        fg_read(p, W.sim_prev, FG_SAMPLED);
        fg_write(p, W.sim_next, FG_STORAGE);
    } else {
        W.ripple_valid = false;
    }

    if (seen) {
        p = fg_pass("water", exec_surface, wt);
        fg_read(p, wt->snap_color, FG_SAMPLED);
        fg_read(p, wt->snap_depth, FG_SAMPLED);
        if (W.sim_next != FG_NONE) fg_read(p, W.sim_next, FG_SAMPLED);
        if (post_froxels() >= 0) fg_read(p, post_froxels(), FG_SAMPLED);
        fg_write(p, hdr, FG_COLOR);
        fg_write(p, depth, FG_DEPTH);
        if (velocity != FG_NONE) fg_write(p, velocity, FG_COLOR);
    }
    W.prev_wave_time = W.w.wave_time;
}

// The scope picture-in-picture (render.c, after its fog): the same water through the scope camera,
// on its own snapshot of the PiP colour / depth (refraction, reflection march); the ripple sim and
// the froxels are the main view's (looked up through frame.ss_info).
void vkw_build_graph_pip(int hdr, int depth, mat4 view_proj) {
    bool any = W.w.enabled && (W.icount > 0 || W.w.ocean_body >= 0);
    if (!W.ok || !any || hdr < 0 || depth < 0) return;
    WaterTarget *wt = &g_wt_pip;
    memset(wt, 0, sizeof(*wt));
    wt->hdr = hdr;
    wt->depth = depth;
    wt->velocity = FG_NONE;
    wt->pip = true;
    wt->inv_vp = mat4_inverse(view_proj);
    u32 w, h;
    fg_extent(hdr, &w, &h);
    FgTexDesc dc = { "water_pip_snap_color", VK_FORMAT_R16G16B16A16_SFLOAT, FG_SIZE_ABS, w, h, 1, 1, 0 };
    FgTexDesc dd = { "water_pip_snap_depth", VK_FORMAT_R32_SFLOAT, FG_SIZE_ABS, w, h, 1, 1, 0 };
    wt->snap_color = fg_texture(&dc);
    wt->snap_depth = fg_texture(&dd);
    int p = fg_pass("scope_pip_water_prep", exec_prep, wt);
    fg_read(p, hdr, FG_SAMPLED);
    fg_read(p, depth, FG_SAMPLED);
    fg_write(p, wt->snap_color, FG_STORAGE);
    fg_write(p, wt->snap_depth, FG_STORAGE);
    p = fg_pass("scope_pip_water", exec_surface, wt);
    fg_read(p, wt->snap_color, FG_SAMPLED);
    fg_read(p, wt->snap_depth, FG_SAMPLED);
    if (W.sim_next != FG_NONE) fg_read(p, W.sim_next, FG_SAMPLED);
    if (post_froxels() >= 0) fg_read(p, post_froxels(), FG_SAMPLED);
    fg_write(p, hdr, FG_COLOR);
    fg_write(p, depth, FG_DEPTH);
}
