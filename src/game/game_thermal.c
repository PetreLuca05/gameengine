// Thermal scope, game side (docs/NIGHT_OPTICS.md "Thermal scope", "Temperature state layer"). The
// temperature model is pure (world/thermal.c), the sensor's CPU half too (render/thermal_sensor.c); here:
//
//   surfaces   the thermal classes integrated on the world clock from the replicated weather (air
//              temperature, clouds, wetness) and the sun (a time jump re-settles the 48 h history)
//   volumes    heat volumes for the PiP's thermal shading, nearest the scope first: characters (face,
//              chest, legs, hands; zombies by their own colder body, corpses cooling from the death
//              tick), vehicles (engine, exhaust, brakes, tyres: local lumped parts from the replicated
//              engine / rpm / brake / speed; a parked warm car leaves a warm patch), barrels (the
//              shooter's muzzle heat, every machine from EV_SHOT), heat sources (fires: level entities)
//   sensor     power while the thermal optic is on the drawn weapon with charge: NUC cadence + the
//              click, AGC from the GPU metering, palette (ACT_OPTIC_MODE while aiming), digital zoom
//              (the wheel while aiming), battery (host: the weapon's InvItem.dur counts what is used),
//              the OSD (palette, zoom, battery, the spot temperature)
#include "game_internal.h"
#include "core/engine.h"
#include "core/log.h"
#include "core/cmd_registry.h"
#include "core/platform.h"
#include "world/thermal.h"
#include "render/thermal_sensor.h"
#include "render/post/post.h"
#include "gameplay/characters.h"
#include "gameplay/attachments.h"
#include "gameplay/weapons.h"
#include "gameplay/inventory.h"
#include "gameplay/ballistics/bal_fx.h"
#include "input/input.h"
#include "audio/audio.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define G (g_engine.game)

#define MAX_TV 16          // vehicles with a heat state
#define MAX_PATCH 24       // warm ground patches (a car parked there)
#define MAX_OVERRIDE 16    // temp_set

static struct {
    ThermalData data;
    ThermalSurfaces surf;
    ThermalPalettes pals;
    bool loaded;
    double last_hours;
    bool have_hours;
    ThermalEnv env;
    struct { EntityId ent; ThermalVehicle v; bool used; } veh[MAX_TV];
    struct { vec3 pos; float t0, age, tau; bool used; } patch[MAX_PATCH];
    struct { EntityId ent; float c; } ovr[MAX_OVERRIDE];
    int ovr_count;
    // sensor
    bool powered;
    ThermalAgc agc;
    ThermalNuc nuc;
    int palette, zoom;
    float spot_c;
    bool spot_valid;
    float battery;          // the drawn weapon's scope charge 0..1 (from the inventory)
    RenderThermal rt;
    Cvar *debug, *range_lo, *range_hi, *zombie_core;
} TS;

static char *read_asset(const char *path) {
    size_t n = 0;
    return platform_read_file(path, &n);
}

static void load(void) {
    thermal_defaults(&TS.data);
    char *m = read_asset("assets/thermal/materials.json"), *e = read_asset("assets/thermal/entities.json");
    char *p = read_asset("assets/thermal/palettes.json");
    if (!thermal_load_materials(&TS.data, m)) log_warn("thermal: assets/thermal/materials.json invalid (defaults)");
    if (!thermal_load_entities(&TS.data, e)) log_warn("thermal: assets/thermal/entities.json invalid (defaults)");
    thermal_palettes_default(&TS.pals);
    if (!thermal_palettes_load(&TS.pals, p)) log_warn("thermal: assets/thermal/palettes.json invalid (defaults)");
    free(m); free(e); free(p);
    u8 rgb[THERMAL_PALETTES][256][3];
    for (int i = 0; i < TS.pals.count; i++) memcpy(rgb[i], TS.pals.pal[i].rgb, sizeof(rgb[i]));
    render_thermal_palettes((const u8 (*)[256][3])rgb, TS.pals.count);
    TS.zoom = 1;
    TS.loaded = true;
}

