#include "gameplay/attachments.h"
#include "characters.h"
#include "net/net.h"
#include "health.h"
#include "weapons.h"
#include "effects.h"
#include "entities.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/log.h"
#include "render/render.h"
#include "render/debug_draw.h"
#include "player/hands.h"
#include "anim/procedural_internal.h"
#include "cJSON.h"
#include "core/platform.h"
#include "assets/assets.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static void cmd_fp_eye_check(int argc, const char **argv);

#define CG (g_engine.chars)
#define LIB (g_engine.anim)

#define MAX_SKINS 16
typedef struct CharSkin {
    char id[32], name[48];
    MeshId lod[CHAR_LOD_COUNT], arm, noarm;
    TextureId texture;              // atlas (0 = vertex colours only)
} CharSkin;

typedef struct CharGlobals {
    MeshId mesh[LIB_MAX_CHARS][CHAR_LOD_COUNT];
    i16 keep_map[ANIM_MAX_BONES];
    CharSkin skins[MAX_SKINS];
    int skin_count, player_skin;
    char npc_def[LIB_MAX_CHARS][32];
    int npc_skin[LIB_MAX_CHARS], npc_count;
    bool skins_loaded;
    Cvar *skin_cvar;
    // First-person body: arm-only meshes (viewmodel pass) and bone sets.
    MeshId arm_mesh[LIB_MAX_CHARS];
    MeshId noarm_mesh[LIB_MAX_CHARS];   // the body without those triangles (world pass in first person)
    bool arm_bone[ANIM_MAX_BONES], head_bone[ANIM_MAX_BONES];
    Cvar *fp_body, *fp_debug;
    u32 lib_revision;
    mat4 inv_bind[ANIM_MAX_BONES];
    LodConfig lod;
    int force_lod;
    // baked per-style arm pose for LOD2+ (captured from LOD0/1 hold solves)
    Xform style_arms[HOLD_STYLE_MAX][ANIM_MAX_BONES];
    bool style_arms_valid[HOLD_STYLE_MAX];
    Cvar *foot_ik, *spine, *turn, *hold, *lean, *damage, *ragdoll, *ragdoll_debug, *corpse_collision, *ragdoll_time;
    Cvar *max_ragdolls, *debug, *hold_debug, *hold_assert, *hitbox_debug, *lod_debug, *lod_force, *lod_bias, *lod_quality;
    Cvar *eval_budget;
    int last_evals, last_deferred;      // budget stats (previous present tick)
    Cvar *predict_hits, *resync_ticks, *local_resim, *net_debug, *zombie_speed;
    u32 hold_failures;
    Cvar *hp_force_state, *hp_static, *cam_debug;
    // hold point / first person diagnostics (last frame)
    float fp_glue_pos[2], fp_glue_ang[2];
    Xform fp_item_cam;               // this frame's rendered item in camera space
    Xform fp_item_target;            // the hold point's item in camera space (no reach pull)
    int fp_hp_state;                 // hold point state / blend progress (tests)
    float fp_hp_blend;
    float fp_hold_w;                 // hand IK weight of this frame's first-person hold (1 = the hold owns the hands)
    float fp_ads_pos, fp_ads_ang;
    vec3 fp_sight_cam;               // the rendered item's sight socket in camera space (ADS checks)
    float fp_reach_pull;             // this frame's first-person reach pull (m)
    Xform fp_cam;
    bool fp_cam_valid;
} CharGlobals;

// ------------------------------------------------------------------ globals

static void ensure_globals(void) {
    if (CG) return;
    CG = (CharGlobals *)calloc(1, sizeof(CharGlobals));
    lod_config_default(&CG->lod);
    CG->force_lod = -1;
}

void characters_register_cvars(void) {
    ensure_globals();
    CharGlobals *g = CG;
    g->foot_ik = cvar_reg_bool("anim_foot_ik", true, CVAR_ARCHIVE, "foot IK on characters (cosmetic)");
    g->spine = cvar_reg_bool("anim_spine", true, CVAR_ARCHIVE, "spine / head look at the aim");
    g->turn = cvar_reg_bool("anim_turn", true, CVAR_ARCHIVE, "turn in place smoothing + foot locking");
    g->hold = cvar_reg_bool("anim_hold", true, CVAR_ARCHIVE, "procedural weapon holding (two-hand IK)");
    g->lean = cvar_reg_bool("anim_lean", true, CVAR_ARCHIVE, "slope / acceleration lean");
    g->damage = cvar_reg_bool("anim_damage", true, CVAR_ARCHIVE, "additive damage reactions");
    g->ragdoll = cvar_reg_bool("anim_ragdoll", true, CVAR_ARCHIVE, "ragdoll on death");
    g->ragdoll_debug = cvar_reg_bool("anim_ragdoll_debug", false, 0, "draw ragdoll shapes, joint cones and masses");
    g->corpse_collision = cvar_reg_bool("anim_corpse_collision", false, 0, "ragdolls collide with players / zombies");
    g->ragdoll_time = cvar_reg_float("anim_ragdoll_time", 45.0f, 2.0f, 600.0f, CVAR_ARCHIVE, "seconds before a ragdoll is removed");
    g->max_ragdolls = cvar_reg_int("anim_max_ragdolls", 24, 1, 256, CVAR_ARCHIVE, "maximum simultaneous simulated ragdolls");
    g->debug = cvar_reg_int("anim_debug", 0, 0, 2, 0, "animation debug: 1 = skeleton + window, 2 = all characters");
    g->hold_debug = cvar_reg_bool("anim_hold_debug", false, 0, "draw hold sockets / wrists and print the left-hand glue error");
    cmd_register("fp_eye_check", cmd_fp_eye_check, "print where the first-person camera socket sits on each character's head (bind pose)");
    g->hold_assert = cvar_reg_bool("anim_hold_assert", false, 0, "log any left hand <-> grip_l error above 2 mm / 1 deg");
    g->hitbox_debug = cvar_reg_bool("hitbox_debug", false, 0, "draw per-part hitboxes with their multipliers");
    g->lod_debug = cvar_reg_bool("r_lod_debug", false, 0, "per-character LOD / pixel size / update rate / systems");
    g->lod_force = cvar_reg_int("r_lod_force", -1, -1, 3, 0, "force a character LOD (-1 = automatic)");
    g->lod_bias = cvar_reg_float("r_lod_bias", 1.0f, 0.25f, 4.0f, CVAR_ARCHIVE, "character LOD bias (>1 = more detail)");
    g->skin_cvar = cvar_reg_string("cl_skin", "", CVAR_ARCHIVE, "your character skin (assets/human/skins.json id; empty = the default)");
    g->fp_body = cvar_reg_bool("cl_fp_body", true, CVAR_ARCHIVE, "first person: draw your own body; the player model's arms replace the placeholder hands");
    g->fp_debug = cvar_reg_int("cl_fp_debug", 0, 0, 2, 0, "first person: 1 = log / draw hand targets and weapon pull-back, 2 = raw body pose (no arm IK, head visible)");
    g->eval_budget = cvar_reg_int("anim_eval_budget", 64, 0, 4096, CVAR_ARCHIVE, "max full character post-passes per tick (0 = unlimited); the rest wait a tick, biggest on screen first");
    g->lod_quality = cvar_reg_int("r_char_quality", 2, 0, 3, CVAR_ARCHIVE, "character detail (0 low .. 3 ultra), scales LOD thresholds");
    g->predict_hits = cvar_reg_bool("cl_predict_hit_reactions", true, CVAR_ARCHIVE, "show an unconfirmed reaction on targets you hit");
    g->resync_ticks = cvar_reg_int("anim_resync_ticks", 8, 1, 120, 0, "ticks of state mismatch before snapping to the host's state");
    g->local_resim = cvar_reg_bool("anim_local_resim", false, 0, "local player: re-simulate the controller on reconcile (else snap state ids)");
    g->net_debug = cvar_reg_bool("anim_net_debug", false, 0, "log resyncs and replicated vs local state");
    g->zombie_speed = cvar_reg_float("zombie_speed", 1.0f, 0.0f, 4.0f, CVAR_SERVER, "zombie wander speed multiplier");
    g->hp_force_state = cvar_reg_int("holdpoint_force_state", -1, -1, HP_STATE_COUNT - 1, 0,
                                     "force a hold point state on every character: -1 auto, 0 hip 1 ads 2 sprint 3 crouch 4 prone 5 empty 6 inspect");
    g->hp_static = cvar_reg_bool("holdpoint_static", false, 0, "hold point without sway / bob / look lag / kick (alignment checks, editing)");
}

void characters_shutdown_globals(void) {
    if (!CG) return;
    for (int c = 0; c < LIB_MAX_CHARS; c++) {
        for (int l = 0; l < CHAR_LOD_COUNT; l++) render_mesh_destroy(CG->mesh[c][l]);
        render_mesh_destroy(CG->arm_mesh[c]);
        render_mesh_destroy(CG->noarm_mesh[c]);
    }
    for (int k = 0; k < CG->skin_count; k++) {
        for (int l = 0; l < CHAR_LOD_COUNT; l++) render_mesh_destroy(CG->skins[k].lod[l]);
        render_mesh_destroy(CG->skins[k].arm);
        render_mesh_destroy(CG->skins[k].noarm);
    }
    free(CG);
    CG = NULL;
}

const LodConfig *chars_lod_config(void) { ensure_globals(); return &CG->lod; }
void chars_force_lod(int lod) { ensure_globals(); CG->force_lod = lod; }

static bool cv(Cvar *c) { return c && c->i != 0; }

// GPU meshes per (character, LOD) with the character's material colors baked into the vertex
// colors; LOD2+ fold finger / end bones onto their kept ancestors (reduced bone set).
// GPU meshes of one skinned mesh: every LOD (LOD2+ fold end bones onto kept ancestors) with the
// material colours baked into the vertex colours, plus the first-person arm / no-arm split of LOD0.
// tiles > 0: texture atlas skin, vertex UVs remapped into the material's tile (mat_tile).
static void build_char_meshes(const SkinMeshData *m, const u32 *colors, const int *mat_tile, int tiles, const i16 *keep_map,
                              MeshId *out_lod, MeshId *out_arm, MeshId *out_noarm) {
    for (int l = 0; l < m->lod_count && l < CHAR_LOD_COUNT; l++) {
        const SkinLod *L = &m->lods[l];
        SkinVertex *v = (SkinVertex *)malloc(sizeof(SkinVertex) * L->vcount);
        for (u32 i = 0; i < L->vcount; i++) {
            v[i] = L->verts[i];
            int mat = DG_MIN((int)L->vmat[i], m->material_count - 1);
            v[i].color = colors[mat];
            // Texture atlas skins: the material's tile (side by side), repeat folded in.
            if (tiles > 0) {
                int tl = DG_CLAMP(mat_tile[mat], 0, tiles - 1);
                float u = v[i].uv.x - floorf(v[i].uv.x), vv = v[i].uv.y - floorf(v[i].uv.y);
                v[i].uv = v2(((float)tl + u) / (float)tiles, vv);
            }
            if (l >= 2)
                for (int k = 0; k < 4; k++) v[i].joints[k] = (u8)keep_map[v[i].joints[k]];
        }
        out_lod[l] = render_skinned_mesh_create(v, L->vcount, L->indices, L->icount);
        if (l == 0) {
            // Arm-only copy for the first-person viewmodel pass: triangles whose vertices are
            // mostly skinned to the arm chains, re-weighted onto arm bones only.
            u32 *remap = (u32 *)malloc(sizeof(u32) * L->vcount);
            SkinVertex *av = (SkinVertex *)malloc(sizeof(SkinVertex) * L->vcount);
            u32 *ai = (u32 *)malloc(sizeof(u32) * L->icount);
            u32 *bi = (u32 *)malloc(sizeof(u32) * L->icount);
            u32 avc = 0, aic = 0, bic = 0;
            for (u32 i = 0; i < L->vcount; i++) remap[i] = 0xFFFFFFFFu;
            for (u32 t = 0; t + 2 < L->icount; t += 3) {
                bool keep = true;
                for (int k = 0; k < 3 && keep; k++) {
                    const SkinVertex *sv = &v[L->indices[t + k]];
                    int aw = 0;
                    for (int j = 0; j < 4; j++) if (CG->arm_bone[sv->joints[j]]) aw += sv->weights[j];
                    keep = aw >= 128;
                }
                if (!keep) {
                    for (int k = 0; k < 3; k++) bi[bic++] = L->indices[t + k];
                    continue;
                }
                for (int k = 0; k < 3; k++) {
                    u32 src = L->indices[t + k];
                    if (remap[src] == 0xFFFFFFFFu) {
                        SkinVertex sv = v[src];
                        int sum = 0;
                        for (int j = 0; j < 4; j++) { if (!CG->arm_bone[sv.joints[j]]) sv.weights[j] = 0; sum += sv.weights[j]; }
                        for (int j = 0; j < 4; j++) sv.weights[j] = (u8)(sum ? (sv.weights[j] * 255 + sum / 2) / sum : 0);
                        remap[src] = avc;
                        av[avc++] = sv;
                    }
                    ai[aic++] = remap[src];
                }
            }
            if (aic) *out_arm = render_skinned_mesh_create(av, avc, ai, aic);
            if (aic && bic) *out_noarm = render_skinned_mesh_create(v, L->vcount, bi, bic);
            free(bi);
            free(remap);
            free(av);
            free(ai);
        }
        free(v);
    }
}

static void ensure_skins(void);

