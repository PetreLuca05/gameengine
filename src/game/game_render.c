#include "gameplay/phone/phone.h"
#include "stb_image.h"
// Presentation: builds draw lists for the world, local view, hands, effects, HUD and menus.
#include "gameplay/zombies/horde.h"
#include "vfx/vfx.h"
#include "game_internal.h"
#include "game_vista.h"
#include "game_water.h"
#include "scene.h"
#include "ui/recoil_debug.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/log.h"
#include "core/platform.h"
#include "render/render.h"
#include "render/debug_draw.h"
#include "world/level.h"
#include "world/chunks.h"
#include "gameplay/weapons.h"
#include "gameplay/characters.h"
#include "ui/anim_debug.h"
#include "player/holdpoint.h"
#include "gameplay/effects.h"
#include "player/hands.h"
#include "player/camera_fp.h"
#include "physics/physics.h"
#include "ui/ui.h"
#include "ui/hud.h"
#include "ui/menus.h"
#include "ui/console.h"
#include "audio/audio.h"
#include "assets/assets.h"
#include "net/net.h"
#include "world/openworld.h"
#include "gameplay/throwables.h"
#include "gameplay/revive/revive.h"
#include "game_revive.h"
#include "input/input.h"
#include "player/spectator.h"
#include "editor/handpose/hp_system.h"
#include <math.h>

#define G (g_engine.game)
#define MAX_CAPSULE_MESHES 16

typedef struct CapsuleMesh { float r, hh; MeshId mesh; } CapsuleMesh;
typedef struct GameRender {
    CapsuleMesh capsules[MAX_CAPSULE_MESHES];
    int capsule_count;
    int cam_mode;              // 0 fallback, 1 head camera, 2 death camera, 3 third person
    vec3 head_eye, ideal_eye;  // cam_debug
    char cam_text[256];
    // camera penetration stats (cam_debug / cam_stats): frames where the eye is behind geometry
    // as seen from the capsule, or geometry is closer than the near plane margin
    u32 pen_frames, pen_behind, pen_near;
    float pen_min_dist;
} GameRender;
#define GR (g_engine.game_render)

static void cmd_cam_stats(int argc, const char **argv);

// fp_cam_test start | report: per-frame first-person invariants while a script drives the player
//   |camera - head bone| < 0.15 m, camera forward . (camera - pelvis) >= 0 (never behind the body),
//   hands on their sockets (< 2 mm / 1 deg while the hold IK owns them).
static struct {
    bool active, loose;   // loose: sway / kick on, item steadiness + ADS sight checks off
    int frames, bad_head, bad_behind, hand_frames, bad_hands;
    float max_head, min_front, max_hand_mm, max_hand_deg;
    // item steadiness: camera-space drift inside one settled hold state (not sprint / ADS / vault)
    int ref_state, item_frames, bad_item, snap_skip, over_half_mm, ads_frames;
    float max_ads_mm, max_ads_deg;
    float prev_yaw, prev_pitch;
    Xform ref_item;
    float max_item_mm, max_item_deg;
} FT;
static void cmd_fp_cam_test(int argc, const char **argv) {
    if (argc > 1 && !strcmp(argv[1], "start")) {
        memset(&FT, 0, sizeof(FT));
        FT.active = true;
        FT.loose = argc > 2 && !strcmp(argv[2], "loose");
        FT.min_front = 1e9f;
        FT.ref_state = -1;
        log_info("fp_cam_test: recording");
        return;
    }
    FT.active = false;
    bool ads_ok = FT.loose || (FT.ads_frames > 0 && FT.max_ads_mm < 0.5f && FT.max_ads_deg < 0.2f);
    if (FT.loose) FT.bad_item = 0;
    bool ok = FT.frames >= 600 && !FT.bad_head && !FT.bad_behind && !FT.bad_hands && !FT.bad_item && ads_ok;
    log_info("fp_cam_test: ADS %d settled frames, sight off the view axis max %.3f mm, barrel %.3f deg (limit 0.5 mm / 0.2 deg)", FT.ads_frames, FT.max_ads_mm, FT.max_ads_deg);
    log_info("fp_cam_test: item on its hold point %d frames (not sprint / ADS / vault): max offset %.2f mm %.2f deg, %d frames over 0.5 mm, %d over 10 mm / 0.5 deg",
             FT.item_frames, FT.max_item_mm, FT.max_item_deg, FT.over_half_mm, FT.bad_item);
    log_info("fp_cam_test: %d frames | camera-head max %.1f mm (%d > 150 mm) | min forward.(cam - head / pelvis) %.1f mm (%d < 0) | hands %d frames, max %.2f mm %.2f deg (%d over 2 mm / 1 deg) -> %s",
             FT.frames, FT.max_head * 1000, FT.bad_head, FT.min_front * 1000, FT.bad_behind, FT.hand_frames, FT.max_hand_mm, FT.max_hand_deg, FT.bad_hands,
             ok ? "PASS" : "FAIL");
}
static void fp_cam_test_frame(vec3 cam_pos, vec3 fwd) {
    if (!FT.active) return;
    Xform head, pelvis;
    if (!chars_fp_bone_world(BR_HEAD, &head) || !chars_fp_bone_world(BR_HIPS, &pelvis)) return;
    FT.frames++;
    float dh = v3_len(v3_sub(cam_pos, head.t));
    // "Behind the body" (horizontal, along the view yaw): the camera must never be behind the
    // head (that exposes the neck / back), nor behind the pelvis while looking level or down
    // (looking up, the eyes rotate back over the neck: anatomical, the view points away from the body).
    vec3 fh = v3_norm(v3(fwd.x, 0, fwd.z));
    float dhead = v3_dot(fh, v3_sub(cam_pos, head.t));
    float dpel = v3_dot(fh, v3_sub(cam_pos, pelvis.t));
    float df = fwd.y > 0.0f ? dhead : fminf(dhead, dpel);   // the pelvis rule applies looking level or down
    FT.max_head = fmaxf(FT.max_head, dh);
    FT.min_front = fminf(FT.min_front, df);
    if (dh > 0.15f) FT.bad_head++;
    if (df < 0.0f) FT.bad_behind++;
    if ((dh > 0.15f || df < 0.0f) && FT.bad_head + FT.bad_behind < 40) {
        Player *p = game_local_player();
        log_info("fp_cam_test: frame %d cam-head %.0f mm fwd.(cam-pelvis) %.0f mm mode %d crouch %.2f pitch %.0f cam (%.2f %.2f %.2f) head (%.2f %.2f %.2f) pelvis (%.2f %.2f %.2f)",
                 FT.frames, dh * 1000, df * 1000, p ? p->sim.move.mode : -1, p ? p->sim.move.crouch_amount : 0, asinf(clampf(fwd.y, -1, 1)) * DG_RAD2DEG,
                 cam_pos.x, cam_pos.y, cam_pos.z, head.t.x, head.t.y, head.t.z, pelvis.t.x, pelvis.t.y, pelvis.t.z);
    }
}
// fp_pose: head / camera relative to the pelvis (forward = view yaw), this frame.
static void cmd_fp_pose(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    Xform head, pelvis;
    if (!chars_fp_bone_world(BR_HEAD, &head) || !chars_fp_bone_world(BR_HIPS, &pelvis)) { log_info("fp_pose: no first-person pose"); return; }
    vec3 f = dir_from_angles(G->view.yaw, 0);
    vec3 d = v3_sub(head.t, pelvis.t);
    log_info("fp_pose: pitch %.0f head-pelvis fwd %.0f mm up %.0f mm", G->view.pitch * DG_RAD2DEG, v3_dot(d, f) * 1000, d.y * 1000);
}

