#include "syn_recipe.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void syn_lib_init(SynLibrary *lib, float sr, SynSampleResolveFn resolve, void *user) {
    memset(lib, 0, sizeof(*lib));
    lib->sr = sr;
    lib->resolve = resolve;
    lib->user = user;
}

void syn_lib_free(SynLibrary *lib) {
    for (int i = 0; i < lib->npatches; i++) free(lib->patch[i]);
    for (int i = 0; i < lib->ngrave; i++) free(lib->graveyard[i]);
    for (int i = 0; i < lib->nrecipes; i++) free(lib->recipe[i]);
    memset(lib, 0, sizeof(*lib));
}

int syn_lib_find_patch(const SynLibrary *lib, const char *name) {
    for (int i = 0; i < lib->npatches; i++) if (!strcmp(lib->patch[i]->name, name)) return i;
    return -1;
}

int syn_lib_find_recipe(const SynLibrary *lib, const char *name) {
    for (int i = 0; i < lib->nrecipes; i++) if (!strcmp(lib->recipe[i]->name, name)) return i;
    return -1;
}

int syn_lib_put_patch(SynLibrary *lib, const SynPatch *src, char *err, int err_size) {
    SynPatch *p = (SynPatch *)malloc(sizeof(SynPatch));
    if (!p) return -1;
    *p = *src;
    if (!syn_patch_compile(p, lib->sr, err, err_size)) { free(p); return -1; }
    int idx = syn_lib_find_patch(lib, p->name);
    if (idx >= 0) {
        // The audio thread may still render the old copy: keep it until shutdown.
        if (lib->ngrave < SYN_LIB_PATCHES) lib->graveyard[lib->ngrave++] = lib->patch[idx];
        lib->patch[idx] = p;
        return idx;
    }
    if (lib->npatches >= SYN_LIB_PATCHES) { snprintf(err, err_size, "patch library full"); free(p); return -1; }
    lib->patch[lib->npatches] = p;
    return lib->npatches++;
}

int syn_lib_add_patch(SynLibrary *lib, const char *json, char *err, int err_size) {
    SynPatch *tmp = (SynPatch *)malloc(sizeof(SynPatch));
    if (!tmp) return -1;
    int idx = -1;
    if (syn_patch_parse(json, tmp, lib->resolve, lib->user, err, err_size)) idx = syn_lib_put_patch(lib, tmp, err, err_size);
    free(tmp);
    return idx;
}

int syn_lib_sample_patch(SynLibrary *lib, const char *sample, bool loop) {
    char name[48];
    snprintf(name, sizeof(name), "sample:%s%s", sample, loop ? ":loop" : "");
    int idx = syn_lib_find_patch(lib, name);
    if (idx >= 0) return idx;
    int sid = lib->resolve ? lib->resolve(sample, lib->user) : -1;
    if (sid < 0) return -1;
    SynPatch *p = (SynPatch *)calloc(1, sizeof(SynPatch));
    if (!p) return -1;
    snprintf(p->name, sizeof(p->name), "%s", name);
    const char *pn[3] = { "pitch", "amp", "gate" };
    for (int i = 0; i < 3; i++) {
        snprintf(p->param_name[i], sizeof(p->param_name[i]), "%s", pn[i]);
        p->param_def[i] = 1.0f;
        p->param_lo[i] = -1e9f;
        p->param_hi[i] = 1e9f;
    }
    p->nparams = 3;
    SynNodeDef *s = &p->nodes[0];
    s->type = SN_SAMPLE;
    snprintf(s->id, sizeof(s->id), "s");
    s->sample = sid;
    s->sustain = -1;
    s->opt = loop ? 1 : 0;
    const SynNodeInfo *info = syn_node_info(SN_SAMPLE);
    for (int k = 0; k < SYN_NODE_PARAMS; k++) s->p[k] = (SynParamSrc){ SYN_SRC_CONST, 0, info->def[k], 1, 0 };
    s->p[0] = (SynParamSrc){ SYN_SRC_PARAM, 0, 1, 1, 0 };
    s->p[1] = (SynParamSrc){ SYN_SRC_PARAM, 1, 1, 1, 0 };
    p->nnodes = 1;
    p->out = 0;
    if (loop) {
        // A loop ends through its gate: a 60 ms release.
        SynNodeDef *e = &p->nodes[1];
        e->type = SN_ENV;
        snprintf(e->id, sizeof(e->id), "e");
        e->sample = -1;
        e->in[0] = 0;
        e->nin = 1;
        const SynNodeInfo *ei = syn_node_info(SN_ENV);
        for (int k = 0; k < SYN_NODE_PARAMS; k++) e->p[k] = (SynParamSrc){ SYN_SRC_CONST, 0, ei->def[k], 1, 0 };
        e->p[2] = (SynParamSrc){ SYN_SRC_PARAM, 2, 1, 1, 0 };
        e->arr[0][0] = 5, e->arr[1][0] = 1, e->arr[2][0] = 0;
        e->arr[0][1] = 60, e->arr[1][1] = 0, e->arr[2][1] = 3;
        e->narr = 2;
        e->sustain = 0;
        p->nnodes = 2;
        p->out = 1;
        p->loop = false;   // the release ends it
    }
    char err[128];
    int r = syn_lib_put_patch(lib, p, err, sizeof(err));
    free(p);
    return r;
}

