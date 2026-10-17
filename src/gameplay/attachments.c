#include "attachments.h"
#include "vfx/vfx_library.h"
#include "core/log.h"
#include "core/platform.h"
#include "cJSON.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static const char *k_slot_names[ATT_SLOTS] = { "optic", "muzzle", "underbarrel", "magazine", "stock" };

int attach_slot_from_name(const char *s) {
    if (!s) return -1;
    for (int i = 0; i < ATT_SLOTS; i++)
        if (!strcmp(s, k_slot_names[i])) return i;
    if (!strcmp(s, "sight") || !strcmp(s, "scope")) return ATT_OPTIC;
    if (!strcmp(s, "barrel")) return ATT_MUZZLE;
    if (!strcmp(s, "grip")) return ATT_UNDERBARREL;
    if (!strcmp(s, "mag")) return ATT_MAGAZINE;
    return -1;
}

const char *attach_slot_name(int slot) { return slot >= 0 && slot < ATT_SLOTS ? k_slot_names[slot] : "?"; }

// ------------------------------------------------------------------ JSON helpers

static float jnum(const cJSON *o, const char *k, float def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}
static const char *jstr(const cJSON *o, const char *k, const char *def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : def;
}
static bool jbool(const cJSON *o, const char *k, bool def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : def;
}
static vec3 jvec3(const cJSON *o, const char *k, vec3 def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsArray(v) || cJSON_GetArraySize(v) < 3) return def;
    return v3((float)cJSON_GetArrayItem(v, 0)->valuedouble, (float)cJSON_GetArrayItem(v, 1)->valuedouble,
              (float)cJSON_GetArrayItem(v, 2)->valuedouble);
}
static vec4 jcolor(const cJSON *o, const char *k, vec4 def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsArray(v) || cJSON_GetArraySize(v) < 3) return def;
    float a = cJSON_GetArraySize(v) > 3 ? (float)cJSON_GetArrayItem(v, 3)->valuedouble : 1.0f;
    return v4((float)cJSON_GetArrayItem(v, 0)->valuedouble, (float)cJSON_GetArrayItem(v, 1)->valuedouble,
              (float)cJSON_GetArrayItem(v, 2)->valuedouble, a);
}

// { "pos": [x, y, z], "rot": [pitch, yaw, roll] deg } or a bare [x, y, z].
static Xform jxform(const cJSON *o, const char *k, Xform def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!v) return def;
    Xform x = def;
    if (cJSON_IsArray(v)) return xf_make(jvec3(o, k, def.t), def.r);
    x.t = jvec3(v, "pos", def.t);
    vec3 e = jvec3(v, "rot", v3(1e9f, 0, 0));
    if (e.x < 1e8f) x.r = quat_euler(e.y * DG_DEG2RAD, e.x * DG_DEG2RAD, e.z * DG_DEG2RAD);
    vec3 f = jvec3(v, "fingers", v3(0, 0, 0)), b = jvec3(v, "back", v3(0, 0, 0));
    if (v3_len2(f) > 0 && v3_len2(b) > 0) x.r = item_palm_frame(f, b);
    return x;
}

static int sight_from(const char *s) {
    if (!strcmp(s, "reddot") || !strcmp(s, "red_dot") || !strcmp(s, "holo")) return SIGHT_REDDOT;
    if (!strcmp(s, "scope")) return SIGHT_SCOPE;
    return SIGHT_IRON;
}

static int reticle_from(const char *s) {
    if (!strcmp(s, "circle_dot")) return RET_CIRCLE_DOT;
    if (!strcmp(s, "duplex")) return RET_DUPLEX;
    if (!strcmp(s, "mildot")) return RET_MILDOT;
    return RET_DOT;
}

// ------------------------------------------------------------------ loading