const ThermalData *thermal_data(void) {
    if (!TS.loaded) load();
    return &TS.data;
}

// ---------------------------------------------------------------- environment

static void env_now(World *w, ThermalEnv *env) {
    WeatherParams wp;
    memset(&wp, 0, sizeof(wp));
    weather_current(w, &wp);
    env->air = wp.v[WP_TEMPERATURE];
    env->cloud = DG_CLAMP(wp.v[WP_CLOUD_COVER], 0.0f, 1.0f);
    env->wetness = DG_CLAMP(w->weather.wetness, 0.0f, 1.0f);
    env->wind = wp.v[WP_WIND];
    env->sun = thermal_sun_input(world_sun_elevation_deg(w));
}

// The past day for a re-settle: today's weather, the sun as a half sine over 6..18 h peaking at the
// elevation it reaches today (close enough for 48 h of inertia).
typedef struct SettleCtx { ThermalEnv now; float peak; } SettleCtx;
static void env_at(double hours, ThermalEnv *out, void *user) {
    const SettleCtx *c = user;
    *out = c->now;
    double h = fmod(hours, 24.0);
    if (h < 0) h += 24.0;
    float elev = (float)(sin((h - 6.0) / 12.0 * 3.14159265) * c->peak);
    out->sun = thermal_sun_input(elev);
}

static void surfaces_frame(World *w) {
    env_now(w, &TS.env);
    double h = world_time_hours(w);
    double dh = h - TS.last_hours;
    if (dh < -12.0) dh += 24.0;
    if (!TS.have_hours || !TS.surf.valid || fabs(dh) > 1.0) {
        SettleCtx c = { TS.env, 62.0f };
        thermal_surfaces_settle(&TS.data, &TS.surf, h, env_at, &c);
    } else if (dh > 0.0) {
        thermal_surfaces_step(&TS.data, &TS.surf, &TS.env, (float)(dh * 3600.0));
    }
    TS.last_hours = h;
    TS.have_hours = true;
}

// ---------------------------------------------------------------- volumes

static float override_c(EntityId e, bool *has) {
    for (int i = 0; i < TS.ovr_count; i++)
        if (TS.ovr[i].ent == e) { *has = true; return TS.ovr[i].c; }
    *has = false;
    return 0.0f;
}

static void vol(RenderThermal *rt, vec3 pos, float r, float t, int kind, float core) {
    if (rt->vol_count >= RT_VOLUMES) return;
    int i = rt->vol_count++;
    rt->vol[i].pos = pos;
    rt->vol[i].radius = r;
    rt->vol[i].temp = t;
    rt->vol[i].kind = kind;
    rt->vol[i].core = core;
}

typedef struct Cand { int kind; int idx; float d2; } Cand;
static int cand_cmp(const void *a, const void *b) {
    float x = ((const Cand *)a)->d2, y = ((const Cand *)b)->d2;
    return x < y ? -1 : x > y ? 1 : 0;
}

// A character's regions. Seconds since death cool every region towards the air (Newtonian).
static int character_volumes(World *w, RenderThermal *rt, CharacterInst *c) {
    vec3 pelvis, head;
    if (!chars_body_axis(w, c->ent, &pelvis, &head)) return 0;
    const ThermalData *d = &TS.data;
    ThermalBody body = c->def && c->def->kind == CK_ZOMBIE ? d->zombie : d->human;
    if (c->def && c->def->kind == CK_ZOMBIE && TS.zombie_core) {
        float dc = TS.zombie_core->f - body.core;
        body.core += dc;
        body.skin += dc;
    }
    bool has;
    float oc = override_c(c->ent, &has);
    if (has) { body.skin += oc - body.core; body.core = oc; }
    float air = TS.env.air, wind = TS.env.wind;
    float face = thermal_role_temp(d, &body, THR_FACE, air, wind);
    float chest = thermal_role_temp(d, &body, THR_TORSO, air, wind);
    float legs = thermal_role_temp(d, &body, THR_LEGS, air, wind);
    float hands = thermal_role_temp(d, &body, THR_HANDS, air, wind);
    if (c->dead && c->death_tick && !has) {
        float t = (float)(w->tick - c->death_tick) * SIM_DT;
        face = thermal_cooling(face, air, t, d->corpse_tau);
        chest = thermal_cooling(chest, air, t, d->corpse_tau);
        legs = thermal_cooling(legs, air, t, d->corpse_tau);
        hands = thermal_cooling(hands, air, t, d->corpse_tau);
    }
    int n0 = rt->vol_count;
    vec3 up = v3_sub(head, pelvis);
    vol(rt, v3_madd(head, up, 0.12f), 0.24f, face, 1, 0.45f);
    vol(rt, v3_add(pelvis, v3_scale(up, 0.55f)), 0.42f, chest, 1, 0.5f);
    vol(rt, v3_sub(pelvis, v3_scale(up, 0.55f)), 0.55f, legs, 1, 0.5f);
    for (int k = 0; k < 2; k++) {
        vec3 palm;
        if (chars_palm_world(w, c->ent, k, &palm)) vol(rt, palm, 0.13f, hands, 1, 0.4f);
    }
    return rt->vol_count - n0;
}

