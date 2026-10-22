// Weapon sights at runtime (docs/ATTACHMENTS.md): the ADS arms / weapon zoom per sight,
// sensitivity scaling, the sight hand-off to the renderer (red dot / scope lens geometry, reticle,
// picture-in-picture camera, ADS depth of field), the ads_debug overlay and the sight tests.
// ADS narrows the world camera a little (cl_ads_world_zoom / the sight's world_zoom, 1.5x) and the
// arms / weapon projection more (the sight's authored ADS zoom); a scope's magnification exists only
// inside its lens.
//
// Order per frame (game_render.c):
//   sights_camera()  before camera_update: ADS progress -> arms / weapon zoom, sensitivity scale
//   ... camera, world, hands, first-person body (the item lands on its hold point) ...
//   sights_frame()   lens / reticle / PiP from the rendered item -> render_set_sight()
//   sights_draw()    ads_debug overlay (UI)
#include "game_internal.h"
#include "gameplay/attachments.h"
#include "gameplay/characters.h"
#include "gameplay/inventory.h"
#include "gameplay/weapons.h"
#include "player/hands.h"
#include "player/holdpoint.h"
#include "input/input.h"
#include "ui/ui.h"
#include "core/log.h"
#include "core/engine.h"
#include "net/replication.h"
#include "net/net.h"
#include "net/protocol.h"
#include "render/fidelity/fidelity.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define G (g_engine.game)

typedef struct SightTest {
    int mode;                 // 0 off, 1 align, 2 fov
    // align: every weapon x optic (and iron sights), pitch + yaw sweep
    int combo, combo_count;
    u16 items[64];
    u8 optics[64];
    int phase, frames;        // frames: simulation ticks in this phase (frame-rate independent)
    u32 last_tick;
    float base_yaw;
    float max_lat_mm, max_ang, min_d, max_d;
    int samples, fails;
    float worst_lat, worst_ang, worst_spread;
    int total_fail;
    char worst_name[64];
    // fov: ADS enter / exit switch frames
    int fov_stage, fov_wait;
    float prev_t, hip_fov;
    int fov_fail, fov_checks;
    bool fov_bad;
    bool quit;                // quit when done (scripted runs)
    bool keep;                // sweep: test the weapon as equipped (no swap)
} SightTest;

static struct {
    Cvar *pip_scale, *brightness, *ads_debug, *sight_debug, *sens_mode, *sens_coef, *relief_scale, *look_lag;
    Cvar *ads_dof, *dof_focus, *dof_fstop, *iron_blur, *thirdperson;
    // this frame
    const AttachmentDef *optic;
    WeaponDef wd;              // effective equipped weapon (copy)
    bool have_wd;
    float ads_t, base_fov, ads_base_fov, ads_fov, sight_fov, vm_zoom, world_zoom, sens_scale;
    bool sway_on;
    int sway_n[2];
    vec2 sway_min[2], sway_max[2];
    // Reticle vs aim (recoil / sway anchoring): pixels between the projected reticle centre and
    // the shot direction while fully aimed, this frame and worst since sight_test recoil.
    float reticle_err_px, reticle_err_max;
    int reticle_frames;
    int kind;
    bool complete;
    RenderSight rs;
    vec3 sight_world;          // ADS sight socket (viewmodel world space)
    bool sight_valid;
    SightTest t;
} S;

// The scope PiP resolution with optic a (RenderSight.pip_scale, x the render height): r_scope_pip_scale
// x the sight's pip_resolution, bounded by the fidelity level's r_scope_pip_scale range, so no sight
// costs more than the level allows (docs/ATTACHMENTS.md "PiP resolution per sight").
static float sight_pip_scale(const AttachmentDef *a) {
    return fid_clamp_current(FF_SCOPE_PIP_SCALE, S.pip_scale->f * (a ? a->pip_resolution : 1.0f));
}

// ------------------------------------------------------------------ FOV + sensitivity

void sights_camera(const Player *p, const WeaponDef *wd, CameraAds *out) {
    memset(out, 0, sizeof(*out));
    S.base_fov = camera_base_fov();
    S.ads_base_fov = camera_ads_base_fov();
    holdpoint_set_ads_relief_scale(S.relief_scale ? S.relief_scale->f : 1.0f);
    holdpoint_set_look_lag_scale(S.look_lag ? S.look_lag->f : 1.0f);
    S.have_wd = wd != NULL;
    if (wd) S.wd = *wd;
    bool gun = wd && wd->kind != WPN_MELEE;
    S.optic = gun ? attach_optic(&G->items, wd->att_applied) : NULL;
    S.ads_t = (p && gun) ? game_ads_vis() : 0.0f;
    bool scope = S.optic && S.optic->sight == SIGHT_SCOPE;
    S.kind = !S.optic ? RSIGHT_NONE : (scope ? RSIGHT_SCOPE_PIP : RSIGHT_REDDOT);
    S.complete = gun && S.ads_t >= 1.0f;
    S.vm_zoom = gun ? attach_ads_vm_zoom(wd, S.optic) : 1.0f;
    S.world_zoom = gun ? attach_ads_world_zoom(wd, S.optic, camera_ads_world_zoom_default()) : 1.0f;
    // The aimed world FOV (ADS Field of View, docs/CAMERA_HOLDPOINT.md "Field of view") and the sight's
    // FOV for the sensitivity, from the resulting FOVs: a scope's magnification (inside the lens,
    // against the ADS base), irons / red dots the aimed world FOV.
    S.ads_fov = gun ? camera_ads_world_fov(S.world_zoom) : S.base_fov;
    S.sight_fov = !gun ? S.base_fov : (scope ? attach_sight_fov(S.optic, S.ads_base_fov) : S.ads_fov);
    if (gun) {
        out->vm_zoom = S.vm_zoom;
        out->world_zoom = S.world_zoom;
        out->ads_fov = S.ads_fov;
        out->t = S.ads_t;
    }
    S.sens_scale = gun ? fov_sens_scale(S.sens_mode->str, S.sens_coef->f, S.sight_fov, S.base_fov) : 1.0f;
    input_set_ads_fov_scale(S.sens_scale);
}

