// Shared lit shading (lit.frag, skin.frag). The includer defines MAT_COLOR, MAT_PARAMS, MAT_FLAGS.
layout(location = 0) in vec3 v_world;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec2 v_uv;
layout(location = 3) in vec4 v_color;

layout(set = 1, binding = 0) uniform sampler2D tex;
// Screen-space inputs from the post stack (push descriptor, render resolution): ambient occlusion
// (indirect only), sun contact shadows, reflections (rgb + confidence), diffuse GI.
layout(set = 2, binding = 0) uniform sampler2D post_ao;
layout(set = 2, binding = 1) uniform sampler2D post_contact;
layout(set = 2, binding = 2) uniform sampler2D post_ssr;
layout(set = 2, binding = 3) uniform sampler2D post_ssgi;
layout(set = 2, binding = 4) uniform sampler3D post_froxel;   // integrated volumetric fog (blended draws)

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

// Sighting-in target: 1 MOA squares at its distance (spacing s in metres), 5 MOA bold, 10 MOA red
// (level.c snaps the target centre onto a 10 MOA crossing; the aim square is its own box).
vec3 apply_moa_grid(vec3 base, vec3 p, vec3 n, float s) {
    vec3 an = abs(n);
    vec2 c;
    if (an.y >= an.x && an.y >= an.z) c = p.xz;
    else if (an.x >= an.z) c = p.zy;
    else c = p.xy;
    s = max(s, 1e-4);
    // Each level fades out once its squares get smaller than a few pixels (readable through a
    // scope, a clean board from far away).
    float px = max(length(fwidth(c)), 1e-6);
    float f1 = clamp((s / px - 4.0) / 4.0, 0.0, 1.0), f5 = clamp((s * 5.0 / px - 4.0) / 4.0, 0.0, 1.0), f10 = clamp((s * 10.0 / px - 4.0) / 4.0, 0.0, 1.0);
    float minor = max(grid_line(c.x, s, 0.8), grid_line(c.y, s, 0.8)) * f1;
    float five = max(grid_line(c.x, s * 5.0, 1.2), grid_line(c.y, s * 5.0, 1.2)) * f5;
    float ten = max(grid_line(c.x, s * 10.0, 1.6), grid_line(c.y, s * 10.0, 1.6)) * f10;
    vec3 col = base;
    col = mix(col, col * 0.55, minor * 0.8);
    col = mix(col, vec3(0.08, 0.1, 0.12), five * 0.85);
    col = mix(col, vec3(0.8, 0.08, 0.05), ten);
    return col;
}

