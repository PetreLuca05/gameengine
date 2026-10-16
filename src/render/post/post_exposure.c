// Auto exposure: histogram (histogram.comp, binding 14) -> percentile average + eye adaptation
// (exposure.comp) into P.exposure_buf, consumed on the GPU by the flare and tonemap passes the
// same frame (no CPU round trip). A copy goes to a host readback buffer for the UI / tests
// (post_exposure_ev, two frames late).
#include "post_internal.h"
#include <string.h>

static FgRes g_color = FG_NONE;

static void exec_exposure(VkCommandBuffer cmd, void *u) {
    (void)u;
    if (!P.exposure_init) {
        vkCmdFillBuffer(cmd, P.exposure_buf.buffer, 0, VK_WHOLE_SIZE, 0);
        vkCmdFillBuffer(cmd, P.histogram_buf.buffer, 0, VK_WHOLE_SIZE, 0);
        post_buffer_barrier(cmd);
        P.exposure_init = true;
    }
    PostBind b = { 0 };
    pb_ubo(&b);
    pb_tex_fg(&b, 0, g_color, P.point);
    pb_buf(&b, 13, P.exposure_buf.buffer, 64, false);
    pb_buf(&b, 14, P.histogram_buf.buffer, 256 * 4, false);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
    u32 w, h;
    fg_extent(g_color, &w, &h);
    post_dispatch(cmd, "histogram", (w + 1) / 2, (h + 1) / 2, 1, 16);
    post_buffer_barrier(cmd);
    post_dispatch(cmd, "exposure", 1, 1, 1, 1);
    post_buffer_barrier(cmd);
    VkBufferCopy c = { 0, 0, 64 };
    vkCmdCopyBuffer(cmd, P.exposure_buf.buffer, P.exposure_rb[R->frame_slot].buffer, 1, &c);
    post_buffer_barrier(cmd);
}

void post_exposure_build(FgRes color) {
    g_color = color;
    int p = fg_pass("exposure", exec_exposure, NULL);
    fg_read(p, color, FG_SAMPLED);
}

// Buffers must be defined before the first reader (autofocus runs before the exposure pass).
static void exec_buffers_init(VkCommandBuffer cmd, void *u) {
    (void)u;
    if (P.exposure_init) return;
    vkCmdFillBuffer(cmd, P.exposure_buf.buffer, 0, VK_WHOLE_SIZE, 0);
    vkCmdFillBuffer(cmd, P.histogram_buf.buffer, 0, VK_WHOLE_SIZE, 0);
    post_buffer_barrier(cmd);
    P.exposure_init = true;
}

void post_buffers_init_build(void) {
    if (!P.exposure_init) fg_pass("post_buffers_init", exec_buffers_init, NULL);
}
