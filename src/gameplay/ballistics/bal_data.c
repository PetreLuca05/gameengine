// Ballistics data parsing (cartridges, materials, muzzle devices).
#define DG_DETMATH   // cross-platform bit-identical math (core/dgmath.h)
#include "gameplay/ballistics/bal_data.h"
#include "core/detmath.h"
#include "gameplay/ballistics/bal_drag.h"
#include "gameplay/ballistics/bal_sim.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>

// Same order as SurfaceMaterial (vfx_types.h) and k_surface_names (vfx_library.c).
static const char *k_mat_names[SURF_COUNT] = {
    "default", "concrete", "brick", "plaster", "metal_thin", "metal_thick", "wood", "dirt",
    "sand", "gravel", "grass", "glass", "water", "flesh", "cloth", "tile", "asphalt",
};
static const char *k_dev_kinds[BAL_DEV_KIND_COUNT] = { "bare", "hider", "comp", "suppressor" };
static const char *k_tracer_colors[BAL_TRC_COUNT] = { "red", "green", "white", "orange" };

static float jnum(const cJSON *o, const char *k, float def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}
static const char *jstr(const cJSON *o, const char *k, const char *def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : def;
}
static int name_index(const char *const *names, int n, const char *s) {
    if (!s) return -1;
    for (int i = 0; i < n; i++)
        if (!strcmp(names[i], s)) return i;
    return -1;
}

const char *bal_material_name(int surf) { return (surf >= 0 && surf < SURF_COUNT) ? k_mat_names[surf] : "default"; }
int bal_material_from_name(const char *name) { return name_index(k_mat_names, SURF_COUNT, name); }
const char *bal_device_kind_name(int kind) { return (kind >= 0 && kind < BAL_DEV_KIND_COUNT) ? k_dev_kinds[kind] : "bare"; }

static u32 hash_mix(u32 h, const char *json) { return dg_hash_u32(h ^ dg_hash_str(json)); }

static void device_bare(BalDevice *v) {
    memset(v, 0, sizeof(*v));
    dg_strcpy(v->id, sizeof(v->id), "bare");
    v->kind = BAL_DEV_BARE;
    for (int i = 0; i < BFC_COUNT; i++) v->mul[i] = 1.0f;
    v->mul[BFC_PRONGS] = 0.0f;
    v->mul[BFC_VENTS] = 0.0f;
    v->mul[BFC_VAPOR] = 0.0f;
    v->velocity_mul = 1.0f;
}

void bal_data_init(BalData *d) {
    memset(d, 0, sizeof(*d));
    for (int i = 0; i < SURF_COUNT; i++) {
        BalMaterial *m = &d->mat[i];
        m->cls = BAL_HARD;
        m->depth_ref = 0.04f;
        m->hardness = 0.7f;
        m->porosity = 0.5f;
        m->exit_spread_deg = 8.0f;
        m->decal_scale = 1.0f;
        m->thickness = 0.2f;
    }
    device_bare(&d->dev[0]);
    d->dev_count = 1;
}

float bal_cartridge_mv(const BalCartridge *c, float barrel_m) {
    if (c->mv_count <= 0) return c->mv_ref;
    if (barrel_m <= c->mv_curve[0][0] || c->mv_count == 1) {
        if (c->mv_count == 1) return c->mv_curve[0][1];
        // Extrapolate the first segment's slope, never below half the first point.
        float s = (c->mv_curve[1][1] - c->mv_curve[0][1]) / (c->mv_curve[1][0] - c->mv_curve[0][0]);
        return fmaxf(c->mv_curve[0][1] + s * (barrel_m - c->mv_curve[0][0]), 0.5f * c->mv_curve[0][1]);
    }
    for (int i = 0; i + 1 < c->mv_count; i++) {
        if (barrel_m <= c->mv_curve[i + 1][0]) {
            float f = (barrel_m - c->mv_curve[i][0]) / (c->mv_curve[i + 1][0] - c->mv_curve[i][0]);
            return c->mv_curve[i][1] + f * (c->mv_curve[i + 1][1] - c->mv_curve[i][1]);
        }
    }
    return c->mv_curve[c->mv_count - 1][1];   // past the curve: powder is burnt out, flat
}