static ThermalVehicle *veh_state(EntityId e, float air, bool create) {
    for (int i = 0; i < MAX_TV; i++)
        if (TS.veh[i].used && TS.veh[i].ent == e) return &TS.veh[i].v;
    if (!create) return NULL;
    for (int i = 0; i < MAX_TV; i++)
        if (!TS.veh[i].used) {
            TS.veh[i].used = true;
            TS.veh[i].ent = e;
            thermal_vehicle_init(&TS.veh[i].v, air, air);
            return &TS.veh[i].v;
        }
    return NULL;
}

// Vehicles heat on every machine from the replicated state (cosmetic); a warm engine standing still
// warms the ground under it, and the patch stays after the car leaves.
static void thermal_vehicles_frame(World *w, float dt) {
    VehThermal vt[MAX_TV];
    int n = vehicles_thermal(w, vt, MAX_TV);
    float air = TS.env.air;
    for (int i = 0; i < n; i++) {
        ThermalVehicle *s = veh_state(vt[i].ent, air, true);
        if (!s) continue;
        ThermalVehInput in = { vt[i].engine, vt[i].load, vt[i].braking, vt[i].speed };
        thermal_vehicle_step(&TS.data, s, &in, air, dt);
        if (vt[i].speed < 0.5f && s->part[THV_ENGINE] > air + 10.0f) {
            float t = air + (s->part[THV_ENGINE] - air) * 0.3f;
            int k = -1;
            for (int j = 0; j < MAX_PATCH; j++)
                if (TS.patch[j].used && v3_dist(TS.patch[j].pos, vt[i].ground) < 1.0f) k = j;
            for (int j = 0; j < MAX_PATCH && k < 0; j++)
                if (!TS.patch[j].used) k = j;
            if (k >= 0) {
                float t0 = TS.patch[k].used ? fmaxf(t, TS.patch[k].t0) : t;
                TS.patch[k].pos = vt[i].ground;
                TS.patch[k].t0 = t0;
                TS.patch[k].age = 0.0f;
                TS.patch[k].tau = TS.data.heat_decal_tau;
                TS.patch[k].used = true;
            }
        }
    }
    for (int j = 0; j < MAX_PATCH; j++) {
        if (!TS.patch[j].used) continue;
        TS.patch[j].age += dt;
        if (TS.patch[j].age > TS.patch[j].tau * 3.0f) TS.patch[j].used = false;
    }
    for (int i = 0; i < MAX_TV; i++)   // forget vehicles that are gone
        if (TS.veh[i].used && !ent_valid(w, TS.veh[i].ent)) TS.veh[i].used = false;
}

