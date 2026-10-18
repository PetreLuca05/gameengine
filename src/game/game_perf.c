// Performance audit runs (docs/PERF_AUDIT.md): the fixed benchmark scenes of assets/perf/scenes.json,
// captured by the profiler (src/core/profiler.h) as JSON profiles for tools/perf_audit.
//
//   perf_audit [all|scene,scene..] [frames=300] [out=perf_out] [levels=modern,low] [settle=120] [cap=fps] [refs] [quit]
//       every scene x level: load, settle, capture N frames along the scene's camera path; refs also
//       takes reference screenshots (UI hidden) at fixed camera keys after the capture.
//   perf_verify <PF-xxxx> [all|scene,..] [frames=240] [out=perf_out/verify] [reps=2] [settle=120] [cap=fps] [quit]
//       A / B proof of a fidelity-neutral fix: A runs with perf_fix_disable <id>, B with every fix;
//       passes alternate A B A B. Each pass captures a profile (timings), a gameplay state hash per
//       tick (the replicated state of every entity: positions, rotations, hp, flags, animation state
//       ids, items) and the reference screenshots. tools/perf_audit verify compares them.
//
// The runs pause the clock and the weather, fix the frame step (dev_fixed_fps 60: one sim tick per
// frame, so every pass does the same work), drop the frame cap / vsync and restore everything after.
// cap=<fps> keeps a frame cap instead: both sides then submit at the same rate, so a CPU-side speed-up
// cannot move GPU timings through the GPU's duty cycle (queueing behind the previous frame, other
// processes' GPU work landing inside the frame's timestamps; docs/perf/FINDINGS.md PF-0021).
// Nothing else changes: a run measures the engine as it is.
#include "game_internal.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/log.h"
#include "core/platform.h"
#include "core/profiler.h"
#include "net/replication.h"
#include "render/render.h"
#include "cJSON.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PERF_MAX_SCENES 32
#define PERF_MAX_SETUP 16
#define PERF_MAX_KEYS 16
#define PERF_MAX_PLAN 256
#define PERF_MAX_SHOTS 6

typedef struct PerfScene {
    char id[32], desc[128], map[32];
    char setup[PERF_MAX_SETUP][96];
    int setup_count;
    float key[PERF_MAX_KEYS][5];
    int key_count;
    bool orbit;
    float orbit_r, orbit_h, orbit_pitch;
} PerfScene;

typedef struct PerfStep { int scene, level; char pass; int rep; } PerfStep;   // pass: 0 audit, 'A' / 'B' verify

typedef enum { PH_SETUP, PH_SETTLE, PH_CAPTURE, PH_SHOTS, PH_NEXT } PerfPhase;

static struct {
    bool loaded;
    PerfScene scenes[PERF_MAX_SCENES];
    int scene_count, settle, settle_default, shots;
    char reset[PERF_MAX_SETUP][96];   // scenes.json "reset": console lines before every scene's setup
    int reset_count;
    // the run
    bool active, verify, refs, quit;
    PerfStep plan[PERF_MAX_PLAN];
    int plan_count, step;
    PerfPhase phase;
    int frame, frames, shot, shot_wait;
    int cap;                 // cap=<fps>: the frame cap kept on (both sides submit at one rate; 0 = none)
    char out[192], fix[16];
    vec3 center;
    // restored after the run
    char saved_fix[128], saved_fps[32], saved_maxfps[32], saved_vsync[32], saved_level[32];
    // gameplay state hash per captured frame (verify)
    SnapFrame snap;
    u32 *hash;
    int hash_frames, hash_count;
    FILE *manifest;
} PF;

static const char *k_levels[] = { "off", "low", "medium", "high", "modern" };

// ------------------------------------------------------------------ scenes

