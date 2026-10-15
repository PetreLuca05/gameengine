#include "character.h"
#include "gameplay/hitboxes.h"
#include "cJSON.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>

static const char *js(const cJSON *o, const char *k, const char *def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : def;
}
static float jf(const cJSON *o, const char *k, float def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}

bool character_def_parse(CharacterDef *d, const char *text, const char *source, char *err, int err_size) {
    memset(d, 0, sizeof(*d));
    cJSON *root = cJSON_Parse(text);
    if (!root) { if (err) snprintf(err, (size_t)err_size, "%s: JSON parse error", source); return false; }
    dg_strcpy(d->id, sizeof(d->id), js(root, "id", "character"));
    d->net_id = (u8)jf(root, "character_id", 0);
    const char *kind = js(root, "kind", "zombie");
    d->kind = !strcmp(kind, "player") ? CK_PLAYER : (!strcmp(kind, "dummy") ? CK_DUMMY : CK_ZOMBIE);
    dg_strcpy(d->mesh_path, sizeof(d->mesh_path), js(root, "mesh", "assets/human/Dummy.fbx"));
    const cJSON *lods = cJSON_GetObjectItemCaseSensitive(root, "lods");
    int li = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, lods) if (cJSON_IsString(e) && li < CHAR_LOD_FILES) dg_strcpy(d->lod_paths[li++], 128, e->valuestring);
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(root, "materials")) {
        if (d->material_count >= SKIN_MAX_MATERIALS || !cJSON_IsArray(e) || cJSON_GetArraySize(e) < 3) continue;
        dg_strcpy(d->material_names[d->material_count], 48, e->string);
        float a = cJSON_GetArraySize(e) > 3 ? (float)cJSON_GetArrayItem(e, 3)->valuedouble : 1.0f;
        d->material_colors[d->material_count++] =
            v4((float)cJSON_GetArrayItem(e, 0)->valuedouble, (float)cJSON_GetArrayItem(e, 1)->valuedouble, (float)cJSON_GetArrayItem(e, 2)->valuedouble, a);
    }
    dg_strcpy(d->anim_set, sizeof(d->anim_set), js(root, "anim_set", d->kind == CK_PLAYER ? "player" : "zombie"));
    part_mult_defaults(d->part_mult);
    part_mult_parse(d->part_mult, cJSON_GetObjectItemCaseSensitive(root, "part_mult"));
    d->ragdoll_scale = jf(root, "ragdoll_scale", 1.0f);
    d->scale = jf(root, "scale", 1.0f);
    d->hp = jf(root, "hp", d->kind == CK_PLAYER ? 100.0f : 120.0f);
    d->move_speed = jf(root, "move_speed", 1.3f);
    d->lod_bias = jf(root, "lod_bias", 1.0f);
    d->hash = anim_hash_str(text, ANIM_HASH_SEED);
    cJSON_Delete(root);
    return true;
}

// ------------------------------------------------------------------ rig validation

static void rep(RigReport *r, const char *fmt, ...) {
    if (r->len >= (int)sizeof(r->text) - 2) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(r->text + r->len, sizeof(r->text) - (size_t)r->len, fmt, ap);
    va_end(ap);
    if (n > 0) r->len = DG_MIN(r->len + n, (int)sizeof(r->text) - 1);
}

// Nearest ancestor of bone b (in skel s) that exists in other (by name); returns its name or NULL.
static const char *nearest_shared_ancestor(const Skeleton *s, int b, const Skeleton *other) {
    for (int p = s->parent[b]; p >= 0; p = s->parent[p])
        if (skel_find(other, s->names[p]) >= 0) return s->names[p];
    return NULL;
}

static bool has_canonical_descendant(const Skeleton *model, int b, const Skeleton *canon) {
    for (int i = b + 1; i < model->count; i++)
        if (skel_is_descendant(model, i, b) && skel_find(canon, model->names[i]) >= 0) return true;
    return false;
}

