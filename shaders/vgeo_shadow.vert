#version 450
// vgeo clusters in the sun shadow cascades, opaque (depth only, vertex pulling): the cascade view's HW
// list (vgeo/vg_shadow.glsl).
#extension GL_GOOGLE_include_directive : require
#define VG_SET 3
#define VG_NO_PUSH
#define VG_VIEW_INDEX 0
#include "vgeo/vgeo_common.glsl"
#include "common.glsl"
#include "vgeo/vg_decode.glsl"
#include "vgeo/vg_shadow.glsl"
void main() {
    vec2 uv;
    uint tex;
    float cutoff;
    gl_Position = vg_shadow_vertex(uv, tex, cutoff);
}
