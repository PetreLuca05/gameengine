#include "syn_material.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *k_names[SYN_MAT_BUILTIN_COUNT] = {
    "default", "concrete", "brick", "plaster", "metal_thin", "metal_thick", "wood", "dirt", "sand", "gravel",
    "grass", "glass", "water", "flesh", "cloth", "tile", "asphalt", "snow", "mud", "leaves", "roof_metal", "foliage",
};

typedef struct MatInit {
    int id;
    float f[8], t60[8], g[8];
    float contact_ms, noise, noise_hz;
    float absorb[3], reflect, tl_base[3], tl_per_cm[3], tl_max;
} MatInit;

// Modes: hand-fitted to recorded taps (concrete / brick / tile: few, heavily damped; metal: inharmonic
// plate series, long; glass: high and bright). Absorption: typical Sabine coefficients at 125 / 1k /
// 4k Hz. Transmission: mass law, a 10 cm concrete wall ~45 dB mid, a 12 mm drywall ~25 dB, a 4 mm pane ~22 dB.
static const MatInit k_init[] = {
    { SYN_MAT_CONCRETE, { 180, 420, 760, 1300, 2100 }, { 0.06f, 0.04f, 0.03f, 0.02f, 0.015f }, { 1, 0.7f, 0.5f, 0.35f, 0.25f },
      0.6f, 0.8f, 2500, { 0.03f, 0.04f, 0.06f }, 0.9f, { 10, 15, 20 }, { 1.5f, 2.2f, 2.8f }, 60 },   // painted, with the usual clutter
    { SYN_MAT_BRICK, { 150, 360, 690, 1150, 1900 }, { 0.07f, 0.05f, 0.035f, 0.025f, 0.02f }, { 1, 0.7f, 0.5f, 0.3f, 0.2f },
      0.7f, 0.75f, 2200, { 0.02f, 0.03f, 0.05f }, 0.85f, { 9, 14, 19 }, { 1.2f, 1.8f, 2.4f }, 58 },
    { SYN_MAT_PLASTER, { 90, 220, 480, 900 }, { 0.10f, 0.07f, 0.05f, 0.03f }, { 1, 0.6f, 0.4f, 0.2f },
      1.2f, 0.5f, 1500, { 0.10f, 0.05f, 0.04f }, 0.7f, { 6, 13, 19 }, { 1.0f, 1.5f, 2.0f }, 40 },
    { SYN_MAT_METAL_THIN, { 520, 1180, 1960, 2870, 3790, 4990, 6400, 7900 }, { 0.5f, 0.42f, 0.36f, 0.3f, 0.26f, 0.22f, 0.18f, 0.15f },
      { 1, 0.8f, 0.7f, 0.55f, 0.45f, 0.35f, 0.25f, 0.18f }, 0.3f, 0.25f, 5000, { 0.03f, 0.03f, 0.04f }, 0.9f, { 8, 14, 20 }, { 6, 8, 10 }, 45 },
    { SYN_MAT_METAL_THICK, { 310, 870, 1520, 2340, 3240, 4410 }, { 1.4f, 1.0f, 0.8f, 0.6f, 0.45f, 0.35f }, { 1, 0.75f, 0.6f, 0.45f, 0.3f, 0.2f },
      0.35f, 0.2f, 4000, { 0.02f, 0.02f, 0.03f }, 0.95f, { 12, 20, 28 }, { 10, 14, 18 }, 60 },
    { SYN_MAT_WOOD, { 140, 310, 560, 900, 1350 }, { 0.12f, 0.09f, 0.07f, 0.05f, 0.035f }, { 1, 0.7f, 0.45f, 0.3f, 0.18f },
      1.0f, 0.35f, 1800, { 0.10f, 0.08f, 0.07f }, 0.7f, { 6, 12, 18 }, { 1.5f, 2.5f, 3.5f }, 40 },
    { SYN_MAT_DIRT, { 0 }, { 0 }, { 0 }, 3.0f, 0.6f, 600, { 0.3f, 0.5f, 0.6f }, 0.5f, { 30, 40, 50 }, { 3, 4, 5 }, 70 },
    { SYN_MAT_SAND, { 0 }, { 0 }, { 0 }, 2.5f, 0.6f, 1200, { 0.4f, 0.6f, 0.7f }, 0.4f, { 30, 40, 50 }, { 3, 4, 5 }, 70 },
    { SYN_MAT_GRAVEL, { 0 }, { 0 }, { 0 }, 1.0f, 1.0f, 3000, { 0.3f, 0.5f, 0.6f }, 0.5f, { 30, 40, 50 }, { 3, 4, 5 }, 70 },
    { SYN_MAT_GRASS, { 0 }, { 0 }, { 0 }, 2.0f, 0.7f, 2500, { 0.2f, 0.5f, 0.7f }, 0.3f, { 30, 40, 50 }, { 3, 4, 5 }, 70 },
    { SYN_MAT_GLASS, { 1800, 3900, 6200, 8800 }, { 0.25f, 0.18f, 0.12f, 0.08f }, { 1, 0.7f, 0.45f, 0.3f },
      0.2f, 0.4f, 6000, { 0.06f, 0.04f, 0.03f }, 0.9f, { 10, 18, 28 }, { 4, 6, 8 }, 38 },
    { SYN_MAT_WATER, { 0 }, { 0 }, { 0 }, 4.0f, 0.8f, 900, { 0.01f, 0.01f, 0.02f }, 0.9f, { 30, 40, 50 }, { 3, 4, 5 }, 70 },
    { SYN_MAT_FLESH, { 90, 160 }, { 0.05f, 0.03f }, { 1, 0.5f }, 4.0f, 0.3f, 400, { 0.3f, 0.5f, 0.6f }, 0.2f, { 8, 12, 16 }, { 0.5f, 1, 1.5f }, 30 },
    { SYN_MAT_CLOTH, { 0 }, { 0 }, { 0 }, 3.0f, 0.4f, 1800, { 0.3f, 0.6f, 0.8f }, 0.2f, { 1, 3, 6 }, { 0.3f, 0.6f, 1.2f }, 15 },
    { SYN_MAT_TILE, { 900, 2300, 4100 }, { 0.04f, 0.03f, 0.02f }, { 1, 0.6f, 0.35f }, 0.3f, 0.6f, 3500, { 0.01f, 0.01f, 0.02f }, 0.9f, { 10, 15, 20 }, { 1.5f, 2.2f, 2.8f }, 55 },
    { SYN_MAT_ASPHALT, { 240, 650 }, { 0.03f, 0.02f }, { 1, 0.5f }, 0.8f, 0.7f, 2000, { 0.02f, 0.04f, 0.05f }, 0.8f, { 10, 15, 20 }, { 1.5f, 2.2f, 2.8f }, 60 },
    { SYN_MAT_SNOW, { 0 }, { 0 }, { 0 }, 6.0f, 0.8f, 1400, { 0.4f, 0.7f, 0.9f }, 0.15f, { 20, 30, 40 }, { 2, 3, 4 }, 60 },
    { SYN_MAT_MUD, { 0 }, { 0 }, { 0 }, 6.0f, 0.7f, 500, { 0.2f, 0.4f, 0.5f }, 0.4f, { 30, 40, 50 }, { 3, 4, 5 }, 70 },
    { SYN_MAT_LEAVES, { 0 }, { 0 }, { 0 }, 2.0f, 1.0f, 3500, { 0.3f, 0.6f, 0.8f }, 0.3f, { 2, 4, 8 }, { 0.5f, 1, 2 }, 20 },
    { SYN_MAT_ROOF_METAL, { 180, 410, 720, 1100, 1680, 2400 }, { 0.5f, 0.4f, 0.32f, 0.25f, 0.2f, 0.15f }, { 1, 0.8f, 0.6f, 0.45f, 0.3f, 0.2f },
      0.4f, 0.3f, 3000, { 0.03f, 0.03f, 0.04f }, 0.9f, { 8, 14, 20 }, { 6, 8, 10 }, 45 },
    { SYN_MAT_FOLIAGE, { 0 }, { 0 }, { 0 }, 2.0f, 0.8f, 3000, { 0.2f, 0.5f, 0.7f }, 0.3f, { 1, 3, 6 }, { 0.1f, 0.3f, 0.6f }, 10 },
};