static void ensure_gpu_meshes(void) {
    ensure_globals();
    if (!LIB || CG->lib_revision == LIB->revision) return;
    for (int c = 0; c < LIB_MAX_CHARS; c++) {
        for (int l = 0; l < CHAR_LOD_COUNT; l++) { render_mesh_destroy(CG->mesh[c][l]); CG->mesh[c][l] = 0; }
        render_mesh_destroy(CG->arm_mesh[c]);
        render_mesh_destroy(CG->noarm_mesh[c]);
        CG->arm_mesh[c] = CG->noarm_mesh[c] = 0;
    }
    const Skeleton *s = &LIB->skel;
    // Bone sets for the first-person body: both arm chains (upper arm and below) and the head.
    for (int i = 0; i < s->count; i++) {
        int p = s->parent[i];
        CG->arm_bone[i] = i == s->role[BR_L_ARM] || i == s->role[BR_R_ARM] || (p >= 0 && CG->arm_bone[p]);
        CG->head_bone[i] = i == s->role[BR_HEAD] || i == s->role[BR_NECK] || (p >= 0 && CG->head_bone[p]);
    }
    for (int i = 0; i < s->count; i++) CG->inv_bind[i] = xf_to_mat4(xf_inverse(s->bind_model[i]));
    for (int i = 0; i < s->count; i++) {
        int b = i;
        while (b >= 0 && !s->lod_keep[b]) b = s->parent[b];
        CG->keep_map[i] = (i16)(b >= 0 ? b : 0);
    }
    for (int c = 0; c < LIB->char_count; c++) {
        const CharacterDef *d = &LIB->chars[c];
        if (!d->valid) continue;
        const SkinMeshData *m = &LIB->meshes[d->mesh];
        u32 colors[SKIN_MAX_MATERIALS];
        for (int k = 0; k < m->material_count; k++) {
            vec4 col = m->material_colors[k];
            for (int o = 0; o < d->material_count; o++)
                if (!strcmp(d->material_names[o], m->material_names[k])) col = d->material_colors[o];
            colors[k] = rgba8v(col);
        }
        build_char_meshes(m, colors, NULL, 0, CG->keep_map, CG->mesh[c], &CG->arm_mesh[c], &CG->noarm_mesh[c]);
    }
    CG->lib_revision = LIB->revision;
    CG->skins_loaded = false;
    ensure_skins();
}

// ------------------------------------------------------------------ skins (assets/human/skins.json)

static int skin_find(const char *id) {
    for (int k = 0; k < CG->skin_count; k++) if (!strcmp(CG->skins[k].id, id)) return k;
    return -1;
}

static void ensure_skins(void) {
    if (CG->skins_loaded || !LIB) return;
    CG->skins_loaded = true;
    for (int k = 0; k < CG->skin_count; k++) {
        for (int l = 0; l < CHAR_LOD_COUNT; l++) render_mesh_destroy(CG->skins[k].lod[l]);
        render_mesh_destroy(CG->skins[k].arm);
        render_mesh_destroy(CG->skins[k].noarm);
    }
    CG->skin_count = 0;
    CG->npc_count = 0;
    CG->player_skin = -1;
    char buf[512];
    char *text = platform_read_file(platform_data_path("assets/human/skins.json", buf, sizeof(buf)), NULL);
    cJSON *root = text ? cJSON_Parse(text) : NULL;
    free(text);
    if (!root) return;
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(root, "skins")) {
        if (CG->skin_count >= MAX_SKINS) break;
        CharSkin *sk = &CG->skins[CG->skin_count];
        memset(sk, 0, sizeof(*sk));
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(e, "id"), *nm = cJSON_GetObjectItemCaseSensitive(e, "name");
        const cJSON *mesh = cJSON_GetObjectItemCaseSensitive(e, "mesh"), *atlas = cJSON_GetObjectItemCaseSensitive(e, "atlas");
        if (!cJSON_IsString(id) || !cJSON_IsString(mesh)) continue;
        dg_strcpy(sk->id, sizeof(sk->id), id->valuestring);
        dg_strcpy(sk->name, sizeof(sk->name), cJSON_IsString(nm) ? nm->valuestring : id->valuestring);
        int mi = anim_lib_load_mesh(LIB, mesh->valuestring);
        if (mi < 0) { log_warn("skin %s: mesh %s unavailable", sk->id, mesh->valuestring); continue; }
        const SkinMeshData *m = &LIB->meshes[mi];
        // Material -> atlas tile; textured materials are white (the texture is the colour).
        int tile[SKIN_MAX_MATERIALS];
        u32 colors[SKIN_MAX_MATERIALS];
        int tiles = 0;
        for (int k = 0; k < m->material_count; k++) { tile[k] = 0; colors[k] = rgba8v(m->material_colors[k]); }
        const cJSON *t;
        cJSON_ArrayForEach(t, cJSON_GetObjectItemCaseSensitive(e, "tiles")) {
            const cJSON *mat = cJSON_GetObjectItemCaseSensitive(t, "material");
            if (cJSON_IsString(mat))
                for (int k = 0; k < m->material_count; k++)
                    if (!strcmp(m->material_names[k], mat->valuestring)) { tile[k] = tiles; colors[k] = 0xFFFFFFFFu; }
            tiles++;
        }
        if (tiles && cJSON_IsString(atlas)) sk->texture = assets_texture(atlas->valuestring);
        if (!sk->texture) tiles = 0;
        build_char_meshes(m, colors, tile, tiles, CG->keep_map, sk->lod, &sk->arm, &sk->noarm);
        CG->skin_count++;
    }
    const cJSON *pl = cJSON_GetObjectItemCaseSensitive(root, "player");
    if (cJSON_IsString(pl)) CG->player_skin = skin_find(pl->valuestring);
    const cJSON *npc = cJSON_GetObjectItemCaseSensitive(root, "npc");
    cJSON_ArrayForEach(e, npc) {
        if (CG->npc_count >= LIB_MAX_CHARS || !cJSON_IsString(e)) continue;
        dg_strcpy(CG->npc_def[CG->npc_count], 32, e->string);
        CG->npc_skin[CG->npc_count] = skin_find(e->valuestring);
        CG->npc_count++;
    }
    cJSON_Delete(root);
    log_info("characters: %d skin(s), player default '%s'", CG->skin_count, CG->player_skin >= 0 ? CG->skins[CG->player_skin].id : "-");
}

// Which skin a character instance wears: the local player's choice (cl_skin), other players the
// default, non-player characters by type (skins.json "npc"); -1 = the character's own mesh.
static int skin_for(const World *w, const CharacterInst *c, int local_slot) {
    ensure_skins();
    if (!CG->skin_count) return -1;
    if (c->is_player) {
        if (c->slot == local_slot && CG->skin_cvar && CG->skin_cvar->str[0]) {
            int k = skin_find(CG->skin_cvar->str);
            if (k >= 0) return k;
        }
        return CG->player_skin;
    }
    (void)w;
    for (int k = 0; k < CG->npc_count; k++)
        if (!strcmp(CG->npc_def[k], c->def->id)) return CG->npc_skin[k];
    return -1;
}

int chars_skin_count(void) { ensure_globals(); ensure_skins(); return CG->skin_count; }
const char *chars_skin_id(int i) { return i >= 0 && i < CG->skin_count ? CG->skins[i].id : ""; }
const char *chars_skin_name(int i) { return i >= 0 && i < CG->skin_count ? CG->skins[i].name : ""; }

// ------------------------------------------------------------------ system

void chars_system_create(World *w) {
    if (!LIB || !LIB->loaded) return;
    ensure_globals();
    w->chars = (CharacterSystem *)calloc(1, sizeof(CharacterSystem));
    if (!w->anim_events) w->anim_events = (AnimEvent *)calloc(MAX_PENDING_EVENTS, sizeof(AnimEvent));
}

static void free_inst(World *w, CharacterInst *c) {
    if (c->rag.active) { ragdoll_destroy(&c->rag, w->phys); w->chars->ragdolls--; }
    free(c->history);
    free(c->pose_prev);
    free(c->pose_cur);
    free(c->settled_pose);
    free(c);
}

void chars_system_destroy(World *w) {
    if (w->chars) {
        for (int i = 0; i < MAX_CHARACTERS; i++)
            if (w->chars->c[i]) free_inst(w, w->chars->c[i]);
        free(w->chars);
        w->chars = NULL;
    }
    free(w->anim_events);
    w->anim_events = NULL;
}

int chars_count(const World *w) { return w->chars ? w->chars->count : 0; }
void chars_budget_stats(int *evals, int *deferred) {
    *evals = CG ? CG->last_evals : 0;
    *deferred = CG ? CG->last_deferred : 0;
}

CharacterInst *char_get_index(const World *w, int i) {
    if (!w->chars || i <= 0 || i >= MAX_ENTITIES) return NULL;
    u16 k = w->chars->index[i];
    return k ? w->chars->c[k - 1] : NULL;
}

CharacterInst *char_get(const World *w, EntityId e) {
    if (!ent_valid(w, e)) return NULL;
    CharacterInst *c = char_get_index(w, (int)ENT_INDEX(e));
    return (c && c->ent == e) ? c : NULL;
}

static vec3 feet_of(const World *w, int i) {
    // Dummies are placed at their capsule center; everything else at the feet.
    return w->type[i] == ET_DUMMY ? v3_sub(w->xf[i].pos, v3(0, 0.9f, 0)) : w->xf[i].pos;
}

CharacterInst *char_attach(World *w, EntityId e, u8 character_id) {
    if (!w->chars || !LIB) return NULL;
    int i = (int)ENT_INDEX(e);
    CharacterInst *existing = char_get(w, e);
    if (existing) return existing;
    const CharacterDef *def = anim_lib_char_by_net(LIB, character_id);
    if (!def) return NULL;
    int slot = -1;
    for (int k = 0; k < MAX_CHARACTERS; k++) if (!w->chars->c[k]) { slot = k; break; }
    if (slot < 0) { log_warn("characters: limit reached"); return NULL; }
    CharacterInst *c = (CharacterInst *)calloc(1, sizeof(CharacterInst));
    c->ent = e;
    c->def = def;
    c->def_index = (int)(def - LIB->chars);
    charrt_init(&c->rt, LIB, def);
    c->pose_prev = (Xform *)calloc(ANIM_MAX_BONES, sizeof(Xform));
    c->pose_cur = (Xform *)calloc(ANIM_MAX_BONES, sizeof(Xform));
    if (w->authority) c->history = (HitboxHistory *)calloc(1, sizeof(HitboxHistory));
    c->body_yaw = quat_get_yaw(w->xf[i].rot);
    c->root = xf_make(feet_of(w, i), quat_yaw(c->body_yaw));
    c->is_player = w->type[i] == ET_PLAYER;
    c->slot = c->is_player ? w->player_slot[i] : -1;
    c->lod = -1;
    c->net.character_id = def->net_id;
    c->was_grounded = true;
    w->char_id[i] = def->net_id;
    w->chars->c[slot] = c;
    w->chars->index[i] = (u16)(slot + 1);
    w->chars->count++;
    return c;
}

void char_detach(World *w, EntityId e) {
    if (!w->chars) return;
    int i = (int)ENT_INDEX(e);
    u16 k = w->chars->index[i];
    if (!k) return;
    CharacterInst *c = w->chars->c[k - 1];
    if (c && c->ent != e) return;
    if (c) free_inst(w, c);
    w->chars->c[k - 1] = NULL;
    w->chars->index[i] = 0;
    w->chars->count--;
}

// ------------------------------------------------------------------ inputs

// Held weapon sockets with the character's attachments applied (sight socket, eye relief, left grip).
static void update_item_sockets(const World *w, CharacterInst *c, const u8 *att) {
    if (!att) att = w->attach[ENT_INDEX(c->ent)];
    memcpy(c->item_att, att, sizeof(c->item_att));
    if (!c->item_weapon) return;
    static WeaponDef eff;
    attach_weapon_effective(w->items, c->item_weapon, att, &eff);
    c->item_sock = eff.sockets;
}

static const ItemSockets *held_sockets(const CharacterInst *c) {
    return c->item_weapon ? &c->item_sock : &c->item_def->sockets;
}

static int item_hold_style(const World *w, u16 item, const WeaponDef **wd_out, const ItemDef **it_out) {
    *wd_out = NULL;
    *it_out = NULL;
    if (!item || !LIB) return HOLD_NONE;
    const ItemDef *it = item_def(w->items, item);
    const WeaponDef *wd = item_weapon(w->items, item);
    *wd_out = wd;
    *it_out = it;
    const char *style = wd ? wd->hold_style : it->hold_style;
    int s = proc_hold_style_index(&LIB->proc, style);
    return s >= 0 ? s : HOLD_ITEM_SMALL;
}

static vec2 local_move(vec3 vel, float body_yaw, float *speed) {
    vec3 h = v3_flat(vel);
    float sp = v3_len(h);
    *speed = sp;
    if (sp < 0.05f) return v2(0, 1);
    vec3 f = forward_from_yaw(body_yaw), r = right_from_yaw(body_yaw);
    return v2(v3_dot(h, r) / sp, v3_dot(h, f) / sp);
}

static void player_inputs(World *w, CharacterInst *c, const Player *p, CharInputs *in) {
    const PlayerMove *m = &p->sim.move;
    const WeaponState *ws = &p->sim.wpn;
    memset(in, 0, sizeof(*in));
    in->move_local = local_move(m->vel, m->body_yaw, &in->speed);
    in->grounded = m->on_ground != 0;
    in->crouch = m->crouch_amount > 0.5f;
    in->sprint = m->sprinting != 0;
    in->ads = ws->ads_t > 0.5f;
    in->mantling = m->mode == MOVE_MANTLE;
    in->vaulting = m->mode == MOVE_VAULT;
    in->hanging = m->mode == MOVE_HANG;
    in->dead = m->mode == MOVE_DEAD || (w->flags[ENT_INDEX(p->ent)] & EF_DEAD);
    in->mantle_t = m->m_t;
    in->vertical_vel = m->vel.y;
    in->aim_pitch = m->pitch;
    in->aim_yaw = m->yaw;
    in->body_yaw = m->body_yaw;
    const InvSlot *eq = weapon_equipped(w, c->slot);
    u16 item = eq ? eq->item : 0;
    in->weapon_id = item;
    in->weapon_type = item_hold_style(w, item, &c->item_weapon, &c->item_def);
    static const u8 none[ATT_SLOTS];
    update_item_sockets(w, c, eq ? eq->att : none);   // players: straight from the (replicated) inventory
    in->health = w->health[ENT_INDEX(p->ent)].hp;
    in->limb_status = c->limb;
}

static void npc_inputs(World *w, CharacterInst *c, int i, CharInputs *in) {
    memset(in, 0, sizeof(*in));
    vec3 vel = v3_scale(v3_sub(w->xf[i].pos, w->xf[i].prev_pos), (float)SIM_HZ);
    if (w->type[i] == ET_ZOMBIE) vel = v3_scale(forward_from_yaw(c->ai.yaw), c->ai.speed);
    in->body_yaw = c->body_yaw;
    in->move_local = local_move(vel, c->body_yaw, &in->speed);
    in->grounded = true;
    in->aim_yaw = c->body_yaw;
    in->aim_pitch = 0;
    in->dead = (w->flags[i] & EF_DEAD) != 0;
    in->attacking = c->ai.attack_t > 0;
    in->health = w->health[i].hp;
    in->limb_status = c->limb;
    in->weapon_type = HOLD_NONE;
    if (w->item[i]) in->weapon_type = item_hold_style(w, w->item[i], &c->item_weapon, &c->item_def);
    update_item_sockets(w, c, NULL);
    in->weapon_id = w->item[i];
}

