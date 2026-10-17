#include "camera_fp.h"
#include "core/engine.h"
#include "core/cvar.h"
#include <math.h>
#include <string.h>

static CamFpCvars g_cv;

void camera_fp_register_cvars(void) {
    CamFpCvars *c = &g_cv;
    c->stiffness = cvar_reg_float("cam_head_stiffness", 260.0f, 1.0f, 5000.0f, CVAR_ARCHIVE, "head camera spring stiffness (1/s^2)");
    c->damping = cvar_reg_float("cam_head_damping", 30.0f, 0.0f, 500.0f, CVAR_ARCHIVE, "head camera spring damping (1/s)");
    c->follow = cvar_reg_float("cam_head_follow", 0.8f, 0.0f, 1.0f, CVAR_ARCHIVE, "0 = camera locked to the ideal eye, 1 = fully follows the animated head");
    c->follow_lateral = cvar_reg_float("cam_head_follow_lateral", 0.35f, 0.0f, 1.0f, CVAR_ARCHIVE, "extra factor on horizontal head motion (sway is the motion-sickness axis)");
    c->clamp = cvar_reg_float("cam_head_clamp", 0.07f, 0.0f, 0.5f, CVAR_ARCHIVE, "max camera displacement from the ideal eye (m)");
    c->bob_scale = cvar_reg_float("cam_bob_scale", 0.0f, 0.0f, 2.0f, CVAR_ARCHIVE, "first-person camera stabilizer / landing dip / bob offset from the head eye (0 = camera exactly on the head)");
    c->kick_scale = cvar_reg_float("cam_kick_scale", 1.0f, 0.0f, 3.0f, CVAR_ARCHIVE, "visual camera kick on shots (aim recoil is separate)");
    c->recon_smooth = cvar_reg_float("cam_recon_smooth_time", 0.1f, 0.0f, 1.0f, CVAR_ARCHIVE, "seconds to smooth prediction corrections of the camera");
    c->third = cvar_reg_bool("cam_third", false, 0, "third-person debug camera (same body / hold point math)");
    c->debug = cvar_reg_int("cam_debug", 0, 0, 2, 0, "1: draw eye / head / hold point / pivot / sockets / IK targets / collision + stats, 2: also log");
    c->radius = cvar_reg_float("cam_collision_radius", 0.11f, 0.0f, 0.5f, CVAR_ARCHIVE, "camera keeps this distance from walls (m)");
    c->death_follow = cvar_reg_float("cam_death_follow", 6.0f, 0.1f, 50.0f, CVAR_ARCHIVE, "death camera follow rate on the ragdoll head");
    c->death_spectate = cvar_reg_float("cam_death_spectate", 0.0f, 0.0f, 60.0f, CVAR_ARCHIVE, "seconds on the ragdoll head before lifting into a spectator view (0 = stay)");
    c->weapon_fov = cvar_reg_float("cam_weapon_fov", 0.0f, 0.0f, 120.0f, CVAR_ARCHIVE, "FOV of the arms / held item (0 = same as the camera)");
}

const CamFpCvars *camera_fp_cvars(void) { return &g_cv; }

void camera_fp_reset(CamHeadStab *s) { memset(s, 0, sizeof(*s)); }

static float cvf(struct Cvar *c, float def) { return c ? c->f : def; }

vec3 camera_fp_stabilize(CamHeadStab *s, vec3 ideal, vec3 head, float yaw, float dt) {
    CamStabParams p = cam_stab_params_default();
    p.stiffness = cvf(g_cv.stiffness, p.stiffness);
    p.damping = cvf(g_cv.damping, p.damping);
    p.follow = cvf(g_cv.follow, p.follow);
    p.follow_lateral = cvf(g_cv.follow_lateral, p.follow_lateral);
    p.clamp = cvf(g_cv.clamp, p.clamp);
    // cam_bob_scale blends this result in from outside (game_render.c); the spring itself runs fully.
    return camera_fp_stabilize_ex(s, ideal, head, yaw, dt, &p);
}

vec3 camera_fp_death(CamHeadStab *s, vec3 head, float dt) {
    dt = clampf(dt, 0.0f, 0.1f);
    if (!s->dead) {
        s->dead = true;
        s->death_t = 0;
        s->death_vel = v3(0, 0, 0);
        s->death_pos = head;   // every death starts on the head (a later death kept the previous death's spot)
    }
    s->death_t += dt;
    vec3 target = head;
    float spec = cvf(g_cv.death_spectate, 0.0f);
    if (spec > 0 && s->death_t > spec) {
        float u = clampf((s->death_t - spec) / 2.0f, 0.0f, 1.0f);
        target = v3_add(head, v3(0, 2.5f * u * u * (3 - 2 * u), 1.5f * u));
    }
    float r = cvf(g_cv.death_follow, 6.0f);
    // Critically damped follow.
    float k = r * r, d = 2.0f * r;
    vec3 acc = v3_sub(v3_scale(v3_sub(target, s->death_pos), k), v3_scale(s->death_vel, d));
    s->death_vel = v3_madd(s->death_vel, acc, dt);
    s->death_pos = v3_madd(s->death_pos, s->death_vel, dt);
    s->init = false;   // re-init the stabilizer on respawn
    return s->death_pos;
}

vec3 camera_fp_collide(CamHeadStab *s, vec3 safe, vec3 eye, CamSphereCastFn cast, void *ctx, float dt) {
    float r = cvf(g_cv.radius, 0.11f);
    if (!cast || r <= 0) return eye;
    vec3 d = v3_sub(eye, safe);
    float len = v3_len(d);
    if (len < 1e-4f) return eye;
    float frac = 1.0f;
    float want = 0;
    if (cast(ctx, safe, d, r, &frac) && frac < 1.0f) want = (1.0f - frac) * len;
    // Pull in immediately, release smoothly (no pop when leaving a wall).
    if (want > s->col_push) s->col_push = want;
    else s->col_push += (want - s->col_push) * (1.0f - expf(-10.0f * dt));
    if (s->col_push <= 1e-5f) return eye;
    return v3_madd(eye, d, -fminf(s->col_push, len) / len);
}
