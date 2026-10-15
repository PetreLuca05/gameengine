#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) in vec3 v_world;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec2 v_uv;
layout(location = 3) in vec4 v_color;

layout(set = 1, binding = 0) uniform sampler2D tex;

layout(push_constant) uniform Push {
    mat4 model;
    vec4 color;
    vec4 params;
    uvec4 flags;
} pc;

layout(location = 0) out vec4 out_color;

const float PI = 3.14159265;

// Anti-aliased grid line intensity for coordinate c with spacing s and width w (in pixels).
float grid_line(float c, float s, float w) {
    float x = c / s;
    float d = abs(fract(x - 0.5) - 0.5) / max(fwidth(x), 1e-5);
    return 1.0 - clamp(d / w, 0.0, 1.0);
}

vec3 apply_grid(vec3 base, vec3 p, vec3 n) {
    vec3 an = abs(n);
    vec2 c;
    vec2 axis_tint_sel;
    if (an.y >= an.x && an.y >= an.z) c = p.xz;
    else if (an.x >= an.z) c = p.zy;
    else c = p.xy;
    float minor = max(grid_line(c.x, 0.25, 0.8), grid_line(c.y, 0.25, 0.8));
    float major = max(grid_line(c.x, 1.0, 1.3), grid_line(c.y, 1.0, 1.3));
    float big   = max(grid_line(c.x, 5.0, 2.0), grid_line(c.y, 5.0, 2.0));
    vec3 col = base;
    col = mix(col, col * 0.72, minor * 0.6);
    col = mix(col, col * 0.35, major);
    col = mix(col, vec3(0.9, 0.55, 0.1) * 0.5, big * 0.6);
    // Highlight the world origin axes on floors: X red, Z blue.
    if (an.y > 0.9) {
        float ax = grid_line(p.z, 1000.0, 2.5) * step(abs(p.z), 0.5);
        float az = grid_line(p.x, 1000.0, 2.5) * step(abs(p.x), 0.5);
        col = mix(col, vec3(0.8, 0.1, 0.1), ax);
        col = mix(col, vec3(0.1, 0.2, 0.9), az);
    }
    return col;
}

// Measuring grid for the recoil wall: 10 cm minor, 50 cm major, 1 m bold lines.
vec3 apply_fine_grid(vec3 base, vec3 p, vec3 n) {
    vec3 an = abs(n);
    vec2 c;
    if (an.y >= an.x && an.y >= an.z) c = p.xz;
    else if (an.x >= an.z) c = p.zy;
    else c = p.xy;
    float minor = max(grid_line(c.x, 0.1, 0.9), grid_line(c.y, 0.1, 0.9));
    float half_m = max(grid_line(c.x, 0.5, 1.4), grid_line(c.y, 0.5, 1.4));
    float meter = max(grid_line(c.x, 1.0, 2.0), grid_line(c.y, 1.0, 2.0));
    vec3 col = base;
    col = mix(col, col * 0.62, minor * 0.7);
    col = mix(col, vec3(0.1, 0.3, 0.6), half_m * 0.8);
    col = mix(col, vec3(0.75, 0.15, 0.1), meter);
    return col;
}

float shadow_factor(vec3 p, vec3 n) {
    if (frame.sun_color.w < 0.5) return 1.0;
    vec3 offset_p = p + n * 0.04;
    vec4 sp = frame.shadow_vp * vec4(offset_p, 1.0);
    sp.xyz /= sp.w;
    vec2 uv = sp.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || sp.z > 1.0) return 1.0;
    float texel = frame.params.y;
    int r = int(frame.params.z);
    float sum = 0.0;
    float count = 0.0;
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++) {
            sum += texture(shadow_map, vec3(uv + vec2(x, y) * texel, sp.z - 0.0015));
            count += 1.0;
        }
    return sum / count;
}

