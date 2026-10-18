// Virtualized geometry rasterisation (docs/VGEO.md "Rasterisation"): the visible clusters of a view
// are written into a 64-bit visibility buffer (vgeo_gpu.h VG_VIS_*) by two rasterisers that produce
// the same value per pixel:
//   HW  clusters >= sw_px across or near the camera: mesh shader (one workgroup per cluster) when the
//       device has VK_EXT_mesh_shader, else vertex pulling (one draw, 384 vertices per cluster);
//       fragment atomicMax, no attachments, no depth test
//   SW  small clusters: compute, one workgroup per cluster, one thread per triangle, 16.8 fixed point
//       edge functions with the HW fill rule (pixel centres, top-left)
// Per pass (two-pass occlusion): pass 1 draws the list entries [0, *_P1), pass 2 [*_P1, total).
// Without 64-bit buffer atomics (MoltenVK) or with VgContext.vis32: the *_v32 shaders, and each pass
// rasterises twice (depth keys, then payloads) into the same u64 layout (vg_raster.glsl VG_VIS32,
// docs/PLATFORM_MACOS.md "vgeo on Metal"); the HW path is vertex pulling then.
//
// Shares the culler's descriptor sets: call vg_raster_resize (binds the buffer at binding 18) before
// vg_cull_record, and record the raster after it (same frame / slot). The view's viewport must be
// the visibility buffer size.
#ifndef DG_VGEO_RASTER_H
#define DG_VGEO_RASTER_H

#include "vgeo_cull.h"

enum VgRasterStamp { VGR_BEGIN, VGR_HW, VGR_SW, VGR_COUNT };

typedef struct VgRaster {
    const VgContext *ctx;
    VgCuller *cull;
    VkPipelineLayout pl;         // cull set layout + VgPush for every raster stage
    VkShaderStageFlags push_stages;
    VkPipeline p_args, p_sw, p_vert, p_mesh;
    VkPipeline p_vert2, p_mesh2;   // masked variant without back-face culling: two-sided materials (a second draw)
    VkPipeline p_vert3, p_mesh3;   // masked variant with back-face culling: one-sided masked / dithered (a third draw)
    VkPipeline p_sw_masked, p_sw_imp;   // SW variants: masked / dithered clusters, impostor quads (vg_raster_sw.glsl)
    VgBuffer vis;                // u64[w * h]
    u32 w, h;
    bool use_mesh;               // HW path: mesh shader (default when available) or vertex pulling
    bool vis32;                  // two-phase 32-bit visibility writes (no mesh shader pipelines then)
    bool serialize;              // barrier between HW and SW: exact per-rasteriser timings (they overlap otherwise)
    VkQueryPool qp;              // VGR_COUNT timestamps per (frame, view, pass)
    bool qp_written[VG_MAX_FRAMES][VG_MAX_VIEWS][2];
    bool ok;
} VgRaster;

bool vg_raster_init(VgRaster *r, const VgContext *ctx, VgCuller *cull);
void vg_raster_shutdown(VgRaster *r);
// (Re)allocates the visibility buffer (waits for the device when it changes) and binds it to the culler.
void vg_raster_resize(VgRaster *r, u32 w, u32 h);
// Clears the visibility buffer to 0 (empty) + barrier to the rasterisers.
void vg_raster_clear(VgRaster *r, VkCommandBuffer cmd);
// Rasterises pass `pass` (1 or 2) of view `slot`, after vg_cull_record of the same frame / slot. Ends
// with a barrier making the visibility buffer visible to compute / fragment / transfer.
void vg_raster_record(VgRaster *r, VkCommandBuffer cmd, u32 frame, u32 slot, u32 pass);
// GPU ms of HW and SW raster for (frame, slot, pass); false when not available. wait: block (tests).
// vis32: [0] the depth phase (HW + SW), [1] the payload phase.
bool vg_raster_timings(const VgRaster *r, u32 frame, u32 slot, u32 pass, bool wait, float ms[2]);

#endif
