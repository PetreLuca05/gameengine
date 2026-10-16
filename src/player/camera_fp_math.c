// Pure head-camera stabilizer math (no cvars / engine): unit tested in tests/test_anim.c.
#include "camera_fp.h"
#include <math.h>

CamStabParams cam_stab_params_default(void) {
    CamStabParams p = { 260.0f, 30.0f, 0.8f, 0.35f, 0.07f, 1.0f };
    return p;
}

vec3 camera_fp_stabilize_ex(CamHeadStab *s, vec3 ideal, vec3 head, float yaw, float dt, const CamStabParams *p) {
    dt = clampf(dt, 0.0f, 0.1f);
    s->dead = false;
    s->death_t = 0;
    // Spring frame: camera yaw (x right, y up, z back) so lateral / forward get their own follow.
    quat iq = quat_conj(quat_yaw(yaw));
    vec3 raw = quat_rotate(iq, v3_sub(head, ideal));
    float follow = p->follow, lat = p->follow_lateral, bob = p->bob_scale;
    vec3 target = v3(raw.x * follow * lat * bob, raw.y * follow * bob, raw.z * follow * lat * bob);
    if (!s->init) { s->disp = target; s->vel = v3(0, 0, 0); s->init = true; }
    int n = (int)ceilf(dt * 240.0f);
    if (n < 1) n = 1;
    float h = dt / (float)n;
    for (int i = 0; i < n; i++) {
        vec3 acc = v3_sub(v3_scale(v3_sub(target, s->disp), p->stiffness), v3_scale(s->vel, p->damping));
        s->vel = v3_madd(s->vel, acc, h);
        s->disp = v3_madd(s->disp, s->vel, h);
        // Hard clamp from the ideal eye with a soft knee (no visible edge when reaching it).
        float lim = p->clamp, len = v3_len(s->disp);
        if (lim > 0 && len > lim * 0.7f) {
            float knee = lim * 0.7f;
            float soft = knee + (lim - knee) * (1.0f - expf(-(len - knee) / fmaxf(lim - knee, 1e-4f)));
            s->disp = v3_scale(s->disp, soft / len);
            vec3 dir = v3_scale(s->disp, 1.0f / fmaxf(v3_len(s->disp), 1e-6f));
            float vo = v3_dot(s->vel, dir);
            if (vo > 0) s->vel = v3_madd(s->vel, dir, -vo);
        }
    }
    s->max_seen = fmaxf(s->max_seen, v3_len(s->disp));
    s->last_raw = raw;
    return v3_add(ideal, quat_rotate(quat_yaw(yaw), s->disp));
}