void bal_cartridge_finalize(BalCartridge *c) {
    c->hash = dg_hash_str(c->id);
    if (c->mv_ref <= 0.0f) c->mv_ref = bal_cartridge_mv(c, c->ref_barrel);
    c->e_ref = 0.5f * c->mass * c->mv_ref * c->mv_ref;
    // Miller twist rule: Sg = 30·m / (t²·d³·l·(1 + l²)) · (v/2800 fps)^(1/3), m grains, d inches,
    // t and l in calibres. Only rifled, elongated bullets drift.
    c->spin_sg = 0.0f;
    if (c->twist > 0.0f && c->diameter > 0.0f && c->length > c->diameter) {
        double d_in = c->diameter / 0.0254, m_gr = c->mass * 15432.358;
        double t_cal = c->twist / c->diameter, l_cal = c->length / c->diameter;
        double sg = 30.0 * m_gr / (t_cal * t_cal * d_in * d_in * d_in * l_cal * (1.0 + l_cal * l_cal));
        sg *= cbrt((c->mv_ref / 0.3048) / 2800.0);
        c->spin_sg = (float)sg;
    }
    // Exponential model: v(x) = v0·e^(-k·x) (a = k·v²). Fit k so the table and the approximation
    // agree on the velocity at 300 m (or at half the cartridge's useful range for slow rounds).
    c->exp_k = 0.0f;
    if (c->bc > 0.0f && c->mv_ref > 0.0f) {
        BalEnv e = bal_env_make(15.0f, 0.0f, BAL_DRAG_TABLE);
        e.spin_drift = false;
        e.rho = 1.2250f;   // ICAO sea level, as the exp model scales by rho / 1.225
        BalBullet b;
        bal_launch(&b, c, 0, v3(0, 0, 0), v3(1, 0, 0), c->mv_ref, 1, 0, false);
        float fit_x = c->mv_ref > 600.0f ? 300.0f : 100.0f;
        float v_fit = 0.0f;
        while (bal_step(&b, c, &e, 1.0f / 240.0f)) {
            if (b.pos.x >= fit_x) { v_fit = v3_len(b.vel); break; }
        }
        if (v_fit > 0.0f) c->exp_k = dm_logf(c->mv_ref / v_fit) / fit_x;
    }
}

static void parse_tracer(BalTracerDef *t, const cJSON *j) {
    memset(t, 0, sizeof(*t));
    if (!cJSON_IsObject(j)) return;
    t->every = (u8)DG_CLAMP((int)jnum(j, "every", 0), 0, 255);
    t->last_n = (u8)DG_CLAMP((int)jnum(j, "last_n", 0), 0, 255);
    int col = name_index(k_tracer_colors, BAL_TRC_COUNT, jstr(j, "color", "red"));
    t->color = (u8)(col >= 0 ? col : BAL_TRC_RED);
    t->ignition_m = jnum(j, "ignition_m", 0);
    t->burn_m = jnum(j, "burn_m", 800);
    t->burn_s = jnum(j, "burn_s", 1.5f);
    t->mass = jnum(j, "mass_g", 0) * 0.001f;
    t->bc = jnum(j, "bc", 0);
}

bool bal_data_parse_cartridges(BalData *d, const char *json) {
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "cartridges");
    const cJSON *j;
    cJSON_ArrayForEach(j, arr) {
        const char *id = jstr(j, "id", NULL);
        if (!id) continue;
        int idx = bal_cartridge_find(d, id);
        if (idx < 0) {
            if (d->cart_count >= BAL_MAX_CARTRIDGES) continue;
            idx = d->cart_count++;
        }
        BalCartridge *c = &d->cart[idx];
        memset(c, 0, sizeof(*c));
        dg_strcpy(c->id, sizeof(c->id), id);
        dg_strcpy(c->name, sizeof(c->name), jstr(j, "name", id));
        c->mass = jnum(j, "mass_g", 8) * 0.001f;
        c->diameter = jnum(j, "diameter_mm", 7.62f) * 0.001f;
        c->length = jnum(j, "length_mm", 0) * 0.001f;
        c->twist = jnum(j, "twist_mm", 0) * 0.001f;
        const char *dr = jstr(j, "drag", "g1");
        c->curve = !strcmp(dr, "g7") ? BAL_CURVE_G7 : (!strcmp(dr, "gs") ? BAL_CURVE_GS : BAL_CURVE_G1);
        c->bc = jnum(j, "bc", 0.3f);
        const cJSON *mv = cJSON_GetObjectItemCaseSensitive(j, "mv_curve"), *pt;
        cJSON_ArrayForEach(pt, mv) {
            if (c->mv_count >= BAL_MV_POINTS || cJSON_GetArraySize(pt) < 2) continue;
            c->mv_curve[c->mv_count][0] = (float)cJSON_GetArrayItem(pt, 0)->valuedouble * 0.001f;
            c->mv_curve[c->mv_count][1] = (float)cJSON_GetArrayItem(pt, 1)->valuedouble;
            c->mv_count++;
        }
        c->ref_barrel = jnum(j, "ref_barrel_mm", c->mv_count ? c->mv_curve[0][0] * 1000.0f : 500.0f) * 0.001f;
        c->mv_ref = jnum(j, "mv", 0);
        c->powder = jnum(j, "powder_g", 1.5f);
        c->gas_temp = jnum(j, "gas_k", 2800);
        c->burn_length = jnum(j, "burn_length_m", 0.5f);
        c->muzzle_pressure = jnum(j, "muzzle_pressure_mpa", 60);
        c->pellets = DG_MAX(1, (int)jnum(j, "pellets", 1));
        const cJSON *pen = cJSON_GetObjectItemCaseSensitive(j, "pen");
        c->pen_hard = jnum(pen, "hard", 1);
        c->pen_soft = jnum(pen, "soft", 1);
        c->fragment_v = jnum(j, "fragment_v", 0);
        c->yaw_depth = jnum(j, "yaw_depth_mm", 0) * 0.001f;
        c->transonic_k = jnum(j, "transonic_k", 0.02f);
        c->max_range = jnum(j, "max_range", 2000);
        parse_tracer(&c->tracer, cJSON_GetObjectItemCaseSensitive(j, "tracer"));
        bal_cartridge_finalize(c);
    }
    cJSON_Delete(root);
    d->hash = hash_mix(d->hash, json);
    return true;
}

