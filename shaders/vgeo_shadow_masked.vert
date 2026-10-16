#version 450
// vgeo clusters in the sun shadow cascades, masked (alpha-tested in vgeo_shadow_masked.frag): the
// cascade view's masked clusters (its SW half, vgeo/vg_shadow.glsl).
#extension GL_GOOGLE_include_directive : require
#define VG_SET 3
#define VG_NO_PUSH
#define VG_VIEW_INDEX 0
#include "vgeo/vgeo_common.glsl"
#include "common.glsl"
#include "vgeo/vg_decode.glsl"
#include "vgeo/vg_shadow.glsl"
layout(location = 0) out vec2 o_uv;
layout(location = 1) flat out uint o_tex;
layout(location = 2) flat out float o_cutoff;
void main() { gl_Position = vg_shadow_vertex(o_uv, o_tex, o_cutoff); }
