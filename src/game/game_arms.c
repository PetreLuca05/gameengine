// First-person arms layer (docs/CAMERA_HOLDPOINT.md "Rule 3"): assets/player/arms_layer.json, the
// per-frame evaluation (src/render/arms_layer.c) handed to the renderer, and the dev commands
// (arms_layer_reload / _save / _set / _info). The arms tuner panel (src/ui/arms_tuner.c) edits the
// same definition. The viewmodel sway (src/player/weapon_sway.c, docs/FIRE_ANIM.md "Viewmodel sway")
// is evaluated here every frame for the viewed player and handed to the layer.
#include "game_internal.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/log.h"
#include "core/platform.h"
#include "gameplay/characters.h"
#include "gameplay/weapons.h"
#include "anim/fire_anim.h"
#include "player/camera.h"
#include "player/weapon_sway.h"
#include "render/arms_layer.h"
#include "render/render.h"
#include "player/spectator.h"
#include "ui/arms_tuner.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define G (g_engine.game)
#define ARMS_JSON "assets/player/arms_layer.json"
#define VMS_JSON "assets/weapons/viewmodel_sway.json"

static struct {
    ArmsLayerDef def;
    ArmsLayerState st;
    ArmsLayerIn in;
    ArmsLayerOut out;
    CharsFpArms fa;           // the first-person arms it was built from
    bool have;                // the layer was set this frame
    Cvar *enable, *debug;
} AL;

// Viewmodel sway: the config file (defaults, per fire class, per weapon id), the resolved config of
// the held item, the state and the view / movement history it reads.
static struct {
    cJSON *root;
    WeaponSwayConfig cfg;
    u32 cfg_hash;             // the item the config was resolved for (0: none yet)
    WeaponSwayState st;
    SwayOutput out;
    int slot;                 // the viewed player (a switch resets the state)
    float yaw, pitch;         // last frame's view angles (rad)
    bool have_view, was_grounded;
    u32 land_tick;
    Cvar *on, *debug;
} VS;

ArmsLayerDef *game_arms_def(void) { return &AL.def; }
bool game_arms_last(ArmsLayerIn *in, ArmsLayerOut *out) {
    if (in) *in = AL.in;
    if (out) *out = AL.out;
    return AL.have;
}

bool game_arms_last_fp(CharsFpArms *fa) {
    if (fa) *fa = AL.fa;
    return AL.have;
}

bool game_arms_load(void) {
    char path[600];
    size_t size = 0;
    char *txt = platform_read_file(platform_data_path(ARMS_JSON, path, sizeof(path)), &size);
    cJSON *root = txt ? cJSON_Parse(txt) : NULL;
    arms_layer_parse(&AL.def, root);
    bool ok = root != NULL;
    if (!ok) log_warn("arms layer: cannot read %s (defaults)", path);
    cJSON_Delete(root);
    free(txt);
    arms_layer_reset(&AL.st);
    return ok;
}

bool game_arms_save(const char *path_override) {
    char *out = arms_layer_to_json(&AL.def);
    char path[600];
#ifdef DG_SOURCE_DIR
    if (!path_override) snprintf(path, sizeof(path), "%s/%s", DG_SOURCE_DIR, ARMS_JSON);
#else
    if (!path_override) platform_data_path(ARMS_JSON, path, sizeof(path));
#endif
    if (path_override) snprintf(path, sizeof(path), "%s", path_override);
    FILE *f = fopen(path, "wb");
    bool ok = f != NULL;
    if (f) { fputs(out, f); fputc('\n', f); fclose(f); }
    // Runtime copy too (a reload keeps the edit when running from the build).
    char rt[600];
    snprintf(rt, sizeof(rt), "%s%s", platform_base_dir(), ARMS_JSON);
    if (ok && !path_override && strcmp(rt, path)) {
        FILE *g = fopen(rt, "wb");
        if (g) { fputs(out, g); fputc('\n', g); fclose(g); }
    }
    free(out);
    log_info("arms_layer_save: %s %s", ok ? "wrote" : "cannot write", path);
    return ok;
}

static void cmd_reload(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    log_info("arms_layer_reload: %s", game_arms_load() ? "ok" : "defaults");
}

