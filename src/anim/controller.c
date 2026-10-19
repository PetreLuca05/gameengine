#include "controller.h"
#include "core/log.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define SIM_RATE_FOR_INERT 60   // inertialization clock: sim ticks per second

static const char *k_std_params[AP_STD_COUNT] = {
    "speed", "move_x", "move_y", "is_grounded", "is_crouching", "is_prone", "is_sprinting", "is_ads", "is_mantling", "mantle_t",
    "is_hanging", "is_vaulting", "vertical_velocity", "turn_angle", "aim_pitch", "aim_yaw", "weapon_type", "weapon_id",
    "health", "limb_status", "is_dead", "is_attacking",
    "jump", "land", "hit", "fire", "reload", "melee", "die", "equip",
    "hit_part", "hit_dir", "hit_strength", "seed",
};
static const u8 k_std_types[AP_STD_COUNT] = {
    PT_FLOAT, PT_FLOAT, PT_FLOAT, PT_BOOL, PT_BOOL, PT_BOOL, PT_BOOL, PT_BOOL, PT_BOOL, PT_FLOAT,
    PT_BOOL, PT_BOOL, PT_FLOAT, PT_FLOAT, PT_FLOAT, PT_FLOAT, PT_INT, PT_INT,
    PT_FLOAT, PT_INT, PT_BOOL, PT_BOOL,
    PT_TRIGGER, PT_TRIGGER, PT_TRIGGER, PT_TRIGGER, PT_TRIGGER, PT_TRIGGER, PT_TRIGGER, PT_TRIGGER,
    PT_INT, PT_INT, PT_FLOAT, PT_INT,
};

const char *ctrl_param_name(int p) { return (p >= 0 && p < AP_STD_COUNT) ? k_std_params[p] : "?"; }

int ctrl_param_index(const ControllerDef *def, const char *name) {
    for (int i = 0; i < def->param_count; i++)
        if (!strcmp(def->params[i].name, name)) return i;
    return -1;
}

int ctrl_slot_index(const ControllerDef *def, const char *name) {
    for (int i = 0; i < def->slot_count; i++)
        if (!strcmp(def->slot_names[i], name)) return i;
    return -1;
}

int ctrl_state_index(const ControllerDef *def, int layer, const char *name) {
    if (layer < 0 || layer >= def->layer_count) return -1;
    const LayerDef *l = &def->layers[layer];
    for (int i = 0; i < l->state_count; i++)
        if (!strcmp(l->states[i].name, name)) return i;
    return -1;
}

const char *ctrl_state_name(const ControllerInst *c, int layer, int state) {
    if (!c->def || layer < 0 || layer >= c->def->layer_count) return "?";
    const LayerDef *l = &c->def->layers[layer];
    return (state >= 0 && state < l->state_count) ? l->states[state].name : "-";
}

// ------------------------------------------------------------------ loading

typedef struct Parse { ControllerDef *d; const Skeleton *skel; char *err; int err_size; bool ok; } Parse;

static void perr(Parse *p, const char *fmt, const char *a) {
    if (p->ok && p->err) snprintf(p->err, (size_t)p->err_size, fmt, a);
    p->ok = false;
}

static const char *js(const cJSON *o, const char *k, const char *def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : def;
}
static float jf(const cJSON *o, const char *k, float def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}
static bool jb(const cJSON *o, const char *k, bool def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : def;
}

static int add_slot(ControllerDef *d, const char *name) {
    int i = ctrl_slot_index(d, name);
    if (i >= 0) return i;
    if (d->slot_count >= AC_MAX_SLOTS) return -1;
    dg_strcpy(d->slot_names[d->slot_count], 32, name);
    return d->slot_count++;
}

static int param_ref(Parse *p, const char *name) {
    int i = ctrl_param_index(p->d, name);
    if (i < 0) perr(p, "controller: unknown parameter '%s'", name);
    return i;
}

static int parse_node(Parse *p, const cJSON *o);

static int new_node(Parse *p) {
    if (p->d->node_count >= AC_MAX_NODES) { perr(p, "controller: too many blend nodes%s", ""); return -1; }
    BlendNode *n = &p->d->nodes[p->d->node_count];
    memset(n, 0, sizeof(*n));
    n->slot = -1;
    n->param_x = n->param_y = -1;
    n->speed = 1.0f;
    return p->d->node_count++;
}