static void parse_attachment(AttachmentDef *a, const cJSON *j, const char *file) {
    memset(a, 0, sizeof(*a));
    dg_strcpy(a->file, sizeof(a->file), file);
    dg_strcpy(a->id, sizeof(a->id), jstr(j, "id", "attachment"));
    dg_strcpy(a->name, sizeof(a->name), jstr(j, "name", a->id));
    a->slot = attach_slot_from_name(jstr(j, "slot", "optic"));
    if (a->slot < 0) { log_warn("attachments: %s: unknown slot, using optic", file); a->slot = ATT_OPTIC; }
    a->sight = a->slot == ATT_OPTIC ? sight_from(jstr(j, "sight", "reddot")) : SIGHT_IRON;
    a->mount = jxform(j, "mount", xf_identity());
    a->sight_pt = jvec3(j, "sight_socket", v3(0, 0.03f, 0));
    const cJSON *er = cJSON_GetObjectItemCaseSensitive(j, "eye_relief");
    a->eye_relief = cJSON_IsNumber(er) ? (float)er->valuedouble : 0.0f;   // "weapon" / missing = keep the stock weld
    a->zoom = fmaxf(1.0f, jnum(j, "zoom", 1.0f));
    a->ads_fov = jnum(j, "ads_fov", 0.0f);
    a->main_zoom = fmaxf(1.0f, jnum(j, "main_zoom", 1.15f));
    a->rail_offset = jnum(j, "rail_offset", 0.0f);
    const cJSON *ro = cJSON_GetObjectItemCaseSensitive(j, "rail_offset");
    a->rail_auto = (cJSON_IsString(ro) && !strcmp(ro->valuestring, "auto")) || (!ro && a->sight == SIGHT_SCOPE);
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(j, "reticle");
    a->reticle.shape = reticle_from(jstr(r, "shape", a->sight == SIGHT_SCOPE ? "duplex" : "dot"));
    a->reticle.moa = jnum(r, "moa", 2.0f);
    a->reticle.outer_moa = jnum(r, "outer_moa", 32.0f);
    vec4 c = jcolor(r, "color", v4(1.0f, 0.06f, 0.03f, 1));
    a->reticle.color = v3(c.x, c.y, c.z);
    a->reticle.brightness = jnum(r, "brightness", 1.0f);
    a->reticle.ffp = jbool(r, "ffp", false);
    a->reticle.illuminated = jbool(r, "illuminated", a->sight == SIGHT_REDDOT);
    a->parallax_m = jnum(r, "parallax_m", 50.0f);
    const cJSON *l = cJSON_GetObjectItemCaseSensitive(j, "lens");
    a->lens_pos = jvec3(l, "pos", a->sight_pt);
    a->lens_radius = jnum(l, "radius", 0.012f);
    a->objective_pos = jvec3(l, "objective", v3_add(a->lens_pos, v3(0, 0, -0.2f)));
    a->objective_radius = jnum(l, "objective_radius", a->lens_radius);
    a->eye_box = jnum(l, "eye_box", 0.006f);
    a->eye_box_depth = jnum(l, "eye_box_depth", 0.03f);
    vec4 t = jcolor(l, "tint", v4(0.92f, 0.97f, 1.0f, 1));
    a->glass_tint = v3(t.x, t.y, t.z);
    a->dust = jnum(l, "dust", 0.25f);
    a->fresnel = jnum(l, "fresnel", 0.04f);
    a->chromatic = jnum(l, "chromatic", 1.5f);
    a->dof_near_blur = jnum(j, "ads_dof_near_blur", a->slot == ATT_OPTIC ? 2.0f : 0.0f);
    const cJSON *m = cJSON_GetObjectItemCaseSensitive(j, "modifiers");
    a->ads_time = jnum(m, "ads_time", jnum(j, "ads_time", 1.0f));
    a->sway_scale = jnum(m, "sway_scale", jnum(j, "sway_scale", 1.0f));
    a->recoil_v = jnum(m, "recoil_vertical", 1.0f);
    a->recoil_h = jnum(m, "recoil_horizontal", 1.0f);
    a->cam_kick = jnum(m, "camera_kick", 1.0f);
    a->vm_kick = jnum(m, "viewmodel_kick", 1.0f);
    a->spread_hip = jnum(m, "spread_hip", 1.0f);
    a->spread_ads = jnum(m, "spread_ads", 1.0f);
    a->damage = jnum(m, "damage", 1.0f);
    a->range = jnum(m, "range", 1.0f);
    a->mag_mult = jnum(m, "mag_size", 1.0f);
    a->reload_time = jnum(m, "reload_time", 1.0f);
    a->muzzle_len = jnum(j, "muzzle_length", 0.0f);
    const cJSON *md = cJSON_GetObjectItemCaseSensitive(j, "muzzle_device");
    a->bal_device = cJSON_IsObject(md);
    a->muzzle_device = vfx_muzzle_device_from_name(a->bal_device ? jstr(md, "kind", "") : jstr(j, "muzzle_device", ""));
    const cJSON *gl = cJSON_GetObjectItemCaseSensitive(j, "grip_l");
    if (gl) { a->has_grip_l = true; a->grip_l = jxform(j, "grip_l", xf_identity()); }
    const cJSON *p;
    cJSON_ArrayForEach(p, cJSON_GetObjectItemCaseSensitive(j, "model")) {
        if (a->part_count >= ATT_MAX_PARTS) break;
        WeaponModelPart *mp = &a->parts[a->part_count++];
        mp->offset = jvec3(p, "offset", v3(0, 0, 0));
        mp->half = jvec3(p, "half", v3(0.01f, 0.01f, 0.01f));
        mp->color = jcolor(p, "color", v4(0.08f, 0.08f, 0.09f, 1));
    }
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

bool attachments_load(ItemDB *db, const char *dir_rel) {
    memset(db->attachments, 0, sizeof(db->attachments));
    db->attachment_count = 0;
    char dir[512];
    platform_data_path(dir_rel, dir, sizeof(dir));
    int count = 0;
    char **files = SDL_GlobDirectory(dir, "*.json", 0, &count);
    if (!files || count == 0) { log_warn("attachments: none in %s", dir); SDL_free(files); return false; }
    char *names[MAX_ATTACHMENTS * 2];
    int nn = DG_MIN(count, ARRAY_COUNT(names));
    for (int i = 0; i < nn; i++) names[i] = files[i];
    qsort(names, (size_t)nn, sizeof(names[0]), cmp_str);
    for (int i = 0; i < nn; i++) {
        if (db->attachment_count >= MAX_ATTACHMENTS) { log_warn("attachments: limit %d reached", MAX_ATTACHMENTS); break; }
        char rel[256], buf[512];
        snprintf(rel, sizeof(rel), "%s/%s", dir_rel, names[i]);
        char *text = platform_read_file(platform_data_path(rel, buf, sizeof(buf)), NULL);
        if (!text) continue;
        cJSON *root = cJSON_Parse(text);
        free(text);
        if (!root) { log_error("attachments: JSON parse error in %s", rel); continue; }
        parse_attachment(&db->attachments[db->attachment_count + 1], root, rel);
        db->attachment_count++;
        cJSON_Delete(root);
    }
    SDL_free(files);
    log_info("attachments: %d definitions", db->attachment_count);
    return true;
}

void attachments_parse_weapon_slots(WeaponDef *d, const cJSON *w) {
    memset(d->att_slots, 0, sizeof(d->att_slots));
    const cJSON *slots = cJSON_GetObjectItemCaseSensitive(w, "attachment_slots");
    const cJSON *s;
    cJSON_ArrayForEach(s, slots) {
        int k = attach_slot_from_name(s->string);
        if (k < 0) { log_warn("weapon %s: unknown attachment slot '%s'", d->id, s->string ? s->string : "?"); continue; }
        WeaponAttachSlot *ws = &d->att_slots[k];
        ws->present = true;
        // Muzzle slots default to the barrel tip (the derived muzzle is 1 cm in front of the face).
        vec3 def = k == ATT_MUZZLE ? v3_add(d->muzzle_offset, v3(0, 0, 0.01f)) : v3(0, 0, 0);
        ws->socket = xf_make(jvec3(s, "pos", def), quat_identity());
        vec3 e = jvec3(s, "rot", v3(1e9f, 0, 0));
        if (e.x < 1e8f) ws->socket.r = quat_euler(e.y * DG_DEG2RAD, e.x * DG_DEG2RAD, e.z * DG_DEG2RAD);
        const cJSON *rail = cJSON_GetObjectItemCaseSensitive(s, "rail");
        if (cJSON_IsArray(rail) && cJSON_GetArraySize(rail) >= 2) {
            ws->rail_min = (float)cJSON_GetArrayItem(rail, 0)->valuedouble;
            ws->rail_max = (float)cJSON_GetArrayItem(rail, 1)->valuedouble;
        }
        const cJSON *a;
        cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(s, "allow")) {
            if (cJSON_IsString(a) && ws->allow_count < 8) dg_strcpy(ws->allow[ws->allow_count++], 32, a->valuestring);
        }
        cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(s, "hide_parts")) {
            if (cJSON_IsNumber(a) && a->valueint >= 0 && a->valueint < 8) ws->hide_parts |= (u8)(1u << a->valueint);
        }
        dg_strcpy(ws->def_id, sizeof(ws->def_id), jstr(s, "default", ""));
    }
}