// ads_sweep start | report: while ADS, the sight socket's camera-space position per frame
// (distance along the view axis and angle off it) against the pitch.
static struct { bool on; int n; float min_d, max_d, max_ang, d_at_min, d_at_max, p_at_min, p_at_max; } AS;
static void cmd_ads_sweep(int argc, const char **argv) {
    if (argc > 1 && !strcmp(argv[1], "start")) { memset(&AS, 0, sizeof(AS)); AS.on = true; AS.min_d = 1e9f; return; }
    AS.on = false;
    log_info("ads_sweep: %d ADS frames | sight distance %.1f .. %.1f mm (spread %.2f mm; min at pitch %.0f, max at pitch %.0f) | off axis max %.3f deg -> %s",
             AS.n, AS.min_d * 1000, AS.max_d * 1000, (AS.max_d - AS.min_d) * 1000, AS.p_at_min, AS.p_at_max, AS.max_ang,
             AS.n > 0 && AS.max_d - AS.min_d < 0.001f && AS.max_ang < 0.1f ? "PASS" : "FAIL");
}
static void ads_sweep_frame(void) {
    if (!AS.on) return;
    Xform ic;
    int st;
    float bl;
    if (!chars_fp_item_cam(&ic, &st, &bl) || st != HP_ADS || bl < 0.999f) return;
    vec3 s = chars_fp_sight_cam();
    float d = -s.z, ang = atan2f(sqrtf(s.x * s.x + s.y * s.y), fmaxf(-s.z, 1e-4f)) * DG_RAD2DEG;
    float pitch = G->view.pitch * DG_RAD2DEG;
    AS.n++;
    if (d < AS.min_d) { AS.min_d = d; AS.p_at_min = pitch; }
    if (d > AS.max_d) { AS.max_d = d; AS.p_at_max = pitch; }
    AS.max_ang = fmaxf(AS.max_ang, ang);
    if ((AS.n % 10) == 0 || d < 0.2395f) log_info("ads_sweep: pitch %5.1f sight distance %.1f mm off axis %.3f deg reach pull %.1f mm", pitch, d * 1000, ang, chars_fp_reach_pull() * 1000);
}

static void fp_cam_test_hands(void) {
    if (!FT.active) return;
    float gp[2], ga[2], ap, aa;
    chars_fp_diag(gp, ga, &ap, &aa);
    if (gp[1] < 0 || chars_fp_hold_weight() < 0.99f) return;   // no hold, or a clip / interaction owns the hands
    {
        Xform ic;
        int st;
        float bl;
        Player *p = game_local_player();
        bool vault = p && (p->sim.move.mode == MOVE_VAULT || p->sim.move.mode == MOVE_MANTLE || p->sim.move.mode == MOVE_HANG);
        // Scripted view snaps (setang: > 30 deg in one frame) are skipped for 5 frames: the tick's
        // spine pose catches up with the frame's view (no real mouse input does this).
        float dv = fmaxf(fabsf(wrap_angle(G->view.yaw - FT.prev_yaw)), fabsf(G->view.pitch - FT.prev_pitch)) * DG_RAD2DEG;
        FT.prev_yaw = G->view.yaw;
        FT.prev_pitch = G->view.pitch;
        if (dv > 30.0f) FT.snap_skip = 5;
        if (FT.snap_skip > 0) FT.snap_skip--;
        else if (chars_fp_item_cam(&ic, &st, &bl)) {
            if (bl >= 0.999f && st == HP_ADS) {
                FT.ads_frames++;
                FT.max_ads_mm = fmaxf(FT.max_ads_mm, ap * 1000.0f);
                if (ap * 1000.0f > 0.5f && FT.ads_frames % 4 == 0) {
                    const WeaponDef *wd = weapon_equipped_def(G->world, G->local_slot);
                    log_info("fp_cam_test: frame %d ADS sight %.2f mm (%s) pitch %.0f | %s", FT.frames, ap * 1000.0f, wd ? wd->id : "?", G->view.pitch * DG_RAD2DEG,
                             chars_holdpoint_text(G->world, G->local_slot));
                }
                FT.max_ads_deg = fmaxf(FT.max_ads_deg, aa);
            }
            bool settled = bl >= 0.999f && st != HP_SPRINT && st != HP_ADS && !vault;
            FT.ref_state = st;
            if (settled) {
                FT.item_frames++;
                float mm = v3_len(ic.t) * 1000.0f, deg = quat_angle_between(ic.r, quat_identity()) * DG_RAD2DEG;
                FT.max_item_mm = fmaxf(FT.max_item_mm, mm);
                FT.max_item_deg = fmaxf(FT.max_item_deg, deg);
                if (mm > 0.5f || deg > 0.2f) FT.over_half_mm++;
                if (mm > 10.0f || deg > 0.5f) {
                    if (FT.bad_item < 60)
                        log_info("fp_cam_test: frame %d item drift %.2f mm %.2f deg (state %d) speed %.1f pitch %.0f | %s", FT.frames, mm, deg, st,
                                 p ? v3_len(v3_flat(p->sim.move.vel)) : 0, G->view.pitch * DG_RAD2DEG, chars_holdpoint_text(G->world, G->local_slot));
                    FT.bad_item++;
                }
            }
        }
    }
    FT.hand_frames++;
    float mm = fmaxf(gp[0], gp[1]) * 1000.0f, deg = fmaxf(ga[0], ga[1]);
    FT.max_hand_mm = fmaxf(FT.max_hand_mm, mm);
    FT.max_hand_deg = fmaxf(FT.max_hand_deg, deg);
    if (mm > 2.0f || deg > 1.0f) {
        FT.bad_hands++;
        if (FT.bad_hands < 25) log_info("fp_cam_test: frame %d hands R %.2f mm %.2f deg  L %.2f mm %.2f deg ikw %.2f | %s", FT.frames, gp[1] * 1000, ga[1], gp[0] * 1000, ga[0], chars_fp_hold_weight(), chars_holdpoint_text(G->world, G->local_slot));
    }
}
void game_render_init(void) {
    sights_register();
    daynight_register();
    shadow_sky_register();
    terrain_game_register();
    ground_ref_register();
    weather_register();
    water_register();
    wind_game_register();
    game_audio_register();
    foliage_register();
    vehicles_register();
    flesh_range_register();
    phone_register();
    cmd_register("ads_sweep", cmd_ads_sweep, "ads_sweep start | report: sight socket distance / angle against the view axis while ADS");
    cmd_register("fp_pose", cmd_fp_pose, "print the first-person head position relative to the pelvis");
    cmd_register("fp_cam_test", cmd_fp_cam_test, "fp_cam_test start | report: first-person camera / body / hands invariants");
    GR = ARENA_PUSH(&g_engine.perm_arena, GameRender);
    GR->pen_min_dist = 1e9f;
    cmd_register("cam_stats", cmd_cam_stats, "print + reset camera penetration / stabilizer stats (sampled while cam_debug is on)");
}
const char *game_cam_debug_text(void) { return GR ? GR->cam_text : ""; }

static void cam_penetration_sample(PhysicsWorld *pw, vec3 safe, vec3 eye) {
    GR->pen_frames++;
    RayHit rh;
    vec3 d = v3_sub(eye, safe);
    if (v3_len2(d) > 1e-8f && phys_raycast(pw, safe, d, PL_MASK_WORLD, PHYS_INVALID_BODY, &rh) && rh.fraction < 0.999f) GR->pen_behind++;
    // 14 directions: geometry closer than the near plane margin would clip into view.
    static const vec3 dirs[14] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 }, { 0.577f, 0.577f, 0.577f },
                                   { -0.577f, 0.577f, 0.577f }, { 0.577f, -0.577f, 0.577f }, { 0.577f, 0.577f, -0.577f }, { -0.577f, -0.577f, 0.577f },
                                   { -0.577f, 0.577f, -0.577f }, { 0.577f, -0.577f, -0.577f }, { -0.577f, -0.577f, -0.577f } };
    float near_margin = 0.06f, best = 1e9f;
    for (int i = 0; i < 14; i++)
        if (phys_raycast(pw, eye, v3_scale(dirs[i], 0.5f), PL_MASK_WORLD, PHYS_INVALID_BODY, &rh)) best = fminf(best, rh.distance);
    if (best < near_margin) GR->pen_near++;
    if (GR->pen_frames == 1 || best < GR->pen_min_dist) GR->pen_min_dist = best;
}

static void cmd_cam_stats(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    log_info("cam_stats: %u frames, eye behind geometry %u, geometry within 6 cm of the eye %u, closest %.1f mm, stabilizer max %.1f mm",
             GR->pen_frames, GR->pen_behind, GR->pen_near, GR->pen_min_dist * 1000.0f, G->cam_stab.max_seen * 1000.0f);
    GR->pen_frames = GR->pen_behind = GR->pen_near = 0;
    GR->pen_min_dist = 1e9f;
    G->cam_stab.max_seen = 0;
}

