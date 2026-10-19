#include "camera.h"
#include "fov.h"
#include "core/engine.h"
#include "core/cvar.h"
#include <math.h>

typedef struct CameraCvars { Cvar *fov, *sv_fov, *sv_fov_max, *max_aspect, *ads_world_zoom, *sprint_fov, *hands_fov, *ads_fov, *weapon_fov; } CameraCvars;

static CameraCvars *cc(void) { return g_engine.camera_cvars; }

// The viewed player's FOV settings (a spectated player's, forwarded in its commands); unset: the local cvars.
static FovPrefs g_view_prefs;
static bool g_view_prefs_set;

void camera_register_cvars(void) {
    CameraCvars *c = ARENA_PUSH(&g_engine.perm_arena, CameraCvars);
    g_engine.camera_cvars = c;
    // The FOV model (docs/CAMERA_HOLDPOINT.md "Field of view", player/fov.h): three settings in
    // Settings -> Graphics -> Comfort. Field of View is horizontal degrees at 16:9, Hor+ (the vertical
    // FOV is the same on every aspect). A host may force one FOV for everyone (sv_fov) or cap it
    // (sv_fov_max), in the same units. Weapon Field of View: the arms layer only (game_arms.c).
    c->fov = cvar_reg_float("cl_fov", FOV_DEFAULT, FOV_MIN, FOV_MAX, CVAR_ARCHIVE | CVAR_EDITABLE,
                            "Field of View: the world camera at the hip (horizontal deg at 16:9, Hor+; 80 = 50.5 deg vertical)");
    c->ads_fov = cvar_reg_int("cl_ads_fov", FOV_ADS_INDEPENDENT, 0, 1, CVAR_ARCHIVE | CVAR_EDITABLE,
                              "ADS Field of View: 0 Independent (aiming goes back to the default 80, then the sight's zoom), 1 Affected (the sight's FOV x Field of View / 80)");
    c->weapon_fov = cvar_reg_int("cl_weapon_fov", WEAPON_FOV_DEFAULT, 0, WEAPON_FOV_COUNT - 1, CVAR_ARCHIVE | CVAR_EDITABLE,
                                 "Weapon Field of View: 0 Narrow, 1 Default, 2 Wide (your arms and held item only, never the world; arms_layer.json weapon_fov)");
    c->sv_fov = cvar_reg_float("sv_fov", 0.0f, 0.0f, 150.0f, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE,
                               "host override of every player's Field of View (horizontal deg at 16:9, replicated; 0 = each player's cl_fov)");
    c->sv_fov_max = cvar_reg_float("sv_fov_max", 0.0f, 0.0f, 150.0f, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE,
                                   "host cap of the players' Field of View (horizontal deg at 16:9, replicated; 0 = no cap)");
    c->max_aspect = cvar_reg_float("sv_fov_max_aspect", 0.0f, 0.0f, 4.0f, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE,
                                   "widest aspect that sees more to the sides; wider screens keep its horizontal FOV (0 = Hor+ on every aspect)");
    c->ads_world_zoom = cvar_reg_float("cl_ads_world_zoom", 1.15f, 1.0f, 1.5f, CVAR_LOCKED,
                                       "ADS: the irons' / red dots' world zoom against the ADS base (tangent ratio; 1 = none; a sight's \"world_zoom\" overrides it)");
    c->sprint_fov = cvar_reg_float("cl_sprint_fov", 0.12f, 0.0f, 0.3f, CVAR_LOCKED, "sprint widening at full sprint: the current FOV's tangent x (1 + this)");
    // The arms layer's FOV (docs/CAMERA_HOLDPOINT.md "Rule 3"): your arms and the held item, never the
    // world. The player picks Weapon Field of View; this overrides the preset (dev, tuning).
    c->hands_fov = cvar_reg_float("cl_hands_fov", 0.0f, 0.0f, 130.0f, CVAR_LOCKED,
                                  "dev: the arms layer's hip FOV (horizontal deg at 16:9) instead of the Weapon Field of View preset (0 = the preset)");
}

FovPrefs camera_fov_prefs_local(void) {
    FovPrefs p = { cc()->fov->f, cc()->ads_fov->i, cc()->weapon_fov->i };
    return p;
}

void camera_set_view_prefs(const FovPrefs *p) {
    g_view_prefs_set = p != NULL;
    if (p) g_view_prefs = *p;
}

FovPrefs camera_fov_prefs(void) { return g_view_prefs_set ? g_view_prefs : camera_fov_prefs_local(); }

static float screen_aspect(void) {
    int w = render_width(), h = render_height();
    return (w > 0 && h > 0) ? (float)w / (float)h / render_stretch_factor() : FOV_REF_ASPECT;
}

// Field of View as it applies (horizontal deg at 16:9): the host's override / cap, else the viewed player's.
static float setting_h16(void) { return camera_fov_setting(camera_fov_prefs().fov, cc()->sv_fov->f, cc()->sv_fov_max->f); }