static void capture_net(World *w, CharacterInst *c) {
    AnimNetState *n = &c->net;
    const CharInputs *in = &c->rt.in;
    const ControllerInst *ct = &c->rt.ctrl;
    for (int l = 0; l < 2; l++) {
        n->state[l] = (u8)(l < ct->def->layer_count ? ct->layer[l].state : 0);
        if (c->last_state[l] != ct->layer[l].state) { c->last_state[l] = ct->layer[l].state; c->state_enter_tick[l] = w->tick - ct->layer[l].state_ticks; }
    }
    n->phase = anq_phase(ct->layer[0].phase);
    n->state_start = (u16)(c->state_enter_tick[0] & 0xFFFF);
    n->speed = anq_speed(in->speed);
    n->move_x = anq_unit(in->move_local.x);
    n->move_y = anq_unit(in->move_local.y);
    n->aim_pitch = anq_pitch(in->aim_pitch);
    n->aim_yaw = anq_angle(in->aim_yaw);
    n->body_yaw = anq_angle(in->body_yaw);
    u8 f = 0;
    if (in->grounded) f |= ANF_GROUNDED;
    if (in->crouch) f |= ANF_CROUCH;
    if (in->prone) f |= ANF_PRONE;
    if (in->sprint) f |= ANF_SPRINT;
    if (in->ads) f |= ANF_ADS;
    if (in->mantling || in->vaulting) f |= ANF_MANTLE;
    if (in->hanging) f |= ANF_HANG;
    if (in->dead) f |= ANF_DEAD;
    n->flags = f;
    n->limb_status = c->limb;
    n->weapon = (u8)DG_MIN(in->weapon_id, 255);
    n->mantle_t = (u8)DG_CLAMP((int)lrintf(in->mantle_t * 255.0f), 0, 255);
    n->character_id = c->def->net_id;
    if (in->dead) { n->death_tick = c->death_tick; n->death_seed = c->death_seed; }
    else { n->death_tick = 0; n->death_seed = 0; }
}

// Inputs reconstructed from replicated state (quantized exactly like the host sent them).
static void inputs_from_net(const World *w, CharacterInst *c, const AnimNetState *s, CharInputs *in) {
    memset(in, 0, sizeof(*in));
    in->speed = and_speed(s->speed);
    in->move_local = v2(and_unit(s->move_x), and_unit(s->move_y));
    in->grounded = (s->flags & ANF_GROUNDED) != 0;
    in->crouch = (s->flags & ANF_CROUCH) != 0;
    in->prone = (s->flags & ANF_PRONE) != 0;
    in->sprint = (s->flags & ANF_SPRINT) != 0;
    in->ads = (s->flags & ANF_ADS) != 0;
    in->mantling = (s->flags & ANF_MANTLE) != 0;
    in->hanging = (s->flags & ANF_HANG) != 0;
    in->dead = (s->flags & ANF_DEAD) != 0;
    in->mantle_t = (float)s->mantle_t / 255.0f;
    in->aim_pitch = and_pitch(s->aim_pitch);
    in->aim_yaw = and_angle(s->aim_yaw);
    in->body_yaw = and_angle(s->body_yaw);
    in->limb_status = s->limb_status;
    in->weapon_id = s->weapon;
    in->weapon_type = item_hold_style(w, s->weapon, &c->item_weapon, &c->item_def);
    update_item_sockets(w, c, NULL);
}

// ------------------------------------------------------------------ host sim

static void host_triggers(World *w, CharacterInst *c, int i) {
    CharInputs *in = &c->rt.in;
    AnimEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.ent = c->ent;
    ev.tick = w->tick;
    if (c->is_player && c->slot >= 0) {
        const Player *p = &w->players[c->slot];
        const PlayerMove *m = &p->sim.move;
        if (m->land_tick != c->last_land_tick) {
            c->last_land_tick = m->land_tick;
            charrt_trigger(&c->rt, AP_T_LAND);
            ev.type = AEV_LAND;
            ev.extra = (u8)DG_CLAMP((int)(fabsf(m->land_speed) * 8.0f), 0, 255);
            chars_push_event(w, &ev);
        }
        if (c->was_grounded && !in->grounded && m->vel.y > 1.5f) {
            charrt_trigger(&c->rt, AP_T_JUMP);
            ev.type = AEV_JUMP;
            chars_push_event(w, &ev);
        }
        bool reloading = p->sim.wpn.reloading != 0;
        if (reloading && !c->was_reloading) {
            charrt_trigger(&c->rt, AP_T_RELOAD);
            ev.type = AEV_RELOAD;
            chars_push_event(w, &ev);
        }
        c->was_reloading = reloading;
        if (p->sim.wpn.equip_t < c->last_equip_t) {
            charrt_trigger(&c->rt, AP_T_EQUIP);
            ev.type = AEV_EQUIP;
            ev.extra = (u8)in->weapon_type;
            chars_push_event(w, &ev);
        }
        c->last_equip_t = p->sim.wpn.equip_t;
    }
    c->was_grounded = in->grounded;
    (void)i;
}

void chars_notify_attack(World *w, int slot, bool melee, bool first) {
    if (!w->chars || slot < 0) return;
    Player *p = &w->players[slot];
    CharacterInst *c = char_get(w, p->ent);
    if (!c) return;
    if (w->authority) {
        charrt_trigger(&c->rt, melee ? AP_T_MELEE : AP_T_FIRE);
        AnimEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = melee ? AEV_MELEE : AEV_FIRE;
        ev.ent = c->ent;
        ev.tick = w->tick;
        ev.extra = (u8)c->rt.in.weapon_type;
        chars_push_event(w, &ev);
    } else if (first && p->local) {
        charrt_trigger(&c->rt, melee ? AP_T_MELEE : AP_T_FIRE);
    }

}

static void update_limbs(CharacterInst *c, float dt) {
    if (c->leg_t > 0) c->leg_t -= dt;
    if (c->arm_t > 0) c->arm_t -= dt;
    c->limb = (u8)((c->leg_t > 0 ? LIMB_LEGS : 0) | (c->arm_t > 0 ? LIMB_ARMS : 0));
}

static void zombie_ai(World *w, CharacterInst *c, int i, float dt);

static void revive(World *w, CharacterInst *c) {
    c->dead = c->rag_pending = c->settled = c->tool_rag = false;
    if (c->rag.active) { ragdoll_destroy(&c->rag, w->phys); w->chars->ragdolls--; }
    free(c->settled_pose);
    c->settled_pose = NULL;
    proc_reset(&c->rt.proc);
    c->have_pose = false;
}

static void sim_one(World *w, CharacterInst *c, int i, float dt) {
    bool dead_now = (w->flags[i] & EF_DEAD) != 0 || (c->is_player && c->slot >= 0 && w->players[c->slot].sim.move.mode == MOVE_DEAD);
    if (c->dead && !dead_now && !c->tool_rag) revive(w, c);
    CharInputs in;
    if (c->is_player && c->slot >= 0 && w->players[c->slot].active) player_inputs(w, c, &w->players[c->slot], &in);
    else npc_inputs(w, c, i, &in);
    c->body_yaw = in.body_yaw;
    charrt_set_inputs(&c->rt, &in);
    host_triggers(w, c, i);
    charrt_tick(&c->rt, dt);
    charrt_eval_base(&c->rt);
    // Root motion (host only) for clips flagged root_motion: NPCs move by it.
    if (!c->is_player) {
        vec3 dp;
        float dy;
        ctrl_root_delta(&c->rt.ctrl, &LIB->lookup, dt, &dp, &dy);
        if (v3_len2(dp) > 0 || dy != 0) {
            vec3 wd = quat_rotate(quat_yaw(c->body_yaw), v3_scale(dp, c->def->scale));
            w->xf[i].pos = v3_add(w->xf[i].pos, wd);
            c->body_yaw = wrap_angle(c->body_yaw + dy);
            c->ai.yaw = c->body_yaw;
        }
    }
    c->root = xf_make(feet_of(w, i), quat_yaw(c->body_yaw));
    // Hitboxes from the sim-only pose (never from render LOD / IK).
    Xform world[ANIM_MAX_BONES];
    charrt_to_world(&c->rt, c->rt.base_model, c->root, c->def->scale, world);
    hitbox_build(&LIB->hitboxes, &LIB->ragdoll, world, c->def->ragdoll_scale, &c->hit);
    c->hit.tick = w->tick;
    if (c->history) hitbox_record(c->history, &c->hit);
    update_limbs(c, dt);
    capture_net(w, c);
}

void chars_host_tick(World *w, float dt) {
    if (!w->chars || !LIB) return;
    w->chars->tick = w->tick;
    for (int k = 0; k < MAX_CHARACTERS; k++) {
        CharacterInst *c = w->chars->c[k];
        if (!c || !ent_valid(w, c->ent)) continue;
        int i = (int)ENT_INDEX(c->ent);
        if (w->type[i] == ET_ZOMBIE) zombie_ai(w, c, i, dt);
        sim_one(w, c, i, dt);
    }
    // Host-side dropped item lifetimes.
    ENT_FOREACH(w, i) {
        if (w->life[i] > 0) {
            w->life[i] -= dt;
            if (w->life[i] <= 0) ent_destroy(w, ENT_MAKE(i, w->gen[i]));
        }
    }
}

// ------------------------------------------------------------------ clients

void chars_client_receive(World *w, EntityId e, const AnimNetState *s, u32 tick) {
    CharacterInst *c = char_get(w, e);
    if (!c) return;
    NetSample *last = c->sample_count ? &c->samples[(c->sample_head + CHAR_NET_SAMPLES - 1) % CHAR_NET_SAMPLES] : NULL;
    if (last && last->tick >= tick) return;
    c->samples[c->sample_head].tick = tick;
    c->samples[c->sample_head].s = *s;
    c->sample_head = (c->sample_head + 1) % CHAR_NET_SAMPLES;
    if (c->sample_count < CHAR_NET_SAMPLES) c->sample_count++;
    c->net = *s;
}

static float lerp_angle(float a, float b, float t) { return wrap_angle(a + wrap_angle(b - a) * t); }

// Replicated parameters interpolated at render time (continuous fields), discrete fields from
// the older sample.
static bool net_at(const CharacterInst *c, double rt, AnimNetState *a_out, u32 *a_tick, CharInputs *in, const World *w, CharacterInst *cm) {
    if (!c->sample_count) return false;
    int oldest = (c->sample_head - c->sample_count + CHAR_NET_SAMPLES) % CHAR_NET_SAMPLES;
    const NetSample *a = &c->samples[oldest], *b = a;
    for (int k = 0; k < c->sample_count; k++) {
        const NetSample *s = &c->samples[(oldest + k) % CHAR_NET_SAMPLES];
        if ((double)s->tick <= rt) a = s;
        if ((double)s->tick >= rt) { b = s; break; }
        b = s;
    }
    CharInputs ia, ib;
    inputs_from_net(w, cm, &a->s, &ia);
    inputs_from_net(w, cm, &b->s, &ib);
    float t = (b->tick > a->tick && rt > (double)a->tick) ? (float)((rt - (double)a->tick) / (double)(b->tick - a->tick)) : 0.0f;
    t = saturate(t);
    *in = ia;
    in->speed = lerpf(ia.speed, ib.speed, t);
    in->move_local = v2(lerpf(ia.move_local.x, ib.move_local.x, t), lerpf(ia.move_local.y, ib.move_local.y, t));
    float ml = v2_len(in->move_local);
    if (ml > 1e-3f) in->move_local = v2_scale(in->move_local, 1.0f / ml);
    in->aim_pitch = lerpf(ia.aim_pitch, ib.aim_pitch, t);
    in->aim_yaw = lerp_angle(ia.aim_yaw, ib.aim_yaw, t);
    in->body_yaw = lerp_angle(ia.body_yaw, ib.body_yaw, t);
    in->mantle_t = lerpf(ia.mantle_t, ib.mantle_t, t);
    *a_out = a->s;
    *a_tick = a->tick;
    return true;
}

static void client_one(World *w, CharacterInst *c, int i, double rt, float dt) {
    AnimNetState rep;
    u32 rep_tick = 0;
    CharInputs in;
    if (!net_at(c, rt, &rep, &rep_tick, &in, w, c)) return;
    // Remote characters: triggers that matter for state come from events; dead state from flags.
    // A die event can be due before the snapshot that carries the flag (snapshots are sparser
    // than ticks): the event's death holds until a newer snapshot says otherwise.
    bool rep_stale = c->dead && rep_tick < c->death_tick;
    if (rep_stale) { in.dead = true; rep.flags |= ANF_DEAD; }
    c->body_yaw = in.body_yaw;
    charrt_set_inputs(&c->rt, &in);
    charrt_tick(&c->rt, dt);
    // Divergence: snap to the host's state ids (with a short crossfade) if we disagree for a while.
    ControllerInst *ct = &c->rt.ctrl;
    bool mismatch = false;
    for (int l = 0; l < 2 && l < ct->def->layer_count; l++) mismatch |= ct->layer[l].state != rep.state[l];
    if (rep_stale) mismatch = false;   // the snapshot predates the death we already applied
    if (!c->synced) {
        // First state for this character (we just joined or it just appeared): adopt the host's
        // states directly - not a divergence, so no resync is counted.
        if (mismatch) c->mismatch_ticks = 1000;
        c->synced = true;
    }
    c->mismatch_ticks = mismatch ? c->mismatch_ticks + 1 : 0;
    if (c->mismatch_ticks > (CG->resync_ticks ? CG->resync_ticks->i : 8)) {
        u32 now = (u32)rt;
        u32 start = (now & 0xFFFF0000u) | rep.state_start;
        if (start > now) start -= 0x10000u;
        for (int l = 0; l < 2 && l < ct->def->layer_count; l++)
            if (ct->layer[l].state != rep.state[l]) ctrl_force_state(ct, l, rep.state[l], now - (l == 0 ? start : now), l == 0 ? and_phase(rep.phase) : 0);
        if (c->mismatch_ticks < 1000) c->resyncs++;
        c->mismatch_ticks = 0;
        if (cv(CG->net_debug)) log_info("anim: resync ent %u to state %s/%s", c->ent, ctrl_state_name(ct, 0, rep.state[0]), ctrl_state_name(ct, 1, rep.state[1]));
    } else if (!mismatch && ct->layer[0].state == rep.state[0]) {
        // Same state: pull the phase towards the host's (extrapolated) phase.
        float target = and_phase(rep.phase);
        float err = target - ct->layer[0].phase;
        err -= floorf(err + 0.5f);
        if (fabsf(err) > 0.25f) ct->layer[0].phase = target;
        else ct->layer[0].phase += err * 0.08f;
        ct->layer[0].phase -= floorf(ct->layer[0].phase);
    }
    c->limb = rep.limb_status;
    c->root = xf_make(feet_of(w, i), quat_yaw(c->body_yaw));
    // Death replicated (late joiners get the death tick + seed).
    if ((rep.flags & ANF_DEAD) && !c->dead && rep.death_tick) {
        c->dead = true;
        c->death_tick = rep.death_tick;
        c->death_seed = rep.death_seed;
        // Long dead already (or never seen alive): settled pose, no simulation.
        c->rag_pending = (u32)rt < rep.death_tick + 30 && c->have_pose;
        if (!c->rag_pending) c->settled = true;
    }
    if (!(rep.flags & ANF_DEAD) && c->dead && !c->tool_rag) revive(w, c);
}

