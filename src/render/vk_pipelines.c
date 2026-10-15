// Descriptor layouts and graphics pipelines (dynamic rendering, no render passes).
#include "vk_internal.h"
#include "core/platform.h"
#include <stdio.h>
#include <stdlib.h>

typedef enum VertexLayout { VL_NONE, VL_MESH, VL_MESH_POS_ONLY, VL_LINE, VL_TEXT, VL_UI, VL_SKIN, VL_SKIN_SHADOW } VertexLayout;

typedef struct PipeDesc {
    const char *vert, *frag;
    VertexLayout vl;
    VkPrimitiveTopology topology;
    VkPolygonMode polygon;
    VkCullModeFlags cull;
    bool depth_test, depth_write;
    bool blend;
    VkFormat color_format;  // VK_FORMAT_UNDEFINED = depth only
    VkFormat depth_format;
    bool depth_bias;
    VkPipelineLayout layout;
} PipeDesc;

bool vkp_create_layouts(void) {
    VkDescriptorSetLayoutBinding fb[4] = { 0 };
    fb[0].binding = 0;
    fb[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    fb[0].descriptorCount = 1;
    fb[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    fb[1].binding = 1;
    fb[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    fb[1].descriptorCount = 1;
    fb[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    for (int i = 2; i < 4; i++) {
        fb[i].binding = (u32)i;
        fb[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        fb[i].descriptorCount = 1;
        fb[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = 4;
    li.pBindings = fb;
    VK_CHECK(vkCreateDescriptorSetLayout(R->device, &li, NULL, &R->frame_layout));

    VkDescriptorSetLayoutBinding tb = { 0 };
    tb.binding = 0;
    tb.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    tb.descriptorCount = 1;
    tb.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    li.bindingCount = 1;
    li.pBindings = &tb;
    VK_CHECK(vkCreateDescriptorSetLayout(R->device, &li, NULL, &R->tex_layout));

    VkDescriptorPoolSize sizes[3] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 16 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_TEXTURES + 64 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16 },
    };
    VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pi.maxSets = MAX_TEXTURES + 64;
    pi.poolSizeCount = 3;
    pi.pPoolSizes = sizes;
    VK_CHECK(vkCreateDescriptorPool(R->device, &pi, NULL, &R->pool));

    VkDescriptorSetLayout sets[2] = { R->frame_layout, R->tex_layout };
    VkPushConstantRange pr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushLit) };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 2;
    pli.pSetLayouts = sets;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pr;
    VK_CHECK(vkCreatePipelineLayout(R->device, &pli, NULL, &R->layout_lit));

    VkPushConstantRange pp = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32 };
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &R->tex_layout;
    pli.pPushConstantRanges = &pp;
    VK_CHECK(vkCreatePipelineLayout(R->device, &pli, NULL, &R->layout_post));

    VkPushConstantRange pu = { VK_SHADER_STAGE_VERTEX_BIT, 0, 16 };
    pli.pPushConstantRanges = &pu;
    VK_CHECK(vkCreatePipelineLayout(R->device, &pli, NULL, &R->layout_ui));
    return true;
}

void vkp_destroy_layouts(void) {
    vkDestroyPipelineLayout(R->device, R->layout_lit, NULL);
    vkDestroyPipelineLayout(R->device, R->layout_post, NULL);
    vkDestroyPipelineLayout(R->device, R->layout_ui, NULL);
    vkDestroyDescriptorPool(R->device, R->pool, NULL);
    vkDestroyDescriptorSetLayout(R->device, R->frame_layout, NULL);
    vkDestroyDescriptorSetLayout(R->device, R->tex_layout, NULL);
}

static VkShaderModule load_shader(const char *name) {
    char path[512];
    snprintf(path, sizeof(path), "%sshaders/%s.spv", platform_base_dir(), name);
    size_t size = 0;
    char *code = platform_read_file(path, &size);
    if (!code || size % 4 != 0) {
        log_error("shader: cannot load %s", path);
        free(code);
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = size;
    ci.pCode = (const u32 *)code;
    VkShaderModule mod = VK_NULL_HANDLE;
    VkResult r = vkCreateShaderModule(R->device, &ci, NULL, &mod);
    free(code);
    if (r != VK_SUCCESS) { log_error("shader: vkCreateShaderModule failed for %s", name); return VK_NULL_HANDLE; }
    return mod;
}

static VkPipeline build_pipeline(const PipeDesc *d) {
    VkShaderModule vs = load_shader(d->vert);
    VkShaderModule fs = d->frag ? load_shader(d->frag) : VK_NULL_HANDLE;
    if (!vs || (d->frag && !fs)) {
        if (vs) vkDestroyShaderModule(R->device, vs, NULL);
        if (fs) vkDestroyShaderModule(R->device, fs, NULL);
        return VK_NULL_HANDLE;
    }
    VkPipelineShaderStageCreateInfo stages[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
                                                  { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    VkVertexInputBindingDescription bind = { 0, 0, VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attrs[6];
    u32 attr_count = 0;
    switch (d->vl) {
    case VL_MESH:
        bind.stride = sizeof(Vertex);
        attrs[0] = (VkVertexInputAttributeDescription){ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, pos) };
        attrs[1] = (VkVertexInputAttributeDescription){ 1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal) };
        attrs[2] = (VkVertexInputAttributeDescription){ 2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv) };
        attrs[3] = (VkVertexInputAttributeDescription){ 3, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(Vertex, color) };
        attr_count = 4;
        break;
    case VL_MESH_POS_ONLY:
        bind.stride = sizeof(Vertex);
        attrs[0] = (VkVertexInputAttributeDescription){ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, pos) };
        attr_count = 1;
        break;
    case VL_LINE:
        bind.stride = sizeof(LineVertex);
        attrs[0] = (VkVertexInputAttributeDescription){ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(LineVertex, pos) };
        attrs[1] = (VkVertexInputAttributeDescription){ 1, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(LineVertex, color) };
        attr_count = 2;
        break;
    case VL_TEXT:
        bind.stride = sizeof(TextVertex);
        attrs[0] = (VkVertexInputAttributeDescription){ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(TextVertex, pos) };
        attrs[1] = (VkVertexInputAttributeDescription){ 1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(TextVertex, uv) };
        attrs[2] = (VkVertexInputAttributeDescription){ 2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(TextVertex, color) };
        attr_count = 3;
        break;
    case VL_UI:
        bind.stride = sizeof(UiVertex);
        attrs[0] = (VkVertexInputAttributeDescription){ 0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(UiVertex, pos) };
        attrs[1] = (VkVertexInputAttributeDescription){ 1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(UiVertex, uv) };
        attrs[2] = (VkVertexInputAttributeDescription){ 2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(UiVertex, color) };
        attr_count = 3;
        break;
    case VL_SKIN:
        bind.stride = sizeof(SkinVertex);
        attrs[0] = (VkVertexInputAttributeDescription){ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SkinVertex, pos) };
        attrs[1] = (VkVertexInputAttributeDescription){ 1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SkinVertex, normal) };
        attrs[2] = (VkVertexInputAttributeDescription){ 2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(SkinVertex, uv) };
        attrs[3] = (VkVertexInputAttributeDescription){ 3, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(SkinVertex, color) };
        attrs[4] = (VkVertexInputAttributeDescription){ 4, 0, VK_FORMAT_R8G8B8A8_UINT, offsetof(SkinVertex, joints) };
        attrs[5] = (VkVertexInputAttributeDescription){ 5, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(SkinVertex, weights) };
        attr_count = 6;
        break;
    case VL_SKIN_SHADOW:
        bind.stride = sizeof(SkinVertex);
        attrs[0] = (VkVertexInputAttributeDescription){ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SkinVertex, pos) };
        attrs[1] = (VkVertexInputAttributeDescription){ 4, 0, VK_FORMAT_R8G8B8A8_UINT, offsetof(SkinVertex, joints) };
        attrs[2] = (VkVertexInputAttributeDescription){ 5, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(SkinVertex, weights) };
        attr_count = 3;
        break;
    case VL_NONE: break;
    }
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    if (d->vl != VL_NONE) {
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &bind;
        vi.vertexAttributeDescriptionCount = attr_count;
        vi.pVertexAttributeDescriptions = attrs;
    }
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = d->topology;
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = d->polygon;
    rs.cullMode = d->cull;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    rs.depthBiasEnable = d->depth_bias ? VK_TRUE : VK_FALSE;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable = d->depth_test;
    ds.depthWriteEnable = d->depth_write;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState cba = { 0 };
    cba.colorWriteMask = 0xF;
    if (d->blend) {
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = d->color_format != VK_FORMAT_UNDEFINED ? 1 : 0;
    cb.pAttachments = &cba;
    VkDynamicState dyn[3] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = d->depth_bias ? 3 : 2;
    dy.pDynamicStates = dyn;

    VkPipelineRenderingCreateInfo rci = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    rci.colorAttachmentCount = d->color_format != VK_FORMAT_UNDEFINED ? 1 : 0;
    rci.pColorAttachmentFormats = &d->color_format;
    rci.depthAttachmentFormat = d->depth_format;

    VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gp.pNext = &rci;
    gp.stageCount = d->frag ? 2 : 1;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dy;
    gp.layout = d->layout;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkResult res = vkCreateGraphicsPipelines(R->device, VK_NULL_HANDLE, 1, &gp, NULL, &pipe);
    vkDestroyShaderModule(R->device, vs, NULL);
    if (fs) vkDestroyShaderModule(R->device, fs, NULL);
    if (res != VK_SUCCESS) { log_error("pipeline creation failed (%s/%s): %d", d->vert, d->frag, (int)res); return VK_NULL_HANDLE; }
    return pipe;
}

