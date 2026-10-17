// Game side of hand poses: discovery, commands, hot reload, interactable targets (docs/HANDPOSE.md).
#include "hp_system.h"
#include "anim/anim_library.h"
#include "core/cvar.h"
#include "core/engine.h"
#include "core/log.h"
#include "core/platform.h"
#include "game/game.h"
#include "gameplay/characters.h"
#include "gameplay/entities.h"
#include "gameplay/items.h"
#include "net/net.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

static struct {
    bool init;
    HandPoseScanReport last;
    char src_dir[600], rt_dir[600];
    bool has_src;
    u64 signature;             // file count + mtimes of the hand pose files (external edit detection)
    double next_poll;
    Cvar *autoreg, *watch;
    u32 preview_case;          // editor: a seat / ledge case previewed at preview_t
    float preview_t;
} HS;

static HandPoseDB *db(void) { return g_engine.anim ? &g_engine.anim->handposes : NULL; }

static void dirs(void) {
    if (HS.rt_dir[0]) return;
    snprintf(HS.rt_dir, sizeof(HS.rt_dir), "%s%s", platform_base_dir(), HANDPOSE_DIR);
#ifdef DG_SOURCE_DIR
    snprintf(HS.src_dir, sizeof(HS.src_dir), "%s/%s", DG_SOURCE_DIR, HANDPOSE_DIR);
    HS.has_src = true;
#endif
}

const char *handpose_system_src_dir(void) { dirs(); return HS.has_src ? HS.src_dir : NULL; }
const char *handpose_system_rt_dir(void) { dirs(); return HS.rt_dir; }
const HandPoseScanReport *handpose_system_report(void) { return &HS.last; }

static u64 dir_signature(const char *dir) {
    u64 h = 1469598103934665603ull;
    const char *subs[4] = { "", "/styles", "/items", "/cases" };
    for (int s = 0; s < 4; s++) {
        char d[700];
        snprintf(d, sizeof(d), "%s%s", dir, subs[s]);
        int count = 0;
        char **files = SDL_GlobDirectory(d, "*.json", 0, &count);
        for (int i = 0; files && i < count; i++) {
            char p[900];
            snprintf(p, sizeof(p), "%s/%s", d, files[i]);
            u64 m = (u64)platform_file_mtime(p);
            h = (h ^ m) * 1099511628211ull;
            h = (h ^ dg_hash_str(files[i])) * 1099511628211ull;
        }
        h = (h ^ (u64)count) * 1099511628211ull;
        SDL_free(files);
    }
    return h;
}
// Reads go through platform_data_path (the runtime copy first): that's the directory to watch.
static u64 signature(void) { dirs(); return dir_signature(HS.rt_dir) ^ (HS.has_src ? dir_signature(HS.src_dir) * 31u : 0); }
void handpose_system_note_own_write(void) { HS.signature = signature(); }

static void collect(HandPoseSourceList *l, World *w) {
    const AnimLibrary *lib = g_engine.anim;
    if (g_engine.items) hp_sources_from_items(l, g_engine.items, &lib->proc);
    hp_sources_from_vehicle_dir(l, "assets/vehicles");
    // Every registered interactable type (placed in a level or not).
    for (int t = 1; t < ET_COUNT; t++) {
        const InteractDef *def = interact_def_for((EntityType)t);
        if (!def || !def->name) continue;
        char src[96];
        snprintf(src, sizeof(src), "interactable:%s", def->name);
        hp_source_from_interactable(l, def->name, (int)def->hands, def->hold_time, def->hand_target != NULL, src);
    }
    hp_sources_builtin(l);
    // Interactables placed in the current level: their source names the level too.
    if (w) {
        ENT_FOREACH(w, i) {
            const InteractDef *def = (w->comps[i] & C_INTERACT) ? w->inter[i].def : NULL;
            if (!def || !def->name) continue;
            char bind[64];
            snprintf(bind, sizeof(bind), "interactable:%s", def->name);
            HandPoseSource *s = NULL;
            for (int k = 0; k < l->count && !s; k++) if (l->v[k].is_case && !strcmp(l->v[k].bind, bind)) s = &l->v[k];
            if (s && !strstr(s->source, "level:")) {
                char add[160];
                snprintf(add, sizeof(add), "%s (level:%s)", s->source, w->level.name[0] ? w->level.name : "?");
                dg_strcpy(s->source, sizeof(s->source), add);
            }
        }
    }
}