static void apply_event(World *w, const AnimEvent *e);

void chars_client_tick(World *w, double rt, float dt) {
    if (!w->chars || !LIB) return;
    w->chars->tick = (u32)rt;
    // Due events (in arrival order).
    CharacterSystem *cs = w->chars;
    int keep = 0;
    for (int k = 0; k < cs->pending_count; k++) {
        if (cs->pending[k].tick <= cs->tick) apply_event(w, &cs->pending[k]);
        else cs->pending[keep++] = cs->pending[k];
    }
    cs->pending_count = keep;
    for (int k = 0; k < MAX_CHARACTERS; k++) {
        CharacterInst *c = w->chars->c[k];
        if (!c || !ent_valid(w, c->ent) || c->local) continue;
        client_one(w, c, (int)ENT_INDEX(c->ent), rt, dt);
    }
}

void chars_local_predict(World *w, int slot, bool first) {
    if (!w->chars || slot < 0 || !first) return;
    Player *p = &w->players[slot];
    CharacterInst *c = char_get(w, p->ent);
    if (!c) return;
    c->local = true;
    CharInputs in;
    player_inputs(w, c, p, &in);
    c->body_yaw = in.body_yaw;
    charrt_set_inputs(&c->rt, &in);
    if (c->was_grounded && !in.grounded && p->sim.move.vel.y > 1.5f) charrt_trigger(&c->rt, AP_T_JUMP);
    if (p->sim.move.land_tick != c->last_land_tick) { c->last_land_tick = p->sim.move.land_tick; charrt_trigger(&c->rt, AP_T_LAND); }
    c->was_grounded = in.grounded;
    charrt_tick(&c->rt, SIM_DT);
    // Snap to the host's replicated state ids when they disagree (reconciliation without re-sim).
    if (c->sample_count) {
        const AnimNetState *rep = &c->net;
        ControllerInst *ct = &c->rt.ctrl;
        bool mismatch = ct->layer[0].state != rep->state[0];
        c->mismatch_ticks = mismatch ? c->mismatch_ticks + 1 : 0;
        if (c->mismatch_ticks > 30) { ctrl_force_state(ct, 0, rep->state[0], 0, and_phase(rep->phase)); c->mismatch_ticks = 0; c->resyncs++; }
    }
    c->root = xf_make(p->sim.move.pos, quat_yaw(c->body_yaw));
}

const AnimNetState *chars_net_state(const World *w, int i) {
    CharacterInst *c = char_get_index(w, i);
    return c ? &c->net : NULL;
}

// ------------------------------------------------------------------ events

void chars_push_event(World *w, const AnimEvent *e) {
    if (!w->authority || !w->anim_events) return;
    if (w->anim_event_count < MAX_PENDING_EVENTS) w->anim_events[w->anim_event_count++] = *e;
    chars_on_event(w, e);   // the host's own view
}

static int body_bone_for_part(int part) {
    for (int b = 0; b < LIB->ragdoll.body_count; b++)
        if (LIB->ragdoll.bodies[b].part == part) return LIB->ragdoll.bodies[b].bone;
    return LIB->skel.role[BR_SPINE1];
}

static void start_reaction(CharacterInst *c, int part, vec3 dir_world, float strength) {
    vec3 dm = quat_rotate(quat_conj(c->root.r), dir_world);
    proc_damage_hit(&c->rt.proc, &LIB->proc, &LIB->skel, part, body_bone_for_part(part), dm, strength);
    if (LIB->proc.hit_mode != 0) {
        // Directional hit clip: 0 front, 1 back, 2 left, 3 right (relative to the body).
        float a = atan2f(dm.x, -dm.z);
        int dir = fabsf(a) < 0.785f ? 1 : (fabsf(a) > 2.356f ? 0 : (a > 0 ? 2 : 3));
        ctrl_set(&c->rt.ctrl, AP_HIT_DIR, (float)dir);
        ctrl_set(&c->rt.ctrl, AP_HIT_PART, (float)part);
        ctrl_trigger(&c->rt.ctrl, AP_T_HIT);
    }
}

void chars_on_event(World *w, const AnimEvent *e) {
    CharacterInst *c = char_get(w, e->ent);
    if (!c) return;
    // Remote characters on clients: defer to the event's tick in render time (bounded: an event
    // more than 2 s ahead of the render clock is applied at once).
    if (!w->authority && !c->local && e->tick > w->chars->tick && e->tick < w->chars->tick + 120 && w->chars->pending_count < 256) {
        w->chars->pending[w->chars->pending_count++] = *e;
        return;
    }
    apply_event(w, e);
}

static void apply_event(World *w, const AnimEvent *e) {
    CharacterInst *c = char_get(w, e->ent);
    if (!c) return;
    if (e->type < AEV_COUNT) c->events_received[e->type]++;
    bool remote = !w->authority && !c->local;
    switch (e->type) {
    case AEV_JUMP: if (remote) charrt_trigger(&c->rt, AP_T_JUMP); break;
    case AEV_LAND: if (remote) { ctrl_set(&c->rt.ctrl, AP_VERTICAL_VEL, -(float)e->extra / 8.0f); charrt_trigger(&c->rt, AP_T_LAND); } break;
    case AEV_RELOAD: if (remote) charrt_trigger(&c->rt, AP_T_RELOAD); break;
    case AEV_EQUIP: if (remote) charrt_trigger(&c->rt, AP_T_EQUIP); break;
    case AEV_MELEE: if (remote) charrt_trigger(&c->rt, AP_T_MELEE); break;
    case AEV_FIRE:
        if (remote) charrt_trigger(&c->rt, AP_T_FIRE);
        if (!c->local) c->kick_v = v3_add(c->kick_v, v3(0, 0.6f, 1.4f));   // third-person recoil on the item
        break;
    case AEV_HIT: {
        // A reaction we already predicted for this target / part within 0.3 s is not doubled.
        if (c->predicted_hit_t > 0 && c->predicted_hit_part == e->body_part) { c->predicted_hit_t = 0; break; }
        start_reaction(c, e->body_part, and_dir(e->dir), and_strength(e->strength));
        break;
    }
    case AEV_DIE:
        c->dead = true;
        c->death_tick = e->tick;
        c->death_seed = e->seed;
        c->death_part = e->body_part;
        c->death_impulse = v3_scale(and_dir(e->dir), and_strength(e->strength) * 60.0f);
        c->rag_pending = true;
        c->settled = false;
        if (remote) { ctrl_set(&c->rt.ctrl, AP_SEED, (float)e->seed); charrt_trigger(&c->rt, AP_T_DIE); }
        break;
    default: break;
    }
}

void chars_predict_hit(World *w, EntityId target, int part, vec3 dir) {
    if (CG && cv(CG->net_debug)) log_info("lagcomp client: predicted hit ent %u %s (render tick %.2f)", target, body_part_name(part), net_client_render_tick());
    if (!CG || !cv(CG->predict_hits)) return;
    CharacterInst *c = char_get(w, target);
    if (!c || c->dead) return;
    start_reaction(c, part, dir, 1.0f);
    c->predicted_hit_t = 0.3f;
    c->predicted_hit_part = part;
}

void chars_on_damage(World *w, EntityId target, float dealt, int part, vec3 dir, vec3 point, bool killed, int damage_type) {
    CharacterInst *c = char_get(w, target);
    if (!c || !w->authority) return;
    if (cv(CG->net_debug)) log_info("lagcomp host: hit ent %u %s for %.1f%s", target, body_part_name(part), dealt, killed ? " (killed)" : "");
    (void)point;
    // Limb effects (host-authoritative, replicated in limb_status).
    if (part >= 0 && part < BP_COUNT) {
        if (body_part_is_leg(part) && w->type[ENT_INDEX(target)] == ET_ZOMBIE) c->leg_t = 4.0f;
        if (body_part_is_arm(part) && c->is_player) c->arm_t = 3.0f;
    }
    float strength = clampf(dealt / 25.0f, 0.3f, 4.0f);
    AnimEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.ent = target;
    ev.tick = w->tick;
    ev.body_part = (u8)(part >= 0 && part < BP_COUNT ? part : BP_CHEST);
    ev.dir = anq_dir(v3_len2(dir) > 1e-8f ? dir : v3(0, 0, 1));
    ev.strength = anq_strength(strength);
    ev.damage_type = (u8)(damage_type & 0x0F);
    ev.seed = (u8)(dg_hash_u32(target ^ w->tick) & 0xFF);
    ev.type = killed ? AEV_DIE : AEV_HIT;
    if (killed) {
        ctrl_set(&c->rt.ctrl, AP_SEED, (float)ev.seed);
        charrt_trigger(&c->rt, AP_T_DIE);
        // Held item: dropped as a host-replicated physics prop (not for respawning test dummies,
        // which keep their item).
        int ti = (int)ENT_INDEX(target);
        u16 item = w->item[ti];
        if (item && c->have_pose && w->type[ti] != ET_DUMMY) {
            const ItemDef *it = item_def(w->items, item);
            vec3 hp = c->pose_cur[LIB->skel.role[BR_R_HAND]].t;
            EntityId pe = spawn_prop(w, hp, c->root.r, VIS_BOX, v3_scale(it->size, 1.0f), rgba8v(it->color), 3.0f);
            if (pe) {
                w->life[ENT_INDEX(pe)] = 30.0f;
                int pi = (int)ENT_INDEX(pe);
                if (w->body[pi].body != PHYS_INVALID_BODY) phys_add_impulse(w->phys, w->body[pi].body, v3_scale(dir, 2.0f), hp);
            }
        }
    }
    chars_push_event(w, &ev);
}

float chars_limb_spread(const World *w, EntityId e) {
    CharacterInst *c = char_get(w, e);
    return (c && (c->limb & LIMB_ARMS)) ? 1.2f : 0.0f;
}

// ------------------------------------------------------------------ hit tests

bool chars_raycast(World *w, EntityId ignore, vec3 o, vec3 d, float range, u32 view_tick256, EntityId *ent, HitboxRayHit *out) {
    if (!w->chars) return false;
    float best = range;
    bool found = false;
    HitboxSet rew;
    for (int k = 0; k < MAX_CHARACTERS; k++) {
        CharacterInst *c = w->chars->c[k];
        if (!c || c->ent == ignore || !ent_valid(w, c->ent) || c->dead) continue;
        if (w->flags[ENT_INDEX(c->ent)] & EF_DEAD) continue;
        const HitboxSet *set = &c->hit;
        if (w->authority && view_tick256 && c->history) {
            hitbox_rewind(c->history, view_tick256, &rew);
            set = &rew;
        }
        HitboxRayHit h;
        if (hitbox_raycast(set, o, d, best, &h)) {
            best = h.t;
            *out = h;
            *ent = c->ent;
            found = true;
        }
    }
    return found;
}

bool chars_sweep(World *w, EntityId ignore, vec3 a, vec3 b, float radius, EntityId *ent, HitboxRayHit *out) {
    if (!w->chars) return false;
    float best = 1e9f;
    bool found = false;
    for (int k = 0; k < MAX_CHARACTERS; k++) {
        CharacterInst *c = w->chars->c[k];
        if (!c || c->ent == ignore || !ent_valid(w, c->ent) || c->dead) continue;
        HitboxRayHit h;
        if (hitbox_sweep(&c->hit, a, b, radius, &h) && h.t < best) {
            best = h.t;
            *out = h;
            *ent = c->ent;
            found = true;
        }
    }
    return found;
}

// ------------------------------------------------------------------ presentation

static bool ray_cb(void *ctx, vec3 o, vec3 d, float *frac, vec3 *point, vec3 *normal) {
    RayHit rh;
    if (!phys_raycast((PhysicsWorld *)ctx, o, d, PL_MASK_WORLD, PHYS_INVALID_BODY, &rh)) return false;
    *frac = rh.fraction;
    *point = rh.point;
    *normal = rh.normal;
    return true;
}

static void spawn_ragdoll(World *w, CharacterInst *c) {
    c->rag_pending = false;
    if (!cv(CG->ragdoll) || !c->have_pose) { c->settled = true; return; }
    // Global cap: drop the oldest ragdoll to a settled pose.
    int cap = CG->max_ragdolls ? CG->max_ragdolls->i : 24;
    if (w->chars->ragdolls >= cap) {
        CharacterInst *old = NULL;
        for (int k = 0; k < MAX_CHARACTERS; k++) {
            CharacterInst *o = w->chars->c[k];
            if (o && o->rag.active && (!old || o->rag.age > old->rag.age)) old = o;
        }
        if (old) {
            ragdoll_read_pose(&old->rag, w->phys, &LIB->ragdoll, &LIB->skel, old->pose_cur);
            memcpy(old->pose_prev, old->pose_cur, sizeof(Xform) * (size_t)LIB->skel.count);
            ragdoll_destroy(&old->rag, w->phys);
            w->chars->ragdolls--;
            old->settled = true;
        }
    }
    RagdollSpawn sp;
    memset(&sp, 0, sizeof(sp));
    sp.bone_world = c->pose_cur;
    sp.prev_world = c->pose_prev;
    sp.dt = SIM_DT * (float)DG_MAX(1, c->interval);
    sp.impulse = c->death_impulse;
    sp.hit_part = c->death_part;
    sp.scale = c->def->ragdoll_scale;
    sp.group_id = (u32)(ENT_INDEX(c->ent) + 1);
    sp.solid = cv(CG->corpse_collision);
    sp.pose = &c->rt.final;
    sp.root = c->root;
    sp.remove_time = CG->ragdoll_time ? CG->ragdoll_time->f : 0.0f;
    if (ragdoll_create(&c->rag, w->phys, &LIB->ragdoll, &LIB->skel, &sp)) w->chars->ragdolls++;
}

