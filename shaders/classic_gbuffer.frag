#version 450
// Classic prepass, opaque materials (no discard: early depth).
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#define PBR_MAPS   // glTF material maps (set 5)
#include "classic_common.glsl"
#define GB_FLAGS pc.flags.x
#define GB_PARAMS pc.params
#include "gbuffer_body.glsl"