// ------------------------------------------------------------------ renderer hand-off

static vec2 g_dot_px, g_axis_deg;

static void cmd_sight_dot(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    log_info("sight_dot: %s dot %.2f %.2f px from the centre, gun axis %.3f %.3f deg off the view", S.complete ? "aimed" : "not aimed", g_dot_px.x, g_dot_px.y, g_axis_deg.x, g_axis_deg.y);
}

void sights_frame(const RenderCamera *cam, bool first_person) {
    RenderSight *rs = &S.rs;
    memset(rs, 0, sizeof(*rs));
    rs->ads_t = S.ads_t;
    rs->complete = S.complete;
    rs->base_fov = S.ads_base_fov;
    rs->sight_fov = S.sight_fov;
    rs->debug = S.sight_debug->i;
    S.sight_valid = false;
    vec3 pos;
    quat rot;
    const WeaponDef *wd = NULL;
    bool have = first_person && hands_fp_item(&pos, &rot, &wd) && wd && wd->kind != WPN_MELEE;
    if (have) {
        Xform item = xf_make(pos, rot);
        S.sight_world = xf_point(item, wd->sockets.sight);
        if (S.sway_on && cam) {
            // sight_test sway: the gun body's direction against the view (deg), hip vs fully aimed.
            vec3 gf = quat_rotate(rot, v3(0, 0, -1));
            vec3 cf = dir_from_angles(cam->yaw, cam->pitch), cr = right_from_yaw(cam->yaw), cu = v3_cross(cr, cf);
            float yaw_d = atan2f(v3_dot(gf, cr), v3_dot(gf, cf)) * DG_RAD2DEG, pit_d = atan2f(v3_dot(gf, cu), v3_dot(gf, cf)) * DG_RAD2DEG;
            int k = S.complete ? 1 : (S.ads_t <= 0.0f ? 0 : -1);
            if (k >= 0) {
                if (!S.sway_n[k]) { S.sway_min[k] = S.sway_max[k] = v2(yaw_d, pit_d); }
                S.sway_min[k] = v2(fminf(S.sway_min[k].x, yaw_d), fminf(S.sway_min[k].y, pit_d));
                S.sway_max[k] = v2(fmaxf(S.sway_max[k].x, yaw_d), fmaxf(S.sway_max[k].y, pit_d));
                S.sway_n[k]++;
            }
        }
        S.sight_valid = true;
        Player *lp = game_local_player();
        Xform icm;
        int hst = -1;
        float hbl = 0;
        bool held = chars_fp_item_cam(&icm, &hst, &hbl);
        // Fully aimed means the hold point has finished blending into ADS too (it trails ads_t).
        if (S.complete && lp && cam && held && hst == HP_ADS && hbl >= 0.999f) {
            // Reticle centre as seen (viewmodel -> world mapping) vs the shot direction.
            vec3 sw = render_viewmodel_to_world(chars_fp_sight_world());
            vec3 to = v3_norm(v3_sub(sw, cam->pos));
            vec2 off = weapon_aim_offset_deg(wd, &lp->sim.wpn, &lp->sim.move, weapon_sway_time(G->last_cmd.seq) + G->alpha * SIM_DT);
            vec3 aim = dir_from_angles(G->view.yaw - off.x * DG_DEG2RAD, G->view.pitch + off.y * DG_DEG2RAD);
            float ang = acosf(clampf(v3_dot(to, aim), -1.0f, 1.0f));
            float px = tanf(ang) * (float)render_height() * 0.5f / tanf(cam->fov_deg * 0.5f * DG_DEG2RAD);
            S.reticle_err_px = px;

            if (px > S.reticle_err_max) S.reticle_err_max = px;
            S.reticle_frames++;
        }
        const AttachmentDef *a = attach_optic(&G->items, wd->att_applied);
        if (a) {
            Xform opt = xf_mul(item, attach_placement(wd, a));
            rs->kind = S.kind;
            // The optic as the arms layer draws it (the body's item stays at the hip: ADS is only in
            // the arms layer): the lens through the arms camera, for the arms projection the sight
            // shaders hit it with.
            rs->lens_pos = render_arms_point(xf_point(opt, a->lens_pos));
            rs->lens_radius = a->lens_radius;
            rs->objective_pos = render_viewmodel_to_world(xf_point(opt, a->objective_pos));
            rs->objective_radius = a->objective_radius;
            rs->fwd = render_arms_dir(xf_dir(opt, v3(0, 0, -1)));
            rs->up = render_arms_dir(xf_dir(opt, v3(0, 1, 0)));
            rs->right = render_arms_dir(xf_dir(opt, v3(1, 0, 0)));
            rs->axis_fwd = rs->fwd;
            rs->axis_up = rs->up;
            rs->axis_right = rs->right;
            if (cam && S.complete) {
                // Fully aimed: the optic looks along the eye -> sight line (an ideal optic). The gun
                // turning about the reticle centre under recoil then never moves the scope picture or
                // the dot off where the shots go.
                vec3 f = v3_norm(v3_sub(render_viewmodel_to_world(chars_fp_sight_world()), cam->pos));
                if (v3_dot(f, rs->fwd) > 0.9f) {
                    rs->fwd = f;
                    rs->right = v3_norm(v3_cross(f, rs->up));
                    rs->up = v3_cross(rs->right, f);
                }
            }
            // Collimated dot (the parallax look): aimed, it stays near the aim line (the screen centre)
            // while the housing sways and kicks around it - only DOT_FOLLOW of the gun's own axis (the
            // sway, the kick) reaches it, a small reaction; before fully aimed it rides the gun's axis.
            // Both at the parallax-free distance (the optic point through the viewmodel -> world mapping).
            {
                const float DOT_FOLLOW = 0.12f;
                float dist = fmaxf(a->parallax_m, 1.0f);
                vec3 on_axis = render_viewmodel_to_world(v3_madd(xf_point(opt, a->lens_pos), xf_dir(opt, v3(0, 0, -1)), dist));
                if (cam && S.ads_t > 0.0f) {
                    vec3 on_aim = v3_madd(cam->pos, dir_from_angles(cam->yaw, cam->pitch), dist);
                    float k = 1.0f - (1.0f - DOT_FOLLOW) * saturate(S.ads_t);
                    rs->dot_point = v3_lerp(on_aim, on_axis, k);
                } else {
                    rs->dot_point = on_axis;
                }
            }
            if (cam) {   // sight_dot: where the dot is against the screen centre
                vec3 cf = dir_from_angles(cam->yaw, cam->pitch), cr = right_from_yaw(cam->yaw), cu = v3_cross(cr, cf);
                vec3 d = v3_norm(v3_sub(rs->dot_point, cam->pos)), g = rs->axis_fwd;
                float k = (float)render_height() * 0.5f / tanf(cam->fov_deg * 0.5f * DG_DEG2RAD);
                g_dot_px = v2(v3_dot(d, cr) / v3_dot(d, cf) * k, v3_dot(d, cu) / v3_dot(d, cf) * k);
                g_axis_deg = v2(atan2f(v3_dot(g, cr), v3_dot(g, cf)) * DG_RAD2DEG, atan2f(v3_dot(g, cu), v3_dot(g, cf)) * DG_RAD2DEG);
            }
            rs->reticle_moa = a->reticle.moa;
            rs->reticle_outer_moa = a->reticle.outer_moa;
            rs->reticle_shape = a->reticle.shape;
            rs->reticle_color = a->reticle.color;
            rs->brightness = a->reticle.brightness * S.brightness->f;
            rs->ffp = a->reticle.ffp;
            rs->illuminated = a->reticle.illuminated;
            rs->tint = a->glass_tint;
            rs->dust = a->dust;
            rs->fresnel = a->fresnel;
            rs->chromatic = a->chromatic;
            rs->power_mrad = a->power_mrad;
            rs->edge_mrad = a->edge_mrad;
            rs->bevel = a->bevel;
            rs->dispersion = a->dispersion;
            rs->coating = a->coating;
            rs->glow = a->glow;
            rs->glow_scatter = a->glow_scatter;
            rs->zoom = a->zoom;
            rs->eye_box = a->eye_box;
            rs->eye_box_depth = a->eye_box_depth;
            rs->eye_relief = holdpoint_ads_relief(a->eye_relief > 0 ? a->eye_relief : 0.1f);
            rs->pip_scale = sight_pip_scale(a);
            rs->dof_near_blur = a->dof_near_blur;
        } else {
            rs->dof_near_blur = S.iron_blur->f;
        }
    }
    // ADS depth of field: on exactly while the aim is complete; the user's mode applies otherwise. Its
    // sharp point: the weapon's front (muzzle) as the arms camera draws it, its depth along the view.
    rs->ads_dof = S.ads_dof->i && S.complete && have;
    if (rs->ads_dof && cam) {
        vec3 mz = render_arms_point(xf_point(xf_make(pos, rot), wd->sockets.muzzle));
        rs->dof_vm_focus = -mat4_mul_point(mat4_fps_view(cam->pos, cam->yaw, cam->pitch, cam->roll), mz).z;
    }
    rs->dof_focus = S.dof_focus->f;
    rs->dof_fstop = S.dof_fstop->f;
    (void)cam;
    render_set_sight(rs);
}

