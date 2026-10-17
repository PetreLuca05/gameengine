// glTF PBR material maps (docs/PBR.md), shared by lit_body.glsl and gbuffer_body.glsl when the includer
// defines PBR_MAPS. Each map is sampled only when its MAT_*_TEX flag is set (no flag = the scalar
// factors alone, exactly as before):
//   slot 0 metallic-roughness  g roughness, b metallic (x the factors), linear
//   slot 1 normal              tangent space, xy x normal_scale, linear (glTF / OpenGL convention: +y = image up)
//   slot 2 occlusion           r, 1 + strength * (r - 1), linear: multiplies the screen-space AO
//   slot 3 emissive            sRGB x emissive colour x the emissive scalar
// The includer may define
//   PBR_TEX(slot, uv)      the sampler (default: set 3 of the lit layout, one set per material)
//   PBR_PARAMS             uvec4: x / y float bits of normal_scale / occlusion_strength, z emissive RGBA8,
//                          w float bits of alpha_cutoff (default: the push constants' pbr)
//   PBR_TANGENT_FRAME(n)   a mat3 (T along +u, B along +v, n) when it knows the triangle (vgeo resolve);
//                          default: the cotangent frame from screen derivatives (no tangent attribute)
#ifndef PBR_TEX
layout(set = 3, binding = 0) uniform sampler2D pbr_tex[4];
#define PBR_TEX(slot, uv) texture(pbr_tex[slot], (uv))
#endif
#ifndef PBR_PARAMS
#define PBR_PARAMS pc.pbr
#endif

void pbr_metal_rough(uint flags, vec2 uv, inout float rough, inout float metal) {
    if ((flags & MAT_MR_TEX) == 0u) return;
    vec4 t = PBR_TEX(0, uv);
    rough *= t.g;
    metal *= t.b;
}

// n: the interpolated (and back-face flipped) normal; p the world position (derivatives).
vec3 pbr_normal(uint flags, vec2 uv, vec3 p, vec3 n) {
    if ((flags & MAT_NORMAL_TEX) == 0u) return n;
#ifdef PBR_TANGENT_FRAME
    mat3 tbn = PBR_TANGENT_FRAME(n);
#else
    mat3 tbn = cotangent_frame(n, dFdx(p), dFdy(p), dFdx(uv), dFdy(uv));
#endif
    vec3 t = PBR_TEX(1, uv).xyz * 2.0 - 1.0;
    t.xy *= uintBitsToFloat(PBR_PARAMS.x);
    // glTF images have v down: image up (+y of the map) is -v.
    vec3 pn = tbn * vec3(t.x, -t.y, t.z);
    return dot(pn, pn) > 1e-8 ? normalize(pn) : n;
}

float pbr_occlusion(uint flags, vec2 uv) {
    if ((flags & MAT_OCCLUSION_TEX) == 0u) return 1.0;
    return 1.0 + uintBitsToFloat(PBR_PARAMS.y) * (PBR_TEX(2, uv).r - 1.0);
}

// The emissive colour per unit of the material's emissive scalar.
vec3 pbr_emissive(vec2 uv) { return PBR_TEX(3, uv).rgb * unpackUnorm4x8(PBR_PARAMS.z).rgb; }

float pbr_alpha_cutoff() { return uintBitsToFloat(PBR_PARAMS.w); }
