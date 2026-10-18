// Instance, device, swapchain and render target management.
#include "vk_internal.h"
#include "vk_portability.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <stdlib.h>
#include <string.h>

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type, const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    (void)type; (void)user;
    bool err = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0;
    if (!err && !(severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)) return VK_FALSE;
    // Log lines are short (LOG_LINE_MAX): long validation messages continue on "[vk+]" lines.
    const char *m = data->pMessage;
    size_t n = strlen(m);
    for (size_t off = 0; off < n && off < 2000; off += 200) {
        if (err) log_error("%s %.200s", off ? "[vk+]" : "[vk]", m + off);
        else log_warn("%s %.200s", off ? "[vk+]" : "[vk]", m + off);
    }
    return VK_FALSE;
}

static bool has_layer(const char *name) {
    u32 count = 0;
    vkEnumerateInstanceLayerProperties(&count, NULL);
    VkLayerProperties *props = (VkLayerProperties *)malloc(sizeof(VkLayerProperties) * (count + 1));
    vkEnumerateInstanceLayerProperties(&count, props);
    bool found = false;
    for (u32 i = 0; i < count; i++) if (strcmp(props[i].layerName, name) == 0) found = true;
    free(props);
    return found;
}

static bool want_validation(void) {
    for (int i = 1; i < g_engine.argc; i++)
        if (strcmp(g_engine.argv[i], "--validation") == 0 || strcmp(g_engine.argv[i], "-validation") == 0) return true;
    const char *env = SDL_getenv("DG_VALIDATION");
    return env && env[0] == '1';
}

// VkPhysicalDevicePortabilitySubsetFeaturesKHR (vulkan_beta.h, which the headers only expose under
// VK_ENABLE_BETA_EXTENSIONS): logged at device creation, the engine uses none of the restricted features.
typedef struct PortabilitySubsetFeatures {
    VkStructureType sType;
    void *pNext;
    VkBool32 constantAlphaColorBlendFactors, events, imageViewFormatReinterpretation, imageViewFormatSwizzle, imageView2DOn3DImage,
        multisampleArrayImage, mutableComparisonSamplers, pointPolygons, samplerMipLodBias, separateStencilMaskRef,
        shaderSampleRateInterpolationFunctions, tessellationIsolines, tessellationPointMode, triangleFans, vertexAttributeAccessBeyondStride;
} PortabilitySubsetFeatures;
#define STYPE_PORTABILITY_SUBSET_FEATURES ((VkStructureType)1000163000)

static void log_portability_subset(VkPhysicalDevice gpu) {
    PortabilitySubsetFeatures ps = { STYPE_PORTABILITY_SUBSET_FEATURES };
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    f2.pNext = &ps;
    vkGetPhysicalDeviceFeatures2(gpu, &f2);
    log_info("vulkan: portability subset (MoltenVK): triangleFans %d, pointPolygons %d, events %d, formatSwizzle %d, mipLodBias %d, "
             "beyondStride %d, constantAlpha %d (none used; docs/PLATFORM_MACOS.md)",
             ps.triangleFans, ps.pointPolygons, ps.events, ps.imageViewFormatSwizzle, ps.samplerMipLodBias, ps.vertexAttributeAccessBeyondStride,
             ps.constantAlphaColorBlendFactors);
}