// ------------------------------------------------------------------ ads_debug overlay

static bool to_ui(vec3 world_vm, vec2 *out) {
    vec2 px;
    if (!render_world_to_screen(render_viewmodel_to_world(world_vm), &px)) return false;
    out->x = px.x * ui_width() / (float)DG_MAX(1, render_width());
    out->y = px.y * ui_height() / (float)DG_MAX(1, render_height());
    return true;
}

void sights_draw(void) {
    if (!S.ads_debug->i) return;
    float cx = ui_width() * 0.5f, cy = ui_height() * 0.5f, s = ui_scale();
    u32 white = UI_RGBA(255, 255, 255, 220), green = UI_RGBA(60, 255, 90, 255), red = UI_RGBA(255, 70, 50, 255);
    ui_line(cx - 14 * s, cy, cx - 3 * s, cy, 1.0f, white);
    ui_line(cx + 3 * s, cy, cx + 14 * s, cy, 1.0f, white);
    ui_line(cx, cy - 14 * s, cx, cy - 3 * s, 1.0f, white);
    ui_line(cx, cy + 3 * s, cx, cy + 14 * s, 1.0f, white);
    char line[256];
    float y = cy + 40 * s;
    if (S.sight_valid) {
        vec2 sp;
        if (to_ui(S.sight_world, &sp)) {
            float e = v2_len(v2(sp.x - cx, sp.y - cy)) * (float)render_width() / ui_width();
            u32 c = (e <= 0.5f) ? green : red;
            ui_rect_outline(sp.x - 4 * s, sp.y - 4 * s, 8 * s, 8 * s, 1.0f, c);
            float ap = 0, aa = 0, gp[2], ga[2];
            chars_fp_diag(gp, ga, &ap, &aa);
            vec3 sc = chars_fp_sight_cam();
            snprintf(line, sizeof(line), "sight %.2f px from centre  |  %.3f mm off axis  %.3f deg  |  eye relief %.1f mm  |  ADS %.2f%s", e, ap * 1000.0f, aa,
                     -sc.z * 1000.0f, S.ads_t, S.complete ? " (complete)" : "");
            ui_text_shadow(cx, y, FONT_SMALL, c, UI_ALIGN_CENTER, line);
            y += 16 * s;
        }
    }
    if (S.rs.kind == RSIGHT_REDDOT) {
        vec2 dp;
        vec2 px;
        if (render_world_to_screen(S.rs.dot_point, &px)) {
            dp.x = px.x * ui_width() / (float)DG_MAX(1, render_width());
            dp.y = px.y * ui_height() / (float)DG_MAX(1, render_height());
            ui_line(dp.x - 5 * s, dp.y - 5 * s, dp.x + 5 * s, dp.y + 5 * s, 1.0f, UI_RGBA(255, 200, 40, 255));
            ui_line(dp.x - 5 * s, dp.y + 5 * s, dp.x + 5 * s, dp.y - 5 * s, 1.0f, UI_RGBA(255, 200, 40, 255));
        }
    }
    const char *kinds[] = { "iron", "red dot", "scope (PiP)" };
    float pip_screen = 0.0f;
    u32 pip_px = render_scope_pip_size(&pip_screen);
    pip_screen *= (float)render_height();
    snprintf(line, sizeof(line), "%s %s  |  world FOV %.2f  hands FOV %.2f (x%.2f aimed)  sight %.2fx (FOV %.2f)  PiP FOV %.2f, %u px over %.0f  |  sens x%.3f (%s %.2f)  |  DoF %s",
             S.have_wd ? S.wd.id : "-", kinds[DG_CLAMP(S.kind, 0, 2)], G->view.fov, render_vm_fov(), S.vm_zoom, attach_sight_zoom(S.optic), S.sight_fov,
             render_scope_pip_fov(), pip_px, pip_screen, S.sens_scale, S.sens_mode->str, S.sens_coef->f, S.rs.ads_dof ? "ADS" : "user");
    ui_text_shadow(cx, y, FONT_SMALL, white, UI_ALIGN_CENTER, line);
}