static void vehicle_volumes(World *w, RenderThermal *rt, const VehThermal *v) {
    ThermalVehicle *s = veh_state(v->ent, TS.env.air, false);
    if (!s) return;
    vol(rt, v->engine_pos, v->engine_r * 1.3f, s->part[THV_ENGINE], 0, 0.35f);
    vol(rt, v->exhaust_pos, 0.3f, s->part[THV_EXHAUST], 0, 0.3f);
    for (int k = 0; k < v->wheel_count; k++) {
        vol(rt, v->wheel_pos[k], v->wheel_r[k] * 1.08f, s->tire_w[k], 0, 0.75f);
        vol(rt, v->wheel_pos[k], v->wheel_r[k] * 0.55f, s->brake_w[k], 0, 0.4f);
    }
    DG_UNUSED(w);
}

// Every heat volume this frame, nearest the camera first, up to RT_VOLUMES.
static void build_volumes(World *w, RenderThermal *rt, vec3 cam) {
    rt->vol_count = 0;
    static Cand cand[MAX_ENTITIES + MAX_TV + MAX_PATCH];
    int n = 0;
    for (int i = 1; i < MAX_ENTITIES; i++) {
        CharacterInst *c = char_get_index(w, i);
        if (!c || !ent_valid(w, c->ent) || c->phantom) continue;
        float d2 = v3_len2(v3_sub(w->xf[i].pos, cam));
        if (d2 > 300.0f * 300.0f) continue;
        cand[n++] = (Cand){ 0, i, d2 };
    }
    VehThermal vt[MAX_TV];
    int nv = vehicles_thermal(w, vt, MAX_TV);
    for (int i = 0; i < nv; i++) cand[n++] = (Cand){ 1, i, v3_len2(v3_sub(vt[i].engine_pos, cam)) };
    for (int j = 0; j < MAX_PATCH; j++)
        if (TS.patch[j].used) cand[n++] = (Cand){ 2, j, v3_len2(v3_sub(TS.patch[j].pos, cam)) };
    for (int s = 0; s < MAX_PLAYERS; s++) {   // barrels: the shooter's muzzle heat
        Player *p = &w->players[s];
        if (!p->active || !ent_valid(w, p->ent) || bal_fx_heat(s) < 0.2f) continue;
        cand[n++] = (Cand){ 3, s, v3_len2(v3_sub(w->xf[ENT_INDEX(p->ent)].pos, cam)) };
    }
    qsort(cand, (size_t)n, sizeof(Cand), cand_cmp);
    for (int k = 0; k < n && rt->vol_count < RT_VOLUMES - 4; k++) {
        switch (cand[k].kind) {
        case 0: character_volumes(w, rt, char_get_index(w, cand[k].idx)); break;
        case 1: if (rt->vol_count < RT_VOLUMES - 10) vehicle_volumes(w, rt, &vt[cand[k].idx]); break;
        case 2: {
            float a = TS.patch[cand[k].idx].age, tau = TS.patch[cand[k].idx].tau;
            float t = thermal_cooling(TS.patch[cand[k].idx].t0, TS.env.air, a, tau);
            if (t > TS.env.air + 0.3f) vol(rt, TS.patch[cand[k].idx].pos, 1.6f, t, 2, 0.3f);
            break;
        }
        case 3: {
            int s = cand[k].idx;
            vec3 mz, dir;
            if (!chars_item_muzzle(w, w->players[s].ent, &mz, &dir)) break;
            float t = thermal_barrel_temp(&TS.data, bal_fx_heat(s), TS.env.air);
            vol(rt, v3_madd(mz, dir, -0.18f), 0.26f, t, 0, 0.35f);
            break;
        }
        }
    }
    // fires (LIGHT_GROUP_FIRE): a hot core and a warm column above it
    for (int i = 1; i < MAX_ENTITIES && rt->vol_count < RT_VOLUMES - 2; i++) {
        if (w->type[i] != ET_LIGHT || !(w->light[i].group & LIGHT_GROUP_FIRE) || !(w->flags[i] & EF_ON)) continue;
        if (!ent_valid(w, ent_from_index(w, i)) || v3_len2(v3_sub(w->xf[i].pos, cam)) > 400.0f * 400.0f) continue;
        vol(rt, v3_add(w->xf[i].pos, v3(0, 0.15f, 0)), 0.7f, TS.data.fire, 0, 0.35f);
        vol(rt, v3_add(w->xf[i].pos, v3(0, 0.1f, 0)), 2.2f, TS.env.air + 25.0f, 2, 0.3f);   // the ground around it
    }
}

