// Vulkan portability (docs/PLATFORM_MACOS.md "Vulkan instance / device"): what a non-conformant
// implementation (MoltenVK on macOS) needs from every Vulkan user of the engine (the renderer, the
// window-less vgeo harness, tools/bench_vgeo):
//   - instance: VK_KHR_portability_enumeration + VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR when
//     the loader lists it (the Khronos loader hides portability drivers otherwise). MoltenVK loaded
//     directly (the .app bundle) does not list it and enumerates itself anyway.
//   - device: VK_KHR_portability_subset enabled whenever the device lists it (required by the spec).
// Both are no-ops on Windows / Linux drivers.
#ifndef DG_VK_PORTABILITY_H
#define DG_VK_PORTABILITY_H

#include "volk.h"
#include "core/base.h"
#include <stdlib.h>
#include <string.h>

#ifndef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
#define VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME "VK_KHR_portability_subset"
#endif

// True when the loader (instance level, no layer) lists `name`.
DG_INLINE bool vkport_instance_has(const char *name) {
    u32 n = 0;
    if (vkEnumerateInstanceExtensionProperties(NULL, &n, NULL) != VK_SUCCESS || !n) return false;
    VkExtensionProperties *p = (VkExtensionProperties *)malloc(sizeof(VkExtensionProperties) * n);
    bool found = false;
    if (vkEnumerateInstanceExtensionProperties(NULL, &n, p) == VK_SUCCESS)
        for (u32 i = 0; i < n && !found; i++) found = !strcmp(p[i].extensionName, name);
    free(p);
    return found;
}

// Appends VK_KHR_portability_enumeration (when listed and not yet in exts) and sets the flag.
DG_INLINE bool vkport_instance(const char **exts, u32 *count, u32 cap, VkInstanceCreateFlags *flags) {
    if (*count >= cap || !vkport_instance_has(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) return false;
    for (u32 i = 0; i < *count; i++)
        if (!strcmp(exts[i], VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) { *flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR; return true; }
    exts[(*count)++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
    *flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    return true;
}

DG_INLINE bool vkport_device_has(VkPhysicalDevice gpu, const char *name) {
    u32 n = 0;
    if (vkEnumerateDeviceExtensionProperties(gpu, NULL, &n, NULL) != VK_SUCCESS || !n) return false;
    VkExtensionProperties *p = (VkExtensionProperties *)malloc(sizeof(VkExtensionProperties) * n);
    bool found = false;
    if (vkEnumerateDeviceExtensionProperties(gpu, NULL, &n, p) == VK_SUCCESS)
        for (u32 i = 0; i < n && !found; i++) found = !strcmp(p[i].extensionName, name);
    free(p);
    return found;
}

// Appends VK_KHR_portability_subset when the device lists it: true on a portability implementation.
DG_INLINE bool vkport_device(VkPhysicalDevice gpu, const char **exts, u32 *count, u32 cap) {
    if (*count >= cap || !vkport_device_has(gpu, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME)) return false;
    exts[(*count)++] = VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME;
    return true;
}

#endif