// ------------------------------------------------------------------ tests

static void put_weapon(Player *p, u16 item, u8 optic) {
    InvSlot *s = inv_dev_sel(&p->inv, &G->items, 0);
    const WeaponDef *wd = item_weapon(&G->items, item);
    memset(s, 0, sizeof(*s));
    s->item = item;
    s->count = 1;
    s->ammo = wd ? (i16)wd->mag_size : 0;
    s->att[ATT_OPTIC] = optic;
    p->inv.revision++;
    game_select(0);
}

static void align_next(void) {
    SightTest *t = &S.t;
    Player *p = game_local_player();
    if (!p) { t->mode = 0; return; }
    if (t->combo >= t->combo_count) {
        log_info("sight_test align: %d combos, worst off-axis %.3f mm, worst angle %.4f deg, worst eye-relief spread %.3f mm (%s) -> %s", t->combo_count,
                 t->worst_lat, t->worst_ang, t->worst_spread, t->worst_name, t->total_fail ? "FAIL" : "PASS");
        t->mode = 0;
        if (t->quit) cbuf_add("quit");
        return;
    }
    if (!t->keep) put_weapon(p, t->items[t->combo], t->optics[t->combo]);
    t->phase = 0;
    t->frames = 0;
    t->last_tick = G->world ? G->world->tick : 0;
    t->max_lat_mm = t->max_ang = 0;
    t->min_d = 1e9f;
    t->max_d = -1e9f;
    t->samples = 0;
    t->fails = 0;
    G->view.pitch = 0;
}

