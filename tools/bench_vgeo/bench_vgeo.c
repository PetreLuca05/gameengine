// bench_vgeo: virtualized geometry GPU benchmark on the window-less harness (docs/VGEO.md "Bench").
//
//   bench_vgeo [--instances N] [--meshes M] [--frames F] [--vertex-pulling] [--pool-mb P] [--validation]
//
// Builds M procedural rocks (~79k triangles each), scatters N instances over a field, and times the
// culling passes at 1920x1080 from 4 cameras with GPU timestamps (instances, traversal, clusters,
// pass 2), with occlusion off and with a synthetic Hi-Z (walls over 60 % of the screen, two passes),
// plus the Hi-Z build. Then the visibility buffer clear + rasterisation of the same views (pass 1
// only without occlusion, both passes with it): HW and SW ms per pass (serialised, so the split is
// exact), triangles and the SW share. Prints ms against the plan budget (docs/VGEO_PLAN.md §9) and
// the counters (instances / nodes / clusters / triangles surviving). --vertex-pulling: HW raster
// without the mesh shader path. Then a 600 frame streaming flythrough at eye level with a pool of
// half the streamable pages (or --pool-mb): pages loaded / evicted per frame, the streamer's CPU ms
// and the GPU ms of its copies + page-table scatter.
#include "vgeo_gpu_harness.h"
#include "render/vgeo/vgeo_procgen.h"
#include "render/vgeo/vgeo_raster.h"
#include "render/vgeo/vgeo_stream.h"
#include "core/engine.h"
#include "core/dgmath.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Engine g_engine;

#define W 1920
#define H 1080
#define FOV (70.0f * DG_DEG2RAD)

static int float_cmp(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}
static double median(float *v, u32 n) {
    if (!n) return 0.0;
    qsort(v, n, sizeof(float), float_cmp);
    return v[n / 2];
}

static u32 g_rng = 99;
static float rnd(void) {
    g_rng = g_rng * 1664525u + 1013904223u;
    return (float)(g_rng >> 8) / 16777216.0f;
}