static bool cam_sphere_cast(void *ctx, vec3 from, vec3 delta, float radius, float *fraction) {
    ShapeHit h;
    if (!phys_sphere_cast((PhysicsWorld *)ctx, from, delta, radius, PL_MASK_WORLD, PHYS_INVALID_BODY, &h)) return false;
    *fraction = h.fraction;
    return true;
}

MeshId game_capsule_mesh(float r, float hh) {
    for (int i = 0; i < GR->capsule_count; i++)
        if (fabsf(GR->capsules[i].r - r) < 1e-3f && fabsf(GR->capsules[i].hh - hh) < 1e-3f) return GR->capsules[i].mesh;
    MeshBuilder mb;
    mb_init(&mb);
    mb_capsule(&mb, v3(0, 0, 0), r, hh, 12, 20, 0xFFFFFFFFu);
    MeshId id = mb_upload(&mb);
    mb_free(&mb);
    if (GR->capsule_count < MAX_CAPSULE_MESHES) {
        GR->capsules[GR->capsule_count].r = r;
        GR->capsules[GR->capsule_count].hh = hh;
        GR->capsules[GR->capsule_count].mesh = id;
        GR->capsule_count++;
    }
    return id;
}

static vec4 unpack(u32 c) {
    return v4((float)(c & 0xFF) / 255.0f, (float)((c >> 8) & 0xFF) / 255.0f, (float)((c >> 16) & 0xFF) / 255.0f, (float)(c >> 24) / 255.0f);
}

// Stable per-part ids of moving entities (velocity / motion blur): entity index + part counter.
static u32 g_obj_base, g_obj_part;
static u32 next_object_id(void) { return g_obj_base ? g_obj_base + (g_obj_part++ & 63u) : 0u; }

static void draw_shape(VisShape shape, vec3 half, vec3 pos, quat rot, Material mat) {
    DrawItem d;
    memset(&d, 0, sizeof(d));
    d.object_id = next_object_id();
    d.mat = mat;
    switch (shape) {
    case VIS_CYLINDER: d.mesh = G->meshes.cylinder; d.model = mat4_trs(pos, rot, v3(half.x * 2, half.y * 2, half.z * 2)); break;
    case VIS_SPHERE: d.mesh = G->meshes.sphere; d.model = mat4_trs(pos, rot, v3s(half.x * 2)); break;
    case VIS_CAPSULE: d.mesh = game_capsule_mesh(half.x, half.y); d.model = mat4_trs(pos, rot, v3s(1)); break;
    case VIS_NONE: return;
    default: d.mesh = G->meshes.cube; d.model = mat4_trs(pos, rot, v3_scale(half, 2)); break;
    }
    render_draw(&d);
}

static Material mat_from(u32 color, float rough, u32 flags) {
    Material m = material_color(unpack(color));
    m.roughness = rough;
    m.flags = flags;
    return m;
}

static void draw_box_local(vec3 pos, quat rot, vec3 offset, vec3 half, Material m) {
    draw_shape(VIS_BOX, half, v3_add(pos, quat_rotate(rot, offset)), rot, m);
}

// Fabricator placeholder (docs/REVIVE.md): plinth, glass chamber (the reconstruction happens inside,
// game_revive.c), back spine, control column with the panel and the sample slot, top cap, light.
// Local frame: -Z is the front; matches the sockets in revive.c.
static void draw_fabricator(World *w, int i, vec3 pos, quat rot, u32 hl) {
    FabView fv;
    revive_fab_view(w, ENT_MAKE(i, w->gen[i]), &fv);
    float t = (float)g_engine.render_time;
    bool recon = fv.m.state == FAB_RECONSTRUCTING, open = fv.m.state == FAB_OPENING;
    float pulse = recon ? 0.6f + 0.4f * sinf(t * (3.0f + 6.0f * fv.progress)) : (open ? 1.0f : 0.35f);
    if (!fv.m.powered) pulse = 0.0f;
    Material metal = mat_from(rgba8(0.28f, 0.3f, 0.33f, 1), 0.45f, 0);
    metal.metallic = 0.8f;
    Material dark = mat_from(rgba8(0.12f, 0.13f, 0.15f, 1), 0.55f, 0);
    draw_box_local(pos, rot, v3(0.3f, 0.06f, 0.0f), v3(0.95f, 0.06f, 0.7f), dark);
    // Chamber floor with an emissive ring, top cap with a light strip.
    vec3 cyan = v3(0.35f, 0.9f, 1.0f);
    Material ring = mat_from(rgba8(cyan.x, cyan.y, cyan.z, 1), 0.3f, MAT_NO_SHADOW | (recon ? MAT_CAUSTICS : 0u));
    ring.emissive = 2.0f + 6.0f * pulse;
    draw_shape(VIS_CYLINDER, v3(0.6f, 0.012f, 0.6f), v3_add(pos, quat_rotate(rot, v3(0, 0.13f, 0))), rot, ring);
    draw_shape(VIS_CYLINDER, v3(0.56f, 0.02f, 0.56f), v3_add(pos, quat_rotate(rot, v3(0, 0.12f, 0))), rot, metal);
    draw_shape(VIS_CYLINDER, v3(0.63f, 0.09f, 0.63f), v3_add(pos, quat_rotate(rot, v3(0, 2.33f, 0))), rot, metal);
    draw_shape(VIS_CYLINDER, v3(0.6f, 0.012f, 0.6f), v3_add(pos, quat_rotate(rot, v3(0, 2.23f, 0))), rot, ring);
    // Back spine + control column (the column has the collision body).
    draw_box_local(pos, rot, v3(0.0f, 1.2f, 0.62f), v3(0.2f, 1.15f, 0.07f), metal);
    Material col = metal;
    col.flags |= hl;
    draw_box_local(pos, rot, v3(0.85f, 0.95f, 0.0f), v3(0.25f, 0.95f, 0.35f), col);
    // Panel: green ready, amber reconstructing, red unpowered; the sample slot glows while busy.
    vec3 pc = !fv.m.powered ? v3(0.9f, 0.15f, 0.1f) : (recon || open ? v3(1.0f, 0.65f, 0.15f) : v3(0.25f, 0.95f, 0.45f));
    Material panel = mat_from(rgba8(pc.x, pc.y, pc.z, 1), 0.2f, hl);
    panel.emissive = fv.m.powered ? 3.0f : 0.6f;
    quat tilt = quat_mul(rot, quat_axis_angle(v3(1, 0, 0), 0.35f));
    draw_shape(VIS_BOX, v3(0.2f, 0.13f, 0.012f), v3_add(pos, quat_rotate(rot, v3(0.85f, 1.38f, -0.36f))), tilt, panel);
    if (recon) {
        // Progress bar on the panel.
        Material pb = mat_from(rgba8(1, 1, 1, 1), 0.2f, MAT_NO_SHADOW);
        pb.emissive = 5.0f;
        float wbar = 0.36f * fmaxf(fv.progress, 0.02f);
        draw_shape(VIS_BOX, v3(wbar * 0.5f, 0.012f, 0.004f), v3_add(pos, quat_rotate(rot, v3(0.85f - 0.18f + wbar * 0.5f, 1.30f, -0.375f))), tilt, pb);
    }
    Material slot = mat_from(rgba8(0.05f, 0.05f, 0.06f, 1), 0.4f, 0);
    draw_box_local(pos, rot, v3(0.85f, 1.06f, -0.36f), v3(0.06f, 0.045f, 0.02f), slot);
    Material rim = mat_from(rgba8(cyan.x, cyan.y, cyan.z, 1), 0.3f, MAT_NO_SHADOW);
    rim.emissive = fv.m.qn || recon ? 6.0f : 1.5f;
    draw_box_local(pos, rot, v3(0.85f, 1.06f, -0.381f), v3(0.065f, 0.006f, 0.004f), rim);
    // Glass + liquid (blended, drawn in the transparent pass), then the reconstruction inside.
    game_revive_draw_chamber(w, i, pos, rot, &fv, pulse);
    // Interior light pulsing with the progress.
    if (fv.m.powered) {
        PointLight l = { v3_add(pos, quat_rotate(rot, v3(0, 1.4f, 0))), 3.5f, cyan, 1.0f + (recon ? 5.0f * pulse * (0.4f + fv.progress) : 0.6f * pulse) };
        l.flags = LIGHT_NO_SHADOW;
        render_add_light(&l);
    }
}