static void cmd_sight_test(int argc, const char **argv) {
    SightTest *t = &S.t;
    const char *what = argc > 1 ? argv[1] : "";
    if (!strcmp(what, "recoil")) {
        S.reticle_err_max = 0;
        S.reticle_frames = 0;
        log_info("sight_test recoil: measuring the reticle centre against the aim while fully aimed");
        return;
    }
    if (!strcmp(what, "sway")) {
        S.sway_on = true;
        memset(S.sway_n, 0, sizeof(S.sway_n));
        log_info("sight_test sway: measuring the gun's sway range at the hip and fully aimed");
        return;
    }
    if (!strcmp(what, "sway_report")) {
        static const char *nm[2] = { "hip", "ADS" };
        for (int k = 0; k < 2; k++)
            log_info("sight_test sway: %s %d frames, gun direction range yaw %.3f deg, pitch %.3f deg", nm[k], S.sway_n[k],
                     S.sway_n[k] ? S.sway_max[k].x - S.sway_min[k].x : 0.0f, S.sway_n[k] ? S.sway_max[k].y - S.sway_min[k].y : 0.0f);
        {
            Player *lp = game_local_player();
            if (lp) log_info("sight_test sway: breath holding %d (used %.2f s), sprinting %d", lp->sim.wpn.breath_holding, lp->sim.wpn.breath, lp->sim.move.sprinting);
        }
        S.sway_on = false;
        return;
    }
    if (!strcmp(what, "recoil_report")) {
        log_info("sight_test recoil: %d aimed frames, worst reticle-to-aim error %.2f px -> %s", S.reticle_frames, S.reticle_err_max,
                 S.reticle_frames > 10 && S.reticle_err_max <= 1.0f ? "PASS" : "FAIL");
        return;
    }
    if (!strcmp(what, "align")) {
        memset(t, 0, sizeof(*t));
        // Every gun x (iron sights + every optic it accepts).
        for (int w = 0; w < G->items.weapon_count; w++) {
            const WeaponDef *wd = &G->items.weapons[w];
            if (wd->kind == WPN_MELEE) continue;
            u16 item = 0;
            for (int i = 1; i < G->items.item_count; i++)
                if (G->items.items[i].weapon == w) item = (u16)i;
            if (!item) continue;
            if (argc > 2 && strcmp(argv[2], "all") && strcmp(argv[2], wd->id)) continue;
            for (int o = 0; o <= G->items.attachment_count && t->combo_count < 64; o++) {
                const AttachmentDef *a = attach_def(&G->items, (u8)o);
                if (o && (!a || a->slot != ATT_OPTIC || !attach_allowed(wd, a))) continue;
                t->items[t->combo_count] = item;
                t->optics[t->combo_count] = (u8)o;
                t->combo_count++;
            }
        }
        t->mode = 1;
        t->quit = argc > 3 && !strcmp(argv[3], "quit");
        align_next();
        log_info("sight_test align: %d weapon x optic combos", t->combo_count);
        return;
    }
    if (!strcmp(what, "sweep")) {
        // The equipped weapon with its current optic: pitch -89..89 + 360 deg yaw sweep.
        Player *p = game_local_player();
        const InvSlot *eq = p ? weapon_equipped(G->world, G->local_slot) : NULL;
        if (!eq || !item_weapon(&G->items, eq->item)) { log_info("sight_test sweep: equip a gun"); return; }
        memset(t, 0, sizeof(*t));
        t->items[0] = eq->item;
        t->optics[0] = eq->att[ATT_OPTIC];
        t->combo_count = 1;
        t->keep = true;
        t->mode = 1;
        align_next();
        log_info("sight_test sweep: hold +aim; sweeping pitch then yaw");
        return;
    }
    if (!strcmp(what, "fov")) {
        memset(t, 0, sizeof(*t));
        t->mode = 2;
        log_info("sight_test fov: aim with +aim, the test logs the frames where ADS completes / ends");
        return;
    }
    if (!strcmp(what, "parallax")) {
        // Red dot parallax (CPU, the sight shader's model): the dot is collimated onto the optic
        // axis at parallax_m; an eye 2 cm off axis must still see it on a 50 m target within 1 MOA.
        u8 id = attach_find(&G->items, "red_dot");
        const AttachmentDef *a = attach_def(&G->items, id);
        if (!a) { log_warn("sight_test parallax: no red_dot"); return; }
        vec3 lens = v3(0, 0, -0.3f), fwd = v3(0, 0, -1);
        vec3 dot = v3_madd(lens, fwd, a->parallax_m);
        float worst = 0;
        for (int k = 0; k < 8; k++) {
            float ang = (float)k / 8.0f * (2.0f * DG_PI);
            vec3 eye = v3(cosf(ang) * 0.02f, sinf(ang) * 0.02f, 0);
            vec3 ray = v3_norm(v3_sub(dot, eye));
            // Hit on the 50 m target plane (z = -50.3) vs the bore line's point there.
            float tz = (-50.3f - eye.z) / ray.z;
            vec3 hit = v3_madd(eye, ray, tz);
            float err_m = sqrtf(hit.x * hit.x + hit.y * hit.y);
            float moa = atanf(err_m / 50.0f) * DG_RAD2DEG * 60.0f;
            worst = fmaxf(worst, moa);
        }
        log_info("sight_test parallax: eye 2 cm off axis, dot on the 50 m target within %.4f MOA (limit 1) -> %s", worst, worst <= 1.0f ? "PASS" : "FAIL");
        return;
    }
    if (!strcmp(what, "aspect")) {
        // The sight socket (camera space) through 16:9, 21:9 and 4:3 projections: the exact
        // screen centre (the construction puts it on the view axis).
        vec3 sc = chars_fp_sight_cam();
        const float aspects[3] = { 16.0f / 9.0f, 21.0f / 9.0f, 4.0f / 3.0f };
        const char *names[3] = { "16:9", "21:9", "4:3" };
        int fails = 0;
        for (int i = 0; i < 3; i++) {
            mat4 pr = mat4_perspective(render_vm_fov() * DG_DEG2RAD, aspects[i], 0.04f, 1500.0f);   // the weapon projection
            vec4 c = mat4_mul_v4(pr, v4(sc.x, sc.y, sc.z, 1));
            float h = 1080.0f, w = h * aspects[i];
            float ex = c.x / c.w * 0.5f * w, ey = c.y / c.w * 0.5f * h;
            float e = sqrtf(ex * ex + ey * ey);
            if (e > 0.5f) fails++;
            log_info("sight_test aspect %s: sight %.3f px from the centre at %dx%d", names[i], e, (int)w, (int)h);
        }
        log_info("sight_test aspect: %s (needs ADS complete)", fails || S.ads_t < 1.0f ? "FAIL" : "PASS");
        return;
    }
    if (!strcmp(what, "net")) {
        // Attachment replication round trip: full + delta snapshots through the real encoder.
        static NetEnt a_ents[2], b_ents[2], c_ents[2];
        SnapFrame base = { 1, 1, a_ents }, cur = { 2, 1, b_ents }, out = { 0, 0, c_ents };
        memset(a_ents, 0, sizeof(a_ents));
        a_ents[0].id = ENT_MAKE(5, 1);
        a_ents[0].type = 1;
        a_ents[0].rot = quat_identity();
        a_ents[0].item = 3;
        a_ents[0].attach[ATT_OPTIC] = 2;
        b_ents[0] = a_ents[0];
        b_ents[0].attach[ATT_MUZZLE] = 4;
        b_ents[0].attach[ATT_OPTIC] = 3;
        u8 buf[512];
        int fails = 0;
        // Full (no baseline) then delta.
        Writer w = writer_make(buf, sizeof(buf));
        repl_write_delta(&w, NULL, &base);
        Reader r = reader_make(buf, w.len);
        bool ok = repl_read_delta(&r, NULL, &out) && out.count == 1 && !memcmp(out.ents[0].attach, base.ents[0].attach, 5);
        if (!ok) fails++;
        u32 full_len = w.len;
        static NetEnt d_ents[2];
        SnapFrame got_base = { 1, out.count, d_ents };
        memcpy(d_ents, c_ents, sizeof(d_ents));
        w = writer_make(buf, sizeof(buf));
        repl_write_delta(&w, &base, &cur);
        r = reader_make(buf, w.len);
        ok = repl_read_delta(&r, &got_base, &out) && out.count == 1 && !memcmp(out.ents[0].attach, cur.ents[0].attach, 5);
        if (!ok) fails++;
        u32 delta_len = w.len;
        // Unchanged attachments: no NF_ATTACH bit in the entity's field mask.
        w = writer_make(buf, sizeof(buf));
        repl_write_delta(&w, &cur, &cur);
        r = reader_make(buf, w.len);
        u16 changes = r_u16(&r);
        u32 mask = 0;
        if (changes) { (void)r_u32(&r); mask = r_u16(&r); }
        if (mask & NF_ATTACH) fails++;
        // The delta carries only the changed slots: 1 mask byte + 2 ids.
        log_info("sight_test net: full %u bytes, 2-slot attachment delta %u bytes, unchanged attachments: NF_ATTACH %s -> %s", full_len, delta_len,
                 (mask & NF_ATTACH) ? "SENT" : "not sent", fails ? "FAIL" : "PASS");
        return;
    }
    if (!strcmp(what, "dump")) {
        // Replicated attachment ids of every player's held weapon on this machine (LAN tests).
        World *w = G->world;
        if (!w) return;
        for (int sl = 0; sl < MAX_PLAYERS; sl++) {
            const Player *p = &w->players[sl];
            if (!p->active) continue;
            int i = (int)ENT_INDEX(p->ent);
            char ids[160] = "";
            for (int k = 0; k < ATT_SLOTS; k++) {
                const AttachmentDef *a = attach_def(&G->items, w->attach[i][k]);
                char one[40];
                snprintf(one, sizeof(one), " %s=%s", attach_slot_name(k), a ? a->id : "-");
                strncat(ids, one, sizeof(ids) - strlen(ids) - 1);
            }
            log_info("sight_test dump: %s slot %d %s item %s |%s", net_role() == NET_CLIENT ? "client" : "host", sl, p->local ? "(local)" : "(remote)",
                     w->item[i] ? item_def(&G->items, w->item[i])->id : "-", ids);
        }
        log_info("sight_test dump: base FOV %.2f, ADS %.2f, camera FOV now %.2f, hands FOV now %.2f (aimed zoom x%.3f), sight %.2fx (FOV %.2f)", S.base_fov, S.ads_t, G->view.fov,
                 render_vm_fov(), S.vm_zoom, attach_sight_zoom(S.optic), S.sight_fov);
        return;
    }
    log_info("usage: sight_test align [weapon] | fov | parallax | aspect | net | dump");
}

