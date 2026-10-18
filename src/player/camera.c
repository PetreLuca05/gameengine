#include "camera.h"
#include "core/engine.h"
#include "core/cvar.h"
#include <math.h>

typedef struct CameraCvars { Cvar *fov, *sv_fov, *sv_fov_max, *max_aspect, *ads_world_zoom, *headbob, *sprint_fov, *bob_amount, *hands_fov; } CameraCvars;

static CameraCvars *cc(void) { return g_engine.camera_cvars; }

void camera_register_cvars(void) {
    CameraCvars *c = ARENA_PUSH(&g_engine.perm_arena, CameraCvars);
    g_engine.camera_cvars = c;
    // The world camera FOV is the player's (Settings -> Graphics -> View): vertical degrees, Hor+ up
    // to sv_fov_max_aspect, wider screens keep that aspect's horizontal FOV. A host may force one FOV
    // for everyone (sv_fov) or cap it (sv_fov_max). Hands / weapon FOV: cam_weapon_fov.
    c->fov = cvar_reg_float("cl_fov", 75.0f, 60.0f, 120.0f, CVAR_ARCHIVE | CVAR_EDITABLE, "world camera vertical FOV (deg; 75 = 107 deg horizontal at 16:9)");
    c->sv_fov = cvar_reg_float("sv_fov", 0.0f, 0.0f, 120.0f, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "host override of every player's world FOV (vertical deg, replicated; 0 = each player's cl_fov)");
    c->sv_fov_max = cvar_reg_float("sv_fov_max", 0.0f, 0.0f, 120.0f, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "host cap of the players' cl_fov (vertical deg, replicated; 0 = no cap)");
    c->max_aspect = cvar_reg_float("sv_fov_max_aspect", 16.0f / 9.0f, 0.0f, 4.0f, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE,
                                   "widest aspect that sees more to the sides; wider screens keep its horizontal FOV (0 = unlimited Hor+)");
    c->ads_world_zoom = cvar_reg_float("cl_ads_world_zoom", 1.5f, 1.0f, 1.5f, CVAR_LOCKED,
                                       "ADS: the world camera narrows by this (tangent ratio) when fully aimed, on top of the hands / weapon zoom (1 = off; a sight's \"world_zoom\" overrides it)");
    c->headbob = cvar_reg_bool("cl_headbob", true, CVAR_ARCHIVE | CVAR_EDITABLE, "camera head bob");
    c->bob_amount = cvar_reg_float("cl_headbob_amount", 1.0f, 0.0f, 1.0f, CVAR_ARCHIVE | CVAR_EDITABLE, "head bob strength");
    c->sprint_fov = cvar_reg_float("cl_sprint_fov", 7.0f, 0.0f, 7.0f, CVAR_ARCHIVE | CVAR_EDITABLE, "extra FOV while sprinting (deg)");
    // The arms layer's FOV (docs/CAMERA_HOLDPOINT.md "Rule 3"): your arms and the held item, never the
    // world. Vertical like cl_fov, Hor+ to the same widest aspect; aimed, the sight's own FOV.
    c->hands_fov = cvar_reg_float("cl_hands_fov", 65.0f, 50.0f, 90.0f, CVAR_ARCHIVE | CVAR_EDITABLE,
                                  "Hands FOV: vertical FOV of your arms and held item (deg; the world keeps cl_fov)");
}

float camera_hands_fov(void) {
    int w = render_width(), h = render_height();
    float aspect = (w > 0 && h > 0) ? (float)w / (float)h / render_stretch_factor() : 16.0f / 9.0f;
    return camera_fov_for_aspect(cc()->hands_fov->f, cc()->max_aspect->f, aspect);
}

float camera_fov_for_aspect(float fov_v, float max_aspect, float aspect) {
    if (max_aspect <= 0.1f || aspect <= max_aspect) return fov_v;
    return 2.0f * atanf(tanf(fov_v * 0.5f * DG_DEG2RAD) * max_aspect / aspect) * DG_RAD2DEG;
}

float camera_fov_setting(float cl_fov, float sv_fov, float sv_fov_max) {
    if (sv_fov >= 1.0f) return sv_fov;
    return sv_fov_max >= 1.0f ? fminf(cl_fov, sv_fov_max) : cl_fov;
}

float camera_base_fov(void) {
    int w = render_width(), h = render_height();
    float aspect = (w > 0 && h > 0) ? (float)w / (float)h / render_stretch_factor() : 16.0f / 9.0f;
    return camera_fov_for_aspect(camera_fov_setting(cc()->fov->f, cc()->sv_fov->f, cc()->sv_fov_max->f), cc()->max_aspect->f, aspect);
}

float camera_ads_world_zoom_default(void) { return cc()->ads_world_zoom->f; }

float camera_ads_fov(float hip_fov, float world_zoom, float ads_t) {
    float k = 1.0f + (fmaxf(world_zoom, 1.0f) - 1.0f) * smoothstepf(0.0f, 1.0f, saturate(ads_t));
    return 2.0f * atanf(tanf(hip_fov * 0.5f * DG_DEG2RAD) / k) * DG_RAD2DEG;
}

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

    // FOV: the player's base (cl_fov / the host's sv_fov), sprint widens it (smoothed). ADS narrows
    // the world (the sight's world zoom, cl_ads_world_zoom 1.5x, along smoothstep(ads_t): exact the frame
    // ADS completes); the arms / weapon projection narrows by its own, stronger vm_ads_zoom and a
    // scope magnifies inside its lens.
    float hip = camera_base_fov();
    if (m->sprinting && grounded && ads_t <= 0.0f) hip += c->sprint_fov->f * saturate(speed / 6.0f);
    if (v->fov_hip <= 1.0f) v->fov_hip = hip;
    v->fov_hip = lerpf(v->fov_hip, hip, exp_decay(8.0f, dt));
    v->fov = (ca && ca->world_zoom > 1.0f && ads_t > 0.0f) ? camera_ads_fov(v->fov_hip, ca->world_zoom, ads_t) : v->fov_hip;

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
    cam.ads_t = (ca && ca->vm_zoom > 1.0f) ? ads_t : 0.0f;
    cam.vm_ads_zoom = ca ? fmaxf(ca->vm_zoom, 1.0f) : 1.0f;
    cam.znear = 0.04f;
    cam.zfar = 1500.0f;
    return cam;
}