// ---------------------------------------------------------------- sensor

static const InvItem *drawn_weapon_item(Player *p) {
    const InvSlot *s = p ? weapon_equipped(G->world, (int)(p - G->world->players)) : NULL;
    if (!s || p->sim.wpn.slot >= INV_SEL_WEAPONS) return NULL;
    // the instance holding this slot's weapon (its dur is the scope battery)
    for (int i = 0; i < INV_MAX_ITEMS; i++) {
        const InvItem *it = &p->inv.t.items[i];
        if (inv_alive(it) && !it->parent && it->loc == INV_LOC_EQUIP + p->sim.wpn.slot) return it;
    }
    return NULL;
}

// Host: the drawn weapon's thermal scope runs its battery down (InvItem.dur counts the charge used).
void thermal_host_tick(World *w, int slot, const UserCmd *c, float dt) {
    DG_UNUSED(c);
    if (!w->authority) return;
    Player *p = &w->players[slot];
    if (p->sim.wpn.slot >= INV_SEL_WEAPONS) return;
    InvItem *it = NULL;
    for (int i = 0; i < INV_MAX_ITEMS; i++) {
        InvItem *x = &p->inv.t.items[i];
        if (inv_alive(x) && !x->parent && x->loc == INV_LOC_EQUIP + p->sim.wpn.slot) it = x;
    }
    if (!it || inv_battery_kind(w->items, &it->s) != INV_BATTERY_OPTIC) return;
    const AttachmentDef *a = attach_def(w->items, it->s.att[ATT_OPTIC]);
    static float acc[MAX_PLAYERS];
    acc[slot] += dt / (a->battery_h * 3600.0f) * INV_BATTERY_FULL;
    if (acc[slot] >= 1.0f && it->dur < INV_BATTERY_FULL) {
        int k = (int)acc[slot];
        acc[slot] -= (float)k;
        it->dur = (u16)DG_MIN(it->dur + k, INV_BATTERY_FULL);
        p->inv.revision++;
        if (it->dur >= INV_BATTERY_FULL) ev_hint(w, slot, "Thermal scope battery empty");
    }
}

bool thermal_sight_active(void) { return TS.powered; }

bool thermal_wheel(int dir) {
    if (!TS.powered || sights_ads() < 0.5f) return false;
    const AttachmentDef *a = sights_optic();
    int zmax = a ? DG_MAX(a->zoom_max, 1) : 1;
    int z = DG_CLAMP(TS.zoom + (dir < 0 ? 1 : -1), 1, zmax);
    if (z != TS.zoom) { TS.zoom = z; audio_play_2d("click", 0.2f, SND_SFX); }
    return true;
}

static void osd(RenderThermal *rt, float battery) {
    char spot[24];
    if (TS.spot_valid) snprintf(spot, sizeof(spot), "%.1f*C", (double)TS.spot_c);
    else snprintf(spot, sizeof(spot), "--.-*C");
    if (rt->dead) {
        snprintf(rt->osd, sizeof(rt->osd), "NO BATTERY\n\n\n");
        return;
    }
    snprintf(rt->osd, sizeof(rt->osd), "%s  %dX\nBAT %d%%%s\n%s\n%s", TS.pals.pal[TS.palette].name, TS.zoom,
             (int)lroundf(battery * 100.0f), TS.agc.fixed ? "  MAN" : "", spot, rt->frozen ? "NUC" : "");
}