static int parse_node(Parse *p, const cJSON *o) {
    if (cJSON_IsString(o)) {
        int id = new_node(p);
        if (id < 0) return -1;
        BlendNode *n = &p->d->nodes[id];
        n->kind = NK_CLIP;
        if (strchr(o->valuestring, '{')) dg_strcpy(n->slot_pattern, sizeof(n->slot_pattern), o->valuestring);
        else n->slot = (i16)add_slot(p->d, o->valuestring);
        return id;
    }
    if (!cJSON_IsObject(o)) { perr(p, "controller: bad blend node%s", ""); return -1; }
    const cJSON *tree = cJSON_GetObjectItemCaseSensitive(o, "tree");
    if (tree) return parse_node(p, tree);
    const char *clip = js(o, "clip", NULL);
    if (clip) {
        int id = new_node(p);
        if (id < 0) return -1;
        BlendNode *n = &p->d->nodes[id];
        n->kind = NK_CLIP;
        n->speed = jf(o, "speed", 1.0f);
        if (strchr(clip, '{')) dg_strcpy(n->slot_pattern, sizeof(n->slot_pattern), clip);
        else n->slot = (i16)add_slot(p->d, clip);
        return id;
    }
    const cJSON *b1 = cJSON_GetObjectItemCaseSensitive(o, "blend1d");
    const cJSON *b2 = cJSON_GetObjectItemCaseSensitive(o, "blend2d");
    if (!b1 && !b2) { perr(p, "controller: node needs clip / blend1d / blend2d%s", ""); return -1; }
    int id = new_node(p);
    if (id < 0) return -1;
    BlendNode *n = &p->d->nodes[id];
    n->speed = jf(o, "speed", 1.0f);
    if (b1) {
        n->kind = NK_BLEND1D;
        n->param_x = (i16)param_ref(p, cJSON_IsString(b1) ? b1->valuestring : "");
    } else {
        const char *mode = js(o, "mode", "directional");
        n->kind = !strcmp(mode, "cartesian") ? NK_BLEND2D_CART : NK_BLEND2D_DIR;
        if (!cJSON_IsArray(b2) || cJSON_GetArraySize(b2) != 2) { perr(p, "controller: blend2d needs [x, y]%s", ""); return id; }
        n->param_x = (i16)param_ref(p, cJSON_GetArrayItem(b2, 0)->valuestring);
        n->param_y = (i16)param_ref(p, cJSON_GetArrayItem(b2, 1)->valuestring);
    }
    const cJSON *ch;
    cJSON_ArrayForEach(ch, cJSON_GetObjectItemCaseSensitive(o, "children")) {
        if (n->child_count >= AC_MAX_CHILDREN) break;
        int k = n->child_count;
        // Note: nodes array may not move (fixed array), n stays valid.
        int cid = parse_node(p, ch);
        if (cid < 0) return id;
        n = &p->d->nodes[id];
        n->child[k] = (i16)cid;
        n->threshold[k] = jf(ch, "at", 0);
        const cJSON *pos = cJSON_GetObjectItemCaseSensitive(ch, "pos");
        if (cJSON_IsArray(pos) && cJSON_GetArraySize(pos) == 2)
            n->pos[k] = v2((float)cJSON_GetArrayItem(pos, 0)->valuedouble, (float)cJSON_GetArrayItem(pos, 1)->valuedouble);
        n->child_count++;
    }
    // 1D children sorted by threshold.
    if (n->kind == NK_BLEND1D)
        for (int a = 0; a < n->child_count; a++)
            for (int b = a + 1; b < n->child_count; b++)
                if (n->threshold[b] < n->threshold[a]) {
                    float t = n->threshold[a]; n->threshold[a] = n->threshold[b]; n->threshold[b] = t;
                    i16 c = n->child[a]; n->child[a] = n->child[b]; n->child[b] = c;
                }
    return id;
}

static u8 parse_op(const char *s) {
    static const char *ops[] = { ">", "<", ">=", "<=", "==", "!=", "true", "false", "trigger" };
    for (int i = 0; i < 9; i++) if (!strcmp(s, ops[i])) return (u8)i;
    return CO_TRUE;
}

static void parse_mask(Parse *p, const char *name, const cJSON *o) {
    ControllerDef *d = p->d;
    if (d->mask_count >= AC_MAX_MASKS) return;
    int m = d->mask_count++;
    dg_strcpy(d->mask_names[m], 24, name);
    mask_fill(&d->masks[m], 0);
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(o, "include")) {
        if (!cJSON_IsArray(e) || cJSON_GetArraySize(e) < 1) continue;
        const char *bone = cJSON_GetArrayItem(e, 0)->valuestring;
        float w = cJSON_GetArraySize(e) > 1 ? (float)cJSON_GetArrayItem(e, 1)->valuedouble : 1.0f;
        bool chain = cJSON_GetArraySize(e) > 2 ? cJSON_IsTrue(cJSON_GetArrayItem(e, 2)) : true;
        int b = skel_find_suffix(p->skel, bone);
        if (b < 0) { log_warn("controller: mask '%s' bone '%s' not in skeleton", name, bone); continue; }
        if (chain) mask_set_chain(p->skel, &d->masks[m], b, w);
        else d->masks[m].w[b] = w;
    }
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(o, "exclude")) {
        if (!cJSON_IsString(e)) continue;
        int b = skel_find_suffix(p->skel, e->valuestring);
        if (b >= 0) mask_set_chain(p->skel, &d->masks[m], b, 0);
    }
}

static int find_mask(const ControllerDef *d, const char *name) {
    for (int i = 0; i < d->mask_count; i++) if (!strcmp(d->mask_names[i], name)) return i;
    return -1;
}

