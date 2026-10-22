#version 450
// The G-buffer of a deferred masked (alpha test) and / or distance-dithered material at EQUAL depth after
// its depth-only prepass (PF-0052, classic_depth_clip.frag): classic_gbuffer_def_clip.frag without the test.
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#define PBR_MAPS   // glTF material maps (set 5)
#include "classic_common.glsl"
layout(set = 1, binding = 0) uniform sampler2D tex;
layout(location = 9) flat in vec4 v_fade;
layout(location = 10) in vec4 v_cover;
layout(location = 11) flat in vec3 v_cover_pivot;
layout(location = 3) out vec4 o_cl_albedo;
layout(location = 4) out uvec4 o_cl_params;
#define GB_FLAGS pc.flags.x
#define GB_PARAMS pc.params
#define GB_DEFERRED
// The lit pass's surface stage, as classic_lit_clip.frag defines it.
#define MAT_COLOR CL_MAT_COLOR
#define MAT_PARAMS pc.params
#define MAT_FLAGS CL_MAT_FLAGS
#define MAT_AO_BYTE (PBR_PARAMS.z >> 24u)
#define LIT_FRONT_FACING gl_FrontFacing
#define LIT_TEX(uv) texture(tex, (uv))
#define LIT_TEX_W(uv, dx, dy) LIT_TEX(uv)
#define LIT_BASE_HOOK base.rgb = cl_lod_debug(base.rgb, v_fade);
#define LIT_COVER_IN v_cover
#define LIT_COVER_PIVOT v_cover_pivot
#define COVER_TEX(uv) texture(cl_pbr_tex[0], (uv))
#define LIT_COVER
#include "classic_deferred.glsl"
#include "lit_surface_fn.glsl"
#include "cover.glsl"
#include "gbuffer_body.glsl"