void attachments_resolve(ItemDB *db) {
    for (int i = 0; i < db->weapon_count; i++) {
        WeaponDef *d = &db->weapons[i];
        memset(d->att_default, 0, sizeof(d->att_default));
        for (int k = 0; k < ATT_SLOTS; k++) {
            if (!d->att_slots[k].present || !d->att_slots[k].def_id[0]) continue;
            u8 id = attach_find(db, d->att_slots[k].def_id);
            const AttachmentDef *a = attach_def(db, id);
            if (!a || a->slot != k || !attach_allowed(d, a)) { log_warn("weapon %s: default %s '%s' not usable", d->id, k_slot_names[k], d->att_slots[k].def_id); continue; }
            d->att_default[k] = id;
        }
    }
}

// ------------------------------------------------------------------ queries

const AttachmentDef *attach_def(const ItemDB *db, u8 id) {
    return (id > 0 && id <= db->attachment_count) ? &db->attachments[id] : NULL;
}

u8 attach_find(const ItemDB *db, const char *id) {
    if (!id || !id[0]) return 0;
    for (int i = 1; i <= db->attachment_count; i++)
        if (!strcmp(db->attachments[i].id, id)) return (u8)i;
    return 0;
}

bool attach_allowed(const WeaponDef *wd, const AttachmentDef *a) {
    if (!wd || !a || a->slot < 0 || a->slot >= ATT_SLOTS) return false;
    const WeaponAttachSlot *s = &wd->att_slots[a->slot];
    if (!s->present) return false;
    if (!s->allow_count) return true;
    for (int i = 0; i < s->allow_count; i++)
        if (!strcmp(s->allow[i], a->id)) return true;
    return false;
}

