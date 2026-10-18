#include "syn_patch.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *k_opt_keys[] = { "wave", "color", "mode", "src", "play", "lfo" };

static bool parse_value(SynPatch *p, const cJSON *v, SynParamSrc *dst, const char **pending_mod, char *err, int err_size) {
    dst->scale = 1.0f;
    dst->offset = 0.0f;
    if (cJSON_IsNumber(v)) {
        dst->kind = SYN_SRC_CONST;
        dst->value = (float)v->valuedouble;
        return true;
    }
    if (cJSON_IsString(v) && v->valuestring[0] == '$') {
        // "$name", "$name*k", "$name*k+o", "$name+o"
        char name[32];
        const char *s = v->valuestring + 1;
        int n = 0;
        while (*s && *s != '*' && *s != '+' && *s != '-' && n < 31) name[n++] = *s++;
        name[n] = 0;
        int idx = syn_patch_param(p, name);
        if (idx < 0) { snprintf(err, err_size, "%s: unknown parameter '%s'", p->name, name); return false; }
        if (*s == '*') dst->scale = strtof(s + 1, (char **)&s);
        if (*s == '+' || *s == '-') dst->offset = strtof(s, NULL);
        dst->kind = SYN_SRC_PARAM;
        dst->index = (u8)idx;
        dst->value = p->param_def[idx] * dst->scale + dst->offset;
        return true;
    }
    if (cJSON_IsObject(v)) {
        const cJSON *sc = cJSON_GetObjectItemCaseSensitive(v, "scale"), *of = cJSON_GetObjectItemCaseSensitive(v, "offset");
        if (cJSON_IsNumber(sc)) dst->scale = (float)sc->valuedouble;
        if (cJSON_IsNumber(of)) dst->offset = (float)of->valuedouble;
        const cJSON *pr = cJSON_GetObjectItemCaseSensitive(v, "param"), *md = cJSON_GetObjectItemCaseSensitive(v, "mod");
        if (cJSON_IsString(pr)) {
            int idx = syn_patch_param(p, pr->valuestring);
            if (idx < 0) { snprintf(err, err_size, "%s: unknown parameter '%s'", p->name, pr->valuestring); return false; }
            dst->kind = SYN_SRC_PARAM;
            dst->index = (u8)idx;
            return true;
        }
        if (cJSON_IsString(md)) {
            dst->kind = SYN_SRC_MOD;
            *pending_mod = md->valuestring;   // resolved once every node id is known
            return true;
        }
    }
    snprintf(err, err_size, "%s: bad parameter value", p->name);
    return false;
}

static void read_rows(const cJSON *a, SynNodeDef *d, int cols, const float *defaults) {
    d->narr = 0;
    const cJSON *row;
    cJSON_ArrayForEach(row, a) {
        if (d->narr >= SYN_MAX_ARR) break;
        for (int c = 0; c < 3; c++) d->arr[c][d->narr] = c < cols ? defaults[c] : 0.0f;
        if (cJSON_IsNumber(row)) d->arr[0][d->narr] = (float)row->valuedouble;
        for (int c = 0; c < cols && cJSON_IsArray(row); c++) {
            const cJSON *x = cJSON_GetArrayItem(row, c);
            if (cJSON_IsNumber(x)) d->arr[c][d->narr] = (float)x->valuedouble;
        }
        d->narr++;
    }
}