bool ctrl_def_load(ControllerDef *def, const char *text, const Skeleton *skel, char *err, int err_size) {
    memset(def, 0, sizeof(*def));
    Parse p = { def, skel, err, err_size, true };
    for (int i = 0; i < AP_STD_COUNT; i++) {
        dg_strcpy(def->params[i].name, 24, k_std_params[i]);
        def->params[i].type = k_std_types[i];
    }
    def->param_count = AP_STD_COUNT;
    def->params[AP_GROUNDED].def = 1;
    def->params[AP_HEALTH].def = 100;
    cJSON *root = cJSON_Parse(text);
    if (!root) { perr(&p, "controller: JSON parse error near %.40s", cJSON_GetErrorPtr() ? cJSON_GetErrorPtr() : "?"); return false; }
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(root, "params")) {
        const char *name = js(e, "name", NULL);
        if (!name) continue;
        int i = ctrl_param_index(def, name);
        if (i < 0 && def->param_count < AC_MAX_PARAMS) {
            i = def->param_count++;
            dg_strcpy(def->params[i].name, 24, name);
            const char *t = js(e, "type", "float");
            def->params[i].type = !strcmp(t, "bool") ? PT_BOOL : !strcmp(t, "int") ? PT_INT : !strcmp(t, "trigger") ? PT_TRIGGER : PT_FLOAT;
        }
        if (i >= 0) def->params[i].def = jf(e, "default", def->params[i].def);
    }
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(root, "weapon_types"))
        if (cJSON_IsString(e) && def->weapon_type_count < 16) dg_strcpy(def->weapon_types[def->weapon_type_count++], 16, e->valuestring);
    const cJSON *masks = cJSON_GetObjectItemCaseSensitive(root, "masks");
    cJSON_ArrayForEach(e, masks) parse_mask(&p, e->string, e);
    const cJSON *lj;
    cJSON_ArrayForEach(lj, cJSON_GetObjectItemCaseSensitive(root, "layers")) {
        if (def->layer_count >= AC_MAX_LAYERS) break;
        LayerDef *L = &def->layers[def->layer_count++];
        dg_strcpy(L->name, 24, js(lj, "name", "layer"));
        const char *mask = js(lj, "mask", NULL);
        L->mask = (i16)(mask ? find_mask(def, mask) : -1);
        if (mask && L->mask < 0) perr(&p, "controller: unknown mask '%s'", mask);
        L->blend = !strcmp(js(lj, "blend", "override"), "additive") ? LB_ADDITIVE : LB_OVERRIDE;
        L->weight = jf(lj, "weight", 1.0f);
        const char *wp = js(lj, "weight_param", NULL);
        L->weight_param = (i16)(wp ? param_ref(&p, wp) : -1);
        const cJSON *sj;
        cJSON_ArrayForEach(sj, cJSON_GetObjectItemCaseSensitive(lj, "states")) {
            if (L->state_count >= AC_MAX_STATES) break;
            StateDef *S = &L->states[L->state_count++];
            memset(S, 0, sizeof(*S));
            dg_strcpy(S->name, 24, js(sj, "name", "state"));
            S->time_param = S->speed_param = -1;
            S->speed = jf(sj, "speed", 1.0f);
            S->loop = jb(sj, "loop", true);
            S->hand_ik = jb(sj, "hand_ik", true);
            S->sync = jb(sj, "sync", false);
            S->speed_match = jb(sj, "speed_match", false);
            S->clip_root_y = !strcmp(js(sj, "root_y", "movement"), "clip");
            const char *tp = js(sj, "time_param", NULL);
            if (tp) S->time_param = (i16)param_ref(&p, tp);
            const char *sp = js(sj, "speed_param", NULL);
            if (sp) S->speed_param = (i16)param_ref(&p, sp);
            if (jb(sj, "empty", false)) S->node = -1;
            else S->node = (i16)parse_node(&p, sj);
            const cJSON *vj;
            cJSON_ArrayForEach(vj, cJSON_GetObjectItemCaseSensitive(sj, "variants"))
                if (S->variant_count < AC_MAX_VARIANTS) S->variants[S->variant_count++] = (i16)parse_node(&p, vj);
        }
        const char *dflt = js(lj, "default", NULL);
        L->default_state = 0;
        for (int i = 0; dflt && i < L->state_count; i++) if (!strcmp(L->states[i].name, dflt)) L->default_state = i;
        const cJSON *tj;
        cJSON_ArrayForEach(tj, cJSON_GetObjectItemCaseSensitive(lj, "transitions")) {
            if (L->trans_count >= AC_MAX_TRANS) break;
            TransitionDef *T = &L->trans[L->trans_count++];
            memset(T, 0, sizeof(*T));
            const char *from = js(tj, "from", "*"), *to = js(tj, "to", "");
            T->from = -1;
            if (strcmp(from, "*")) {
                T->from = -2;
                for (int i = 0; i < L->state_count; i++) if (!strcmp(L->states[i].name, from)) T->from = (i16)i;
                if (T->from == -2) perr(&p, "controller: unknown transition source '%s'", from);
            }
            T->to = -1;
            for (int i = 0; i < L->state_count; i++) if (!strcmp(L->states[i].name, to)) T->to = (i16)i;
            if (T->to < 0) perr(&p, "controller: unknown transition target '%s'", to);
            T->duration = jf(tj, "duration", 0.2f);
            T->exit_time = jf(tj, "exit_time", -1.0f);
            T->inertialize = jb(tj, "inertialize", false);
            T->interruptible = jb(tj, "interruptible", true);
            T->allow_self = jb(tj, "allow_self", false);
            const cJSON *cj;
            cJSON_ArrayForEach(cj, cJSON_GetObjectItemCaseSensitive(tj, "when")) {
                if (T->cond_count >= AC_MAX_COND || !cJSON_IsArray(cj) || cJSON_GetArraySize(cj) < 2) continue;
                Condition *C = &T->cond[T->cond_count++];
                C->param = (i16)param_ref(&p, cJSON_GetArrayItem(cj, 0)->valuestring);
                C->op = parse_op(cJSON_GetArrayItem(cj, 1)->valuestring);
                C->value = cJSON_GetArraySize(cj) > 2 ? (float)cJSON_GetArrayItem(cj, 2)->valuedouble : 0;
            }
        }
    }
    cJSON_Delete(root);
    def->hash = anim_hash_str(text, ANIM_HASH_SEED);
    return p.ok;
}

// ------------------------------------------------------------------ binding

static int find_clip(const ClipLookup *cl, const char *name) {
    for (int i = 0; i < cl->count; i++)
        if (!strcmp(cl->clips[i].name, name)) return i;
    return -1;
}

// "clip", "clip@mirror", "clip@reverse", "clip@0.2:0.5" (normalized sub-range), combinable.
static SlotBinding parse_binding(const ClipLookup *cl, const char *spec) {
    SlotBinding b = { -1, false, false, 0.0f, 1.0f };
    char name[96];
    dg_strcpy(name, sizeof(name), spec);
    char *at = strchr(name, '@');
    if (at) {
        *at = 0;
        char *tok = at + 1;
        while (tok && *tok) {
            char *next = strchr(tok, '@');
            if (next) *next++ = 0;
            if (!strcmp(tok, "mirror")) b.mirror = true;
            else if (!strcmp(tok, "reverse")) b.reverse = true;
            else if (strchr(tok, ':')) {
                char *colon = strchr(tok, ':');
                float a = strtof(tok, NULL), c = strtof(colon + 1, NULL);
                if (c > a) { b.r0 = saturate(a); b.r1 = saturate(c); }
            }
            tok = next;
        }
    }
    if (name[0]) b.clip = (i16)find_clip(cl, name);
    return b;
}

