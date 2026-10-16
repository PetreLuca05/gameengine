#include "vgeo_gpu_harness.h"
#include "core/log.h"
#include "core/platform.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_vgh_vk_messages;

static VKAPI_ATTR VkBool32 VKAPI_CALL on_message(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type,
                                                 const VkDebugUtilsMessengerCallbackDataEXT *d, void *user) {
    (void)type;
    (void)user;
    if (!(sev & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT))) return VK_FALSE;
    g_vgh_vk_messages++;
    printf("[vk] %.600s\n", d->pMessage);
    return VK_FALSE;
}

static VkShaderModule load_shader(const char *name) {
    extern VgHarness *g_vgh_current;
    char path[512];
    snprintf(path, sizeof(path), "%sshaders/%s.spv", SDL_GetBasePath(), name);
    size_t size = 0;
    char *code = platform_read_file(path, &size);
    if (!code || size % 4) { printf("harness: cannot load %s\n", path); free(code); return VK_NULL_HANDLE; }
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = size;
    ci.pCode = (const u32 *)code;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(g_vgh_current->ctx.device, &ci, NULL, &m);
    free(code);
    return m;
}
VgHarness *g_vgh_current;

static bool has_ext(VkPhysicalDevice gpu, const char *name) {
    u32 n = 0;
    vkEnumerateDeviceExtensionProperties(gpu, NULL, &n, NULL);
    VkExtensionProperties *p = (VkExtensionProperties *)malloc(sizeof(*p) * (n + 1));
    vkEnumerateDeviceExtensionProperties(gpu, NULL, &n, p);
    bool f = false;
    for (u32 i = 0; i < n; i++) f |= !strcmp(p[i].extensionName, name);
    free(p);
    return f;
}