// Time-of-day sky (src/render/sky): irradiance from the SH9 of the environment, the
// environment itself (octahedral, mips by roughness), the cloud shadow on the ground plane.
vec3 sky_irradiance(vec3 d) {
    const float A0 = 3.14159265, A1 = 2.0943951, A2 = 0.785398163;
    vec3 e = sky_sh.sh[0].rgb * 0.282095 * A0;
    e += (sky_sh.sh[1].rgb * d.y + sky_sh.sh[2].rgb * d.z + sky_sh.sh[3].rgb * d.x) * 0.488603 * A1;
    e += (sky_sh.sh[4].rgb * 1.092548 * d.x * d.y + sky_sh.sh[5].rgb * 1.092548 * d.y * d.z + sky_sh.sh[6].rgb * 0.315392 * (3.0 * d.z * d.z - 1.0) +
          sky_sh.sh[7].rgb * 1.092548 * d.x * d.z + sky_sh.sh[8].rgb * 0.546274 * (d.x * d.x - d.y * d.y)) * A2;
    return max(e, vec3(0.0));
}
vec2 sky_env_uv(vec3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    vec2 e = n.y >= 0.0 ? n.xz : (1.0 - abs(n.zx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.z >= 0.0 ? 1.0 : -1.0);
    return e * 0.5 + 0.5;
}
float cloud_shadow_at(vec3 p) {
    if (frame.sky_flags.w < 0.5) return 1.0;
    vec2 uv = (p.xz - frame.cshadow.xy) * frame.cshadow.z;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return 1.0;
    return textureLod(sky_cloud_shadow, uv, 0.0).r;
}

// One cascade of the 2 x 2 shadow atlas: -1 when p is outside its tile.
float shadow_cascade(int c, vec3 p, vec3 n) {
    float k = c == 0 ? 1.0 : (c == 1 ? 2.5 : 7.0);   // texel size relative to cascade 0
    vec3 offset_p = p + n * 0.03 * k;
    vec4 sp = frame.shadow_vps[c] * vec4(offset_p, 1.0);
    sp.xyz /= sp.w;
    vec2 uv = sp.xy * 0.5 + 0.5;
    vec2 tile = vec2(c & 1, c >> 1) * 0.5;
    vec2 local = (uv - tile) * 2.0;
    float edge = 2.0 * frame.params.y * (frame.params.z + 1.0);
    if (any(lessThan(local, vec2(edge))) || any(greaterThan(local, vec2(1.0 - edge))) || sp.z > 1.0 || sp.z < 0.0) return -1.0;
    float texel = frame.params.y;
    int r = int(frame.params.z);
    float sum = 0.0;
    float count = 0.0;
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++) {
            sum += texture(shadow_map, vec3(uv + vec2(x, y) * texel, sp.z - 0.0012 * k));
            count += 1.0;
        }
    return sum / count;
}