static void settled_pose(CharacterInst *c) {
    if (!c->settled_pose) c->settled_pose = (Xform *)malloc(sizeof(Xform) * ANIM_MAX_BONES);
    ragdoll_settled_pose(&LIB->skel, &c->rt.base, c->root, c->death_seed, c->settled_pose);
}

static int systems_for_lod(int lod) { return lod; }

static void post_one(World *w, CharacterInst *c, int i, float dt, vec3 cam_pos, int local_slot) {
    (void)cam_pos;
    int lod = c->lod;
    if (!w->authority && !c->local) charrt_eval_base(&c->rt);   // clients: render-side base pose
    else if (c->local && !w->authority) charrt_eval_base(&c->rt);
    ProcInput pi;
    charrt_proc_input(&c->rt, &pi, c->root, dt * (float)DG_MAX(1, c->interval));
    pi.raycast = ray_cb;
    pi.ray_ctx = w->phys;
    int sys = systems_for_lod(lod);
    pi.foot_ik = cv(CG->foot_ik) && sys <= 0;
    pi.spine = cv(CG->spine) && sys <= 1;
    pi.turn = cv(CG->turn) && sys <= 0;
    pi.lean = cv(CG->lean) && sys <= 0;
    pi.damage = cv(CG->damage) && sys <= 1;
    pi.hold = cv(CG->hold) && sys <= 1;
    pi.ads_t = c->rt.in.ads ? 1.0f : 0.0f;
    ItemSockets sock;
    const ItemSockets *item = NULL;
    if (c->item_weapon) item = &c->item_sock;
    else if (c->item_def && c->rt.in.weapon_id) item = &c->item_def->sockets;
    if (item) { sock = *item; item = &sock; }
    pi.item = item;
    pi.item_kick_pos = v3(0, c->kick_x.y * 0.01f, c->kick_x.z * 0.02f);
    pi.item_kick_rot = v3(c->kick_x.y * 3.0f, 0, 0);
    if (c->local) {
        // The owner's first-person weapon is the body's: the viewmodel recoil springs drive it.
        vec3 kp, kr;
        hands_vm_kick(&kp, &kr);
        pi.item_kick_pos = kp;
        pi.item_kick_rot = kr;
    }
    {
        const WeaponCvars *wc = weapons_cvars();
        float kk = c->local ? 1.0f : (wc->kick_scale ? wc->kick_scale->f : 0.0f);   // the owner's springs are scaled in hands.c
        pi.item_kick_pos = v3_scale(pi.item_kick_pos, kk);
        pi.item_kick_rot = v3_scale(pi.item_kick_rot, kk);
        pi.hp_sway_scale = wc->sway_scale ? wc->sway_scale->f : 0.0f;
        pi.hp_bob_scale = wc->bob_scale ? wc->bob_scale->f : 0.0f;
    }
    if (c->rt.in.crouch) pi.crouch_drop = 0.38f;
    pi.first_person = c->local && cv(CG->fp_body);
    pi.hp_static = cv(CG->hp_static);
    if (CG->hp_force_state && CG->hp_force_state->i >= 0) {
        int fs = CG->hp_force_state->i;
        pi.ads_t = fs == HP_ADS ? 1.0f : 0.0f;
        pi.sprinting = fs == HP_SPRINT;
        pi.crouched = fs == HP_CROUCH;
        pi.prone = fs == HP_PRONE;
        pi.empty_hands = fs == HP_EMPTY;
        pi.inspect = fs == HP_INSPECT;
    }
    if (c->is_player && c->slot >= 0) {
        const PlayerMove *m = &w->players[c->slot].sim.move;
        if (m->hand_anchor_valid && (m->mode == MOVE_HANG || m->mode == MOVE_MANTLE || m->mode == MOVE_VAULT)) {
            pi.hand_anchor[0] = m->hand_anchor[0];
            pi.hand_anchor[1] = m->hand_anchor[1];
            pi.hand_anchor_w = 1.0f;
        }
    }
    charrt_post(&c->rt, &pi);
    // Aim of this post-pass (the first-person pass corrects the spine to the frame's view).
    c->fp_pitch[0] = c->fp_pitch[1];
    c->fp_yaw[0] = c->fp_yaw[1];
    c->fp_body_yaw[0] = c->fp_body_yaw[1];
    c->fp_pitch[1] = pi.aim_pitch;
    c->fp_yaw[1] = pi.aim_yaw;
    c->fp_body_yaw[1] = c->rt.proc.render_body_yaw;
    if (!c->have_pose) {
        c->fp_pitch[0] = c->fp_pitch[1];
        c->fp_yaw[0] = c->fp_yaw[1];
        c->fp_body_yaw[0] = c->fp_body_yaw[1];
    }
    // LOD2+: baked per-style arm pose, item pinned to the right hand.
    const Skeleton *s = &LIB->skel;
    int style = DG_CLAMP(c->rt.in.weapon_type, 0, HOLD_STYLE_MAX - 1);
    int arm_roots[2] = { s->role[BR_L_SHOULDER], s->role[BR_R_SHOULDER] };
    if (pi.hold && item && c->rt.proc.has_item && c->rt.proc.glue_err_pos < 0.002f) {
        for (int b = 0; b < s->count; b++)
            if (skel_is_descendant(s, b, arm_roots[0]) || skel_is_descendant(s, b, arm_roots[1])) CG->style_arms[style][b] = c->rt.final.local[b];
        CG->style_arms_valid[style] = true;
    }
    if (!pi.hold && item) {
        if (CG->style_arms_valid[style]) {
            for (int b = 0; b < s->count; b++)
                if (skel_is_descendant(s, b, arm_roots[0]) || skel_is_descendant(s, b, arm_roots[1])) c->rt.final.local[b] = CG->style_arms[style][b];
            pose_to_model(s, &c->rt.final, c->rt.final_model);
        }
        Xform palm_r = xf_mul(c->rt.final_model[s->role[BR_R_HAND]], LIB->hands.palm[1]);
        c->rt.proc.item_model = xf_mul(palm_r, xf_inverse(item->grip_r));
        c->rt.proc.item_world = xf_mul(xf_make(c->root.t, quat_yaw(c->rt.proc.render_body_yaw)), c->rt.proc.item_model);
        c->rt.proc.has_item = true;
    }
    if (!item) c->rt.proc.has_item = false;
    // Glue diagnostics.
    if (item && pi.hold && c->rt.proc.has_item && proc_hold_style(&LIB->proc, style)->left_hand && item->has_left && pi.hand_ik_weight >= 1.0f) {
        if (cv(CG->hold_assert) && (c->rt.proc.glue_err_pos > 0.002f || c->rt.proc.glue_err_ang > 1.0f)) {
            CG->hold_failures++;
            log_error("anim_hold_assert: ent %u style %s left hand %.2f mm / %.2f deg off grip_l", c->ent, proc_hold_style(&LIB->proc, style)->name,
                      c->rt.proc.glue_err_pos * 1000.0f, c->rt.proc.glue_err_ang);
        }
        if (cv(CG->hold_debug) && (int)(w->tick % 30) == 0)
            log_info("anim_hold_debug: ent %u %s glue %.3f mm / %.3f deg%s", c->ent, proc_hold_style(&LIB->proc, style)->name,
                     c->rt.proc.glue_err_pos * 1000.0f, c->rt.proc.glue_err_ang, c->rt.proc.reach_shifted ? " (shifted)" : "");
    }
    // World transforms: rendered root uses the smoothed body yaw.
    Xform root = xf_make(c->root.t, quat_yaw(c->rt.proc.render_body_yaw));
    memcpy(c->pose_prev, c->pose_cur, sizeof(Xform) * (size_t)s->count);
    c->item_prev = c->item_cur;
    c->item_cam_prev = c->item_cam_cur;
    if (c->rt.proc.has_item) c->item_cam_cur = c->rt.proc.item_cam;
    charrt_to_world(&c->rt, c->rt.final_model, root, c->def->scale, c->pose_cur);
    c->item_cur = c->rt.proc.has_item ? xf_mul(xf_make(root.t, root.r), xf_make(v3_scale(c->rt.proc.item_model.t, c->def->scale), c->rt.proc.item_model.r)) : c->item_cur;
    bool had_item = c->item_valid;
    c->item_valid = c->rt.proc.has_item;
    if (!c->have_pose || !had_item) { memcpy(c->pose_prev, c->pose_cur, sizeof(Xform) * (size_t)s->count); c->item_prev = c->item_cur; c->item_cam_prev = c->item_cam_cur; }
    c->have_pose = true;
    // Client-side hitboxes from the local pose (predicted reactions, debug).
    if (!w->authority) {
        Xform world[ANIM_MAX_BONES];
        charrt_to_world(&c->rt, c->rt.base_model, c->root, c->def->scale, world);
        hitbox_build(&LIB->hitboxes, &LIB->ragdoll, world, c->def->ragdoll_scale, &c->hit);
    }
    (void)i;
    (void)local_slot;
}

void chars_present_tick(World *w, vec3 cam_pos, float fov_deg, float screen_h, float dt, int local_slot) {
    if (!w->chars || !LIB) return;
    ensure_gpu_meshes();
    CharGlobals *g = CG;
    g->lod.bias = (g->lod_bias ? g->lod_bias->f : 1.0f) * (g->lod_quality ? 0.5f + 0.25f * (float)g->lod_quality->i : 1.0f);
    int force = g->lod_force && g->lod_force->i >= 0 ? g->lod_force->i : g->force_lod;
    float tanh = tanf(fov_deg * 0.5f * DG_DEG2RAD);
    bool step_phys = false;
    // Characters due for a post-pass this tick (evaluated after the loop, within the budget).
    static int due[MAX_CHARACTERS];
    int due_count = 0;
    for (int k = 0; k < MAX_CHARACTERS; k++) {
        CharacterInst *c = w->chars->c[k];
        if (!c || !ent_valid(w, c->ent)) continue;
        int i = (int)ENT_INDEX(c->ent);
        if (c->is_player && c->slot >= 0) c->local = c->slot == local_slot;
        if (c->predicted_hit_t > 0) c->predicted_hit_t -= dt;
        // Remote recoil kick spring.
        c->kick_v = v3_add(c->kick_v, v3_scale(v3_add(v3_scale(c->kick_x, -220.0f), v3_scale(c->kick_v, -22.0f)), dt));
        c->kick_x = v3_madd(c->kick_x, c->kick_v, dt);
        // LOD by projected height.
        float dist = fmaxf(0.1f, v3_dist(cam_pos, c->root.t));
        c->pixels = screen_h * (LIB->skel.height * c->def->scale) / (2.0f * dist * tanh);
        int lod = force >= 0 ? force : lod_select(&g->lod, c->lod, c->pixels * c->def->lod_bias);
        if (c->local) lod = 0;
        if (lod != c->lod) {
            c->fade_from = c->lod >= 0 ? c->lod : lod;
            c->lod_fade = c->lod >= 0 ? 0.0f : 1.0f;
            c->lod = lod;
        }
        c->lod_fade = fminf(1.0f, c->lod_fade + dt / 0.15f);
        c->interval = g->lod.update_every[DG_CLAMP(c->lod, 0, CHAR_LOD_COUNT - 1)];
        // Off-screen: the state machine keeps ticking (sim / client tick); the render pose freezes.
        c->culled = c->pixels < g->lod.cull_pixels;
        if (c->dead) {
            if (c->rag_pending) {
                bool sim_ok = c->lod <= 2 && !c->culled;
                if (!w->authority && w->chars->tick < c->death_tick) { /* wait for the death tick in render time */ }
                else if (sim_ok) spawn_ragdoll(w, c);
                else { c->rag_pending = false; c->settled = true; }
            }
            if (c->rag.active) {
                step_phys = true;
                bool alive = ragdoll_step(&c->rag, w->phys, &LIB->ragdoll, dt);
                // Keep the bone transforms current (debug draw, held-item drop, settled pose).
                memcpy(c->pose_prev, c->pose_cur, sizeof(Xform) * (size_t)LIB->skel.count);
                ragdoll_read_pose(&c->rag, w->phys, &LIB->ragdoll, &LIB->skel, c->pose_cur);
                if (!alive) {
                    ragdoll_destroy(&c->rag, w->phys);
                    w->chars->ragdolls--;
                    c->settled = true;
                    if (w->authority && w->type[i] == ET_ZOMBIE) { ent_destroy(w, c->ent); continue; }
                }
            } else if (c->settled && !c->settled_pose) {
                // Late joiners / long-dead characters never ran a post-pass: base pose once, then
                // the seeded lying pose (renderable from here on).
                if (!c->have_pose) charrt_eval_base(&c->rt);
                settled_pose(c);
                memcpy(c->pose_cur, c->settled_pose, sizeof(Xform) * (size_t)LIB->skel.count);
                memcpy(c->pose_prev, c->settled_pose, sizeof(Xform) * (size_t)LIB->skel.count);
                c->have_pose = true;
            }
            continue;
        }
        if (c->culled && c->have_pose) continue;
        if (c->have_pose && (int)(w->chars->tick - c->last_eval) < c->interval) continue;
        due[due_count++] = k;
    }
    // Budget: the local player first, then by projected size, then by how overdue (stable order).
    int budget = g->eval_budget && g->eval_budget->i > 0 ? g->eval_budget->i : MAX_CHARACTERS;
    if (due_count > budget) {
        for (int a = 1; a < due_count; a++) {
            int v = due[a], b = a - 1;
            const CharacterInst *cv_ = w->chars->c[v];
            float pv = cv_->local ? 1e9f : cv_->pixels * (1.0f + 0.25f * (float)(w->chars->tick - cv_->last_eval));
            while (b >= 0) {
                const CharacterInst *cb = w->chars->c[due[b]];
                float pb = cb->local ? 1e9f : cb->pixels * (1.0f + 0.25f * (float)(w->chars->tick - cb->last_eval));
                if (pb >= pv) break;
                due[b + 1] = due[b];
                b--;
            }
            due[b + 1] = v;
        }
    }
    int evals = due_count < budget ? due_count : budget;
    for (int n = 0; n < evals; n++) {
        CharacterInst *c = w->chars->c[due[n]];
        c->last_eval = w->chars->tick;
        post_one(w, c, (int)ENT_INDEX(c->ent), dt, cam_pos, local_slot);
    }
    g->last_evals = evals;
    g->last_deferred = due_count - evals;
    // Clients step their replica physics only for cosmetic ragdolls (the host steps its own world).
    // Ragdolls simulating: 2 collision substeps (joint limits + floor contacts hold on hard impacts).
    phys_set_collision_steps(w->phys, w->chars->ragdolls > 0 ? 2 : 1);
    if (step_phys && !w->authority) phys_step(w->phys, dt);
}