void thermal_frame(float dt) {
    if (!TS.loaded) load();
    World *w = G->world;
    RenderThermal *rt = &TS.rt;
    memset(rt, 0, sizeof(*rt));
    if (!w) { render_set_thermal(rt); return; }
    surfaces_frame(w);
    thermal_vehicles_frame(w, dt);
    const AttachmentDef *a = sights_optic();
    Player *vp = game_view_player();
    const InvItem *wi = vp ? drawn_weapon_item(vp) : NULL;
    bool thermal = a && a->sensor == SENSOR_THERMAL;
    float battery = wi && thermal ? inv_battery_charge(w->items, wi) : 0.0f;
    if (wi && thermal && inv_battery_kind(w->items, &wi->s) != INV_BATTERY_OPTIC) battery = 1.0f;
    bool powered = thermal && battery > 0.0f;
    if (powered && !TS.powered) {   // power on: a calibration first, the AGC starts over
        thermal_nuc_reset(&TS.nuc, TS.env.air);
        TS.agc.valid = false;
        TS.spot_valid = false;
    }
    TS.powered = powered;
    TS.battery = battery;
    // palette: ACT_OPTIC_MODE while aiming through it
    if (input_consume_press(ACT_OPTIC_MODE) && thermal && sights_ads() > 0.5f) {
        TS.palette = (TS.palette + 1) % DG_MAX(TS.pals.count, 1);
        audio_play_2d("click", 0.25f, SND_SFX);
    }
    TS.zoom = DG_CLAMP(TS.zoom, 1, a ? DG_MAX(a->zoom_max, 1) : 1);
    if (!thermal || sights_ads() < 0.02f) { render_set_thermal(rt); return; }
    // the sensor model
    if (powered && thermal_nuc_step(&TS.nuc, dt, TS.env.air)) audio_play_2d("click", 0.12f, SND_SFX);
    float lo, hi, sp;
    if (render_thermal_measure(&lo, &hi, &sp)) {
        thermal_agc_step(&TS.agc, lo, hi, dt);
        TS.spot_c = thermal_spot_temp(sp, 0.95f, TS.env.air);
        TS.spot_valid = true;
    }
    if (TS.range_lo->f < TS.range_hi->f) {   // thermal_debug range
        TS.agc.fixed = true;
        TS.agc.lo = thermal_radiance(TS.range_lo->f);
        TS.agc.hi = thermal_radiance(TS.range_hi->f);
    } else {
        TS.agc.fixed = false;
    }
    if (!TS.agc.valid) {   // until the first measurement: the air .. a person
        TS.agc.lo = thermal_radiance(TS.env.air - 8.0f);
        TS.agc.hi = thermal_radiance(36.0f);
    }
    rt->on = true;
    rt->air = TS.env.air;
    rt->ground = thermal_surface_temp(&TS.data, &TS.surf, &TS.env, THC_SOIL, 0.0f, 1.0f);
    rt->bulb = TS.data.bulb;
    rt->sky_depression = TS.data.sky_clear_depression * (1.0f - (1.0f - TS.data.sky_cloud_keep) * TS.env.cloud);
    rt->sky_exponent = TS.data.sky_exponent;
    rt->wetness = TS.env.wetness;
    float clear = 1.0f - TS.env.cloud, night = 1.0f - DG_MIN(TS.env.sun * 4.0f, 1.0f);
    for (int c = 0; c < THC_COUNT; c++) {
        const ThermalClassDef *k = &TS.data.cls[c];
        rt->cls[c][0] = TS.surf.valid ? TS.surf.t[c] : thermal_class_eq(&TS.data, c, &TS.env);
        rt->cls[c][1] = k->emissivity;
        rt->cls[c][2] = k->direct_gain * (TS.surf.valid ? TS.surf.solar[c] : TS.env.sun) * (1.0f - 0.75f * TS.env.cloud);
        rt->cls[c][3] = k->sky_facing_loss * clear * night;
    }
    ThermalCore core = thermal_core(post_night_optics_quality() >= 2 ? 1 : 0);
    rt->core_w = core.w;
    rt->core_h = core.h;
    rt->zoom = TS.zoom;
    rt->palette = TS.palette;
    rt->agc_lo = TS.agc.lo;
    rt->agc_hi = TS.agc.hi;
    rt->drift = thermal_nuc_drift(&TS.nuc);
    rt->frozen = TS.nuc.freeze > 0.0f;
    rt->dead = !powered;
    rt->seed = wi ? dg_hash_u32(wi->uid * 2654435761u + 7u) : 1u;
    rt->spot_e = 0.95f;
    build_volumes(w, rt, render_sight()->objective_pos);
    osd(rt, battery);
    render_set_thermal(rt);
}

