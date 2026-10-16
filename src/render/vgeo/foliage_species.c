#include "foliage_species.h"
#include "vgeo_build.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *k_wind_keys[11] = { "trunk_stiffness", "branch_stiffness", "leaf_stiffness", "full_wind", "main_freq", "detail_freq",
                                       "flutter_freq", "max_bend_deg", "settle_time", "response", "height" };

static float jnum(const cJSON *o, const char *k, float def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}

static bool parse_entry(const cJSON *e, VgSpecies *sp, char *err, int err_size) {
    memset(sp, 0, sizeof(*sp));
    const cJSON *n = cJSON_GetObjectItemCaseSensitive(e, "name"), *src = cJSON_GetObjectItemCaseSensitive(e, "source");
    if (!cJSON_IsString(n) || !cJSON_IsString(src)) { snprintf(err, (size_t)err_size, "species entry without \"name\" / \"source\""); return false; }
    dg_strcpy(sp->name, sizeof(sp->name), n->valuestring);
    dg_strcpy(sp->source, sizeof(sp->source), src->valuestring);
    const cJSON *pr = cJSON_GetObjectItemCaseSensitive(e, "preset");
    sp->preset = VGWP_TREE;
    if (cJSON_IsString(pr)) sp->preset = !strcmp(pr->valuestring, "grass") ? VGWP_GRASS : !strcmp(pr->valuestring, "bush") ? VGWP_BUSH : VGWP_TREE;
    VgFoliageImportOptions *o = &sp->import;
    vgeo_foliage_import_defaults(o);
    o->unit = jnum(e, "unit", 0.0f);
    o->normalize_branch = !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(e, "normalize_branch"));
    const cJSON *ch = cJSON_GetObjectItemCaseSensitive(e, "wind_channels");
    if (cJSON_IsString(ch) && !strcmp(ch->valuestring, "auto")) o->detect = true;
    else if (cJSON_IsObject(ch)) {
        static const char *keys[VGFC_COUNT] = { "flutter", "phase", "branch" };
        for (int k = 0; k < VGFC_COUNT; k++) {
            const cJSON *c = cJSON_GetObjectItemCaseSensitive(ch, keys[k]);
            if (!c) continue;
            int v = cJSON_IsString(c) ? vgeo_foliage_channel_parse(c->valuestring) : -1;
            if (v < 0) { snprintf(err, (size_t)err_size, "%s: wind_channels.%s must be r, g, b, a or none", sp->name, keys[k]); return false; }
            o->channels[k] = v;
        }
    }
    const cJSON *mats = cJSON_GetObjectItemCaseSensitive(e, "materials"), *m;
    cJSON_ArrayForEach(m, mats) {
        if (o->kind_count >= 8 || !cJSON_IsString(m) || !m->string) continue;
        dg_strcpy(o->kinds[o->kind_count].pattern, sizeof(o->kinds[0].pattern), m->string);
        o->kinds[o->kind_count++].kind = !strcmp(m->valuestring, "bark") ? VGFM_BARK : VGFM_LEAF;
    }
    const cJSON *w = cJSON_GetObjectItemCaseSensitive(e, "wind");
    for (int k = 0; k < 11; k++) sp->wind_set[k] = jnum(w, k_wind_keys[k], NAN);
    const cJSON *cap = cJSON_GetObjectItemCaseSensitive(e, "capsule");
    if (cJSON_IsArray(cap) && cJSON_GetArraySize(cap) >= 2) {
        sp->capsule = true;
        sp->capsule_r = (float)cJSON_GetArrayItem(cap, 0)->valuedouble;
        sp->capsule_h = (float)cJSON_GetArrayItem(cap, 1)->valuedouble;
    }
    const cJSON *fade = cJSON_GetObjectItemCaseSensitive(e, "fade");
    sp->fade_start = sp->preset == VGWP_GRASS ? 40.0f : sp->preset == VGWP_BUSH ? 100.0f : 850.0f;
    sp->fade_end = sp->preset == VGWP_GRASS ? 65.0f : sp->preset == VGWP_BUSH ? 150.0f : 1100.0f;
    if (cJSON_IsArray(fade) && cJSON_GetArraySize(fade) >= 2) {
        sp->fade_start = (float)cJSON_GetArrayItem(fade, 0)->valuedouble;
        sp->fade_end = (float)cJSON_GetArrayItem(fade, 1)->valuedouble;
    }
    sp->shadow_max_dist = jnum(e, "shadow_max_dist", sp->preset == VGWP_TREE ? 70.0f : 25.0f);
    sp->alpha_cutoff = jnum(e, "alpha_cutoff", 0.5f);
    const cJSON *rg = cJSON_GetObjectItemCaseSensitive(e, "roughness");
    sp->rough_bark = 0.9f, sp->rough_leaf = 0.7f;
    if (cJSON_IsArray(rg) && cJSON_GetArraySize(rg) >= 2) {
        sp->rough_bark = (float)cJSON_GetArrayItem(rg, 0)->valuedouble;
        sp->rough_leaf = (float)cJSON_GetArrayItem(rg, 1)->valuedouble;
    }
    sp->impostor = !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(e, "impostor")) && sp->preset != VGWP_GRASS;
    char *txt = cJSON_PrintUnformatted(e);
    sp->key = txt ? vgeo_fnv1a(txt, strlen(txt), 2166136261u) : 1;
    cJSON_free(txt);
    return true;
}

bool vgeo_species_parse(const char *json, VgSpeciesLib *lib, char *err, int err_size) {
    memset(lib, 0, sizeof(*lib));
    if (err_size > 0) err[0] = 0;
    cJSON *root = cJSON_Parse(json);
    if (!root) { snprintf(err, (size_t)err_size, "species: JSON syntax error near '%.32s'", cJSON_GetErrorPtr() ? cJSON_GetErrorPtr() : "?"); return false; }
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(root, "species")) {
        if (lib->count >= VG_SPECIES_MAX) break;
        char e2[160] = "";
        if (parse_entry(e, &lib->s[lib->count], e2, sizeof(e2))) lib->count++;
        else if (err_size > 0 && !err[0]) dg_strcpy(err, (size_t)err_size, e2);
    }
    cJSON_Delete(root);
    return true;
}

int vgeo_species_find(const VgSpeciesLib *lib, const char *name) {
    for (int i = 0; i < lib->count; i++)
        if (!strcmp(lib->s[i].name, name)) return i;
    return -1;
}

void vgeo_species_wind(const VgSpecies *sp, float height, VgWindParams *out) {
    float h = isnan(sp->wind_set[10]) ? height : sp->wind_set[10];
    vgeo_wind_defaults(out, sp->preset, h);
    float *f = (float *)out;   // VgWindParams: 11 floats in k_wind_keys order (+ pad)
    for (int k = 0; k < 10; k++)
        if (!isnan(sp->wind_set[k])) f[k] = sp->wind_set[k];
}