void syn_material_defaults(SynMaterialTable *t) {
    memset(t, 0, sizeof(*t));
    t->count = SYN_MAT_BUILTIN_COUNT;
    for (int i = 0; i < SYN_MAT_BUILTIN_COUNT; i++) snprintf(t->m[i].name, sizeof(t->m[i].name), "%s", k_names[i]);
    for (int k = 0; k < (int)(sizeof(k_init) / sizeof(k_init[0])); k++) {
        const MatInit *s = &k_init[k];
        SynMaterial *m = &t->m[s->id];
        for (int j = 0; j < 8 && s->f[j] > 0; j++) {
            m->f[j] = s->f[j];
            m->t60[j] = s->t60[j];
            m->g[j] = s->g[j];
            m->nmodes = j + 1;
        }
        m->contact_ms = s->contact_ms;
        m->noise = s->noise;
        m->noise_hz = s->noise_hz;
        m->reflect = s->reflect;
        m->tl_max = s->tl_max;
        for (int b = 0; b < SYN_BANDS; b++) {
            m->absorb[b] = s->absorb[b];
            m->tl_base[b] = s->tl_base[b];
            m->tl_per_cm[b] = s->tl_per_cm[b];
        }
    }
    // Untagged surfaces sound like concrete.
    SynMaterial d = t->m[SYN_MAT_CONCRETE];
    snprintf(d.name, sizeof(d.name), "default");
    t->m[SYN_MAT_DEFAULT] = d;
}