static bool vms_load(void) {
    char path[600];
    size_t size = 0;
    char *txt = platform_read_file(platform_data_path(VMS_JSON, path, sizeof(path)), &size);
    cJSON_Delete(VS.root);
    VS.root = txt ? cJSON_Parse(txt) : NULL;
    free(txt);
    VS.cfg_hash = 0;   // re-resolved on the next frame
    if (!VS.root) log_warn("viewmodel sway: cannot read %s (built-in defaults)", path);
    return VS.root != NULL;
}

// The held item's config: built-in defaults < "default" < "classes"[fire class] < "weapons"[item id].
static void vms_resolve(const char *item_id, const WeaponDef *wd) {
    weapon_sway_defaults(&VS.cfg);
    if (!VS.root) return;
    weapon_sway_parse(&VS.cfg, cJSON_GetObjectItemCaseSensitive(VS.root, "default"));
    const char *cls = wd && wd->fire.cls >= 0 ? fire_class_name(wd->fire.cls) : NULL;
    if (cls) weapon_sway_parse(&VS.cfg, cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(VS.root, "classes"), cls));
    if (item_id) weapon_sway_parse(&VS.cfg, cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(VS.root, "weapons"), item_id));
}

static void cmd_vms_reload(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    log_info("viewmodel_sway_reload: %s", vms_load() ? "ok" : "defaults");
}

static void vms_info(void) {
    const SwayOutput *o = &VS.out;
    static const char *names[WSW_LAYERS] = { "look", "move", "bob" };
    log_info("viewmodel_sway_info: %s, total rot %.2f %.2f %.2f deg pos %.1f %.1f %.1f mm, sprint %.2f, bob amp %.2f phase %.2f", VS.on && VS.on->i ? "on" : "off",
             o->rot_deg.x, o->rot_deg.y, o->rot_deg.z, o->pos_offset.x * 1000, o->pos_offset.y * 1000, o->pos_offset.z * 1000, o->sprint_w, VS.st.bob_amt,
             VS.st.bob_phase);
    for (int i = 0; i < WSW_LAYERS; i++)
        log_info("viewmodel_sway_info:   %-4s rot %.2f %.2f %.2f deg pos %.1f %.1f %.1f mm", names[i], o->layer_rot[i].x, o->layer_rot[i].y, o->layer_rot[i].z,
                 o->layer_pos[i].x * 1000, o->layer_pos[i].y * 1000, o->layer_pos[i].z * 1000);
}

static void cmd_vms_info(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    vms_info();
}

static u32 id_hash(const char *s) {
    u32 h = 2166136261u;
    for (; s && *s; s++) h = (h ^ (u8)*s) * 16777619u;
    return h ? h : 1u;
}

static float wrap_pi(float a) {
    while (a > DG_PI) a -= 2.0f * DG_PI;
    while (a < -DG_PI) a += 2.0f * DG_PI;
    return a;
}

