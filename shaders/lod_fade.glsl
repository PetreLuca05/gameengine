// LOD cross-fade dither (docs/LOD.md, src/render/lod_fade.c). Include after common.glsl.
//
// Signed fade value f of a draw / instance (lod_fade_draws, vk_classic.c):
//   0          fully visible
//   (0, 1)     fading in: keeps the texels whose threshold n < f (coverage f)
//   (-1, 0)    fading out: keeps n >= 1 + f (coverage -f), the complement of a fade-in at 1 + f
// The outgoing and the incoming LOD of one cross-fade use the same n, so their coverage sums to 1
// exactly: no holes, no double coverage (also in the shadow maps: shadow passes dither the same way).
//
// n is a 16 x 16 void-and-cluster blue-noise rank (tools/gen_lod_bluenoise.py), shifted per frame by
// an R2 offset when TAA resolves it (frame.lod_fade.x = frame index & 63, 0 = static): TAA averages the
// two LODs into a blend instead of a fixed pattern. frame.lod_fade.z = 0: the legacy dither (the static
// interleaved gradient noise of the skinned path before docs/LOD.md, r_vgeo 1 / r_lod_legacy 1).
const uint LOD_BN[256] = uint[](
    237u,  21u,  52u, 145u, 174u,  62u, 229u,  20u, 182u, 136u,  24u, 160u,  81u, 230u, 188u, 128u,
     90u, 155u, 190u, 109u,   5u, 202u,  45u, 112u, 216u,  51u, 104u, 249u, 121u,  40u, 148u,  14u,
     68u, 247u,  33u, 218u,  82u, 133u, 157u, 246u,  85u, 147u, 209u,   0u, 173u,  64u, 221u, 199u,
    170u, 103u, 127u,  56u, 235u, 185u,  70u,  10u, 176u,  32u,  74u, 132u, 194u,  94u, 117u,  31u,
    225u,   3u, 210u, 150u,  23u,  97u,  39u, 200u, 123u, 239u, 167u, 217u,  47u,  22u, 253u, 138u,
     49u, 164u,  72u, 180u, 115u, 252u, 142u, 228u, 101u,  59u,  12u, 107u, 234u, 153u, 184u,  80u,
    122u,  93u, 233u,  15u, 203u,  54u, 163u,  77u,  26u, 192u, 158u,  83u, 126u,  66u,  11u, 204u,
     29u, 193u, 135u,  42u,  87u, 125u,   9u, 175u, 220u, 137u, 245u,  37u, 211u, 166u, 100u, 242u,
    171u,  58u, 248u, 159u, 226u, 187u, 214u,  95u,  48u, 110u,  61u, 181u,  18u, 232u,  46u, 141u,
      1u, 111u,  78u,  19u, 106u,  65u,  34u, 152u, 238u,   2u, 205u,  88u, 144u, 116u,  71u, 222u,
    154u, 198u, 215u, 177u, 143u, 250u, 119u, 197u,  75u, 124u, 156u, 255u,  30u, 201u, 179u,  91u,
     43u, 131u,  28u,  96u,  50u,   7u, 169u,  25u, 227u,  44u, 189u, 105u,  53u, 129u,  13u, 243u,
    108u, 231u,  69u, 240u, 186u, 219u,  84u, 134u,  99u, 172u,  16u,  73u, 223u, 165u, 208u,  63u,
    183u,   6u, 139u, 162u, 113u,  67u, 151u, 244u,  55u, 212u, 146u, 241u, 114u,  27u,  86u, 149u,
    213u,  79u, 196u,  41u,  17u, 207u,  36u, 191u,   4u, 120u,  92u,  38u, 178u, 140u, 251u,  35u,
    168u, 118u, 224u,  89u, 254u, 130u,  98u, 161u,  76u, 236u, 195u,  60u, 206u,   8u, 102u,  57u
);

// Threshold in (0, 1) of a pixel (frame: the per-frame rotation, 0 = static).
float lod_dither_at(ivec2 px, uint frame_k) {
    uvec2 o = uvec2(fract(vec2(0.7548777, 0.5698403) * float(frame_k)) * 16.0);
    uvec2 p = (uvec2(px) + o) & 15u;
    return (float(LOD_BN[p.y * 16u + p.x]) + 0.5) / 256.0;
}

// The legacy dither of the skinned shaders (static interleaved gradient noise).
float lod_dither_legacy(vec2 frag) { return fract(52.9829189 * fract(0.06711056 * frag.x + 0.00583715 * frag.y)); }

float lod_dither(vec2 frag) {
    return frame.lod_fade.z > 0.5 ? lod_dither_at(ivec2(frag), uint(frame.lod_fade.x)) : lod_dither_legacy(frag);
}

// true: the texel is not covered by this draw's share of the fade (discard).
bool lod_faded_n(float f, float n) { return f != 0.0 && (f > 0.0 ? n >= f : n < 1.0 + f); }
bool lod_faded(float f, vec2 frag) { return f != 0.0 && lod_faded_n(f, lod_dither(frag)); }

// r_lod_fade_debug (frame.lod_fade.y bit 0): fading draws tinted (green in, red out).
vec3 lod_fade_debug_tint(vec3 c, float f) {
    if ((uint(frame.lod_fade.y) & 1u) == 0u || f == 0.0) return c;
    return mix(c, f > 0.0 ? vec3(0.1, 1.0, 0.1) : vec3(1.0, 0.1, 0.1), 0.6);
}

// r_lod_debug 2 (frame.lod_fade.y bit 1): the LOD level as a colour (0 white, 1 green, 2 yellow, 3 red, impostor blue).
vec3 lod_level_tint(vec3 c, float level) {
    if ((uint(frame.lod_fade.y) & 2u) == 0u || level < 0.0) return c;
    const vec3 k[5] = vec3[](vec3(1.0), vec3(0.2, 1.0, 0.2), vec3(1.0, 0.9, 0.1), vec3(1.0, 0.2, 0.1), vec3(0.2, 0.4, 1.0));
    return c * k[min(uint(level + 0.5), 4u)];
}