static bool load_scenes(void) {
    if (PF.loaded) return true;
    char *txt = platform_read_file("assets/perf/scenes.json", NULL);
    cJSON *root = txt ? cJSON_Parse(txt) : NULL;
    free(txt);
    if (!root) { log_error("perf: assets/perf/scenes.json missing or invalid"); return false; }
    PF.settle_default = cJSON_GetObjectItem(root, "settle_frames") ? cJSON_GetObjectItem(root, "settle_frames")->valueint : 120;
    PF.shots = cJSON_GetObjectItem(root, "shots") ? DG_CLAMP(cJSON_GetObjectItem(root, "shots")->valueint, 0, PERF_MAX_SHOTS) : 3;
    cJSON *rl;
    cJSON_ArrayForEach(rl, cJSON_GetObjectItem(root, "reset")) if (cJSON_IsString(rl) && PF.reset_count < PERF_MAX_SETUP) dg_strcpy(PF.reset[PF.reset_count++], 96, rl->valuestring);
    cJSON *sc;
    cJSON_ArrayForEach(sc, cJSON_GetObjectItem(root, "scenes")) {
        if (PF.scene_count >= PERF_MAX_SCENES) break;
        PerfScene *s = &PF.scenes[PF.scene_count];
        memset(s, 0, sizeof(*s));
        cJSON *v;
        if ((v = cJSON_GetObjectItem(sc, "id")) && cJSON_IsString(v)) dg_strcpy(s->id, sizeof(s->id), v->valuestring);
        if ((v = cJSON_GetObjectItem(sc, "desc")) && cJSON_IsString(v)) dg_strcpy(s->desc, sizeof(s->desc), v->valuestring);
        if ((v = cJSON_GetObjectItem(sc, "map")) && cJSON_IsString(v)) dg_strcpy(s->map, sizeof(s->map), v->valuestring);
        cJSON *line;
        cJSON_ArrayForEach(line, cJSON_GetObjectItem(sc, "setup")) if (cJSON_IsString(line) && s->setup_count < PERF_MAX_SETUP) dg_strcpy(s->setup[s->setup_count++], 96, line->valuestring);
        cJSON *key;
        cJSON_ArrayForEach(key, cJSON_GetObjectItem(sc, "camera")) {
            if (s->key_count >= PERF_MAX_KEYS || cJSON_GetArraySize(key) < 5) continue;
            for (int k = 0; k < 5; k++) s->key[s->key_count][k] = (float)cJSON_GetArrayItem(key, k)->valuedouble;
            s->key_count++;
        }
        cJSON *orb = cJSON_GetObjectItem(sc, "orbit");
        if (orb) {
            s->orbit = true;
            s->orbit_r = cJSON_GetObjectItem(orb, "radius") ? (float)cJSON_GetObjectItem(orb, "radius")->valuedouble : 15.0f;
            s->orbit_h = cJSON_GetObjectItem(orb, "height") ? (float)cJSON_GetObjectItem(orb, "height")->valuedouble : 3.0f;
            s->orbit_pitch = cJSON_GetObjectItem(orb, "pitch") ? (float)cJSON_GetObjectItem(orb, "pitch")->valuedouble : -8.0f;
        }
        if (s->id[0] && s->map[0] && (s->key_count >= 2 || s->orbit)) PF.scene_count++;
        else log_warn("perf: scene %d skipped (id, map and a camera path or orbit needed)", PF.scene_count);
    }
    cJSON_Delete(root);
    PF.loaded = true;
    return PF.scene_count > 0;
}

static int scene_find(const char *id) {
    for (int i = 0; i < PF.scene_count; i++) if (!strcmp(PF.scenes[i].id, id)) return i;
    return -1;
}

// "all" or a comma separated list -> scene indices.
static int parse_scenes(const char *arg, int *out) {
    int n = 0;
    if (!arg || !strcmp(arg, "all")) {
        for (int i = 0; i < PF.scene_count; i++) out[n++] = i;
        return n;
    }
    char buf[256];
    dg_strcpy(buf, sizeof(buf), arg);
    for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) {
        int k = scene_find(t);
        if (k >= 0) out[n++] = k;
        else log_warn("perf: unknown scene '%s'", t);
    }
    return n;
}

// ------------------------------------------------------------------ camera

static float catmull(float p0, float p1, float p2, float p3, float t) {
    return 0.5f * ((2 * p1) + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t * t + (-p0 + 3 * p1 - 3 * p2 + p3) * t * t * t);
}

static void scene_camera(const PerfScene *s, float u, RenderCamera *cam) {
    u = fminf(fmaxf(u, 0.0f), 1.0f);
    float v[5];
    if (s->orbit) {
        float a = u * 2.0f * 3.14159265f;
        v[0] = PF.center.x + sinf(a) * s->orbit_r;
        v[1] = PF.center.y + s->orbit_h;
        v[2] = PF.center.z + cosf(a) * s->orbit_r;
        v[3] = a * (180.0f / 3.14159265f);   // yaw 0 = -z: looking back at the centre
        v[4] = s->orbit_pitch;
    } else {
        int n = s->key_count;
        float x = u * (float)(n - 1);
        int i = DG_CLAMP((int)x, 0, n - 2);
        float t = fminf(x - (float)i, 1.0f);
        for (int k = 0; k < 5; k++)
            v[k] = catmull(s->key[i > 0 ? i - 1 : 0][k], s->key[i][k], s->key[i + 1][k], s->key[i + 2 < n ? i + 2 : n - 1][k], t);
    }
    cam->pos = v3(v[0], v[1], v[2]);
    cam->yaw = -v[3] * DG_DEG2RAD;
    cam->pitch = v[4] * DG_DEG2RAD;
    cam->roll = 0;
}

