#include "vfx.h"
#include "vfx_curve.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/log.h"
#include "core/platform.h"
#include "render/render.h"
#include "render/mesh_gen.h"
#include "render/debug_draw.h"
#include "physics/physics.h"
#include "audio/audio.h"
#include "game/game.h"
#include "gameplay/effects.h"
#include "world/world.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VFX_TICK (1.0f / 60.0f)
#define MAX_FRAME_SPRITES 8192
#define MAX_FRAME_DECALS 4096

typedef struct VfxGame {
    VfxLibrary lib;
    VfxSystem sys;
    const BasicMeshes *meshes;
    bool loaded;
    float accum;
    float sun_intensity;
    vec3 wind;
    u32 local_muzzle;          // handle of the latest first-person flash (follows the barrel)
    vec3 muzzle_pos, muzzle_dir;
    bool muzzle_valid;
    RenderVfxSprite sprites[MAX_FRAME_SPRITES];
    RenderVfxDecal decals[MAX_FRAME_DECALS];
    vec4 instances[VFX_MAX_INSTANCES * 2];
    float sort_d[MAX_FRAME_SPRITES];
    int sort_idx[MAX_FRAME_SPRITES];
    char stats[256];
    Cvar *enable, *quality, *debug, *half_res, *lighting, *decal_max, *timescale, *predict, *night, *gore;
    // dev room muzzle pedestal
    bool pedestal;
    int ped_type, ped_device, ped_shots, ped_fixed;
    float ped_timer;
    u32 quality_mod;
} VfxGame;

static VfxGame *V;

static const float k_quality_mul[4] = { 0.35f, 0.65f, 1.0f, 1.35f };
static const int k_quality_sort[4] = { 8192, 32768, 65536, 131072 };

// ------------------------------------------------------------------ callbacks

static bool cb_raycast(void *ctx, vec3 from, vec3 delta, vec3 *hit, vec3 *normal, int *material) {
    (void)ctx;
    World *w = game_world();
    if (!w || !w->phys) return false;
    RayHit rh;
    if (!phys_raycast(w->phys, from, delta, PL_MASK_WORLD, PHYS_INVALID_BODY, &rh)) return false;
    *hit = rh.point;
    *normal = rh.normal;
    *material = rh.material | (rh.user ? 0x100 : 0);   // 0x100: not level static (no decals on props / doors)
    return true;
}

// Material -> sound class (impacts, casings, debris): hard / metal / wood / soft / glass / water / flesh.
static const char *surface_sound_class(int m) {
    switch (m) {
    case SURF_METAL_THIN: case SURF_METAL_THICK: return "metal";
    case SURF_WOOD: return "wood";
    case SURF_DIRT: case SURF_SAND: case SURF_GRASS: case SURF_CLOTH: return "soft";
    case SURF_GLASS: return "glass";
    case SURF_WATER: return "water";
    case SURF_FLESH: return "flesh";
    default: return "hard";
    }
}

// "<mesh>_<material>" from the CPU tier ("casing_rifle_concrete") -> "<kind>_<class>" ("casing_hard").
static void cb_sound(void *ctx, const char *name, vec3 pos, float volume) {
    (void)ctx;
    char kind[32];
    int n = 0;
    while (name[n] && name[n] != '_' && n < 31) { kind[n] = name[n]; n++; }
    kind[n] = 0;
    const char *mat = strrchr(name, '_');
    int m = mat ? surface_from_name(mat + 1) : -1;
    if (m < 0 && mat) {
        // two-word materials (metal_thin)
        const char *p = name;
        while ((p = strchr(p, '_')) != NULL) { p++; if ((m = surface_from_name(p)) >= 0) break; }
    }
    const char *cls = surface_sound_class(m < 0 ? SURF_DEFAULT : m);
    if (!strcmp(kind, "shard") && m == SURF_GLASS) { audio_play("shard_glass", pos, volume, SND_SFX); return; }
    if (!strcmp(kind, "shell")) cls = (m == SURF_DIRT || m == SURF_SAND || m == SURF_GRASS || m == SURF_WATER) ? "soft" : "hard";
    if (!strcmp(kind, "casing") && m == SURF_FLESH) cls = "soft";
    if (strcmp(kind, "casing") && strcmp(kind, "shell")) {
        strcpy(kind, "debris");
        cls = (!strcmp(cls, "soft") || !strcmp(cls, "water") || !strcmp(cls, "flesh")) ? "soft" : "hard";
    }
    char snd[64];
    snprintf(snd, sizeof(snd), "%s_%s", kind, cls);
    audio_play(snd, pos, volume, SND_SFX);
}

// ------------------------------------------------------------------ library