bool vkd_create_device(SDL_Window *window) {
    // volk and SDL's surface creation must use one Vulkan library: on macOS SDL prefers the bundled
    // libMoltenVK.dylib, then the loader (SDL_VULKAN_LIBRARY overrides), volk only knows the loader.
    // The window (SDL_WINDOW_VULKAN) has loaded SDL's choice already.
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    if (gipa) volkInitializeCustom(gipa);
    else VK_CHECK(volkInitialize());

    u32 sdl_ext_count = 0;
    const char *const *sdl_exts = SDL_Vulkan_GetInstanceExtensions(&sdl_ext_count);
    const char *exts[16];
    u32 ext_count = 0;
    for (u32 i = 0; i < sdl_ext_count && ext_count < 14; i++) exts[ext_count++] = sdl_exts[i];
    // HDR10 output needs the extended colour spaces of VK_EXT_swapchain_colorspace.
    {
        u32 n = 0;
        vkEnumerateInstanceExtensionProperties(NULL, &n, NULL);
        VkExtensionProperties props[256];
        n = DG_MIN(n, 256u);
        vkEnumerateInstanceExtensionProperties(NULL, &n, props);
        for (u32 i = 0; i < n; i++)
            if (!strcmp(props[i].extensionName, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME) && ext_count < 14) {
                exts[ext_count++] = VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME;
                R->has_colorspace_ext = true;
            }
    }

    const char *layers[1];
    u32 layer_count = 0;
    R->validation = false;
    if (want_validation()) {
        if (has_layer("VK_LAYER_KHRONOS_validation")) {
            layers[layer_count++] = "VK_LAYER_KHRONOS_validation";
            exts[ext_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
            R->validation = true;
        } else {
            log_warn("validation requested but VK_LAYER_KHRONOS_validation not found");
        }
    }

    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "Deadground";
    app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app.pEngineName = "Deadground";
    app.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    if (vkport_instance(exts, &ext_count, 16, &ici.flags)) log_info("vulkan: portability enumeration enabled");
    ici.enabledExtensionCount = ext_count;
    ici.ppEnabledExtensionNames = exts;
    ici.enabledLayerCount = layer_count;
    ici.ppEnabledLayerNames = layers;
    VkResult res = vkCreateInstance(&ici, NULL, &R->instance);
    if (res != VK_SUCCESS) { log_error("vkCreateInstance failed (%d)", (int)res); return false; }
    volkLoadInstance(R->instance);

    if (R->validation) {
        VkDebugUtilsMessengerCreateInfoEXT dci = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
        dci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        dci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        dci.pfnUserCallback = debug_callback;
        vkCreateDebugUtilsMessengerEXT(R->instance, &dci, NULL, &R->messenger);
        log_info("vulkan: validation layer enabled");
    }

    if (!SDL_Vulkan_CreateSurface(window, R->instance, NULL, &R->surface)) {
        log_error("SDL_Vulkan_CreateSurface: %s", SDL_GetError());
        return false;
    }

    // Pick a GPU: prefer discrete, require 1.3 + a graphics queue that can present.
    u32 gpu_count = 0;
    vkEnumeratePhysicalDevices(R->instance, &gpu_count, NULL);
    if (!gpu_count) { log_error("no Vulkan GPU found"); return false; }
    VkPhysicalDevice gpus[16];
    gpu_count = DG_MIN(gpu_count, 16u);
    vkEnumeratePhysicalDevices(R->instance, &gpu_count, gpus);
    int best = -1, best_score = -1;
    u32 best_family = 0;
    for (u32 i = 0; i < gpu_count; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(gpus[i], &p);
        if (p.apiVersion < VK_API_VERSION_1_3) {
            // MoltenVK reports 1.3 from 1.3.0 (Vulkan SDK 1.4.309); older ones report 1.2
            log_warn("vulkan: '%s' skipped: api %u.%u < 1.3 (on macOS: update MoltenVK / the Vulkan SDK)", p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
                     VK_API_VERSION_MINOR(p.apiVersion));
            continue;
        }
        u32 qcount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(gpus[i], &qcount, NULL);
        VkQueueFamilyProperties qprops[16];
        qcount = DG_MIN(qcount, 16u);
        vkGetPhysicalDeviceQueueFamilyProperties(gpus[i], &qcount, qprops);
        for (u32 q = 0; q < qcount; q++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(gpus[i], q, R->surface, &present);
            if ((qprops[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 100 : 10;
                if (score > best_score) { best_score = score; best = (int)i; best_family = q; }
                break;
            }
        }
    }
    if (best < 0) { log_error("no Vulkan 1.3 GPU with present support"); return false; }
    R->gpu = gpus[best];
    R->queue_family = best_family;
    vkGetPhysicalDeviceProperties(R->gpu, &R->gpu_props);
    {
        VkPhysicalDeviceVulkan12Properties p12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES };
        VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        p2.pNext = &p12;
        vkGetPhysicalDeviceProperties2(R->gpu, &p2);
        R->driver_id = p12.driverID;
        log_info("vulkan: GPU '%s' (api %u.%u.%u, vendor %04x, driver %s %s)", R->gpu_props.deviceName, VK_API_VERSION_MAJOR(R->gpu_props.apiVersion),
                 VK_API_VERSION_MINOR(R->gpu_props.apiVersion), VK_API_VERSION_PATCH(R->gpu_props.apiVersion), R->gpu_props.vendorID, p12.driverName,
                 p12.driverInfo);
    }

    VkPhysicalDeviceFeatures supported;
    vkGetPhysicalDeviceFeatures(R->gpu, &supported);
    R->has_wireframe = supported.fillModeNonSolid == VK_TRUE;
    R->max_anisotropy = supported.samplerAnisotropy ? R->gpu_props.limits.maxSamplerAnisotropy : 1.0f;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = R->queue_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    f13.shaderDemoteToHelperInvocation = VK_TRUE;
    VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    f12.pNext = &f13;
    // Virtualized geometry (docs/VGEO.md): scalar layouts, int64, indirect count draws, descriptor
    // indexing. Enabled when present; vgeo stays off without scalar layouts / int64 / non-uniform
    // texture indexing. 64-bit buffer atomics are optional: without them (MoltenVK) the visibility
    // buffer takes the 32-bit two-phase writes (vgeo_raster.h).
    {
        VkPhysicalDeviceVulkan12Features s12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
        VkPhysicalDeviceFeatures2 s2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        s2.pNext = &s12;
        vkGetPhysicalDeviceFeatures2(R->gpu, &s2);
        f12.scalarBlockLayout = s12.scalarBlockLayout;
        f12.shaderBufferInt64Atomics = s12.shaderBufferInt64Atomics;
        f12.drawIndirectCount = s12.drawIndirectCount;
        f12.runtimeDescriptorArray = s12.runtimeDescriptorArray;
        f12.descriptorBindingPartiallyBound = s12.descriptorBindingPartiallyBound;
        f12.shaderSampledImageArrayNonUniformIndexing = s12.shaderSampledImageArrayNonUniformIndexing;
        R->vg_scalar = s12.scalarBlockLayout;
        R->vg_int64 = s2.features.shaderInt64;
        R->vg_atomic64 = s12.shaderBufferInt64Atomics && s2.features.shaderInt64;
        R->vg_indirect_count = s12.drawIndirectCount && s2.features.multiDrawIndirect;
        R->vg_descriptor_indexing = s12.runtimeDescriptorArray && s12.descriptorBindingPartiallyBound && s12.shaderSampledImageArrayNonUniformIndexing;
        supported.shaderInt64 = s2.features.shaderInt64;
        supported.multiDrawIndirect = s2.features.multiDrawIndirect;
        supported.drawIndirectFirstInstance = s2.features.drawIndirectFirstInstance;
    }
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    f2.pNext = &f12;
    f2.features.samplerAnisotropy = supported.samplerAnisotropy;
    f2.features.fillModeNonSolid = supported.fillModeNonSolid;
    f2.features.textureCompressionBC = supported.textureCompressionBC;
    // Post-processing: storage images of every format (r8, rg16f, r11g11b10...) without format
    // qualifiers where needed, independent blend for MRT pipelines.
    f2.features.shaderStorageImageExtendedFormats = supported.shaderStorageImageExtendedFormats;
    f2.features.shaderStorageImageReadWithoutFormat = supported.shaderStorageImageReadWithoutFormat;
    f2.features.shaderStorageImageWriteWithoutFormat = supported.shaderStorageImageWriteWithoutFormat;
    f2.features.independentBlend = supported.independentBlend;
    f2.features.fragmentStoresAndAtomics = supported.fragmentStoresAndAtomics;
    f2.features.vertexPipelineStoresAndAtomics = supported.vertexPipelineStoresAndAtomics;   // vgeo vertex pulling (shared writable set)
    f2.features.shaderInt64 = supported.shaderInt64;
    f2.features.multiDrawIndirect = supported.multiDrawIndirect;
    f2.features.drawIndirectFirstInstance = supported.drawIndirectFirstInstance;
    if (!supported.shaderStorageImageExtendedFormats) log_warn("vulkan: shaderStorageImageExtendedFormats missing - some post effects disabled");

    const char *dev_exts[8] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    bool unused_att = false;
    u32 dev_ext_count = 1;
    R->portability = vkport_device(R->gpu, dev_exts, &dev_ext_count, 8);
    if (R->portability) log_portability_subset(R->gpu);
    {
        u32 n = 0;
        vkEnumerateDeviceExtensionProperties(R->gpu, NULL, &n, NULL);
        VkExtensionProperties *props = (VkExtensionProperties *)malloc(sizeof(VkExtensionProperties) * n);
        vkEnumerateDeviceExtensionProperties(R->gpu, NULL, &n, props);
        for (u32 i = 0; i < n; i++)
            if (!strcmp(props[i].extensionName, VK_EXT_MESH_SHADER_EXTENSION_NAME)) R->vg_mesh_shader = true;
            else if (!strcmp(props[i].extensionName, VK_EXT_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_EXTENSION_NAME)) unused_att = true;
            else if (!strcmp(props[i].extensionName, VK_EXT_DEVICE_FAULT_EXTENSION_NAME)) R->has_device_fault = true;
            else if (!strcmp(props[i].extensionName, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) {
                dev_exts[dev_ext_count++] = VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME;
                R->has_push_descriptor = true;
            }
        free(props);
        if (!R->has_push_descriptor) log_error("vulkan: VK_KHR_push_descriptor missing - post-processing disabled");
    }
    // vgeo HW rasteriser (vgeo_raster.c): VK_EXT_mesh_shader without task shaders, maintenance4
    // (glslang's LocalSizeId in mesh shaders); vertex pulling otherwise.
    VkPhysicalDeviceMeshShaderFeaturesEXT mesh = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT };
    if (R->vg_mesh_shader) {
        VkPhysicalDeviceMeshShaderFeaturesEXT sm = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT };
        VkPhysicalDeviceVulkan13Features s13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        s13.pNext = &sm;
        VkPhysicalDeviceFeatures2 s2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        s2.pNext = &s13;
        vkGetPhysicalDeviceFeatures2(R->gpu, &s2);
        R->vg_mesh_shader = sm.meshShader && s13.maintenance4;
        if (R->vg_mesh_shader) {
            mesh.meshShader = VK_TRUE;
            f13.maintenance4 = VK_TRUE;
            mesh.pNext = f13.pNext;
            f13.pNext = &mesh;
            dev_exts[dev_ext_count++] = VK_EXT_MESH_SHADER_EXTENSION_NAME;
        }
    }
    // Engine Fidelity: levels without temporal passes render the G-buffer without its velocity target
    // (a null attachment view under a pipeline that has the format): needs this extension.
    VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT ua = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_FEATURES_EXT };
    if (unused_att) {
        VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT su = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_FEATURES_EXT };
        VkPhysicalDeviceFeatures2 s2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        s2.pNext = &su;
        vkGetPhysicalDeviceFeatures2(R->gpu, &s2);
        if (su.dynamicRenderingUnusedAttachments) {
            ua.dynamicRenderingUnusedAttachments = VK_TRUE;
            ua.pNext = f13.pNext;
            f13.pNext = &ua;
            dev_exts[dev_ext_count++] = VK_EXT_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_EXTENSION_NAME;
            R->has_unused_attachments = true;
        }
    }
    if (!R->has_unused_attachments) log_info("vulkan: no VK_EXT_dynamic_rendering_unused_attachments: the G-buffer velocity is always written");
    // VK_EXT_device_fault: on VK_ERROR_DEVICE_LOST the log gets the faulting addresses and the vendor's
    // description (vkr_check_failed).
    VkPhysicalDeviceFaultFeaturesEXT fault = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT };
    if (R->has_device_fault) {
        VkPhysicalDeviceFaultFeaturesEXT sf = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT };
        VkPhysicalDeviceFeatures2 s2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        s2.pNext = &sf;
        vkGetPhysicalDeviceFeatures2(R->gpu, &s2);
        R->has_device_fault = sf.deviceFault;
        if (R->has_device_fault) {
            fault.deviceFault = VK_TRUE;
            fault.deviceFaultVendorBinary = VK_FALSE;
            fault.pNext = f13.pNext;
            f13.pNext = &fault;
            dev_exts[dev_ext_count++] = VK_EXT_DEVICE_FAULT_EXTENSION_NAME;
        }
    }
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = dev_ext_count;
    dci.ppEnabledExtensionNames = dev_exts;
    res = vkCreateDevice(R->gpu, &dci, NULL, &R->device);
    if (res != VK_SUCCESS) { log_error("vkCreateDevice failed (%d)", (int)res); return false; }
    volkLoadDevice(R->device);
    vkGetDeviceQueue(R->device, R->queue_family, 0, &R->queue);

    VmaVulkanFunctions vf = { 0 };
    vf.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    vf.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo aci = { 0 };
    aci.vulkanApiVersion = VK_API_VERSION_1_3;
    aci.physicalDevice = R->gpu;
    aci.device = R->device;
    aci.instance = R->instance;
    aci.pVulkanFunctions = &vf;
    VK_CHECK(vmaCreateAllocator(&aci, &R->vma));
    return true;
}

