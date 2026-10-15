#version 450
// Tonemap + optional FXAA + brightness/gamma. Output is UNORM, so sRGB encoding is done here.
layout(location = 0) in vec2 v_uv;
layout(set = 0, binding = 0) uniform sampler2D hdr;
layout(set = 1, binding = 0) uniform sampler2D bloom;   // bloom mip 0 (half resolution)

layout(push_constant) uniform Push {
    vec4 params;   // x exposure, y gamma, z aa mode (0 none, 1 fxaa, 2 taa stub), w vignette
    vec4 texel;    // xy = 1/source size, z = bloom intensity, w = exposure boost (muzzle flash flicker)
} pc;

layout(location = 0) out vec4 out_color;

vec3 aces(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

vec3 tonemap(vec3 c) { return aces(c * pc.params.x * (1.0 + pc.texel.w)); }
vec3 fetch(vec2 uv) { return tonemap(texture(hdr, uv).rgb + texture(bloom, uv).rgb * pc.texel.z); }
float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

vec3 fxaa(vec2 uv) {
    vec2 t = pc.texel.xy;
    vec3 rgbNW = fetch(uv + vec2(-1.0, -1.0) * t);
    vec3 rgbNE = fetch(uv + vec2(1.0, -1.0) * t);
    vec3 rgbSW = fetch(uv + vec2(-1.0, 1.0) * t);
    vec3 rgbSE = fetch(uv + vec2(1.0, 1.0) * t);
    vec3 rgbM = fetch(uv);
    float lNW = luma(rgbNW), lNE = luma(rgbNE), lSW = luma(rgbSW), lSE = luma(rgbSE), lM = luma(rgbM);
    float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
    float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
    if (lMax - lMin < max(0.0312, lMax * 0.125)) return rgbM;
    vec2 dir;
    dir.x = -((lNW + lNE) - (lSW + lSE));
    dir.y = ((lNW + lSW) - (lNE + lSE));
    float reduce = max((lNW + lNE + lSW + lSE) * 0.25 * 0.125, 1.0 / 128.0);
    float rcp = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
    dir = clamp(dir * rcp, vec2(-8.0), vec2(8.0)) * t;
    vec3 a = 0.5 * (fetch(uv + dir * (1.0 / 3.0 - 0.5)) + fetch(uv + dir * (2.0 / 3.0 - 0.5)));
    vec3 b = a * 0.5 + 0.25 * (fetch(uv + dir * -0.5) + fetch(uv + dir * 0.5));
    float lb = luma(b);
    return (lb < lMin || lb > lMax) ? a : b;
}

void main() {
    vec3 c = (pc.params.z > 0.5) ? fxaa(v_uv) : fetch(v_uv);
    // vignette
    vec2 d = v_uv - 0.5;
    c *= 1.0 - dot(d, d) * pc.params.w;
    // linear -> sRGB with user gamma adjustment
    c = pow(max(c, vec3(0.0)), vec3(1.0 / (2.2 * pc.params.y)));
    out_color = vec4(c, 1.0);
}