static void load_library(void) {
    char dir[512];
    platform_data_path("assets/vfx", dir, sizeof(dir));
    memset(&V->lib, 0, sizeof(V->lib));
    V->loaded = vfx_lib_load(&V->lib, dir);
    vfx_sys_init(&V->sys, &V->lib);
    V->sys.raycast = cb_raycast;
    V->sys.sound = cb_sound;
    V->sys.cb_ctx = V;
    render_vfx_set_library(&V->lib);
    log_info("vfx: %d effects, %d emitters, %d textures (%d errors)", V->lib.effect_count, V->lib.emitter_count, V->lib.texture_count,
             V->lib.load_errors);
}

static void cmd_vfx_reload(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    vfx_game_reload();
}

static void cmd_vfx_spawn(int argc, const char **argv) {
    if (argc < 2) { log_info("usage: vfx_spawn <effect> [distance]"); return; }
    Player *p = game_local_player();
    World *w = game_world();
    if (!p || !w) return;
    vec3 eye = player_eye_pos(&p->sim.move);
    vec3 dir = dir_from_angles(p->sim.move.yaw, p->sim.move.pitch);
    float dist = argc > 2 ? (float)atof(argv[2]) : 3.0f;
    VfxSpawnParams sp = vfx_spawn_params(v3_madd(eye, dir, dist));
    RayHit rh;
    if (phys_raycast(w->phys, eye, v3_scale(dir, dist), PL_MASK_WORLD, PHYS_INVALID_BODY, &rh)) {
        sp.pos = rh.point;
        sp.normal = rh.normal;
        sp.material = rh.material;
        sp.decal_ok = rh.user == 0;
    }
    sp.incoming = dir;
    sp.rot = quat_euler(p->sim.move.yaw, p->sim.move.pitch, 0);
    if (!vfx_game_spawn(argv[1], &sp)) log_warn("vfx_spawn: '%s' not spawned (unknown effect or culled)", argv[1]);
}

// Every effect on a grid in front of the player (GPU test / overview).
static void cmd_vfx_spawn_all(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    Player *p = game_local_player();
    if (!p) return;
    vec3 eye = player_eye_pos(&p->sim.move);
    vec3 f = dir_from_angles(p->sim.move.yaw, 0), r = right_from_yaw(p->sim.move.yaw);
    int n = 0;
    for (int i = 0; i < V->lib.effect_count; i++) {
        if (!strcmp(V->lib.effects[i].name, "stress_test")) continue;
        int col = n % 8, row = n / 8;
        vec3 pos = v3_add(v3_madd(eye, f, 4.0f + (float)row * 1.2f), v3_scale(r, ((float)col - 3.5f) * 0.9f));
        pos.y = eye.y - 0.4f;
        VfxSpawnParams sp = vfx_spawn_params(pos);
        sp.normal = v3_neg(f);
        sp.incoming = f;
        sp.rot = quat_euler(p->sim.move.yaw - DG_PI * 0.5f, 0, 0);
        sp.decal_ok = false;
        sp.material = SURF_CONCRETE;
        vfx_sys_spawn(&V->sys, i, &sp);
        n++;
    }
    log_info("vfx_spawn_all: %d effects", n);
}

static void cmd_vfx_list(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    for (int i = 0; i < V->lib.effect_count; i++) log_info("  %-28s %d emitters (%s)", V->lib.effects[i].name, V->lib.effects[i].emitter_count, V->lib.effects[i].source);
}

static void cmd_vfx_clear(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    vfx_sys_clear(&V->sys);
    RenderVfxFrame f;
    memset(&f, 0, sizeof(f));
    f.reset = true;
    render_vfx_frame(&f);
}

// Particle stress test: N GPU particles spawned in front of the camera over one second.
static void cmd_vfx_stress(int argc, const char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 1000000;
    int e = vfx_lib_find_effect(&V->lib, "stress_test");
    if (e < 0) { log_warn("vfx_stress: effect 'stress_test' missing"); return; }
    Player *p = game_local_player();
    vec3 pos = p ? v3_madd(player_eye_pos(&p->sim.move), dir_from_angles(p->sim.move.yaw, 0), 8.0f) : v3(0, 2, 0);
    VfxSpawnParams sp = vfx_spawn_params(pos);
    sp.count_mul = (float)n / 600000.0f;   // stress_test: 600k particles over 1 s at scale 1
    vfx_game_spawn_id(e, &sp);
    log_info("vfx_stress: %d particles requested", n);
}