void vkd_destroy_device(void) {
    if (R->vma) vmaDestroyAllocator(R->vma);
    if (R->device) vkDestroyDevice(R->device, NULL);
    if (R->surface) vkDestroySurfaceKHR(R->instance, R->surface, NULL);
    if (R->messenger) vkDestroyDebugUtilsMessengerEXT(R->instance, R->messenger, NULL);
    if (R->instance) vkDestroyInstance(R->instance, NULL);
}

bool vkd_create_swapchain(void) {
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(R->gpu, R->surface, &caps));
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(g_engine.window, &w, &h);
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) { extent.width = (u32)w; extent.height = (u32)h; }
    extent.width = DG_CLAMP(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
    extent.height = DG_CLAMP(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    if (extent.width == 0 || extent.height == 0) return false; // minimized

    u32 fmt_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(R->gpu, R->surface, &fmt_count, NULL);
    VkSurfaceFormatKHR fmts[64];
    fmt_count = DG_MIN(fmt_count, 64u);
    vkGetPhysicalDeviceSurfaceFormatsKHR(R->gpu, R->surface, &fmt_count, fmts);
    VkSurfaceFormatKHR chosen = fmts[0];
    for (u32 i = 0; i < fmt_count; i++) {
        if ((fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM || fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM) &&
            fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { chosen = fmts[i]; break; }
    }
    // HDR10 (ST.2084 PQ, BT.2020 primaries) when the display offers it and r_hdr_output is on.
    R->hdr_supported = false;
    for (u32 i = 0; i < fmt_count; i++)
        if (R->has_colorspace_ext && fmts[i].colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT &&
            (fmts[i].format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || fmts[i].format == VK_FORMAT_A2R10G10B10_UNORM_PACK32)) {
            R->hdr_supported = true;
            if (R->r_hdr_output && R->r_hdr_output->i) chosen = fmts[i];
        }
    R->swap_hdr = chosen.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT;
    R->hdr_output_applied = R->r_hdr_output ? R->r_hdr_output->i : 0;

    u32 pm_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(R->gpu, R->surface, &pm_count, NULL);
    VkPresentModeKHR modes[16];
    pm_count = DG_MIN(pm_count, 16u);
    vkGetPhysicalDeviceSurfacePresentModesKHR(R->gpu, R->surface, &pm_count, modes);
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    bool vsync = R->r_vsync->i != 0;
    if (!vsync) {
        bool has_mailbox = false, has_immediate = false;
        for (u32 i = 0; i < pm_count; i++) {
            if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) has_mailbox = true;
            if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) has_immediate = true;
        }
        if (has_immediate) mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
        else if (has_mailbox) mode = VK_PRESENT_MODE_MAILBOX_KHR;
    }
    R->vsync_applied = vsync;

    // Triple buffering: request 3 images.
    u32 image_count = DG_MAX(3u, caps.minImageCount);
    if (caps.maxImageCount && image_count > caps.maxImageCount) image_count = caps.maxImageCount;

    VkSwapchainKHR old = R->swapchain;
    VkSwapchainCreateInfoKHR sci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    sci.surface = R->surface;
    sci.minImageCount = image_count;
    sci.imageFormat = chosen.format;
    sci.imageColorSpace = chosen.colorSpace;
    sci.imageExtent = extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = mode;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = old;
    VK_CHECK(vkCreateSwapchainKHR(R->device, &sci, NULL, &R->swapchain));
    if (old) {
        for (u32 i = 0; i < R->swap_count; i++) {
            vkDestroyImageView(R->device, R->swap_views[i], NULL);
            vkDestroySemaphore(R->device, R->render_done[i], NULL);
        }
        vkDestroySwapchainKHR(R->device, old, NULL);
    }

    R->swap_format = chosen.format;
    R->swap_extent = extent;
    vkGetSwapchainImagesKHR(R->device, R->swapchain, &R->swap_count, NULL);
    R->swap_count = DG_MIN(R->swap_count, (u32)MAX_SWAP_IMAGES);
    vkGetSwapchainImagesKHR(R->device, R->swapchain, &R->swap_count, R->swap_images);
    for (u32 i = 0; i < R->swap_count; i++) {
        VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image = R->swap_images[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = R->swap_format;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.levelCount = 1;
        vci.subresourceRange.layerCount = 1;
        VK_CHECK(vkCreateImageView(R->device, &vci, NULL, &R->swap_views[i]));
        R->swap_layouts[i] = VK_IMAGE_LAYOUT_UNDEFINED;
        VkSemaphoreCreateInfo sem = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VK_CHECK(vkCreateSemaphore(R->device, &sem, NULL, &R->render_done[i]));
    }
    static const char *mode_names[] = { "immediate", "mailbox", "fifo", "fifo_relaxed" };
    log_info("swapchain: %ux%u, %u images, present=%s, %s%s", extent.width, extent.height, R->swap_count,
             mode <= 3 ? mode_names[mode] : "?", R->swap_hdr ? "HDR10 (PQ)" : "SDR (sRGB)", R->hdr_supported ? ", display supports HDR10" : "");
    R->swap_dirty = false;
    return true;
}

void vkd_destroy_swapchain(void) {
    for (u32 i = 0; i < R->swap_count; i++) {
        vkDestroyImageView(R->device, R->swap_views[i], NULL);
        vkDestroySemaphore(R->device, R->render_done[i], NULL);
    }
    if (R->swapchain) vkDestroySwapchainKHR(R->device, R->swapchain, NULL);
    R->swapchain = VK_NULL_HANDLE;
    R->swap_count = 0;
}

void vkd_create_targets(void) {
    float scale = clampf(R->target_scale > 0.0f ? R->target_scale : 1.0f, 0.25f, 2.0f);   // render.c: r_render_scale x upscaler
    u32 w = DG_MAX(1u, (u32)(R->swap_extent.width * scale));
    u32 h = DG_MAX(1u, (u32)(R->swap_extent.height * scale));
    R->hdr = vkr_target_create(w, h, VK_FORMAT_R16G16B16A16_SFLOAT,
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT);
    // Depth is kept (STORE) and sampled: soft particles, decals and next-frame GPU particle collision.
    R->depth = vkr_target_create(w, h, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                 VK_IMAGE_ASPECT_DEPTH_BIT);
    // Viewmodel depth: separate so the world depth survives the first-person pass.
    R->vm_depth = vkr_target_create(w, h, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);   // sampled: sight lens occlusion
    R->render_scale_applied = scale;
    vkx_targets_changed();
}

void vkd_destroy_targets(void) {
    vkr_target_destroy(&R->hdr);
    vkr_target_destroy(&R->depth);
    vkr_target_destroy(&R->vm_depth);
}

void vkd_create_shadow_map(u32 size) {
    if (R->shadow.image) vkr_target_destroy(&R->shadow);
    R->shadow = vkr_target_create(size, size, VK_FORMAT_D32_SFLOAT,
                                  VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    R->shadow_size = size;
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) {
        if (!R->frames[i].frame_set) continue;
        // 1: compare sampler (PCF taps), 15: raw depth (PCSS blocker search).
        VkDescriptorImageInfo ii[2] = { { R->sampler_shadow, R->shadow.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                                        { R->sampler_depth, R->shadow.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
        VkDescriptorSet sets[FRAME_SET_COUNT];
        frame_sets_of(&R->frames[i], sets);
        for (int k = 0; k < FRAME_SET_COUNT; k++) {
            if (!sets[k]) continue;
            VkWriteDescriptorSet w[2];
            for (int b = 0; b < 2; b++) {
                w[b] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                w[b].dstSet = sets[k];
                w[b].dstBinding = b ? 15 : 1;
                w[b].descriptorCount = 1;
                w[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w[b].pImageInfo = &ii[b];
            }
            vkUpdateDescriptorSets(R->device, 2, w, 0, NULL);
        }
    }
}

// The viewmodel shadow tile (docs/SHADOWS.md): its own D32 map, bindings 16 (compare) / 17 (raw).
// Always allocated (8 x 8 with shadows off) so every frame set binds a valid image.
void vkd_create_vm_shadow_map(u32 size) {
    if (R->vm_shadow.image) vkr_target_destroy(&R->vm_shadow);
    R->vm_shadow = vkr_target_create(size, size, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                     VK_IMAGE_ASPECT_DEPTH_BIT);
    R->vm_shadow_size = size;
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) {
        if (!R->frames[i].frame_set) continue;
        VkDescriptorImageInfo ii[2] = { { R->sampler_shadow, R->vm_shadow.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                                        { R->sampler_depth, R->vm_shadow.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
        VkDescriptorSet sets[FRAME_SET_COUNT];
        frame_sets_of(&R->frames[i], sets);
        for (int k = 0; k < FRAME_SET_COUNT; k++) {
            if (!sets[k]) continue;
            VkWriteDescriptorSet w[2];
            for (int b = 0; b < 2; b++) {
                w[b] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                w[b].dstSet = sets[k];
                w[b].dstBinding = 16u + (u32)b;
                w[b].descriptorCount = 1;
                w[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w[b].pImageInfo = &ii[b];
            }
            vkUpdateDescriptorSets(R->device, 2, w, 0, NULL);
        }
    }
}

// The moon's map: D32, bindings 18 (compare) / 19 (raw). Always allocated (16 x 16 when unused).
void vkd_create_moon_shadow_map(u32 size) {
    if (R->moon_shadow.image) vkr_target_destroy(&R->moon_shadow);
    R->moon_shadow = vkr_target_create(size, size, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                       VK_IMAGE_ASPECT_DEPTH_BIT);
    R->moon_shadow_size = size;
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) {
        if (!R->frames[i].frame_set) continue;
        VkDescriptorImageInfo ii[2] = { { R->sampler_shadow, R->moon_shadow.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                                        { R->sampler_depth, R->moon_shadow.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
        VkDescriptorSet sets[FRAME_SET_COUNT];
        frame_sets_of(&R->frames[i], sets);
        for (int k = 0; k < FRAME_SET_COUNT; k++) {
            if (!sets[k]) continue;
            VkWriteDescriptorSet w[2];
            for (int b = 0; b < 2; b++) {
                w[b] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                w[b].dstSet = sets[k];
                w[b].dstBinding = 18u + (u32)b;
                w[b].descriptorCount = 1;
                w[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w[b].pImageInfo = &ii[b];
            }
            vkUpdateDescriptorSets(R->device, 2, w, 0, NULL);
        }
    }
}

// VK_CHECK failure: a lost device first logs what VK_EXT_device_fault knows (addresses, vendor codes).
void vkr_check_failed(VkResult r, const char *file, int line, const char *expr) {
    if (r == VK_ERROR_DEVICE_LOST && R->device && R->has_device_fault && vkGetDeviceFaultInfoEXT) {
        VkDeviceFaultCountsEXT c = { VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT };
        VkResult cr = vkGetDeviceFaultInfoEXT(R->device, &c, NULL);
        if (cr != VK_SUCCESS) log_error("device fault: no information (%d)", (int)cr);
        else {
            VkDeviceFaultAddressInfoEXT addr[16];
            VkDeviceFaultVendorInfoEXT vend[16];
            if (c.addressInfoCount > 16) c.addressInfoCount = 16;
            if (c.vendorInfoCount > 16) c.vendorInfoCount = 16;
            c.vendorBinarySize = 0;
            VkDeviceFaultInfoEXT fi = { VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT };
            fi.pAddressInfos = addr;
            fi.pVendorInfos = vend;
            VkResult fr = vkGetDeviceFaultInfoEXT(R->device, &c, &fi);
            log_error("device fault (%d): %s; %u addresses, %u vendor records", (int)fr, fi.description, c.addressInfoCount, c.vendorInfoCount);
            for (u32 i = 0; i < c.addressInfoCount; i++)
                log_error("  address type %d at 0x%llx (precision 0x%llx)", (int)addr[i].addressType, (unsigned long long)addr[i].reportedAddress,
                          (unsigned long long)addr[i].addressPrecision);
            for (u32 i = 0; i < c.vendorInfoCount; i++)
                log_error("  vendor: %s (code 0x%llx, data 0x%llx)", vend[i].description, (unsigned long long)vend[i].vendorFaultCode,
                          (unsigned long long)vend[i].vendorFaultData);
        }
    }
    log_fatal("Vulkan error %d at %s:%d (%s)", (int)r, file, line, expr);
}
