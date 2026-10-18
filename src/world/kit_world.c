// Building kit in the world (docs/BUILDING_KIT.md "Engine integration"); see kit_world.h.
#include "world/kit_world.h"
#include "world/kit_layout.h"
#include "assets/kit_piece.h"
#include "gameplay/entities.h"
#include "vfx/vfx_library.h"
#include "core/engine.h"
#include "game/game.h"
#include "core/cvar.h"
#include "core/log.h"
#include "core/platform.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define KIT_DIR "assets/kits/building"
#define KIT_MAX_PIECES 512
#define KIT_MAX_SETS 32
#define KIT_MAX_VARIANTS 16

typedef struct KitMatSet {
    char slot[32];
    int surface;                       // SurfaceMaterial
    bool glass;
    bool windows;                      // MAT_WINDOWS: lit at night (city façades, docs/STORY.md "Vista")
    char names[KIT_MAX_VARIANTS][24];
    float v[KIT_MAX_VARIANTS][6];      // r g b roughness metallic emissive
    int n;
} KitMatSet;

typedef struct KitGpu {
    char id[KIT_ID];
    bool loaded, failed;
    i64 mtime;
    KitPiece *cpu;                     // sockets, hulls (collision rebuilds)
    MeshId mesh[KIT_MAX_SLOTS];
    int set[KIT_MAX_SLOTS];            // material set per slot (-1 = unknown: grey)
    int slot_n;
} KitGpu;

typedef struct KitInst {
    int piece;
    bool batch;                        // merged into the level's batch meshes (no entity)
    bool collide;
    vec3 pos;
    float yaw;                         // radians
    u8 variant[KIT_MAX_SLOTS];
    vec3 tint;
} KitInst;

static struct {
    KitGpu pieces[KIT_MAX_PIECES];
    int piece_n;
    KitMatSet sets[KIT_MAX_SETS];
    int set_n;
    bool sets_loaded;
    cJSON *manifest;
    // the current level's instances (tail of the level's draw / body arrays)
    World *world;
    KitInst *inst;
    int inst_n, inst_cap;
    int draw_first, body_first;
    MeshId batch_mesh[256];            // static batches of the current level (destroyed on rebuild)
    int batch_n;
    double poll_t;
    Cvar *hot_reload, *validate;
} KW;

int kit_world_instance_count(void) { return KW.inst_n; }

// ------------------------------------------------------------------ data

static char *read_text(const char *rel) {
    char buf[512];
    return platform_read_file(platform_data_path(rel, buf, sizeof(buf)), NULL);
}

static void load_sets(void) {
    if (KW.sets_loaded) return;
    KW.sets_loaded = true;
    char *mt = read_text(KIT_DIR "/kit.json");
    KW.manifest = mt ? cJSON_Parse(mt) : NULL;
    free(mt);
    if (!KW.manifest) log_warn("kit: %s/kit.json missing or invalid (no entity hooks, no rules)", KIT_DIR);
    static const char *const names[] = { "paint", "wallpaper", "wood", "tile", "concrete", "brick", "stone", "render", "glass", "metal", "trim", "fabric", "plastic", "carpet", "emissive", "window" };
    for (int i = 0; i < ARRAY_COUNT(names) && KW.set_n < KIT_MAX_SETS; i++) {
        char path[160];
        snprintf(path, sizeof(path), KIT_DIR "/materials/%s.json", names[i]);
        char *t = read_text(path);
        cJSON *j = t ? cJSON_Parse(t) : NULL;
        free(t);
        if (!j) continue;
        KitMatSet *s = &KW.sets[KW.set_n++];
        memset(s, 0, sizeof(*s));
        snprintf(s->slot, sizeof(s->slot), "M_%s", names[i]);
        const char *surf = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "surface"));
        s->surface = surf ? surface_from_name(surf) : -1;
        if (s->surface < 0) s->surface = SURF_DEFAULT;
        s->glass = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "glass"));
        s->windows = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "windows"));
        const cJSON *v;
        cJSON_ArrayForEach(v, cJSON_GetObjectItemCaseSensitive(j, "variants")) {
            if (s->n >= KIT_MAX_VARIANTS) break;
            dg_strcpy(s->names[s->n], sizeof(s->names[0]), v->string);
            for (int k = 0; k < 6; k++) s->v[s->n][k] = k < cJSON_GetArraySize(v) ? (float)cJSON_GetArrayItem(v, k)->valuedouble : 0.0f;
            s->n++;
        }
        cJSON_Delete(j);
    }
}