// Dev room muzzle pedestal: fires the selected flash (or cycles every type x device) sideways
// from the pedestal top so it can be judged from the side and the front.
static void cmd_vfx_pedestal(int argc, const char **argv) {
    if (argc > 1 && !strcmp(argv[1], "off")) { V->pedestal = false; return; }
    V->pedestal = true;
    V->ped_fixed = 0;
    V->ped_shots = 0;
    V->ped_type = VFX_MUZZLE_RIFLE;
    V->ped_device = VFX_DEV_BARE;
    if (argc > 1) {
        int t = vfx_muzzle_type_from_name(argv[1]);
        if (t >= 0) { V->ped_type = t; V->ped_fixed = 1; }
        if (argc > 2) {
            int d = vfx_muzzle_device_from_name(argv[2]);
            if (d >= 0) V->ped_device = d;
        }
    }
    log_info("vfx_pedestal: %s", V->ped_fixed ? "fixed" : "cycling all types x devices");
}

static void cmd_vfx_daynight(int argc, const char **argv) {
    int v = argc > 1 ? atoi(argv[1]) : !V->night->i;
    cvar_set_int(V->night, v);
    log_info("environment: %s", v ? "night" : "day");
}

static void pedestal_tick(float dt) {
    if (!V->pedestal) return;
    V->ped_timer -= dt;
    if (V->ped_timer > 0) return;
    V->ped_timer = 0.3f;
    vec3 pos = v3(-25.0f, 1.12f, -6.0f);
    vfx_game_muzzle(pos, v3(1, 0, 0), V->ped_type, V->ped_device, false);
    if (!V->ped_fixed && ++V->ped_shots >= 6) {
        V->ped_shots = 0;
        if (++V->ped_device >= VFX_DEV_COUNT) {
            V->ped_device = 0;
            V->ped_type = (V->ped_type + 1) % VFX_MUZZLE_TYPE_COUNT;
        }
        log_info("vfx_pedestal: muzzle_%s_%s", vfx_muzzle_type_name(V->ped_type), vfx_muzzle_device_name(V->ped_device));
    }
}

int vfx_game_gore(void) { return V ? DG_CLAMP(V->gore->i, 0, 2) : 2; }

bool vfx_game_night(void) { return V && V->night->i; }

void vfx_game_init(const BasicMeshes *meshes) {
    V = (VfxGame *)calloc(1, sizeof(VfxGame));
    V->meshes = meshes;
    V->sun_intensity = 2.0f;
    V->enable = cvar_reg_bool("vfx_enable", true, CVAR_ARCHIVE, "particle effects (muzzle flashes, impacts)");
    V->quality = cvar_reg_int("vfx_quality", 2, 0, 3, CVAR_ARCHIVE, "VFX quality: 0 low, 1 medium, 2 high, 3 ultra");
    V->debug = cvar_reg_int("vfx_debug", 0, 0, 2, 0, "VFX debug: 1 stats, 2 + instance markers");
    V->half_res = cvar_reg_bool("vfx_half_res", false, CVAR_ARCHIVE, "render particles at half resolution (post stack upsample)");
    V->lighting = cvar_reg_bool("vfx_lighting", true, CVAR_ARCHIVE, "lit particles (sun, shadows, point lights, 6-way maps)");
    V->decal_max = cvar_reg_int("vfx_decal_max", 256, 0, 4096, CVAR_ARCHIVE, "maximum impact decals (oldest fade first)");
    V->timescale = cvar_reg_float("vfx_timescale", 1.0f, 0.0f, 4.0f, CVAR_CHEAT, "VFX time scale (editor slow motion)");
    V->predict = cvar_reg_bool("cl_predict_impacts", true, CVAR_ARCHIVE, "show own bullet impacts immediately (predicted)");
    V->night = cvar_reg_bool("env_night", false, 0, "night lighting (dev: muzzle flash day/night look)");
    V->gore = cvar_reg_int("gore_level", 2, 0, 2, CVAR_ARCHIVE, "gore: 0 off (dust puffs, no blood decals), 1 low, 2 high");
    cmd_register("vfx_pedestal", cmd_vfx_pedestal, "vfx_pedestal [type [device] | off]: dev room muzzle flash pedestal");
    cmd_register("vfx_daynight", cmd_vfx_daynight, "vfx_daynight [0|1]: toggle night lighting");
    cmd_register("vfx_reload", cmd_vfx_reload, "reload assets/vfx effects and textures");
    cmd_register("vfx_spawn", cmd_vfx_spawn, "vfx_spawn <effect> [dist]: spawn an effect where you look");
    cmd_register("vfx_list", cmd_vfx_list, "list loaded effects");
    cmd_register("vfx_spawn_all", cmd_vfx_spawn_all, "spawn every effect on a grid in front of you");
    cmd_register("vfx_clear", cmd_vfx_clear, "remove all particles and decals");
    cmd_register("vfx_stress", cmd_vfx_stress, "vfx_stress [count]: GPU particle stress test (default 1M)");
    load_library();
}

