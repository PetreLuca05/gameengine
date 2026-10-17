#version 450
// Classic lit pass of masked / dithered materials (depth EQUAL): the alpha test and fade dither run
// again, since distant cards (of one tree or of neighbours) can share a depth value with the
// surface the prepass kept.
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#define PBR_MAPS   // glTF material maps (set 5)
#include "classic_common.glsl"
#include "classic_mask.glsl"
layout(location = 9) flat in float v_fade;
layout(location = 10) in vec4 v_cover;
layout(location = 11) flat in vec3 v_cover_pivot;
// Ground cover (shaders/cover.glsl): the nta map rides in the material set's first slot (vk_classic.c mat_maps).
#define LIT_COVER
#define LIT_COVER_IN v_cover
#define LIT_COVER_PIVOT v_cover_pivot
#define COVER_TEX(uv) texture(cl_pbr_tex[0], (uv))
#define MAT_COLOR CL_MAT_COLOR
#define MAT_PARAMS pc.params
#define MAT_FLAGS CL_MAT_FLAGS
#define LIT_PRE_MAIN if (cl_faded(v_fade, gl_FragCoord.xy) || (pc.model[3].y > 0.0 && cl_mask_alpha(tex, v_uv * max(pc.params.w, 1e-3)) < pc.model[3].y)) discard;
#include "lit_body.glsl"