// ---------------------------------------------------------------- commands

static EntityId entity_arg(World *w, const char *s) {
    if (!strcmp(s, "self") || !strcmp(s, "me")) { Player *p = game_local_player(); return p ? p->ent : ENT_NONE; }
    EntityId e = (EntityId)strtoul(s, NULL, 0);
    if (ent_valid(w, e)) return e;
    return ent_find_by_name(w, s);
}

static void cmd_temp_set(int argc, const char **argv) {
    World *w = G->world;
    if (!w || argc < 3) { log_info("usage: temp_set <entity id | name | self> <C | off>"); return; }
    EntityId e = entity_arg(w, argv[1]);
    if (!ent_valid(w, e)) { log_info("temp_set: no entity '%s'", argv[1]); return; }
    for (int i = 0; i < TS.ovr_count; i++)
        if (TS.ovr[i].ent == e) { TS.ovr[i] = TS.ovr[--TS.ovr_count]; break; }
    if (strcmp(argv[2], "off") && TS.ovr_count < MAX_OVERRIDE) {
        TS.ovr[TS.ovr_count].ent = e;
        TS.ovr[TS.ovr_count++].c = (float)atof(argv[2]);
    }
    // a vehicle: its engine
    ThermalVehicle *v = veh_state(e, TS.env.air, true);
    if (v && strcmp(argv[2], "off") && vehicles_thermal(w, NULL, 0) >= 0) {
        bool is_veh = false;
        VehThermal vt[MAX_TV];
        int n = vehicles_thermal(w, vt, MAX_TV);
        for (int i = 0; i < n; i++) is_veh |= vt[i].ent == e;
        if (is_veh) thermal_vehicle_init(v, TS.env.air, (float)atof(argv[2]));
        else for (int i = 0; i < MAX_TV; i++) if (TS.veh[i].used && TS.veh[i].ent == e) TS.veh[i].used = false;
    }
    log_info("temp_set: entity %u %s", e, strcmp(argv[2], "off") ? argv[2] : "off");
}

static void cmd_thermal_debug(int argc, const char **argv) {
    World *w = G->world;
    if (!w) return;
    if (argc >= 2 && !strcmp(argv[1], "range")) {
        float a = argc > 2 ? (float)atof(argv[2]) : 0.0f, b = argc > 3 ? (float)atof(argv[3]) : 0.0f;
        cvar_set_float(TS.range_lo, a);
        cvar_set_float(TS.range_hi, b);
        log_info("thermal_debug: range %s", a < b ? "fixed" : "AGC");
        return;
    }
    if (argc >= 2 && !strcmp(argv[1], "palette") && argc > 2) { TS.palette = DG_CLAMP(atoi(argv[2]), 0, TS.pals.count - 1); return; }
    if (argc >= 2 && !strcmp(argv[1], "zoom") && argc > 2) { TS.zoom = DG_CLAMP(atoi(argv[2]), 1, 4); return; }
    if (!TS.loaded) load();
    env_now(w, &TS.env);
    log_info("thermal_debug: air %.1f C, cloud %.2f, sun %.2f, wet %.2f, wind %.1f m/s; sky zenith %.1f C", TS.env.air, TS.env.cloud,
             TS.env.sun, TS.env.wetness, TS.env.wind, thermal_sky_temp(&TS.data, TS.env.air, TS.env.cloud, 1.0f));
    for (int c = 0; c < THC_COUNT; c++)
        log_info("thermal_debug:   %-14s e %.2f  %.1f C  (solar %.2f)", thermal_class_name(c), TS.data.cls[c].emissivity,
                 TS.surf.valid ? TS.surf.t[c] : thermal_class_eq(&TS.data, c, &TS.env), TS.surf.valid ? TS.surf.solar[c] : TS.env.sun);
    const RenderThermal *rt = &TS.rt;
    log_info("thermal_debug: sensor %s powered %d battery %.2f palette %s zoom %dx core %dx%d volumes %d", rt->on ? "on" : "off", TS.powered,
             TS.battery, TS.pals.pal[TS.palette].name, TS.zoom, rt->core_w, rt->core_h, rt->vol_count);
    log_info("thermal_debug: AGC %s %.1f .. %.1f C  spot %.1f C  NUC #%d in %.1f s (interval %.0f s, freeze %.2f, drift %.2f)",
             TS.agc.fixed ? "fixed" : "auto", thermal_temp_of_radiance(TS.agc.lo), thermal_temp_of_radiance(TS.agc.hi), TS.spot_c,
             TS.nuc.count, TS.nuc.interval - TS.nuc.since, TS.nuc.interval, TS.nuc.freeze, thermal_nuc_drift(&TS.nuc));
    for (int i = 0; i < rt->vol_count && TS.debug->i; i++)
        log_info("thermal_debug:   volume %d kind %d (%.1f %.1f %.1f) r %.2f  %.1f C", i, rt->vol[i].kind, rt->vol[i].pos.x, rt->vol[i].pos.y,
                 rt->vol[i].pos.z, rt->vol[i].radius, rt->vol[i].temp);
}