static void render_entity(World *w, int i, float alpha, bool focused) {
    EntityType t = (EntityType)w->type[i];
    Transform *xf = &w->xf[i];
    vec3 pos = v3_lerp(xf->prev_pos, xf->pos, alpha);
    quat rot = quat_nlerp(xf->prev_rot, xf->rot, alpha);
    float anim = lerpf(w->anim_prev[i], w->anim[i], alpha);
    Visual *v = &w->vis[i];
    u32 hl = focused ? MAT_HIGHLIGHT : 0;
    Material m = mat_from(v->color, 0.75f, hl);
    g_obj_base = ((u32)i + 1u) << 6;
    g_obj_part = 0;
    if (v->emissive) m.emissive = v->emissive / 255.0f;

    if (v->model) {
        const Model *mdl = assets_model(v->model);
        if (mdl) {
            // Fit the model's bounds to the entity's physical extents.
            vec3 ext = v3_scale(v3_sub(mdl->bounds.max, mdl->bounds.min), 0.5f);
            vec3 mid = v3_scale(v3_add(mdl->bounds.max, mdl->bounds.min), 0.5f);
            vec3 sc = v3(v->half.x / fmaxf(ext.x, 1e-3f), v->half.y / fmaxf(ext.y, 1e-3f), v->half.z / fmaxf(ext.z, 1e-3f));
            mat4 base = mat4_mul(mat4_trs(pos, rot, sc), mat4_translate(v3_neg(mid)));
            for (int k = 0; k < mdl->mesh_count; k++) {
                DrawItem d;
                memset(&d, 0, sizeof(d));
                d.object_id = next_object_id();
                d.mesh = mdl->meshes[k].mesh;
                d.model = mat4_mul(base, mdl->meshes[k].local);
                d.mat = mdl->meshes[k].mat;
                d.mat.color = v4_lerp(d.mat.color, unpack(v->color), 0.35f);
                d.mat.flags |= hl;
                render_draw(&d);
            }
            return;
        }
    }
    switch (t) {
    case ET_DOOR: {
        if ((v->color >> 24) < 0xFF) {
            // Glass leaf (building kit sliders, "glass": true): a pane in the glass bucket and a metal stile.
            Material gm = mat_from(v->color | 0xFF000000u, 0.03f, MAT_GLASS | MAT_NO_SHADOW | hl);
            gm.color.w = 0.12f;   // as the kit's glass set
            draw_shape(VIS_BOX, v3(v->half.x - 0.03f, v->half.y - 0.03f, 0.008f), pos, rot, gm);
            Material fm = mat_from(rgba8(0.18f, 0.19f, 0.2f, 1), 0.4f, hl);
            fm.metallic = 0.8f;
            for (int sd = -1; sd <= 1; sd += 2) {
                draw_box_local(pos, rot, v3((float)sd * (v->half.x - 0.015f), 0, 0), v3(0.015f, v->half.y, v->half.z), fm);
                draw_box_local(pos, rot, v3(0, (float)sd * (v->half.y - 0.015f), 0), v3(v->half.x, 0.015f, v->half.z), fm);
            }
            draw_box_local(pos, rot, v3(w->door[i].knob_local.x, w->door[i].knob_local.y, 0), v3(0.012f, 0.12f, v->half.z + 0.02f), fm);
            break;
        }
        draw_shape(VIS_BOX, v->half, pos, rot, m);
        // Knobs on both sides.
        Material km = mat_from(rgba8(0.75f, 0.68f, 0.3f, 1), 0.3f, hl);
        km.metallic = 0.9f;
        vec3 knob = w->door[i].knob_local;
        draw_box_local(pos, rot, v3(knob.x, knob.y, 0.05f), v3(0.025f, 0.025f, 0.03f), km);
        draw_box_local(pos, rot, v3(knob.x, knob.y, -0.05f), v3(0.025f, 0.025f, 0.03f), km);
        // Door viewer (the peek visor): a brass ring with a dark lens on both faces.
        {
            vec3 vl = w->door[i].visor_local;
            quat vr = quat_mul(rot, quat_axis_angle(v3(1, 0, 0), DG_PI * 0.5f));
            Material lens = mat_from(rgba8(0.05f, 0.06f, 0.08f, 1), 0.05f, 0);
            lens.metallic = 0.2f;
            for (int sd = -1; sd <= 1; sd += 2) {
                float z = (float)sd * (v->half.z + 0.006f);
                draw_shape(VIS_CYLINDER, v3(0.022f, 0.008f, 0.022f), v3_add(pos, quat_rotate(rot, v3(vl.x, vl.y, z))), vr, km);
                draw_shape(VIS_CYLINDER, v3(0.012f, 0.004f, 0.012f), v3_add(pos, quat_rotate(rot, v3(vl.x, vl.y, z + (float)sd * 0.008f))), vr, lens);
            }
        }
        if (w->flags[i] & EF_LOCKED) {
            Material lm = mat_from(rgba8(0.9f, 0.1f, 0.1f, 1), 0.4f, MAT_UNLIT);
            draw_box_local(pos, rot, v3(knob.x, knob.y + 0.1f, 0.03f), v3(0.02f, 0.03f, 0.01f), lm);
            draw_box_local(pos, rot, v3(knob.x, knob.y + 0.1f, -0.03f), v3(0.02f, 0.03f, 0.01f), lm);
        }
        break;
    }
    case ET_LEVER: {
        draw_shape(VIS_BOX, v->half, pos, rot, m);
        float ang = lerpf(-0.75f, 0.75f, smoothstepf(0, 1, anim));
        quat hr = quat_mul(rot, quat_axis_angle(v3(1, 0, 0), -ang));
        vec3 pivot = v3_add(pos, quat_rotate(rot, v3(0, 0.15f, 0)));
        Material hm = mat_from(rgba8(0.15f, 0.15f, 0.15f, 1), 0.5f, hl);
        draw_shape(VIS_BOX, v3(0.02f, 0.22f, 0.02f), v3_add(pivot, quat_rotate(hr, v3(0, 0.22f, 0))), hr, hm);
        Material gm = mat_from((w->flags[i] & EF_ON) ? rgba8(0.2f, 0.8f, 0.2f, 1) : rgba8(0.8f, 0.2f, 0.15f, 1), 0.5f, hl);
        draw_shape(VIS_BOX, v3(0.05f, 0.035f, 0.035f), v3_add(pivot, quat_rotate(hr, v3(0, 0.45f, 0))), hr, gm);
        break;
    }
    case ET_SWITCH: {
        draw_shape(VIS_BOX, v->half, pos, rot, m);
        float tilt = (w->flags[i] & EF_ON) ? -0.3f : 0.3f;
        quat rr = quat_mul(rot, quat_axis_angle(v3(1, 0, 0), tilt));
        Material rm = mat_from((w->flags[i] & EF_ON) ? rgba8(0.95f, 0.85f, 0.4f, 1) : rgba8(0.4f, 0.4f, 0.4f, 1), 0.5f, hl);
        if (w->flags[i] & EF_ON) rm.emissive = 0.4f;
        draw_shape(VIS_BOX, v3(0.025f, 0.045f, 0.012f), v3_add(pos, quat_rotate(rot, v3(0, 0, 0.018f))), rr, rm);
        break;
    }
    case ET_LIGHT: {
        bool on = (w->flags[i] & EF_ON) != 0;
        Material lm = mat_from(v->color, 0.5f, MAT_NO_SHADOW | (on ? MAT_UNLIT : 0));
        if (on) lm.color = v4(lm.color.x * 4, lm.color.y * 4, lm.color.z * 4, 1);
        else lm.color = v4(0.15f, 0.15f, 0.15f, 1);
        draw_shape(VIS_BOX, v->half, pos, rot, lm);
        if (on) {
            // Scheduled lamps may flicker (cosmetic, deterministic per lamp).
            float k = 1.0f;
            if (w->light[i].flicker > 0) {
                float t = (float)g_engine.render_time * 9.0f + (float)i * 3.17f;
                float n = 0.5f + 0.5f * sinf(t) * sinf(t * 2.31f + 1.3f) * sinf(t * 0.37f + 0.7f);
                k = 1.0f - w->light[i].flicker * (n > 0.82f ? 0.85f : n * 0.25f);
            }
            PointLight l = { v3_add(pos, v3(0, -0.15f, 0)), w->light[i].radius, w->light[i].color, w->light[i].intensity * k };
            if (w->light[i].group & LIGHT_GROUP_NO_SHADOW) l.flags = LIGHT_NO_SHADOW;
            if (w->light[i].shadow_given) l.shadow = &w->light[i].shadow;   // Unity-style per-light settings
            render_add_light(&l);
        }
        break;
    }
    case ET_CONTAINER: {
        if (v->shape == VIS_NONE) break;   // furniture is the container (building kit)
        vec3 h = v->half;
        m.texture = assets_texture("crate");
        if (m.texture) { m.flags |= MAT_TEXTURED | MAT_TRIPLANAR; m.tex_scale = 1.6f; m.color = v4(1, 1, 1, 1); }
        float lid_h = 0.06f;
        draw_box_local(pos, rot, v3(0, -lid_h * 0.5f, 0), v3(h.x, h.y - lid_h * 0.5f, h.z), m);
        // Lid hinged at the back top edge.
        float ang = anim * 1.6f;
        quat lr = quat_mul(rot, quat_axis_angle(v3(1, 0, 0), ang));
        vec3 hinge = v3_add(pos, quat_rotate(rot, v3(0, h.y - lid_h, -h.z)));
        Material lm = mat_from(rgba8(0.38f, 0.27f, 0.15f, 1), 0.8f, hl);
        draw_shape(VIS_BOX, v3(h.x + 0.01f, lid_h * 0.5f, h.z + 0.01f), v3_add(hinge, quat_rotate(lr, v3(0, lid_h * 0.5f, h.z))), lr, lm);
        Material bm = mat_from(rgba8(0.2f, 0.2f, 0.22f, 1), 0.4f, hl);
        draw_box_local(pos, rot, v3(-h.x + 0.08f, 0, h.z + 0.005f), v3(0.03f, h.y * 0.9f, 0.01f), bm);
        draw_box_local(pos, rot, v3(h.x - 0.08f, 0, h.z + 0.005f), v3(0.03f, h.y * 0.9f, 0.01f), bm);
        break;
    }
    case ET_BENCH: break;   // interaction volume only (the bench itself is a level brush)
    case ET_CORPSE: break;   // the adopted ragdoll / lying pose (characters.c)
    case ET_FABRICATOR: draw_fabricator(w, i, pos, rot, hl); break;
    case ET_PICKUP: {
        if (w->flags[i] & EF_HIDDEN) break;
        // The same model as in the hands, lying where it landed (casts a shadow; highlighted on focus).
        u16 it = w->item[i];
        const WeaponDef *wd = it ? item_weapon(w->items, it) : NULL;
        const ItemDef *idef = it ? item_def(w->items, it) : NULL;
        if (wd || idef) hands_draw_held_att(wd, wd ? NULL : idef, w->attach[i], pos, rot, hl);
        else draw_shape((VisShape)v->shape, v->half, pos, rot, m);
        break;
    }
    case ET_DUMMY: {
        if (char_get_index(w, i)) {
            // Skinned dummy (characters.c draws it): just the stand.
            Material post = mat_from(rgba8(0.3f, 0.3f, 0.3f, 1), 0.6f, 0);
            draw_shape(VIS_BOX, v3(0.25f, 0.02f, 0.25f), v3_add(v3_sub(pos, v3(0, 0.9f, 0)), v3(0, 0.02f, 0)), rot, post);
            break;
        }
        // Toppled backwards when dead.
        quat tilt = quat_mul(rot, quat_axis_angle(v3(1, 0, 0), -anim * 1.45f));
        vec3 base = v3_sub(pos, v3(0, 0.9f, 0));
        vec3 center = v3_add(base, quat_rotate(tilt, v3(0, 0.9f, 0)));
        draw_shape(VIS_CAPSULE, v->half, center, tilt, m);
        Material hm = mat_from(rgba8(0.85f, 0.75f, 0.5f, 1), 0.8f, 0);
        draw_shape(VIS_SPHERE, v3s(0.16f), v3_add(base, quat_rotate(tilt, v3(0, 1.6f, 0))), tilt, hm);
        Material ring = mat_from(rgba8(0.85f, 0.15f, 0.1f, 1), 0.6f, 0);
        draw_shape(VIS_CYLINDER, v3(0.31f, 0.03f, 0.31f), v3_add(base, quat_rotate(tilt, v3(0, 1.2f, 0))), tilt, ring);
        draw_shape(VIS_CYLINDER, v3(0.31f, 0.03f, 0.31f), v3_add(base, quat_rotate(tilt, v3(0, 0.8f, 0))), tilt, ring);
        Material post = mat_from(rgba8(0.3f, 0.3f, 0.3f, 1), 0.6f, 0);
        draw_shape(VIS_BOX, v3(0.25f, 0.02f, 0.25f), v3_add(base, v3(0, 0.02f, 0)), rot, post);
        break;
    }
    case ET_ZOMBIE:
        break;   // characters.c
    case ET_PLAYER: {
        int slot = w->player_slot[i];
        if (slot == game_view_slot()) break; // first person (own or spectated): drawn as hands
        if (char_get_index(w, i)) break;  // skinned character (characters.c)
        bool dead = (w->flags[i] & EF_DEAD) != 0;
        bool crouch = (w->flags[i] & EF_CROUCHED) != 0;
        float height = crouch ? PLAYER_CROUCH_HEIGHT : PLAYER_HEIGHT;
        float yaw = quat_get_yaw(rot);
        quat body_rot = quat_yaw(yaw);
        if (dead) body_rot = quat_mul(body_rot, quat_axis_angle(v3(1, 0, 0), -1.4f));
        float hh = height * 0.5f - PLAYER_RADIUS - 0.1f;
        vec3 center = v3_add(pos, quat_rotate(body_rot, v3(0, height * 0.5f - 0.1f, 0)));
        draw_shape(VIS_CAPSULE, v3(PLAYER_RADIUS, hh, PLAYER_RADIUS), center, body_rot, m);
        Material head = mat_from(rgba8(0.85f, 0.66f, 0.52f, 1), 0.7f, 0);
        vec3 head_pos = v3_add(pos, quat_rotate(body_rot, v3(0, height - 0.15f, 0)));
        draw_shape(VIS_BOX, v3(0.12f, 0.13f, 0.12f), head_pos, body_rot, head);
        Material visor = mat_from(rgba8(0.1f, 0.1f, 0.12f, 1), 0.2f, 0);
        draw_box_local(head_pos, body_rot, v3(0, 0.02f, -0.12f), v3(0.09f, 0.03f, 0.01f), visor);
        if (!dead) {
            float pitch = w->anim[i];
            vec3 eye = v3_add(pos, v3(0, crouch ? PLAYER_EYE_CROUCH : PLAYER_EYE_STAND, 0));
            hands_render_remote(w, slot, eye, yaw, pitch);
        }
        break;
    }
    case ET_PROJECTILE:
        if (w->item[i]) {
            // Thrown items (gameplay/throwables.c): their own shape, burning flares / fire light up.
            if (v->shape != VIS_NONE) draw_shape((VisShape)v->shape, v->half, pos, rot, m);
            throwables_render(w, i);
            break;
        }
        draw_shape(VIS_BOX, v->half, pos, rot, m);
        break;
    case ET_GATE:
        m.texture = assets_texture("hazard");
        if (m.texture) { m.flags |= MAT_TEXTURED | MAT_TRIPLANAR; m.tex_scale = 0.5f; m.color = v4(1, 1, 1, 1); m.metallic = 0.6f; m.roughness = 0.5f; }
        draw_shape(VIS_BOX, v->half, pos, rot, m);
        break;
    case ET_PROP:
        if (v->shape == VIS_BOX) {
            m.texture = assets_texture("crate");
            if (m.texture) { m.flags |= MAT_TEXTURED | MAT_TRIPLANAR; m.tex_scale = 1.0f / fmaxf(0.2f, v->half.x * 2.0f); m.color = v4_lerp(v4(1, 1, 1, 1), unpack(v->color), 0.3f); }
        }
        draw_shape((VisShape)v->shape, v->half, pos, rot, m);
        break;
    default:
        draw_shape((VisShape)v->shape, v->half, pos, rot, m);
        break;
    }
}