static u32 name_hash(const char *s) {
    u32 h = 2166136261u;
    while (*s) h = (h ^ (u8)*s++) * 16777619u;
    return h;
}

int syn_lib_sample_variants(SynLibrary *lib, const char *sample) {
    u32 h = name_hash(sample);
    for (int i = 0; i < lib->nvar; i++) if (lib->var_hash[i] == h) return lib->var_count[i];
    int n = 0;
    while (n < SYN_MAX_VARIANTS && lib->resolve) {
        char take[40];
        snprintf(take, sizeof(take), "%s_%d", sample, n + 1);
        if (lib->resolve(take, lib->user) < 0) break;
        n++;
    }
    if (lib->nvar < SYN_LIB_VARIANTS) {
        lib->var_hash[lib->nvar] = h;
        lib->var_count[lib->nvar++] = (u8)n;
    }
    return n;
}

int syn_lib_sample_pick(SynLibrary *lib, const char *sample, u32 seed, bool loop) {
    int n = syn_lib_sample_variants(lib, sample);
    if (n <= 0) return syn_lib_sample_patch(lib, sample, loop);
    SynRng rng = syn_rng(seed ^ 0x6A09E667u);
    int k = 1 + (int)(syn_rng01(&rng) * (float)n);
    if (k > n) k = n;
    char take[40];
    snprintf(take, sizeof(take), "%s_%d", sample, k);
    return syn_lib_sample_patch(lib, take, loop);
}

int syn_recipe_input(const SynRecipe *r, const char *name) {
    for (int i = 0; i < r->ninputs; i++) if (!strcmp(r->input_name[i], name)) return i;
    return -1;
}

static void recipe_defaults(SynRecipe *r) {
    memset(r, 0, sizeof(*r));
    r->priority = 0.5f;
    r->spatial = true;
    r->env = true;
    r->max_instances = 8;
    r->min_dist = 1.5f;
    r->max_dist = 60.0f;
    r->rolloff = 1.0f;
    r->rear_lp = 20000.0f;
    r->reverb = 1.0f;
    r->stack_s = 0.03f;
}

static bool compile_expr(SynExpr *e, const cJSON *v, const SynRecipe *r, float def, char *err, int err_size) {
    if (!v) { syn_expr_const(e, def); return true; }
    if (cJSON_IsNumber(v)) { syn_expr_const(e, (float)v->valuedouble); return true; }
    if (cJSON_IsBool(v)) { syn_expr_const(e, cJSON_IsTrue(v) ? 1.0f : 0.0f); return true; }
    if (!cJSON_IsString(v)) { snprintf(err, err_size, "%s: expected a number or an expression", r->name); return false; }
    const char *names[RCP_MAX_INPUTS];
    for (int i = 0; i < r->ninputs; i++) names[i] = r->input_name[i];
    char e2[160];
    if (!syn_expr_compile(e, v->valuestring, names, r->ninputs, e2, sizeof(e2))) {
        snprintf(err, err_size, "%s: '%s': %s", r->name, v->valuestring, e2);
        return false;
    }
    return true;
}

static SynBind *bind_for(SynLayer *l, int param) {
    for (int i = 0; i < l->nbind; i++) if (l->bind[i].param == param) return &l->bind[i];
    if (l->nbind >= RCP_MAX_BINDS) return NULL;
    SynBind *b = &l->bind[l->nbind++];
    memset(b, 0, sizeof(*b));
    b->param = param;
    return b;
}

static int parse_bus(const char *s) {
    if (!strcmp(s, "voice")) return SYN_BUS_VOICE;
    if (!strcmp(s, "ui")) return SYN_BUS_UI;
    if (!strcmp(s, "ambience")) return SYN_BUS_AMBIENCE;
    return SYN_BUS_SFX;
}

