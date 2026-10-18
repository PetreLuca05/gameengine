// Vista sequences: parse + evaluate (docs/STORY.md "Vista"). Pure; see vista_seq.h.
#include "story/vista_seq.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static float jnum(const cJSON *o, const char *k, float def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}

static void jstr(char *dst, int n, const cJSON *o, const char *k) {
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, k));
    snprintf(dst, (size_t)n, "%s", s ? s : "");
}

static void jpair(const cJSON *o, const char *k, float *a, float *b) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_GetArraySize(v) >= 2) {
        *a = (float)cJSON_GetArrayItem(v, 0)->valuedouble;
        *b = (float)cJSON_GetArrayItem(v, 1)->valuedouble;
    }
}

static void parse_spots(const cJSON *arr, VistaSpotDef *out, int *n, float def_time, float def_size) {
    const cJSON *e;
    cJSON_ArrayForEach(e, arr) {
        if (*n >= VISTA_MAX_ITEMS) break;
        VistaSpotDef *s = &out[(*n)++];
        jstr(s->at, VISTA_NAME, e, "at");
        s->t = jnum(e, "t", 0);
        s->time = jnum(e, "time", def_time);
        s->size = jnum(e, "size", def_size);
    }
}

bool vista_seq_parse(VistaSeqLib *lib, const char *json, char *err, int err_size) {
    memset(lib, 0, sizeof(*lib));
    cJSON *root = json ? cJSON_Parse(json) : NULL;
    if (!cJSON_IsObject(root)) {
        snprintf(err, (size_t)err_size, "not a JSON object");
        cJSON_Delete(root);
        return false;
    }
    const cJSON *q;
    cJSON_ArrayForEach(q, root) {
        if (q->string[0] == '_' || !cJSON_IsObject(q) || lib->n >= VISTA_MAX_SEQ) continue;
        VistaSeqDef *d = &lib->seq[lib->n++];
        snprintf(d->name, sizeof(d->name), "%s", q->string);
        const cJSON *e;
        cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(q, "power")) {
            if (d->power_n >= VISTA_MAX_POWER) break;
            VistaPowerKey *k = &d->power[d->power_n++];
            k->t = jnum(e, "t", 0);
            k->sector = (int)jnum(e, "sector", 0) & 7;
            k->to = jnum(e, "to", 1);
            k->fade = jnum(e, "fade", 0);
        }
        const cJSON *tr = cJSON_GetObjectItemCaseSensitive(q, "traffic");
        if (tr) {
            d->traffic = true;
            d->density = jnum(tr, "density", -1);
            d->stall = jnum(tr, "stall", -1);
            d->dark = jnum(tr, "dark", -1);
            d->ramp = jnum(tr, "ramp", 0);
        }
        const cJSON *h = cJSON_GetObjectItemCaseSensitive(q, "heli");
        if (h) {
            d->heli = true;
            const cJSON *path = cJSON_GetObjectItemCaseSensitive(h, "path");
            if (cJSON_GetArraySize(path) >= 2) {
                snprintf(d->heli_from, VISTA_NAME, "%s", cJSON_GetStringValue(cJSON_GetArrayItem(path, 0)));
                snprintf(d->heli_to, VISTA_NAME, "%s", cJSON_GetStringValue(cJSON_GetArrayItem(path, 1)));
            } else {
                jstr(d->heli_from, VISTA_NAME, h, "from");
                jstr(d->heli_to, VISTA_NAME, h, "to");
            }
            d->heli_time = jnum(h, "duration", 30);
            d->approach = jnum(h, "approach", 250);
            d->crash = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(h, "crash"));
            d->land = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(h, "land"));
        }
        cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(q, "smoke")) {
            if (d->smoke_n >= VISTA_MAX_ITEMS) break;
            VistaSmokeDef *s = &d->smoke[d->smoke_n++];
            jstr(s->at, VISTA_NAME, e, "at");
            s->t = jnum(e, "t", 0);
            s->grow = jnum(e, "grow", 30);
            s->rise = jnum(e, "rise", 3);
            s->life = jnum(e, "life", 60);
            s->size0 = 10;
            s->size1 = 60;
            jpair(e, "size", &s->size0, &s->size1);
            s->rate = jnum(e, "rate", 1.5f);
            s->shade0 = 0.1f;
            s->shade1 = 0.35f;
            jpair(e, "shade", &s->shade0, &s->shade1);
            s->spread = jnum(e, "spread", 8);
            s->drift[0] = 2.0f;
            s->drift[1] = -0.8f;
            jpair(e, "drift", &s->drift[0], &s->drift[1]);
            s->prewarm = jnum(e, "prewarm", 0);
        }
        parse_spots(cJSON_GetObjectItemCaseSensitive(q, "fire"), d->fire, &d->fire_n, 0, 14);
        parse_spots(cJSON_GetObjectItemCaseSensitive(q, "flash"), d->flash, &d->flash_n, 2.5f, 50);
        const cJSON *b = cJSON_GetObjectItemCaseSensitive(q, "birds");
        d->birds_scatter = b && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(b, "scatter"));
        d->birds_gone = b && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(b, "gone"));
        d->day = (int)jnum(q, "day", 0);
        d->crowd = (int)jnum(cJSON_GetObjectItemCaseSensitive(q, "crowd"), "count", -1);
    }
    cJSON_Delete(root);
    return true;
}