bool syn_patch_parse(const char *json, SynPatch *p, SynSampleResolveFn resolve, void *user, char *err, int err_size) {
    memset(p, 0, sizeof(*p));
    p->out = -1;
    cJSON *root = cJSON_Parse(json);
    if (!root) { snprintf(err, err_size, "patch: JSON parse error"); return false; }
    bool ok = false;
    const cJSON *v;
    if (cJSON_IsString(v = cJSON_GetObjectItemCaseSensitive(root, "name"))) snprintf(p->name, sizeof(p->name), "%s", v->valuestring);
    p->loop = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "loop"));
    if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(root, "max_dur"))) p->max_dur = (float)v->valuedouble;
    if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(root, "tau"))) p->param_tau = (float)v->valuedouble;
    const cJSON *pr;
    cJSON_ArrayForEach(pr, cJSON_GetObjectItemCaseSensitive(root, "params")) {
        if (p->nparams >= SYN_MAX_PARAMS) { snprintf(err, err_size, "%s: more than %d parameters", p->name, SYN_MAX_PARAMS); goto done; }
        int i = p->nparams++;
        snprintf(p->param_name[i], sizeof(p->param_name[i]), "%s", pr->string);
        float def = 0, lo = -1e9f, hi = 1e9f;
        if (cJSON_IsNumber(pr)) def = (float)pr->valuedouble;
        else if (cJSON_IsArray(pr)) {
            def = (float)cJSON_GetArrayItem(pr, 0)->valuedouble;
            if (cJSON_GetArraySize(pr) >= 3) { lo = (float)cJSON_GetArrayItem(pr, 1)->valuedouble; hi = (float)cJSON_GetArrayItem(pr, 2)->valuedouble; }
        }
        p->param_def[i] = def, p->param_lo[i] = lo, p->param_hi[i] = hi;
    }
    const cJSON *nodes = cJSON_GetObjectItemCaseSensitive(root, "nodes");
    const char *pending[SYN_MAX_NODES][SYN_NODE_PARAMS];
    const cJSON *inputs[SYN_MAX_NODES];
    memset(pending, 0, sizeof(pending));
    const cJSON *nd;
    cJSON_ArrayForEach(nd, nodes) {
        if (p->nnodes >= SYN_MAX_NODES) { snprintf(err, err_size, "%s: more than %d nodes", p->name, SYN_MAX_NODES); goto done; }
        int ni = p->nnodes++;
        SynNodeDef *d = &p->nodes[ni];
        d->sustain = -1;
        d->sample = -1;
        const cJSON *ty = cJSON_GetObjectItemCaseSensitive(nd, "type"), *id = cJSON_GetObjectItemCaseSensitive(nd, "id");
        int t = cJSON_IsString(ty) ? syn_node_type(ty->valuestring) : -1;
        if (t < 0) { snprintf(err, err_size, "%s: node %d: unknown type '%s'", p->name, ni, cJSON_IsString(ty) ? ty->valuestring : "?"); goto done; }
        d->type = (u8)t;
        if (cJSON_IsString(id)) snprintf(d->id, sizeof(d->id), "%s", id->valuestring);
        else snprintf(d->id, sizeof(d->id), "n%d", ni);
        const SynNodeInfo *info = syn_node_info(t);
        for (int k = 0; k < SYN_NODE_PARAMS; k++) {
            d->p[k].kind = SYN_SRC_CONST;
            d->p[k].value = info->def[k];
            d->p[k].scale = 1.0f;
        }
        inputs[ni] = cJSON_GetObjectItemCaseSensitive(nd, "in");
        const cJSON *it;
        cJSON_ArrayForEach(it, nd) {
            const char *key = it->string;
            if (!strcmp(key, "type") || !strcmp(key, "id") || !strcmp(key, "in") || key[0] == '_') continue;
            bool opt_key = false;
            for (int o = 0; o < (int)(sizeof(k_opt_keys) / sizeof(k_opt_keys[0])); o++) opt_key |= !strcmp(key, k_opt_keys[o]);
            if (opt_key) {
                int o = cJSON_IsString(it) ? syn_node_opt(t, it->valuestring) : -1;
                if (o < 0) { snprintf(err, err_size, "%s: node '%s': bad %s", p->name, d->id, key); goto done; }
                d->opt = (u8)o;
            } else if (!strcmp(key, "stages")) {
                static const float def3[3] = { 10, 0, 0 };
                read_rows(it, d, 3, def3);
            } else if (!strcmp(key, "adsr") && cJSON_GetArraySize(it) == 4) {
                float a = (float)cJSON_GetArrayItem(it, 0)->valuedouble, dd = (float)cJSON_GetArrayItem(it, 1)->valuedouble;
                float s = (float)cJSON_GetArrayItem(it, 2)->valuedouble, r = (float)cJSON_GetArrayItem(it, 3)->valuedouble;
                const float rows[3][3] = { { a, 1, 0 }, { dd, s, 3 }, { r, 0, 4 } };
                for (int k = 0; k < 3; k++) for (int c = 0; c < 3; c++) d->arr[c][k] = rows[k][c];
                d->narr = 3;
                d->sustain = 1;
            } else if (!strcmp(key, "sustain") && cJSON_IsNumber(it)) {
                d->sustain = (i8)it->valueint;
            } else if (!strcmp(key, "modes")) {
                static const float def3[3] = { 1000, 0.2f, 1 };
                read_rows(it, d, 3, def3);
            } else if (!strcmp(key, "gains")) {
                static const float def1[3] = { 1, 0, 0 };
                read_rows(it, d, 1, def1);
            } else if (!strcmp(key, "sample") && cJSON_IsString(it)) {
                d->sample = resolve ? resolve(it->valuestring, user) : -1;
                if (d->sample < 0 && resolve) { snprintf(err, err_size, "%s: node '%s': no sample '%s'", p->name, d->id, it->valuestring); goto done; }
            } else if (!strcmp(key, "max_ms") && cJSON_IsNumber(it)) {
                d->max_ms = (float)it->valuedouble;
            } else {
                int slot = syn_param_slot(t, key);
                if (slot < 0) { snprintf(err, err_size, "%s: node '%s' (%s) has no '%s'", p->name, d->id, info->name, key); goto done; }
                if (!parse_value(p, it, &d->p[slot], &pending[ni][slot], err, err_size)) goto done;
            }
        }
    }
    // Second pass: inputs and modulation sources by id.
    for (int ni = 0; ni < p->nnodes; ni++) {
        SynNodeDef *d = &p->nodes[ni];
        const cJSON *in = inputs[ni];
        const cJSON *one;
        if (cJSON_IsString(in)) {
            int j = syn_patch_node(p, in->valuestring);
            if (j < 0) { snprintf(err, err_size, "%s: node '%s': no input '%s'", p->name, d->id, in->valuestring); goto done; }
            d->in[d->nin++] = (i8)j;
        } else cJSON_ArrayForEach(one, in) {
            int j = cJSON_IsString(one) ? syn_patch_node(p, one->valuestring) : -1;
            if (j < 0 || d->nin >= SYN_MAX_INPUTS) { snprintf(err, err_size, "%s: node '%s': bad input", p->name, d->id); goto done; }
            d->in[d->nin++] = (i8)j;
        }
        for (int k = 0; k < SYN_NODE_PARAMS; k++) {
            if (!pending[ni][k]) continue;
            int j = syn_patch_node(p, pending[ni][k]);
            if (j < 0) { snprintf(err, err_size, "%s: node '%s': no modulator '%s'", p->name, d->id, pending[ni][k]); goto done; }
            d->p[k].index = (u8)j;
        }
    }
    if (cJSON_IsString(v = cJSON_GetObjectItemCaseSensitive(root, "out"))) {
        p->out = syn_patch_node(p, v->valuestring);
        if (p->out < 0) { snprintf(err, err_size, "%s: no out node '%s'", p->name, v->valuestring); goto done; }
    }
    ok = true;