// Iron sight eye relief of a weapon: its own override, else its hold style's (assets/human/holdpoint.json).
static const ProcParams *g_proc;
void attach_set_hold_params(const ProcParams *p) { g_proc = p; }
static float iron_relief(const WeaponDef *wd) {
    if (wd->sockets.hp.eye_relief > 0) return wd->sockets.hp.eye_relief;
    if (g_proc) {
        int i = proc_hold_style_index(g_proc, wd->hold_style);
        if (i >= 0 && g_proc->styles[i].hp.eye_relief > 0) return g_proc->styles[i].hp.eye_relief;
    }
    return 0.24f;
}

static float rail_position(const WeaponDef *wd, const AttachmentDef *a) {
    if (!a->rail_auto || a->eye_relief <= 0 || a->slot != ATT_OPTIC) return a->rail_offset;
    // Scopes slide on the rail like a real mounting: the cheek weld (eye -> grip distance of the
    // iron sights) and the scope's eye relief fix where its sight point must be.
    const WeaponAttachSlot *s = &wd->att_slots[a->slot];
    float weld = iron_relief(wd) + wd->iron_sight.z;          // eye -> grip along the view axis
    Xform at0 = xf_mul(s->socket, xf_inverse(a->mount));
    float z0 = xf_point(at0, a->sight_pt).z;                  // sight point z with no offset
    return (weld - a->eye_relief) - z0;
}

