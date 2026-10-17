// The ground's colour at a world position without shading it (docs/FOLIAGE_GRASS.md "Ground matching"):
// the terrain material map (bilinear over its 4 samples) -> each material's mean albedo
// (frame.ground_color, the terrain layers' means) x the shared macro variation. Ground cover mixes its
// lower blades toward it; the terrain's grass term blends toward ground_top beyond the grass fade.
#ifndef TERRAIN_GROUND_GLSL
#define TERRAIN_GROUND_GLSL
#include "surf_bindings.glsl"
#include "terrain/terrain_macro.glsl"

// Material weights of the 4 map samples around xz (ids out, weights out; false: no map).
bool terrain_map_gather(vec2 xz, out ivec4 ids, out vec4 w) {
    ids = ivec4(0);
    w = vec4(1.0, 0.0, 0.0, 0.0);
    if (frame.surf_map.w < 0.5) return false;
    vec2 g = (xz - frame.surf_map.xy) * frame.surf_map.z;   // sample k sits at origin + k spacing
    vec2 gf = floor(g), fr = g - gf;
    int n = int(frame.surf_map.w);
    for (int k = 0; k < 4; k++) {
        ivec2 c = clamp(ivec2(gf) + ivec2(k & 1, k >> 1), ivec2(0), ivec2(n - 1));
        ids[k] = min(int(texelFetch(surf_matmap, c, 0).r * 255.0 + 0.5), 15);
    }
    w = vec4((1.0 - fr.x) * (1.0 - fr.y), fr.x * (1.0 - fr.y), (1.0 - fr.x) * fr.y, fr.x * fr.y);
    return true;
}

// Mean ground albedo (linear) at xz, macro variation applied; top: the grass tops' colour there.
vec3 terrain_ground_color(vec2 xz, out vec3 top) {
    ivec4 ids;
    vec4 w;
    if (!terrain_map_gather(xz, ids, w)) {
        top = vec3(0.16, 0.17, 0.06);
        return vec3(0.075, 0.067, 0.025);
    }
    vec3 c = vec3(0.0);
    top = vec3(0.0);
    for (int k = 0; k < 4; k++) {
        c += frame.ground_color[ids[k]].rgb * w[k];
        top += frame.ground_top[ids[k]].rgb * w[k];
    }
    vec3 f = ground_macro_field(xz);
    top = ground_macro_apply(top, f, 1.0);
    return ground_macro_apply(c, f, 1.0);
}
#endif