void handpose_system_scan(World *w, bool log) {
    HandPoseDB *d = db();
    if (!d) return;
    dirs();
    HandPoseSourceList l = { 0 };
    collect(&l, w);
    HandPoseScanOpts o = { 0 };
    bool write = HS.has_src && (!HS.autoreg || HS.autoreg->i);
    o.write_dir = write ? HS.src_dir : NULL;
    o.copy_dir = write ? HS.rt_dir : NULL;
    o.log = log;
    hp_scan(d, &l, &o, &HS.last);
    hp_sources_free(&l);
    g_engine.anim->manifest.hash[AC_HAND_POSES] = d->hash;
    if (HS.last.written) handpose_system_note_own_write();
    if (log && HS.last.created) {
        char ids[512];
        int n = 0;
        for (int i = 0; i < HS.last.created && i < (int)ARRAY_COUNT(HS.last.created_ids) && n < (int)sizeof(ids) - 50; i++)
            n += snprintf(ids + n, sizeof(ids) - (size_t)n, "%s%s", i ? ", " : "", HS.last.created_ids[i]);
        log_info("handpose: new entries (stubs, review them in 'scene handpose'): %s", ids);
    }
}

bool handpose_system_reload(void) {
    if (!g_engine.anim) return false;
    anim_lib_reload_handposes(g_engine.anim);
    Game *g = game();
    handpose_system_scan(g ? g->world : NULL, true);
    HS.signature = signature();
    if (net_role() != NET_OFF && net_host_client_count() > 0)
        log_warn("handpose_reload: asset hashes changed in a live session - clients desync until they reload the same files");
    return true;
}

bool handpose_system_save(int entry) {
    HandPoseDB *d = db();
    if (!d || entry < 0 || entry >= d->count) return false;
    dirs();
    bool ok = hp_write_entry(&d->e[entry], HS.has_src ? HS.src_dir : HS.rt_dir, HS.rt_dir);
    if (ok) d->e[entry].on_disk = true;
    handpose_system_note_own_write();
    g_engine.anim->manifest.hash[AC_HAND_POSES] = d->hash;
    return ok;
}

bool handpose_system_save_case(int ci) {
    HandPoseDB *d = db();
    if (!d || ci < 0 || ci >= d->case_count) return false;
    dirs();
    bool ok = hp_write_case(&d->c[ci], HS.has_src ? HS.src_dir : HS.rt_dir, HS.rt_dir);
    if (ok) d->c[ci].on_disk = true;
    handpose_system_note_own_write();
    g_engine.anim->manifest.hash[AC_HAND_POSES] = d->hash;
    return ok;
}

// ------------------------------------------------------------------ runtime case lookup

const HandPoseCase *handpose_case_by_hash(u32 hash) {
    const HandPoseDB *d = db();
    int i = d ? handpose_case_find_hash(d, hash) : -1;
    return i >= 0 ? &d->c[i] : NULL;
}

static u32 case_for_bind(const char *bind) {
    const HandPoseDB *d = db();
    int i = d ? handpose_case_find_bind(d, bind) : -1;
    return i >= 0 ? d->c[i].hash : 0;
}

u32 handpose_interact_case_name(const char *name, bool open) {
    if (!name) return 0;
    char bind[80];
    if (open) {
        snprintf(bind, sizeof(bind), "interactable:%s/open", name);
        u32 h = case_for_bind(bind);
        if (h) return h;
    }
    snprintf(bind, sizeof(bind), "interactable:%s", name);
    return case_for_bind(bind);
}

u32 handpose_interact_case(World *w, EntityId e) {
    if (!ent_valid(w, e)) return 0;
    int i = (int)ENT_INDEX(e);
    const InteractDef *def = w->inter[i].def;
    return def ? handpose_interact_case_name(def->name, (w->flags[i] & EF_OPEN) != 0) : 0;
}

u32 handpose_seat_case(World *w, int slot) {
    const char *v = g_vehicle_pose.seat_vehicle ? g_vehicle_pose.seat_vehicle(w, slot) : NULL;
    if (!v) return 0;
    bool driver = !g_vehicle_pose.seat_driver || g_vehicle_pose.seat_driver(w, slot);
    char bind[80];
    if (driver) snprintf(bind, sizeof(bind), "vehicle:%s:driver", v);
    else snprintf(bind, sizeof(bind), "vehicle:passenger");
    return case_for_bind(bind);
}

void handpose_case_preview(u32 case_hash, float t) {
    HS.preview_case = case_hash;
    HS.preview_t = t;
}

float handpose_seat_case_time(u32 h) {
    if (h && h == HS.preview_case) return HS.preview_t;
    const HandPoseCase *c = handpose_case_by_hash(h);
    return c ? c->use : 0.0f;
}

float handpose_seat_case_weight(u32 h) {
    if (!h || h != HS.preview_case) return 1.0f;
    // The editor's timeline: reach in over [0, contact], hold, let go over [release, duration].
    const HandPoseCase *c = handpose_case_by_hash(h);
    int ph;
    float u;
    handpose_case_phase(c, HS.preview_t, &ph, &u);
    return ph == HCP_REACH ? smoothstepf(0, 1, u) : ph == HCP_HOLD ? 1.0f : 1.0f - smoothstepf(0, 1, u);
}

u32 handpose_mantle_case(void) {
    u32 h = case_for_bind("mantle");
    const HandPoseCase *c = handpose_case_by_hash(h);
    return c && (c->key_count || h == HS.preview_case) ? h : 0;
}

