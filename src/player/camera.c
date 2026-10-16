#include "camera.h"
#include "core/engine.h"
#include "core/cvar.h"
#include <math.h>

typedef struct CameraCvars { Cvar *fov, *headbob, *sprint_fov, *bob_amount; } CameraCvars;

static CameraCvars *cc(void) { return g_engine.camera_cvars; }

void camera_register_cvars(void) {
    CameraCvars *c = ARENA_PUSH(&g_engine.perm_arena, CameraCvars);
    g_engine.camera_cvars = c;
    c->fov = cvar_reg_float("cl_fov", 75.0f, 50.0f, 120.0f, CVAR_ARCHIVE, "vertical field of view (deg)");
    c->headbob = cvar_reg_bool("cl_headbob", true, CVAR_ARCHIVE, "camera head bob");
    c->bob_amount = cvar_reg_float("cl_headbob_amount", 1.0f, 0.0f, 3.0f, CVAR_ARCHIVE, "head bob strength");
    c->sprint_fov = cvar_reg_float("cl_sprint_fov", 7.0f, 0.0f, 20.0f, CVAR_ARCHIVE, "extra FOV while sprinting (deg)");
}

float camera_base_fov(void) { return cc()->fov->f; }

void camera_apply_look(ViewState *v, vec2 d) {
    v->yaw = wrap_angle(v->yaw + d.x);
    v->pitch = clampf(v->pitch + d.y, -88.0f * DG_DEG2RAD, 88.0f * DG_DEG2RAD);
}

RenderCamera camera_update(ViewState *v, const PlayerMove *m, float dt, const CameraAds *ca, bool dead) {
    CameraCvars *c = cc();
    float ads_t = ca ? saturate(ca->t) : 0.0f;
    bool ads = ads_t > 0.5f;
    dt = fminf(dt, 0.1f);
    float eye_h = player_eye_height(m);
    float target_y = m->pos.y + eye_h;
    if (dead) target_y = m->pos.y + 0.3f;
    // Stair smoothing: on the ground, ease the eye over step-ups/downs instead of snapping.
    bool grounded = m->on_ground && m->mode == MOVE_WALK;
    if (!v->smooth_init || !grounded || fabsf(target_y - v->smooth_eye_y) > 0.8f) {
        v->smooth_eye_y = target_y;
        v->smooth_init = true;
    } else {
        v->smooth_eye_y = lerpf(v->smooth_eye_y, target_y, exp_decay(16.0f, dt));
    }

    // Landing dip (critically damped spring).
    if (m->land_tick != v->last_land_tick) {
        v->last_land_tick = m->land_tick;
        v->land_vel -= clampf(m->land_speed * 0.06f, 0.0f, 0.9f);
    }
    // Sub-stepped (<= 1/120 s): explicit Euler with this stiffness diverges above ~40 ms frames.
    int steps = (int)ceilf(dt * 120.0f);
    float h = steps > 0 ? dt / (float)steps : 0.0f;
    for (int i = 0; i < steps; i++) {
        float acc = -v->land_offset * 220.0f - v->land_vel * 24.0f;
        v->land_vel += acc * h;
        v->land_offset += v->land_vel * h;
    }

    // Head bob.
    float speed = v3_len(v3_flat(m->vel));
    Cvar *bs = cvar_find("cam_bob_scale");
    float bob_target = (c->headbob->i && grounded) ? saturate(speed / 6.5f) * c->bob_amount->f * (bs ? bs->f : 0.0f) : 0.0f;
    if (ads) bob_target *= 0.2f;
    v->bob_amount = lerpf(v->bob_amount, bob_target, exp_decay(8.0f, dt));
    v->bob_phase = m->stride * (5.0f / 4.5f);   // from the replicated walk cycle (spectators see the same bob)
    float bob_y = fabsf(sinf(v->bob_phase)) * 0.035f * v->bob_amount - 0.017f * v->bob_amount;
    float bob_x = cosf(v->bob_phase) * 0.02f * v->bob_amount;
    v->bob_roll = cosf(v->bob_phase) * 0.006f * v->bob_amount;

    vec3 right = right_from_yaw(v->yaw);
    vec3 eye = v3(m->pos.x, v->smooth_eye_y + bob_y + v->land_offset, m->pos.z);
    eye = v3_madd(eye, right, bob_x);
    v->eye = eye;

    // FOV: sprint widens (smoothed), ADS narrows along the weapon's ADS progress: exactly the
    // aimed FOV once ads_t reaches 1, and a full-screen scope switches to its FOV right then.
    float hip = c->fov->f;
    if (m->sprinting && grounded && ads_t <= 0.0f) hip += c->sprint_fov->f * saturate(speed / 6.0f);
    if (v->fov_hip <= 1.0f) v->fov_hip = hip;
    v->fov_hip = lerpf(v->fov_hip, hip, exp_decay(8.0f, dt));
    float fov = v->fov_hip;
    if (ca && ca->fov > 1.0f && ads_t > 0.0f) fov = lerpf(v->fov_hip, ca->fov, smoothstepf(0.0f, 1.0f, ads_t));
    if (ca && ca->snap_fov > 1.0f && ads_t >= 1.0f) fov = ca->snap_fov;
    v->fov = fov;

    // Slight roll while hanging, pitch nudge during mantle.
    float roll_target = m->mode == MOVE_HANG ? 0.03f : 0.0f;
    if (dead) roll_target = 0.4f;
    v->roll = lerpf(v->roll, roll_target, exp_decay(6.0f, dt));

    RenderCamera cam;
    cam.pos = eye;
    cam.yaw = v->yaw;
    cam.pitch = v->pitch + (m->mode == MOVE_MANTLE ? -0.08f * sinf(saturate(m->m_t) * DG_PI) : 0.0f);
    cam.roll = v->roll + v->bob_roll;
    cam.fov_deg = v->fov;
    cam.vm_fov_deg = v->fov_hip;
    cam.ads_t = (ca && ca->fov > 1.0f) ? ads_t : 0.0f;
    cam.znear = 0.04f;
    cam.zfar = 1500.0f;
    return cam;
}