static void align_frame(void) {
    SightTest *t = &S.t;
    Player *p = game_local_player();
    if (!p || !G->world) { t->mode = 0; return; }
    // Counted in simulation ticks, not frames: the equip / ADS / hold point blend and the tick's spine
    // take the same time at any frame rate (uncapped, 600 frames were less than one equip + ADS).
    int steps = (int)(G->world->tick - t->last_tick);
    t->last_tick = G->world->tick;
    if (steps <= 0) {
        if (t->phase == 0) return;
        steps = 0;
    }
    t->frames += steps;
    // Settle: equip, aim fully, the hold point blend done.
    Xform ic;
    int st;
    float bl;
    bool aimed = p->sim.wpn.ads_t >= 1.0f && chars_fp_item_cam(&ic, &st, &bl) && st == HP_ADS && bl >= 0.999f;
    if (t->phase == 0) {
        if (aimed && t->frames > 20) { t->phase = 1; t->frames = 0; G->view.pitch = -89.0f * DG_DEG2RAD; t->base_yaw = G->view.yaw; }
        else if (t->frames > 600) { log_warn("sight_test align: %s never aimed", item_def(&G->items, t->items[t->combo])->id); t->total_fail++; t->combo++; align_next(); }
        return;
    }
    // Sweep: pitch -89 -> 89 (1 deg / tick) then yaw 360 deg (4 deg / tick) at pitch 0. The
    // first ticks after each jump let the tick's spine catch up (as a real mouse would).
    if (t->phase == 1) {
        if (t->frames > 15) G->view.pitch = fminf(G->view.pitch + (float)steps * DG_DEG2RAD, 89.0f * DG_DEG2RAD);
        if (G->view.pitch >= 89.0f * DG_DEG2RAD - 1e-4f && t->frames > 200) { t->phase = 2; t->frames = 0; G->view.pitch = 0; }
    } else if (t->phase == 2) {
        if (t->frames > 15) G->view.yaw = wrap_angle(G->view.yaw + 4.0f * (float)steps * DG_DEG2RAD);
        if (t->frames > 15 + 90) t->phase = 3;
    }
    if (t->frames > 15 && aimed) {
        float gp[2], ga[2], ap, aa;
        chars_fp_diag(gp, ga, &ap, &aa);
        vec3 sc = chars_fp_sight_cam();
        float d = -sc.z;
        t->samples++;
        t->max_lat_mm = fmaxf(t->max_lat_mm, ap * 1000.0f);
        t->max_ang = fmaxf(t->max_ang, aa);
        t->min_d = fminf(t->min_d, d);
        t->max_d = fmaxf(t->max_d, d);
        if (ap * 1000.0f > 0.5f || aa > 0.1f) {
            if (t->fails++ < 3) log_info("sight_test align:   pitch %.0f yaw %.0f off axis %.3f mm %.4f deg", G->view.pitch * DG_RAD2DEG, G->view.yaw * DG_RAD2DEG, ap * 1000, aa);
        }
    }
    if (t->phase == 3) {
        const AttachmentDef *a = attach_def(&G->items, t->optics[t->combo]);
        float spread = (t->max_d - t->min_d) * 1000.0f;
        bool fail = t->fails > 0 || spread > 0.5f || t->samples < 100;
        char name[64];
        snprintf(name, sizeof(name), "%s + %s", item_def(&G->items, t->items[t->combo])->id, a ? a->id : "iron");
        log_info("sight_test align: %-24s %4d frames  off axis max %.3f mm  angle max %.4f deg  eye relief %.1f mm (spread %.3f mm) -> %s", name, t->samples,
                 t->max_lat_mm, t->max_ang, t->min_d * 1000.0f, spread, fail ? "FAIL" : "ok");
        if (t->max_lat_mm > t->worst_lat || t->max_ang > t->worst_ang || spread > t->worst_spread) dg_strcpy(t->worst_name, sizeof(t->worst_name), name);
        t->worst_lat = fmaxf(t->worst_lat, t->max_lat_mm);
        t->worst_ang = fmaxf(t->worst_ang, t->max_ang);
        t->worst_spread = fmaxf(t->worst_spread, spread);
        if (fail) t->total_fail++;
        G->view.yaw = t->base_yaw;
        t->combo++;
        align_next();
    }
}