static bool perf_camera(RenderCamera *cam) {
    if (!PF.active || PF.step >= PF.plan_count) return false;
    const PerfScene *s = &PF.scenes[PF.plan[PF.step].scene];
    float u = 0.0f;
    if (PF.phase == PH_CAPTURE) {
        int f = prof_capture_frame();
        u = f < 0 ? 0.0f : (float)f / (float)DG_MAX(PF.frames - 1, 1);
    } else if (PF.phase == PH_SHOTS) {
        u = PF.shots > 1 ? (float)PF.shot / (float)(PF.shots - 1) : 0.0f;
    }
    scene_camera(s, u, cam);
    return true;
}

// ------------------------------------------------------------------ gameplay state hash

static u32 fnv(u32 h, const void *p, size_t n) {
    const u8 *b = (const u8 *)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

// Every captured frame (one sim tick): the replicated state of the whole world, entity by entity
// (capture_one zeroes each record first: no padding garbage), plus the tick.
static void hash_frame(int frame) {
    if (!PF.active || !PF.verify || !PF.hash || frame < 0 || frame >= PF.hash_frames) return;
    World *w = game_world();
    if (!w || !PF.snap.ents) return;
    repl_capture(w, v3(0, 0, 0), 1.0e9f, &PF.snap);
    u32 h = fnv(2166136261u, &w->tick, sizeof(w->tick));
    h = fnv(h, PF.snap.ents, sizeof(NetEnt) * (size_t)PF.snap.count);
    PF.hash[frame] = h;
    PF.hash_count = frame + 1;
}

static void write_state(FILE *f) {
    if (!PF.active || !PF.verify || !PF.hash) { fprintf(f, "null"); return; }
    fprintf(f, "{\"kind\":\"replicated-entities-fnv32\",\"hashes\":[");
    for (int i = 0; i < PF.hash_count; i++) fprintf(f, i ? ",%u" : "%u", PF.hash[i]);
    fprintf(f, "]}");
}

// ------------------------------------------------------------------ run

static void save_cvar(const char *name, char *dst, size_t size) {
    Cvar *cv = cvar_find(name);
    dg_strcpy(dst, size, cv ? cv->str : "");
}

// The run is a dev tool: dev_fixed_fps / perf_fix_disable are LOCKED (docs/SETTINGS_AUDIT.md), so its
// writes come as CVAR_SRC_HOST (also under --release-locks).
static void dev_set(const char *name, const char *v) {
    CvarSource src = cvar_source_push(CVAR_SRC_HOST);
    cvar_set(name, v);
    cvar_source_pop(src);
}

static void restore_cvar(const char *name, const char *v) { if (cvar_find(name)) dev_set(name, v); }

static void step_path(char *dst, size_t size, const PerfStep *st, const char *ext, int shot) {
    const PerfScene *s = &PF.scenes[st->scene];
    char pass[16] = "";
    if (st->pass) snprintf(pass, sizeof(pass), "_%c%d", st->pass, st->rep);
    if (shot >= 0) snprintf(dst, size, "%s/%s_%s%s_shot%d.%s", PF.out, s->id, k_levels[st->level], pass, shot, ext);
    else snprintf(dst, size, "%s/%s_%s%s.%s", PF.out, s->id, k_levels[st->level], pass, ext);
}

static void run_finish(void) {
    restore_cvar("perf_fix_disable", PF.saved_fix);
    restore_cvar("dev_fixed_fps", PF.saved_fps);
    restore_cvar("r_maxfps", PF.saved_maxfps);
    restore_cvar("r_vsync", PF.saved_vsync);
    restore_cvar("r_fidelity", PF.saved_level);
    render_set_ui_hidden(false);
    if (PF.manifest) {
        fprintf(PF.manifest, "\n]}\n");
        fclose(PF.manifest);
        PF.manifest = NULL;
    }
    free(PF.hash);
    PF.hash = NULL;
    PF.active = false;
    log_info("perf_%s: done, %d pass(es) -> %s (next: python tools/perf_audit/perf_audit.py %s %s)", PF.verify ? "verify" : "audit", PF.plan_count, PF.out,
             PF.verify ? "verify" : "analyze", PF.out);
    if (PF.quit) g_engine.quit = true;
}

static bool run_begin(bool verify, const char *out) {
    if (PF.active || prof_capture_active()) { log_warn("perf: a run or capture is already active"); return false; }
    dg_strcpy(PF.out, sizeof(PF.out), out);
    SDL_CreateDirectory(PF.out);
    char path[256];
    snprintf(path, sizeof(path), "%s/manifest.json", PF.out);
    PF.manifest = fopen(path, "wb");
    if (!PF.manifest) { log_error("perf: cannot write %s", path); return false; }
    fprintf(PF.manifest, "{\"kind\":\"%s\",\"fix\":", verify ? "verify" : "audit");
    prof_json_str(PF.manifest, PF.fix);
    fprintf(PF.manifest, ",\"frames\":%d,\"settle\":%d,\"shots\":%d,\"passes\":[", PF.frames, PF.settle, PF.refs ? PF.shots : 0);
    save_cvar("perf_fix_disable", PF.saved_fix, sizeof(PF.saved_fix));
    save_cvar("dev_fixed_fps", PF.saved_fps, sizeof(PF.saved_fps));
    save_cvar("r_maxfps", PF.saved_maxfps, sizeof(PF.saved_maxfps));
    save_cvar("r_vsync", PF.saved_vsync, sizeof(PF.saved_vsync));
    save_cvar("r_fidelity", PF.saved_level, sizeof(PF.saved_level));
    dev_set("dev_fixed_fps", "60");
    char cap[16];
    snprintf(cap, sizeof(cap), "%d", PF.cap);
    cvar_set("r_maxfps", cap);
    cvar_set("r_vsync", "0");
    PF.verify = verify;
    PF.step = 0;
    PF.phase = PH_SETUP;
    PF.active = true;
    if (verify) {
        PF.hash_frames = PF.frames;
        PF.hash = (u32 *)calloc((size_t)PF.frames, sizeof(u32));
        if (!PF.snap.ents) PF.snap.ents = (NetEnt *)calloc(MAX_SNAP_ENTS, sizeof(NetEnt));
    }
    return true;
}

static void manifest_pass(const PerfStep *st) {
    const PerfScene *s = &PF.scenes[st->scene];
    char prof[256];
    step_path(prof, sizeof(prof), st, "json", -1);
    fprintf(PF.manifest, "%s\n {\"scene\":\"%s\",\"level\":\"%s\",\"pass\":\"%c\",\"rep\":%d,\"profile\":", PF.step ? "," : "", s->id, k_levels[st->level],
            st->pass ? st->pass : '-', st->rep);
    prof_json_str(PF.manifest, prof);
    fprintf(PF.manifest, ",\"shots\":[");
    for (int k = 0; PF.refs && k < PF.shots; k++) {
        char shot[256];
        step_path(shot, sizeof(shot), st, "png", k);
        if (k) fputc(',', PF.manifest);
        prof_json_str(PF.manifest, shot);
    }
    fprintf(PF.manifest, "]}");
}

// Main loop, after the command buffer (main.c): advances the run by one frame.
void game_perf_frame(void) {
    if (!PF.active) return;
    if (PF.step >= PF.plan_count) { run_finish(); return; }
    PerfStep *st = &PF.plan[PF.step];
    const PerfScene *s = &PF.scenes[st->scene];
    switch (PF.phase) {
    case PH_SETUP: {
        // Start every pass on the same TAA jitter phase: from here every frame count of the pass is
        // fixed, so the reference shots land on the same sim tick and jitter in A and B.
        if (render_frame_index() % 8) break;
        dev_set("perf_fix_disable", st->pass == 'A' ? PF.fix : "");
        char lv[8];
        snprintf(lv, sizeof(lv), "%d", st->level);
        cvar_set("r_fidelity", lv);
        char line[128];
        snprintf(line, sizeof(line), "map %s", s->map);
        cmd_execute_line(line);   // loads now (game_host)
        for (int i = 0; i < PF.reset_count; i++) cmd_execute_line(PF.reset[i]);   // what an earlier scene's setup may have changed
        for (int i = 0; i < s->setup_count; i++) cmd_execute_line(s->setup[i]);
        Player *p = game_local_player();
        PF.center = p ? p->sim.move.pos : v3(0, 0, 0);
        g_engine.render_time = 0.0;   // dev_fixed_fps: every pass renders the same shader times
        log_info("perf: pass %d / %d: %s (%s) at %s%s%s", PF.step + 1, PF.plan_count, s->id, s->desc, k_levels[st->level], st->pass == 'A' ? ", fix off: " : "",
                 st->pass == 'A' ? PF.fix : "");
        PF.phase = PH_SETTLE;
        PF.frame = 0;
        render_set_ui_hidden(false);
        break;
    }
    case PH_SETTLE:
        // Streaming, LOD fades, exposure and the temporal histories settle at the path's start.
        if (++PF.frame < PF.settle) break;
        {
            ProfCaptureDesc d;
            memset(&d, 0, sizeof(d));
            step_path(d.path, sizeof(d.path), st, "json", -1);
            dg_strcpy(d.scene, sizeof(d.scene), s->id);
            snprintf(d.label, sizeof(d.label), "%s%c%d", PF.verify ? "verify " : "audit", st->pass ? st->pass : ' ', st->rep);
            dg_strcpy(d.fidelity, sizeof(d.fidelity), k_levels[st->level]);
            d.frames = PF.frames;
            d.warmup = 1;
            if (PF.hash) { memset(PF.hash, 0, sizeof(u32) * (size_t)PF.hash_frames); PF.hash_count = 0; }
            if (!prof_capture_start(&d)) { PF.phase = PH_NEXT; break; }
            PF.phase = PH_CAPTURE;
        }
        break;
    case PH_CAPTURE:
        if (prof_capture_active()) break;
        PF.phase = PF.refs ? PH_SHOTS : PH_NEXT;
        PF.shot = 0;
        PF.shot_wait = 0;
        render_set_ui_hidden(PF.refs);
        break;
    case PH_SHOTS:
        // Each key: hold 24 frames (temporal AA, exposure and LOD fades converge), shoot, wait for the
        // file. The pass started on jitter phase 0 and every count since is fixed: the same tick and
        // jitter in every pass (A and B compare like for like).
        if (PF.shot >= PF.shots) { PF.phase = PH_NEXT; render_set_ui_hidden(false); break; }
        PF.shot_wait++;
        if (PF.shot_wait == 24) {
            char path[256];
            step_path(path, sizeof(path), st, "png", PF.shot);
            render_screenshot(path);
            PF.shot_wait = 1000;
        } else if (PF.shot_wait >= 1004) {
            PF.shot++;
            PF.shot_wait = 0;
        }
        break;
    case PH_NEXT:
        manifest_pass(st);
        PF.step++;
        PF.phase = PH_SETUP;
        break;
    }
}

// ------------------------------------------------------------------ commands

static bool has_flag(int argc, const char **argv, const char *flag) {
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], flag)) return true;
    return false;
}