void vfx_game_shutdown(void) {
    free(V);
    V = NULL;
}

void vfx_game_reload(void) {
    if (!V) return;
    load_library();
}

bool vfx_game_active(void) { return V && V->enable->i && V->lib.effect_count > 0; }
bool fx_predict_impacts(void) { return !V || V->predict->i; }
VfxSystem *vfx_game_system(void) { return V ? &V->sys : NULL; }
VfxLibrary *vfx_game_library(void) { return V ? &V->lib : NULL; }

void vfx_game_set_environment(float sun_intensity, vec3 wind) {
    if (!V) return;
    V->sun_intensity = sun_intensity;
    V->wind = wind;
}

// ------------------------------------------------------------------ spawning

u32 vfx_game_spawn_id(int effect, const VfxSpawnParams *p) {
    if (!vfx_game_active() || effect < 0) return 0;
    return vfx_sys_spawn(&V->sys, effect, p);
}

u32 vfx_game_spawn(const char *effect, const VfxSpawnParams *p) {
    if (!vfx_game_active()) return 0;
    return vfx_game_spawn_id(vfx_lib_find_effect(&V->lib, effect), p);
}

static quat rot_from_dir(vec3 d) {
    d = v3_norm(d);
    float yaw = atan2f(-d.x, -d.z), pitch = asinf(clampf(d.y, -1, 1));
    return quat_euler(yaw, pitch, 0);
}

void vfx_game_impact(vec3 pos, vec3 normal, vec3 incoming, int material, int caliber, u32 flags) {
    if (!vfx_game_active()) return;
    bool head = (flags & 1u) != 0, flesh = (flags & 2u) != 0;
    if (flesh) material = SURF_FLESH;
    int e = vfx_lib_impact_effect(&V->lib, material, head);
    VfxCaliberScale cs = vfx_caliber_scale(caliber);
    VfxSpawnParams sp = vfx_spawn_params(pos);
    sp.normal = v3_len2(normal) > 1e-6f ? v3_norm(normal) : v3(0, 1, 0);
    sp.incoming = v3_len2(incoming) > 1e-6f ? v3_norm(incoming) : v3_neg(sp.normal);
    sp.rot = rot_from_dir(sp.normal);
    sp.material = material;
    sp.scale = cs.size;
    sp.count_mul = cs.count;
    sp.energy = cs.energy;
    // Grazing hits: fewer chunks, more streaks along the surface (emitters with VFX_DIR_SURFACE).
    float graze = 1.0f - fabsf(v3_dot(sp.normal, sp.incoming));
    sp.intensity = 1.0f - graze * 0.3f;
    sp.decal_ok = (flags & 4u) != 0;
    if (vfx_sys_spawn(&V->sys, e, &sp)) {
        const char *snd = e >= 0 && V->lib.effects[e].sound[0] ? V->lib.effects[e].sound : NULL;
        char def[32];
        if (!snd) { snprintf(def, sizeof(def), "impact_%s", surface_sound_class(material)); snd = def; }
        audio_play(snd, pos, 0.55f * fminf(1.0f, cs.energy), SND_SFX);
    }
}

u32 vfx_game_muzzle(vec3 pos, vec3 dir, int type, int device, bool viewmodel) {
    if (!vfx_game_active()) return 0;
    int e = vfx_lib_muzzle_effect(&V->lib, type, device);
    VfxSpawnParams sp = vfx_spawn_params(pos);
    sp.rot = rot_from_dir(dir);
    sp.viewmodel = viewmodel;
    sp.socket = viewmodel ? 1 : 0;
    Player *p = game_local_player();
    if (viewmodel && p) sp.velocity = p->sim.move.vel;
    u32 h = vfx_sys_spawn(&V->sys, e, &sp);
    if (viewmodel) V->local_muzzle = h;
    return h;
}

void vfx_game_shell(vec3 pos, vec3 vel, int caliber) {
    if (!vfx_game_active()) return;
    const char *names[VFX_CAL_COUNT] = { "shell_pistol", "shell_pistol", "shell_rifle", "shell_shotgun", "shell_rifle", "shell_pistol" };
    int e = vfx_lib_find_effect(&V->lib, names[DG_CLAMP(caliber, 0, VFX_CAL_COUNT - 1)]);
    if (e < 0) e = vfx_lib_find_effect(&V->lib, "shell_rifle");
    VfxSpawnParams sp = vfx_spawn_params(pos);
    sp.velocity = vel;
    sp.rot = rot_from_dir(v3_len2(vel) > 1e-6f ? vel : v3(1, 0, 0));
    vfx_sys_spawn(&V->sys, e, &sp);
}