bool vkp_create_pipelines(void) {
    VkFormat hdr = VK_FORMAT_R16G16B16A16_SFLOAT, depth = VK_FORMAT_D32_SFLOAT;
    PipeDesc d[PIPE_COUNT];
    memset(d, 0, sizeof(d));
    d[PIPE_LIT] = (PipeDesc){ "lit.vert", "lit.frag", VL_MESH, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_FILL,
                              VK_CULL_MODE_BACK_BIT, true, true, false, hdr, depth, false, R->layout_lit };
    d[PIPE_LIT_WIRE] = d[PIPE_LIT];
    d[PIPE_LIT_WIRE].polygon = R->has_wireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    d[PIPE_LIT_WIRE].cull = VK_CULL_MODE_NONE;
    d[PIPE_LIT_BLEND] = d[PIPE_LIT];
    d[PIPE_LIT_BLEND].blend = true;
    d[PIPE_LIT_BLEND].depth_write = false;
    d[PIPE_LIT_BLEND].cull = VK_CULL_MODE_NONE;
    d[PIPE_SHADOW] = (PipeDesc){ "shadow.vert", NULL, VL_MESH_POS_ONLY, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_FILL,
                                 VK_CULL_MODE_NONE, true, true, false, VK_FORMAT_UNDEFINED, depth, true, R->layout_lit };
    d[PIPE_LINES] = (PipeDesc){ "lines.vert", "lines.frag", VL_LINE, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_POLYGON_MODE_FILL,
                                VK_CULL_MODE_NONE, true, false, true, hdr, depth, false, R->layout_lit };
    d[PIPE_LINES_OVERLAY] = d[PIPE_LINES];
    d[PIPE_LINES_OVERLAY].depth_test = false;
    d[PIPE_TEXT] = (PipeDesc){ "text.vert", "text.frag", VL_TEXT, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_FILL,
                               VK_CULL_MODE_NONE, true, false, true, hdr, depth, false, R->layout_lit };
    d[PIPE_POST] = (PipeDesc){ "fullscreen.vert", "post.frag", VL_NONE, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_FILL,
                               VK_CULL_MODE_NONE, false, false, false, R->swap_format, VK_FORMAT_UNDEFINED, false, R->layout_post };
    d[PIPE_UI] = (PipeDesc){ "ui.vert", "ui.frag", VL_UI, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_FILL,
                             VK_CULL_MODE_NONE, false, false, true, R->swap_format, VK_FORMAT_UNDEFINED, false, R->layout_ui };

    d[PIPE_SKIN] = d[PIPE_LIT];
    d[PIPE_SKIN].vert = "skin.vert";
    d[PIPE_SKIN].frag = "skin.frag";
    d[PIPE_SKIN].vl = VL_SKIN;
    d[PIPE_SKIN_WIRE] = d[PIPE_SKIN];
    d[PIPE_SKIN_WIRE].polygon = R->has_wireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    d[PIPE_SKIN_WIRE].cull = VK_CULL_MODE_NONE;
    d[PIPE_SKIN_SHADOW] = d[PIPE_SHADOW];
    d[PIPE_SKIN_SHADOW].vert = "skin_shadow.vert";
    d[PIPE_SKIN_SHADOW].vl = VL_SKIN_SHADOW;

    VkPipeline built[PIPE_COUNT];
    bool ok = true;
    for (int i = 0; i < PIPE_COUNT; i++) {
        built[i] = build_pipeline(&d[i]);
        if (!built[i]) ok = false;
    }
    if (!ok) {
        for (int i = 0; i < PIPE_COUNT; i++) if (built[i]) vkDestroyPipeline(R->device, built[i], NULL);
        return false;
    }
    vkp_destroy_pipelines();
    memcpy(R->pipes, built, sizeof(built));
    return true;
}