static int find_set(const char *slot) {
    for (int i = 0; i < KW.set_n; i++)
        if (!strcmp(KW.sets[i].slot, slot)) return i;
    return -1;
}

static const cJSON *manifest_piece(const char *id) {
    return cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(KW.manifest, "pieces"), id);
}

static void free_gpu(KitGpu *g) {
    for (int s = 0; s < g->slot_n; s++)
        if (g->mesh[s]) render_mesh_destroy(g->mesh[s]);
    if (g->cpu) { kit_piece_free(g->cpu); free(g->cpu); }
    g->cpu = NULL;
    g->slot_n = 0;
    g->loaded = false;
}

static bool load_gpu(KitGpu *g) {
    char rel[160], buf[512], err[256];
    snprintf(rel, sizeof(rel), KIT_DIR "/%s.glb", g->id);
    const char *path = platform_data_path(rel, buf, sizeof(buf));
    KitPiece *p = calloc(1, sizeof(KitPiece));
    if (!kit_piece_load(p, path, g->id, err, sizeof(err))) {
        log_warn("kit: %s", err);
        free(p);
        g->failed = true;
        return false;
    }
    g->mtime = platform_file_mtime(path);
    if (KW.validate->i) {
        KitRules r;
        kit_rules_default(&r);
        char out[2048];
        if (kit_piece_validate(p, &r, NULL, out, sizeof(out))) log_warn("kit: %s", out);
    }
    const KitLod *L = &p->lods[0];
    g->slot_n = L->slot_n;
    for (int s = 0; s < L->slot_n; s++) {
        const KitSlotMesh *m = &L->slots[s];
        Vertex *v = malloc(sizeof(Vertex) * (m->vcount ? m->vcount : 1));
        for (u32 k = 0; k < m->vcount; k++) {
            v[k].pos = v3(m->pos[3 * k], m->pos[3 * k + 1], m->pos[3 * k + 2]);
            v[k].normal = v3(m->nrm[3 * k], m->nrm[3 * k + 1], m->nrm[3 * k + 2]);
            v[k].uv = v2(m->uv0[2 * k], m->uv0[2 * k + 1]);
            v[k].color = 0xFFFFFFFFu;
        }
        g->mesh[s] = m->vcount ? render_mesh_create(v, m->vcount, m->idx, m->icount) : 0;
        g->set[s] = find_set(m->slot);
        if (g->set[s] < 0) log_warn("kit: %s: material slot '%s' has no set (grey)", g->id, m->slot);
        free(v);
    }
    g->cpu = p;
    g->loaded = true;
    g->failed = false;
    return true;
}

static int piece_index(const char *id) {
    for (int i = 0; i < KW.piece_n; i++)
        if (!strcmp(KW.pieces[i].id, id)) return KW.pieces[i].failed ? -1 : i;
    if (KW.piece_n >= KIT_MAX_PIECES) { log_warn("kit: more than %d pieces", KIT_MAX_PIECES); return -1; }
    KitGpu *g = &KW.pieces[KW.piece_n];
    memset(g, 0, sizeof(*g));
    dg_strcpy(g->id, sizeof(g->id), id);
    KW.piece_n++;
    return load_gpu(g) ? KW.piece_n - 1 : -1;
}

// ------------------------------------------------------------------ instances -> draws / bodies

static u32 rgba_pack(vec3 c) {
    return (u32)(DG_CLAMP(c.x, 0, 1) * 255.0f + 0.5f) | (u32)(DG_CLAMP(c.y, 0, 1) * 255.0f + 0.5f) << 8 | (u32)(DG_CLAMP(c.z, 0, 1) * 255.0f + 0.5f) << 16;
}

