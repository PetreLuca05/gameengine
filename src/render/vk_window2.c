// The second window (render.h "Second window"): its own surface + swapchain on the main device,
// recorded into the main frame (its UI pass after the final pass, the same graph and submit) and
// presented with the main swapchain. Its image is acquired without waiting: a frame where none is
// free skips it (the main window never waits for the second one).
#include "vk_internal.h"
#include "render/phone_view.h"
#include "graph/fg.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <stdio.h>
#include <string.h>
#include "stb_image_write.h"

static struct {
    SDL_Window *win;
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkSurfaceFormatKHR format;
    VkExtent2D extent;
    u32 count;
    VkImage images[MAX_SWAP_IMAGES];
    VkImageView views[MAX_SWAP_IMAGES];
    VkSemaphore done[MAX_SWAP_IMAGES];
    VkSemaphore acquired[FRAMES_IN_FLIGHT];
    VkImageLayout layout;      // this frame's image (fg_import tracks it)
    bool open, dirty;
    bool have_image;           // acquired this frame
    bool drawn;                // its pass was recorded (the image is presented)
    u32 index;
    FgRes res;
    char shot_path[512];
    GpuBuffer shot;
    bool shot_armed;
} W2;

static void destroy_swapchain(void) {
    for (u32 i = 0; i < W2.count; i++) {
        vkDestroyImageView(R->device, W2.views[i], NULL);
        vkDestroySemaphore(R->device, W2.done[i], NULL);
    }
    W2.count = 0;
    if (W2.swapchain) vkDestroySwapchainKHR(R->device, W2.swapchain, NULL);
    W2.swapchain = VK_NULL_HANDLE;
}

static bool create_swapchain(void) {
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(R->gpu, W2.surface, &caps) != VK_SUCCESS) return false;
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(W2.win, &w, &h);
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) { extent.width = (u32)w; extent.height = (u32)h; }
    extent.width = DG_CLAMP(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
    extent.height = DG_CLAMP(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    if (extent.width == 0 || extent.height == 0) return false;   // minimised
    // Never waits on vsync: mailbox, else immediate, else fifo (then the acquire just skips frames).
    u32 pm_count = 0;
    VkPresentModeKHR modes[16];
    vkGetPhysicalDeviceSurfacePresentModesKHR(R->gpu, W2.surface, &pm_count, NULL);
    pm_count = DG_MIN(pm_count, 16u);
    vkGetPhysicalDeviceSurfacePresentModesKHR(R->gpu, W2.surface, &pm_count, modes);
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    for (u32 i = 0; i < pm_count; i++) if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) mode = modes[i];
    for (u32 i = 0; i < pm_count; i++) if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) mode = modes[i];
    u32 image_count = DG_MAX(3u, caps.minImageCount);
    if (caps.maxImageCount && image_count > caps.maxImageCount) image_count = caps.maxImageCount;
    VkSwapchainKHR old = W2.swapchain;
    VkSwapchainCreateInfoKHR sci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    sci.surface = W2.surface;
    sci.minImageCount = image_count;
    sci.imageFormat = W2.format.format;
    sci.imageColorSpace = W2.format.colorSpace;
    sci.imageExtent = extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = mode;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = old;
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    if (vkCreateSwapchainKHR(R->device, &sci, NULL, &sc) != VK_SUCCESS) return false;
    destroy_swapchain();   // the old one (retired by the new)
    W2.swapchain = sc;
    W2.extent = extent;
    vkGetSwapchainImagesKHR(R->device, sc, &W2.count, NULL);
    W2.count = DG_MIN(W2.count, (u32)MAX_SWAP_IMAGES);
    vkGetSwapchainImagesKHR(R->device, sc, &W2.count, W2.images);
    for (u32 i = 0; i < W2.count; i++) {
        VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image = W2.images[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = W2.format.format;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.levelCount = 1;
        vci.subresourceRange.layerCount = 1;
        VK_CHECK(vkCreateImageView(R->device, &vci, NULL, &W2.views[i]));
        VkSemaphoreCreateInfo sem = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VK_CHECK(vkCreateSemaphore(R->device, &sem, NULL, &W2.done[i]));
    }
    W2.dirty = false;
    log_info("second window: swapchain %ux%u, %u images", extent.width, extent.height, W2.count);
    return true;
}

