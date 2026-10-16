#include "gui_layout.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Solver {
    LayoutNode *n;
    int count;
    float s;
    int kids[GUI_LAYOUT_MAX_NODES][GUI_LAYOUT_MAX_NODES];
    int kid_count[GUI_LAYOUT_MAX_NODES];
} Solver;

float gui_layout_scale(float w, float h, float user_scale) {
    if (user_scale > 0.01f) return user_scale;
    float s = h / 1080.0f;
    if (s < 0.6f) s = 0.6f;
    if (s > 4.0f) s = 4.0f;
    if (w / s < 1280.0f) s = w / 1280.0f;   // narrow screens (4:3, 5:4): keep 1280 design px across
    return s;
}

GuiRect gui_layout_safe_rect(const LayoutParams *p) {
    float m = p->safe_frac * fminf(p->w, p->h);
    GuiRect r = { m, m, p->w - 2 * m, p->h - 2 * m };
    if (p->max_aspect > 0.0f && r.h > 0 && r.w / r.h > p->max_aspect) {
        float nw = r.h * p->max_aspect;
        r.x += (r.w - nw) * 0.5f;
        r.w = nw;
    }
    return r;
}

static float pad(const Solver *S, const LayoutNode *n, int k) { return n->pad[k] * S->s; }

// Fit size (children + gaps + padding) along both axes; fixed sizes only (fractions need a parent).
static void measure(const Solver *S, int i, float *ow, float *oh) {
    const LayoutNode *n = &S->n[i];
    float w = 0, h = 0;
    int vis = 0;
    for (int k = 0; k < S->kid_count[i]; k++) {
        int c = S->kids[i][k];
        const LayoutNode *cn = &S->n[c];
        if (cn->hidden) continue;
        float cw, ch;
        measure(S, c, &cw, &ch);
        if (cn->w > 0) cw = cn->w * S->s;
        if (cn->h > 0) ch = cn->h * S->s;
        if (cn->aspect > 0 && cn->w > 0 && cn->h <= 0) ch = cw / cn->aspect;
        if (cn->aspect > 0 && cn->h > 0 && cn->w <= 0) cw = ch * cn->aspect;
        if (n->dir == GD_ROW) { w += cw; h = fmaxf(h, ch); }
        else if (n->dir == GD_COLUMN) { h += ch; w = fmaxf(w, cw); }
        else { w = fmaxf(w, cw + fabsf(cn->x) * S->s); h = fmaxf(h, ch + fabsf(cn->y) * S->s); }
        vis++;
    }
    if (vis > 1 && n->dir == GD_ROW) w += n->gap * S->s * (float)(vis - 1);
    if (vis > 1 && n->dir == GD_COLUMN) h += n->gap * S->s * (float)(vis - 1);
    *ow = w + pad(S, n, 0) + pad(S, n, 2);
    *oh = h + pad(S, n, 1) + pad(S, n, 3);
}

// Size of node i inside a parent content box (pw, ph). stretch_w / h: fill that axis when auto.
static void resolve_size(const Solver *S, int i, float pw, float ph, bool stretch_w, bool stretch_h, float *ow, float *oh) {
    const LayoutNode *n = &S->n[i];
    float fw, fh;
    measure(S, i, &fw, &fh);
    float w = n->w > 0 ? n->w * S->s : (n->w_pct > 0 ? n->w_pct * pw : (stretch_w ? pw : fw));
    float h = n->h > 0 ? n->h * S->s : (n->h_pct > 0 ? n->h_pct * ph : (stretch_h ? ph : fh));
    bool auto_w = n->w <= 0 && n->w_pct <= 0, auto_h = n->h <= 0 && n->h_pct <= 0;
    if (n->max_w_pct > 0) w = fminf(w, n->max_w_pct * pw);
    if (n->max_h_pct > 0) h = fminf(h, n->max_h_pct * ph);
    if (n->max_w > 0) w = fminf(w, n->max_w * S->s);
    if (n->max_h > 0) h = fminf(h, n->max_h * S->s);
    if (n->min_w > 0) w = fmaxf(w, n->min_w * S->s);
    if (n->min_h > 0) h = fmaxf(h, n->min_h * S->s);
    if (n->aspect > 0) {
        if (auto_h && !auto_w) h = w / n->aspect;
        else if (auto_w && !auto_h) w = h * n->aspect;
        else if (w / fmaxf(h, 1e-3f) > n->aspect) w = h * n->aspect;   // contain
        else h = w / n->aspect;
    }
    // Never larger than the parent (a min size may still exceed a tiny viewport).
    *ow = fmaxf(0.0f, fminf(w, fmaxf(pw, n->min_w * S->s)));
    *oh = fmaxf(0.0f, fminf(h, fmaxf(ph, n->min_h * S->s)));
}

