#include "post_internal.h"
#include "core/cvar.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

PostCtx P;

void post_settings_default(PostSettings *s) {
    memset(s, 0, sizeof(*s));
    s->aa = POST_AA_TAA;
    s->upscaler = POST_UP_OFF;
    s->render_scale = 1.0f;
    s->sharpen = 0.35f;
    s->contact_shadows = 1;
    s->ao = 2;
    s->ao_radius = 0.9f;
    s->ao_intensity = 1.0f;
    s->ssgi = 0;
    s->ssgi_intensity = 1.0f;
    s->ssr = 2;
    s->ssr_intensity = 1.0f;
    s->ssr_max_roughness = 0.6f;
    s->volumetrics = 2;
    s->vol_local_lights = true;
    s->fog_density = 0.012f;
    s->fog_height = 0.0f;
    s->fog_falloff = 0.08f;
    s->fog_anisotropy = 0.55f;
    s->sun_shafts = 1.0f;
    s->fog_albedo = v3(0.9f, 0.92f, 0.95f);
    s->motion_blur = 1;
    s->mb_intensity = 0.5f;
    s->dof = 1;
    s->dof_focus = 10.0f;
    s->dof_fstop = 2.8f;
    s->dof_focal_mm = 50.0f;
    s->dof_max_coc = 16.0f;
    s->dof_autofocus = 1;
    s->dof_quality = 2;
    s->bloom = 2;
    s->bloom_intensity = 0.05f;
    s->bloom_dirt = 0.0f;
    s->exposure_auto = 1;
    s->exposure_comp = 0.0f;
    s->exposure_manual = 0.0f;
    s->adapt_up = 2.5f;
    s->adapt_down = 1.2f;
    s->ev_min = -4.0f;
    s->ev_max = 6.0f;
    s->hist_low = 0.5f;
    s->hist_high = 0.95f;
    s->flare = 0.0f;
    s->chromatic = 0.15f;
    s->vignette = 0.25f;
    s->distortion = 0.0f;
    s->grain = 0.04f;
    s->tonemapper = POST_TM_AGX;
    s->lift = v3(0, 0, 0);
    s->gamma = v3(1, 1, 1);
    s->gain = v3(1, 1, 1);
    s->saturation = 1.0f;
    s->contrast = 1.0f;
    s->lut_weight = 1.0f;
    s->brightness = 1.0f;
    s->user_gamma = 1.0f;
    s->paper_white = 200.0f;
    s->max_nits = 1000.0f;
    s->fb_tint = v3(1, 1, 1);
}

const PostSettings *post_settings(void) { return &P.set; }
void render_post_settings(const PostSettings *s) { P.pending = *s; }

float post_halton(u32 index, u32 base) {
    float f = 1.0f, r = 0.0f;
    u32 i = index;
    while (i > 0) {
        f /= (float)base;
        r += f * (float)(i % base);
        i /= base;
    }
    return r;
}

bool post_wants_jitter(void) { return P.ok && !P.set.direct_output && (P.set.aa == POST_AA_TAA || P.set.upscaler == POST_UP_TAAU); }

bool post_direct(void) { return P.ok && P.set.direct_output; }

// Asked while the graph is built, before post_begin_frame: this frame's settings are P.pending.
static bool temporal_aa(const PostSettings *s) { return s->aa == POST_AA_TAA || s->upscaler == POST_UP_TAAU; }
bool post_needs_depth_history(void) {
    const PostSettings *s = &P.pending;
    return P.ok && !s->direct_output && (s->ao || s->ssr || s->ssgi || temporal_aa(s));
}
bool post_needs_velocity(void) {
    const PostSettings *s = &P.pending;
    if (!P.ok || !R->has_unused_attachments) return true;   // no null attachments: always written
    if (s->direct_output) return false;
    return post_needs_depth_history() || s->motion_blur > 0 || s->debug_view == 5;
}