Xform attach_placement(const WeaponDef *wd, const AttachmentDef *a) {
    if (!wd || !a) return xf_identity();
    const WeaponAttachSlot *s = &wd->att_slots[a->slot];
    float off = rail_position(wd, a);
    if (s->rail_max > s->rail_min) off = clampf(off, s->rail_min, s->rail_max);
    else off = 0.0f;
    Xform rail = xf_mul(s->socket, xf_make(v3(0, 0, off), quat_identity()));
    return xf_mul(rail, xf_inverse(a->mount));
}

const AttachmentDef *attach_optic(const ItemDB *db, const u8 *att) {
    if (!att) return NULL;
    const AttachmentDef *a = attach_def(db, att[ATT_OPTIC]);
    return (a && a->slot == ATT_OPTIC) ? a : NULL;
}

void attach_defaults(const WeaponDef *wd, u8 out[ATT_SLOTS]) {
    memset(out, 0, ATT_SLOTS);
    if (wd) memcpy(out, wd->att_default, ATT_SLOTS);
}

void attach_weapon_effective(const ItemDB *db, const WeaponDef *base, const u8 *att, WeaponDef *out) {
    if (out != base) *out = *base;
    memset(out->att_applied, 0, sizeof(out->att_applied));
    if (!att) return;
    u8 hide = 0;
    for (int k = 0; k < ATT_SLOTS; k++) {
        const AttachmentDef *a = attach_def(db, att[k]);
        if (!a || a->slot != k || !attach_allowed(base, a)) continue;
        out->att_applied[k] = att[k];
        hide |= base->att_slots[k].hide_parts;
        Xform place = attach_placement(base, a);
        RecoilParams *r = &out->recoil;
        r->v_first *= a->recoil_v;
        r->v_per *= a->recoil_v;
        r->h_per *= a->recoil_h;
        r->h_min *= a->recoil_h;
        r->h_max *= a->recoil_h;
        r->cam_pitch *= a->cam_kick;
        r->cam_yaw *= a->cam_kick;
        r->cam_roll *= a->cam_kick;
        r->cam_back *= a->cam_kick;
        r->vm_back *= a->vm_kick;
        r->vm_up *= a->vm_kick;
        r->vm_pitch *= a->vm_kick;
        r->vm_yaw *= a->vm_kick;
        r->vm_roll *= a->vm_kick;
        r->base_hip *= a->spread_hip;
        r->base_ads *= a->spread_ads;
        r->ads_time *= a->ads_time;
        r->idle_sway *= a->sway_scale;
        r->scope_sway *= a->sway_scale;
        out->damage *= a->damage;
        out->range *= a->range;
        out->falloff_start *= a->range;
        out->falloff_end *= a->range;
        if (a->mag_mult != 1.0f && out->mag_size > 0) out->mag_size = DG_MAX(1, (int)lrintf((float)out->mag_size * a->mag_mult));
        out->reload_time *= a->reload_time;
        if (k == ATT_MUZZLE) {
            // Muzzle devices: flashes / tracers leave the device's front face.
            vec3 front = xf_point(place, v3(0, 0, -a->muzzle_len));
            if (a->muzzle_len > 0) {
                out->muzzle_offset = front;
                out->sockets.muzzle = front;
                out->sockets.length = fmaxf(out->sockets.length, -front.z);
            }
            if (a->muzzle_device >= 0) out->vfx_muzzle_device = a->muzzle_device;
            if (a->bal_device) dg_strcpy(out->muzzle_device_id, sizeof(out->muzzle_device_id), a->id);
        }
        if (k == ATT_OPTIC) {
            // The optic's sight point is the ADS socket. Eye relief: the optic's own, or (red dots)
            // the weapon's stock weld: the grip stays where the iron sights put it.
            vec3 sp = xf_point(place, a->sight_pt);
            if (a->eye_relief > 0) {
                out->sockets.hp.eye_relief = a->eye_relief;
                out->sockets.hp.eye_relief_add = 0;
            } else {
                out->sockets.hp.eye_relief_add += base->iron_sight.z - sp.z;
            }
            out->sockets.sight = sp;
        }
        if (a->has_grip_l) {
            Xform g = xf_mul(place, a->grip_l);
            out->sockets.grip_l = g;
            out->sockets.has_left = true;
            // The left hand's pose is the grip's own (attachment space socket).
            out->sockets.pose_id[0] = handpose_entry_hash(HPC_ATTACHMENT, a->id);
            out->sockets.has_pose_space[0] = true;
            out->sockets.pose_space[0] = place;
            out->support_offset = g.t;
        }
    }
    if (hide) {
        int n = 0;
        for (int i = 0; i < base->part_count; i++)
            if (!(hide & (1u << i))) out->parts[n++] = base->parts[i];
        out->part_count = n;
    }
}