bool ctrl_bind(AnimBinding *b, const ControllerDef *def, const char *set_json, const ClipLookup *cl, char *err, int err_size) {
    memset(b, 0, sizeof(*b));
    for (int i = 0; i < AC_MAX_SLOTS; i++) b->slots[i].clip = -1;
    cJSON *root = cJSON_Parse(set_json);
    if (!root) { if (err) snprintf(err, (size_t)err_size, "anim set: JSON parse error"); return false; }
    dg_strcpy(b->name, sizeof(b->name), js(root, "name", "set"));
    const cJSON *slots = cJSON_GetObjectItemCaseSensitive(root, "slots");
    const cJSON *e;
    // Slots the controller doesn't reference yet are still bindable through patterns.
    cJSON_ArrayForEach(e, slots) {
        if (!cJSON_IsString(e) || !e->valuestring[0]) continue;
        int s = ctrl_slot_index(def, e->string);
        SlotBinding sb = parse_binding(cl, e->valuestring);
        if (sb.clip < 0) log_warn("anim set '%s': slot '%s' -> clip '%s' not found", b->name, e->string, e->valuestring);
        if (s >= 0) b->slots[s] = sb;
    }
    b->fallback_clip = -1;
    int idle = ctrl_slot_index(def, "idle");
    if (idle >= 0) b->fallback_clip = b->slots[idle].clip;
    if (b->fallback_clip < 0 && cl->count) b->fallback_clip = 0;
    // Pattern nodes: "{weapon}" substituted by each weapon type name.
    for (int n = 0; n < def->node_count; n++) {
        for (int w = 0; w < 16; w++) b->pattern_slot[n][w] = -1;
        const BlendNode *nd = &def->nodes[n];
        if (nd->kind != NK_CLIP || !nd->slot_pattern[0]) continue;
        for (int w = 0; w < def->weapon_type_count; w++) {
            char name[64];
            const char *br = strstr(nd->slot_pattern, "{weapon}");
            if (!br) continue;
            snprintf(name, sizeof(name), "%.*s%s%s", (int)(br - nd->slot_pattern), nd->slot_pattern, def->weapon_types[w], br + 8);
            const cJSON *v = cJSON_GetObjectItemCaseSensitive(slots, name);
            if (!cJSON_IsString(v) || !v->valuestring[0]) {
                // Unbound for this weapon type: the plain slot ("run_f_{weapon}" -> "run_f"), so a
                // set only lists the weapon variants it has (zombies, unarmed keep their clips).
                char base[64];
                const char *cut = br > nd->slot_pattern && br[-1] == '_' ? br - 1 : br;
                snprintf(base, sizeof(base), "%.*s%s", (int)(cut - nd->slot_pattern), nd->slot_pattern, br + 8);
                v = cJSON_GetObjectItemCaseSensitive(slots, base);
            }
            if (cJSON_IsString(v) && v->valuestring[0]) {
                SlotBinding sb = parse_binding(cl, v->valuestring);
                // Encoded as -(clip + 2) so it never collides with a slot index.
                if (sb.clip >= 0) b->pattern_slot[n][w] = (i16)(-(sb.clip + 2));
            }
        }
    }
    // One line per set: unbound slots fall back (idle on the base layer, procedural hold above).
    char list[640];
    int missing = 0, len = 0;
    list[0] = 0;
    for (int s = 0; s < def->slot_count; s++)
        if (b->slots[s].clip < 0) {
            if (len < (int)sizeof(list) - 40) len += snprintf(list + len, sizeof(list) - (size_t)len, "%s%s", missing ? ", " : "", def->slot_names[s]);
            missing++;
        }
    if (missing) log_info("anim set '%s': %d of %d slots unbound (fallbacks): %s", b->name, missing, def->slot_count, list);
    u32 h = anim_hash_str(set_json, ANIM_HASH_SEED);
    for (int i = 0; i < cl->count; i++) h = anim_hash(&cl->clips[i].hash, sizeof(u32), h);
    b->hash = h;
    cJSON_Delete(root);
    return true;
}

// ------------------------------------------------------------------ blend weights

typedef struct Leaf { i16 clip; bool mirror, reverse; float weight; float speed; float r0, r1; } Leaf;
typedef struct LeafSet { Leaf l[24]; int n; } LeafSet;

static float pv(const ControllerInst *c, int p) { return (p >= 0) ? c->params[p] : 0.0f; }

// Gradient band weights (cartesian or polar/directional), Johansen 2009.
static void blend2d_weights(const BlendNode *n, vec2 pt, bool polar, float *w) {
    int k = n->child_count;
    float sum = 0;
    for (int i = 0; i < k; i++) {
        float wi = 1.0f;
        vec2 pi = n->pos[i];
        for (int j = 0; j < k && wi > 0; j++) {
            if (j == i) continue;
            vec2 pj = n->pos[j];
            vec2 vij, vip;
            if (polar) {
                float mi = v2_len(pi), mj = v2_len(pj), mp = v2_len(pt);
                float avg = (mi + mj) * 0.5f;
                float ai = mi > 1e-4f ? atan2f(pi.x, pi.y) : (mj > 1e-4f ? atan2f(pj.x, pj.y) : 0);
                float aj = mj > 1e-4f ? atan2f(pj.x, pj.y) : ai;
                float ap = mp > 1e-4f ? atan2f(pt.x, pt.y) : ai;
                float dij = wrap_angle(aj - ai), dip = wrap_angle(ap - ai);
                const float kdir = 2.0f;
                if (avg < 1e-4f) avg = 1e-4f;
                vij = v2((mj - mi) / avg, dij * kdir);
                vip = v2((mp - mi) / avg, dip * kdir);
            } else {
                vij = v2_sub(pj, pi);
                vip = v2_sub(pt, pi);
            }
            float l2 = vij.x * vij.x + vij.y * vij.y;
            if (l2 < 1e-8f) continue;
            float h = 1.0f - (vip.x * vij.x + vip.y * vij.y) / l2;
            if (h < wi) wi = h;
        }
        w[i] = fmaxf(0.0f, wi);
        sum += w[i];
    }
    if (sum <= 1e-6f) {
        // Outside the hull: nearest child.
        int best = 0;
        float bd = 1e9f;
        for (int i = 0; i < k; i++) {
            float d = v2_len(v2_sub(pt, n->pos[i]));
            if (d < bd) { bd = d; best = i; }
        }
        for (int i = 0; i < k; i++) w[i] = i == best ? 1.0f : 0.0f;
        return;
    }
    for (int i = 0; i < k; i++) w[i] /= sum;
}