// What a character / vehicle reads (the model's value, as the shader gets it): tests and tuning.
static void cmd_thermal_probe(int argc, const char **argv) {
    World *w = G->world;
    if (!w || argc < 2) { log_info("usage: thermal_probe <entity id | name | self>"); return; }
    EntityId e = entity_arg(w, argv[1]);
    CharacterInst *c = ent_valid(w, e) ? char_get(w, e) : NULL;
    RenderThermal tmp;
    memset(&tmp, 0, sizeof(tmp));
    if (!TS.loaded) load();
    env_now(w, &TS.env);
    if (c && character_volumes(w, &tmp, c)) {
        log_info("thermal_probe: %u %s face %.1f C chest %.1f C legs %.1f C%s", e, c->def && c->def->kind == CK_ZOMBIE ? "zombie" : "human",
                 tmp.vol[0].temp, tmp.vol[1].temp, tmp.vol[2].temp, c->dead ? " (dead)" : "");
        return;
    }
    ThermalVehicle *v = veh_state(e, TS.env.air, false);
    if (v) {
        log_info("thermal_probe: %u vehicle engine %.1f exhaust %.1f brake %.1f tyre %.1f C", e, v->part[THV_ENGINE], v->part[THV_EXHAUST],
                 v->part[THV_BRAKE], v->part[THV_TIRE]);
        return;
    }
    log_info("thermal_probe: %u: no character / vehicle heat state", e);
}

void thermal_register(void) {
    TS.debug = cvar_reg_bool("thermal_debug_volumes", false, CVAR_LOCKED, "thermal_debug also lists every heat volume");
    TS.range_lo = cvar_reg_float("thermal_range_lo", 0.0f, -60.0f, 1500.0f, CVAR_LOCKED, "thermal scope fixed range low C (with thermal_range_hi above it: no AGC; dev)");
    TS.range_hi = cvar_reg_float("thermal_range_hi", 0.0f, -60.0f, 1500.0f, CVAR_LOCKED, "thermal scope fixed range high C (dev)");
    TS.zombie_core = cvar_reg_float("thermal_zombie_core", 24.0f, 15.0f, 37.0f, CVAR_LOCKED | CVAR_RULE,
                                    "zombie body core temperature (C): how dim they read on thermal (gameplay rule, docs/NIGHT_OPTICS.md)");
    cmd_register("thermal_debug", cmd_thermal_debug,
                 "thermal_debug [range <lo C> <hi C> | palette <n> | zoom <1..4>]: the thermal model (air, classes, emissivity), the sensor (AGC range, NUC timer, spot) and its overrides");
    cmd_register("thermal_probe", cmd_thermal_probe, "thermal_probe <entity | self>: the heat state of a character (face / chest / legs) or vehicle (engine / exhaust / brakes / tyres)");
    cmd_register("temp_set", cmd_temp_set, "temp_set <entity | self> <C | off>: override a character's core / a vehicle's engine temperature (thermal scope, dev)");
}