bool vgh_init(VgHarness *h, bool validation) {
    memset(h, 0, sizeof(*h));
    g_vgh_current = h;
    if (volkInitialize() != VK_SUCCESS) { printf("harness: no Vulkan loader\n"); return false; }
    const char *layers[1] = { "VK_LAYER_KHRONOS_validation" };
    const char *exts[1] = { VK_EXT_DEBUG_UTILS_EXTENSION_NAME };
    if (validation) {
        u32 n = 0;
        vkEnumerateInstanceLayerProperties(&n, NULL);
        VkLayerProperties *lp = (VkLayerProperties *)malloc(sizeof(*lp) * (n + 1));
        vkEnumerateInstanceLayerProperties(&n, lp);
        bool found = false;
        for (u32 i = 0; i < n; i++) found |= !strcmp(lp[i].layerName, layers[0]);
        free(lp);
        if (!found) printf("harness: validation layer not found (set VK_ADD_LAYER_PATH), running without\n");
        validation = found;
    }
    h->validation = validation;
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "vgeo harness";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    ici.enabledLayerCount = validation ? 1 : 0;
    ici.ppEnabledLayerNames = layers;
    ici.enabledExtensionCount = validation ? 1 : 0;
    ici.ppEnabledExtensionNames = exts;
    if (vkCreateInstance(&ici, NULL, &h->instance) != VK_SUCCESS) { printf("harness: vkCreateInstance failed\n"); return false; }
    volkLoadInstance(h->instance);
    if (validation) {
        VkDebugUtilsMessengerCreateInfoEXT mi = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
        mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        mi.pfnUserCallback = on_message;
        vkCreateDebugUtilsMessengerEXT(h->instance, &mi, NULL, &h->messenger);
    }
    u32 n = 0;
    vkEnumeratePhysicalDevices(h->instance, &n, NULL);
    VkPhysicalDevice gpus[16];
    n = n > 16 ? 16 : n;
    vkEnumeratePhysicalDevices(h->instance, &n, gpus);
    int best = -1, best_score = -1;
    u32 family = 0;
    for (u32 i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(gpus[i], &p);
        if (p.apiVersion < VK_API_VERSION_1_3) continue;
        u32 qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(gpus[i], &qn, NULL);
        VkQueueFamilyProperties q[16];
        qn = qn > 16 ? 16 : qn;
        vkGetPhysicalDeviceQueueFamilyProperties(gpus[i], &qn, q);
        for (u32 k = 0; k < qn; k++)
            if ((q[k].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (q[k].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
                int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 100 : 10;
                if (score > best_score) { best_score = score; best = (int)i; family = k; }
                break;
            }
    }
    if (best < 0) { printf("harness: no Vulkan 1.3 GPU\n"); return false; }
    VgContext *c = &h->ctx;
    c->gpu = gpus[best];
    c->queue_family = family;
    vkGetPhysicalDeviceProperties(c->gpu, &h->props);
    c->timestamp_period = h->props.limits.timestampPeriod;
    // Same vgeo feature set as vk_device.c.
    VkPhysicalDeviceVulkan12Features s12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceFeatures2 s2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    s2.pNext = &s12;
    vkGetPhysicalDeviceFeatures2(c->gpu, &s2);
    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    f13.synchronization2 = VK_TRUE;
    f13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    f12.pNext = &f13;
    f12.scalarBlockLayout = s12.scalarBlockLayout;
    f12.shaderBufferInt64Atomics = s12.shaderBufferInt64Atomics;
    f12.drawIndirectCount = s12.drawIndirectCount;
    f12.runtimeDescriptorArray = s12.runtimeDescriptorArray;
    f12.descriptorBindingPartiallyBound = s12.descriptorBindingPartiallyBound;
    f12.shaderSampledImageArrayNonUniformIndexing = s12.shaderSampledImageArrayNonUniformIndexing;
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    f2.pNext = &f12;
    f2.features.shaderInt64 = s2.features.shaderInt64;
    f2.features.multiDrawIndirect = s2.features.multiDrawIndirect;
    f2.features.shaderStorageImageExtendedFormats = s2.features.shaderStorageImageExtendedFormats;
    if (!s12.scalarBlockLayout) { printf("harness: scalarBlockLayout missing\n"); return false; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    if (vkCreateDevice(c->gpu, &dci, NULL, &c->device) != VK_SUCCESS) { printf("harness: vkCreateDevice failed\n"); return false; }
    volkLoadDevice(c->device);
    vkGetDeviceQueue(c->device, family, 0, &c->queue);
    VmaVulkanFunctions vf = { 0 };
    vf.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    vf.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo ai = { 0 };
    ai.vulkanApiVersion = VK_API_VERSION_1_3;
    ai.physicalDevice = c->gpu;
    ai.device = c->device;
    ai.instance = h->instance;
    ai.pVulkanFunctions = &vf;
    if (vmaCreateAllocator(&ai, &c->vma) != VK_SUCCESS) return false;
    c->frames = 2;
    c->load_shader = load_shader;
    c->int64_atomics = s12.shaderBufferInt64Atomics && s2.features.shaderInt64;
    c->draw_indirect_count = s12.drawIndirectCount;
    c->descriptor_indexing = s12.runtimeDescriptorArray && s12.descriptorBindingPartiallyBound;
    c->mesh_shader = has_ext(c->gpu, "VK_EXT_mesh_shader");
    printf("harness: %s (api %u.%u), int64 atomics %d, indirect count %d, mesh shader %d, validation %d\n", h->props.deviceName,
           VK_API_VERSION_MAJOR(h->props.apiVersion), VK_API_VERSION_MINOR(h->props.apiVersion), c->int64_atomics, c->draw_indirect_count, c->mesh_shader,
           validation);
    return true;
}

void vgh_shutdown(VgHarness *h) {
    if (h->ctx.device) vkDeviceWaitIdle(h->ctx.device);
    if (h->ctx.vma) vmaDestroyAllocator(h->ctx.vma);
    if (h->ctx.device) vkDestroyDevice(h->ctx.device, NULL);
    if (h->messenger) vkDestroyDebugUtilsMessengerEXT(h->instance, h->messenger, NULL);
    if (h->instance) vkDestroyInstance(h->instance, NULL);
    memset(h, 0, sizeof(*h));
}

void vgh_read(VgHarness *h, const VgBuffer *b, VkDeviceSize offset, VkDeviceSize size, void *dst) {
    if (!size) return;
    VgBuffer rb = vgc_buffer(&h->ctx, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VGM_READBACK);
    VkCommandPool pool;
    VkCommandBuffer cmd = vgc_begin_once(&h->ctx, &pool);
    VkBufferCopy r = { offset, 0, size };
    vkCmdCopyBuffer(cmd, b->buffer, rb.buffer, 1, &r);
    vgc_submit_wait(&h->ctx, pool, cmd);
    vmaInvalidateAllocation(h->ctx.vma, rb.alloc, 0, VK_WHOLE_SIZE);
    memcpy(dst, rb.mapped, (size_t)size);
    vgc_buffer_free(&h->ctx, &rb);
}

// ------------------------------------------------------------------ Hi-Z images

static VkImage make_image(VgHarness *h, u32 w, u32 hh, u32 mips, VkImageUsageFlags usage, VmaAllocation *a) {
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R32_SFLOAT;
    ii.extent.width = w;
    ii.extent.height = hh;
    ii.extent.depth = 1;
    ii.mipLevels = mips;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.usage = usage;
    VmaAllocationCreateInfo ai = { 0 };
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    VkImage img = VK_NULL_HANDLE;
    vmaCreateImage(h->ctx.vma, &ii, &ai, &img, a, NULL);
    return img;
}

static VkImageView make_view(VgHarness *h, VkImage img, u32 base, u32 count) {
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R32_SFLOAT;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.baseMipLevel = base;
    vi.subresourceRange.levelCount = count;
    vi.subresourceRange.layerCount = 1;
    VkImageView v = VK_NULL_HANDLE;
    vkCreateImageView(h->ctx.device, &vi, NULL, &v);
    return v;
}

static void barrier(VkCommandBuffer cmd, VkImage img, u32 mips, VkImageLayout o, VkImageLayout n) {
    VkImageMemoryBarrier2 b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = o;
    b.newLayout = n;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = mips;
    b.subresourceRange.layerCount = 1;
    VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

bool vgh_hiz_create(VgHarness *h, VgHizImage *z, u32 w, u32 hh) {
    memset(z, 0, sizeof(*z));
    z->w = w;
    z->h = hh;
    u32 hw = (w + 1) / 2, hh2 = (hh + 1) / 2;   // half resolution, rounded up (as FG_SIZE_HALF)
    u32 m = 1, s = hw > hh2 ? hw : hh2;
    while ((s >> m) && m < 16) m++;
    z->mips = m;
    z->depth = make_image(h, w, hh, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &z->depth_alloc);
    z->hiz = make_image(h, hw, hh2, m, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &z->hiz_alloc);
    if (!z->depth || !z->hiz) return false;
    z->depth_view = make_view(h, z->depth, 0, 1);
    z->hiz_view = make_view(h, z->hiz, 0, m);
    for (u32 i = 0; i < m; i++) z->mip_views[i] = make_view(h, z->hiz, i, 1);
    VkCommandPool pool;
    VkCommandBuffer cmd = vgc_begin_once(&h->ctx, &pool);
    barrier(cmd, z->hiz, m, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    barrier(cmd, z->depth, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vgc_submit_wait(&h->ctx, pool, cmd);
    return true;
}

void vgh_hiz_destroy(VgHarness *h, VgHizImage *z) {
    VkDevice d = h->ctx.device;
    vkDeviceWaitIdle(d);
    for (u32 i = 0; i < z->mips; i++) vkDestroyImageView(d, z->mip_views[i], NULL);
    if (z->hiz_view) vkDestroyImageView(d, z->hiz_view, NULL);
    if (z->depth_view) vkDestroyImageView(d, z->depth_view, NULL);
    if (z->hiz) vmaDestroyImage(h->ctx.vma, z->hiz, z->hiz_alloc);
    if (z->depth) vmaDestroyImage(h->ctx.vma, z->depth, z->depth_alloc);
    memset(z, 0, sizeof(*z));
}

void vgh_hiz_build(VgHarness *h, VgCuller *c, VgHizImage *z, const float *depth) {
    VkDeviceSize size = (VkDeviceSize)z->w * z->h * 4;
    VgBuffer st = vgc_buffer(&h->ctx, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VGM_UPLOAD);
    memcpy(st.mapped, depth, (size_t)size);
    vmaFlushAllocation(h->ctx.vma, st.alloc, 0, VK_WHOLE_SIZE);
    VkCommandPool pool;
    VkCommandBuffer cmd = vgc_begin_once(&h->ctx, &pool);
    barrier(cmd, z->depth, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy r = { 0 };
    r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    r.imageSubresource.layerCount = 1;
    r.imageExtent.width = z->w;
    r.imageExtent.height = z->h;
    r.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cmd, st.buffer, z->depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    barrier(cmd, z->depth, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    barrier(cmd, z->hiz, z->mips, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    vg_hiz_record(c, cmd, 0, z->depth_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, z->mip_views, z->mips, z->w, z->h);
    barrier(cmd, z->hiz, z->mips, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vgc_submit_wait(&h->ctx, pool, cmd);
    vgc_buffer_free(&h->ctx, &st);
}

void vgh_hiz_read(VgHarness *h, VgHizImage *z, u32 mip, float *out) {
    u32 w = (z->w + 1) / 2, hh = (z->h + 1) / 2;
    for (u32 k = 0; k < mip; k++) { w = w > 1 ? w / 2 : 1; hh = hh > 1 ? hh / 2 : 1; }
    VgBuffer rb = vgc_buffer(&h->ctx, (VkDeviceSize)w * hh * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VGM_READBACK);
    VkCommandPool pool;
    VkCommandBuffer cmd = vgc_begin_once(&h->ctx, &pool);
    barrier(cmd, z->hiz, z->mips, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy r = { 0 };
    r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    r.imageSubresource.mipLevel = mip;
    r.imageSubresource.layerCount = 1;
    r.imageExtent.width = w;
    r.imageExtent.height = hh;
    r.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cmd, z->hiz, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buffer, 1, &r);
    barrier(cmd, z->hiz, z->mips, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vgc_submit_wait(&h->ctx, pool, cmd);
    vmaInvalidateAllocation(h->ctx.vma, rb.alloc, 0, VK_WHOLE_SIZE);
    memcpy(out, rb.mapped, (size_t)w * hh * 4);
    vgc_buffer_free(&h->ctx, &rb);
}

VgHizRef vgh_hiz_ref(const VgHizImage *z) {
    VgHizRef r = { z->hiz_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, z->w, z->h, z->mips };
    return r;
}