static mat4 g_palette[ANIM_MAX_BONES];

// Skin of the character being drawn (skin_for; -1 = the character's own mesh).
static int g_draw_skin = -1;

static MeshId char_lod_mesh(const CharacterInst *c, int lod) {
    lod = DG_CLAMP(lod, 0, CHAR_LOD_COUNT - 1);
    if (g_draw_skin >= 0) {
        for (int l = lod; l >= 0; l--) if (CG->skins[g_draw_skin].lod[l]) return CG->skins[g_draw_skin].lod[l];
        return 0;
    }
    return CG->mesh[c->def_index][lod];
}
static TextureId char_texture(void) { return g_draw_skin >= 0 ? CG->skins[g_draw_skin].texture : 0; }

static void draw_char(World *w, CharacterInst *c, const Xform *pose, int lod, float fade, u32 flags, bool shadow) {
    const Skeleton *s = &LIB->skel;
    MeshId mesh = char_lod_mesh(c, lod);
    if (!mesh) return;
    for (int b = 0; b < s->count; b++) g_palette[b] = mat4_mul(xf_to_mat4(pose[b]), CG->inv_bind[b]);
    SkinnedDraw d;
    memset(&d, 0, sizeof(d));
    d.mesh = mesh;
    d.palette = g_palette;
    d.bone_count = (u32)s->count;
    d.color = v4(1, 1, 1, 1);
    d.roughness = 0.75f;
    d.flags = flags;
    d.fade = fade;
    d.cast_shadow = shadow;
    d.center = pose[s->role[BR_HIPS]].t;
    d.radius = s->height * c->def->scale;
    d.texture = char_texture();
    if (d.texture) d.flags |= MAT_TEXTURED;
    // Stable id per character + LOD (previous palette -> per-bone velocity for TAA / motion blur).
    d.object_id = shadow ? 0u : (0x20000000u | ((u32)ENT_INDEX(c->ent) << 4) | (u32)DG_CLAMP(lod, 0, 7));
    render_draw_skinned(&d);
    (void)w;
}

static void draw_char_mesh(MeshId mesh, CharacterInst *c, const Xform *pose, u32 flags, bool shadow) {
    const Skeleton *s = &LIB->skel;
    if (!mesh) return;
    for (int b = 0; b < s->count; b++) g_palette[b] = mat4_mul(xf_to_mat4(pose[b]), CG->inv_bind[b]);
    SkinnedDraw d;
    memset(&d, 0, sizeof(d));
    d.mesh = mesh;
    d.palette = g_palette;
    d.bone_count = (u32)s->count;
    d.color = v4(1, 1, 1, 1);
    d.roughness = 0.75f;
    d.flags = flags;
    d.cast_shadow = shadow;
    d.center = pose[s->role[BR_HIPS]].t;
    d.radius = s->height * c->def->scale;
    d.texture = char_texture();
    if (d.texture) d.flags |= MAT_TEXTURED;
    d.object_id = 0x20000000u | ((u32)ENT_INDEX(c->ent) << 4) | ((flags & MAT_VIEWMODEL) ? 9u : 8u);
    render_draw_skinned(&d);
}

// Camera / eye offset produced by the aim pitch bending the spine (Spine, Spine1, Spine2, Neck,
// Head with the controller's spine weights), from the canonical bind pose: looking down moves
// the eyes forward and down over the feet. Pure function of (yaw, pitch) and the shared assets,
// so the host and every client get the same gameplay eye (player_eye_pos).
vec3 chars_eye_offset(float yaw, float pitch) {
    if (!LIB || !LIB->loaded) return v3(0, 0, 0);
    const Skeleton *s = &LIB->skel;
    const ProcParams *p = &LIB->proc;
    int bones[5] = { s->role[BR_SPINE], s->role[BR_SPINE1], s->role[BR_SPINE2], s->role[BR_NECK], s->role[BR_HEAD] };
    int head = s->role[BR_HEAD];
    if (head < 0) return v3(0, 0, 0);
    // Eyes: in front of and above the head joint (character faces -Z in bind).
    vec3 eye0 = v3_add(s->bind_model[head].t, v3(0, 0.08f, -0.09f));
    vec3 pivots[5];
    for (int i = 0; i < 5; i++) pivots[i] = bones[i] >= 0 ? s->bind_model[bones[i]].t : eye0;
    float pt = clampf(pitch, -p->pitch_limit * DG_DEG2RAD, p->pitch_limit_up * DG_DEG2RAD);
    vec3 eye = eye0;
    for (int i = 0; i < 5; i++) {
        if (bones[i] < 0) continue;
        quat q = quat_axis_angle(v3(1, 0, 0), pt * (pt > 0 ? p->spine_weights_up[i] : p->spine_weights[i]));
        eye = v3_add(pivots[i], quat_rotate(q, v3_sub(eye, pivots[i])));
        for (int j = i + 1; j < 5; j++) pivots[j] = v3_add(pivots[i], quat_rotate(q, v3_sub(pivots[j], pivots[i])));
    }
    return quat_rotate(quat_yaw(yaw), v3_sub(eye, eye0));
}

// This frame's first-person body pose (shared by the camera socket and the body / arm draw).
static struct {
    u64 frame;
    EntityId ent;
    Xform world[ANIM_MAX_BONES], model[ANIM_MAX_BONES];
    Pose pose;
    Xform root;
    Xform spine2_before;   // chest before the frame's spine correction (the held item follows it)
} FP;

static float lerp_angle_fp(float a, float b, float t) { return wrap_angle(a + wrap_angle(b - a) * t); }

static void fp_prepare(World *w, CharacterInst *c, float alpha, float yaw, float pitch) {
    const Skeleton *s = &LIB->skel;
    const ProcParams *pp = &LIB->proc;
    float span = (float)DG_MAX(1, c->interval);
    float it = saturate(((float)(w->chars->tick - c->last_eval) + alpha) / span);
    for (int b = 0; b < s->count; b++) FP.world[b] = xf_lerp(c->pose_prev[b], c->pose_cur[b], it);
    // Model space = the rendered body frame (feet + smoothed body yaw), as the post-pass used.
    float body_yaw = lerp_angle_fp(c->fp_body_yaw[0], c->fp_body_yaw[1], it);
    FP.root = xf_make(c->root.t, quat_yaw(body_yaw));
    FP.root.s = v3s(c->def->scale);
    Xform inv_root = xf_inverse(FP.root);
    FP.pose.count = s->count;
    for (int b = 0; b < s->count; b++) {
        FP.model[b] = xf_mul(inv_root, FP.world[b]);
        int p = s->parent[b];
        FP.pose.local[b] = p >= 0 ? xf_mul(xf_inverse(FP.model[p]), FP.model[b]) : FP.model[b];
    }
    // The post-pass aimed the spine at the tick's view; bend it the rest of the way to this
    // frame's view (same bones / weights / limits as proc_spine), so the head - and the camera
    // socket on it - follows the mouse without a tick of lag.
    if (s->role[BR_SPINE2] >= 0) FP.spine2_before = FP.world[s->role[BR_SPINE2]];
    if (cv(CG->spine)) {
        float lim_p = pp->pitch_limit * DG_DEG2RAD, lim_u = pp->pitch_limit_up * DG_DEG2RAD, lim_y = pp->yaw_limit * DG_DEG2RAD;
        float posed_pitch = clampf(lerpf(c->fp_pitch[0], c->fp_pitch[1], it), -lim_p, lim_u);
        float posed_aim = lerp_angle_fp(c->fp_yaw[0], c->fp_yaw[1], it);
        float posed_rel = clampf(wrap_angle(posed_aim - body_yaw), -lim_y, lim_y);
        float want_rel = clampf(wrap_angle(yaw - body_yaw), -lim_y, lim_y);
        float dy = want_rel - posed_rel;
        int bones[5] = { s->role[BR_SPINE], s->role[BR_SPINE1], s->role[BR_SPINE2], s->role[BR_NECK], s->role[BR_HEAD] };
        float acc = 0;
        for (int i = 0; i < 5; i++) {
            if (bones[i] < 0) continue;
            float wgt = pp->spine_weights[i];
            acc += want_rel * wgt;
            vec3 lateral = quat_rotate(quat_yaw(acc), v3(1, 0, 0));
            // Pitch: the difference of the up / down distributions between the tick's pose and
            // this frame's view (same split as proc_spine).
            float want_p = clampf(pitch, -lim_p, lim_u);
            float al = c->rt.proc.ads_lean;
            float pw = want_p * proc_spine_pitch_weight(pp, i, want_p, al) - posed_pitch * proc_spine_pitch_weight(pp, i, posed_pitch, al);
            proc_rotate_bone(s, &FP.pose, FP.model, bones[i], quat_mul(quat_axis_angle(lateral, pw), quat_yaw(dy * wgt)));
        }
    }
    for (int b = 0; b < s->count; b++) FP.world[b] = xf_mul(FP.root, FP.model[b]);
    FP.frame = g_engine.frame_index;
    FP.ent = c->ent;
}

static CharacterInst *local_fp_char(World *w, int local_slot);

// World transform of a role bone in this frame's first-person pose (tests / debug).
bool chars_fp_bone_world(int role, Xform *out) {
    if (!LIB || !CG || FP.frame + 1 < g_engine.frame_index) return false;   // this or the previous frame (console commands run before render)
    int b = LIB->skel.role[role];
    if (b < 0) return false;
    *out = FP.world[b];
    return true;
}

// fp_eye_check: where the "camera" socket sits on the player model's head (bind pose): relative
// to the head bone, the front of the face and the top of the skull (vertices mostly skinned to
// the head). The eye should be ~ 8..14 cm below the top and just behind the face surface.
static void cmd_fp_eye_check(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    ensure_globals();
    if (!LIB) return;
    const Skeleton *s = &LIB->skel;
    int head = s->role[BR_HEAD];
    if (head < 0) return;
    for (int c = 0; c < LIB->char_count; c++) {
        const CharacterDef *d = &LIB->chars[c];
        if (!d->valid) continue;
        Xform eye;
        if (!character_socket_world(d, "camera", s->bind_model, &eye)) continue;
        const SkinLod *L = &LIB->meshes[d->mesh].lods[0];
        float top = -1e9f, front = 1e9f, back = -1e9f, chin = 1e9f;
        int n = 0;
        for (u32 i = 0; i < L->vcount; i++) {
            const SkinVertex *v = &L->verts[i];
            int hw = 0;
            for (int k = 0; k < 4; k++) if (CG->head_bone[v->joints[k]] && v->joints[k] != s->role[BR_NECK]) hw += v->weights[k];
            if (hw < 200) continue;
            n++;
            top = fmaxf(top, v->pos.y);
            chin = fminf(chin, v->pos.y);
            front = fminf(front, v->pos.z);   // model forward = -Z
            back = fmaxf(back, v->pos.z);
            // Eye-level face front: the vertices near the eye height.
        }
        float face_at_eye = 1e9f;
        for (u32 i = 0; i < L->vcount; i++) {
            const SkinVertex *v = &L->verts[i];
            int hw = 0;
            for (int k = 0; k < 4; k++) if (CG->head_bone[v->joints[k]] && v->joints[k] != s->role[BR_NECK]) hw += v->weights[k];
            if (hw < 200 || fabsf(v->pos.y - eye.t.y) > 0.04f) continue;
            face_at_eye = fminf(face_at_eye, v->pos.z);
        }
        vec3 hb = s->bind_model[head].t;
        log_info("fp_eye_check %s: head bone (%.3f %.3f %.3f), eye (%.3f %.3f %.3f) = head + (%.3f up, %.3f fwd); head mesh %d verts: "
                 "top %.3f (eye %.3f below), chin %.3f, face front %.3f, back %.3f, face at eye height %.3f -> eye %.3f %s the face",
                 d->id, hb.x, hb.y, hb.z, eye.t.x, eye.t.y, eye.t.z, eye.t.y - hb.y, hb.z - eye.t.z, n, top, top - eye.t.y, chin, front, back,
                 face_at_eye, fabsf(eye.t.z - face_at_eye), eye.t.z < face_at_eye ? "in front of" : "behind");
    }
}

bool chars_fp_camera(World *w, float alpha, int local_slot, float yaw, float pitch, Xform *socket_world) {
    CharacterInst *c = local_fp_char(w, local_slot);
    if (!c) return false;
    fp_prepare(w, c, alpha, yaw, pitch);
    return character_socket_world(c->def, "camera", FP.world, socket_world);
}

static CharacterInst *local_fp_char(World *w, int local_slot) {
    if (!w->chars || !LIB || !CG || !cv(CG->fp_body)) return NULL;
    for (int k = 0; k < MAX_CHARACTERS; k++) {
        CharacterInst *c = w->chars->c[k];
        if (c && c->is_player && c->slot == local_slot && ent_valid(w, c->ent)) return (c->have_pose && !c->dead && !c->rag.active) ? c : NULL;
    }
    return NULL;
}

// Finger curl per first-person grip (deg: thumb, index, middle, ring, pinky), added to the pose.
static const float k_grip_curl[GRIP_COUNT][5] = {
    [GRIP_RELAXED] = { 10, 15, 20, 25, 30 },  [GRIP_FIST] = { 50, 95, 100, 100, 100 },
    [GRIP_POINT] = { 50, 0, 95, 95, 95 },     [GRIP_GRAB] = { 35, 50, 55, 60, 65 },
    [GRIP_PISTOL] = { 40, 70, 80, 85, 90 },   [GRIP_RIFLE] = { 40, 70, 80, 85, 90 },
    [GRIP_MELEE] = { 50, 85, 90, 90, 90 },    [GRIP_PALM] = { 0, 5, 5, 5, 5 },
};