// A node is bound if it (or any child) resolves to a clip in the anim set.
static bool node_bound(const ControllerInst *c, int node, int depth) {
    if (node < 0 || depth > 8) return false;
    const BlendNode *n = &c->def->nodes[node];
    if (n->kind == NK_CLIP) {
        if (n->slot_pattern[0]) {
            int wt = DG_CLAMP((int)c->params[AP_WEAPON_TYPE], 0, 15);
            return c->bind->pattern_slot[node][wt] <= -2;
        }
        return n->slot >= 0 && c->bind->slots[n->slot].clip >= 0;
    }
    for (int i = 0; i < n->child_count; i++) if (node_bound(c, n->child[i], depth + 1)) return true;
    return false;
}

static void collect(const ControllerInst *c, int node, float weight, float speed, LeafSet *out);

// Blend weights over the bound children only (unbound ones are left out of the blend space, so
// they never take weight and force a renormalization pop near their position).
static void blend_weights_bound(const ControllerInst *c, const BlendNode *n, float *w) {
    int k = n->child_count, idx[AC_MAX_CHILDREN], m = 0;
    for (int i = 0; i < k; i++) { w[i] = 0; if (node_bound(c, n->child[i], 0)) idx[m++] = i; }
    if (m == 0) for (int i = 0; i < k; i++) idx[m++] = i;
    BlendNode t = *n;
    t.child_count = m;
    for (int j = 0; j < m; j++) { t.child[j] = n->child[idx[j]]; t.threshold[j] = n->threshold[idx[j]]; t.pos[j] = n->pos[idx[j]]; }
    float tw[AC_MAX_CHILDREN] = { 0 };
    if (n->kind == NK_BLEND1D) {
        float v = pv(c, n->param_x);
        if (m == 1 || v <= t.threshold[0]) tw[0] = 1;
        else if (v >= t.threshold[m - 1]) tw[m - 1] = 1;
        else {
            for (int i = 0; i < m - 1; i++)
                if (v >= t.threshold[i] && v <= t.threshold[i + 1]) {
                    float span = t.threshold[i + 1] - t.threshold[i];
                    float u = span > 1e-6f ? (v - t.threshold[i]) / span : 0;
                    tw[i] = 1 - u;
                    tw[i + 1] = u;
                    break;
                }
        }
    } else {
        blend2d_weights(&t, v2(pv(c, n->param_x), pv(c, n->param_y)), n->kind == NK_BLEND2D_DIR, tw);
    }
    for (int j = 0; j < m; j++) w[idx[j]] = tw[j];
}

static void collect(const ControllerInst *c, int node, float weight, float speed, LeafSet *out) {
    if (node < 0 || weight <= 0.0f) return;
    const BlendNode *n = &c->def->nodes[node];
    speed *= n->speed;
    if (n->kind == NK_CLIP) {
        if (out->n >= 24) return;
        Leaf *l = &out->l[out->n];
        l->clip = -1;
        l->mirror = l->reverse = false;
        l->r0 = 0.0f;
        l->r1 = 1.0f;
        if (n->slot_pattern[0]) {
            int wt = DG_CLAMP((int)c->params[AP_WEAPON_TYPE], 0, 15);
            i16 enc = c->bind->pattern_slot[node][wt];
            if (enc <= -2) l->clip = (i16)(-enc - 2);
        } else if (n->slot >= 0) {
            const SlotBinding *sb = &c->bind->slots[n->slot];
            l->clip = sb->clip;
            l->mirror = sb->mirror;
            l->reverse = sb->reverse;
            l->r0 = sb->r0;
            l->r1 = sb->r1;
        }
        l->weight = weight;
        l->speed = speed;
        out->n++;
        return;
    }
    float w[AC_MAX_CHILDREN];
    int k = n->child_count;
    blend_weights_bound(c, n, w);
    for (int i = 0; i < k; i++) collect(c, n->child[i], weight * w[i], speed, out);
}

// Leaves with unbound clips are dropped; if nothing is left, the fallback (idle) plays on
// the base layer, and upper layers contribute nothing (n = 0).
static void resolve_leaves_ex(const ControllerInst *c, int node, LeafSet *ls, bool fallback) {
    ls->n = 0;
    collect(c, node, 1.0f, 1.0f, ls);
    float sum = 0;
    int m = 0;
    for (int i = 0; i < ls->n; i++)
        if (ls->l[i].clip >= 0 && ls->l[i].weight > 1e-4f) { ls->l[m++] = ls->l[i]; sum += ls->l[i].weight; }
    ls->n = m;
    if (m == 0 || sum <= 0) {
        if (!fallback) { ls->n = 0; return; }
        ls->n = 1;
        ls->l[0].clip = c->bind->fallback_clip;
        ls->l[0].weight = 1;
        ls->l[0].speed = 1;
        ls->l[0].mirror = ls->l[0].reverse = false;
        ls->l[0].r0 = 0.0f;
        ls->l[0].r1 = 1.0f;
        return;
    }
    for (int i = 0; i < m; i++) ls->l[i].weight /= sum;
}

static void resolve_leaves(const ControllerInst *c, int node, LeafSet *ls) { resolve_leaves_ex(c, node, ls, true); }

static float leaves_duration(const LeafSet *ls, const ClipLookup *cl) {
    float d = 0;
    for (int i = 0; i < ls->n; i++) {
        if (ls->l[i].clip < 0 || ls->l[i].clip >= cl->count) continue;
        const Clip *cp = &cl->clips[ls->l[i].clip];
        float span = cp->frames > 1 ? (float)(cp->frames - 1) / cp->rate : 0.0f;
        span *= ls->l[i].r1 - ls->l[i].r0;
        float s = ls->l[i].speed * (cp->speed > 0 ? cp->speed : 1.0f);
        d += ls->l[i].weight * (s > 1e-4f ? span / s : span);
    }
    return d;
}

static int state_node(const LayerDef *L, int state, int variant) {
    if (state < 0 || state >= L->state_count) return -1;
    const StateDef *S = &L->states[state];
    if (variant > 0 && variant <= S->variant_count) return S->variants[variant - 1];
    return S->node;
}

// ------------------------------------------------------------------ runtime