bool bal_data_parse_materials(BalData *d, const char *json) {
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
    const cJSON *mats = cJSON_GetObjectItemCaseSensitive(root, "materials");
    const cJSON *j;
    cJSON_ArrayForEach(j, mats) {
        int i = bal_material_from_name(j->string);
        if (i < 0) continue;
        BalMaterial *m = &d->mat[i];
        m->cls = !strcmp(jstr(j, "class", "hard"), "soft") ? BAL_SOFT : BAL_HARD;
        m->depth_ref = jnum(j, "depth_ref_mm", m->depth_ref * 1000.0f) * 0.001f;
        m->hardness = jnum(j, "hardness", m->hardness);
        m->porosity = jnum(j, "porosity", m->porosity);
        m->exit_spread_deg = jnum(j, "exit_spread_deg", m->exit_spread_deg);
        m->decal_scale = jnum(j, "decal_scale", m->decal_scale);
        m->thickness = jnum(j, "thickness_mm", m->thickness * 1000.0f) * 0.001f;
    }
    cJSON_Delete(root);
    d->hash = hash_mix(d->hash, json);
    return true;
}

static const char *k_comp_names[BFC_COUNT] = {
    "primary", "mach", "secondary", "prongs", "vents", "vapor", "smoke", "sparks", "blast", "light",
};

bool bal_data_parse_attachment(BalData *d, const char *json) {
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
    const cJSON *blk = cJSON_GetObjectItemCaseSensitive(root, "muzzle_device");
    const char *id = jstr(root, "id", NULL);
    if (cJSON_IsObject(blk) && id) {
        int idx = -1;
        for (int i = 1; i < d->dev_count; i++)
            if (!strcmp(d->dev[i].id, id)) idx = i;
        if (idx < 0 && d->dev_count < BAL_MAX_DEVICES) idx = d->dev_count++;
        if (idx > 0) {
            BalDevice *v = &d->dev[idx];
            device_bare(v);
            dg_strcpy(v->id, sizeof(v->id), id);
            int k = name_index(k_dev_kinds, BAL_DEV_KIND_COUNT, jstr(blk, "kind", "bare"));
            v->kind = (u8)(k >= 0 ? k : BAL_DEV_BARE);
            const cJSON *fl = cJSON_GetObjectItemCaseSensitive(blk, "flash");
            for (int c = 0; c < BFC_COUNT; c++) v->mul[c] = jnum(fl, k_comp_names[c], v->mul[c]);
            v->report_db = jnum(blk, "report_db", 0);
            v->report_db_side = jnum(blk, "report_db_side", 0);
            v->velocity_mul = jnum(blk, "velocity_mul", 1);
            const cJSON *poi = cJSON_GetObjectItemCaseSensitive(blk, "poi_moa");
            if (cJSON_GetArraySize(poi) >= 2) {
                v->poi_moa[0] = (float)cJSON_GetArrayItem(poi, 0)->valuedouble;
                v->poi_moa[1] = (float)cJSON_GetArrayItem(poi, 1)->valuedouble;
            }
            v->prongs = (int)jnum(blk, "prongs", 0);
            const cJSON *ports = cJSON_GetObjectItemCaseSensitive(blk, "ports"), *p;
            cJSON_ArrayForEach(p, ports) {
                if (v->port_count >= BAL_MAX_PORTS || cJSON_GetArraySize(p) < 3) continue;
                v->ports[v->port_count++] = v3_norm(v3((float)cJSON_GetArrayItem(p, 0)->valuedouble,
                                                       (float)cJSON_GetArrayItem(p, 1)->valuedouble,
                                                       (float)cJSON_GetArrayItem(p, 2)->valuedouble));
            }
            v->pop_cold_s = jnum(blk, "pop_cold_s", 0);
            v->pop_flash = jnum(blk, "pop_flash", 0.6f);
            v->can_heat_rounds = jnum(blk, "can_heat_rounds", 4);
            v->can_heat_tau = jnum(blk, "can_heat_tau", 20);
        }
    }
    cJSON_Delete(root);
    d->hash = hash_mix(d->hash, json);
    return true;
}

int bal_cartridge_find(const BalData *d, const char *id) {
    if (!id) return -1;
    for (int i = 0; i < d->cart_count; i++)
        if (!strcmp(d->cart[i].id, id)) return i;
    return -1;
}

int bal_device_find(const BalData *d, const char *id) {
    if (!id) return 0;
    for (int i = 0; i < d->dev_count; i++)
        if (!strcmp(d->dev[i].id, id)) return i;
    return 0;
}