done:
    cJSON_Delete(root);
    return ok;
}

static cJSON *value_json(const SynPatch *p, const SynParamSrc *s) {
    char buf[64];
    switch (s->kind) {
    case SYN_SRC_PARAM:
        if (s->scale == 1.0f && s->offset == 0.0f) snprintf(buf, sizeof(buf), "$%s", p->param_name[s->index]);
        else if (s->offset == 0.0f) snprintf(buf, sizeof(buf), "$%s*%g", p->param_name[s->index], s->scale);
        else snprintf(buf, sizeof(buf), "$%s*%g%+g", p->param_name[s->index], s->scale, s->offset);
        return cJSON_CreateString(buf);
    case SYN_SRC_MOD: {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "mod", p->nodes[s->index].id);
        cJSON_AddNumberToObject(o, "scale", s->scale);
        cJSON_AddNumberToObject(o, "offset", s->offset);
        return o;
    }
    default: return cJSON_CreateNumber(s->value);
    }
}

static const char *opt_name(int type, int opt, char *buf, size_t n) {
    const SynNodeInfo *in = syn_node_info(type);
    const char *s = in->opts;
    for (int i = 0; s && i < opt; i++) {
        s = strchr(s, '|');
        if (s) s++;
    }
    if (!s) return NULL;
    size_t len = strcspn(s, "|");
    if (len >= n) len = n - 1;
    memcpy(buf, s, len);
    buf[len] = 0;
    return buf;
}