// This frame's viewmodel sway for the viewed player (false: off, nothing held, the hand pose editor's
// still pose). Cosmetic: read from the view and the movement state, never written back.
static bool vms_frame(const CharsFpArms *fa, Player *p, float dt) {
    Cvar *hs = cvar_find("holdpoint_static");
    if (!VS.on || !VS.on->i || !p || !fa->has_item || (hs && hs->i)) {
        weapon_sway_reset(&VS.st);
        VS.have_view = false;
        memset(&VS.out, 0, sizeof(VS.out));
        return false;
    }
    int slot = game_view_slot();
    if (slot != VS.slot) {
        weapon_sway_reset(&VS.st);
        VS.have_view = false;
        VS.slot = slot;
    }
    u32 h = id_hash(fa->item_id);
    if (h != VS.cfg_hash) {
        const WeaponDef *wd = weapon_equipped_def(G->world, slot);
        vms_resolve(fa->item_id, wd && !strcmp(wd->id, fa->item_id ? fa->item_id : "") ? wd : NULL);
        VS.cfg_hash = h;
    }
    const PlayerMove *m = &p->sim.move;
    WeaponSwayInput in;
    memset(&in, 0, sizeof(in));
    // The view's rotation this frame (deg, right / up): the player's view angles, never the camera
    // kick (the recoil stays the fire animation's own).
    float yaw = G->view.yaw, pitch = G->view.pitch;
    if (VS.have_view) {
        in.mouse_dx = -wrap_pi(yaw - VS.yaw) * DG_RAD2DEG;
        in.mouse_dy = (pitch - VS.pitch) * DG_RAD2DEG;
    }
    VS.yaw = yaw;
    VS.pitch = pitch;
    VS.have_view = true;
    in.aim_yaw = yaw;
    in.aim_pitch = pitch;
    in.velocity = m->vel;
    vec3 right = v3(cosf(yaw), 0, -sinf(yaw)), fwd = v3(-sinf(yaw), 0, -cosf(yaw));
    in.velocity_local = v3(v3_dot(m->vel, right), m->vel.y, v3_dot(m->vel, fwd));
    in.speed = sqrtf(m->vel.x * m->vel.x + m->vel.z * m->vel.z);
    in.grounded = m->on_ground && m->mode == MOVE_WALK;
    in.just_landed = m->land_tick != VS.land_tick;
    in.land_impact_speed = m->land_speed;
    in.just_jumped = VS.was_grounded && !in.grounded && m->vel.y > 0.5f;
    VS.land_tick = m->land_tick;
    VS.was_grounded = in.grounded;
    in.crouching = m->crouched || m->crouch_amount > 0.5f;
    in.sprinting = m->sprinting != 0;
    in.ads_blend = saturate(fa->ads_w);
    in.is_mantle = m->mode == MOVE_MANTLE || m->mode == MOVE_VAULT;
    in.bob_scale = 1.0f - saturate(fa->clip_w);   // a weapon's own run clip carries the gun: no second bob
    weapon_sway_update(&VS.st, &VS.cfg, &in, dt, &VS.out);
    if (VS.debug && VS.debug->i && (g_engine.frame_index % 30) == 0) vms_info();
    return true;
}
static void cmd_save(int argc, const char **argv) {
    if (cvar_release_locks()) { log_info("arms_layer_save: dev builds only"); return; }
    game_arms_save(argc > 1 ? argv[1] : NULL);
}

// arms_layer_set <state> <offset|rotation|scale|fov_add|camera|camera_rot|blend> <x> [y z] [item]
static void cmd_set(int argc, const char **argv) {
    if (argc < 4) { log_info("arms_layer_set <state> <offset|rotation|scale|fov_add|camera|camera_rot|blend> <x> [y z] [item]"); return; }
    if (cvar_release_locks()) { log_info("arms_layer_set: dev builds only"); return; }
    int s = holdpoint_state_from_name(argv[1]);
    if (s < 0) { log_info("arms_layer_set: unknown state '%s'", argv[1]); return; }
    bool vec = !strcmp(argv[2], "offset") || !strcmp(argv[2], "rotation") || !strcmp(argv[2], "camera") || !strcmp(argv[2], "camera_rot");
    if (vec && argc < 6) { log_info("arms_layer_set: %s takes x y z", argv[2]); return; }
    const char *item = argc > (vec ? 6 : 4) ? argv[vec ? 6 : 4] : NULL;
    if (!strcmp(argv[2], "blend")) { AL.def.blend[s] = fmaxf(0.0f, (float)atof(argv[3])); return; }
    ArmsPose *p = arms_layer_edit(&AL.def, s, item);
    if (!strcmp(argv[2], "offset")) p->offset = v3(clampf((float)atof(argv[3]), -0.3f, 0.3f), clampf((float)atof(argv[4]), -0.3f, 0.3f), clampf((float)atof(argv[5]), -0.3f, 0.3f));
    else if (!strcmp(argv[2], "rotation")) p->rotation = v3((float)atof(argv[3]), (float)atof(argv[4]), (float)atof(argv[5]));
    else if (!strcmp(argv[2], "scale")) p->scale = clampf((float)atof(argv[3]), 0.9f, 1.1f);
    else if (!strcmp(argv[2], "fov_add")) p->fov_add = clampf((float)atof(argv[3]), -20.0f, 20.0f);
    else if (!strcmp(argv[2], "camera")) p->cam_pos = v3((float)atof(argv[3]), (float)atof(argv[4]), (float)atof(argv[5]));
    else if (!strcmp(argv[2], "camera_rot")) p->cam_rot = v3(clampf((float)atof(argv[3]), -15.0f, 15.0f), clampf((float)atof(argv[4]), -15.0f, 15.0f), clampf((float)atof(argv[5]), -15.0f, 15.0f));
    else { log_info("arms_layer_set: unknown field '%s'", argv[2]); return; }
    log_info("arms_layer_set: %s%s%s %s", holdpoint_state_name(s), item ? " / " : "", item ? item : "", argv[2]);
}

