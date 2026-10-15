#version 450
// Scope (premultiplied over). Picture-in-picture: the ocular disc shows the scope camera's image
// magnified by k relative to the main view, sampled per pixel from the main-projection ray (the
// image stays locked to the optic axis), with radial chromatic aberration, a soft inner rim, the
// eye box (scope shadow when the eye is off the exit pupil or at the wrong relief) and the
// reticle: first focal plane (fixed against the target, scales with magnification) or second
// focal plane (fixed in the eyepiece). Away from the eye the ocular is dark coated glass.
// Full-screen mode (r_scope_mode 1, once aimed): the main view already has the scope FOV; the
// shader draws the tube vignette and the reticle around the optic axis.
#extension GL_GOOGLE_include_directive : require
#include "sight_common.glsl"
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

vec3 pip_sample(vec2 puv) {
    if (any(lessThan(puv, vec2(0.0))) || any(greaterThan(puv, vec2(1.0)))) return vec3(0.0);
    return textureLod(pip_tex, puv, 0.0).rgb;
}

vec2 pip_uv(vec2 tw) {
    vec3 wd = S.fwd.xyz + tw.x * S.right.xyz + tw.y * S.up.xyz;
    vec4 c = S.pip_from_view * vec4(wd, 0.0);
    return c.xy / max(c.w, 1e-6) * 0.5 + 0.5;
}

vec3 reticle_over(vec3 col, vec2 t, float px, float scale) {
    float shape = S.glass.z;
    float thick = S.dotp.w * scale, outer = S.glass.w * scale;
    float m = reticle_mask(t, px, shape, thick, outer);
    col = mix(col, S.color.rgb, m);
    if (S.eye.w > 0.5) {
        // Illuminated centre: a small emissive dot.
        float r = length(t);
        float c = 1.0 - smoothstep(thick * 1.6 - px, thick * 1.6 + px, r);
        col += vec3(1.0, 0.08, 0.03) * 4.0 * exposure_inv() * c;
    }
    return col;
}

void main() {
    vec2 uv = gl_FragCoord.xy / S.render.zw;
    float kind = S.fwd.w, ads = S.up.w, dbg = S.vmdepth.z;
    float px_main = 2.0 * U.depthp.z / max(S.misc.w, 1.0);
    vec3 dm = main_ray(uv);
    float df = dot(dm, S.fwd.xyz);
    vec2 t = vec2(dot(dm, S.right.xyz), dot(dm, S.up.xyz)) / max(df, 1e-4);

    if (dbg == 2.0) {
        // Raw PiP, centred square.
        float a = S.misc.y;
        vec2 q = (uv - 0.5) * vec2(a, 1.0) + 0.5;
        out_color = vec4(pip_sample(q), 1.0);
        return;
    }
    if (kind == 3.0 && S.right.w > 0.5) {
        // Full screen: tube vignette around the optic axis, reticle in world angles (k = 1).
        vec4 c = U.proj * vec4(S.fwd.xyz, 0.0);
        vec2 cuv = c.xy / max(c.w, 1e-6) * 0.5 + 0.5;
        vec2 d = (uv - cuv) * vec2(S.misc.y, 1.0);
        float r = length(d) / 0.47;
        float w = max(fwidth(r), 1e-4);
        float inside = 1.0 - smoothstep(1.0 - w, 1.0 + w, r);
        float vig = smoothstep(0.7, 1.0, r) * 0.55;
        vec3 col = reticle_over(vec3(0.0), t, px_main, 1.0);
        float ink = reticle_mask(t, px_main, S.glass.z, S.dotp.w, S.glass.w);
        float alpha = max(1.0 - inside, max(vig * inside, ink * inside));
        out_color = vec4(sanitize(col * inside), alpha);
        return;
    }

    LensHit h = lens_hit(uv);
    float cov = h.ok ? disc_cov(h.r) : 0.0;
    if (dbg == 6.0) {
        // Lens test: blue = outside the disc, red = occluded by the viewmodel, green = visible.
        vec3 c = cov <= 0.0 ? vec3(0.0, 0.0, 1.0) : (lens_occluded(uv, h.z) ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0));
        out_color = vec4(c * exposure_inv() * 0.3, 1.0);
        return;
    }
    if (cov <= 0.0 || lens_occluded(uv, h.z)) discard;
    // Dark coated glass (hip / no picture): faint sky reflection.
    float fr = S.glass.x + (1.0 - S.glass.x) * pow(1.0 - clamp(h.cos_t, 0.0, 1.0), 5.0);
    vec3 glass = U.sky.rgb * max(U.sky.w, 0.0) * (0.04 + fr * 0.5);
    vec3 col = glass;
    if (kind == 2.0) {
        float k = max(S.scope.x, 1.0);
        vec2 tw = t / k;
        // Chromatic aberration: radial, growing to S.glass.y pixels at the rim.
        float lens_px = 1.0 / max(fwidth(h.r), 1e-4);
        float ca = S.glass.y / max(lens_px, 1.0) * h.r * h.r;
        vec3 img = vec3(pip_sample(pip_uv(tw * (1.0 + ca))).r, pip_sample(pip_uv(tw)).g, pip_sample(pip_uv(tw * (1.0 - ca))).b);
        // Reticle: FFP in world angles, SFP fixed in the eyepiece (nominal magnification).
        bool ffp = S.vmdepth.w > 0.5;
        float kn = max(S.scope.y, 1.0);
        vec2 rt = ffp ? tw : t / kn;
        float rpx = px_main / (ffp ? k : kn);
        img = reticle_over(img, rt, rpx, 1.0);
        // Soft inner rim.
        img *= 1.0 - 0.3 * smoothstep(0.78, 1.0, h.r);
        // Eye box: the exit pupil seen from the eye. Off axis the visible circle slides the other
        // way and a crescent of shadow closes in; a wrong relief shrinks it.
        vec2 eo = S.eye.xy / S.scope.z;
        float dz = clamp(abs(S.eye.z) / S.scope.w, 0.0, 1.0);
        float rr = 1.0 - 0.6 * dz;
        vec2 sc = -eo * 0.5;
        float ew = max(fwidth(h.r), 1e-3) * 4.0 + 0.05;
        float vis = 1.0 - smoothstep(rr - ew, rr, length(h.q - sc));
        vis *= 1.0 - smoothstep(1.4, 2.2, length(eo));
        float through = smoothstep(0.55, 1.0, ads);
        if (dbg == 3.0) img = mix(vec3(1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0), vis) * exposure_inv() * 0.3;
        if (dbg == 4.0) img = reticle_over(vec3(exposure_inv() * 0.5), rt, rpx, 1.0);
        col = mix(glass, img * vis, through);
    }
    if (dbg == 1.0) col = vec3(0.2, 1.0, 0.2) * exposure_inv() * 0.3;
    out_color = vec4(sanitize(col) * cov, cov);
}