void chars_render_local_fp(World *w, float alpha, int local_slot) {
    CharacterInst *c = local_fp_char(w, local_slot);
    HandsFpTargets t;
    ensure_globals();
    CG->fp_hold_w = 0;
    CG->fp_glue_pos[0] = CG->fp_glue_pos[1] = -1;   // no hold this frame
    if (!c || !hands_fp_targets(&t)) return;
    const Skeleton *s = &LIB->skel;
    // This frame's pose (prepared by the camera socket; otherwise aimed at the hands' camera).
    if (FP.frame != g_engine.frame_index || FP.ent != c->ent) {
        vec3 f = quat_rotate(t.cam_rot, v3(0, 0, -1));
        fp_prepare(w, c, alpha, atan2f(-f.x, -f.z), asinf(clampf(f.y, -1.0f, 1.0f)));
    }
    // True first person (Unreal-style): the body and the item it holds are exactly the
    // third-person character everyone else sees - procedural hold, both hands glued, no
    // viewmodel offsets, no stretching - and the camera sits on the head socket.
    Xform *world = FP.world, *model = FP.model;
    Pose *posep = &FP.pose;
    Xform root = FP.root;
    Xform inv_root = xf_inverse(root);
    bool raw = CG->fp_debug && CG->fp_debug->i >= 2;
    // Hold point (true first person): the item is a child of this frame's camera; both arms are
    // re-solved onto its sockets every frame, so hands stay glued and the item never lags the view.
    // Clip-driven states (reload / equip / melee: hand IK weight < 1) hand control to the right
    // hand bone; the item is always finalized from the solved right hand.
    if (c->item_valid && (c->item_weapon || c->item_def) && CG->fp_cam_valid && !raw) {
        const ItemSockets *k = held_sockets(c);
        float span = (float)DG_MAX(1, c->interval);
        float it = saturate(((float)(w->chars->tick - c->last_eval) + alpha) / span);
        Xform icam = xf_lerp(c->item_cam_prev, c->item_cam_cur, it);
        icam.s = v3(1, 1, 1);
        Xform cam_m = xf_mul(inv_root, CG->fp_cam);
        cam_m.s = v3(1, 1, 1);
        Xform want = xf_mul(cam_m, icam);
        float ikw = 1.0f;
        ctrl_hand_ik_weight(&c->rt.ctrl, &ikw);
        int style = DG_CLAMP(c->rt.in.weapon_type, 0, HOLD_STYLE_MAX - 1);
        const HoldStyle *hs = proc_hold_style(&LIB->proc, style);
        int ra = s->role[BR_R_ARM], rf = s->role[BR_R_FOREARM], rh = s->role[BR_R_HAND];
        int la = s->role[BR_L_ARM], lf = s->role[BR_L_FOREARM], lh = s->role[BR_L_HAND];
        quat cam_rm = cam_m.r;
        // Reach: this frame's spine (re-bent to the frame's view) can move the support shoulder
        // away from the tick's solve. If grip_l is out of reach, pull the hold point towards the
        // left shoulder by the shortfall (never stretch the arm); warn once per item.
        Xform grip_l = k->grip_l;   // ADS: may slide back along the handguard (exact ADS kept)
        if (hs->left_hand && k->has_left && la >= 0 && lf >= 0 && lh >= 0 && ikw > 0.99f) {
            float reach = v3_dist(model[la].t, model[lf].t) + v3_dist(model[lf].t, model[lh].t);
            float pulled = 0;
            bool ads_exact = c->rt.proc.hp_out.state == HP_ADS;
            float slide = 0, max_slide = ads_exact ? proc_grip_l_slide_max(k) : 0.0f;
            // Shoulder first (clavicle), then the support hand along the handguard (ADS), then the hold point.
            proc_clavicle_reach(s, posep, model, s->role[BR_L_SHOULDER], la, proc_hand_for_socket(xf_mul(want, k->grip_l), LIB->hands.palm[0]).t, reach, 0.004f, 75.0f);
            for (int iter = 0; iter < 8; iter++) {
                grip_l = k->grip_l;
                grip_l.t.z += slide;
                Xform hand_l = proc_hand_for_socket(xf_mul(want, grip_l), LIB->hands.palm[0]);
                float need = v3_dist(model[la].t, hand_l.t);
                if (need <= reach - 0.002f) break;
                if (need > reach + 0.5f) break;   // teleport frame: the body hasn't caught up with the camera yet
                if (slide < max_slide - 1e-5f) {
                    vec3 to_sh = v3_norm(v3_sub(model[la].t, hand_l.t));
                    float g = v3_dot(to_sh, quat_rotate(want.r, v3(0, 0, 1)));
                    if (g > 0.2f) {
                        slide = fminf(max_slide, slide + (need - reach + 0.003f) / g);
                        continue;
                    }
                }
                // Along the item's own barrel axis first (sights stay on the view axis in ADS),
                // towards the shoulder only when the barrel doesn't shorten the reach.
                vec3 to_sh = v3_norm(v3_sub(model[la].t, hand_l.t));
                vec3 back = quat_rotate(want.r, v3(0, 0, 1));
                float gain = v3_dot(to_sh, back);
                vec3 dir = gain >= 0.5f ? back : to_sh;
                float d = (need - reach + 0.003f) / (gain >= 0.5f ? gain : 1.0f);
                want.t = v3_madd(want.t, dir, d);
                pulled += d;
            }
            CG->fp_reach_pull = pulled;
            static u32 warned_item;
            u32 key = (u32)(uintptr_t)k;
            if (pulled > 0.03f && warned_item != key) {
                warned_item = key;
                log_warn("first person: grip_l out of reach this frame, hold point pulled %.0f mm towards the shoulder", pulled * 1000.0f);
            }
        }
        if (ra >= 0 && rf >= 0 && rh >= 0) {
            Xform hand_r = proc_hand_for_socket(xf_mul(want, k->grip_r), LIB->hands.palm[1]);
            if (ikw > 0.99f) {
                float reach_r = v3_dist(model[ra].t, model[rf].t) + v3_dist(model[rf].t, model[rh].t);
                proc_clavicle_reach(s, posep, model, s->role[BR_R_SHOULDER], ra, hand_r.t, reach_r, 0.003f, 45.0f);
            }
            anim_ik_two_bone_pose(s, posep, model, ra, rf, rh, hand_r.t, &hand_r.r, quat_rotate(cam_rm, hs->elbow_r), ikw);
        }
        Xform item = want;
        if (rh >= 0) item = xf_mul(xf_mul(model[rh], LIB->hands.palm[1]), xf_inverse(k->grip_r));
        Xform palm_r = rh >= 0 ? xf_mul(model[rh], LIB->hands.palm[1]) : xf_identity();
        Xform sock_r = xf_mul(want, k->grip_r);
        CG->fp_glue_pos[1] = v3_dist(palm_r.t, sock_r.t);
        CG->fp_glue_ang[1] = quat_angle_between(palm_r.r, sock_r.r) * DG_RAD2DEG;
        CG->fp_glue_pos[0] = CG->fp_glue_ang[0] = 0;
        CG->fp_hold_w = ikw;
        for (int h = 0; h < 2; h++) if (t.ov_w[h] > 0.001f) CG->fp_hold_w = 0;   // interaction / ledge grab owns a hand
        if (hs->left_hand && k->has_left && la >= 0 && lf >= 0 && lh >= 0 && ikw > 0.001f) {
            Xform sock_l = xf_mul(item, grip_l);
            Xform hand_l = proc_hand_for_socket(sock_l, LIB->hands.palm[0]);
            anim_ik_two_bone_pose(s, posep, model, la, lf, lh, hand_l.t, &hand_l.r, quat_rotate(cam_rm, hs->elbow_l), ikw);
            Xform palm_l = xf_mul(model[lh], LIB->hands.palm[0]);
            CG->fp_glue_pos[0] = v3_dist(palm_l.t, sock_l.t);
            CG->fp_glue_ang[0] = quat_angle_between(palm_l.r, sock_l.r) * DG_RAD2DEG;
        }
        // ADS check: the sight on the view axis (lateral error + barrel angle to the view).
        vec3 sight_c = xf_point(xf_mul(xf_inverse(cam_m), item), k->sight);
        CG->fp_ads_pos = sqrtf(sight_c.x * sight_c.x + sight_c.y * sight_c.y);
        CG->fp_sight_cam = sight_c;
        CG->fp_ads_ang = quat_angle_between(xf_mul(xf_inverse(cam_m), item).r, quat_identity()) * DG_RAD2DEG;
        CG->fp_item_cam = xf_mul(xf_inverse(cam_m), item);
        CG->fp_item_target = c->rt.proc.hp_out.item;   // the hold point's item (before any reach pull)
        CG->fp_hp_state = c->rt.proc.hp_out.state;
        CG->fp_hp_blend = c->rt.proc.hp_out.blend;
        Xform item_w = xf_mul(root, item);
        item_w.s = v3(1, 1, 1);
        hands_fp_set_item(item_w.t, item_w.r);
        // cam_debug: hold point axes, pivot, sockets, IK targets, collision cast.
        Cvar *cd = cvar_find("cam_debug");
        if (cd && cd->i) {
            Xform hold_w = xf_mul(CG->fp_cam, xf_mul(icam, xf_make(k->anchor.t, quat_identity())));
            vec3 o = hold_w.t;
            dbg_line(o, v3_add(o, quat_rotate(hold_w.r, v3(0.05f, 0, 0))), DBG_RED, 0, false);
            dbg_line(o, v3_add(o, quat_rotate(hold_w.r, v3(0, 0.05f, 0))), DBG_GREEN, 0, false);
            dbg_line(o, v3_add(o, quat_rotate(hold_w.r, v3(0, 0, -0.08f))), DBG_BLUE, 0, false);
            dbg_cross(xf_point(item_w, k->pivot), 0.015f, DBG_MAGENTA, 0);
            dbg_cross(xf_point(item_w, k->sight), 0.01f, DBG_CYAN, 0);
            dbg_cross(xf_point(item_w, k->muzzle), 0.012f, DBG_ORANGE, 0);
            dbg_cross(xf_mul(item_w, k->grip_r).t, 0.012f, DBG_YELLOW, 0);
            if (k->has_left) dbg_cross(xf_mul(item_w, k->grip_l).t, 0.012f, DBG_YELLOW, 0);
            const HoldPointOutput *ho = &c->rt.proc.hp_out;
            if (v3_len2(v3_sub(ho->cast_to, ho->cast_from)) > 0) dbg_line(ho->cast_from, ho->cast_to, ho->cast_hit ? DBG_RED : DBG_GREEN, 0, false);
        }
    } else if (c->item_valid) {
        float span = (float)DG_MAX(1, c->interval);
        float it = saturate(((float)(w->chars->tick - c->last_eval) + alpha) / span);
        Xform item = xf_lerp(c->item_prev, c->item_cur, it);
        item.s = v3(1, 1, 1);
        hands_fp_set_item(item.t, item.r);
    }
    // Interactions / ledge grabs from the first-person hand system (door knobs, levers, pickups
    // with the free hand) are IK'd on top of the body, blended by their weight.
    for (int h = 0; h < 2 && !raw; h++) {
        if (t.ov_w[h] <= 0.001f) continue;
        int ua = s->role[h ? BR_R_ARM : BR_L_ARM], fa = s->role[h ? BR_R_FOREARM : BR_L_FOREARM], ha = s->role[h ? BR_R_HAND : BR_L_HAND];
        if (ua < 0 || fa < 0 || ha < 0) continue;
        Xform palm = xf_mul(inv_root, t.ov_palm[h]);
        Xform hand = proc_hand_for_socket(palm, LIB->hands.palm[h]);
        float side = h ? 1.0f : -1.0f;
        vec3 pole = quat_rotate(quat_conj(root.r), quat_rotate(t.cam_rot, v3(0.8f * side, -1.0f, 0.25f)));
        anim_ik_two_bone_pose(s, posep, model, ua, fa, ha, hand.t, &hand.r, pole, t.ov_w[h]);
        if (t.grip[h] < GRIP_COUNT) proc_curl_fingers(s, posep, model, h, k_grip_curl[t.grip[h]], &LIB->hands, t.ov_w[h]);
    }
    // Neck, head (and hair / eyes / end bones) collapsed: the camera sits inside the head
    // (owner's view only: other players and the shadow pass get the full body).
    for (int b = 0; b < s->count; b++) {
        world[b] = xf_mul(root, model[b]);
        if (CG->head_bone[b] && !raw) world[b].s = v3s(1e-4f);
    }
    // Arms are drawn once, in the viewmodel pass with the item (no wall clipping, own FOV); the
    // world pass gets the body without them.
    g_draw_skin = skin_for(w, c, local_slot);
    MeshId arm = g_draw_skin >= 0 ? CG->skins[g_draw_skin].arm : CG->arm_mesh[c->def_index];
    MeshId noarm = g_draw_skin >= 0 ? CG->skins[g_draw_skin].noarm : CG->noarm_mesh[c->def_index];
    MeshId body = arm && noarm ? noarm : char_lod_mesh(c, 0);
    draw_char_mesh(body, c, world, 0, false);
    draw_char_mesh(arm, c, world, MAT_VIEWMODEL, false);
    hands_set_body_arms(arm != 0);
    g_draw_skin = -1;
    if (cv(CG->fp_debug))
        for (int h = 0; h < 2; h++) {
            if (t.ov_w[h] > 0.001f) dbg_cross(t.ov_palm[h].t, 0.03f, DBG_YELLOW, 0);
            dbg_cross(world[s->role[h ? BR_R_HAND : BR_L_HAND]].t, 0.02f, DBG_RED, 0);
        }
}

static bool g_thirdperson;
void chars_set_thirdperson(bool on) { g_thirdperson = on; }

void chars_set_fp_camera(vec3 pos, quat rot, bool valid) {
    ensure_globals();
    CG->fp_cam = xf_make(pos, rot);
    CG->fp_cam_valid = valid;
}

const char *chars_holdpoint_text(World *w, int local_slot) {
    static char buf[96];
    buf[0] = 0;
    CharacterInst *c = local_fp_char(w, local_slot);
    if (!c) return buf;
    const ProcState *p = &c->rt.proc;
    snprintf(buf, sizeof(buf), "hold %s %.0f%%  pull %.2f  reach %.0f mm", holdpoint_state_name(p->hp_out.state), p->hp_out.blend * 100.0f, p->hp_out.pull,
             p->reach_shift * 1000.0f);
    return buf;
}

float chars_fp_hold_weight(void) { ensure_globals(); return CG->fp_hold_w; }
bool chars_fp_item_cam(Xform *item_cam, int *state, float *blend) {
    ensure_globals();
    if (CG->fp_hold_w < 0.99f) return false;
    // Offset of the rendered item from its hold point target (camera space).
    *item_cam = xf_mul(xf_inverse(CG->fp_item_target), CG->fp_item_cam);
    *state = CG->fp_hp_state;
    *blend = CG->fp_hp_blend;
    return true;
}