static void cmd_info(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    if (!AL.have) { log_info("arms_layer_info: no arms layer this frame"); return; }
    const ArmsLayerIn *in = &AL.in;
    vec3 t = v3(AL.out.layer.m[12], AL.out.layer.m[13], AL.out.layer.m[14]);
    log_info("arms_layer_info: state %s item %s ads %.2f raise %.2f fov hands %.1f world %.1f | layer move %.1f %.1f %.1f mm | present %.1f %.1f %.1f mm",
             holdpoint_state_name(in->hp_state), in->item_id ? in->item_id : "-", in->ads_w, AL.out.raise, in->fov_hands, in->fov_world, t.x * 1000, t.y * 1000,
             t.z * 1000, in->present.t.x * 1000, in->present.t.y * 1000, in->present.t.z * 1000);
}

void game_arms_init(void) {
    AL.enable = cvar_reg_bool("arms_layer", true, CVAR_LOCKED,
                              "first-person arms layer: the arms + held item drawn with their own camera / FOV and presented ADS (0 = the world camera, no ADS presentation)");
    AL.debug = cvar_reg_int("arms_layer_debug", 0, 0, 1, CVAR_LOCKED, "1: log the arms layer every 30 frames");
    cmd_register("arms_layer_reload", cmd_reload, "reload assets/player/arms_layer.json (the first-person arms layer)");
    cmd_register("arms_layer_save", cmd_save, "write the arms layer definition [path] (the source tree's assets/player/arms_layer.json)");
    cmd_register("arms_layer_set", cmd_set, "arms_layer_set <state> <offset|rotation|scale|fov_add|camera|camera_rot|blend> <x> [y z] [item]: edit the arms layer live");
    cmd_register("arms_layer_info", cmd_info, "print this frame's arms layer (state, raise, FOVs, the layer move)");
    VS.on = cvar_reg_bool("cl_viewmodel_sway", true, CVAR_LOCKED,
                          "viewmodel sway (cosmetic, first person only): look lag, movement sway, walk / sprint bob and the sprint pose of the drawn weapon (assets/weapons/viewmodel_sway.json); 0 = the hold point's look lag + bob");
    VS.debug = cvar_reg_int("viewmodel_sway_debug", 0, 0, 1, CVAR_LOCKED, "1: log the viewmodel sway layers every 30 frames");
    cmd_register("viewmodel_sway_reload", cmd_vms_reload, "re-read assets/weapons/viewmodel_sway.json (the viewmodel sway config)");
    cmd_register("viewmodel_sway_info", cmd_vms_info, "print this frame's viewmodel sway: the total and each layer");
    VS.slot = -1;
    vms_load();
    arms_tuner_init();
    game_arms_load();
}

float game_arms_hip_fov(void) { return camera_hands_fov(AL.def.weapon_fov); }

// The sprint's widening of the world FOV (camera.c: the hip FOV's tangent x (1 + cl_sprint_fov),
// smoothed; cam->vm_fov_deg is that hip FOV) as a tangent ratio against Field of View, 1 otherwise.
static float sprint_widen(const RenderCamera *cam) {
    float base = camera_base_fov();
    if (cam->vm_fov_deg <= 1.0f || base <= 1.0f) return 1.0f;
    return fmaxf(1.0f, tanf(cam->vm_fov_deg * 0.5f * DG_DEG2RAD) / tanf(base * 0.5f * DG_DEG2RAD));
}

// The arms FOV of this frame: Weapon Field of View (Hor+ like the world FOV, never the world's) plus
// the state's fov_add, widened like the world while sprinting (the gun and hands zoom out with the
// world instead of holding still against it); aimed, the sight's own FOV (render_vm_fov_for), the
// same for every preset.
static float hands_fov(const RenderCamera *cam, float fov_add) {
    RenderCamera hc = *cam;
    float v = clampf(game_arms_hip_fov() + fov_add, 20.0f, 120.0f);
    hc.vm_fov_deg = 2.0f * atanf(tanf(v * 0.5f * DG_DEG2RAD) * sprint_widen(cam)) * DG_RAD2DEG;
    return render_vm_fov_for(&hc, 0.0f);
}