void vkp_destroy_pipelines(void) {
    for (int i = 0; i < PIPE_COUNT; i++) {
        if (R->pipes[i]) vkDestroyPipeline(R->device, R->pipes[i], NULL);
        R->pipes[i] = VK_NULL_HANDLE;
    }
}

// ------------------------------------------------------------------ hot reload

static const char *k_shader_files[] = { "lit.vert", "lit.frag", "shadow.vert", "lines.vert", "lines.frag", "text.vert",
                                        "text.frag", "fullscreen.vert", "post.frag", "ui.vert", "ui.frag",
                                        "skin.vert", "skin.frag", "skin_shadow.vert" };

static i64 shader_sources_mtime(void) {
#ifdef DG_SOURCE_DIR
    i64 newest = 0;
    char path[512];
    for (int i = 0; i < ARRAY_COUNT(k_shader_files); i++) {
        snprintf(path, sizeof(path), "%s/shaders/%s", DG_SOURCE_DIR, k_shader_files[i]);
        i64 t = platform_file_mtime(path);
        if (t > newest) newest = t;
    }
    snprintf(path, sizeof(path), "%s/shaders/common.glsl", DG_SOURCE_DIR);
    i64 t = platform_file_mtime(path);
    return t > newest ? t : newest;
#else
    return 0;
#endif
}