int syn_lib_add_recipe(SynLibrary *lib, const char *json, char *err, int err_size) {
    cJSON *root = cJSON_Parse(json);
    if (!root) { snprintf(err, err_size, "recipe: JSON parse error"); return -1; }
    SynRecipe *r = (SynRecipe *)malloc(sizeof(SynRecipe));
    int result = -1;
    if (!r) goto done;
    recipe_defaults(r);
    const cJSON *v;
    if (cJSON_IsString(v = cJSON_GetObjectItemCaseSensitive(root, "name"))) snprintf(r->name, sizeof(r->name), "%s", v->valuestring);
    if (cJSON_IsString(v = cJSON_GetObjectItemCaseSensitive(root, "bus"))) r->bus = (u8)parse_bus(v->valuestring);
    if (r->bus == SYN_BUS_UI) r->spatial = false, r->env = false;
    if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(root, "priority"))) r->priority = (float)v->valuedouble;
    if (cJSON_IsBool(v = cJSON_GetObjectItemCaseSensitive(root, "loop"))) r->loop = cJSON_IsTrue(v);
    if (cJSON_IsBool(v = cJSON_GetObjectItemCaseSensitive(root, "echo"))) r->echo = cJSON_IsTrue(v);
    if (cJSON_IsBool(v = cJSON_GetObjectItemCaseSensitive(root, "doppler"))) r->doppler = cJSON_IsTrue(v);
    if (cJSON_IsBool(v = cJSON_GetObjectItemCaseSensitive(root, "spatial"))) r->spatial = cJSON_IsTrue(v);
    if (cJSON_IsBool(v = cJSON_GetObjectItemCaseSensitive(root, "env"))) r->env = cJSON_IsTrue(v);
    if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(root, "max_instances"))) r->max_instances = v->valueint;
    if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(root, "reverb"))) r->reverb = (float)v->valuedouble;
    if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(root, "stack_s"))) r->stack_s = (float)v->valuedouble;
    const cJSON *dist = cJSON_GetObjectItemCaseSensitive(root, "distance");
    if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(dist, "min"))) r->min_dist = (float)v->valuedouble;
    if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(dist, "max"))) r->max_dist = (float)v->valuedouble;
    if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(dist, "rolloff"))) r->rolloff = (float)v->valuedouble;
    const cJSON *dir = cJSON_GetObjectItemCaseSensitive(root, "directivity");
    if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(dir, "front_db"))) r->front_db = (float)v->valuedouble;
    if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(dir, "rear_db"))) r->rear_db = (float)v->valuedouble;
    if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(dir, "rear_lp"))) r->rear_lp = (float)v->valuedouble;
    const cJSON *in;
    cJSON_ArrayForEach(in, cJSON_GetObjectItemCaseSensitive(root, "inputs")) {
        if (r->ninputs >= RCP_MAX_INPUTS) { snprintf(err, err_size, "%s: more than %d inputs", r->name, RCP_MAX_INPUTS); goto done; }
        snprintf(r->input_name[r->ninputs], sizeof(r->input_name[0]), "%s", in->string);
        r->input_def[r->ninputs++] = cJSON_IsNumber(in) ? (float)in->valuedouble : 0.0f;
    }
    // Every recipe can read where it is heard from (the env tick fills these when declared).
    const char *always[] = { "distance", "seed01" };
    for (int i = 0; i < 2; i++)
        if (syn_recipe_input(r, always[i]) < 0 && r->ninputs < RCP_MAX_INPUTS) {
            snprintf(r->input_name[r->ninputs], sizeof(r->input_name[0]), "%s", always[i]);
            r->input_def[r->ninputs++] = 0.0f;
        }
    const cJSON *ly;
    cJSON_ArrayForEach(ly, cJSON_GetObjectItemCaseSensitive(root, "layers")) {
        if (r->nlayers >= RCP_MAX_LAYERS) { snprintf(err, err_size, "%s: more than %d layers", r->name, RCP_MAX_LAYERS); goto done; }
        SynLayer *l = &r->layer[r->nlayers];
        memset(l, 0, sizeof(*l));
        const cJSON *pn = cJSON_GetObjectItemCaseSensitive(ly, "patch"), *sn = cJSON_GetObjectItemCaseSensitive(ly, "sample");
        if (cJSON_IsString(pn)) l->patch = syn_lib_find_patch(lib, pn->valuestring);
        else if (cJSON_IsString(sn) && sn->valuestring[0] == '@') {
            l->dyn = true;
            snprintf(l->dyn_suffix, sizeof(l->dyn_suffix), "%s", sn->valuestring + 1);
            l->patch = -1;
        } else if (cJSON_IsString(sn) || cJSON_IsArray(sn)) {
            const cJSON *it;
            bool any = false;
            if (cJSON_IsString(sn)) snprintf(l->sample[l->nsamples++], sizeof(l->sample[0]), "%s", sn->valuestring);
            else cJSON_ArrayForEach(it, sn) {
                if (!cJSON_IsString(it) || l->nsamples >= RCP_MAX_SAMPLES) { snprintf(err, err_size, "%s: bad sample list", r->name); goto done; }
                snprintf(l->sample[l->nsamples++], sizeof(l->sample[0]), "%s", it->valuestring);
            }
            for (int k = 0; k < l->nsamples; k++) any |= syn_lib_sample_pick(lib, l->sample[k], 0, r->loop) >= 0;
            if (!any) continue;   // a missing sample layer is dropped (the synth layers carry the sound)
            const cJSON *sel = cJSON_GetObjectItemCaseSensitive(ly, "sample_sel");
            l->has_sel = sel != NULL;
            if (!compile_expr(&l->sel, sel, r, 0.0f, err, err_size)) goto done;
            l->patch = -1;
        } else l->patch = -1;
        if (l->patch < 0 && !l->dyn && !l->nsamples) {
            snprintf(err, err_size, "%s: layer %d: no patch '%s'", r->name, r->nlayers, cJSON_IsString(pn) ? pn->valuestring : "?");
            goto done;
        }
        // Dynamic sample layers bind against the generic sample patch (pitch, amp, gate).
        static const SynPatch k_sample_params = { .nparams = 3, .param_name = { "pitch", "amp", "gate" }, .param_def = { 1, 1, 1 } };
        const SynPatch *p = l->dyn || l->nsamples ? &k_sample_params : lib->patch[l->patch];
        l->optional = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(ly, "optional"));
        const cJSON *w = cJSON_GetObjectItemCaseSensitive(ly, "when");
        l->has_when = w != NULL;
        if (!compile_expr(&l->when, w, r, 1.0f, err, err_size)) goto done;
        if (!compile_expr(&l->gain_db, cJSON_GetObjectItemCaseSensitive(ly, "gain_db"), r, 0.0f, err, err_size)) goto done;
        const cJSON *b;
        cJSON_ArrayForEach(b, cJSON_GetObjectItemCaseSensitive(ly, "bind")) {
            int pi = syn_patch_param(p, b->string);
            if (pi < 0) { snprintf(err, err_size, "%s: patch %s has no parameter '%s'", r->name, p->name, b->string); goto done; }
            SynBind *sb = bind_for(l, pi);
            if (!sb) { snprintf(err, err_size, "%s: too many bindings", r->name); goto done; }
            sb->has_expr = true;
            if (!compile_expr(&sb->expr, b, r, 0.0f, err, err_size)) goto done;
        }
        cJSON_ArrayForEach(b, cJSON_GetObjectItemCaseSensitive(ly, "random")) {
            int pi = syn_patch_param(p, b->string);
            if (pi < 0 || cJSON_GetArraySize(b) != 3) { snprintf(err, err_size, "%s: bad random '%s'", r->name, b->string); goto done; }
            SynBind *sb = bind_for(l, pi);
            if (!sb) { snprintf(err, err_size, "%s: too many bindings", r->name); goto done; }
            const char *k = cJSON_GetArrayItem(b, 0)->valuestring;
            sb->rand = !k ? SYN_RAND_NONE : !strcmp(k, "db") ? SYN_RAND_DB : !strcmp(k, "mul") ? SYN_RAND_MUL : SYN_RAND_ADD;
            sb->rlo = (float)cJSON_GetArrayItem(b, 1)->valuedouble;
            sb->rhi = (float)cJSON_GetArrayItem(b, 2)->valuedouble;
        }
        const cJSON *pitch = cJSON_GetObjectItemCaseSensitive(ly, "pitch");
        int pp = syn_patch_param(p, "pitch");
        if (cJSON_IsArray(pitch) && pp >= 0) {
            SynBind *sb = bind_for(l, pp);
            if (sb) {
                sb->rand = SYN_RAND_MUL;
                sb->rlo = (float)cJSON_GetArrayItem(pitch, 0)->valuedouble;
                sb->rhi = (float)cJSON_GetArrayItem(pitch, 1)->valuedouble;
            }
        }
        r->nlayers++;
    }
    if (!r->nlayers) { snprintf(err, err_size, "%s: no playable layer", r->name); goto done; }
    int idx = syn_lib_find_recipe(lib, r->name);
    if (idx >= 0) {
        // Recipes are read on the game thread only: replace in place.
        *lib->recipe[idx] = *r;
        result = idx;
    } else if (lib->nrecipes < SYN_LIB_RECIPES) {
        lib->recipe[lib->nrecipes] = r;
        r = NULL;
        result = lib->nrecipes++;
    } else snprintf(err, err_size, "recipe library full");
