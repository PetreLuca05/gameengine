#version 450
// Fading skinned shadow casters (PIPE_SKIN_SHADOW_FADE, docs/LOD.md): the LOD cross-fade and the
// shadow distance fade, dithered into the shadow map with the same complementary masks as the main
// view (the outgoing and incoming LOD's shadows sum to one caster: no double shadow, no gap).
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "lod_fade.glsl"
layout(location = 0) flat in float v_fade;
void main() {
    if (lod_faded(v_fade, gl_FragCoord.xy)) discard;
}