void vfx_game_set_local_muzzle(vec3 pos, vec3 dir, bool valid) {
    if (!V) return;
    V->muzzle_pos = pos;
    V->muzzle_dir = dir;
    V->muzzle_valid = valid;
}

// ------------------------------------------------------------------ simulation

static void apply_config(vec3 cam) {
    VfxConfig *c = &V->sys.cfg;
    int q = DG_CLAMP(V->quality->i, 0, 3);
    c->quality_mul = k_quality_mul[q];
    c->decal_max = V->decal_max->i;
    static const int k_blood_max[3] = { 0, 96, 256 };
    c->blood_decal_max = k_blood_max[DG_CLAMP(V->gore->i, 0, 2)];
    if (c->blood_decal_max == 0 && V->sys.decal_count > 0)   // gore switched off: no blood stays behind
        for (int i = V->sys.decal_live_n - 1; i >= 0; i--)
            if (V->sys.decals[V->sys.decal_live[i]].pool == VFX_DECAL_POOL_BLOOD) vfx_sys_decal_release(&V->sys, V->sys.decal_live[i]);
    c->timescale = V->timescale->f;
    c->camera = cam;
    c->wind = V->wind;
    c->lighting = V->lighting->i != 0;
}

void vfx_game_update(float dt) {
    if (!V) return;
    if (!V->enable->i) { V->accum = 0; return; }
    // Fixed 60 Hz steps: emitter scheduling and the CPU tier are deterministic per tick.
    V->accum += fminf(dt, 0.25f) * V->timescale->f;
    while (V->accum >= VFX_TICK) {
        pedestal_tick(VFX_TICK);
        vfx_sys_tick(&V->sys, VFX_TICK);
        V->accum -= VFX_TICK;
    }
}

// ------------------------------------------------------------------ render

static int cmp_sort(const void *a, const void *b) {
    float da = V->sort_d[*(const int *)a], db = V->sort_d[*(const int *)b];
    return da < db ? 1 : (da > db ? -1 : 0);   // far first
}

static void push_mesh(const VfxCpuParticle *p, const VfxEmitterDef *e, vec4 col) {
    if (!V->meshes) return;
    // Casings / shards: small boxes (shape from the mesh name).
    vec3 half = v3(p->size * 0.5f, p->size * 0.5f, p->size * 0.5f);
    const char *m = e->render.mesh;
    MeshId mesh = V->meshes->cube;
    if (!strncmp(m, "casing", 6) || !strncmp(m, "shell", 5)) {
        mesh = V->meshes->cylinder;
        half = v3(p->size * 0.18f, p->size * 0.5f, p->size * 0.18f);
        if (strstr(m, "shotgun")) half = v3(p->size * 0.3f, p->size * 0.5f, p->size * 0.3f);
    } else if (strstr(m, "glass")) {
        half = v3(p->size * 0.5f, p->size * 0.04f, p->size * 0.35f);
    } else if (strstr(m, "wood") || strstr(m, "splinter")) {
        half = v3(p->size * 0.12f, p->size * 0.1f, p->size * 0.5f);
    }
    DrawItem di;
    memset(&di, 0, sizeof(di));
    di.mesh = mesh;
    di.model = mat4_trs(p->pos, p->rot, v3_scale(half, 2.0f));
    di.mat = material_color(col);
    di.mat.roughness = 0.35f;
    di.mat.metallic = (!strncmp(m, "casing", 6) || strstr(m, "metal")) ? 1.0f : 0.0f;
    di.mat.flags = MAT_NO_SHADOW;
    render_draw(&di);
}