done:
    free(r);
    cJSON_Delete(root);
    return result;
}

int syn_lib_sample_recipe(SynLibrary *lib, const char *sample, bool loop, bool spatial, int bus) {
    char name[32];
    snprintf(name, sizeof(name), "~%s%s%s", sample, loop ? ":l" : "", spatial ? "" : ":2d");
    int idx = syn_lib_find_recipe(lib, name);
    if (idx >= 0) return idx;
    int pi = syn_lib_sample_pick(lib, sample, 0, loop);
    if (pi < 0 || lib->nrecipes >= SYN_LIB_RECIPES) return -1;
    SynRecipe *r = (SynRecipe *)malloc(sizeof(SynRecipe));
    if (!r) return -1;
    recipe_defaults(r);
    snprintf(r->name, sizeof(r->name), "%s", name);
    r->loop = loop;
    r->spatial = spatial;
    r->env = spatial;
    r->bus = (u8)bus;
    r->max_instances = 6;
    snprintf(r->input_name[0], sizeof(r->input_name[0]), "distance");
    snprintf(r->input_name[1], sizeof(r->input_name[1]), "seed01");
    r->ninputs = 2;
    r->nlayers = 1;
    r->layer[0].patch = -1;   // picked per voice: a take when the sample has some
    r->layer[0].nsamples = 1;
    snprintf(r->layer[0].sample[0], sizeof(r->layer[0].sample[0]), "%s", sample);
    syn_expr_const(&r->layer[0].when, 1.0f);
    syn_expr_const(&r->layer[0].gain_db, 0.0f);
    lib->recipe[lib->nrecipes] = r;
    return lib->nrecipes++;
}