void ctrl_init(ControllerInst *c, const ControllerDef *def, const AnimBinding *bind) {
    memset(c, 0, sizeof(*c));
    c->def = def;
    c->bind = bind;
    for (int i = 0; i < def->param_count; i++) c->params[i] = def->params[i].def;
    for (int l = 0; l < def->layer_count; l++) {
        c->layer[l].state = (i16)def->layers[l].default_state;
        c->layer[l].prev_state = -1;
    }
}

static bool cond_ok(const ControllerInst *c, const Condition *k) {
    float v = pv(c, k->param);
    switch (k->op) {
    case CO_GT: return v > k->value;
    case CO_LT: return v < k->value;
    case CO_GE: return v >= k->value;
    case CO_LE: return v <= k->value;
    case CO_EQ: return fabsf(v - k->value) < 1e-4f;
    case CO_NE: return fabsf(v - k->value) >= 1e-4f;
    case CO_TRUE: return v > 0.5f;
    case CO_FALSE: return v <= 0.5f;
    case CO_TRIGGER: return k->param >= 0 && k->param < 64 && (c->triggers & (1ull << k->param)) != 0;
    }
    return false;
}

static float advance_phase(const ControllerInst *c, const LayerDef *L, int state, int variant, float phase, const ClipLookup *cl,
                           float dt, bool *finished) {
    const StateDef *S = &L->states[state];
    if (S->time_param >= 0) {
        *finished = c->params[S->time_param] >= 1.0f;
        return saturate(c->params[S->time_param]);
    }
    int node = state_node(L, state, variant);
    if (node < 0) { *finished = true; return 0; }
    LeafSet ls;
    // Same fallback rule as evaluation: only the base layer plays idle for unbound clips; an
    // unbound one-shot on an upper layer finishes at once (it must not hold the hands for the
    // length of the idle clip).
    resolve_leaves_ex(c, node, &ls, L == &c->def->layers[0]);
    float dur = leaves_duration(&ls, cl);
    float speed = S->speed * (S->speed_param >= 0 ? c->params[S->speed_param] : 1.0f);
    if (S->speed_match) {
        float avg = 0;
        for (int i = 0; i < ls.n; i++)
            if (ls.l[i].clip >= 0 && ls.l[i].clip < cl->count) avg += ls.l[i].weight * cl->clips[ls.l[i].clip].avg_speed * ls.l[i].speed;
        if (avg > 0.3f) speed *= clampf(c->params[AP_SPEED] / avg, 0.6f, 2.0f);
    }
    float p = phase + (dur > 1e-4f ? dt * speed / dur : 1.0f);
    if (S->loop) {
        if (p >= 1.0f) p -= floorf(p);
        *finished = false;
    } else {
        *finished = p >= 1.0f;
        p = fminf(p, 1.0f);
    }
    return p;
}

static void log_transition(ControllerInst *c, int layer, int from, int to) {
    TransitionLog *t = &c->history[c->history_head];
    c->history_head = (c->history_head + 1) % AC_HISTORY;
    t->tick = c->tick;
    t->layer = (u8)layer;
    t->from = (i16)from;
    t->to = (i16)to;
}

void ctrl_tick(ControllerInst *c, const ClipLookup *cl, float dt) {
    const ControllerDef *d = c->def;
    c->event_count = 0;
    for (int li = 0; li < d->layer_count; li++) {
        const LayerDef *L = &d->layers[li];
        LayerState *S = &c->layer[li];
        bool finished = false;
        float old_phase = S->phase;
        S->phase = advance_phase(c, L, S->state, S->variant, S->phase, cl, dt, &finished);
        S->state_ticks++;
        if (S->blend_total) {
            bool pf;
            if (S->prev_state >= 0) S->prev_phase = advance_phase(c, L, S->prev_state, S->prev_variant, S->prev_phase, cl, dt, &pf);
            if (++S->blend_ticks >= S->blend_total) { S->blend_total = 0; S->blend_ticks = 0; S->prev_state = -1; }
        }
        // Footstep events from the base layer's sync markers.
        if (li == 0 && cl && S->state >= 0) {
            int node = state_node(L, S->state, S->variant);
            if (node >= 0) {
                LeafSet ls;
                resolve_leaves(c, node, &ls);
                int lead = 0;
                for (int i = 1; i < ls.n; i++) if (ls.l[i].weight > ls.l[lead].weight) lead = i;
                const Clip *cp = (ls.n && ls.l[lead].clip >= 0 && ls.l[lead].clip < cl->count) ? &cl->clips[ls.l[lead].clip] : NULL;
                if (cp && cp->has_markers && c->params[AP_SPEED] > 0.3f) {
                    for (int f = 0; f < 2; f++) {
                        float m = cp->plant[f];
                        bool crossed = old_phase <= S->phase ? (old_phase < m && S->phase >= m) : (old_phase < m || S->phase >= m);
                        if (crossed && c->event_count < 8) c->events[c->event_count++] = (u8)(f == 0 ? CEV_FOOTSTEP_L : CEV_FOOTSTEP_R);
                    }
                }
            }
        }
        // Transitions.
        if (S->blend_total && !S->interruptible) continue;
        for (int ti = 0; ti < L->trans_count; ti++) {
            const TransitionDef *T = &L->trans[ti];
            if (T->from >= 0 && T->from != S->state) continue;
            if (T->from == -1 && T->to == S->state && !T->allow_self) continue;
            if (T->from < -1) continue;
            bool ok = true;
            for (int k = 0; k < T->cond_count && ok; k++) ok = cond_ok(c, &T->cond[k]);
            if (!ok) continue;
            if (T->exit_time >= 0.0f) {
                bool loop = L->states[S->state].loop;
                if (!loop && !finished && S->phase < T->exit_time) continue;
                if (loop && S->phase < T->exit_time) continue;
            }
            int from = S->state;
            const StateDef *dst = &L->states[T->to];
            bool keep_phase = L->states[from].sync && dst->sync;
            int variant = 0;
            if (dst->variant_count > 0) variant = (int)((u32)c->params[AP_SEED] % (u32)(dst->variant_count + 1));
            if (T->inertialize || T->duration <= 0.0f) {
                S->prev_state = -1;
                S->blend_total = 0;
                if (T->inertialize && T->duration > 0.0f) {
                    c->inert_pending = true;
                    c->inert_tick = c->tick + 1;
                    c->inert_duration = T->duration;
                }
            } else {
                S->prev_state = (i16)from;
                S->prev_variant = S->variant;
                S->prev_phase = S->phase;
                S->blend_ticks = 0;
                S->blend_total = (u16)DG_MAX(1, (int)lrintf(T->duration / dt));
            }
            S->interruptible = T->interruptible;
            S->state = T->to;
            S->variant = (i16)variant;
            S->state_ticks = 0;
            if (!keep_phase) S->phase = 0;
            log_transition(c, li, from, T->to);
            break;
        }
    }
    c->triggers = 0;
    c->tick++;
}