float handpose_mantle_case_time(float progress) {
    u32 h = case_for_bind("mantle");
    if (h && h == HS.preview_case) return HS.preview_t;
    const HandPoseCase *c = handpose_case_by_hash(h);
    if (!c) return 0.0f;
    return progress < 0 ? c->use : saturate(progress) * c->duration;
}

float handpose_reload_ik_weight(const char *weapon, float elapsed, float total) {
    if (!weapon) return -1.0f;
    char bind[80];
    snprintf(bind, sizeof(bind), "reload:%s", weapon);
    u32 h = case_for_bind(bind);
    const HandPoseCase *c = handpose_case_by_hash(h);
    if (!c || !(c->reviewed || h == HS.preview_case)) return -1.0f;   // a stub: the controller's transitions as before
    float t = total > 0 ? saturate(elapsed / total) * c->duration : 0.0f;
    // Hand-off: the IK lets go over [0, contact] (the clip takes the hands), takes them back over
    // [release, duration].
    if (t < c->contact) return 1.0f - smoothstepf(0.0f, fmaxf(c->contact, 1e-3f), t);
    if (t < c->release) return 0.0f;
    return smoothstepf(c->release, fmaxf(c->duration, c->release + 1e-3f), t);
}

Xform handpose_seat_anchor(const char *vehicle, bool driver) {
    if (!vehicle) return xf_identity();
    char bind[80];
    if (driver) snprintf(bind, sizeof(bind), "vehicle:%s:driver", vehicle);
    else snprintf(bind, sizeof(bind), "vehicle:passenger");
    const HandPoseCase *c = handpose_case_by_hash(case_for_bind(bind));
    return c ? c->anchor : xf_identity();
}

void handpose_system_poll(void) {
    if (!HS.init || !g_engine.anim || (HS.watch && !HS.watch->i)) return;
    double now = platform_time();
    if (now < HS.next_poll) return;
    HS.next_poll = now + 1.0;
    u64 s = signature();
    if (s == HS.signature) return;
    log_info("handpose: assets/handposes changed on disk, reloading");
    handpose_system_reload();
}

// ------------------------------------------------------------------ interactables

static int auto_entry(const InteractDef *def) {
    const HandPoseDB *d = db();
    if (!d || !def || !def->name) return -1;
    char bind[80];
    snprintf(bind, sizeof(bind), "interactable:%s", def->name);
    int i = handpose_case_find_bind(d, bind);
    // Auto-placed anchors act once a person reviewed them (or while the editor previews the case).
    return i >= 0 && (d->c[i].reviewed || (HS.preview_case && d->c[i].hash == HS.preview_case)) ? i : -1;
}

bool handpose_interact_has_target(const InteractDef *def) { return def && (def->hand_target || auto_entry(def) >= 0); }

bool handpose_interact_target(World *w, EntityId e, int player, int hand, vec3 *pos, quat *rot) {
    int i = (int)ENT_INDEX(e);
    const InteractDef *def = w->inter[i].def;
    if (!def) return false;
    if (def->hand_target) return def->hand_target(w, e, player, hand, pos, rot);
    if (auto_entry(def) < 0) return false;
    // Top of the bounds, palm down, fingers away from the user.
    *pos = v3_add(w->xf[i].pos, quat_rotate(w->xf[i].rot, v3(0, w->vis[i].half.y, 0)));
    *rot = quat_mul(quat_yaw(w->players[player].sim.move.yaw), quat_axis_angle(v3(1, 0, 0), -1.2f));
    return true;
}

// ------------------------------------------------------------------ commands

static void cmd_reload(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    handpose_system_reload();
}

static void cmd_scan(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    Game *g = game();
    handpose_system_scan(g ? g->world : NULL, true);
}

static void cmd_report(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    const HandPoseDB *d = db();
    if (!d) return;
    char *buf = (char *)malloc(65536);
    hp_report_text(d, buf, 65536);
    // One log line per report line (the console splits on nothing).
    for (char *line = buf, *nl; line && *line; line = nl) {
        nl = strchr(line, '\n');
        if (nl) *nl++ = 0;
        log_info("%s", line);
    }
    free(buf);
}


void handpose_system_init(void) {
    dirs();
    HS.autoreg = cvar_reg_bool("handpose_autoreg", true, 0, "dev builds: write a stub assets/handposes/<id>.json for every new holdable / interactable");
    HS.watch = cvar_reg_bool("handpose_watch", true, 0, "reload assets/handposes when its files change on disk");
    cmd_register("handpose_reload", cmd_reload, "re-read assets/handposes and rescan for new entries (docs/HANDPOSE.md)");
    cmd_register("handpose_scan", cmd_scan, "scan items, attachments, vehicles and interactables for new grips / cases");
    cmd_register("handpose_report", cmd_report, "list unreviewed, auto-placed and orphaned grips and cases");

    HS.init = true;
}

void handpose_system_startup(void) {
    if (!HS.init) handpose_system_init();
    handpose_system_scan(NULL, true);
    HS.signature = signature();
}
