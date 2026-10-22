// Deferred lighting of classic content (docs/perf/FINDINGS.md PF-0039, docs/CLASSIC_RENDERER.md
// "Deferred lighting"): what the prepass of a deferred classic material stores beside the shared
// normal / material / velocity targets, for classic_deferred.frag. The values are the lit pass's own
// (lit_surface.glsl, then its material and weather terms), so the full-screen pass shades exactly what
// the lit pass would have shaded at depth EQUAL, without drawing the geometry a second time.
//   albedo  RGBA16F    rgb the albedo after the weather, a the roughness after the weather
//   params  RGBA16UI   x baked AO (vertex alpha x occlusion map x ground cover; half), y snow cover (half),
//                      z ground cover thickness (half), w CLD_* flags | the material's AO byte << 8
// The metallic is the G-buffer material's g; the normal is the G-buffer's (ground cover facing down
// is turned up again, as the lit pass does, in classic_deferred.frag).
#define CLD_DEFERRED 1u            // a deferred classic material kept this pixel (0: the opaque pass lights it)
#define CLD_COVER 2u               // ground cover (MAT_COVER): translucency, its id in bits 2..4
#define CLD_COVER_ID_SHIFT 2u
#define CLD_NO_RECEIVE 32u         // Receive Shadows off
#define CLD_UNLIT 64u
#define CLD_AO_SHIFT 8u

vec3 cld_oct_decode(vec2 e) {
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}