void game_render_world(World *w, float alpha, vec3 cam_pos, vec3 cam_fwd) {
    if (w->chunks) chunks_render(w, cam_pos);
    if (!w->chunks || w->openworld) level_draw_static(w, cam_pos, cam_fwd);
    openworld_frame(w, cam_pos);
    water_frame(w, cam_pos);
    ENT_FOREACH(w, i) {
        if (!(w->comps[i] & C_VISUAL) && w->type[i] != ET_ZOMBIE) continue;
        if ((w->flags[i] & EF_HIDDEN) && w->type[i] != ET_PICKUP) continue;
        render_entity(w, i, alpha, ENT_MAKE(i, w->gen[i]) == G->focus);
    }
    g_obj_base = 0;
    chars_render(w, G->alpha, cam_pos, game_view_slot());   // the viewed player's body: first-person treatment
}

static void draw_debug(World *w, vec3 cam) {
    if (G->r_drawcollision->i) {
        phys_debug_draw(w->phys, cam, 25.0f);
        for (int s = 0; s < MAX_PLAYERS; s++) {
            Player *p = &w->players[s];
            if (!p->active || !p->character) continue;
            float h = p->sim.move.crouched ? PLAYER_CROUCH_HEIGHT : PLAYER_HEIGHT;
            dbg_capsule(phys_character_pos(p->character), PLAYER_RADIUS, h, DBG_MAGENTA, 0);
        }
    }
    chars_debug_draw(w, cam);
    weapons_aim_debug_frame(w);   // aim_debug (docs/AIM.md)
    horde_debug_draw(w);
    if (G->r_drawhitboxes->i) {
        ENT_FOREACH(w, i) {
            if (!(w->comps[i] & C_HITBOX)) continue;
            vec3 feet = w->type[i] == ET_DUMMY ? v3_sub(w->xf[i].pos, v3(0, 0.9f, 0)) : w->xf[i].pos;
            dbg_capsule(feet, w->hitbox[i].radius, w->hitbox[i].height, DBG_RED, 0);
            float hy = w->type[i] == ET_PLAYER ? w->hitbox[i].height - 0.18f : w->hitbox[i].head_height;
            dbg_sphere(v3_add(feet, v3(0, hy, 0)), w->hitbox[i].head_radius, DBG_YELLOW, 0);
        }
    }
}