// A slot's material: its set's variant (colour, roughness, metallic, emissive) x the instance tint.
static void slot_material(Material *out, int set_i, int variant, vec3 tint) {
    const KitMatSet *set = set_i >= 0 ? &KW.sets[set_i] : NULL;
    const float *v = set && set->n ? set->v[variant < set->n ? variant : 0] : NULL;
    vec3 c = v3_mul(v ? v3(v[0], v[1], v[2]) : v3(0.7f, 0.7f, 0.7f), tint);
    *out = material_color(v4(c.x, c.y, c.z, 1));
    out->roughness = v ? v[3] : 0.8f;
    out->metallic = v ? v[4] : 0.0f;
    out->emissive = v ? v[5] : 0.0f;
    if (set && set->windows) out->flags |= MAT_WINDOWS;
    if (set && set->glass) {
        out->flags |= MAT_GLASS | MAT_NO_SHADOW;
        out->color.w = 0.12f;
    }
}

static vec3 inst_point(const KitInst *in, const float *p) {
    return v3_add(in->pos, quat_rotate(quat_yaw(in->yaw), v3(p[0], p[1], p[2])));
}

static void build_instance(World *w, const KitInst *in) {
    LevelStatic *ls = &w->level;
    const KitGpu *g = &KW.pieces[in->piece];
    quat rot = quat_yaw(in->yaw);
    mat4 model = mat4_trs(in->pos, rot, v3s(1));
    for (int s = 0; s < g->slot_n && !in->batch; s++) {   // batched instances draw through build_batches
        if (!g->mesh[s] || ls->draw_count >= MAX_STATIC_DRAWS) continue;
        StaticDraw *d = &ls->draws[ls->draw_count++];
        memset(d, 0, sizeof(*d));
        d->mesh = g->mesh[s];
        d->model = model;
        slot_material(&d->mat, g->set[s], in->variant[s], in->tint);
    }
    const KitPiece *p = g->cpu;
    int surface = g->slot_n && g->set[0] >= 0 ? KW.sets[g->set[0]].surface : SURF_DEFAULT;
    for (int h = 0; p && in->collide && h < p->hull_n && ls->body_count < MAX_STATIC_BODIES; h++) {
        const KitHull *hu = &p->hulls[h];
        BodyDesc bd;
        memset(&bd, 0, sizeof(bd));
        bd.motion = PM_STATIC;
        bd.layer = PL_STATIC;
        bd.friction = 0.8f;
        bd.material = (u8)surface;
        bd.rot = rot;
        if (hu->box) {
            float c[3] = { (hu->mn[0] + hu->mx[0]) * 0.5f, (hu->mn[1] + hu->mx[1]) * 0.5f, (hu->mn[2] + hu->mx[2]) * 0.5f };
            bd.shape.kind = SHAPE_BOX;
            bd.shape.half = v3(fmaxf((hu->mx[0] - hu->mn[0]) * 0.5f, 0.005f), fmaxf((hu->mx[1] - hu->mn[1]) * 0.5f, 0.005f), fmaxf((hu->mx[2] - hu->mn[2]) * 0.5f, 0.005f));
            bd.pos = inst_point(in, c);
        } else {
            bd.shape.kind = SHAPE_MESH;
            bd.shape.verts = (const vec3 *)hu->verts;   // 3 floats per vertex, piece space (Jolt copies)
            bd.shape.vert_count = hu->vcount;
            bd.shape.indices = hu->idx;
            bd.shape.index_count = hu->icount;
            bd.pos = in->pos;
        }
        BodyId id = phys_add_body(w->phys, &bd);
        if (id != PHYS_INVALID_BODY) ls->bodies[ls->body_count++] = id;
    }
    // Bounds (camera / culling helpers use them).
    for (int k = 0; k < 8 && p; k++) {
        float c[3] = { (k & 1) ? p->mx[0] : p->mn[0], (k & 2) ? p->mx[1] : p->mn[1], (k & 4) ? p->mx[2] : p->mn[2] };
        vec3 q = inst_point(in, c);
        ls->bounds_min = v3_min(ls->bounds_min, q);
        ls->bounds_max = v3_max(ls->bounds_max, q);
    }
}

// Static batching: the batched instances' slots merged per (material set, variant, tint), world space.
typedef struct KitBatch { int set, variant; u32 tint; MeshBuilder mb; } KitBatch;