// Off has no histogram: a fixed exposure metered from the light (sun illuminance on the ground plus
// the sky ambient) like a light meter, clamped to the day / night EV range. No adaptation.
static float direct_exposure(const PostFrameIn *in, const PostSettings *s) {
    float lum_sun = 0.2126f * in->sun_color.x + 0.7152f * in->sun_color.y + 0.0722f * in->sun_color.z;
    float lum_sky = 0.2126f * in->sky.x + 0.7152f * in->sky.y + 0.0722f * in->sky.z;
    float e = in->sun_intensity * fmaxf(in->sun_dir.y, 0.0f) * lum_sun + in->ambient * lum_sky;
    float lavg = fmaxf(0.18f * e / 3.14159265f, 1e-6f);   // mid-grey lit by it
    // +1.4 EV: the scene key (light surfaces, the sky in view) measured against the histogram exposure
    // at noon / morning (docs/ENGINE_FIDELITY.md "Off exposure"); night sits on the floor either way.
    float ev = log2f(lavg * 100.0f / 12.5f) + 1.4f;
    ev = clampf(ev, s->ev_min, s->ev_max);
    return 0.18f / (exp2f(ev) * 12.5f / 100.0f) * exp2f(s->exposure_comp);
}
float post_direct_exposure(void) { return P.direct_exposure; }

vec2 post_jitter_pixels(u32 frame) {
    u32 i = (frame % 8) + 1;   // Halton(2,3), 8 phases
    return v2(post_halton(i, 2) - 0.5f, post_halton(i, 3) - 0.5f);
}

float post_render_scale(void) {
    if (!P.ok) return 1.0f;
    float s = P.set.upscaler == POST_UP_OFF ? 1.0f : clampf(P.set.render_scale, 0.5f, 1.0f);
    return s;
}

VkSampler post_linear_sampler(void) { return P.linear; }
VkImageView post_white_view(void) { return P.white; }
VkImageView post_black_view(void) { return P.black; }

// ------------------------------------------------------------------ binding helpers

static void pb_add(PostBind *b, u32 binding, VkDescriptorType type) {
    if (b->n >= 16) return;
    VkWriteDescriptorSet *w = &b->w[b->n];
    memset(w, 0, sizeof(*w));
    w->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w->dstBinding = binding;
    w->descriptorCount = 1;
    w->descriptorType = type;
    if (type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) w->pBufferInfo = &b->buf[b->n];
    else w->pImageInfo = &b->img[b->n];
    b->n++;
}