static int build_sprites(vec3 cam, int *out_add) {
    const VfxLibrary *lib = &V->lib;
    int add = 0, alpha = 0;
    static RenderVfxSprite tmp_alpha[MAX_FRAME_SPRITES];
    for (int i = 0; i < V->sys.cpu_count; i++) {
        const VfxCpuParticle *p = &V->sys.cpu[i];
        if (!p->alive) continue;
        const VfxEmitterDef *e = &lib->emitters[p->emitter];
        const VfxUpdateDef *u = &e->update;
        float tl = clampf(p->age / fmaxf(p->life, 1e-4f), 0, 1);
        float size = p->size * vfx_curve_eval1(&u->size_curve, tl);
        vec4 cc = vfx_curve_eval(&u->color_curve, tl);
        vec4 col = v4(p->color.x * cc.x, p->color.y * cc.y, p->color.z * cc.z, p->color.w * cc.w);
        u8 mode = e->render.mode;
        if (mode == VFX_RENDER_MESH) { push_mesh(p, e, col); continue; }
        if (mode == VFX_RENDER_NONE) continue;
        float frame = p->frame;
        if (u->frame_over_life && e->render.texture_id >= 0) {
            const VfxTextureMeta *m = &lib->textures[e->render.texture_id];
            int frames = m->variants > 1 ? m->variants : m->frames;
            frame = fminf(tl * (float)frames, (float)frames - 0.001f);
        }
        int segs = 1;
        if (mode == VFX_RENDER_RIBBON) segs = p->trail_count - 1;
        for (int s = 0; s < segs; s++) {
            RenderVfxSprite sp;
            memset(&sp, 0, sizeof(sp));
            vec3 pos = p->pos, vel = p->vel;
            if (mode == VFX_RENDER_RIBBON) {
                pos = p->trail[s];
                vel = v3_sub(p->trail[s + 1], p->trail[s]);
                // Fade towards the tail.
                col.w = p->color.w * cc.w * (float)(s + 1) / (float)segs;
            }
            sp.pos_size = v4(pos.x, pos.y, pos.z, size);
            sp.vel_rot = v4(vel.x, vel.y, vel.z, p->rotation * DG_DEG2RAD);
            sp.color = col;
            sp.info[0] = (u32)p->emitter;
            memcpy(&sp.info[1], &frame, 4);
            sp.info[2] = 0;
            bool additive = mode == VFX_RENDER_ADD;
            int copies = e->render.orient == VFX_ORIENT_CROSS ? 2 : 1;
            for (int c = 0; c < copies; c++) {
                sp.info[2] = c ? 0x100u : 0u;
                if (additive) {
                    if (add < MAX_FRAME_SPRITES) V->sprites[add++] = sp;
                } else if (alpha < MAX_FRAME_SPRITES) {
                    V->sort_d[alpha] = v3_len2(v3_sub(pos, cam));
                    tmp_alpha[alpha++] = sp;
                }
            }
        }
    }
    // Alpha sprites back to front after the additive block.
    int room = MAX_FRAME_SPRITES - add;
    if (alpha > room) alpha = room;
    for (int i = 0; i < alpha; i++) V->sort_idx[i] = i;
    qsort(V->sort_idx, (size_t)alpha, sizeof(int), cmp_sort);
    for (int i = 0; i < alpha; i++) V->sprites[add + i] = tmp_alpha[V->sort_idx[i]];
    *out_add = add;
    return alpha;
}

static VfxBoneFn g_bone_fn;
void vfx_game_set_bone_fn(VfxBoneFn fn) { g_bone_fn = fn; }