bool render_window2_open(struct SDL_Window *window) {
    if (!R || !R->device || !window) return false;
    if (W2.open) render_window2_close();
    memset(&W2, 0, sizeof(W2));
    W2.win = window;
    if (!SDL_Vulkan_CreateSurface(window, R->instance, NULL, &W2.surface)) {
        log_error("second window: SDL_Vulkan_CreateSurface: %s", SDL_GetError());
        return false;
    }
    VkBool32 present = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(R->gpu, R->queue_family, W2.surface, &present);
    // The UI pipeline (PIPE_UI) is built for the main swapchain's format: the same format + colour space.
    u32 n = 0;
    VkSurfaceFormatKHR fmts[64];
    vkGetPhysicalDeviceSurfaceFormatsKHR(R->gpu, W2.surface, &n, NULL);
    n = DG_MIN(n, 64u);
    vkGetPhysicalDeviceSurfaceFormatsKHR(R->gpu, W2.surface, &n, fmts);
    bool found = false;
    for (u32 i = 0; i < n && !found; i++)
        if (fmts[i].format == R->swap_format && (fmts[i].colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT) == R->swap_hdr) { W2.format = fmts[i]; found = true; }
    if (!present || !found) {
        log_error("second window: %s", !present ? "the graphics queue can't present to it" : "no surface format matching the main window's");
        vkDestroySurfaceKHR(R->instance, W2.surface, NULL);
        memset(&W2, 0, sizeof(W2));
        return false;
    }
    VkSemaphoreCreateInfo sem = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) VK_CHECK(vkCreateSemaphore(R->device, &sem, NULL, &W2.acquired[i]));
    W2.open = true;
    W2.dirty = !create_swapchain();
    return true;
}

void render_window2_close(void) {
    if (!W2.open) return;
    vkDeviceWaitIdle(R->device);
    destroy_swapchain();
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) vkDestroySemaphore(R->device, W2.acquired[i], NULL);
    vkDestroySurfaceKHR(R->instance, W2.surface, NULL);
    vkr_buffer_destroy_now(&W2.shot);
    memset(&W2, 0, sizeof(W2));
    log_info("second window: closed");
}

bool render_window2_active(void) { return R && W2.open; }

void render_window2_size(int *w, int *h) {
    *w = W2.open && W2.swapchain ? (int)W2.extent.width : 0;
    *h = W2.open && W2.swapchain ? (int)W2.extent.height : 0;
}

void render_window2_screenshot(const char *path) {
    if (W2.open) dg_strcpy(W2.shot_path, sizeof(W2.shot_path), path);
}

// render_end_frame, before recording: follow the window's size, acquire an image if one is free.
void vkw2_frame_begin(void) {
    W2.have_image = W2.drawn = false;
    W2.res = FG_NONE;
    if (!W2.open) return;
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(W2.win, &w, &h);
    if (w <= 0 || h <= 0 || (SDL_GetWindowFlags(W2.win) & SDL_WINDOW_MINIMIZED)) return;
    if (W2.dirty || !W2.swapchain || (u32)w != W2.extent.width || (u32)h != W2.extent.height) {
        vkDeviceWaitIdle(R->device);
        if (!create_swapchain()) { W2.dirty = true; return; }
    }
    VkResult res = vkAcquireNextImageKHR(R->device, W2.swapchain, 0, W2.acquired[R->frame_slot], VK_NULL_HANDLE, &W2.index);
    if (res == VK_ERROR_OUT_OF_DATE_KHR) { W2.dirty = true; return; }
    if (res == VK_SUBOPTIMAL_KHR) W2.dirty = true;
    else if (res != VK_SUCCESS) return;   // none free (VK_NOT_READY / VK_TIMEOUT): skip this frame
    W2.have_image = true;
}

static void exec_window2(VkCommandBuffer cmd, void *u) {
    (void)u;
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    ca.imageView = W2.views[W2.index];
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ca.clearValue.color.float32[0] = 0.09f;
    ca.clearValue.color.float32[1] = 0.10f;
    ca.clearValue.color.float32[2] = 0.11f;
    ca.clearValue.color.float32[3] = 1.0f;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea.extent = W2.extent;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cmd, &ri);
    VkViewport vp = { 0, 0, (float)W2.extent.width, (float)W2.extent.height, 0, 1 };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkr_ui_draw_cmds(cmd, R->ui_cmds + R->w2_ui_first, R->w2_ui_ccount, W2.extent.width, W2.extent.height, PIPE_UI);
    vkCmdEndRendering(cmd);
}

