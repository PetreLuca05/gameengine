// Classic impostors (src/render/vk_classic.c): a camera-facing quad per instance, textured from its
// mesh's hemi-octahedral atlas (the vgeo bake, shaders/vgeo/vg_impostor.glsl conventions): the 3 frames
// nearest to the view direction blended, each sampled where the pixel's point on the quad projects
// into it. Albedo atlas (sRGB, alpha = coverage) at set 3 binding 1, mesh-space normals at binding 0.
// Include after classic_common.glsl.
layout(location = 0) in vec3 i_world;
layout(location = 1) flat in vec4 i_rot;
layout(location = 2) flat in vec4 i_ctr;     // quad centre (world), half size
layout(location = 3) flat in vec4 i_fade;   // fade alpha, cross-fade interval, debug (cl_faded)

vec3 cl_hemi_oct_decode(vec2 uv) {
    vec2 p = uv * 2.0 - 1.0;
    vec3 d = vec3((p.x + p.y) * 0.5, 0.0, (p.x - p.y) * 0.5);
    d.y = 1.0 - abs(d.x) - abs(d.z);
    return normalize(d);
}
vec2 cl_hemi_oct_encode(vec3 d) {
    d.y = max(d.y, 0.0);
    d /= max(abs(d.x) + abs(d.y) + abs(d.z), 1e-8);
    return vec2(d.x + d.z, d.x - d.z) * 0.5 + 0.5;
}
void cl_impostor_basis(vec3 f, out vec3 r, out vec3 u) {
    vec3 up = abs(f.y) > 0.999 ? vec3(0.0, 0.0, -1.0) : vec3(0.0, 1.0, 0.0);
    r = normalize(cross(up, f));
    u = cross(f, r);
}

// The atlas at world point p of the quad: rgb albedo (linear), a coverage; n: world normal.
vec4 cl_impostor_sample(vec3 p, out vec3 n) {
    vec3 c = i_ctr.xyz;
    float rad = max(pc.model[0].w, 1e-8), scale = i_ctr.w / rad;
    vec4 iq = vec4(-i_rot.xyz, i_rot.w);
    vec3 dl = cl_qrot(iq, frame.camera_pos.xyz - c);
    float nf = max(pc.model[1].x, 2.0);
    vec2 g = cl_hemi_oct_encode(dot(dl, dl) > 1e-12 ? normalize(dl) : vec3(0.0, 1.0, 0.0)) * (nf - 1.0);
    vec2 i0 = clamp(floor(g), vec2(0.0), vec2(nf - 2.0)), f = g - i0;
    vec2 fr[3];
    vec3 w;
    if (f.x + f.y < 1.0) { fr[0] = i0; fr[1] = i0 + vec2(1.0, 0.0); fr[2] = i0 + vec2(0.0, 1.0); w = vec3(1.0 - f.x - f.y, f.x, f.y); }
    else { fr[0] = i0 + vec2(1.0); fr[1] = i0 + vec2(0.0, 1.0); fr[2] = i0 + vec2(1.0, 0.0); w = vec3(f.x + f.y - 1.0, 1.0 - f.x, 1.0 - f.y); }
    float tile = float(textureSize(cl_aux2, 0).x) / nf;
    float proj_scale = 0.5 * frame.screen.y * abs(frame.proj[1][1]);
    float dist = max(length(frame.camera_pos.xyz - c), 0.1);
    float lod = max(log2(tile / max(2.0 * i_ctr.w * proj_scale / dist, 1e-4)), 0.0);
    float hm = 0.5 / tile;
    vec3 l = cl_qrot(iq, p - c) / max(scale, 1e-8);
    vec4 acc = vec4(0.0);
    vec3 nacc = vec3(0.0);
    for (int k = 0; k < 3; k++) {
        vec3 fd = cl_hemi_oct_decode(fr[k] / (nf - 1.0)), r, u;
        cl_impostor_basis(fd, r, u);
        vec2 t = vec2(0.5 + 0.5 * dot(l, r) / rad, 0.5 - 0.5 * dot(l, u) / rad);
        if (any(lessThan(t, vec2(0.0))) || any(greaterThan(t, vec2(1.0)))) continue;
        vec2 uv = (fr[k] + clamp(t, vec2(hm), vec2(1.0 - hm))) / nf;
        vec4 a = textureLod(cl_aux2, uv, lod);
        acc += vec4(a.rgb * a.a, a.a) * w[k];
        nacc += (textureLod(cl_aux, uv, lod).xyz * 2.0 - 1.0) * a.a * w[k];
    }
    n = normalize(cl_qrot(i_rot, dot(nacc, nacc) > 1e-12 ? nacc : vec3(0.0, 1.0, 0.0)));
    return vec4(acc.a > 1e-6 ? acc.rgb / acc.a : vec3(0.0), acc.a);
}

// The inputs of the shared G-buffer / lit bodies (*_CUSTOM_INPUTS); false: not covered.
vec3 v_world, v_normal;
vec2 v_uv;
vec4 v_color, v_cur_clip, v_prev_clip;
// The values are always written: the lit pass (depth EQUAL) shades every pixel the prepass kept, even
// where its own coverage test would land on the other side of the threshold.
bool cl_impostor_pixel() {
    vec3 n;
    vec4 a = cl_impostor_sample(i_world, n);
    v_world = i_world;
    v_normal = n;
    v_uv = vec2(0.0);
    v_color = vec4(a.rgb, 1.0);
    v_cur_clip = frame.view_proj_nj * vec4(i_world, 1.0);
    v_prev_clip = frame.prev_view_proj_nj * vec4(i_world, 1.0);
    return a.a >= 0.5 && !cl_faded(i_fade, gl_FragCoord.xy);
}