static bool fill_decal(const VfxDecal *d, RenderVfxDecal *o, bool *dynamic) {
    const VfxLibrary *lib = &V->lib;
    const VfxTextureMeta *m = &lib->textures[d->texture];
    if (!(m->uv.z > m->uv.x)) return false;
    vec3 pos = d->pos, nn = d->normal, tan = d->tangent;
    *dynamic = false;
    if (d->kind == VFX_DECAL_SKINNED) {
        // Bone space -> world with this frame's pose (animation or ragdoll).
        vec3 bp;
        quat br;
        if (!g_bone_fn || !g_bone_fn(d->attach, d->bone, &bp, &br)) return false;
        pos = v3_add(bp, quat_rotate(br, d->lpos));
        nn = v3_norm(quat_rotate(br, d->lnormal));
        tan = quat_rotate(br, d->ltangent);
        *dynamic = true;
    }
    float size = d->size, aspect = d->aspect > 0 ? d->aspect : 1.0f;
    if (d->grow_time > 0 && d->grow_to > size) {
        float g = clampf(d->age / d->grow_time, 0, 1);
        g = 1.0f - (1.0f - g) * (1.0f - g);   // fast, then slowing (a pool spreading, a stain soaking)
        size = size + (d->grow_to - size) * g;
    }
    if (d->kind == VFX_DECAL_DRIP && d->drip_len > 0) {
        // The drip runs down the surface: a strip growing from the impact along the down slope.
        float g = clampf(d->age / fmaxf(d->drip_time, 0.1f), 0, 1);
        g = sqrtf(g);
        vec3 down = v3_sub(v3(0, -1, 0), v3_scale(nn, -nn.y));
        down = v3_len2(down) > 1e-6f ? v3_norm(down) : v3(0, 0, 1);
        float len = fmaxf(size, d->drip_len * g);
        aspect = len / fmaxf(size, 1e-3f);
        pos = v3_madd(d->drip_start, down, len * 0.5f - size * 0.5f);
        tan = down;   // stretched along the run
    }
    vec3 t;
    if (v3_len2(tan) > 1e-6f) {
        t = v3_norm(v3_sub(tan, v3_scale(nn, v3_dot(tan, nn))));
    } else {
        vec3 up = fabsf(nn.y) > 0.9f ? v3(0, 0, 1) : v3(0, 1, 0);
        vec3 t0 = v3_norm(v3_cross(up, nn));
        vec3 b0 = v3_cross(nn, t0);
        float ca = cosf(d->angle), sa = sinf(d->angle);
        t = v3_add(v3_scale(t0, ca), v3_scale(b0, sa));
    }
    float fade = d->life > 0 ? clampf((d->life - d->age) / (d->life * 0.2f), 0, 1) : 1.0f;
    // Drying: wet blood dries darker and rougher.
    float dry = d->dry_time > 0 ? clampf(d->age / d->dry_time, 0, 1) : 0.0f;
    float dk = 1.0f + (d->dry_darken - 1.0f) * dry;
    float rough = d->roughness > 0 ? d->roughness + (0.85f - d->roughness) * dry : 1.0f;
    o->pos_size = v4(pos.x, pos.y, pos.z, size);
    o->normal_depth = v4(nn.x, nn.y, nn.z, d->depth);
    o->tangent_angle = v4(t.x, t.y, t.z, aspect);
    o->uv = m->uv;
    o->nuv = v4(0, 0, 0, 0);
    int np = 0;
    if (d->normal_tex >= 0 && lib->textures[d->normal_tex].uv.z > lib->textures[d->normal_tex].uv.x) {
        o->nuv = lib->textures[d->normal_tex].uv;
        np = lib->textures[d->normal_tex].page;
    }
    o->tint = v4(d->tint.x * dk, d->tint.y * dk, d->tint.z * dk, d->tint.w * fade);
    int frames = m->variants > 1 ? m->variants : m->frames;
    o->info[0] = (u32)DG_CLAMP(m->cols, 1, 255) | ((u32)DG_CLAMP(m->rows, 1, 255) << 8) | ((u32)DG_CLAMP(frames, 1, 65535) << 16);
    o->info[1] = (u32)(d->variant % DG_MAX(1, frames));
    o->info[2] = (u32)m->page | ((u32)np << 8);
    o->info[3] = (u32)DG_CLAMP((int)(rough * 255.0f), 0, 255) | (d->roughness > 0 ? 0x100u : 0u);
    return true;
}