// build_frame_graph, after the final pass: the window's UI (it samples phone view 2's scene image).
void vkw2_build(void) {
    if (!W2.have_image) return;
    W2.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    W2.res = fg_import("window2", W2.images[W2.index], W2.views[W2.index], W2.format.format, W2.extent.width, W2.extent.height, VK_IMAGE_ASPECT_COLOR_BIT,
                       &W2.layout);
    int p = fg_pass("window2_ui", exec_window2, NULL);
    FgRes scene = phone_view_scene_res(RENDER_PHONE_VIEW_WINDOW2);
    if (scene != FG_NONE) fg_read(p, scene, FG_SAMPLED);
    fg_write(p, W2.res, FG_COLOR);
    W2.drawn = p >= 0;
}

// After the graph (the frame's last command buffer): the image to present (+ the optional copy).
void vkw2_finish(VkCommandBuffer cmd) {
    if (!W2.drawn) return;
    VkImage img = W2.images[W2.index];
    VkImageLayout from = W2.layout;
    if (W2.shot_path[0]) {
        u32 w = W2.extent.width, h = W2.extent.height;
        vkr_buffer_destroy_now(&W2.shot);
        W2.shot = vkr_buffer_create_readback((VkDeviceSize)w * h * 4);
        vkr_image_barrier(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, 1);
        VkBufferImageCopy region = { 0 };
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent.width = w;
        region.imageExtent.height = h;
        region.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, W2.shot.buffer, 1, &region);
        vkr_image_barrier(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                          VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_NONE, 0, 1);
        W2.shot_armed = true;
        return;
    }
    vkr_image_barrier(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, from, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0, 1);
}

// The submit's extra wait (the acquired image) / signal (its present); VK_NULL_HANDLE: none.
void vkw2_semaphores(VkSemaphore *wait, VkSemaphore *signal) {
    *wait = W2.drawn ? W2.acquired[R->frame_slot] : VK_NULL_HANDLE;
    *signal = W2.drawn ? W2.done[W2.index] : VK_NULL_HANDLE;
}

// Adds the window's image to the frame's present; returns the swapchains in it (1 or 2).
u32 vkw2_present(VkSwapchainKHR *swapchains, u32 *indices, VkSemaphore *waits) {
    if (!W2.drawn) return 1;
    swapchains[1] = W2.swapchain;
    indices[1] = W2.index;
    waits[1] = W2.done[W2.index];
    return 2;
}

void vkw2_present_result(VkResult r) {
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) W2.dirty = true;
}

bool vkw2_shot_pending(void) { return W2.shot_armed; }

// After the frame's fence: the copy is complete.
void vkw2_shot_finish(void) {
    if (!W2.shot_armed) return;
    W2.shot_armed = false;
    u32 w = W2.extent.width, h = W2.extent.height;
    vmaInvalidateAllocation(R->vma, W2.shot.alloc, 0, VK_WHOLE_SIZE);
    u8 *px = (u8 *)W2.shot.mapped;
    bool bgra = W2.format.format == VK_FORMAT_B8G8R8A8_UNORM || W2.format.format == VK_FORMAT_B8G8R8A8_SRGB;
    for (u32 i = 0; i < w * h; i++) {
        if (bgra) { u8 t = px[i * 4]; px[i * 4] = px[i * 4 + 2]; px[i * 4 + 2] = t; }
        px[i * 4 + 3] = 255;
    }
    stbi_write_png_compression_level = 1;
    if (stbi_write_png(W2.shot_path, (int)w, (int)h, 4, px, (int)w * 4)) log_info("second window screenshot saved: %s", W2.shot_path);
    else log_error("second window screenshot: failed to write %s", W2.shot_path);
    vkr_buffer_destroy_now(&W2.shot);
    W2.shot_path[0] = 0;
}

void vkw2_shutdown(void) { render_window2_close(); }