const VistaSeqDef *vista_seq_find(const VistaSeqLib *lib, const char *name) {
    for (int i = 0; i < lib->n; i++)
        if (!strcmp(lib->seq[i].name, name)) return &lib->seq[i];
    return NULL;
}

static float clamp01(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }

static void heli_eval(const VistaSeqDef *d, float age, VistaPointFn point, void *user, VistaFrame *out) {
    float a[3], b[3];
    if (out->heli_n >= 4 || !point(user, d->heli_from, a) || !point(user, d->heli_to, b)) return;
    float u = age / (d->heli_time > 0.01f ? d->heli_time : 0.01f);
    if (u < 0 || (u >= 1 && !d->land)) return;
    VistaHeli *h = &out->heli[out->heli_n++];
    memset(h, 0, sizeof(*h));
    h->rotor = age * 40.0f;
    if (d->land) {
        // In high over the first three quarters (40 m above the pad), then down onto it; it stays.
        float above = 40.0f, v = u < 0.75f ? u / 0.75f : 1.0f, dn = u < 0.75f ? 0.0f : (u >= 1.0f ? 1.0f : (u - 0.75f) / 0.25f);
        float e = v * v * (3.0f - 2.0f * v);
        for (int i = 0; i < 3; i++) h->pos[i] = a[i] + (b[i] - a[i]) * e;
        h->pos[1] = a[1] + (b[1] + above - a[1]) * e - above * dn * dn * (3.0f - 2.0f * dn);
        h->yaw = atan2f(-(b[0] - a[0]), -(b[2] - a[2]));
        h->roll = u < 0.75f ? 0.06f : 0.0f;
        return;
    }
    if (!d->crash) {
        for (int i = 0; i < 3; i++) h->pos[i] = a[i] + (b[i] - a[i]) * u;
        h->yaw = atan2f(-(b[0] - a[0]), -(b[2] - a[2]));   // engine yaw: 0 looks -Z
        h->roll = 0.08f;
        return;
    }
    // Crash: in from `from`'s side at `approach` m, a tightening spiral down onto `to`, spinning.
    float dx = a[0] - b[0], dz = a[2] - b[2], L = sqrtf(dx * dx + dz * dz);
    if (L < 1) L = 1, dx = 1, dz = 0;
    dx /= L, dz /= L;
    float r = d->approach * (1.0f - u), ang = 3.5f * u * u;
    float cx = dx * cosf(ang) - dz * sinf(ang), cz = dx * sinf(ang) + dz * cosf(ang);
    h->pos[0] = b[0] + cx * r;
    h->pos[2] = b[2] + cz * r;
    h->pos[1] = b[1] + 70.0f * (1.0f - u * u);
    h->yaw = atan2f(cx, cz) + 9.0f * u * u * u;   // the tail rotor gone: the body spins up
    h->roll = 0.15f + 0.5f * u;
    h->crashing = true;
}

