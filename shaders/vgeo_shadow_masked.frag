#version 450
// Alpha test of masked vgeo clusters in the sun shadow cascades (vgeo_shadow_masked.vert): the
// material's mask texture from the vgeo texture table (binding 23 of set 3).
#extension GL_GOOGLE_include_directive : require
#define VG_SET 3
#define VG_NO_PUSH
#define VG_VIEW_INDEX 0
#include "vgeo/vgeo_common.glsl"
layout(location = 0) in vec2 i_uv;
layout(location = 1) flat in uint i_tex;
layout(location = 2) flat in float i_cutoff;
void main() {
    if (texture(vg_tex[nonuniformEXT(i_tex)], i_uv).a < i_cutoff) discard;
}
