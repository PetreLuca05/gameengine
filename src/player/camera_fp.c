#include "camera_fp.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/log.h"
#include "core/platform.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EYE_JSON "assets/player/eye_nominal.json"

static CamFpCvars g_cv;
static EyeNominalTable g_eye;

void camera_fp_register_cvars(void) {
    CamFpCvars *c = &g_cv;
    c->glide_stiffness = cvar_reg_float("cam_glide_stiffness", 150.0f, 1.0f, 5000.0f, CVAR_LOCKED,
                                        "first-person camera glide spring stiffness (1/s^2): stance / speed / item changes and step-ups");
    c->glide_damping = cvar_reg_float("cam_glide_damping", 24.5f, 0.0f, 500.0f, CVAR_LOCKED, "first-person camera glide spring damping (1/s; 2 sqrt(stiffness) = critical)");
    c->anchor_radius = cvar_reg_float("cam_anchor_radius", 0.08f, 0.01f, 0.5f, CVAR_LOCKED,
                                      "the nominal eye must stay this close to the animated head eye (m): logged when not, the body is never moved (cam_anchor_report)");
    c->anim_influence = cvar_reg_float("cam_anim_influence", 0.0f, 0.0f, 1.0f, CVAR_LOCKED, "dev: 0 = the camera glides on the nominal eye, 1 = the raw head bone eye (comparison)");
    c->kick_scale = cvar_reg_float("cam_kick_scale", 1.0f, 0.25f, 1.0f, CVAR_ARCHIVE | CVAR_EDITABLE, "visual camera kick on shots (aim recoil is separate)");
    c->move_shake = cvar_reg_float("cl_move_shake", 0.0f, 0.0f, 1.0f, CVAR_ARCHIVE | CVAR_EDITABLE,
                                   "movement camera shake: head bob, landing dip, ledge / mantle tilt (0 = off: the camera glides)");
    c->recon_smooth = cvar_reg_float("cam_recon_smooth_time", 0.1f, 0.0f, 1.0f, CVAR_LOCKED, "seconds to smooth prediction corrections of the camera");
    c->third = cvar_reg_bool("cam_third", false, CVAR_LOCKED, "third-person debug camera (same body / hold point math)");
    c->debug = cvar_reg_int("cam_debug", 0, 0, 2, CVAR_LOCKED, "1: draw eye / head / hold point / pivot / sockets / IK targets / collision + stats, 2: also log");
    c->radius = cvar_reg_float("cam_collision_radius", 0.11f, 0.0f, 0.5f, CVAR_LOCKED, "camera keeps this distance from walls (m)");
    c->death_follow = cvar_reg_float("cam_death_follow", 6.0f, 0.1f, 50.0f, CVAR_LOCKED, "death camera follow rate on the ragdoll head");
    c->death_spectate = cvar_reg_float("cam_death_spectate", 0.0f, 0.0f, 60.0f, CVAR_LOCKED, "seconds on the ragdoll head before lifting into a spectator view (0 = stay)");
    camera_fp_eye_table_load();
}

const CamFpCvars *camera_fp_cvars(void) { return &g_cv; }

static float cvf(struct Cvar *c, float def) { return c ? c->f : def; }

CamGlideParams camera_fp_glide_params(void) {
    CamGlideParams p = cam_glide_params_default();
    p.stiffness = cvf(g_cv.glide_stiffness, p.stiffness);
    p.damping = cvf(g_cv.glide_damping, p.damping);
    return p;
}

void camera_fp_reset(CamHeadStab *s) { memset(s, 0, sizeof(*s)); }

const EyeNominalTable *camera_fp_eye_table(void) { return &g_eye; }
EyeNominalTable *camera_fp_eye_table_mut(void) { return &g_eye; }

bool camera_fp_eye_table_load(void) {
    char path[600];
    size_t size = 0;
    char *txt = platform_read_file(platform_data_path(EYE_JSON, path, sizeof(path)), &size);
    cJSON *root = txt ? cJSON_Parse(txt) : NULL;
    eye_table_parse(&g_eye, root);
    if (!g_eye.valid) log_warn("camera: no nominal eye table %s (run cam_calibrate): the camera follows the head eye", path);
    cJSON_Delete(root);
    free(txt);
    return g_eye.valid;
}

bool camera_fp_eye_table_save(const char *path_override) {
    char *out = eye_table_to_json(&g_eye);
    char path[600];
#ifdef DG_SOURCE_DIR
    if (!path_override) snprintf(path, sizeof(path), "%s/%s", DG_SOURCE_DIR, EYE_JSON);
#else
    if (!path_override) platform_data_path(EYE_JSON, path, sizeof(path));
#endif
    if (path_override) snprintf(path, sizeof(path), "%s", path_override);
    FILE *f = fopen(path, "wb");
    bool ok = f != NULL;
    if (f) { fputs(out, f); fputc('\n', f); fclose(f); }
    char rt[600];
    snprintf(rt, sizeof(rt), "%s%s", platform_base_dir(), EYE_JSON);
    if (ok && !path_override && strcmp(rt, path)) {
        FILE *g = fopen(rt, "wb");
        if (g) { fputs(out, g); fputc('\n', g); fclose(g); }
    }
    free(out);
    log_info("camera: %s %s", ok ? "wrote" : "cannot write", path);
    return ok;
}

bool camera_fp_eye_table_save_runtime(void) {
    char rt[600];
    snprintf(rt, sizeof(rt), "%s%s", platform_base_dir(), EYE_JSON);
    return camera_fp_eye_table_save(rt);
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