void vista_eval(const VistaSeqLib *lib, const VistaActive *act, int n, VistaPointFn point, void *user, VistaFrame *out) {
    memset(out, 0, sizeof(*out));
    for (int s = 0; s < 8; s++) out->power[s] = 1.0f;
    out->traffic_density = 0.25f;
    out->stall_age = -1;
    out->birds_scatter_age = -1;
    out->crowd = 0;
    for (int i = 0; i < n; i++) {
        const VistaSeqDef *d = vista_seq_find(lib, act[i].name);
        float age = act[i].age;
        if (!d || age < 0) continue;
        // Power: keys in order; each moves its sector from the value before it to `to` over `fade`.
        for (int k = 0; k < d->power_n; k++) {
            const VistaPowerKey *p = &d->power[k];
            if (age < p->t) continue;
            float f = p->fade > 0 ? clamp01((age - p->t) / p->fade) : 1.0f;
            float from = out->power[p->sector];
            if (from > 1.0f && p->to <= 1.0f) from = 1.0f;   // a flickering district fades from on
            out->power[p->sector] = f >= 1.0f ? p->to : from + (p->to - from) * f;
        }
        if (d->traffic) {
            float f = d->ramp > 0 ? clamp01(age / d->ramp) : 1.0f;
            if (d->density >= 0) out->traffic_density += (d->density - out->traffic_density) * f;
            if (d->stall >= 0) {
                out->traffic_stall += (d->stall - out->traffic_stall) * f;
                out->stall_age = age;
            }
            if (d->dark >= 0) out->traffic_dark += (d->dark - out->traffic_dark) * f;
        }
        if (d->heli) heli_eval(d, age, point, user, out);
        for (int k = 0; k < d->smoke_n && out->smoke_n < 16; k++) {
            const VistaSmokeDef *sd = &d->smoke[k];
            VistaSmoke *sm = &out->smoke[out->smoke_n];
            if (age < sd->t || !point(user, sd->at, sm->pos)) continue;
            sm->age = age - sd->t + sd->prewarm;
            sm->def = sd;
            out->smoke_n++;
        }
        for (int k = 0; k < d->fire_n && out->fire_n < 16; k++) {
            VistaSpot *f = &out->fire[out->fire_n];
            if (age < d->fire[k].t || !point(user, d->fire[k].at, f->pos)) continue;
            f->age = age - d->fire[k].t;
            f->size = d->fire[k].size;
            f->time = d->fire[k].time;
            out->fire_n++;
        }
        for (int k = 0; k < d->flash_n && out->flash_n < 8; k++) {
            VistaSpot *f = &out->flash[out->flash_n];
            float fa = age - d->flash[k].t;
            if (fa < 0 || fa > d->flash[k].time || !point(user, d->flash[k].at, f->pos)) continue;
            f->age = fa;
            f->size = d->flash[k].size;
            f->time = d->flash[k].time;
            out->flash_n++;
        }
        if (d->birds_scatter) out->birds_scatter_age = age;
        if (d->birds_gone) out->birds_scatter_age = 1e6f;
        if (d->day > out->day) out->day = d->day;
        if (d->crowd >= 0) out->crowd = d->crowd;
    }
}

static float hash1(int k, int salt) {
    unsigned h = (unsigned)k * 2654435761u ^ (unsigned)salt * 40503u;
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    return (float)(h & 0xFFFFFF) / 16777216.0f;
}

int vista_smoke_count(const VistaSmoke *s) {
    return s->def ? (int)(s->age * s->def->rate) + 1 : 0;
}

bool vista_smoke_particle(const VistaSmoke *s, int k, float out_pos[3], float *size, float *shade, float *alpha) {
    const VistaSmokeDef *d = s->def;
    if (!d || d->rate <= 0) return false;
    float born = (float)k / d->rate, a = s->age - born;
    if (a < 0 || a >= d->life) return false;
    // A young column is thin: particles are kept in proportion to how far it has grown.
    float grown = d->grow > 0 ? clamp01(born / d->grow) : 1.0f;
    if (hash1(k, 1) > 0.25f + 0.75f * grown) return false;
    float u = a / d->life;
    float ang = hash1(k, 2) * 6.2831853f, rad = d->spread * (0.3f + 0.7f * hash1(k, 3)) * (1.0f + 2.5f * u);
    float rise = d->rise * a * (1.0f - 0.35f * u);
    out_pos[0] = s->pos[0] + cosf(ang) * rad + d->drift[0] * a;
    out_pos[1] = s->pos[1] + rise + d->size0 * 0.3f;
    out_pos[2] = s->pos[2] + sinf(ang) * rad + d->drift[1] * a;
    *size = (d->size0 + (d->size1 - d->size0) * sqrtf(u)) * (0.8f + 0.4f * hash1(k, 4)) * (0.6f + 0.4f * grown);
    *shade = d->shade0 + (d->shade1 - d->shade0) * u;
    float fin = clamp01(u / 0.04f), fout = clamp01((1.0f - u) / 0.35f);
    *alpha = fin * fout * (0.55f + 0.45f * grown);
    return true;
}