// FOV / DoF switch: while aiming the world FOV is exactly the hip FOV blended to the aimed world FOV
// (ADS Field of View) along smoothstep(ads_t); the frame ADS completes must already show the aimed world FOV, the
// aimed hands / weapon FOV and the ADS DoF; the first frame it is incomplete again must show none of
// them (the user's DoF mode is back).
static void fov_frame(const RenderCamera *cam) {
    SightTest *t = &S.t;
    float vm = render_vm_fov();
    RenderCamera hip_cam = *cam;
    hip_cam.ads_t = 0;
    hip_cam.vm_fov_deg = game_arms_hip_fov();
    float vm_hip = render_vm_fov_for(&hip_cam, 0.0f);
    float vm_target = S.vm_zoom > 1.0f ? 2.0f * atanf(tanf(RENDER_ADS_REF_FOV * 0.5f * DG_DEG2RAD) / S.vm_zoom) * DG_RAD2DEG : vm_hip;   // absolute (render_vm_fov_for)
    bool complete = S.complete;
    bool was = t->prev_t >= 1.0f;
    float hip = cam->vm_fov_deg > 1.0f ? cam->vm_fov_deg : cam->fov_deg;   // the hip FOV (sprint included)
    if (S.ads_t <= 0.0f || t->hip_fov <= 1.0f) t->hip_fov = hip;   // (also when the test starts mid-aim)
    float world_now = fov_ads_blend(t->hip_fov, S.ads_fov, S.ads_t), world_aimed = fov_ads_blend(t->hip_fov, S.ads_fov, 1.0f);
    if (S.ads_t > 0.0f && fabsf(cam->fov_deg - world_now) > 1e-3f) {
        if (!t->fov_bad) {
            t->fov_checks++;
            t->fov_fail++;
            log_info("sight_test fov: world FOV %.3f while aiming at %.3f (expected %.3f, hip %.3f) -> FAIL", cam->fov_deg, S.ads_t, world_now, t->hip_fov);
        }
        t->fov_bad = true;   // once per run of bad frames
    } else t->fov_bad = false;
    if (complete && !was) {
        bool ok = fabsf(vm - vm_target) < 1e-3f && S.rs.ads_dof && fabsf(cam->fov_deg - world_aimed) < 1e-3f;
        t->fov_checks++;
        if (!ok) t->fov_fail++;
        log_info("sight_test fov: ADS complete -> world FOV %.3f (hip %.3f, world zoom %.3f: target %.3f), hands FOV %.3f (target %.3f) ADS DoF %d -> %s", cam->fov_deg,
                 t->hip_fov, S.world_zoom, world_aimed, vm, vm_target, S.rs.ads_dof, ok ? "ok" : "FAIL");
        log_info("sight_test fov: sensitivity x%.4f (%s, sight FOV %.3f, hip %.3f)", S.sens_scale, S.sens_mode->str, S.sight_fov, S.base_fov);
    } else if (!complete && was) {
        bool ok = !S.rs.ads_dof && (S.vm_zoom <= 1.0f || fabsf(vm - vm_target) > 1e-5f || fabsf(vm_hip - vm_target) < 1e-3f)   /* off the aimed FOV (unless hip = aimed) */ && fabsf(cam->fov_deg - world_now) < 1e-3f;   // back on the ADS curve below 1 (flat at the top: no fixed margin)
        t->fov_checks++;
        if (!ok) t->fov_fail++;
        log_info("sight_test fov: ADS left -> world FOV %.3f, hands FOV %.3f ADS DoF %d (user mode restored) -> %s", cam->fov_deg, vm, S.rs.ads_dof, ok ? "ok" : "FAIL");
    }
    t->prev_t = S.ads_t;
}

// fov_report: the world FOV chain this frame (tests: tools/tests/fov_setting.cfg).
static void cmd_fov_report(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    Cvar *sv = cvar_find("sv_fov"), *svm = cvar_find("sv_fov_max");
    FovPrefs fp = camera_fov_prefs();
    static const char *const ads[] = { "independent", "affected" }, *const wf[] = { "narrow", "default", "wide" };
    int w = render_width(), h = render_height();
    float aspect = h > 0 ? (float)w / (float)h : FOV_REF_ASPECT;
    log_info("fov_report: fov %.1f (horizontal at 16:9) ads %s weapon %s sv_fov %.1f sv_fov_max %.1f -> base %.2f (vertical deg, %.2f horizontal at %.3f), ADS base %.2f, ADS %.2f "
             "(world zoom %.3f), camera now %.2f, hands now %.2f (hip %.2f), sensitivity x%.4f",
             fp.fov, ads[DG_CLAMP(fp.ads_mode, 0, 1)], wf[DG_CLAMP(fp.weapon_fov, 0, 2)], sv ? sv->f : 0.0f, svm ? svm->f : 0.0f, camera_base_fov(),
             fov_v_to_h(camera_base_fov(), aspect), aspect, camera_ads_base_fov(), camera_ads_world_fov(S.world_zoom > 1.0f ? S.world_zoom : 1.0f), S.world_zoom,
             G->view.fov, render_vm_fov(), game_arms_hip_fov(), S.sens_scale);
}