// Flashlights (F): a cookie-projected spot light. The local one
// sits just ahead of the eye (in front of the held gun) along the view; other players' come from
// their eye along the replicated aim (EF_FLASHLIGHT).
static void flashlights_render(World *w, const RenderCamera *cam, const Player *lp, bool dead, int view_slot) {
    static TextureId cookie;
    static bool tried;
    if (!tried) {
        tried = true;
        int cw, ch, cc;
        char path[512];
        snprintf(path, sizeof(path), "%sassets/textures/flashlight_cookie.png", platform_base_dir());
        unsigned char *px = stbi_load(path, &cw, &ch, &cc, 4);
        if (px) {
            cookie = render_texture_create(cw, ch, px, true, false);
            stbi_image_free(px);
            render_set_spot_cookie(cookie);
        } else {
            log_warn("flashlight: cookie %s missing (plain cone)", path);
        }
    }
    SpotLight s;
    memset(&s, 0, sizeof(s));
    s.range = 55.0f;
    s.inner_deg = 7.0f;
    s.outer_deg = 24.0f;
    s.intensity = 140.0f;
    s.color = v3(1.0f, 0.97f, 0.92f);
    // The viewed player's light rides the camera: the local toggle, or a spectated player's replicated one.
    bool own_on = view_slot == G->local_slot ? G->flashlight : (lp && (w->flags[ENT_INDEX(lp->ent)] & EF_FLASHLIGHT));
    if (own_on && lp && !dead) {
        vec3 f = dir_from_angles(cam->yaw, cam->pitch), r = right_from_yaw(cam->yaw), u = v3_cross(r, f);
        // Off-hand side, a little ahead of the eye: the held gun (to the right, mostly behind
        // the light) isn't flooded, the beam still starts at the view.
        s.pos = v3_add(v3_add(v3_madd(cam->pos, f, 0.28f), v3_scale(r, -0.12f)), v3_scale(u, -0.05f));
        s.dir = f;
        render_add_spot(&s);
    }
    for (int k = 0; k < MAX_PLAYERS; k++) {
        const Player *op = &w->players[k];
        if (!op->active || k == view_slot || !ent_valid(w, op->ent)) continue;
        int i = (int)ENT_INDEX(op->ent);
        if (!(w->flags[i] & EF_FLASHLIGHT) || (w->flags[i] & EF_DEAD)) continue;
        float yaw = quat_get_yaw(w->xf[i].rot), pitch = w->anim[i];
        vec3 f = dir_from_angles(yaw, pitch);
        vec3 eye = v3_add(w->xf[i].pos, v3(0, (w->flags[i] & EF_CROUCHED) ? PLAYER_EYE_CROUCH : PLAYER_EYE_STAND, 0));
        s.pos = v3_madd(eye, f, 0.4f);
        s.dir = f;
        render_add_spot(&s);
    }
}