float shadow_factor(vec3 p, vec3 n) {
    if (frame.sun_color.w < 0.5) return 1.0;
    int count = int(frame.shadow_splits.w);
    if (count <= 1) {
        float s = shadow_cascade(0, p, n);
        return s < 0.0 ? 1.0 : s;
    }
    // Cascade by distance from the camera; the last 15 % of a cascade blends into the next one.
    float d = length(p - frame.camera_pos.xyz);
    for (int c = 0; c < 3; c++) {
        float radius = frame.shadow_splits[c];
        if (d > radius && c < 2) continue;
        float s = shadow_cascade(c, p, n);
        if (s < 0.0) continue;
        float blend = c < 2 ? smoothstep(radius * 0.85, radius, d) : 0.0;
        if (blend > 0.0) {
            float s2 = shadow_cascade(c + 1, p, n);
            if (s2 >= 0.0) s = mix(s, s2, blend);
        }
        return s;
    }
    return 1.0;
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
#ifdef LIT_PRE_MAIN
    LIT_PRE_MAIN
#endif
    uint flags = MAT_FLAGS;
    vec3 n = normalize(v_normal);
    if (!gl_FrontFacing) n = -n;
    vec4 base = MAT_COLOR * v_color;

    if ((flags & MAT_TEXTURED) != 0u) {
        vec4 t;
        if ((flags & MAT_TRIPLANAR) != 0u) {
            float s = MAT_PARAMS.w;
            vec3 w = abs(n);
            w /= (w.x + w.y + w.z);
            t = texture(tex, v_world.zy * s) * w.x + texture(tex, v_world.xz * s) * w.y + texture(tex, v_world.xy * s) * w.z;
        } else {
            t = texture(tex, v_uv * max(MAT_PARAMS.w, 1e-3));
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
    if ((flags & MAT_GRID_MOA) != 0u) base.rgb = apply_moa_grid(base.rgb, v_world, n, MAT_PARAMS.w);

    vec3 color;
    if ((flags & MAT_UNLIT) != 0u) {
        color = base.rgb;
    } else {
        vec3 v = normalize(frame.camera_pos.xyz - v_world);
        float rough = clamp(MAT_PARAMS.x, 0.04, 1.0);
        float metal = clamp(MAT_PARAMS.y, 0.0, 1.0);
        vec3 albedo = base.rgb;
        // Screen-space inputs (never on the viewmodel: it is excluded from the screen-space passes).
        bool vm = (flags & MAT_VIEWMODEL) != 0u;
        vec2 suv = gl_FragCoord.xy * frame.screen.zw;
        float ao = (!vm && frame.post_flags.x > 0.5) ? mix(1.0, texture(post_ao, suv).r, frame.post_params.z) : 1.0;
        float contact = (!vm && frame.post_flags.y > 0.5) ? texture(post_contact, suv).r : 1.0;
        // hemispheric ambient (indirect diffuse): AO only darkens indirect light
        float hemi = n.y * 0.5 + 0.5;
        vec3 ambient = mix(frame.ground_ambient.rgb, frame.sky_ambient.rgb, hemi) * frame.sky_ambient.w;
        bool sky_env_on = frame.sky_flags.x > 0.5;
        if (sky_env_on) ambient = sky_irradiance(n) / PI * frame.sky_flags.y;   // time-of-day sky (Lambert: E / pi)
        if (!vm && frame.post_flags.w > 0.5) ambient += texture(post_ssgi, suv).rgb * frame.post_params.y;
        color = ambient * albedo * (1.0 - metal) * ao;
        // indirect specular: sky / ground by the reflection vector, replaced by SSR where it hit
        vec3 rv = reflect(-v, n);
        float ndv0 = max(dot(n, v), 1e-4);
        vec3 f0 = mix(vec3(0.04), albedo, metal);
        vec3 fr = f0 + (max(vec3(1.0 - rough), f0) - f0) * pow(1.0 - ndv0, 5.0);
        vec3 env = mix(frame.ground_ambient.rgb, frame.sky_ambient.rgb, rv.y * 0.5 + 0.5) * frame.sky_ambient.w;
        if (sky_env_on) env = textureLod(sky_env, sky_env_uv(rv), rough * frame.sky_flags.z).rgb * frame.sky_flags.y;
        vec3 spec_ind = env;
        if (!vm && frame.post_flags.z > 0.5) {
            vec4 ssr = texture(post_ssr, suv);
            spec_ind = mix(env, ssr.rgb, clamp(ssr.a * frame.post_params.x, 0.0, 1.0));
        }
        float spec_occ = clamp(pow(ndv0 + ao, exp2(-16.0 * rough - 1.0)) - 1.0 + ao, 0.0, 1.0);
        color += spec_ind * fr * frame.post_params.w * spec_occ;
        // sun
        vec3 l = normalize(frame.sun_dir.xyz);
        float sh = shadow_factor(v_world, n) * contact * cloud_shadow_at(v_world);
        color += brdf(n, v, l, albedo, rough, metal) * frame.sun_color.rgb * frame.sun_dir.w * sh;
        // Fill: the other celestial light (the moon by day / the sun at twilight), unshadowed.
        if (frame.fill_dir.w > 0.0) color += brdf(n, v, normalize(frame.fill_dir.xyz), albedo, rough, metal) * frame.fill_color.rgb * frame.fill_dir.w;
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
    color += base.rgb * MAT_PARAMS.z; // emissive

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
#ifdef LIT_FROXEL_FOG
    // Blended surfaces are drawn after the volumetric fog pass: fog their own contribution with
    // the froxel volume (in-scattering + transmittance up to this depth).
    if (LIT_FROXEL_FOG && frame.post_fog.x > 0.5) {
        vec2 fuv = gl_FragCoord.xy * frame.screen.zw;
        float vz = max(dot(v_world - frame.camera_pos.xyz, -vec3(frame.view[0][2], frame.view[1][2], frame.view[2][2])), 0.15);
        float w = clamp(log(vz / 0.15) / log(frame.post_fog.y / 0.15), 0.0, 1.0);
        vec4 fv = textureLod(post_froxel, vec3(fuv, w), 0.0);
        color = color * fv.a + fv.rgb;
    }
#endif

    out_color = vec4(color, base.a);
}