static void place_children(Solver *S, int i);

static void place_anchor(Solver *S, int c, GuiRect box) {
    LayoutNode *cn = &S->n[c];
    float w, h;
    resolve_size(S, c, box.w, box.h, false, false, &w, &h);
    int ax = cn->anchor % 3, ay = cn->anchor / 3;
    float x = box.x + (ax == 0 ? 0 : (ax == 1 ? (box.w - w) * 0.5f : box.w - w)) + cn->x * S->s;
    float y = box.y + (ay == 0 ? 0 : (ay == 1 ? (box.h - h) * 0.5f : box.h - h)) + cn->y * S->s;
    cn->rect = gui_rect(x, y, w, h);
    place_children(S, c);
}

static void place_children(Solver *S, int i) {
    LayoutNode *n = &S->n[i];
    GuiRect box = { n->rect.x + pad(S, n, 0), n->rect.y + pad(S, n, 1), n->rect.w - pad(S, n, 0) - pad(S, n, 2),
                    n->rect.h - pad(S, n, 1) - pad(S, n, 3) };
    box.w = fmaxf(box.w, 0);
    box.h = fmaxf(box.h, 0);
    if (n->dir == GD_OVERLAY) {
        for (int k = 0; k < S->kid_count[i]; k++) {
            int c = S->kids[i][k];
            if (!S->n[c].hidden) place_anchor(S, c, box);
        }
        return;
    }
    bool row = n->dir == GD_ROW;
    float main = row ? box.w : box.h, cross = row ? box.h : box.w;
    float gap = n->gap * S->s;
    float sizes[GUI_LAYOUT_MAX_NODES][2];
    float used = 0, flex_total = 0;
    int vis = 0;
    for (int k = 0; k < S->kid_count[i]; k++) {
        int c = S->kids[i][k];
        LayoutNode *cn = &S->n[c];
        if (cn->hidden) continue;
        bool stretch = n->align == GAL_STRETCH;
        float w, h;
        resolve_size(S, c, box.w, box.h, row ? false : stretch, row ? stretch : false, &w, &h);
        sizes[k][0] = w;
        sizes[k][1] = h;
        if (cn->flex > 0) flex_total += cn->flex;
        else used += row ? w : h;
        vis++;
    }
    if (vis > 1) used += gap * (float)(vis - 1);
    float remain = fmaxf(0.0f, main - used);
    float pos = row ? box.x : box.y;
    for (int k = 0; k < S->kid_count[i]; k++) {
        int c = S->kids[i][k];
        LayoutNode *cn = &S->n[c];
        if (cn->hidden) continue;
        float w = sizes[k][0], h = sizes[k][1];
        if (cn->flex > 0 && flex_total > 0) {
            float m = remain * cn->flex / flex_total;
            if (row) w = m; else h = m;
        }
        float csz = row ? h : w;
        float off = n->align == GAL_CENTER ? (cross - csz) * 0.5f : (n->align == GAL_END ? cross - csz : 0.0f);
        if (row) cn->rect = gui_rect(pos, box.y + off, w, h);
        else cn->rect = gui_rect(box.x + off, pos, w, h);
        pos += (row ? w : h) + gap;
        place_children(S, c);
    }
}

