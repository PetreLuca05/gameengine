// Window-less Vulkan device for the vgeo GPU tests and tools/bench_vgeo: no surface, no swapchain,
// the same device features as the game (src/render/vk_device.c) and the vgeo module on a VgContext.
// --validation enables VK_LAYER_KHRONOS_validation; every warning / error is printed as "[vk] ..."
// and counted (tests require zero).
#ifndef DG_VGEO_GPU_HARNESS_H
#define DG_VGEO_GPU_HARNESS_H

#include "render/vgeo/vgeo_cull.h"

typedef struct VgHarness {
    VkInstance instance;
    VkDebugUtilsMessengerEXT messenger;
    VkPhysicalDeviceProperties props;
    VgContext ctx;
    bool validation;
} VgHarness;

extern int g_vgh_vk_messages;

bool vgh_init(VgHarness *h, bool validation);
void vgh_shutdown(VgHarness *h);
void vgh_read(VgHarness *h, const VgBuffer *b, VkDeviceSize offset, VkDeviceSize size, void *dst);

// A synthetic depth image (R32F, sampled) and its MAX Hi-Z pyramid (R32F mip chain, GENERAL).
typedef struct VgHizImage {
    u32 w, h, mips;
    VkImage depth, hiz;
    VmaAllocation depth_alloc, hiz_alloc;
    VkImageView depth_view, hiz_view, mip_views[16];
} VgHizImage;
bool vgh_hiz_create(VgHarness *h, VgHizImage *z, u32 w, u32 h_);
void vgh_hiz_destroy(VgHarness *h, VgHizImage *z);
// Uploads depth (w * h floats, 0 near .. 1 far) and builds the pyramid with the culler's builder.
void vgh_hiz_build(VgHarness *h, VgCuller *c, VgHizImage *z, const float *depth);
// Reads pyramid mip m back (vg_hiz_size >> m).
void vgh_hiz_read(VgHarness *h, VgHizImage *z, u32 mip, float *out);
VgHizRef vgh_hiz_ref(const VgHizImage *z);

#endif
