// Night vision goggles: shared by shaders/post/nvg_*.comp (docs/NIGHT_OPTICS.md "Image intensifier").
// The CPU mirror (tests) is src/render/post/nvg_model.c: keep the mask and the noise model in step.

// The angle (deg) between a pixel's ray (x, y, f: pixels from the view centre, focal length in pixels)
// and a tube axis turned cx_deg about the vertical.
float nvg_axis_angle(vec2 xy, float f, float cx_deg) {
    float c = radians(cx_deg);
    return degrees(acos(clamp((xy.x * sin(c) + f * cos(c)) / length(vec3(xy, f)), -1.0, 1.0)));
}

// A tube's field against its radius r_deg: no hard rim, an extreme eyepiece vignette (full brightness to
// NVG_VIG_INNER of the radius, a long fall to black at NVG_VIG_OUTER; nvg_model.h).
const float NVG_VIG_INNER = 0.35, NVG_VIG_OUTER = 1.15;
float nvg_circle(vec2 xy, float f, float cx_deg, float r_deg) {
    float d = nvg_axis_angle(xy, f, cx_deg);
    float t = clamp((d / r_deg - NVG_VIG_INNER) / (NVG_VIG_OUTER - NVG_VIG_INNER), 0.0, 1.0);
    float v = 1.0 - t * t;
    return v * v * v;
}

// One circle, or two overlapping ones (dual tube: a figure 8). xy from the view centre in pixels,
// x already divided by the horizontal stretch (the field spans the screen's width).
float nvg_mask(vec2 xy, float f, float tubes, float circle_deg, float sep_deg) {
    float r = circle_deg * 0.5;
    if (tubes < 1.5) return nvg_circle(xy, f, 0.0, r);
    return max(nvg_circle(xy, f, -sep_deg, r), nvg_circle(xy, f, sep_deg, r));
}

// The radial position inside the field (0 on a tube axis .. 1 at its edge; the nearer tube).
float nvg_radial(vec2 xy, float f, float tubes, float circle_deg, float sep_deg) {
    float a = tubes < 1.5 ? nvg_axis_angle(xy, f, 0.0) : min(nvg_axis_angle(xy, f, -sep_deg), nvg_axis_angle(xy, f, sep_deg));
    return a / (circle_deg * 0.5);
}

uvec3 nvg_pcg3d(uvec3 v) {
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return v;
}
vec3 nvg_rand3(uvec3 v) { return vec3(nvg_pcg3d(v) >> 8u) * (1.0 / 16777216.0); }

float nvg_gauss(vec2 u) { return sqrt(-2.0 * log(max(u.x, 1e-7))) * cos(6.2831853 * u.y); }