void gui_layout_solve(LayoutNode *n, int count, const LayoutParams *p) {
    static Solver S;
    if (count > GUI_LAYOUT_MAX_NODES) count = GUI_LAYOUT_MAX_NODES;
    S.n = n;
    S.count = count;
    S.s = p->scale > 0 ? p->scale : 1.0f;
    memset(S.kid_count, 0, sizeof(S.kid_count));
    for (int i = 0; i < count; i++) {
        n[i].rect = gui_rect(0, 0, 0, 0);
        int pa = n[i].parent;
        if (pa >= 0 && pa < i) S.kids[pa][S.kid_count[pa]++] = i;
    }
    GuiRect view = { 0, 0, p->w, p->h };
    GuiRect safe = gui_layout_safe_rect(p);
    for (int i = 0; i < count; i++) {
        if (n[i].parent >= 0 || n[i].hidden) continue;
        GuiRect box = n[i].safe ? safe : view;
        if (n[i].w <= 0 && n[i].h <= 0 && n[i].w_pct <= 0 && n[i].h_pct <= 0) {
            n[i].rect = box;   // a root without a size is its whole box
            place_children(&S, i);
        } else {
            place_anchor(&S, i, box);
        }
    }
    // Avoidance: move beside the named node (to the side with more room), shrink to stay inside
    // the parent's box, then re-place the subtree.
    for (int i = 0; i < count; i++) {
        if (!n[i].avoid[0] || n[i].hidden) continue;
        int t = gui_layout_find(n, count, n[i].avoid);
        if (t < 0 || n[t].hidden || !gui_rect_overlap(n[i].rect, n[t].rect)) continue;
        GuiRect box = n[i].parent >= 0 ? n[n[i].parent].rect : (n[i].safe ? safe : view);
        float gap = 24.0f * S.s;
        GuiRect r = n[i].rect, a = n[t].rect;
        float room_right = box.x + box.w - (a.x + a.w + gap), room_left = a.x - gap - box.x;
        if (room_right >= room_left) {
            r.x = fmaxf(r.x, a.x + a.w + gap);
            if (r.x + r.w > box.x + box.w) r.w = fmaxf(0.0f, box.x + box.w - r.x);
        } else {
            float right = fminf(r.x + r.w, a.x - gap);
            r.w = fmaxf(0.0f, fminf(r.w, right - box.x));
            r.x = right - r.w;
        }
        n[i].rect = r;
        place_children(&S, i);
    }
}

int gui_layout_find(const LayoutNode *n, int count, const char *id) {
    for (int i = 0; i < count; i++) if (!strcmp(n[i].id, id)) return i;
    return -1;
}

static int parse_anchor(const char *s) {
    static const char *names[9] = { "top_left", "top", "top_right", "left", "center", "right", "bottom_left", "bottom", "bottom_right" };
    for (int i = 0; i < 9; i++) if (!strcmp(s, names[i])) return i;
    return GA_TL;
}

static void get_str(const cJSON *o, const char *key, char *dst, size_t size) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsString(v)) dg_strcpy(dst, size, v->valuestring);
}

static const LayoutToken *g_tokens;
static int g_token_count;
static char g_bad_token[40];   // the first unknown "@token" of the current parse

void gui_layout_set_tokens(const LayoutToken *tokens, int count) {
    g_tokens = tokens;
    g_token_count = count;
}

bool gui_layout_token(const char *name, float *out) {
    for (int i = 0; i < g_token_count; i++)
        if (!strcmp(g_tokens[i].name, name)) { *out = g_tokens[i].value; return true; }
    return false;
}

// A number, or "@token" (theme.json "layout").
static float get_num(const cJSON *o, const char *key, float def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsString(v) && v->valuestring[0] == '@') {
        float t;
        if (gui_layout_token(v->valuestring + 1, &t)) return t;
        if (!g_bad_token[0]) dg_strcpy(g_bad_token, sizeof(g_bad_token), v->valuestring);
        return def;
    }
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}