// Positional arguments that are not flags.
static const char *positional(int argc, const char **argv, int want) {
    int n = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "quit") || !strcmp(argv[i], "refs") || strchr(argv[i], '=')) continue;
        if (n++ == want) return argv[i];
    }
    return NULL;
}

static const char *kv(int argc, const char **argv, const char *key) {
    size_t n = strlen(key);
    for (int i = 1; i < argc; i++) if (!strncmp(argv[i], key, n) && argv[i][n] == '=') return argv[i] + n + 1;
    return NULL;
}

static void cmd_perf_audit(int argc, const char **argv) {
    if (!load_scenes()) return;
    int sc[PERF_MAX_SCENES];
    int ns = parse_scenes(positional(argc, argv, 0), sc);
    const char *fr = positional(argc, argv, 1), *out = positional(argc, argv, 2), *lv = kv(argc, argv, "levels");
    PF.frames = fr ? DG_CLAMP(atoi(fr), 10, 20000) : 300;
    PF.settle = kv(argc, argv, "settle") ? DG_MAX(2, atoi(kv(argc, argv, "settle"))) : PF.settle_default;
    PF.refs = has_flag(argc, argv, "refs");
    PF.quit = has_flag(argc, argv, "quit");
    PF.cap = kv(argc, argv, "cap") ? DG_MAX(0, atoi(kv(argc, argv, "cap"))) : 0;
    PF.fix[0] = 0;
    int levels[5], nl = 0;
    char lb[64];
    dg_strcpy(lb, sizeof(lb), lv ? lv : "modern,low");
    for (char *t = strtok(lb, ","); t && nl < 5; t = strtok(NULL, ","))
        for (int l = 0; l < 5; l++) if (!strcmp(t, k_levels[l])) levels[nl++] = l;
    PF.plan_count = 0;
    for (int i = 0; i < ns; i++)
        for (int l = 0; l < nl && PF.plan_count < PERF_MAX_PLAN; l++) PF.plan[PF.plan_count++] = (PerfStep){ sc[i], levels[l], 0, 0 };
    if (!PF.plan_count) { log_warn("perf_audit: nothing to run (scenes: %d, levels: %d)", ns, nl); return; }
    if (run_begin(false, out ? out : "perf_out")) log_info("perf_audit: %d pass(es), %d frames each -> %s", PF.plan_count, PF.frames, PF.out);
}