// sight_pip_res [optic multiplier]: every scope's pip_resolution and the PiP size it gets at the current
// render height and fidelity level; with a value, sets that optic's multiplier live (dev tuning, not
// saved: copy it into assets/attachments/<optic>.json).
static void cmd_sight_pip_res(int argc, const char **argv) {
    if (argc == 2 || argc > 3) { log_info("usage: sight_pip_res [optic multiplier]"); return; }
    if (argc == 3) {
        u8 id = attach_find(&G->items, argv[1]);
        if (!id || G->items.attachments[id].sight != SIGHT_SCOPE) { log_warn("sight_pip_res: '%s' is not a scope", argv[1]); return; }
        G->items.attachments[id].pip_resolution = clampf((float)atof(argv[2]), 0.1f, 4.0f);
    }
    FidBounds b = fid_bounds(fid_level(), fid_era(), FF_SCOPE_PIP_SCALE);
    log_info("sight_pip_res: r_scope_pip_scale %.2f, fidelity %s allows %.2f .. %.2f", (double)S.pip_scale->f, fid_level_name(fid_level()), (double)b.lo, (double)b.hi);
    for (int i = 1; i <= G->items.attachment_count; i++) {
        const AttachmentDef *a = &G->items.attachments[i];
        if (a->sight != SIGHT_SCOPE) continue;
        float s = sight_pip_scale(a);
        u32 n = render_scope_pip_px(s);
        log_info("  %-12s pip_resolution %.2f -> %.3f x the render height -> PiP %ux%u (%.2f Mpx)%s", a->id, (double)a->pip_resolution, (double)s, n, n,
                 (double)n * n * 1e-6, a == S.optic ? "  (mounted)" : "");
    }
    float frac = 0.0f;
    u32 now = render_scope_pip_size(&frac);
    float screen = frac * (float)render_height();
    if (now) log_info("  now: PiP %u px over %.0f px of the screen (%.0f %% of its height; %.2f texels per pixel)", now, (double)screen, (double)(frac * 100.0f),
                      (double)((float)now / fmaxf(screen, 1.0f)));
}

static void cmd_sight_test_report(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    log_info("sight_test fov: %d switches checked, %d failed -> %s", S.t.fov_checks, S.t.fov_fail, S.t.fov_checks >= 2 && !S.t.fov_fail ? "PASS" : "FAIL");
    S.t.mode = 0;
}

void sights_test_frame(const RenderCamera *cam) {
    if (S.t.mode == 1) align_frame();
    else if (S.t.mode == 2) fov_frame(cam);
}

bool sights_test_running(void) { return S.t.mode == 1; }
bool sights_dof_managed(void) { return S.ads_dof && S.ads_dof->i; }

// ------------------------------------------------------------------ registration

void sights_register(void) {
    S.look_lag = cvar_reg_float("wpn_look_lag", 0.5f, 0.0f, 2.0f, CVAR_LOCKED, "how far the weapon trails mouse turns at the hip (x the hold style's look lag; 0 = locked to the view)");
    S.relief_scale = cvar_reg_float("ads_eye_relief_scale", 0.5f, 0.2f, 1.5f, CVAR_LOCKED, "ADS: sight distance from the eye x this (1 = the stock weld / the scope eye relief)");
    S.pip_scale = cvar_reg_float("r_scope_pip_scale", 1.0f, 0.2f, 2.0f, CVAR_TIER, "scope picture-in-picture resolution (fraction of the render height x the sight's pip_resolution; > 1 supersamples, max 2048 px)");
    S.brightness = cvar_reg_float("sight_brightness", 1.0f, 0.25f, 2.0f, CVAR_ARCHIVE | CVAR_EDITABLE, "red dot / illuminated reticle brightness");
    S.ads_debug = cvar_reg_bool("ads_debug", false, CVAR_LOCKED, "ADS overlay: the sight socket projected against the exact screen centre, world / hands FOV, sight zoom, sensitivity");
    S.sight_debug = cvar_reg_int("sight_debug", 0, 0, 7, CVAR_LOCKED, "sight shader views: 1 lens mask, 2 PiP raw, 3 eye box, 4 reticle only, 5 glass only, 6 lens test, 7 red dot refraction (red) / bevel (green)");
    S.sens_mode = cvar_reg_string("ads_sens_mode", "monitor_distance", CVAR_ARCHIVE | CVAR_EDITABLE, "ADS sensitivity scaling: monitor_distance | fov_ratio | off");
    S.sens_coef = cvar_reg_float("ads_sens_coef", 1.0f, 0.0f, 3.0f, CVAR_ARCHIVE | CVAR_EDITABLE, "ADS sensitivity coefficient (monitor distance: fraction of the half screen height; fov_ratio: multiplier)");
    S.ads_dof = cvar_reg_bool("ads_dof", true, CVAR_TIER, "ADS depth of field once fully aimed: the world stays sharp, the weapon gets a slight blur");
    S.dof_focus = cvar_reg_float("ads_dof_focus", 80.0f, 5.0f, 1000.0f, CVAR_LOCKED, "ADS depth of field: fixed focus distance (m)");
    S.dof_fstop = cvar_reg_float("ads_dof_fstop", 4.0f, 0.5f, 32.0f, CVAR_LOCKED, "ADS depth of field: f-stop");
    S.iron_blur = cvar_reg_float("ads_dof_near_blur", 3.5f, 0.0f, 16.0f, CVAR_LOCKED, "ADS near-field (weapon) blur with iron sights, CoC pixels (optics: their own value)");
    cmd_register("sight_dot", cmd_sight_dot, "sight_dot: the red dot's offset from the screen centre (px) and the gun axis vs the view (deg), this frame");
    cmd_register("sight_test", cmd_sight_test, "sight_test align [weapon|all] [quit] | sweep | fov | parallax | aspect | net | dump: attachment / sight tests");
    cmd_register("sight_test_report", cmd_sight_test_report, "end sight_test fov and print the result");
    cmd_register("sight_pip_res", cmd_sight_pip_res, "sight_pip_res [optic multiplier]: scope PiP resolution per sight (pip_resolution x r_scope_pip_scale, bounded by the fidelity level) and the PiP size now; a value sets it live (not saved)");
    cmd_register("fov_report", cmd_fov_report, "fov_report: cl_fov / sv_fov / base / current world and hands FOV (tests)");
}