float d_ggx(float ndh, float a) {
    float a2 = a * a;
    float d = ndh * ndh * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

vec3 f_schlick(float vdh, vec3 f0) { return f0 + (1.0 - f0) * pow(1.0 - vdh, 5.0); }

float v_smith(float ndv, float ndl, float a) {
    float k = a * 0.5;
    return 1.0 / ((ndv * (1.0 - k) + k) * (ndl * (1.0 - k) + k) * 4.0);
}

vec3 brdf(vec3 n, vec3 v, vec3 l, vec3 albedo, float rough, float metal) {
    vec3 h = normalize(v + l);
    float ndl = max(dot(n, l), 0.0);
    float ndv = max(dot(n, v), 1e-4);
    float ndh = max(dot(n, h), 0.0);
    float vdh = max(dot(v, h), 0.0);
    float a = max(rough * rough, 0.02);
    vec3 f0 = mix(vec3(0.04), albedo, metal);
    vec3 F = f_schlick(vdh, f0);
    vec3 spec = d_ggx(ndh, a) * v_smith(ndv, ndl, a) * F;
    vec3 kd = (1.0 - F) * (1.0 - metal);
    return (kd * albedo / PI + spec) * ndl;
}

void main() {
    uint flags = pc.flags.x;
    vec3 n = normalize(v_normal);
    if (!gl_FrontFacing) n = -n;
    vec4 base = pc.color * v_color;

    if ((flags & MAT_TEXTURED) != 0u) {
        vec4 t;
        if ((flags & MAT_TRIPLANAR) != 0u) {
            float s = pc.params.w;
            vec3 w = abs(n);
            w /= (w.x + w.y + w.z);
            t = texture(tex, v_world.zy * s) * w.x + texture(tex, v_world.xz * s) * w.y + texture(tex, v_world.xy * s) * w.z;
        } else {
            t = texture(tex, v_uv * max(pc.params.w, 1e-3));
        }
        base *= t;
    }
    if ((flags & MAT_CHECKER) != 0u) {
        vec2 c = floor(v_world.xz * 0.25);
        float k = mod(c.x + c.y, 2.0);
        base.rgb *= mix(0.8, 1.0, k);
    }
    if ((flags & MAT_GRID) != 0u && frame.params.x > 0.5) base.rgb = apply_grid(base.rgb, v_world, n);
    if ((flags & MAT_GRID_FINE) != 0u) base.rgb = apply_fine_grid(base.rgb, v_world, n);

    vec3 color;
    if ((flags & MAT_UNLIT) != 0u) {
        color = base.rgb;
    } else {
        vec3 v = normalize(frame.camera_pos.xyz - v_world);
        float rough = clamp(pc.params.x, 0.04, 1.0);
        float metal = clamp(pc.params.y, 0.0, 1.0);
        vec3 albedo = base.rgb;
        // hemispheric ambient
        float hemi = n.y * 0.5 + 0.5;
        vec3 ambient = mix(frame.ground_ambient.rgb, frame.sky_ambient.rgb, hemi) * frame.sky_ambient.w;
        color = ambient * albedo * (1.0 - metal * 0.5);
        // sun
        vec3 l = normalize(frame.sun_dir.xyz);
        float sh = shadow_factor(v_world, n);
        color += brdf(n, v, l, albedo, rough, metal) * frame.sun_color.rgb * frame.sun_dir.w * sh;
        // point lights
        int count = int(frame.ground_ambient.w);
        for (int i = 0; i < count; i++) {
            vec3 lp = frame.lights[i].pos_radius.xyz;
            float radius = frame.lights[i].pos_radius.w;
            vec3 dl = lp - v_world;
            float dist = length(dl);
            if (dist > radius) continue;
            vec3 ldir = dl / max(dist, 1e-4);
            float falloff = clamp(1.0 - pow(dist / radius, 4.0), 0.0, 1.0);
            falloff = falloff * falloff / (dist * dist + 1.0);
            color += brdf(n, v, ldir, albedo, rough, metal) * frame.lights[i].color_intensity.rgb * frame.lights[i].color_intensity.w * falloff;
        }
    }
    color += base.rgb * pc.params.z; // emissive

    if ((flags & MAT_HIGHLIGHT) != 0u) {
        vec3 v = normalize(frame.camera_pos.xyz - v_world);
        float rim = pow(1.0 - max(dot(n, v), 0.0), 2.0);
        float pulse = 0.5 + 0.5 * sin(frame.camera_pos.w * 6.0);
        color += vec3(1.0, 0.8, 0.3) * (0.15 + rim * 0.8) * (0.6 + 0.4 * pulse);
    }

    // exponential fog
    float fd = length(frame.camera_pos.xyz - v_world);
    float fog = 1.0 - exp(-fd * frame.fog.w);
    color = mix(color, frame.fog.rgb, clamp(fog, 0.0, 1.0));

    out_color = vec4(color, base.a);
}