float camera_fov_v(float h16) { return camera_fov_for_aspect(fov_h16_to_v(h16), cc()->max_aspect->f, screen_aspect()); }

float camera_hands_fov(const float presets[WEAPON_FOV_COUNT]) {
    float h = cc()->hands_fov->f >= 1.0f ? cc()->hands_fov->f : fov_weapon(presets, camera_fov_prefs().weapon_fov);
    return camera_fov_v(h);
}

float camera_fov_for_aspect(float fov_v, float max_aspect, float aspect) {
    if (max_aspect <= 0.1f || aspect <= max_aspect) return fov_v;
    return 2.0f * atanf(tanf(fov_v * 0.5f * DG_DEG2RAD) * max_aspect / aspect) * DG_RAD2DEG;
}

float camera_fov_setting(float cl_fov, float sv_fov, float sv_fov_max) {
    if (sv_fov >= 1.0f) return sv_fov;
    return sv_fov_max >= 1.0f ? fminf(cl_fov, sv_fov_max) : cl_fov;
}

float camera_base_fov(void) { return camera_fov_v(setting_h16()); }

float camera_ads_base_fov(void) { return camera_fov_v(fov_ads_base(camera_fov_prefs().ads_mode, setting_h16())); }

float camera_ads_world_fov(float sight_zoom) { return camera_fov_v(fov_ads(camera_fov_prefs().ads_mode, setting_h16(), sight_zoom)); }

float camera_ads_world_zoom_default(void) { return cc()->ads_world_zoom->f; }

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

    // Head bob: part of the optional movement shake (cl_move_shake, Comfort; never the animation's
    // motion, docs/CAMERA_HOLDPOINT.md "Rule 2"); off by default, the camera glides.
    float speed = v3_len(v3_flat(m->vel));
    Cvar *ms = cvar_find("cl_move_shake");
    float shake = ms ? ms->f : 0.0f;
    float bob_target = grounded ? saturate(speed / 6.5f) * shake : 0.0f;
    if (ads) bob_target *= 0.2f;
    v->bob_amount = lerpf(v->bob_amount, bob_target, exp_decay(8.0f, dt));
    v->bob_phase = m->stride * (5.0f / 4.5f);   // from the replicated walk cycle (spectators see the same bob)
    float bob_y = fabsf(sinf(v->bob_phase)) * 0.035f * v->bob_amount - 0.017f * v->bob_amount;
    float bob_x = cosf(v->bob_phase) * 0.02f * v->bob_amount;
    v->bob_roll = cosf(v->bob_phase) * 0.006f * v->bob_amount;

    vec3 right = right_from_yaw(v->yaw);
    v->shake = v3_madd(v3(0, bob_y + v->land_offset * shake, 0), right, bob_x);
    vec3 eye = v3_add(v3(m->pos.x, v->smooth_eye_y, m->pos.z), v->shake);
    v->eye = eye;

    // FOV (docs/CAMERA_HOLDPOINT.md "Field of view"): Field of View (or the host's sv_fov), sprint
    // widens it (a multiplier, smoothed). ADS blends to the aimed world FOV (ADS Field of View: the
    // default 80 or Field of View, narrowed by the sight's world zoom) along smoothstep(ads_t): exact
    // the frame ADS completes. The arms layer has its own FOV (Weapon Field of View) and a scope
    // magnifies inside its lens.
    float hip = camera_base_fov();
    if (m->sprinting && grounded && ads_t <= 0.0f) hip = fov_sprint(hip, c->sprint_fov->f * saturate(speed / 6.0f));
    if (v->fov_hip <= 1.0f) v->fov_hip = hip;
    v->fov_hip = lerpf(v->fov_hip, hip, exp_decay(8.0f, dt));
    v->fov = (ca && ca->ads_fov > 1.0f && ads_t > 0.0f) ? fov_ads_blend(v->fov_hip, ca->ads_fov, ads_t) : v->fov_hip;

    // Slight roll while hanging, pitch nudge during mantle (movement shake); the death tilt.
    float roll_target = m->mode == MOVE_HANG ? 0.03f * shake : 0.0f;
    if (dead) roll_target = 0.4f;
    v->roll = lerpf(v->roll, roll_target, exp_decay(6.0f, dt));

    RenderCamera cam;
    cam.pos = eye;
    cam.yaw = v->yaw;
    cam.pitch = v->pitch + (m->mode == MOVE_MANTLE ? -0.08f * shake * sinf(saturate(m->m_t) * DG_PI) : 0.0f);
    cam.roll = v->roll + v->bob_roll;
    cam.fov_deg = v->fov;
    cam.vm_fov_deg = v->fov_hip;
    cam.ads_t = (ca && ca->vm_zoom > 1.0f) ? ads_t : 0.0f;
    cam.vm_ads_zoom = ca ? fmaxf(ca->vm_zoom, 1.0f) : 1.0f;
    cam.znear = 0.04f;
    cam.zfar = 1500.0f;
    return cam;
}