void game_arms_frame(const RenderCamera *cam, vec2 aim_deg, float aim_pitch, float screen_kick) {
    AL.have = false;
    CharsFpArms fa;
    // No layer under a camera that isn't the player's eye (flythroughs, a spectator switching targets):
    // the arms then stay where the world camera sees them.
    if (!AL.enable || !AL.enable->i || render_camera_overridden() || spectator_blending() || !chars_fp_arms(&fa)) {
        render_set_arms_layer(NULL);
        return;
    }
    Player *p = game_view_player();
    AL.fa = fa;
    ArmsLayerIn *in = &AL.in;
    memset(in, 0, sizeof(*in));
    // A weapon's own run clip carrying the gun (controller "carry"): the arms camera glides from the
    // head's eye to the world camera, so the run's motion shows on screen (the presentation lets go of
    // the gun as well: the clip owns the arms, proc_presented_move).
    in->pose_eye = fa.pose_eye;
    if (fa.clip_w > 0.0f) in->pose_eye.t = v3_lerp(fa.pose_eye.t, cam->pos, saturate(fa.clip_w));
    in->present = fa.present;
    in->has_item = fa.has_item;
    in->item_cam = fa.item_cam;
    in->pivot = fa.pivot;
    in->sight = fa.sight;
    in->length = fa.length;
    in->item_id = fa.item_id;
    in->hp_state = fa.hp_state;
    in->ads_w = fa.ads_w;   // the presentation hold point's own ADS weight (the aim claims use the same)
    // The raise follows this frame, not the 60 Hz tick.
    in->equip_t = p && fa.has_item ? fminf(p->sim.wpn.equip_t + G->alpha * SIM_DT, AL.def.equip_time + 1.0f) : -1.0f;
    in->aim_deg = aim_deg;
    in->aim_pitch = aim_pitch;
    in->fov_world = cam->fov_deg;
    in->fov_hands = hands_fov(cam, AL.st.cur.fov_add * (1.0f - in->ads_w));   // last frame's fov_add (the aim needs the FOV first)
    in->kick_pos = fa.kick_pos;
    in->kick_rot = fa.kick_rot;
    const WeaponCvars *wc = weapons_cvars();
    in->kick_scale = wc && wc->visual_scale ? wc->visual_scale->f : 1.0f;
    in->sway_hip_rot = fa.sway_hip_rot;
    in->sway_hip_pos = fa.sway_hip_pos;
    in->sway_ads_rot = fa.sway_ads_rot;
    in->sway_scale = wc && wc->sway_cosmetic ? wc->sway_cosmetic->f : 1.0f;
    in->lb_hip_rot = fa.lb_hip_rot;
    in->lb_hip_pos = fa.lb_hip_pos;
    in->lb_ads_rot = fa.lb_ads_rot;
    in->vm_on = vms_frame(&fa, p, (float)g_engine.frame_dt);
    in->vm_pos = VS.out.pos_offset;
    in->vm_rot = VS.out.rot_deg;
    in->screen_kick = screen_kick;
    // Weapon Field of View: sideways with the preset so the sight keeps its place (against Default).
    in->lateral_scale = fov_lateral_scale(game_arms_hip_fov(), camera_fov_v(AL.def.weapon_fov[WEAPON_FOV_DEFAULT]));
    in->dt = (float)g_engine.frame_dt;
    arms_layer_eval(&AL.def, &AL.st, in, &AL.out);
    RenderArmsLayer rl;
    memset(&rl, 0, sizeof(rl));
    rl.valid = true;
    rl.view = AL.out.view;
    rl.fov_deg = hands_fov(cam, AL.out.fov_add);
    rl.znear = AL.def.znear;
    rl.arm_cut = AL.def.arm_cut * AL.out.cam_w;   // aimed the camera is on the eye: nothing to cut, the hands stay whole
    rl.shoulder[0] = fa.shoulder[0];
    rl.shoulder[1] = fa.shoulder[1];
    rl.seam_r0 = AL.def.seam_r0;   // (seam_r1 0: off, the arms camera draws only the arms + item)
    rl.seam_r1 = AL.def.seam ? AL.def.seam_r1 : 0.0f;
    rl.screen_scale = AL.out.screen_scale;   // smaller on screen about the anchor at the hip, 1 aimed
    rl.screen_anchor = AL.out.screen_anchor;
    in->fov_hands = rl.fov_deg;
    render_set_arms_layer(&rl);
    AL.have = true;
    if (AL.debug && AL.debug->i && (g_engine.frame_index % 30) == 0) cmd_info(0, NULL);
}