int main(int argc, char **argv) {
    u32 instances = 100000, meshes = 8, frames = 60, pool_mb = 0;
    bool validation = false, vertex_pulling = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--instances") && i + 1 < argc) instances = (u32)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--meshes") && i + 1 < argc) meshes = (u32)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = (u32)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pool-mb") && i + 1 < argc) pool_mb = (u32)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--validation")) validation = true;
        else if (!strcmp(argv[i], "--vertex-pulling")) vertex_pulling = true;
    }
    VgHarness h;
    if (!vgh_init(&h, validation)) return 1;
    VgScene s;
    vgeo_scene_init(&s, &h.ctx);
    double t0 = (double)SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency();
    for (u32 m = 0; m < meshes; m++) {
        VgGenMesh g;
        memset(&g, 0, sizeof(g));
        float c[3] = { 0, 0, 0 };
        vgeo_gen_rock(&g, 100 + m, 63, 1.0f, c, 0.25f + 0.05f * (float)(m % 4));
        VgSource src;
        memset(&src, 0, sizeof(src));
        src.vertex_count = g.vertex_count;
        src.pos = g.pos;
        src.index_count = g.index_count;
        src.indices = g.indices;
        VgBuildOptions opt;
        memset(&opt, 0, sizeof(opt));
        VgBuilt b;
        char err[256];
        if (!vgeo_build(&src, &opt, &b, err, sizeof(err))) { printf("build failed: %s\n", err); return 1; }
        vgeo_scene_add_mesh(&s, &b);
        vgeo_free(&b);
        vgeo_gen_free(&g);
    }
    u32 side = (u32)ceil(sqrt((double)instances));
    float spacing = 6.0f;
    for (u32 i = 0; i < instances; i++) {
        float p[3] = { ((float)(i % side) - (float)side * 0.5f) * spacing + rnd() * 3.0f, 0.0f, ((float)(i / side) - (float)side * 0.5f) * spacing + rnd() * 3.0f };
        quat q = quat_mul(quat_yaw(rnd() * 6.2831853f), quat_axis_angle(v3(1, 0, 0), (rnd() - 0.5f) * 0.6f));
        float r[4] = { q.x, q.y, q.z, q.w };
        vgeo_scene_add_instance(&s, i % meshes, p, r, 0.4f + rnd() * 2.0f);
    }
    vgeo_scene_commit(&s);
    double t1 = (double)SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency();
    printf("scene: %u meshes (%llu clusters), %u instances (%.0f m field), %u cells, %u pages (%.0f MB), BVH depth %u, built in %.1f s\n", s.mesh_count,
           (unsigned long long)s.cluster_count, s.instance_count, side * spacing, s.cell_count, s.page_count,
           (double)s.page_count * VGEO_PAGE_SIZE / (1024.0 * 1024.0), s.max_depth, t1 - t0);
    VgCuller c;
    VgCullCaps caps = { 1u << 20, 1u << 19, 1u << 21, 1u << 20, 4096 };
    if (!vg_cull_init(&c, &h.ctx, &s, &caps)) return 1;
    VgRaster rast;
    if (!vg_raster_init(&rast, &h.ctx, &c)) return 1;
    rast.serialize = true;
    rast.use_mesh = rast.use_mesh && !vertex_pulling;
    vg_raster_resize(&rast, W, H);

    // Synthetic Hi-Z: buildings (walls at 40 m) over the left 60 % of the screen.
    VgHizImage walls;
    vgh_hiz_create(&h, &walls, W, H);
    mat4 proj = mat4_perspective(FOV, (float)W / (float)H, 0.1f, 5000.0f);
    float wz = (proj.m[10] * -40.0f + proj.m[14]) / 40.0f;
    float *depth = (float *)malloc(sizeof(float) * W * H);
    for (u32 y = 0; y < H; y++)
        for (u32 x = 0; x < W; x++) depth[y * W + x] = x < W * 6 / 10 ? wz : 1.0f;
    vgh_hiz_build(&h, &c, &walls, depth);
    free(depth);
    VgHizRef wr = vgh_hiz_ref(&walls);

    // Hi-Z build timing (1080p, full chain).
    VkQueryPool qp;
    VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qi.queryCount = 2;
    vkCreateQueryPool(h.ctx.device, &qi, NULL, &qp);
    double hiz_ms = 0;
    for (u32 f = 0; f < frames; f++) {
        VkCommandPool pool;
        VkCommandBuffer cmd = vgc_begin_once(&h.ctx, &pool);
        vkCmdResetQueryPool(cmd, qp, 0, 2);
        VkImageMemoryBarrier2 b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        b.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = walls.hiz;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.levelCount = walls.mips;
        b.subresourceRange.layerCount = 1;
        VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &dep);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, qp, 0);
        vg_hiz_record(&c, cmd, 0, walls.depth_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, walls.mip_views, walls.mips, W, H);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, qp, 1);
        b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        vkCmdPipelineBarrier2(cmd, &dep);
        vgc_submit_wait(&h.ctx, pool, cmd);
        u64 ts[2];
        vkGetQueryPoolResults(h.ctx.device, qp, 0, 2, sizeof(ts), ts, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        if (f) hiz_ms += (double)(ts[1] - ts[0]) * h.ctx.timestamp_period * 1e-6;
    }
    hiz_ms /= frames > 1 ? frames - 1 : 1;
    vkDestroyQueryPool(h.ctx.device, qp, NULL);

    float half = side * spacing * 0.5f;
    struct { const char *name; vec3 eye, target; } cams[4] = {
        { "eye level, across", { -half * 0.9f, 1.7f, 0 }, { half, 1.0f, 0 } },
        { "eye level, inside", { 0, 1.7f, 0 }, { 30, 1.5f, -40 } },
        { "elevated 60 m", { -half, 60, -half }, { 0, 0, 0 } },
        { "top down 400 m", { 0, 400, 1 }, { 0, 0, 0 } },
    };
    printf("\n%-20s %-9s %8s %8s %8s %8s %8s | %8s %9s %9s %11s %6s\n", "camera", "occlusion", "inst ms", "trav ms", "clus ms", "pass2 ms", "total", "inst", "nodes",
           "clusters", "tris", "SW %");
    double worst = 0, rworst = 0;
    struct { double ms[4]; u32 tris, hw, sw; } rr[8];
    {
        // Warm-up: keep the GPU busy ~0.5 s so it runs at full clocks during the timed runs.
        mat4 view = mat4_look_at(cams[2].eye, cams[2].target, v3(0, 1, 0));
        mat4 vp = mat4_mul(proj, view);
        VgViewGpu v;
        float eye[3] = { cams[2].eye.x, cams[2].eye.y, cams[2].eye.z };
        vgeo_view_setup(&v, vp.m, vp.m, eye, (float)H / (2.0f * tanf(FOV * 0.5f)), 0.1f, W, H, 1.0f, 32.0f, VGV_NO_OCCLUSION);
        double t_end = (double)SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency() + 0.5;
        while ((double)SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency() < t_end) {
            VkCommandPool pool;
            VkCommandBuffer cmd = vgc_begin_once(&h.ctx, &pool);
            for (int k = 0; k < 20; k++) vg_cull_record(&c, cmd, 0, 0, &v, NULL, NULL);
            vgc_submit_wait(&h.ctx, pool, cmd);
        }
    }
    for (int ci = 0; ci < 4; ci++)
        for (int occ = 0; occ < 2; occ++) {
            mat4 view = mat4_look_at(cams[ci].eye, cams[ci].target, v3(0, 1, 0));
            mat4 vp = mat4_mul(proj, view);
            VgViewGpu v;
            float eye[3] = { cams[ci].eye.x, cams[ci].eye.y, cams[ci].eye.z };
            vgeo_view_setup(&v, vp.m, vp.m, eye, (float)H / (2.0f * tanf(FOV * 0.5f)), 0.1f, W, H, 1.0f, 32.0f,
                            occ ? VGV_TWO_PASS : VGV_NO_OCCLUSION);
            double acc[4] = { 0 };
            float *rs = (float *)malloc(sizeof(float) * 4 * frames);   // raster ms per frame (median: robust to a shared GPU)
            u32 n = 0;
            for (u32 f = 0; f < frames; f++) {
                VkCommandPool pool;
                VkCommandBuffer cmd = vgc_begin_once(&h.ctx, &pool);
                vg_cull_record(&c, cmd, 0, 0, &v, occ ? &wr : NULL, occ ? &wr : NULL);
                vg_raster_clear(&rast, cmd);
                vg_raster_record(&rast, cmd, 0, 0, 1);
                if (occ) vg_raster_record(&rast, cmd, 0, 0, 2);
                vgc_submit_wait(&h.ctx, pool, cmd);
                float ms[4], r1[2] = { 0, 0 }, r2[2] = { 0, 0 };
                if (f >= 2 && vg_cull_timings(&c, 0, 0, true, ms) && vg_raster_timings(&rast, 0, 0, 1, true, r1) &&
                    (!occ || vg_raster_timings(&rast, 0, 0, 2, true, r2))) {
                    for (int k = 0; k < 4; k++) acc[k] += ms[k];
                    rs[0 * frames + n] = r1[0];
                    rs[1 * frames + n] = r1[1];
                    rs[2 * frames + n] = r2[0];
                    rs[3 * frames + n] = r2[1];
                    n++;
                }
            }
            vg_cull_collect(&c, 0);
            VgCounters ctr;
            vg_cull_counters(&c, 0, &ctr);
            double racc[4];
            for (int k = 0; k < 4; k++) {
                acc[k] /= n ? n : 1;
                racc[k] = median(rs + k * frames, n);
                rr[ci * 2 + occ].ms[k] = racc[k];
            }
            free(rs);
            rr[ci * 2 + occ].tris = ctr.n[VGN_TRIS];
            rr[ci * 2 + occ].hw = ctr.n[VGN_HW];
            rr[ci * 2 + occ].sw = ctr.n[VGN_SW];
            double rtotal = racc[0] + racc[1] + racc[2] + racc[3];
            if (rtotal > rworst) rworst = rtotal;
            double total = acc[0] + acc[1] + acc[2] + acc[3];
            if (total > worst) worst = total;
            u32 vis = ctr.n[VGN_HW] + ctr.n[VGN_SW];
            printf("%-20s %-9s %8.3f %8.3f %8.3f %8.3f %8.3f | %8u %9u %9u %11u %5.1f%s\n", cams[ci].name, occ ? "2-pass" : "off", acc[0], acc[1], acc[2], acc[3], total,
                   ctr.n[VGN_INST_VIS] + ctr.n[VGN_INST_P2], ctr.n[VGN_NODES_VISITED], vis, ctr.n[VGN_TRIS], vis ? 100.0 * ctr.n[VGN_SW] / vis : 0.0,
                   ctr.n[VGN_OVERFLOW] ? " OVERFLOW" : "");
        }
    printf("\nraster (%s, HW / SW serialised, median ms)\n%-20s %-9s %8s %8s %8s %8s %8s | %11s %9s %9s %6s\n", rast.use_mesh ? "mesh shader" : "vertex pulling", "camera",
           "occlusion", "HW p1", "SW p1", "HW p2", "SW p2", "total", "tris", "HW cl", "SW cl", "SW %");
    for (int ci = 0; ci < 4; ci++)
        for (int occ = 0; occ < 2; occ++) {
            const double *m = rr[ci * 2 + occ].ms;
            u32 hw = rr[ci * 2 + occ].hw, sw = rr[ci * 2 + occ].sw;
            printf("%-20s %-9s %8.3f %8.3f %8.3f %8.3f %8.3f | %11u %9u %9u %5.1f\n", cams[ci].name, occ ? "2-pass" : "off", m[0], m[1], m[2], m[3], m[0] + m[1] + m[2] + m[3],
                   rr[ci * 2 + occ].tris, hw, sw, hw + sw ? 100.0 * sw / (hw + sw) : 0.0);
        }
    printf("\nhi-z build 1920x1080 (%u mips): %.3f ms   (budget: 0.1 ms for two builds, Phase 3)\n", walls.mips, hiz_ms);
    printf("culling, worst camera: %.3f ms   (budget: 0.5 ms for cells + instances + traversal + clusters, both passes)\n", worst);
    printf("raster, worst camera: %.3f ms   (budget: 1.2 ms for HW + SW raster, both passes)\n", rworst);
    printf("cpu: %u dispatches per view\n", c.dispatches);

    // Streaming flythrough: eye level across the field, heading sweeping +-60 degrees.
    {
        VgStream st;
        VgStreamConfig cfg = { pool_mb ? pool_mb : 1, 64 };
        vg_stream_init(&st, &s, &c, &cfg);
        if (!pool_mb) {
            vgeo_scene_set_pool(&s, s.root_count + (s.page_count - s.root_count) / 2);
            vgeo_scene_commit(&s);
        }
        const u32 N = 600;
        u32 max_load = 0, max_evict = 0, gpu_n = 0, quick = 0;
        u32 *was = (u32 *)malloc(sizeof(u32) * s.page_count), *evicted_at = (u32 *)calloc(s.page_count, sizeof(u32));
        memcpy(was, s.page_table, sizeof(u32) * s.page_count);
        double cpu_sum = 0, cpu_max = 0, gpu_sum = 0, gpu_max = 0, cull_sum = 0;
        for (u32 f = 0; f < N; f++) {
            float t = (float)f / (float)(N - 1);
            vec3 eye = v3(-half * 0.9f + 1.8f * half * t, 1.7f, -half * 0.3f + 0.6f * half * t);
            float yaw = sinf(t * 12.0f) * 1.05f;
            vec3 target = v3(eye.x + 40.0f * cosf(yaw), 1.5f, eye.z + 40.0f * sinf(yaw));
            mat4 vp = mat4_mul(proj, mat4_look_at(eye, target, v3(0, 1, 0)));
            VgViewGpu v;
            float e3[3] = { eye.x, eye.y, eye.z };
            vgeo_view_setup(&v, vp.m, vp.m, e3, (float)H / (2.0f * tanf(FOV * 0.5f)), 0.1f, W, H, 1.0f, 32.0f, VGV_NO_OCCLUSION);
            u32 slot = f % h.ctx.frames;
            VkCommandPool pool;
            VkCommandBuffer cmd = vgc_begin_once(&h.ctx, &pool);
            vg_stream_update(&st, cmd, slot);
            vg_cull_record(&c, cmd, slot, 0, &v, NULL, NULL);
            vgc_submit_wait(&h.ctx, pool, cmd);
            const VgStreamStats *ss = vg_stream_stats(&st);
            for (u32 p = 0; p < s.page_count; p++) {   // reloads soon after an eviction (thrash)
                if (was[p] != ~0u && s.page_table[p] == ~0u) evicted_at[p] = f + 1;
                if (was[p] == ~0u && s.page_table[p] != ~0u && evicted_at[p] && f + 1 - evicted_at[p] < 30) quick++;
                was[p] = s.page_table[p];
            }
            max_load = DG_MAX(max_load, ss->loaded);
            max_evict = DG_MAX(max_evict, ss->evicted);
            cpu_sum += ss->cpu_ms;
            cpu_max = ss->cpu_ms > cpu_max ? ss->cpu_ms : cpu_max;
            if (f >= h.ctx.frames + 2) {
                gpu_sum += ss->gpu_ms;
                gpu_max = ss->gpu_ms > gpu_max ? ss->gpu_ms : gpu_max;
                gpu_n++;
            }
            float ms[4];
            if (vg_cull_timings(&c, slot, 0, true, ms)) cull_sum += ms[0] + ms[1] + ms[2] + ms[3];
        }
        const VgStreamStats *ss = vg_stream_stats(&st);
        printf("\nstreaming flythrough (%u frames, pool %u slots / %.0f MB of %u pages, %u root, %u per frame):\n", N, s.pool_slots,
               (double)s.pool_slots * VGEO_PAGE_SIZE / (1024.0 * 1024.0), s.page_count, s.root_count, cfg.pages_per_frame);
        printf("  loaded %llu (max %u / frame), evicted %llu (max %u / frame), %u reloaded within 30 frames of their eviction, resident at the end %u\n",
               (unsigned long long)ss->loaded_total, max_load, (unsigned long long)ss->evicted_total, max_evict, quick, ss->resident);
        free(was);
        free(evicted_at);
        printf("  stream cpu %.3f ms mean / %.3f max, gpu %.3f ms mean / %.3f max (budget 0.1 ms), cull %.3f ms mean\n", cpu_sum / N, cpu_max,
               gpu_n ? gpu_sum / gpu_n : 0.0, gpu_max, cull_sum / N);
        vg_stream_shutdown(&st);
    }
    vg_raster_shutdown(&rast);
    vgh_hiz_destroy(&h, &walls);
    vg_cull_shutdown(&c);
    vgeo_scene_free(&s);
    vgh_shutdown(&h);
    if (validation) printf("validation messages: %d\n", g_vgh_vk_messages);
    return validation && g_vgh_vk_messages ? 1 : 0;
}