bool gui_layout_parse(const char *json, LayoutNode *out, int max, int *count, char *err, int err_size) {
    *count = 0;
    if (err && err_size) err[0] = 0;
    g_bad_token[0] = 0;
    cJSON *root = cJSON_Parse(json);
    if (!root) { if (err) snprintf(err, (size_t)err_size, "JSON syntax error near: %.40s", cJSON_GetErrorPtr() ? cJSON_GetErrorPtr() : "?"); return false; }
    const cJSON *nodes = cJSON_GetObjectItemCaseSensitive(root, "nodes");
    bool ok = cJSON_IsArray(nodes);
    const cJSON *o;
    cJSON_ArrayForEach(o, nodes) {
        if (*count >= max || *count >= GUI_LAYOUT_MAX_NODES) { if (err) snprintf(err, (size_t)err_size, "too many nodes"); ok = false; break; }
        LayoutNode *n = &out[*count];
        memset(n, 0, sizeof(*n));
        n->parent = -1;
        get_str(o, "id", n->id, sizeof(n->id));
        get_str(o, "parent", n->parent_id, sizeof(n->parent_id));
        get_str(o, "avoid", n->avoid, sizeof(n->avoid));
        get_str(o, "label", n->label, sizeof(n->label));
        get_str(o, "action", n->action, sizeof(n->action));
        get_str(o, "if", n->cond, sizeof(n->cond));
        if (n->parent_id[0]) {
            n->parent = gui_layout_find(out, *count, n->parent_id);
            if (n->parent < 0) { if (err) snprintf(err, (size_t)err_size, "node '%s': unknown parent '%s' (parents come first)", n->id, n->parent_id); ok = false; break; }
        }
        char s[24] = "";
        get_str(o, "dir", s, sizeof(s));
        n->dir = !strcmp(s, "row") ? GD_ROW : (!strcmp(s, "column") ? GD_COLUMN : GD_OVERLAY);
        s[0] = 0;
        get_str(o, "anchor", s, sizeof(s));
        n->anchor = s[0] ? parse_anchor(s) : GA_TL;
        s[0] = 0;
        get_str(o, "align", s, sizeof(s));
        n->align = !strcmp(s, "center") ? GAL_CENTER : (!strcmp(s, "end") ? GAL_END : (!strcmp(s, "stretch") ? GAL_STRETCH : GAL_START));
        n->x = get_num(o, "x", 0);
        n->y = get_num(o, "y", 0);
        n->w = get_num(o, "w", 0);
        n->h = get_num(o, "h", 0);
        n->w_pct = get_num(o, "w_pct", 0);
        n->h_pct = get_num(o, "h_pct", 0);
        n->min_w = get_num(o, "min_w", 0);
        n->min_h = get_num(o, "min_h", 0);
        n->max_w = get_num(o, "max_w", 0);
        n->max_h = get_num(o, "max_h", 0);
        n->max_w_pct = get_num(o, "max_w_pct", 0);
        n->max_h_pct = get_num(o, "max_h_pct", 0);
        n->aspect = get_num(o, "aspect", 0);
        n->flex = get_num(o, "flex", 0);
        n->gap = get_num(o, "gap", 0);
        float pd = get_num(o, "pad", 0);
        for (int k = 0; k < 4; k++) n->pad[k] = pd;
        const cJSON *pa = cJSON_GetObjectItemCaseSensitive(o, "pad");
        if (cJSON_IsArray(pa) && cJSON_GetArraySize(pa) == 4)
            for (int k = 0; k < 4; k++) {
                const cJSON *pv = cJSON_GetArrayItem(pa, k);
                float t = 0;
                if (cJSON_IsString(pv) && pv->valuestring[0] == '@') {
                    if (gui_layout_token(pv->valuestring + 1, &t)) n->pad[k] = t;
                    else if (!g_bad_token[0]) dg_strcpy(g_bad_token, sizeof(g_bad_token), pv->valuestring);
                } else n->pad[k] = (float)pv->valuedouble;
            }
        n->safe = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "safe"));
        if (g_bad_token[0]) { if (err) snprintf(err, (size_t)err_size, "node '%s': unknown theme token '%s'", n->id, g_bad_token); ok = false; break; }
        (*count)++;
    }
    if (!cJSON_IsArray(nodes) && err) snprintf(err, (size_t)err_size, "missing \"nodes\" array");
    cJSON_Delete(root);
    return ok;
}
