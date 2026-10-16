// Vertex stage of the classic mesh draws (classic.vert, classic_shadow.vert): the instance transform,
// then the material's WPO; velocity from last frame's wind and time.
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec4 in_color;
layout(location = 4) in vec4 i_pos;
layout(location = 5) in vec4 i_rot;
layout(location = 6) in vec4 i_misc;

layout(location = 2) out vec2 v_uv;
#ifndef CL_SHADOW
layout(location = 0) out vec3 v_world;
layout(location = 1) out vec3 v_normal;
layout(location = 3) out vec4 v_color;
layout(location = 7) out vec4 v_cur_clip;
layout(location = 8) out vec4 v_prev_clip;
layout(location = 9) flat out float v_fade;
// The prepass and the lit pass (depth EQUAL) must produce bit-identical depth.
invariant gl_Position;
#endif

void main() {
    vec3 base = i_pos.xyz + cl_qrot(i_rot, in_pos * i_pos.w);
    vec3 w = base + cl_wpo(base, i_pos.xyz, in_color, i_misc.x, false);
    v_uv = in_uv;
#ifdef CL_SHADOW
    gl_Position = shadow_pass_vp(pc.flags.w) * vec4(w, 1.0);
#else
    v_world = w;
    v_normal = cl_qrot(i_rot, in_normal);
    v_color = pc.flags.y != 0u ? vec4(1.0) : in_color;   // WPO materials: the colour is wind weights
    v_fade = i_misc.y;
    gl_Position = frame.view_proj * vec4(w, 1.0);
    v_cur_clip = frame.view_proj_nj * vec4(w, 1.0);
    vec3 pw = pc.flags.y != 0u ? base + cl_wpo(base, i_pos.xyz, in_color, i_misc.x, true) : w;
    v_prev_clip = frame.prev_view_proj_nj * vec4(pw, 1.0);
#endif
}