bool rig_validate(const Skeleton *canon, const Skeleton *model, float tol, RigReport *r) {
    memset(r, 0, sizeof(*r));
    // Canonical bones missing from the model.
    for (int c = 0; c < canon->count; c++) {
        if (skel_find(model, canon->names[c]) >= 0) continue;
        r->missing++;
        // A missing bone whose canonical descendants exist in the model breaks the hierarchy.
        bool child_present = false;
        for (int k = c + 1; k < canon->count && !child_present; k++)
            if (skel_is_descendant(canon, k, c) && skel_find(model, canon->names[k]) >= 0) child_present = true;
        if (child_present) {
            // Renamed? A model bone that isn't canonical, at the same place (same shared parent).
            const char *cp = canon->parent[c] >= 0 ? canon->names[canon->parent[c]] : NULL;
            const char *cand = NULL;
            for (int m = 0; m < model->count; m++) {
                if (skel_find(canon, model->names[m]) >= 0) continue;
                const char *mp = model->parent[m] >= 0 ? model->names[model->parent[m]] : NULL;
                if ((cp && mp && !strcmp(cp, mp)) || (!cp && !mp)) { cand = model->names[m]; break; }
            }
            r->hierarchy_errors++;
            if (cand) rep(r, "ERROR renamed bone: canonical '%s' appears as '%s'\n", canon->names[c], cand);
            else rep(r, "ERROR missing intermediate bone '%s' (its children are present)\n", canon->names[c]);
        } else {
            if (r->missing <= 24) rep(r, "  subset: '%s' missing (canonical bind pose used)\n", canon->names[c]);
        }
    }
    for (int m = 0; m < model->count; m++) {
        int c = skel_find(canon, model->names[m]);
        if (c < 0) {
            r->extra++;
            if (has_canonical_descendant(model, m, canon)) {
                // Inserted bone between canonical ones (or the renamed case, already reported).
                bool reported = false;
                for (int k = 0; k < canon->count && !reported; k++)
                    if (skel_find(model, canon->names[k]) < 0) reported = strstr(r->text, model->names[m]) != NULL;
                if (!reported) {
                    r->hierarchy_errors++;
                    rep(r, "ERROR extra bone '%s' has canonical children (hierarchy differs)\n", model->names[m]);
                }
            } else if (r->extra <= 24) {
                const char *anc = nearest_shared_ancestor(model, m, canon);
                rep(r, "  extra leaf bone '%s' follows '%s' (cosmetic layer)\n", model->names[m], anc ? anc : "(root)");
            }
            continue;
        }
        // Parent check against the nearest shared ancestors.
        const char *mp = nearest_shared_ancestor(model, m, canon);
        const char *cp = nearest_shared_ancestor(canon, c, model);
        if ((mp == NULL) != (cp == NULL) || (mp && cp && strcmp(mp, cp))) {
            r->hierarchy_errors++;
            rep(r, "ERROR bone '%s' parent differs: model '%s' vs canonical '%s'\n", model->names[m], mp ? mp : "(root)", cp ? cp : "(root)");
            continue;
        }
        // Proportions.
        if (model->parent[m] >= 0 && canon->parent[c] >= 0) {
            float lm = v3_len(model->bind[m].t), lc = v3_len(canon->bind[c].t);
            if (lc > 0.02f && fabsf(lm - lc) / lc > tol) {
                r->proportion_warnings++;
                if (r->proportion_warnings <= 16)
                    rep(r, "  warn: '%s' length %.3f m vs canonical %.3f m (%+.0f%%)\n", model->names[m], lm, lc, (lm - lc) / lc * 100.0f);
            }
        }
    }
    r->identical = r->missing == 0 && r->extra == 0 && r->hierarchy_errors == 0;
    r->ok = r->hierarchy_errors == 0;
    rep(r, "%s: %d missing, %d extra, %d hierarchy errors, %d proportion warnings\n",
        r->ok ? (r->identical ? "OK identical rig" : "OK compatible rig") : "REJECTED", r->missing, r->extra, r->hierarchy_errors,
        r->proportion_warnings);
    return r->ok;
}