void game_render(void) {
    World *w = G->world;
    if (!w) {
        ui_text_shadow(ui_width() * 0.5f, ui_height() * 0.45f, FONT_LARGE, UI_RGBA(230, 230, 230, 255), UI_ALIGN_CENTER,
                       net_role() == NET_CLIENT ? "Connecting..." : "No world loaded");
        menus_draw();
        console_draw();
        return;
    }
    // The viewed player: the local one, or the one spectated in first / third person (its own
    // re-simulated state, presented exactly like the local player's, docs/SPECTATE.md).
    Player *p = game_view_player();
    int vs = game_view_slot();
    bool spec = vs != G->local_slot;
    const UserCmd *vcmd = game_view_cmd();
    if (spectator_first_person()) spectator_view_angles(G->alpha, &G->view.yaw, &G->view.pitch);
    PlayerMove interp;
    if (p && spec && spectator_view_interp(&interp, G->alpha)) {
        // (render state from spectator.c)
    } else if (p) {
        interp = p->sim.move;
        PlayerMove prev = (G->mode == GM_CLIENT) ? G->predicted_prev : p->sim.move;
        if (G->mode == GM_HOST) {
            int i = (int)ENT_INDEX(p->ent);
            prev.pos = w->xf[i].prev_pos;
        }
        interp.pos = v3_add(v3_lerp(prev.pos, p->sim.move.pos, G->alpha), G->error_offset);
        interp.crouch_amount = lerpf(prev.crouch_amount, p->sim.move.crouch_amount, G->alpha);
        interp.stride = lerpf(prev.stride, p->sim.move.stride, G->alpha);
    } else {
        memset(&interp, 0, sizeof(interp));
        interp.pos = v3(0, 2, 10);
    }
    bool ads = p && (vcmd->buttons & BTN_AIM) && (p->sim.move.mode == MOVE_WALK || (p->sim.move.mode == MOVE_SEATED && !p->sim.move.veh_onehand));
    const WeaponDef *wd = p ? weapon_equipped_def(w, vs) : NULL;
    if (wd && wd->kind == WPN_MELEE) ads = false;
    input_set_ads(ads && !spec);   // ADS look sensitivity is for the own mouse
    bool dead = p && p->sim.move.mode == MOVE_DEAD;
    CameraAds cads;
    sights_camera(p, wd, &cads);
    RenderCamera cam = camera_update(&G->view, &interp, (float)g_engine.frame_dt, &cads, dead);
    // Weapon feel: camera kick spring (visual only, aim unaffected) + aim sway (bullets use it too).
    if (p && !dead) {
        // Fire animation layer 5 (docs/FIRE_ANIM.md): the camera follows the body's reaction 2-3 ticks
        // after the gun (its own overshoot), plus the roll twitch and the FOV punch.
        float ck = cvar_find("cam_kick_scale") ? cvar_find("cam_kick_scale")->f : 1.0f;   // visual camera kick (aim recoil unaffected)
        const FireAnimOut *fo = game_fire_sample(G->alpha);
        vec3 kr = v3_scale(v3(fo->cam_rot.x, fo->cam_rot.y, fo->cam_rot.z), ck);
        float kb = fo->cam_back * ck;
        cam.fov_deg = fmaxf(5.0f, cam.fov_deg - fo->fov_punch * ck);
        // Sway and ADS bob move the weapon (about the eye), not the camera: the sight centre stays
        // exactly on the bullets' line (weapon_aim_offset_deg is what the shots use).
        vec2 sway = weapon_aim_offset_deg(wd, &p->sim.wpn, &p->sim.move, weapon_sway_time(vcmd->seq) + G->alpha * SIM_DT);
        Cvar *rvm = cvar_find("r_viewmodel_fov");
        float vmf = render_vm_fov_for(&cam, rvm ? rvm->f : 0.0f);
        // The visual camera kick turns the view but not the aim: while aimed the weapon counters it,
        // so the reticle keeps marking exactly where the shots go (docs/RECOIL.md "Hip vs ADS").
        float ads_a = wd ? game_ads_vis() : 0.0f;
        chars_set_fp_aim_offset(sway.x - kr.y * ads_a, sway.y - kr.x * ads_a, cam.pitch + kr.x * DG_DEG2RAD, cam.fov_deg, vmf);
        cam.pitch += kr.x * DG_DEG2RAD;
        cam.yaw -= kr.y * DG_DEG2RAD;
        cam.roll += kr.z * DG_DEG2RAD;
        cam.pos = v3_madd(cam.pos, dir_from_angles(cam.yaw, cam.pitch), -kb);
    }
    // cam_weapon_fov (arms + held item projection) <-> the renderer's r_viewmodel_fov (older configs).
    {
        static u32 seen_cam, seen_r;
        Cvar *cw = camera_fp_cvars()->weapon_fov, *rv = cvar_find("r_viewmodel_fov");
        if (cw && rv) {
            CvarSource src = cvar_source_push(CVAR_SRC_HOST);   // both locked (dev builds may still set either)
            if (cw->modified != seen_cam && cw->f != rv->f) cvar_set_float(rv, cw->f);
            else if (rv->modified != seen_r && cw->f != rv->f) cvar_set_float(cw, rv->f);
            seen_cam = cw->modified;
            seen_r = rv->modified;
            cvar_source_pop(src);
        }
    }
    // True first-person camera. Order of operations (nothing after the arm IK moves the camera,
    // the hold point or the item):
    //   tick:  input -> movement -> animation (base, upper body, additive) -> spine / head look ->
    //          head eye (model space) -> hold point (camera relative) -> item on the hold point ->
    //          arm IK to grip_r / grip_l -> finger curl -> foot IK      (weapon_hold.c, procedural.c)
    //   frame: view angles -> spine / head re-bent to this frame's view (chars_fp_camera) -> read
    //          the head bone x eye socket -> camera (+ wall collision) -> hold point re-applied to
    //          this camera -> item -> arm IK -> fingers (chars_render_local_fp) -> render.
    // Rotation is always the view angles; position is the head eye.
    {
        Xform sock;
        Player *lp = p;   // the viewed player
        const CamFpCvars *cc = camera_fp_cvars();
        bool tp = cvar_find("cl_thirdperson")->i != 0 || (cc->third && cc->third->i) || (spec && spectator_mode() == SPEC_THIRD);
        float fdt = (float)g_engine.frame_dt;
        chars_set_thirdperson(tp);
        GR->cam_mode = 0;
        // Spectator free camera (noclip): no body, no hands.
        bool fcam = spectator_free_camera(&cam);
        bool pcam = !fcam && lp && peek_camera(w, &cam);   // door peek through the visor (game_peek.c)
        // A dev scene's camera (src/game/scene.h): third person, the body drawn whole.
        bool scam = !fcam && !pcam && lp && scene_camera(w, &cam);
        int vcam = lp && !scam && !fcam && !pcam ? vehicles_camera(w, &cam) : 0;
        if (fcam || pcam) {
            chars_set_thirdperson(true);   // free camera / door peek (the camera is on the far side of the door): the body stays whole
            G->view.eye = cam.pos;
            GR->cam_mode = 3;
        } else if (scam) {
            chars_set_thirdperson(true);
            G->view.eye = cam.pos;
            GR->cam_mode = 3;
        } else if (vcam) {
            G->view.eye = cam.pos;
            GR->cam_mode = vcam;
        } else if (tp && lp) {
            vec3 head = v3_add(interp.pos, v3(0, 1.6f, 0));
            cam.pos = v3_madd(head, dir_from_angles(cam.yaw, cam.pitch), -cvar_find("cl_thirdperson_dist")->f);
            cam.pos = v3_add(cam.pos, v3_scale(right_from_yaw(cam.yaw), 0.4f));
            G->view.eye = cam.pos;
            GR->cam_mode = 3;
        } else if (dead && lp) {
            vec3 head;
            if (chars_head_world(w, lp->ent, G->alpha, &head)) {
                cam.pos = camera_fp_death(&G->cam_stab, head, fdt);
                G->view.eye = cam.pos;
                GR->cam_mode = 2;
            }
        } else if (lp && cvar_find("cl_camera_socket")->i && chars_fp_camera(w, G->alpha, vs, cam.yaw, cam.pitch, &sock)) {
            // Camera = the head eye: Head bone x the "camera" socket (assets/characters/player.json),
            // read after this frame's spine / head look solve. The old capsule-based stabilizer (an
            // "ideal" capsule eye followed by a clamped spring, which left the camera up to 45 cm
            // behind the head) is kept as an optional offset scaled by cam_bob_scale (0 = none).
            // No extra smoothing: the body and the camera move as one (step-ups move both).
            float bob_k = camera_fp_cvars()->bob_scale ? camera_fp_cvars()->bob_scale->f : 0.0f;
            vec3 head = v3_add(sock.t, v3(0, G->view.land_offset * bob_k, 0));
            vec3 ideal = v3_add(v3(interp.pos.x, G->view.smooth_eye_y + G->view.land_offset, interp.pos.z), player_eye_offset(cam.yaw, cam.pitch));
            vec3 stab = camera_fp_stabilize(&G->cam_stab, ideal, head, cam.yaw, fdt);
            vec3 eye = v3_madd(head, v3_sub(stab, head), bob_k);
            // Never inside walls / ledges (mantle, vault, crouch under geometry): cast from the capsule.
            vec3 safe = v3(interp.pos.x, interp.pos.y + fminf(player_eye_height(&interp) * 0.6f, 1.0f), interp.pos.z);
            eye = camera_fp_collide(&G->cam_stab, safe, eye, cam_sphere_cast, w->phys, fdt);
            if (cc->debug && cc->debug->i) cam_penetration_sample(w->phys, safe, eye);
            cam.pos = eye;
            cam.roll = G->view.roll;
            float hr = cvar_find("cl_camera_socket_roll")->f;
            if (hr > 0) {
                vec3 rgt = quat_rotate(sock.r, v3(1, 0, 0));
                cam.roll += asinf(clampf(rgt.y, -1.0f, 1.0f)) * hr;   // a little of the head's own roll
            }
            G->view.eye = cam.pos;
            GR->cam_mode = 1;
            GR->head_eye = head;
            GR->ideal_eye = ideal;
        } else if (!dead) {
            cam.pos = v3_add(cam.pos, player_eye_offset(cam.yaw, cam.pitch));
        }
        // Spectator target switches (and entering / leaving spectating) blend the camera over
        // spec_blend_time; everything below (hold point, hands, sights) follows the blended camera.
        spectator_blend_camera(&cam, fdt);
        if (GR->cam_mode != 0) G->view.eye = cam.pos;
        chars_set_fp_camera(cam.pos, quat_euler(cam.yaw, cam.pitch, cam.roll), GR->cam_mode == 1);
        if (GR->cam_mode == 1) fp_cam_test_frame(cam.pos, dir_from_angles(cam.yaw, cam.pitch));
        // cam_debug: eye / head / ideal markers + the stats line (HUD).
        GR->cam_text[0] = 0;
        if (cc->debug && cc->debug->i && lp) {
            float gp[2], ga[2], ap, aa;
            chars_fp_diag(gp, ga, &ap, &aa);
            const char *modes[] = { "fallback", "head", "death", "third" };
            vec3 d = G->cam_stab.disp;
            snprintf(GR->cam_text, sizeof(GR->cam_text),
                     "cam %s  head-ideal %.0f %.0f %.0f mm  cam-head %.0f mm  disp %.1f %.1f %.1f mm (max %.1f, clamp %.0f)  wall %.0f mm | hands R %.2f mm %.2f deg  L %.2f mm %.2f deg | sight %.2f mm %.2f deg | %s",
                     modes[GR->cam_mode], G->cam_stab.last_raw.x * 1000, G->cam_stab.last_raw.y * 1000, G->cam_stab.last_raw.z * 1000,
                     v3_len(v3_sub(cam.pos, GR->head_eye)) * 1000, d.x * 1000, d.y * 1000, d.z * 1000, G->cam_stab.max_seen * 1000, cc->clamp->f * 1000, G->cam_stab.col_push * 1000,
                     gp[1] * 1000, ga[1], gp[0] * 1000, ga[0], ap * 1000, aa, chars_holdpoint_text(w, vs));
            if (GR->cam_mode == 1) {
                vec3 f = dir_from_angles(cam.yaw, cam.pitch);
                dbg_cross(v3_madd(GR->head_eye, f, 0.3f), 0.01f, DBG_RED, 0);       // head eye (shown 30 cm ahead)
                dbg_cross(v3_madd(GR->ideal_eye, f, 0.3f), 0.01f, DBG_GREEN, 0);    // ideal eye
                dbg_cross(v3_madd(cam.pos, f, 0.3f), 0.006f, DBG_WHITE, 0);         // camera
            }
            if (cc->debug->i >= 2 && (g_engine.frame_index % 30) == 0) log_info("cam_debug: %s", GR->cam_text);
        }
    }
    flashlights_render(w, &cam, p, dead, vs);
    terrain_game_frame(cam.pos);   // terrain level / debug / reload (before the frame's parameters)
    render_set_camera(&cam);
    // Time of day first: the post settings (exposure floor / curves, fog) follow this frame's sky.
    RenderEnvironment tod_env;
    memset(&tod_env, 0, sizeof(tod_env));
    weather_frame(w, &cam, p);
    daynight_environment(w, &cam, &tod_env);
    {
        PostFrameInfo pfi;
        memset(&pfi, 0, sizeof(pfi));
        pfi.cam_pos = cam.pos;
        pfi.ads = ads;
        pfi.sprinting = p && p->sim.move.sprinting;
        pfi.level_fog = w->level.fog_density;
        game_post_frame(&pfi);
        game_fidelity_frame();
    }

    LevelStatic *ls = &w->level;
    RenderEnvironment env;
    memset(&env, 0, sizeof(env));
    env.sun_dir = v3_len2(ls->sun_dir) > 0 ? ls->sun_dir : v3_norm(v3(0.3f, 1, 0.2f));
    env.sun_color = v3_len2(ls->sun_color) > 0 ? ls->sun_color : v3(1, 1, 1);
    env.sun_intensity = ls->sun_intensity > 0 ? ls->sun_intensity : 2.0f;
    env.sky_ambient = ls->ambient_sky;
    env.ground_ambient = ls->ambient_ground;
    env.ambient_intensity = 0.55f;
    env.fog_color = ls->fog_color;
    env.fog_density = ls->fog_density;
    env.sky = ls->sky;
    env.sky_zenith = ls->sky_zenith;
    env.sky_horizon = ls->sky_horizon;
    env.clouds = ls->clouds;
    env.shadow_center = v3_madd(cam.pos, dir_from_angles(cam.yaw, 0), 12.0f);
    env.shadow_extent = 32.0f;
    if (tod_env.atmosphere) {
        // Physically based sky + day / night (game_daynight.c): everything but the shadow extent.
        tod_env.shadow_extent = env.shadow_extent;
        env = tod_env;
    } else if (vfx_game_night()) {
        // Dev night: moonlight + dim ambient (muzzle flashes and their lights dominate).
        env.sun_intensity *= 0.03f;
        env.sun_color = v3(0.6f, 0.7f, 1.0f);
        env.ambient_intensity *= 0.08f;
        env.fog_color = v3_scale(env.fog_color, 0.05f);
    }
    render_set_environment(&env);
    vfx_game_set_environment(env.sun_intensity, wind_game_at(cam.pos));   // particles drift with the local wind (gusts)

    vec3 fwd = dir_from_angles(cam.yaw, cam.pitch);
    vehicles_frame(w, cam.pos);
    foliage_frame(w);
    game_render_world(w, G->mode == GM_HOST ? G->alpha : 1.0f, cam.pos, fwd);
    vista_render(cam.pos, env.atmosphere ? DG_CLAMP((0.12f - env.sun_true_dir.y) / 0.24f, 0.0f, 1.0f) : 0.0f);   // story levels: the city outside

    if (p && !dead) {
        HandsInput hi;
        memset(&hi, 0, sizeof(hi));
        hi.world = w;
        hi.player = vs;
        hi.eye = cam.pos;
        hi.yaw = cam.yaw;
        hi.pitch = cam.pitch;
        hi.roll = cam.roll;
        hi.look_delta = spec ? spectator_frame_look() : G->frame_look;
        hi.dt = (float)g_engine.frame_dt;
        if (spec) {
            // A spectated player's reach / interaction comes with its commands (focus = use_target).
            hi.focus = (EntityId)vcmd->use_target;
            hi.use_pressed = spectator_take_use_press();
            hi.use_held = (vcmd->buttons & BTN_USE) != 0;
            hi.use_target_hands = -1;
            if (hi.use_pressed && ent_valid(w, hi.focus)) {
                const InteractDef *def = w->inter[ENT_INDEX(hi.focus)].def;
                if (handpose_interact_has_target(def)) {
                    hi.use_target_valid = true;
                    hi.use_target = ent_focus_point(w, hi.focus);
                    hi.use_target_hands = (int)def->hands;
                }
            }
        } else {
            hi.focus = G->use_pressed_frame && G->use_focus ? G->use_focus : G->focus;
            hi.use_target_valid = G->use_pressed_frame && G->use_focus_hands >= 0;
            hi.use_target = G->use_focus_point;
            hi.use_target_hands = G->use_focus_hands;
            hi.use_pressed = G->use_pressed_frame;
            hi.use_held = (G->last_cmd.buttons & BTN_USE) != 0;
        }
        hi.ads = ads;
        {
            const FireAnimOut *fo = game_fire_sample(G->alpha);   // the empty-hands fallback rig (cl_fp_body 0)
            hi.vm_kick_pos = v3_add(fo->gun_pos, v3(0, 0, fo->screen_kick));
            hi.vm_kick_rot = fo->gun_rot;
            hands_set_fire_mech(true, fo->slide, fo->wobble_rot);
        }
        hands_update(&hi);
        bool tpv = cvar_find("cl_thirdperson")->i || GR->cam_mode == 3;   // incl. the third-person vehicle camera
        if (!tpv) {
            chars_render_local_fp(w, G->alpha, vs);
            game_fire_flush_flash();   // the muzzle flash on this frame's (kicked) gun
            fp_cam_test_hands();
            ads_sweep_frame();
        }
        if (!tpv) hands_render();
    }
    // Sights: lens / reticle / scope picture-in-picture from the item as it is drawn this frame.
    sights_frame(&cam, p && !dead && !cvar_find("cl_thirdperson")->i && GR->cam_mode != 3);
    sights_test_frame(&cam);
    // Phone: lens view from the phone as drawn this frame, companion server, capture.
    phone_frame(w, &cam, p);
    fx_render(cam.pos);
    draw_debug(w, cam.pos);
    recoil_debug_draw(cam.pos);
    anim_debug_draw();
    wind_game_draw_debug(cam.pos);
    vec3 right = right_from_yaw(cam.yaw);
    vec3 up = v3_cross(right, fwd);
    dbg_submit((float)g_engine.frame_dt, cam.pos, right, up);
    game_audio_frame(w, &cam, p, ads);   // listener state + the world the environment chain listens to
    audio_update(cam.pos, fwd, up);

    if (!game_post_photo_mode() && !scene_hides_hud()) { peek_draw(); hud_draw(); if (!menus_open()) wheels_draw(); vehicles_draw_hud(); phone_draw(); }
    vehicles_draw();
    sights_draw();
    daynight_draw();
    shadow_sky_draw();
    ground_ref_draw();
    weather_draw();
    game_audio_draw(cam.pos);
    flesh_range_draw();
    wind_game_draw_panel();
    scene_draw();
    game_post_draw_overlay();
    game_fidelity_draw();
    game_post_draw_photo_ui();
    menus_draw();
    console_draw();
}