void ctrl_force_state(ControllerInst *c, int layer, int state, u32 state_ticks, float phase) {
    if (layer < 0 || layer >= c->def->layer_count) return;
    const LayerDef *L = &c->def->layers[layer];
    if (state < 0 || state >= L->state_count) return;
    LayerState *S = &c->layer[layer];
    if (S->state != state) {
        S->prev_state = S->state;
        S->prev_phase = S->phase;
        S->prev_variant = S->variant;
        S->blend_ticks = 0;
        S->blend_total = 6;   // short crossfade hides the snap
        log_transition(c, layer, S->state, state);
    }
    S->state = (i16)state;
    S->state_ticks = state_ticks;
    S->phase = phase;
}

float ctrl_state_time(const ControllerInst *c, int layer) {
    return (layer >= 0 && layer < c->def->layer_count) ? c->layer[layer].phase : 0;
}

bool ctrl_hand_ik_weight(const ControllerInst *c, float *w) {
    float out = 1.0f;
    for (int li = 0; li < c->def->layer_count; li++) {
        const LayerDef *L = &c->def->layers[li];
        const LayerState *S = &c->layer[li];
        float cur = L->states[S->state].hand_ik ? 1.0f : 0.0f;
        if (S->blend_total && S->prev_state >= 0) {
            float a = (float)S->blend_ticks / (float)S->blend_total;
            float prev = L->states[S->prev_state].hand_ik ? 1.0f : 0.0f;
            cur = lerpf(prev, cur, a);
        }
        float lw = L->weight * (L->weight_param >= 0 ? c->params[L->weight_param] : 1.0f);
        if (L->states[S->state].node < 0 && (!S->blend_total || S->prev_state < 0)) continue;  // empty state: no say
        out = fminf(out, lerpf(1.0f, cur, saturate(lw)));
    }
    *w = out;
    return out > 0.0f;
}

// ------------------------------------------------------------------ evaluation

static void sample_leaves(const ControllerInst *c, const Skeleton *skel, const ClipLookup *cl, const LeafSet *ls, float phase, Pose *out) {
    Pose tmp;
    float total = 0;
    bool first = true;
    // Phase sync on one shared reference: state phase 0 = the left foot plant of every looping
    // clip (mirrored clips swap feet, reversed clips run their plants backwards). The reference
    // never depends on which leaves are active, so weights / leaf sets can change without a pop.
    for (int i = 0; i < ls->n; i++) {
        const Leaf *l = &ls->l[i];
        if (l->clip < 0 || l->clip >= cl->count) continue;
        const Clip *cp = &cl->clips[l->clip];
        float ph = phase;
        bool synced = cp->loop && cp->has_markers;
        if (synced) {
            float left = l->mirror ? cp->plant[1] : cp->plant[0];
            ph = phase + (l->reverse ? 1.0f - left : left);
            ph -= floorf(ph);
        }
        if (l->reverse) ph = 1.0f - ph;
        ph = l->r0 + ph * (l->r1 - l->r0);
        Pose *dst = first ? out : &tmp;
        clip_sample_phase(cp, ph, dst);
        if (l->mirror) pose_mirror(skel, dst, dst);
        total += l->weight;
        if (!first) pose_blend(out, &tmp, l->weight / total, out);
        first = false;
    }
    if (first) pose_bind(skel, out);
}

static bool state_pose(const ControllerInst *c, const Skeleton *skel, const ClipLookup *cl, const LayerDef *L, int state, int variant,
                       float phase, Pose *out) {
    int node = state_node(L, state, variant);
    if (node < 0) return false;
    LeafSet ls;
    resolve_leaves_ex(c, node, &ls, L == &c->def->layers[0]);
    if (ls.n == 0) return false;
    sample_leaves(c, skel, cl, &ls, phase, out);
    return true;
}

// Inertialization decay (Bollo): quintic with initial position x0 and velocity v0 over t1.
static float inert_eval(float x0, float v0, float t, float t1) {
    if (t >= t1 || t1 <= 0) return 0;
    if (x0 * v0 > 0) v0 = 0;   // velocity must point towards zero
    float t1v = -5.0f * x0 / (v0 != 0 ? v0 : -1e-9f);
    if (v0 != 0 && t1v > 0 && t1v < t1) t1 = t1v;
    float a0 = (-8.0f * v0 * t1 - 20.0f * x0) / (t1 * t1);
    float A = -(a0 * t1 * t1 + 6 * v0 * t1 + 12 * x0) / (2 * t1 * t1 * t1 * t1 * t1);
    float B = (3 * a0 * t1 * t1 + 16 * v0 * t1 + 30 * x0) / (2 * t1 * t1 * t1 * t1);
    float C = -(3 * a0 * t1 * t1 + 12 * v0 * t1 + 20 * x0) / (2 * t1 * t1 * t1);
    return A * t * t * t * t * t + B * t * t * t * t + C * t * t * t + a0 * 0.5f * t * t + v0 * t + x0;
}

static vec3 inert_vec(vec3 x0, vec3 v0, float t, float t1) {
    float len = v3_len(x0);
    if (len < 1e-6f) return v3(0, 0, 0);
    vec3 dir = v3_scale(x0, 1.0f / len);
    float vl = v3_dot(v0, dir);
    return v3_scale(dir, inert_eval(len, vl, t, t1));
}