static void build_batches(World *w) {
    KitBatch *b = calloc(256, sizeof(KitBatch));
    int bn = 0;
    for (int i = 0; i < KW.inst_n; i++) {
        const KitInst *in = &KW.inst[i];
        if (!in->batch) continue;
        const KitGpu *g = &KW.pieces[in->piece];
        quat rot = quat_yaw(in->yaw);
        u32 tint = rgba_pack(in->tint);
        for (int s = 0; s < g->slot_n; s++) {
            int k = 0;
            for (; k < bn; k++)
                if (b[k].set == g->set[s] && b[k].variant == in->variant[s] && b[k].tint == tint) break;
            if (k == bn) {
                if (bn >= 256) continue;
                b[bn].set = g->set[s];
                b[bn].variant = in->variant[s];
                b[bn].tint = tint;
                mb_init(&b[bn].mb);
                bn++;
            }
            const KitSlotMesh *m = &g->cpu->lods[0].slots[s];
            u32 base = b[k].mb.vcount;
            for (u32 v = 0; v < m->vcount; v++) {
                vec3 p = v3_add(in->pos, quat_rotate(rot, v3(m->pos[3 * v], m->pos[3 * v + 1], m->pos[3 * v + 2])));
                vec3 n = quat_rotate(rot, v3(m->nrm[3 * v], m->nrm[3 * v + 1], m->nrm[3 * v + 2]));
                mb_vertex(&b[k].mb, p, n, v2(m->uv0[2 * v], m->uv0[2 * v + 1]), 0xFFFFFFFFu);
            }
            for (u32 t = 0; t + 2 < m->icount; t += 3) mb_tri(&b[k].mb, base + m->idx[t], base + m->idx[t + 1], base + m->idx[t + 2]);
        }
    }
    LevelStatic *ls = &w->level;
    for (int k = 0; k < bn; k++) {
        if (b[k].mb.vcount && ls->draw_count < MAX_STATIC_DRAWS && KW.batch_n < 256) {
            MeshId id = mb_upload(&b[k].mb);
            KW.batch_mesh[KW.batch_n++] = id;
            StaticDraw *d = &ls->draws[ls->draw_count++];
            memset(d, 0, sizeof(*d));
            d->mesh = id;
            d->model = mat4_identity();
            KitInst fake;
            memset(&fake, 0, sizeof(fake));
            vec3 tint = v3((float)(b[k].tint & 255) / 255.0f, (float)((b[k].tint >> 8) & 255) / 255.0f, (float)((b[k].tint >> 16) & 255) / 255.0f);
            slot_material(&d->mat, b[k].set, b[k].variant, tint);
        }
        mb_free(&b[k].mb);
    }
    free(b);
}

static void free_batches(void) {
    if (KW.batch_n) render_wait_idle();
    for (int k = 0; k < KW.batch_n; k++) render_mesh_destroy(KW.batch_mesh[k]);
    KW.batch_n = 0;
}

static void rebuild_all(World *w) {
    LevelStatic *ls = &w->level;
    for (int b = KW.body_first; b < ls->body_count; b++) phys_remove_body(w->phys, ls->bodies[b]);
    ls->draw_count = KW.draw_first;
    ls->body_count = KW.body_first;
    free_batches();
    for (int i = 0; i < KW.inst_n; i++) build_instance(w, &KW.inst[i]);
    build_batches(w);
    phys_optimize(w->phys);
}

// ------------------------------------------------------------------ entities at sockets

static float jf(const cJSON *o, const char *k, float def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}
static vec3 jv3(const cJSON *o, const char *k, vec3 def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsArray(v) || cJSON_GetArraySize(v) < 3) return def;
    return v3((float)cJSON_GetArrayItem(v, 0)->valuedouble, (float)cJSON_GetArrayItem(v, 1)->valuedouble, (float)cJSON_GetArrayItem(v, 2)->valuedouble);
}
static bool jb(const cJSON *o, const char *k, bool def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : def;
}
static u32 rgba(vec3 c) {
    return (u32)(DG_CLAMP(c.x, 0, 1) * 255.0f) | (u32)(DG_CLAMP(c.y, 0, 1) * 255.0f) << 8 | (u32)(DG_CLAMP(c.z, 0, 1) * 255.0f) << 16 | 0xFF000000u;
}

// The instance's parameters win over the manifest's defaults.
static const cJSON *param(const cJSON *inst, const cJSON *def, const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(inst, k);
    return v ? v : cJSON_GetObjectItemCaseSensitive(def, k);
}