void pb_tex(PostBind *b, u32 binding, VkImageView view, VkSampler s) {
    if (b->n >= 16) return;
    b->img[b->n] = (VkDescriptorImageInfo){ s ? s : P.linear, view ? view : P.white, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    pb_add(b, binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
}

void pb_tex_general(PostBind *b, u32 binding, VkImageView view, VkSampler s) {
    if (b->n >= 16) return;
    b->img[b->n] = (VkDescriptorImageInfo){ s ? s : P.linear, view ? view : P.white, view ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    pb_add(b, binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
}

void pb_tex_fg(PostBind *b, u32 binding, FgRes r, VkSampler s) {
    if (b->n >= 16) return;
    VkImageLayout lay = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkFormat f = fg_format(r);
    if (f == VK_FORMAT_D32_SFLOAT) lay = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    b->img[b->n] = (VkDescriptorImageInfo){ s ? s : P.linear, fg_valid(r) ? fg_view(r) : P.white, fg_valid(r) ? lay : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    pb_add(b, binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
}

void pb_img(PostBind *b, u32 binding, VkImageView view) {
    if (b->n >= 16) return;
    b->img[b->n] = (VkDescriptorImageInfo){ VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL };
    pb_add(b, binding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
}
void pb_img_fg(PostBind *b, u32 binding, FgRes r) { pb_img(b, binding, fg_view(r)); }

void pb_buf(PostBind *b, u32 binding, VkBuffer buf, VkDeviceSize size, bool uniform) {
    if (b->n >= 16) return;
    b->buf[b->n] = (VkDescriptorBufferInfo){ buf, 0, size };
    pb_add(b, binding, uniform ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
}
void pb_ubo(PostBind *b) { pb_buf(b, 12, P.ubo[R->frame_slot].buffer, sizeof(PostUBO), true); }

void pb_push(VkCommandBuffer cmd, PostBind *b, VkPipelineBindPoint bp) {
    vkCmdPushDescriptorSetKHR(cmd, bp, P.pl, 0, (u32)b->n, b->w);
}

void post_push_constants(VkCommandBuffer cmd, const void *data, u32 size) {
    vkCmdPushConstants(cmd, P.pl, VK_SHADER_STAGE_ALL, 0, size, data);
}

void post_dispatch(VkCommandBuffer cmd, const char *pipe, u32 w, u32 h, u32 d, u32 group) {
    VkPipeline p = post_compute_pipe(pipe);
    if (!p) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    vkCmdDispatch(cmd, (w + group - 1) / group, (h + group - 1) / group, DG_MAX(1u, d));
}

void post_fullscreen(VkCommandBuffer cmd, VkPipeline pipe, VkImageView color, u32 w, u32 h, bool load) {
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    ca.imageView = color;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea.extent.width = w;
    ri.renderArea.extent.height = h;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cmd, &ri);
    VkViewport vp = { 0, 0, (float)w, (float)h, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { w, h } };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

// ------------------------------------------------------------------ pipelines

static PostPipe *find_pipe(const char *name, bool compute, VkFormat color, int blend) {
    for (int i = 0; i < P.pipe_count; i++) {
        PostPipe *p = &P.pipes[i];
        if (p->compute == compute && p->color == color && p->blend == blend && !strcmp(p->name, name)) return p;
    }
    if (P.pipe_count >= POST_MAX_PIPES) return NULL;
    PostPipe *p = &P.pipes[P.pipe_count++];
    memset(p, 0, sizeof(*p));
    snprintf(p->name, sizeof(p->name), "%s", name);
    p->compute = compute;
    p->color = color;
    p->blend = blend;
    return p;
}

static char g_shader_names[POST_MAX_PIPES][64];

static VkShaderModule load_post_shader(const char *file) {
    // Register for hot reload / recompile (stable storage for the name).
    for (int i = 0; i < POST_MAX_PIPES; i++) {
        if (!g_shader_names[i][0]) {
            snprintf(g_shader_names[i], sizeof(g_shader_names[i]), "%s", file);
            vkp_register_shader(g_shader_names[i]);
            break;
        }
        if (!strcmp(g_shader_names[i], file)) break;
    }
    return vkp_load_shader(file);
}

VkPipeline post_compute_pipe(const char *name) {
    PostPipe *p = find_pipe(name, true, VK_FORMAT_UNDEFINED, 0);
    if (!p) return VK_NULL_HANDLE;
    if (p->pipe) return p->pipe;
    char file[80];
    if (strchr(name, '/')) snprintf(file, sizeof(file), "%s.comp", name);   // other shader folders (sky/...)
    else snprintf(file, sizeof(file), "post/%s.comp", name);
    VkShaderModule m = load_post_shader(file);
    if (!m) return VK_NULL_HANDLE;
    VkComputePipelineCreateInfo ci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = m;
    ci.stage.pName = "main";
    ci.layout = P.pl;
    VkResult r = vkCreateComputePipelines(R->device, VK_NULL_HANDLE, 1, &ci, NULL, &p->pipe);
    vkDestroyShaderModule(R->device, m, NULL);
    if (r != VK_SUCCESS) { log_error("post: compute pipeline %s failed (%d)", name, (int)r); p->pipe = VK_NULL_HANDLE; }
    return p->pipe;
}

VkPipeline post_graphics_pipe(const char *frag, VkFormat color, int blend) {
    PostPipe *p = find_pipe(frag, false, color, blend);
    if (!p) return VK_NULL_HANDLE;
    if (p->pipe) return p->pipe;
    char file[80];
    if (strchr(frag, '/')) snprintf(file, sizeof(file), "%s.frag", frag);   // other shader folders (sights/...)
    else snprintf(file, sizeof(file), "post/%s.frag", frag);
    VkShaderModule vs = vkp_load_shader("fullscreen.vert"), fs = load_post_shader(file);
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
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState cba = { 0 };
    cba.colorWriteMask = 0xF;
    if (blend == 3) {
        // Multiply (glass tint): dst * src.rgb, alpha kept.
        cba.blendEnable = VK_TRUE;
        cba.colorBlendOp = cba.alphaBlendOp = VK_BLEND_OP_ADD;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    } else if (blend) {
        cba.blendEnable = VK_TRUE;
        cba.colorBlendOp = cba.alphaBlendOp = VK_BLEND_OP_ADD;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstColorBlendFactor = blend == 1 ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstAlphaBlendFactor = blend == 1 ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    }
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo rci = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    rci.colorAttachmentCount = 1;
    rci.pColorAttachmentFormats = &color;
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
    gp.layout = P.pl;
    VkResult r = vkCreateGraphicsPipelines(R->device, VK_NULL_HANDLE, 1, &gp, NULL, &p->pipe);
    vkDestroyShaderModule(R->device, vs, NULL);
    vkDestroyShaderModule(R->device, fs, NULL);
    if (r != VK_SUCCESS) { log_error("post: pipeline %s failed (%d)", frag, (int)r); p->pipe = VK_NULL_HANDLE; }
    return p->pipe;
}

// Hot reload: drop every pipeline; they are rebuilt lazily from the new SPIR-V.
bool post_create_pipelines(void) {
    if (!P.layout) return true;
    for (int i = 0; i < P.pipe_count; i++) {
        if (P.pipes[i].pipe) vkDestroyPipeline(R->device, P.pipes[i].pipe, NULL);
        P.pipes[i].pipe = VK_NULL_HANDLE;
    }
    return true;
}

// ------------------------------------------------------------------ init

bool post_init(void) {
    memset(&P, 0, sizeof(P));
    if (!R->has_push_descriptor) { log_error("post: push descriptors unavailable, post stack disabled"); return false; }
    // 0-7 and 15 samplers, 8-11 storage images, 12 UBO, 13-14 storage buffers.
    VkDescriptorSetLayoutBinding b[16];
    memset(b, 0, sizeof(b));
    for (u32 i = 0; i < 16; i++) {
        b[i].binding = i;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_ALL;
        b[i].descriptorType = i < 8 || i == 15 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                    : (i < 12 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : (i == 12 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER));
    }
    VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = 16;
    li.pBindings = b;
    VK_CHECK(vkCreateDescriptorSetLayout(R->device, &li, NULL, &P.layout));
    VkPushConstantRange pr = { VK_SHADER_STAGE_ALL, 0, 128 };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &P.layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pr;
    VK_CHECK(vkCreatePipelineLayout(R->device, &pli, NULL, &P.pl));
    VkSamplerCreateInfo s = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    s.magFilter = s.minFilter = VK_FILTER_LINEAR;
    s.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    s.addressModeU = s.addressModeV = s.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    s.maxLod = VK_LOD_CLAMP_NONE;
    VK_CHECK(vkCreateSampler(R->device, &s, NULL, &P.linear));
    s.addressModeU = s.addressModeV = s.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VK_CHECK(vkCreateSampler(R->device, &s, NULL, &P.linear_repeat));
    s.magFilter = s.minFilter = VK_FILTER_NEAREST;
    s.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    s.addressModeU = s.addressModeV = s.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(R->device, &s, NULL, &P.point));
    s.magFilter = s.minFilter = VK_FILTER_LINEAR;
    s.compareEnable = VK_TRUE;
    s.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    s.addressModeU = s.addressModeV = s.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    s.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    VK_CHECK(vkCreateSampler(R->device, &s, NULL, &P.shadow_cmp));
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) {
        P.ubo[i] = vkr_buffer_create(sizeof(PostUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
        P.exposure_rb[i] = vkr_buffer_create_readback(64);
    }
    P.exposure_buf = vkr_buffer_create(64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
    P.histogram_buf = vkr_buffer_create(256 * 4 + 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
    post_textures_init();
    post_settings_default(&P.set);
    P.pending = P.set;
    P.ok = true;
    log_info("post: stack ready (push descriptors, frame graph)");
    return true;
}

void post_shutdown(void) {
    if (!P.layout) return;
    vkDeviceWaitIdle(R->device);
    post_create_pipelines();
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) {
        vkr_buffer_destroy_now(&P.ubo[i]);
        vkr_buffer_destroy_now(&P.exposure_rb[i]);
    }
    vkr_buffer_destroy_now(&P.exposure_buf);
    vkr_buffer_destroy_now(&P.histogram_buf);
    post_sights_shutdown();
    post_weather_shutdown();
    post_textures_shutdown();
    vkDestroySampler(R->device, P.linear, NULL);
    vkDestroySampler(R->device, P.linear_repeat, NULL);
    vkDestroySampler(R->device, P.point, NULL);
    vkDestroySampler(R->device, P.shadow_cmp, NULL);
    vkDestroyPipelineLayout(R->device, P.pl, NULL);
    vkDestroyDescriptorSetLayout(R->device, P.layout, NULL);
    memset(&P, 0, sizeof(P));
}

void post_targets_changed(void) { P.exposure_init = false; }

// ------------------------------------------------------------------ per frame

static const char *k_debug_names[] = { "off", "ao", "contact", "ssr", "ssgi", "velocity", "normal", "material", "depth", "hiz", "coc",
                                       "luminance", "taa_weight", "froxel", "bloom", "flare", "mb_tiles", "fog" };
const char *post_debug_name(int i) { return i >= 0 && i < (int)ARRAY_COUNT(k_debug_names) ? k_debug_names[i] : "off"; }
int post_debug_count(void) { return (int)ARRAY_COUNT(k_debug_names); }

float post_exposure_ev(void) { return P.last_ev; }

void post_begin_frame(const PostFrameIn *in) {
    P.in = *in;
    P.set = P.pending;
    P.hiz = P.ao = P.contact = P.ssr = P.ssgi = P.froxels = P.color_prev = P.depth_prev = P.vol_history = FG_NONE;
    // Host readback of the exposure written two frames ago (UI / tests only).
    float *rb = (float *)P.exposure_rb[R->frame_slot].mapped;
    if (rb && P.exposure_init) P.last_ev = rb[1];
    const PostSettings *s = &P.set;
    P.direct_exposure = direct_exposure(in, s);
    PostUBO *u = (PostUBO *)P.ubo[R->frame_slot].mapped;
    u->view = in->view;
    u->proj = in->proj;
    u->view_proj = in->view_proj;
    u->inv_view_proj = in->inv_view_proj;
    u->inv_proj = in->inv_proj;
    u->view_proj_nj = in->view_proj_nj;
    u->prev_view_proj_nj = in->prev_view_proj_nj;
    u->inv_view_proj_nj = mat4_inverse(in->view_proj_nj);
    u->shadow_vp = R->shadow_vp;
    u->cam_pos = v4(in->cam_pos.x, in->cam_pos.y, in->cam_pos.z, in->time);
    u->screen = v4((float)in->render_w, (float)in->render_h, 1.0f / (float)in->render_w, 1.0f / (float)in->render_h);
    u->output = v4((float)in->output_w, (float)in->output_h, 1.0f / (float)in->output_w, 1.0f / (float)in->output_h);
    u->jitter = v4(in->jitter.x, in->jitter.y, in->prev_jitter.x, in->prev_jitter.y);
    u->depth = v4(in->znear, in->zfar, tanf(in->fov_y * 0.5f), (float)in->frame);
    u->sun_dir = v4(in->sun_dir.x, in->sun_dir.y, in->sun_dir.z, in->sun_intensity);
    u->sun_color = v4(in->sun_color.x, in->sun_color.y, in->sun_color.z, R->r_shadow_quality->i > 0 ? 1.0f : 0.0f);
    u->sky = v4(in->sky.x, in->sky.y, in->sky.z, in->ambient);
    u->ground = v4(in->ground.x, in->ground.y, in->ground.z, (float)R->light_count);
    u->misc = v4(in->dt, in->camera_cut ? 1.0f : 0.0f, in->ads ? 1.0f : 0.0f, (float)in->output_w / (float)DG_MAX(1u, in->output_h));
    u->s_ao = v4(s->ao_radius, s->ao_intensity, (float)s->ao, s->ao >= 2 ? 12.0f : 6.0f);
    u->s_ssr = v4(s->ssr_intensity, s->ssr_max_roughness, (float)s->ssr, 0.15f);
    u->s_gi = v4(s->ssgi_intensity, (float)s->ssgi, 0.35f, s->contact_shadows >= 2 ? 14.0f : 8.0f);   // contact steps by quality
    u->s_vol = v4(s->fog_density, s->fog_height, s->fog_falloff, s->fog_anisotropy);
    u->s_vol2 = v4(s->fog_albedo.x, s->fog_albedo.y, s->fog_albedo.z, s->sun_shafts);
    u->s_mb = v4(s->mb_intensity, (float)s->motion_blur, 40.0f, 16.0f);
    u->s_dof = v4(s->dof_focus, s->dof_fstop, s->dof_focal_mm, s->dof_max_coc);
    u->s_dof2 = v4((float)s->dof_autofocus, s->dof_quality >= 2 ? (float)s->dof_bokeh : 0.0f, (float)s->dof, 24.0f);
    u->s_bloom = v4(s->bloom_intensity, s->bloom_dirt, 6.0f, (float)s->bloom);
    u->s_exp = v4(s->exposure_comp, s->exposure_manual, s->adapt_up, s->adapt_down);
    u->s_exp2 = v4(s->ev_min, s->ev_max, s->hist_low, s->hist_high);
    u->s_lens = v4(s->flare, s->anamorphic, s->chromatic, s->vignette);
    u->s_lens2 = v4(s->distortion, s->grain, s->sharpen, s->exposure_auto ? 1.0f : 0.0f);
    u->s_tm = v4((float)s->tonemapper, s->wb_temp, s->wb_tint, s->saturation);
    u->s_lift = v4(s->lift.x, s->lift.y, s->lift.z, s->contrast);
    u->s_gamma = v4(s->gamma.x, s->gamma.y, s->gamma.z, s->lut_weight);
    u->s_gain = v4(s->gain.x, s->gain.y, s->gain.z, (float)s->colorblind);
    u->s_out = v4(R->swap_hdr ? 1.0f : 0.0f, s->paper_white, s->max_nits, s->brightness);
    u->s_fb = v4(s->fb_damage, s->fb_lowhealth, s->fb_flash, s->fb_hitmarker);
    u->s_fb2 = v4(s->fb_sprint, s->fb_underwater, s->fb_nightvision, s->fb_desaturate);
    u->s_fb_tint = v4(s->fb_tint.x, s->fb_tint.y, s->fb_tint.z, s->user_gamma);
    u->s_fb3 = v4(saturate(s->fb_black), saturate(s->fb_blur), 0.0f, 0.0f);
    u->s_dbg = v4((float)s->debug_view, s->compare_split ? 1.0f : 0.0f, 1.0f, s->fog_ambient);   // w: fog ambient in-scatter (weather)
    for (u32 i = 0; i < R->light_count && i < MAX_POINT_LIGHTS; i++) {
        u->lights[i * 2] = R->lights[i].pos_radius;
        u->lights[i * 2 + 1] = R->lights[i].color_intensity;
    }
}

// Compute / transfer -> compute + fragment memory dependency for the storage buffers (the frame
// graph only tracks images).
void post_buffer_barrier(VkCommandBuffer cmd) {
    VkMemoryBarrier2 mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &dep);
}

// One mip of a GENERAL-layout chain as a sampled texture (bloom: bilinear taps between mips).
void pb_tex_mip(PostBind *b, u32 binding, FgRes r, u32 mip, VkSampler s) {
    if (b->n >= 16) return;
    VkImageView v = fg_valid(r) ? fg_mip_view(r, mip) : VK_NULL_HANDLE;
    b->img[b->n] = (VkDescriptorImageInfo){ s ? s : P.linear, v ? v : P.white, v ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    pb_add(b, binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
}

// Set 2 of the lit pipelines: AO / contact (white = none), SSR / SSGI (black = none).
void post_push_lit_inputs(VkCommandBuffer cmd, const PostLitInputs *in, bool froxel_fog) {
    VkDescriptorImageInfo img[5];
    VkWriteDescriptorSet w[5];
    memset(w, 0, sizeof(w));
    FgRes res[4] = { FG_NONE, FG_NONE, FG_NONE, FG_NONE };
    if (in && P.ok) {
        res[0] = in->ao_on ? in->ao : FG_NONE;
        res[1] = in->contact_on ? in->contact : FG_NONE;
        res[2] = in->ssr_on ? in->ssr : FG_NONE;
        res[3] = in->ssgi_on ? in->ssgi : FG_NONE;
    }
    for (int i = 0; i < 4; i++) {
        bool ok = res[i] >= 0 && fg_valid(res[i]);
        VkImageView fallback = i < 2 ? P.white : P.black;
        img[i] = (VkDescriptorImageInfo){ P.linear, ok ? fg_view(res[i]) : fallback, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstBinding = (u32)i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].pImageInfo = &img[i];
    }
    // Binding 4: the froxel volume (transparent pass) or any 3D texture.
    bool fog = froxel_fog && P.ok && fg_valid(P.froxels);
    img[4] = (VkDescriptorImageInfo){ P.linear, fog ? fg_view(P.froxels) : P.lut, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    w[4] = w[0];
    w[4].dstBinding = 4;
    w[4].pImageInfo = &img[4];
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->layout_lit, 2, 5, w);
}
