// Vertex stage of the classic mesh draws (classic.vert, classic_shadow.vert): the instance transform,
// then the material's WPO (wpo_veg, the vgeo path's model and inputs); velocity from last frame's wind
// field, tile and time; the bent / fluttering normal with VGW_NORMALS (vgeo: the resolve).
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec4 in_color;
layout(location = 4) in vec4 i_pos;
layout(location = 5) in vec4 i_rot;
layout(location = 6) in vec4 i_misc;

layout(location = 2) out vec2 v_uv;
layout(location = 9) flat out vec4 v_fade;   // the instance's fade alpha, cross-fade interval, debug (classic_common.glsl cl_faded)
#ifndef CL_SHADOW
layout(location = 0) out vec3 v_world;
layout(location = 1) out vec3 v_normal;
layout(location = 3) out vec4 v_color;
layout(location = 7) out vec4 v_cur_clip;
layout(location = 8) out vec4 v_prev_clip;
layout(location = 10) out vec4 v_cover;          // ground cover (shaders/cover.glsl): x root -> tip, y card hash, z clump hash
layout(location = 11) flat out vec3 v_cover_pivot;
// The prepass and the lit pass (depth EQUAL) must produce bit-identical depth.
invariant gl_Position;
#endif

void main() {
    vec3 base = i_pos.xyz + cl_qrot(i_rot, in_pos * i_pos.w);
    vec3 n = cl_qrot(i_rot, in_normal), nb = n;
    uint layers = cw.misc.z;
    v_uv = in_uv;
    v_fade = i_misc.yzwx;
#ifdef CL_SHADOW
    vec3 w = base + cl_wpo(base, n, i_pos.xyz, i_pos.w, in_color, i_misc.x, false, layers & ~VGW_NORMALS, nb);
    gl_Position = shadow_pass_vp(pc.flags.w) * vec4(w, 1.0);
#else
    vec3 w = base + cl_wpo(base, n, i_pos.xyz, i_pos.w, in_color, i_misc.x, false, layers, nb);
    v_world = w;
    v_normal = nb;
    v_color = pc.flags.y != 0u ? (cl_wind_debug() ? cl_wind_debug_color(i_pos.xyz, in_color) : vec4(1.0)) : in_color;   // WPO materials: the colour is wind weights
    v_cover = vec4(in_color.r, in_color.a, i_misc.x, 0.0);
    v_cover_pivot = i_pos.xyz;
    gl_Position = frame.view_proj * vec4(w, 1.0);
    v_cur_clip = frame.view_proj_nj * vec4(w, 1.0);
    vec3 pw = w;
    if (pc.flags.y != 0u) {
        vec3 np = n;
        pw = base + cl_wpo(base, n, i_pos.xyz, i_pos.w, in_color, i_misc.x, true, layers & ~VGW_NORMALS, np);
    }
    v_prev_clip = frame.prev_view_proj_nj * vec4(pw, 1.0);
#endif
}