static bool compile_shaders_from_source(void) {
#if defined(DG_SOURCE_DIR) && defined(DG_SHADER_COMPILER)
    for (int i = 0; i < ARRAY_COUNT(k_shader_files); i++) {
        char cmd[2048];
        const char *src_dir = DG_SOURCE_DIR "/shaders";
        if (strcmp(DG_SHADER_COMPILER_KIND, "glslc") == 0)
            snprintf(cmd, sizeof(cmd), "\"\"%s\" --target-env=vulkan1.3 -O -I \"%s\" -o \"%sshaders/%s.spv\" \"%s/%s\"\"",
                     DG_SHADER_COMPILER, src_dir, platform_base_dir(), k_shader_files[i], src_dir, k_shader_files[i]);
        else
            snprintf(cmd, sizeof(cmd), "\"\"%s\" -V --quiet --target-env vulkan1.3 -I\"%s\" -o \"%sshaders/%s.spv\" \"%s/%s\"\"",
                     DG_SHADER_COMPILER, src_dir, platform_base_dir(), k_shader_files[i], src_dir, k_shader_files[i]);
#ifndef _WIN32
        // POSIX shells don't need the outer quote pair cmd.exe wants.
        size_t n = strlen(cmd);
        memmove(cmd, cmd + 1, n - 2);
        cmd[n - 2] = 0;
#endif
        int rc = system(cmd);
        if (rc != 0) { log_error("shader compile failed: %s", k_shader_files[i]); return false; }
    }
    return true;
#else
    return false;
#endif
}

bool render_reload_shaders(void) {
    if (!compile_shaders_from_source()) log_warn("shaders: recompile failed or unavailable, reloading existing SPIR-V");
    vkDeviceWaitIdle(R->device);
    bool ok = vkp_create_pipelines();
    log_info(ok ? "shaders: reloaded" : "shaders: reload failed, keeping old pipelines");
    return ok;
}

void render_poll_shader_hot_reload(void) {
    if (!R->r_shader_hotreload->i) return;
    if (g_engine.real_time < R->next_shader_poll) return;
    R->next_shader_poll = g_engine.real_time + 1.0;
    i64 t = shader_sources_mtime();
    if (!R->shader_src_mtime) { R->shader_src_mtime = t; return; }
    if (t > R->shader_src_mtime) {
        R->shader_src_mtime = t;
        log_info("shaders: source change detected, hot reloading");
        render_reload_shaders();
    }
}