char *syn_patch_write(const SynPatch *p, const char *const *sample_names, int sample_count) {
    static const char *k_opt_key_of[SN_COUNT] = {
        [SN_OSC] = "wave", [SN_NOISE] = "color", [SN_GRAINS] = "src", [SN_BIQUAD] = "mode", [SN_SVF] = "mode",
        [SN_ONEPOLE] = "mode", [SN_SHAPER] = "mode", [SN_SAMPLE] = "play", [SN_AM] = "lfo",
    };
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", p->name);
    if (p->loop) cJSON_AddBoolToObject(root, "loop", 1);
    if (p->max_dur > 0) cJSON_AddNumberToObject(root, "max_dur", p->max_dur);
    cJSON *params = cJSON_AddObjectToObject(root, "params");
    for (int i = 0; i < p->nparams; i++) {
        if (p->param_lo[i] > -1e8f) {
            cJSON *a = cJSON_CreateArray();
            cJSON_AddItemToArray(a, cJSON_CreateNumber(p->param_def[i]));
            cJSON_AddItemToArray(a, cJSON_CreateNumber(p->param_lo[i]));
            cJSON_AddItemToArray(a, cJSON_CreateNumber(p->param_hi[i]));
            cJSON_AddItemToObject(params, p->param_name[i], a);
        } else cJSON_AddNumberToObject(params, p->param_name[i], p->param_def[i]);
    }
    cJSON *nodes = cJSON_AddArrayToObject(root, "nodes");
    for (int i = 0; i < p->nnodes; i++) {
        const SynNodeDef *d = &p->nodes[i];
        const SynNodeInfo *info = syn_node_info(d->type);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", d->id);
        cJSON_AddStringToObject(o, "type", info->name);
        char ob[24];
        if (k_opt_key_of[d->type] && opt_name(d->type, d->opt, ob, sizeof(ob))) cJSON_AddStringToObject(o, k_opt_key_of[d->type], ob);
        if (d->nin == 1) cJSON_AddStringToObject(o, "in", p->nodes[d->in[0]].id);
        else if (d->nin > 1) {
            cJSON *a = cJSON_AddArrayToObject(o, "in");
            for (int k = 0; k < d->nin; k++) cJSON_AddItemToArray(a, cJSON_CreateString(p->nodes[d->in[k]].id));
        }
        for (int k = 0; k < SYN_NODE_PARAMS; k++) {
            if (!info->param[k]) continue;
            const SynParamSrc *s = &d->p[k];
            if (s->kind == SYN_SRC_CONST && s->value == info->def[k]) continue;
            cJSON_AddItemToObject(o, info->param[k], value_json(p, s));
        }
        if (d->narr && (d->type == SN_ENV || d->type == SN_MODAL || d->type == SN_MIX)) {
            int cols = d->type == SN_MIX ? 1 : 3;
            cJSON *a = cJSON_AddArrayToObject(o, d->type == SN_ENV ? "stages" : d->type == SN_MODAL ? "modes" : "gains");
            for (int r = 0; r < d->narr; r++) {
                if (cols == 1) { cJSON_AddItemToArray(a, cJSON_CreateNumber(d->arr[0][r])); continue; }
                cJSON *row = cJSON_CreateArray();
                for (int c = 0; c < cols; c++) cJSON_AddItemToArray(row, cJSON_CreateNumber(d->arr[c][r]));
                cJSON_AddItemToArray(a, row);
            }
            if (d->type == SN_ENV && d->sustain >= 0) cJSON_AddNumberToObject(o, "sustain", d->sustain);
        }
        if (d->sample >= 0 && d->sample < sample_count && sample_names) cJSON_AddStringToObject(o, "sample", sample_names[d->sample]);
        if (d->max_ms > 0) cJSON_AddNumberToObject(o, "max_ms", d->max_ms);
        cJSON_AddItemToArray(nodes, o);
    }
    if (p->out >= 0) cJSON_AddStringToObject(root, "out", p->nodes[p->out].id);
    char *s = cJSON_Print(root);
    cJSON_Delete(root);
    return s;
}