static void cmd_perf_verify(int argc, const char **argv) {
    if (!load_scenes()) return;
    const char *id = positional(argc, argv, 0);
    // "all": every fix off on the A side (the nightly CI checks images / state of all fixes together).
    if (!id || (strncmp(id, "PF-", 3) && strcmp(id, "all"))) {
        log_info("usage: perf_verify <PF-xxxx|all> [all|scene,..] [frames=240] [out] [reps=2] [levels=modern] [settle=120] [cap=fps] [quit]");
        return;
    }
    int sc[PERF_MAX_SCENES];
    int ns = parse_scenes(positional(argc, argv, 1), sc);
    const char *fr = positional(argc, argv, 2), *out = positional(argc, argv, 3), *reps_s = kv(argc, argv, "reps"), *lv = kv(argc, argv, "levels");
    PF.frames = fr ? DG_CLAMP(atoi(fr), 10, 20000) : 240;
    PF.settle = kv(argc, argv, "settle") ? DG_MAX(2, atoi(kv(argc, argv, "settle"))) : PF.settle_default;
    PF.refs = true;
    PF.quit = has_flag(argc, argv, "quit");
    PF.cap = kv(argc, argv, "cap") ? DG_MAX(0, atoi(kv(argc, argv, "cap"))) : 0;
    dg_strcpy(PF.fix, sizeof(PF.fix), id);
    int reps = reps_s ? DG_CLAMP(atoi(reps_s), 1, 8) : 2;
    int levels[5], nl = 0;
    char lb[64];
    dg_strcpy(lb, sizeof(lb), lv ? lv : "modern");
    for (char *t = strtok(lb, ","); t && nl < 5; t = strtok(NULL, ","))
        for (int l = 0; l < 5; l++) if (!strcmp(t, k_levels[l])) levels[nl++] = l;
    PF.plan_count = 0;
    // A B A B ..: drift (thermals, background load) hits both sides alike.
    for (int i = 0; i < ns; i++)
        for (int l = 0; l < nl; l++)
            for (int r = 0; r < reps && PF.plan_count + 2 <= PERF_MAX_PLAN; r++) {
                PF.plan[PF.plan_count++] = (PerfStep){ sc[i], levels[l], 'A', r };
                PF.plan[PF.plan_count++] = (PerfStep){ sc[i], levels[l], 'B', r };
            }
    char dir[192];
    snprintf(dir, sizeof(dir), "%s/%s", out ? out : "perf_out/verify", id);
    SDL_CreateDirectory(out ? out : "perf_out/verify");
    if (run_begin(true, dir)) log_info("perf_verify %s: %d pass(es), %d frames each -> %s", id, PF.plan_count, PF.frames, PF.out);
}

static void cmd_perf_scenes(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    if (!load_scenes()) return;
    for (int i = 0; i < PF.scene_count; i++) log_info("  %-14s %-10s %s", PF.scenes[i].id, PF.scenes[i].map, PF.scenes[i].desc);
}

void game_perf_init(void) {
    cmd_register("perf_audit", cmd_perf_audit,
                 "perf_audit [all|scene,..] [frames=300] [out=perf_out] [levels=modern,low] [settle=120] [cap=fps] [refs] [quit]: profile the benchmark scenes (docs/PERF_AUDIT.md)");
    cmd_register("perf_verify", cmd_perf_verify,
                 "perf_verify <PF-xxxx|all> [all|scene,..] [frames=240] [out=perf_out/verify] [reps=2] [levels=modern] [settle=120] [cap=fps] [quit]: A/B timings, image and state diffs of a fix");
    cmd_register("perf_scenes", cmd_perf_scenes, "list the perf_audit benchmark scenes (assets/perf/scenes.json)");
    render_set_camera_override(perf_camera);
    prof_add_frame_hook(hash_frame);
    prof_add_section("state", write_state);
}