static void read3(const cJSON *a, float out[3]) {
    for (int i = 0; i < 3 && cJSON_IsArray(a); i++) {
        const cJSON *v = cJSON_GetArrayItem(a, i);
        if (cJSON_IsNumber(v)) out[i] = (float)v->valuedouble;
    }
}

bool syn_material_parse(SynMaterialTable *t, const char *json, char *err, int err_size) {
    cJSON *root = cJSON_Parse(json);
    if (!root) { snprintf(err, err_size, "materials: JSON parse error"); return false; }
    const cJSON *mats = cJSON_GetObjectItemCaseSensitive(root, "materials");
    const cJSON *it;
    cJSON_ArrayForEach(it, mats) {
        int id = -1;
        for (int i = 0; i < t->count; i++) if (!strcmp(t->m[i].name, it->string)) id = i;
        if (id < 0 && t->count < SYN_MAT_MAX) { id = t->count++; memset(&t->m[id], 0, sizeof(t->m[id])); snprintf(t->m[id].name, sizeof(t->m[id].name), "%s", it->string); }
        if (id < 0) continue;
        SynMaterial *m = &t->m[id];
        const cJSON *modes = cJSON_GetObjectItemCaseSensitive(it, "modes");
        if (cJSON_IsArray(modes)) {
            m->nmodes = 0;
            const cJSON *md;
            cJSON_ArrayForEach(md, modes) {
                if (m->nmodes >= SYN_MAT_MODES || cJSON_GetArraySize(md) < 3) continue;
                m->f[m->nmodes] = (float)cJSON_GetArrayItem(md, 0)->valuedouble;
                m->t60[m->nmodes] = (float)cJSON_GetArrayItem(md, 1)->valuedouble;
                m->g[m->nmodes] = (float)cJSON_GetArrayItem(md, 2)->valuedouble;
                m->nmodes++;
            }
        }
        const cJSON *v;
        if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(it, "contact_ms"))) m->contact_ms = (float)v->valuedouble;
        if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(it, "noise"))) m->noise = (float)v->valuedouble;
        if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(it, "noise_hz"))) m->noise_hz = (float)v->valuedouble;
        if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(it, "reflect"))) m->reflect = (float)v->valuedouble;
        if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(it, "tl_max"))) m->tl_max = (float)v->valuedouble;
        read3(cJSON_GetObjectItemCaseSensitive(it, "absorb"), m->absorb);
        read3(cJSON_GetObjectItemCaseSensitive(it, "tl_base"), m->tl_base);
        read3(cJSON_GetObjectItemCaseSensitive(it, "tl_per_cm"), m->tl_per_cm);
    }
    cJSON_Delete(root);
    return true;
}

static SynMaterialTable g_default;
static const SynMaterialTable *g_table;

void syn_material_set_table(const SynMaterialTable *t) { g_table = t; }

const SynMaterialTable *syn_material_table(void) {
    if (!g_table) {
        syn_material_defaults(&g_default);
        g_table = &g_default;
    }
    return g_table;
}

const SynMaterial *syn_material(int index) {
    const SynMaterialTable *t = syn_material_table();
    if (index < 0 || index >= t->count) index = 0;
    return &t->m[index];
}

int syn_material_find(const char *name) {
    const SynMaterialTable *t = syn_material_table();
    for (int i = 0; i < t->count; i++) if (!strcmp(t->m[i].name, name)) return i;
    return -1;
}

void syn_material_tl(const SynMaterial *m, float thickness_m, float out_db[SYN_BANDS]) {
    float cm = thickness_m * 100.0f;
    for (int b = 0; b < SYN_BANDS; b++) {
        float db = m->tl_base[b] + m->tl_per_cm[b] * cm;
        out_db[b] = db < m->tl_max ? db : m->tl_max;
    }
}