static void spawn_hooked(World *w, const KitInst *in, const cJSON *entry) {
    const cJSON *def = cJSON_GetObjectItemCaseSensitive(manifest_piece(KW.pieces[in->piece].id), "entity");
    if (!def || jb(entry, "no_entity", false)) return;
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(def, "type"));
    const char *sock = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(def, "socket"));
    const KitSocket *s = sock ? kit_piece_socket(KW.pieces[in->piece].cpu, sock, 0) : NULL;
    if (!type || !s) return;
    vec3 off = jv3(def, "offset", v3(0, 0, 0));
    float sp[3] = { s->pos[0] + off.x, s->pos[1] + off.y, s->pos[2] + off.z };
    vec3 pos = inst_point(in, sp);
    float syaw = 2.0f * atan2f(s->rot[1], s->rot[3]);   // sockets turn about Y only
    float yaw = in->yaw + syaw;
    EntityId e = ENT_NONE;
    if (!strcmp(type, "door")) {
        const cJSON *key = param(entry, def, "key");
        u16 key_item = cJSON_IsString(key) ? item_find(w->items, key->valuestring) : 0;
        const cJSON *lk = param(entry, def, "locked");
        e = spawn_door(w, pos, yaw, jf(def, "width", 0.88f), jf(def, "height", 2.08f), cJSON_IsTrue(lk), key_item, rgba(jv3(entry, "color", jv3(def, "color", v3(0.92f, 0.9f, 0.86f)))));
    } else if (!strcmp(type, "light")) {
        const cJSON *on = param(entry, def, "on");
        e = spawn_light(w, pos, jv3(entry, "color", jv3(def, "color", v3(1.0f, 0.92f, 0.8f))), jf(entry, "radius", jf(def, "radius", 6)), jf(entry, "intensity", jf(def, "intensity", 6)),
                        (u32)jf(entry, "group", jf(def, "group", 0)), on ? cJSON_IsTrue(on) : true);
    } else if (!strcmp(type, "switch")) {
        e = spawn_switch(w, pos, yaw, (u32)jf(entry, "group", jf(def, "group", 1)));
    } else if (!strcmp(type, "container")) {
        if (!cJSON_GetObjectItemCaseSensitive(entry, "loot") && !jb(entry, "container", false)) return;   // only stocked furniture is a container
        e = spawn_container_ex(w, v3_sub(pos, v3(0, 0.05f, 0)), yaw, v3(0.05f, 0.05f, 0.02f), false);   // the furniture is the visual
        int i = (int)ENT_INDEX(e);
        const cJSON *it;
        int slot = 0;
        cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(entry, "loot")) {
            if (slot >= CONTAINER_SLOTS || !cJSON_IsArray(it)) break;
            u16 item = item_find(w->items, cJSON_GetStringValue(cJSON_GetArrayItem(it, 0)));
            if (!item) continue;
            InvSlot *sl = &w->cont[i].slots[slot++];
            sl->item = item;
            sl->count = (u16)(cJSON_GetArraySize(it) > 1 ? cJSON_GetArrayItem(it, 1)->valueint : 1);
        }
    }
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "name"));
    if (e && name) dg_strcpy(w->names[ENT_INDEX(e)], sizeof(w->names[0]), name);
}

// ------------------------------------------------------------------ level

static char *prefab_loader(void *user, const char *name) {
    DG_UNUSED(user);
    char path[160];
    snprintf(path, sizeof(path), KIT_DIR "/prefabs/%s.json", name);
    return read_text(path);
}

