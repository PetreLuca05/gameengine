// Surface texture bindings of the frame set (src/render/surface_tex.c): included by surfaces.glsl (the
// legacy lit pass) and shaders/terrain/terrain_ground.glsl (ground colour under foliage, any lit pass).
#ifndef SURF_BINDINGS_GLSL
#define SURF_BINDINGS_GLSL
layout(set = 0, binding = 12) uniform sampler2DArray surf_albedo;
layout(set = 0, binding = 13) uniform sampler2DArray surf_nrh;
layout(set = 0, binding = 14) uniform sampler2D surf_matmap;
#endif