vec3 chars_fp_sight_cam(void) { ensure_globals(); return CG->fp_sight_cam; }
float chars_fp_reach_pull(void) { ensure_globals(); return CG->fp_reach_pull; }

void chars_fp_diag(float *glue_pos, float *glue_ang, float *ads_pos, float *ads_ang) {
    ensure_globals();
    for (int h = 0; h < 2; h++) { glue_pos[h] = CG->fp_glue_pos[h]; glue_ang[h] = CG->fp_glue_ang[h]; }
    *ads_pos = CG->fp_ads_pos;
    *ads_ang = CG->fp_ads_ang;
}

void chars_render(World *w, float alpha, vec3 cam_pos, int local_slot) {
    if (!w->chars || !LIB) return;
    ensure_gpu_meshes();
    const Skeleton *s = &LIB->skel;
    static Xform pose[ANIM_MAX_BONES];
    for (int k = 0; k < MAX_CHARACTERS; k++) {
        CharacterInst *c = w->chars->c[k];
        if (!c || !ent_valid(w, c->ent) || !c->have_pose) continue;
        int i = (int)ENT_INDEX(c->ent);
        if (w->flags[i] & EF_HIDDEN) continue;
        if (c->culled && !c->dead) continue;
        // Interpolate the full post-IK pose (hands + item from the same two ticks).
        if (c->rag.active) {
            ragdoll_read_pose(&c->rag, w->phys, &LIB->ragdoll, s, pose);
        } else if (c->dead && c->settled_pose) {
            memcpy(pose, c->settled_pose, sizeof(Xform) * (size_t)s->count);
        } else if (c->dead) {
            memcpy(pose, c->pose_cur, sizeof(Xform) * (size_t)s->count);
        } else {
            float span = (float)DG_MAX(1, c->interval);
            float t = saturate(((float)(w->chars->tick - c->last_eval) + alpha) / span);
            for (int b = 0; b < s->count; b++) pose[b] = xf_lerp(c->pose_prev[b], c->pose_cur[b], t);
        }
        bool local = c->is_player && c->slot == local_slot && !g_thirdperson;
        u32 flags = local ? MAT_SHADOW_ONLY : 0;
        g_draw_skin = skin_for(w, c, local_slot);
        float dist = v3_dist(cam_pos, c->root.t);
        bool shadow = c->pixels > CG->lod.shadow_pixels || local;
        int lod = c->lod < 0 ? 0 : c->lod;
        if (c->lod_fade < 1.0f && c->fade_from != lod) {
            draw_char(w, c, pose, c->fade_from, -(1.0f - c->lod_fade), flags, false);
            draw_char(w, c, pose, lod, c->lod_fade, flags, false);
        } else {
            draw_char(w, c, pose, lod, 0, flags, false);
        }
        // Shadows: one LOD coarser than the main draw.
        if (shadow) draw_char(w, c, pose, DG_MIN(lod + 1, CHAR_LOD_COUNT - 1), 0, MAT_SHADOW_ONLY, true);
        g_draw_skin = -1;
        // Held item (glued to the right hand).
        if (c->item_valid && !c->dead && (c->item_weapon || c->item_def)) {
            float span = (float)DG_MAX(1, c->interval);
            float t = saturate(((float)(w->chars->tick - c->last_eval) + alpha) / span);
            Xform it = xf_lerp(c->item_prev, c->item_cur, t);
            hands_draw_held_att(c->item_weapon, c->item_weapon ? NULL : c->item_def, c->item_att, it.t, it.r, local ? (MAT_SHADOW_ONLY) : 0);
        }
        (void)dist;
    }
}

bool chars_ragdoll_now(World *w, EntityId e, vec3 impulse) {
    CharacterInst *c = char_get(w, e);
    if (!c || !c->have_pose) return false;
    if (c->rag.active) { ragdoll_push(&c->rag, w->phys, impulse); return true; }
    c->dead = true;
    c->tool_rag = true;
    c->death_impulse = impulse;
    c->death_part = BP_CHEST;
    c->death_tick = w->chars->tick;
    c->rag_pending = true;
    return true;
}

bool chars_item_muzzle(World *w, EntityId e, vec3 *pos, vec3 *dir) {
    CharacterInst *c = char_get(w, e);
    if (!c || !c->item_valid || !(c->item_weapon || c->item_def)) return false;
    const ItemSockets *k = held_sockets(c);
    *pos = xf_point(c->item_cur, k->muzzle);
    *dir = quat_rotate(c->item_cur.r, v3(0, 0, -1));
    return true;
}

bool chars_head_world(World *w, EntityId e, float alpha, vec3 *out) {
    CharacterInst *c = char_get(w, e);
    if (!c || !c->have_pose || !LIB) return false;
    const Skeleton *s = &LIB->skel;
    static Xform pose[ANIM_MAX_BONES];
    if (c->rag.active) ragdoll_read_pose(&c->rag, w->phys, &LIB->ragdoll, s, pose);
    else if (c->dead && c->settled_pose) memcpy(pose, c->settled_pose, sizeof(Xform) * (size_t)s->count);
    else {
        float span = (float)DG_MAX(1, c->interval);
        float t = saturate(((float)(w->chars->tick - c->last_eval) + alpha) / span);
        for (int b = 0; b < s->count; b++) pose[b] = xf_lerp(c->pose_prev[b], c->pose_cur[b], t);
    }
    Xform eye;
    if (character_socket_world(c->def, "camera", pose, &eye)) { *out = eye.t; return true; }
    int head = s->role[BR_HEAD];
    if (head < 0) return false;
    *out = pose[head].t;
    return true;
}

bool chars_ragdoll_release(World *w, EntityId e) {
    CharacterInst *c = char_get(w, e);
    if (!c || !c->tool_rag) return false;
    revive(w, c);
    return true;
}

// ------------------------------------------------------------------ zombies (host test behaviour)

EntityId spawn_zombie(World *w, vec3 feet, float yaw, u8 character_id) {
    EntityId e = ent_spawn(w, ET_ZOMBIE);
    if (!e) return e;
    int i = (int)ENT_INDEX(e);
    const CharacterDef *def = LIB ? anim_lib_char_by_net(LIB, character_id) : NULL;
    w->char_id[i] = character_id;
    w->comps[i] |= C_HEALTH | C_HITBOX;
    health_init(&w->health[i], def ? def->hp : 120.0f, 0);
    w->health[i].regen_delay = 0;
    ent_set_pos(w, e, feet, quat_yaw(yaw), true);
    entity_setup(w, e);
    CharacterInst *c = char_get(w, e);
    if (c) {
        c->ai.home = feet;
        c->ai.target = feet;
        c->ai.wander_radius = 4.0f;
        c->ai.yaw = yaw;
        c->body_yaw = yaw;
        c->ai.timer = 0.5f + (float)(dg_hash_u32(e) % 100) * 0.03f;
    }
    return e;
}

void zombies_attack(World *w) {
    if (!w->chars || !w->authority) return;
    for (int k = 0; k < MAX_CHARACTERS; k++) {
        CharacterInst *c = w->chars->c[k];
        if (!c || !ent_valid(w, c->ent) || w->type[ENT_INDEX(c->ent)] != ET_ZOMBIE || c->dead) continue;
        c->ai.attack_t = 1.2f;
        c->ai.mode = ZM_ATTACK;
        ctrl_set(&c->rt.ctrl, AP_SEED, (float)(dg_hash_u32(c->ent ^ w->tick) % 3));
        charrt_trigger(&c->rt, AP_T_MELEE);
        AnimEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = AEV_MELEE;
        ev.ent = c->ent;
        ev.tick = w->tick;
        chars_push_event(w, &ev);
    }
}

static void zombie_ai(World *w, CharacterInst *c, int i, float dt) {
    ZombieAI *ai = &c->ai;
    if ((w->flags[i] & EF_DEAD) || c->dead) { ai->speed = 0; return; }
    float speed_mult = (CG->zombie_speed ? CG->zombie_speed->f : 1.0f) * ((c->limb & LIMB_LEGS) ? 0.45f : 1.0f);
    if (ai->attack_t > 0) {
        ai->attack_t -= dt;
        ai->speed = 0;
        if (ai->attack_t <= 0) ai->mode = ZM_IDLE;
        return;
    }
    ai->timer -= dt;
    vec3 pos = w->xf[i].pos;
    if (ai->timer <= 0) {
        u32 h = dg_hash_u32(c->ent * 2654435761u ^ w->tick);
        if (ai->mode == ZM_WANDER || (h & 3) == 0) {
            ai->mode = ZM_IDLE;
            ai->timer = 1.0f + (float)(h % 300) / 100.0f;
        } else {
            ai->mode = ZM_WANDER;
            float a = (float)(h % 6283) / 1000.0f, r = ai->wander_radius * (float)((h >> 12) % 100) / 100.0f;
            ai->target = v3_add(ai->home, v3(sinf(a) * r, 0, cosf(a) * r));
            ai->timer = 6.0f;
        }
    }
    float want = 0;
    if (ai->mode == ZM_WANDER) {
        vec3 to = v3_flat(v3_sub(ai->target, pos));
        float d = v3_len(to);
        if (d < 0.3f) { ai->mode = ZM_IDLE; ai->timer = 1.5f; }
        else {
            float target_yaw = atan2f(-to.x, -to.z);
            float diff = wrap_angle(target_yaw - ai->yaw);
            ai->yaw = wrap_angle(ai->yaw + clampf(diff, -2.5f * dt, 2.5f * dt));
            want = (c->def ? c->def->move_speed : 1.2f) * speed_mult * (fabsf(diff) < 1.0f ? 1.0f : 0.3f);
        }
    }
    ai->speed = approachf(ai->speed, want, 3.0f * dt);
    // Kinematic move with a wall probe and ground snap.
    vec3 step = v3_scale(forward_from_yaw(ai->yaw), ai->speed * dt);
    RayHit rh;
    vec3 chest = v3_add(pos, v3(0, 1.0f, 0));
    if (ai->speed > 0 && phys_raycast(w->phys, chest, v3_scale(v3_norm(step), 0.5f), PL_MASK_WORLD, PHYS_INVALID_BODY, &rh)) {
        ai->yaw = wrap_angle(ai->yaw + 2.0f);
        ai->speed = 0;
        step = v3(0, 0, 0);
        ai->mode = ZM_IDLE;
        ai->timer = 0.5f;
    }
    vec3 np = v3_add(pos, step);
    if (phys_raycast(w->phys, v3_add(np, v3(0, 0.6f, 0)), v3(0, -1.6f, 0), PL_MASK(PL_STATIC), PHYS_INVALID_BODY, &rh)) np.y = rh.point.y;
    w->xf[i].pos = np;
    w->xf[i].rot = quat_yaw(ai->yaw);
    c->body_yaw = ai->yaw;
    if (w->body[i].body != PHYS_INVALID_BODY) phys_move_kinematic(w->phys, w->body[i].body, v3_add(np, v3(0, 0.9f, 0)), quat_identity(), dt);
}

// ------------------------------------------------------------------ debug

void chars_debug_draw(World *w, vec3 cam_pos) {
    if (!w->chars || !LIB || !CG) return;
    const Skeleton *s = &LIB->skel;
    for (int k = 0; k < MAX_CHARACTERS; k++) {
        CharacterInst *c = w->chars->c[k];
        if (!c || !ent_valid(w, c->ent) || !c->have_pose) continue;
        bool near = v3_dist(cam_pos, c->root.t) < 25.0f;
        if (CG->debug && CG->debug->i && (CG->debug->i >= 2 || near)) {
            for (int b = 1; b < s->count; b++) {
                int p = s->parent[b];
                if (p >= 0) dbg_line(c->pose_cur[p].t, c->pose_cur[b].t, DBG_CYAN, 0, false);
            }
        }
        if (cv(CG->hitbox_debug) && (!c->local || (CG->debug && CG->debug->i >= 2))) {
            static const u32 cols[6] = { DBG_RED, DBG_ORANGE, DBG_YELLOW, DBG_GREEN, DBG_CYAN, DBG_MAGENTA };
            dbg_set_shape_depth(false);
            for (int h = 0; h < c->hit.count; h++) {
                const HitCapsule *hc = &c->hit.c[h];
                dbg_capsule_axis(hc->a, hc->b, hc->r, cols[hc->part % 6], 0);
                if (near) {
                    char t[40];
                    snprintf(t, sizeof(t), "%s x%.2f", body_part_name(hc->part), c->def->part_mult[hc->part]);
                    dbg_text(v3_scale(v3_add(hc->a, hc->b), 0.5f), t, DBG_WHITE, 0.025f, 0);
                }
            }
            dbg_set_shape_depth(true);
        }
        if (cv(CG->hold_debug) && near && c->item_valid && (c->item_weapon || c->item_def)) {
            const ItemSockets *k2 = held_sockets(c);
            Xform it = c->item_cur;
            Xform gr = xf_mul(it, k2->grip_r), gl = xf_mul(it, k2->grip_l);
            dbg_cross(gr.t, 0.03f, DBG_GREEN, 0);
            if (k2->has_left) dbg_cross(gl.t, 0.03f, DBG_YELLOW, 0);
            dbg_line(gl.t, v3_add(gl.t, quat_rotate(gl.r, v3(0, 0, -0.06f))), DBG_YELLOW, 0, false);
            dbg_line(gl.t, v3_add(gl.t, quat_rotate(gl.r, v3(0, 0.04f, 0))), DBG_BLUE, 0, false);
            dbg_cross(c->pose_cur[s->role[BR_L_HAND]].t, 0.02f, DBG_RED, 0);
            dbg_cross(c->pose_cur[s->role[BR_R_HAND]].t, 0.02f, DBG_RED, 0);
            dbg_cross(xf_point(it, k2->muzzle), 0.02f, DBG_ORANGE, 0);
        }
        if (cv(CG->lod_debug) && v3_dist(cam_pos, c->root.t) < 140.0f) {
            char t[96];
            snprintf(t, sizeof(t), "LOD%d %.0fpx 1/%d %s%s%s", c->lod, c->pixels, c->interval, c->lod <= 0 ? "foot spine turn " : "",
                     c->lod <= 1 ? "hold dmg" : "baked-arms", c->culled ? " CULLED" : "");
            dbg_text(v3_add(c->root.t, v3(0, LIB->skel.height * c->def->scale + 0.6f, 0)), t, DBG_YELLOW, 0.06f, 0);   // above the HUD health bar
        }
        if (cv(CG->ragdoll_debug) && c->rag.active) ragdoll_debug_draw(&c->rag, w->phys, &LIB->ragdoll);
    }
}
