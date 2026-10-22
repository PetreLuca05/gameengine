// GPU layout guard (src/render/vk_pipelines.c render_reload_shaders): the byte sizes of the structs C and
// GLSL share. vk_internal.h asserts them at build time; a shader hot reload refuses a source tree whose
// values differ from the running exe's (its shaders would read records the exe does not write: every
// skinned character garbled until the next build). Change a struct in C and GLSL together, then here.
#ifndef DG_SHADER_LAYOUT_H
#define DG_SHADER_LAYOUT_H

#define DG_LAYOUT_SKIN_INSTANCE 160   // GpuSkinInstance / common.glsl SkinInstance
#define DG_LAYOUT_FRAME_DATA 12400    // FrameData / common.glsl FrameData (set 0 binding 0)

#endif