void ctrl_evaluate(ControllerInst *c, const Skeleton *skel, const ClipLookup *cl, Pose *out) {
    const ControllerDef *d = c->def;
    pose_bind(skel, out);
    bool any_solo = false;
    for (int li = 0; li < d->layer_count; li++) any_solo |= c->solo[li];
    Pose cur, prev, ref;
    for (int li = 0; li < d->layer_count; li++) {
        if (c->mute[li] || (any_solo && !c->solo[li])) continue;
        const LayerDef *L = &d->layers[li];
        const LayerState *S = &c->layer[li];
        float lw = L->weight * (L->weight_param >= 0 ? c->params[L->weight_param] : 1.0f);
        if (lw <= 0) continue;
        bool has_cur = state_pose(c, skel, cl, L, S->state, S->variant, S->phase, &cur);
        float a = 1.0f;
        bool has_prev = false;
        if (S->blend_total && S->prev_state >= 0) {
            a = (float)S->blend_ticks / (float)S->blend_total;
            a = a * a * (3.0f - 2.0f * a);
            has_prev = state_pose(c, skel, cl, L, S->prev_state, S->prev_variant, S->prev_phase, &prev);
        }
        if (!has_cur && !has_prev) continue;
        // Empty states blend from / to the incoming pose.
        if (!has_cur) { pose_copy(&cur, out); }
        if (has_prev) pose_blend(&prev, &cur, a, &cur);
        else if (!has_cur) continue;
        else if (S->blend_total && S->prev_state >= 0) { lw *= a; }   // prev empty: fade in
        const float *mask = L->mask >= 0 ? d->masks[L->mask].w : NULL;
        if (L->blend == LB_ADDITIVE) {
            // Reference: the state's first frame.
            state_pose(c, skel, cl, L, S->state, S->variant, 0.0f, &ref);
            pose_additive(out, &cur, &ref, lw, mask, out);
        } else {
            pose_blend_masked(out, &cur, mask, lw, out);
        }
    }
    // Inertialization: capture the offset on the first evaluation after the switch.
    if (c->inert_pending && c->have_last) {
        c->inert_pending = false;
        for (int b = 0; b < skel->count; b++) {
            quat dq = quat_mul(c->last_out.local[b].r, quat_conj(out->local[b].r));
            c->inert_rot_x0[b] = quat_to_scaled_axis(dq);
            quat vq = quat_mul(c->last_out.local[b].r, quat_conj(c->prev_out.local[b].r));
            c->inert_rot_v0[b] = v3_scale(quat_to_scaled_axis(vq), (float)SIM_RATE_FOR_INERT);
        }
        c->inert_pos_x0 = v3_sub(c->last_out.local[0].t, out->local[0].t);
        c->inert_pos_v0 = v3_scale(v3_sub(c->last_out.local[0].t, c->prev_out.local[0].t), (float)SIM_RATE_FOR_INERT);
    } else if (c->inert_pending) {
        c->inert_pending = false;
        c->inert_duration = 0;
    }
    if (c->inert_duration > 0) {
        float t = (float)(c->tick - c->inert_tick) / (float)SIM_RATE_FOR_INERT;
        if (t >= c->inert_duration) c->inert_duration = 0;
        else {
            for (int b = 0; b < skel->count; b++) {
                vec3 off = inert_vec(c->inert_rot_x0[b], c->inert_rot_v0[b], t, c->inert_duration);
                out->local[b].r = quat_norm(quat_mul(quat_from_scaled_axis(off), out->local[b].r));
            }
            out->local[0].t = v3_add(out->local[0].t, inert_vec(c->inert_pos_x0, c->inert_pos_v0, t, c->inert_duration));
        }
    }
    pose_copy(&c->prev_out, c->have_last ? &c->last_out : out);
    pose_copy(&c->last_out, out);
    c->have_last = true;
}

bool ctrl_state_start_hips_y(const ControllerInst *c, const ClipLookup *cl, int layer, int state, float *y) {
    if (!cl || layer < 0 || layer >= c->def->layer_count) return false;
    const LayerDef *L = &c->def->layers[layer];
    int node = state_node(L, state, state == c->layer[layer].state ? c->layer[layer].variant : 0);
    if (node < 0) return false;
    LeafSet ls;
    resolve_leaves_ex(c, node, &ls, layer == 0);
    int lead = -1;
    for (int i = 0; i < ls.n; i++)
        if (ls.l[i].clip >= 0 && ls.l[i].clip < cl->count && (lead < 0 || ls.l[i].weight > ls.l[lead].weight)) lead = i;
    if (lead < 0) return false;
    const Clip *cp = &cl->clips[ls.l[lead].clip];
    float u = ls.l[lead].reverse ? ls.l[lead].r1 : ls.l[lead].r0;
    int f = DG_CLAMP((int)lrintf(u * (float)(cp->frames - 1)), 0, cp->frames - 1);
    *y = cp->data[(size_t)f * (size_t)cp->bones].t.y;
    return true;
}

void ctrl_root_delta(const ControllerInst *c, const ClipLookup *cl, float dt, vec3 *dpos, float *dyaw) {
    *dpos = v3(0, 0, 0);
    *dyaw = 0;
    if (!c->def->layer_count) return;
    const LayerDef *L = &c->def->layers[0];
    const LayerState *S = &c->layer[0];
    int node = state_node(L, S->state, S->variant);
    if (node < 0) return;
    LeafSet ls;
    resolve_leaves(c, node, &ls);
    for (int i = 0; i < ls.n; i++) {
        if (ls.l[i].clip < 0) continue;
        const Clip *cp = &cl->clips[ls.l[i].clip];
        if (!cp->root_motion) continue;
        float span = cp->frames > 1 ? (float)(cp->frames - 1) / cp->rate : 0.0f;
        float t1 = S->phase * span;
        float t0 = t1 - dt * ls.l[i].speed * cp->speed;
        vec3 p;
        float y;
        clip_root_delta(cp, t0, t1, &p, &y);
        *dpos = v3_madd(*dpos, p, ls.l[i].weight);
        *dyaw += y * ls.l[i].weight;
    }
}