// Static decals first, then the skinned ones (drawn after the characters): returns the total,
// *dyn = how many at the end are skinned.
static int build_decals(int *dyn) {
    int n = 0, nd = 0;
    static RenderVfxDecal tmp_dyn[MAX_FRAME_DECALS];
    for (int li = 0; li < V->sys.decal_live_n && n + nd < MAX_FRAME_DECALS; li++) {
        int slot = V->sys.decal_live[li];
        VfxDecal *d = &V->sys.decals[slot];
        if (d->texture < 0) continue;
        RenderVfxDecal o;
        bool dynamic;
        if (!fill_decal(d, &o, &dynamic)) {
            if (d->kind == VFX_DECAL_SKINNED) { vfx_sys_decal_release(&V->sys, slot); li--; }   // its character is gone (the last entry moved here)
            continue;
        }
        if (dynamic) tmp_dyn[nd++] = o;
        else V->decals[n++] = o;
        if (V->debug->i >= 1 && d->pool == VFX_DECAL_POOL_BLOOD) {
            // Projection box (blood decals): tangent stretched by the aspect, depth along the normal.
            vec3 c0 = v3(o.pos_size.x, o.pos_size.y, o.pos_size.z), nn = v3(o.normal_depth.x, o.normal_depth.y, o.normal_depth.z);
            vec3 t = v3(o.tangent_angle.x, o.tangent_angle.y, o.tangent_angle.z), bb = v3_cross(nn, t);
            float hx = o.pos_size.w * 0.5f * o.tangent_angle.w, hy = o.pos_size.w * 0.5f, hz = o.normal_depth.w;
            vec3 cr[8];
            for (int k = 0; k < 8; k++)
                cr[k] = v3_add(c0, v3_add(v3_scale(t, (k & 1) ? hx : -hx), v3_add(v3_scale(bb, (k & 2) ? hy : -hy), v3_scale(nn, (k & 4) ? hz : -hz))));
            static const u8 ed[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
            u32 colr = dynamic ? DBG_MAGENTA : (d->kind == VFX_DECAL_DRIP ? DBG_CYAN : DBG_ORANGE);
            for (int k = 0; k < 12; k++) dbg_line(cr[ed[k][0]], cr[ed[k][1]], colr, 0, false);
        }
        if (V->debug->i >= 2) {
            vec3 p = v3(o.pos_size.x, o.pos_size.y, o.pos_size.z), nn = v3(o.normal_depth.x, o.normal_depth.y, o.normal_depth.z);
            dbg_line(p, v3_madd(p, nn, 0.08f), dynamic ? DBG_MAGENTA : DBG_YELLOW, 0, false);
        }
    }
    memcpy(&V->decals[n], tmp_dyn, sizeof(RenderVfxDecal) * (size_t)nd);
    *dyn = nd;
    return n + nd;
}

void vfx_game_render(vec3 cam) {
    if (!V) return;
    bool q_changed = V->quality->modified != V->quality_mod;
    V->quality_mod = V->quality->modified;
    (void)q_changed;
    apply_config(cam);
    if (!V->enable->i) {
        V->sys.spawn_count = 0;
        RenderVfxFrame f;
        memset(&f, 0, sizeof(f));
        f.dt = 0;
        render_vfx_frame(&f);
        return;
    }
    // First-person flash follows the barrel while it lives.
    if (V->local_muzzle && V->muzzle_valid && vfx_sys_alive(&V->sys, V->local_muzzle)) {
        Player *p = game_local_player();
        vfx_sys_set_transform(&V->sys, V->local_muzzle, V->muzzle_pos, rot_from_dir(V->muzzle_dir), p ? p->sim.move.vel : v3(0, 0, 0));
    }
    int inst_count = 0;
    for (int i = 0; i < VFX_MAX_INSTANCES; i++) {
        const VfxInstance *in = &V->sys.inst[i];
        if (!in->active) continue;
        V->instances[i * 2] = v4(in->p.pos.x, in->p.pos.y, in->p.pos.z, 0);
        V->instances[i * 2 + 1] = v4(in->p.rot.x, in->p.rot.y, in->p.rot.z, in->p.rot.w);
        inst_count = i + 1;
    }
    int add = 0;
    int alpha = build_sprites(cam, &add);
    int dyn_decals = 0;
    int decals = build_decals(&dyn_decals);
    RenderVfxFrame f;
    memset(&f, 0, sizeof(f));
    f.spawns = V->sys.spawns;
    f.spawn_count = V->sys.spawn_count;
    f.sprites = V->sprites;
    f.sprite_add = add;
    f.sprite_alpha = alpha;
    f.decals = V->decals;
    f.decal_count = decals;
    f.decal_dynamic = dyn_decals;
    f.instances = V->instances;
    f.instance_count = inst_count;
    f.dt = (float)g_engine.frame_dt * V->timescale->f;
    if (f.dt > 0.1f) f.dt = 0.1f;
    f.timescale = V->timescale->f;
    f.wind = V->wind;
    // Muzzle flashes read far less bright in daylight (the eye is adapted): 1 at night.
    f.exposure_visibility = clampf(1.2f - 0.3f * V->sun_intensity, 0.2f, 1.0f);
    f.lighting = V->lighting->i != 0;
    f.collision = V->quality->i >= 1;
    f.max_sorted = k_quality_sort[DG_CLAMP(V->quality->i, 0, 3)];
    render_vfx_frame(&f);
    V->sys.spawn_count = 0;
    // Effect + particle lights (budgeted, best first).
    VfxLight lights[24];
    int nl = vfx_sys_collect_lights(&V->sys, lights, V->quality->i >= 2 ? 24 : 8);
    for (int i = 0; i < nl; i++) {
        PointLight l = { lights[i].pos, lights[i].radius, lights[i].color, lights[i].intensity };
        render_add_light(&l);
    }
    if (V->debug->i) {
        RenderVfxStats gs;
        render_vfx_stats(&gs);
        const VfxStats *s = &V->sys.stats;
        snprintf(V->stats, sizeof(V->stats),
                 "vfx: inst %d  cpu %d  decals %d  lights %d | gpu alive %u (+%u, drop %u) add %u alpha %u dist %u sort %u | %.2f/%.2f/%.2f/%.2f ms",
                 s->instances, s->cpu_particles, s->decals, nl, gs.alive, gs.emitted, gs.dropped, gs.add, gs.alpha, gs.distort, gs.sort_n,
                 gs.ms_compute, gs.ms_decals, gs.ms_particles, gs.ms_bloom);
        if (V->debug->i >= 2)
            for (int i = 0; i < VFX_MAX_INSTANCES; i++)
                if (V->sys.inst[i].active) dbg_cross(V->sys.inst[i].p.pos, 0.05f, DBG_YELLOW, 0);
    } else {
        V->stats[0] = 0;
    }
}

const char *vfx_game_stats_text(void) { return V ? V->stats : ""; }