// ADS FOVs are absolute (the same whatever the hip FOV in the settings): the weapon's own
// "ads_fov" for iron sights, scope magnification against a 75 deg reference view.
#define ADS_REF_FOV 75.0f
static float iron_fov(const WeaponDef *eff, float base_fov) {
    (void)base_fov;
    return (eff && eff->ads_fov > 1.0f) ? eff->ads_fov : ADS_REF_FOV * 0.8f;
}
static float zoom_fov(float fov, float zoom) { return 2.0f * atanf(tanf(fov * 0.5f * DG_DEG2RAD) / fmaxf(1.0f, zoom)) * DG_RAD2DEG; }

// Optics zoom on top of the weapon's own ADS zoom: a red dot is never wider than the irons.
float attach_sight_fov(const WeaponDef *eff, const AttachmentDef *optic, float base_fov) {
    if (optic) {
        if (optic->ads_fov > 1.0f) return optic->ads_fov;
        if (optic->sight == SIGHT_SCOPE) return zoom_fov(ADS_REF_FOV, optic->zoom);   // magnification of the scope picture
        return zoom_fov(iron_fov(eff, base_fov), optic->zoom);
    }
    return iron_fov(eff, base_fov);
}

float attach_main_ads_fov(const WeaponDef *eff, const AttachmentDef *optic, float base_fov, bool pip) {
    if (optic && optic->sight == SIGHT_SCOPE && pip) return zoom_fov(iron_fov(eff, base_fov), optic->main_zoom);   // around the PiP lens
    return attach_sight_fov(eff, optic, base_fov);
}

int attach_draw_parts(const ItemDB *db, const WeaponDef *base, const u8 *att, AttachDrawPart *out, int max) {
    if (!att || !base) return 0;
    int n = 0;
    for (int k = 0; k < ATT_SLOTS; k++) {
        const AttachmentDef *a = attach_def(db, att[k]);
        if (!a || a->slot != k || !attach_allowed(base, a)) continue;
        Xform place = attach_placement(base, a);
        for (int i = 0; i < a->part_count && n < max; i++) {
            out[n].center = xf_point(place, a->parts[i].offset);
            out[n].rot = place.r;
            out[n].half = a->parts[i].half;
            out[n].color = a->parts[i].color;
            n++;
        }
    }
    return n;
}