void kit_world_level(World *w, const cJSON *root) {
    KW.world = w;
    KW.inst_n = 0;
    free_batches();   // the previous level's
    KW.draw_first = w->level.draw_count;
    KW.body_first = w->level.body_count;
    if (!cJSON_GetObjectItemCaseSensitive(root, "kit") && !cJSON_GetObjectItemCaseSensitive(root, "prefabs")) return;
    load_sets();
    KitLayout L;
    kit_layout_build(&L, root, prefab_loader, NULL);
    if (L.errors) log_warn("kit: %d layout error(s), first: %s", L.errors, L.err);
    for (int i = 0; i < L.n; i++) {
        const KitPlaced *pl = &L.items[i];
        int pi = piece_index(pl->piece);
        if (pi < 0) { log_warn("kit: unknown piece '%s'", pl->piece); continue; }
        if (KW.inst_n == KW.inst_cap) {
            KW.inst_cap = KW.inst_cap ? KW.inst_cap * 2 : 512;
            KW.inst = realloc(KW.inst, sizeof(KitInst) * (size_t)KW.inst_cap);
        }
        KitInst *in = &KW.inst[KW.inst_n++];
        memset(in, 0, sizeof(*in));
        in->piece = pi;
        in->pos = v3(pl->pos[0], pl->pos[1], pl->pos[2]);
        in->yaw = pl->yaw * DG_DEG2RAD;
        in->tint = jv3(pl->entry, "tint", v3(1, 1, 1));
        in->batch = pl->batch;
        in->collide = jb(pl->entry, "collide", true);
        const cJSON *mat = cJSON_GetObjectItemCaseSensitive(pl->entry, "mat");
        const KitGpu *g = &KW.pieces[pi];
        for (int s = 0; s < g->slot_n; s++) {
            const char *slot = g->cpu->lods[0].slots[s].slot;
            const char *vn = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(mat, slot));
            if (!vn || g->set[s] < 0) continue;
            const KitMatSet *set = &KW.sets[g->set[s]];
            for (int k = 0; k < set->n; k++)
                if (!strcmp(set->names[k], vn)) in->variant[s] = (u8)k;
        }
        build_instance(w, in);
        if (w->authority && !in->batch) spawn_hooked(w, in, pl->entry);
    }
    kit_layout_free(&L);
    build_batches(w);
    log_info("kit: %d instances of %d pieces: %d draws, %d bodies", KW.inst_n, KW.piece_n, w->level.draw_count - KW.draw_first, w->level.body_count - KW.body_first);
}

bool kit_world_reload(World *w, const char *piece) {
    int n = 0;
    for (int i = 0; i < KW.piece_n; i++) {
        KitGpu *g = &KW.pieces[i];
        if (piece && strcmp(g->id, piece)) continue;
        render_wait_idle();
        free_gpu(g);
        if (load_gpu(g)) n++;
    }
    if (!n) return false;
    if (w && w == KW.world) rebuild_all(w);
    log_info("kit: reloaded %d piece(s)%s%s", n, piece ? ": " : "", piece ? piece : "");
    return true;
}

void kit_world_poll(World *w) {
    if (!KW.hot_reload->i || !KW.piece_n) return;
    double now = (double)g_engine.real_time;
    if (now - KW.poll_t < 1.0) return;
    KW.poll_t = now;
    for (int i = 0; i < KW.piece_n; i++) {
        KitGpu *g = &KW.pieces[i];
        char rel[160], buf[512];
        snprintf(rel, sizeof(rel), KIT_DIR "/%s.glb", g->id);
        i64 m = platform_file_mtime(platform_data_path(rel, buf, sizeof(buf)));
        if (m && m != g->mtime) kit_world_reload(w, g->id);
    }
}

static void cmd_kit_reload(int argc, const char **argv) {
    Game *g = g_engine.game;
    if (!kit_world_reload(g ? g->world : NULL, argc > 1 ? argv[1] : NULL)) log_info("kit_reload: nothing loaded%s%s", argc > 1 ? " named " : "", argc > 1 ? argv[1] : "");
}

static void cmd_kit_info(int argc, const char **argv) {
    DG_UNUSED(argc);
    DG_UNUSED(argv);
    log_info("kit: %d pieces loaded, %d instances in the level, %d material sets", KW.piece_n, KW.inst_n, KW.set_n);
}

void kit_world_register(void) {
    KW.hot_reload = cvar_reg_bool("kit_hot_reload", true, CVAR_LOCKED, "Building kit: reload pieces whose .glb changed on disk (every second; docs/BUILDING_KIT.md)");
    KW.validate = cvar_reg_bool("kit_validate_on_load", false, CVAR_LOCKED, "Building kit: run the kit validator on every piece as it loads (log warnings)");
    cmd_register("kit_reload", cmd_kit_reload, "kit_reload [piece]: reload building kit pieces from disk and rebuild their instances (docs/BUILDING_KIT.md)");
    cmd_register("kit_info", cmd_kit_info, "kit_info: loaded building kit pieces, level instances, material sets");
}