void syn_recipe_eval(SynLibrary *lib, const SynRecipe *r, const float *inputs, u32 seed, const char *sample, SynRecipeOut *out) {
    memset(out, 0, sizeof(*out));
    out->nlayers = r->nlayers;
    for (int li = 0; li < r->nlayers; li++) {
        const SynLayer *l = &r->layer[li];
        SynLayerOut *o = &out->layer[li];
        int pi = l->patch;
        if (l->dyn || l->nsamples) {
            char name[48];
            if (l->dyn) {
                if (!sample || !sample[0]) continue;
                snprintf(name, sizeof(name), "%s%s", sample, l->dyn_suffix);
            } else {
                int k = l->has_sel ? (int)floorf(syn_expr_eval(&l->sel, inputs) + 0.5f) : 0;
                k = k < 0 ? 0 : k >= l->nsamples ? l->nsamples - 1 : k;
                snprintf(name, sizeof(name), "%s", l->sample[k]);
            }
            // The take: one draw per (seed, layer), so a re-evaluation keeps it.
            pi = syn_lib_sample_pick(lib, name, seed ^ ((u32)li * 0x9E3779B9u), r->loop);
        }
        if (pi < 0 || pi >= lib->npatches) continue;
        const SynPatch *p = lib->patch[pi];
        o->patch = p;
        o->optional = l->optional;
        o->on = !l->has_when || syn_expr_eval(&l->when, inputs) > 0.5f;
        o->gain = syn_db(syn_expr_eval(&l->gain_db, inputs));
        for (int i = 0; i < p->nparams; i++) o->params[i] = p->param_def[i];
        for (int bi = 0; bi < l->nbind; bi++) {
            const SynBind *b = &l->bind[bi];
            float x = b->has_expr ? syn_expr_eval(&b->expr, inputs) : p->param_def[b->param];
            if (b->rand) {
                // One draw per (seed, layer, parameter): stable across re-evaluations.
                SynRng rng = syn_rng(seed ^ ((u32)li * 0x45D9F3Bu) ^ ((u32)b->param * 0x27D4EB2Fu));
                float u = b->rlo + (b->rhi - b->rlo) * syn_rng01(&rng);
                x = b->rand == SYN_RAND_DB ? x * syn_db(u) : b->rand == SYN_RAND_MUL ? x * u : x + u;
            }
            if (x < p->param_lo[b->param]) x = p->param_lo[b->param];
            if (x > p->param_hi[b->param]) x = p->param_hi[b->param];
            o->params[b->param] = x;
        }
    }
}
