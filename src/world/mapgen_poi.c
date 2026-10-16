#include "mapgen_poi.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ------------------------------------------------------------------ lots

typedef enum MpLotKind {
    LK_APARTMENT, LK_SHOP, LK_OFFICE, LK_CLINIC, LK_POLICE, LK_SUPERMARKET, LK_FUEL, LK_PARKING, LK_PARK,
    LK_HOUSE, LK_SHED, LK_PLAYGROUND,
    LK_HALL, LK_WAREHOUSE, LK_ADMIN, LK_WORKSHOP, LK_TANKS, LK_STACK, LK_PIPES, LK_YARD_CLUTTER,
    LK_FARMHOUSE, LK_BARN, LK_SILOS, LK_WATER_TANK, LK_TRACTOR, LK_HAY,
    LK_HANGAR, LK_TERMINAL, LK_CONTROL_TOWER, LK_FUEL_DEPOT, LK_PLANE_WRECK,
    LK_CONTAINERS, LK_GANTRY, LK_JIB_CRANE, LK_SHIP, LK_PIER, LK_QUAY, LK_HARBOR_OFFICE,
    LK_CABIN, LK_TENT, LK_CAMPFIRE, LK_RADIO_TOWER, LK_HUNT_STAND, LK_LOGS, LK_ROCKS,
    LK_DOCK, LK_BOAT,
    LK_PLANT, LK_EXCAVATOR, LK_DUMPTRUCK, LK_GRAVEL, LK_DRILL, LK_BUNKER,
    LK_FENCE, LK_CHECKPOINT, LK_CAR, LK_STREETLIGHTS,
    LK_COUNT
} MpLotKind;

static const char *const k_lot_names[LK_COUNT] = {
    "apartment", "shop", "office", "clinic", "police", "supermarket", "fuel", "parking", "park", "house", "shed", "playground",
    "hall", "warehouse", "admin", "workshop", "tanks", "smokestack", "pipes", "yard_clutter", "farmhouse", "barn", "silos", "water_tank",
    "tractor", "hay", "hangar", "terminal", "control_tower", "fuel_depot", "plane_wreck", "containers", "gantry", "jib_crane", "ship",
    "pier", "quay", "harbor_office", "cabin", "tent", "campfire", "radio_tower", "hunt_stand", "logs", "rocks", "dock", "boat", "plant",
    "excavator", "dumptruck", "gravel", "drill", "bunker", "fence", "checkpoint", "car", "streetlights",
};
const char *mappoi_lot_name(int kind) { return kind >= 0 && kind < LK_COUNT ? k_lot_names[kind] : "?"; }

static MpLot *add_lot(MapPois *p, int kind, int poi, vec2 pos, vec2 size, float yaw, int storeys, int flags, u32 seed, const char *tier) {
    if (p->lot_count == p->lot_cap) {
        p->lot_cap = p->lot_cap ? p->lot_cap * 2 : 1024;
        p->lots = (MpLot *)realloc(p->lots, sizeof(MpLot) * (size_t)p->lot_cap);
    }
    MpLot *l = &p->lots[p->lot_count++];
    memset(l, 0, sizeof(*l));
    l->kind = (u16)kind;
    l->poi = (u8)(poi + 1);
    l->pos = pos;
    l->size = size;
    l->yaw = yaw;
    l->storeys = (u8)storeys;
    l->flags = (u8)flags;
    l->seed = seed;
    dg_strcpy(l->tier, sizeof(l->tier), tier ? tier : "");
    if (flags & MPL_ENTERABLE) p->enterable_count++;
    return l;
}

// ------------------------------------------------------------------ helpers

static u32 C(float r, float g, float b) {
    u32 R = (u32)(saturate(r) * 255.0f + 0.5f), G = (u32)(saturate(g) * 255.0f + 0.5f), B = (u32)(saturate(b) * 255.0f + 0.5f);
    return R | (G << 8) | (B << 16) | (255u << 24);
}
static u32 Cv(vec3 c) { return C(c.x, c.y, c.z); }
static vec3 jitter_col(Rng *r, vec3 c, float amt) { float k = 1.0f + rng_range(r, -amt, amt); return v3_scale(c, k); }

static vec2 rot2(vec2 v, float yaw) {   // local (x, z) -> world offset, same convention as quat_yaw
    float c = cosf(yaw), s = sinf(yaw);
    return v2(v.x * c + v.y * s, -v.x * s + v.y * c);
}
static float yaw_facing(vec2 dir) { return atan2f(dir.x, dir.y); }   // local +Z along dir

static float jnum(const cJSON *o, const char *k, float def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}
static vec2 jv2(const cJSON *o, const char *k, vec2 def) {
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) < 2) return def;
    return v2((float)cJSON_GetArrayItem(a, 0)->valuedouble, (float)cJSON_GetArrayItem(a, 1)->valuedouble);
}
static const char *jstr(const cJSON *o, const char *k, const char *def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : def;
}

static bool in_poi(const MpPoi *poi, vec2 p, float margin) {
    vec2 l = rot2(v2_sub(p, poi->center), -poi->yaw);
    return fabsf(l.x) <= poi->half.x + margin && fabsf(l.y) <= poi->half.y + margin;
}

int mappoi_find(const MapPois *p, float x, float z) {
    for (int i = 0; i < p->poi_count; i++) if (in_poi(&p->pois[i], v2(x, z), 0)) return i;
    return -1;
}

// Footprint overlap test against earlier lots (rotated rectangles, separating axes).
static bool rect_overlap(vec2 ca, vec2 ha, float ya, vec2 cb, vec2 hb, float yb) {
    vec2 ax[4] = { rot2(v2(1, 0), ya), rot2(v2(0, 1), ya), rot2(v2(1, 0), yb), rot2(v2(0, 1), yb) };
    vec2 d = v2_sub(cb, ca);
    for (int i = 0; i < 4; i++) {
        vec2 a = ax[i];
        float pa = ha.x * fabsf(a.x * ax[0].x + a.y * ax[0].y) + ha.y * fabsf(a.x * ax[1].x + a.y * ax[1].y);
        float pb = hb.x * fabsf(a.x * ax[2].x + a.y * ax[2].y) + hb.y * fabsf(a.x * ax[3].x + a.y * ax[3].y);
        if (fabsf(d.x * a.x + d.y * a.y) > pa + pb) return false;
    }
    return true;
}
static bool lot_free(const MapPois *p, int from, vec2 c, vec2 size, float yaw, float margin) {
    vec2 h = v2(size.x * 0.5f + margin, size.y * 0.5f + margin);
    for (int i = from; i < p->lot_count; i++) {
        const MpLot *l = &p->lots[i];
        if (l->kind == LK_FENCE || l->kind == LK_STREETLIGHTS || l->kind == LK_CAR) continue;
        if (rect_overlap(c, h, yaw, l->pos, v2_scale(l->size, 0.5f), l->yaw)) return false;
    }
    return true;
}

// Distance from a footprint to the nearest road centreline sample (planning time: control polylines).
static float road_clear(const MapGen *g, vec2 c, vec2 size, float yaw) {
    float best = 1e9f;
    for (int r = 0; r < g->road_count; r++) {
        const vec2 *pts = g->road_ctrl + g->road_ctrl_first[r];
        int n = g->road_ctrl_count[r];
        for (int i = 0; i + 1 < n; i++) {
            // coarse: segment vs footprint corners + centre
            vec2 a = pts[i], b = pts[i + 1], ab = v2_sub(b, a);
            float l2 = ab.x * ab.x + ab.y * ab.y;
            for (int k = 0; k < 5; k++) {
                vec2 q = k == 4 ? c : v2_add(c, rot2(v2((k & 1 ? 0.5f : -0.5f) * size.x, (k & 2 ? 0.5f : -0.5f) * size.y), yaw));
                float t = l2 > 0 ? clampf(((q.x - a.x) * ab.x + (q.y - a.y) * ab.y) / l2, 0, 1) : 0;
                vec2 cl = v2_add(a, v2_scale(ab, t));
                float d = v2_len(v2_sub(q, cl)) - g->roads[r].width * 0.5f;
                best = fminf(best, d);
            }
        }
    }
    return best;
}

static void add_pad(MapGen *g, vec2 c, vec2 size, float yaw, float blend, int mat, float level, bool auto_level) {
    MgFeature f;
    memset(&f, 0, sizeof(f));
    f.kind = MGF_PAD;
    f.pos = c;
    f.size = v2_scale(size, 0.5f);
    f.yaw = yaw;
    f.blend = blend;
    f.material = mat;
    f.level = level;
    f.auto_level = auto_level;
    mapgen_add_feature(g, &f);
}
static void add_paint(MapGen *g, vec2 c, vec2 size, float yaw, int mat) {
    MgFeature f;
    memset(&f, 0, sizeof(f));
    f.kind = MGF_PAINT;
    f.pos = c;
    f.size = v2_scale(size, 0.5f);
    f.yaw = yaw;
    f.material = mat;
    mapgen_add_feature(g, &f);
}
static void add_forest(MapGen *g, vec2 c, vec2 r, float density, float pine) {
    MgFeature f;
    memset(&f, 0, sizeof(f));
    f.kind = MGF_FOREST;
    f.pos = c;
    f.size = r;
    f.param = density;
    f.height = pine;
    mapgen_add_feature(g, &f);
}
static void lot_keepout(MapGen *g, const MpLot *l, float margin) {
    mapgen_add_keepout(g, l->pos, v2(l->size.x * 0.5f + margin, l->size.y * 0.5f + margin), l->yaw, l->kind);
}
static int road2(MapGen *g, int cls, float w, vec2 a, vec2 b, const char *name) {
    vec2 pts[2] = { a, b };
    return mapgen_add_road(g, cls, w, pts, 2, false, false, name);
}

// ------------------------------------------------------------------ planners

typedef struct Plan { MapPois *p; MapGen *g; int poi; const cJSON *j; Rng rng; } Plan;

// Main City: a street grid (avenue through the middle), perimeter blocks of 2-6 storey buildings,
// and special blocks (clinic, police, supermarket, fuel station, parking, park).
static void plan_city(Plan *pl) {
    MpPoi *poi = &pl->p->pois[pl->poi];
    MapGen *g = pl->g;
    vec2 c = poi->center, h = poi->half;
    float bx = jnum(pl->j, "block_x", 76), bz = jnum(pl->j, "block_z", 70);
    int nx = (int)floorf(2 * h.x / bx), nz = (int)floorf(2 * h.y / bz);
    float x0 = c.x - (float)nx * bx * 0.5f, z0 = c.y - (float)nz * bz * 0.5f;
    add_pad(g, c, v2(2 * h.x + 30, 2 * h.y + 30), 0, 45, MGM_CONCRETE, 0, true);
    int avenue = nz / 2;
    for (int i = 0; i <= nx; i++) road2(g, MGR_STREET, 9, v2(x0 + (float)i * bx, z0 - 6), v2(x0 + (float)i * bx, z0 + (float)nz * bz + 6), "city street");
    for (int j = 0; j <= nz; j++) road2(g, MGR_STREET, j == avenue ? 13.0f : 9.0f, v2(x0 - 6, z0 + (float)j * bz), v2(x0 + (float)nx * bx + 6, z0 + (float)j * bz), j == avenue ? "city avenue" : "city street");
    // special blocks (block coordinates from the description or defaults around the centre)
    const char *spec_names[6] = { "clinic", "police", "supermarket", "fuel", "parking", "park" };
    int spec_kind[6] = { LK_CLINIC, LK_POLICE, LK_SUPERMARKET, LK_FUEL, LK_PARKING, LK_PARK };
    int sx[6] = { 1, nx - 2, 2, nx - 1, nx - 3, 0 }, sz[6] = { avenue, avenue - 1, avenue - 1, 0, nz - 1, nz - 1 };
    const cJSON *sb = cJSON_GetObjectItemCaseSensitive(pl->j, "special_blocks");
    for (int k = 0; k < 6; k++) {
        const cJSON *e = cJSON_GetObjectItemCaseSensitive(sb, spec_names[k]);
        if (cJSON_IsArray(e) && cJSON_GetArraySize(e) >= 2) { sx[k] = cJSON_GetArrayItem(e, 0)->valueint; sz[k] = cJSON_GetArrayItem(e, 1)->valueint; }
    }
    int enter_budget = 0;
    for (int bi = 0; bi < nx; bi++)
        for (int bj = 0; bj < nz; bj++) {
            float cx = x0 + ((float)bi + 0.5f) * bx, cz = z0 + ((float)bj + 0.5f) * bz;
            float iw = bx - 9.0f - 6.0f, id = bz - 9.0f - 6.0f;   // inside the sidewalks
            int special = -1;
            for (int k = 0; k < 6; k++) if (sx[k] == bi && sz[k] == bj) special = k;
            u32 seed = (u32)(bi * 131 + bj * 977) ^ pl->p->seed;
            if (special >= 0) {
                int kind = spec_kind[special];
                MpLot *l = NULL;
                if (kind == LK_CLINIC) l = add_lot(pl->p, kind, pl->poi, v2(cx, cz + id * 0.2f), v2(iw * 0.8f, 22), 0, 3, MPL_ENTERABLE | MPL_ROOF, seed, "medical");
                else if (kind == LK_POLICE) l = add_lot(pl->p, kind, pl->poi, v2(cx, cz - id * 0.15f), v2(iw * 0.7f, 20), 0, 2, MPL_ENTERABLE | MPL_LOCKED, seed, "police");
                else if (kind == LK_SUPERMARKET) l = add_lot(pl->p, kind, pl->poi, v2(cx, cz + id * 0.18f), v2(iw * 0.85f, id * 0.55f), 0, 1, MPL_ENTERABLE, seed, "food");
                else if (kind == LK_FUEL) l = add_lot(pl->p, kind, pl->poi, v2(cx, cz), v2(iw, id), 0, 1, MPL_ENTERABLE, seed, "garage");
                else if (kind == LK_PARKING) { l = add_lot(pl->p, kind, pl->poi, v2(cx, cz), v2(iw, id), 0, 1, 0, seed, "household"); add_paint(g, v2(cx, cz), v2(iw, id), 0, MGM_ASPHALT); }
                else { l = add_lot(pl->p, kind, pl->poi, v2(cx, cz), v2(iw, id), 0, 1, 0, seed, "household"); add_paint(g, v2(cx, cz), v2(iw, id), 0, MGM_GRASS); add_forest(g, v2(cx, cz), v2(iw * 0.5f, id * 0.5f), 1.2f, 0.2f); }
                if (kind != LK_PARK && kind != LK_PARKING) lot_keepout(g, l, 2);
                continue;
            }
            // perimeter buildings: north / south rows take the full width, east / west the middle
            float dep = 15.0f;
            for (int side = 0; side < 4; side++) {
                bool ns = side < 2;
                float len = ns ? iw : id - 2 * dep;
                float yaw = side == 0 ? 0.0f : (side == 1 ? DG_PI : (side == 2 ? DG_PI * 0.5f : -DG_PI * 0.5f));
                // side 0: south row facing south? front faces the street: local +Z points out of the block
                vec2 out = side == 0 ? v2(0, 1) : (side == 1 ? v2(0, -1) : (side == 2 ? v2(1, 0) : v2(-1, 0)));
                yaw = yaw_facing(out);
                vec2 along = v2(out.y, -out.x);
                vec2 edge = v2_add(v2(cx, cz), v2_scale(out, (ns ? id : iw) * 0.5f - dep * 0.5f));
                float u = -len * 0.5f;
                while (u < len * 0.5f - 6.0f) {
                    float fw = rng_range(&pl->rng, 13.0f, 24.0f);
                    if (u + fw > len * 0.5f) fw = len * 0.5f - u;
                    if (fw < 8.0f) break;
                    vec2 pc = v2_add(edge, v2_scale(along, u + fw * 0.5f));
                    bool on_avenue = (bj == avenue && side == 1) || (bj == avenue - 1 && side == 0);
                    int kind, st;
                    float rr = rng_float(&pl->rng);
                    if (on_avenue) { kind = LK_SHOP; st = 2 + (int)(rr * 2.0f); }
                    else if (rr < 0.55f) { kind = LK_APARTMENT; st = 4 + (int)(rng_float(&pl->rng) * 3.0f); }
                    else if (rr < 0.8f) { kind = LK_OFFICE; st = 3 + (int)(rng_float(&pl->rng) * 3.0f); }
                    else { kind = LK_SHOP; st = 2 + (int)(rng_float(&pl->rng) * 2.0f); }
                    int flags = rng_float(&pl->rng) < 0.12f ? MPL_DAMAGED : 0;
                    bool enter = (enter_budget < 2 + bi + bj * nx) && rng_float(&pl->rng) < 0.18f && fw >= 12.0f;
                    if (enter) { flags |= MPL_ENTERABLE; enter_budget++; if (kind == LK_APARTMENT) flags |= MPL_ROOF; }
                    const char *tier = kind == LK_SHOP ? (rng_float(&pl->rng) < 0.5f ? "food" : "household") : (kind == LK_OFFICE ? "office" : "household");
                    MpLot *l = add_lot(pl->p, kind, pl->poi, pc, v2(fw - 0.6f, dep - 0.5f), yaw, st, flags, seed + (u32)(side * 7919 + (int)(u * 10)), tier);
                    l->variant = (u8)(rng_next(&pl->rng) & 7);
                    lot_keepout(g, l, 0.5f);
                    u += fw;
                }
            }
        }
    // street furniture, signals and wrecks along the grid (built from these lots)
    add_lot(pl->p, LK_STREETLIGHTS, pl->poi, c, v2(2 * h.x, 2 * h.y), 0, 0, 0, pl->p->seed ^ 0x51u, "")->variant = 1;
    poi->enterable = pl->p->enterable_count;
}

// Suburbs: curved residential streets with detached houses, garages, fences, sheds, a park.
static void plan_suburbs(Plan *pl) {
    MpPoi *poi = &pl->p->pois[pl->poi];
    MapGen *g = pl->g;
    vec2 c = poi->center, h = poi->half;
    int streets = (int)jnum(pl->j, "streets", 4);
    int first_road = g->road_count;
    for (int s = 0; s < streets; s++) {
        float zc = c.y - h.y + (2.0f * h.y) * ((float)s + 0.5f) / (float)streets;
        vec2 pts[9];
        float ph = rng_range(&pl->rng, 0, 6.28f), amp = rng_range(&pl->rng, 10, 22);
        for (int k = 0; k < 9; k++) {
            float x = c.x - h.x + 2.0f * h.x * (float)k / 8.0f;
            pts[k] = v2(x, zc + sinf(ph + (float)k * 0.9f) * amp);
        }
        mapgen_add_road(g, MGR_STREET, 7.5f, pts, 9, true, false, "suburb street");
    }
    // connectors (north-south), curved
    for (int k = 0; k < 3; k++) {
        float xc = c.x - h.x * 0.75f + h.x * 0.75f * (float)k;
        vec2 pts[5];
        for (int i = 0; i < 5; i++) {
            float z = c.y - h.y + 2.0f * h.y * (float)i / 4.0f;
            pts[i] = v2(xc + sinf((float)i * 1.3f + (float)k) * 14.0f, z);
        }
        pts[0].y += 14.0f;
        pts[4].y -= 14.0f;
        mapgen_add_road(g, MGR_STREET, 7.0f, pts, 5, true, false, "suburb lane");
    }
    for (int r = first_road; r < g->road_count; r++) g->roads[r].keep_level = false;   // gentle terrain: smooth profiles
    // park + playground in the middle
    vec2 park = v2_add(c, v2(rng_range(&pl->rng, -h.x * 0.3f, h.x * 0.3f), 0));
    add_paint(g, park, v2(46, 30), 0, MGM_GRASS);
    MpLot *pk = add_lot(pl->p, LK_PLAYGROUND, pl->poi, park, v2(40, 26), 0, 1, 0, pl->p->seed ^ 77u, "household");
    int first_lot = pl->p->lot_count - 1;
    // houses along every street, both sides
    for (int r = first_road; r < first_road + streets; r++) {
        const vec2 *pts = g->road_ctrl + g->road_ctrl_first[r];
        int n = g->road_ctrl_count[r];
        // walk the control polyline in 23 m steps
        float acc = 0, next = 12;
        for (int i = 0; i + 1 < n; i++) {
            vec2 a = pts[i], b = pts[i + 1];
            float l = v2_len(v2_sub(b, a));
            vec2 dir = v2_scale(v2_sub(b, a), 1.0f / l);
            while (acc + l >= next) {
                vec2 p = v2_add(a, v2_scale(dir, next - acc));
                next += rng_range(&pl->rng, 21, 27);
                for (int side = -1; side <= 1; side += 2) {
                    vec2 nrm = v2((float)side * dir.y, (float)-side * dir.x);   // outward from the street
                    float w = rng_range(&pl->rng, 9.5f, 13.0f), d = rng_range(&pl->rng, 8.0f, 10.5f);
                    vec2 hc = v2_add(p, v2_scale(nrm, 13.0f + d * 0.5f));
                    float yaw = yaw_facing(v2_scale(nrm, -1.0f));   // front faces the street
                    if (!in_poi(poi, hc, -6)) continue;
                    if (!lot_free(pl->p, first_lot, hc, v2(w + 6, d + 14), yaw, 1.0f)) continue;
                    if (road_clear(g, hc, v2(w, d), yaw) < 6.0f) continue;
                    if (rng_float(&pl->rng) < 0.08f) continue;   // empty lot
                    int st = rng_float(&pl->rng) < 0.35f ? 2 : 1;
                    int flags = (rng_float(&pl->rng) < 0.38f ? MPL_ENTERABLE : 0) | (rng_float(&pl->rng) < 0.18f ? MPL_DAMAGED : 0);
                    MpLot *hl = add_lot(pl->p, LK_HOUSE, pl->poi, hc, v2(w, d), yaw, st, flags, rng_next(&pl->rng), "household");
                    hl->variant = (u8)(rng_next(&pl->rng) & 15);
                    lot_keepout(g, hl, 1.0f);
                    // driveway to the garage side
                    vec2 drive = v2_add(p, v2_add(v2_scale(nrm, 7.0f), rot2(v2(w * 0.5f + 2.0f, 0), yaw)));
                    add_paint(g, drive, v2(3.5f, 8.0f), yaw, MGM_CONCRETE);
                    if (rng_float(&pl->rng) < 0.45f) {
                        vec2 sc = v2_add(hc, rot2(v2(rng_range(&pl->rng, -3, 3), -d * 0.5f - 7.0f), yaw));
                        if (lot_free(pl->p, first_lot, sc, v2(3.5f, 3), yaw, 0.5f)) lot_keepout(g, add_lot(pl->p, LK_SHED, pl->poi, sc, v2(3, 2.6f), yaw, 1, rng_float(&pl->rng) < 0.6f ? MPL_ENTERABLE : 0, rng_next(&pl->rng), "garage"), 0.5f);
                    }
                }
            }
            acc += l;
        }
    }
    (void)pk;
    poi->enterable = pl->p->enterable_count;
}

// Fenced site with a gate facing `gate_dir` (unit), a checkpoint at the gate.
static void plan_fence_rect(Plan *pl, vec2 c, vec2 size, float yaw, vec2 gate_world, float gate_w, int style) {
    vec2 h = v2_scale(size, 0.5f);
    vec2 corners[4] = { v2(-h.x, -h.y), v2(h.x, -h.y), v2(h.x, h.y), v2(-h.x, h.y) };
    for (int e = 0; e < 4; e++) {
        vec2 a = v2_add(c, rot2(corners[e], yaw)), b = v2_add(c, rot2(corners[(e + 1) % 4], yaw));
        vec2 mid = v2_scale(v2_add(a, b), 0.5f), d = v2_sub(b, a);
        float len = v2_len(d);
        MpLot *f = add_lot(pl->p, LK_FENCE, pl->poi, mid, v2(len, 2.4f), atan2f(d.x, d.y) - DG_PI * 0.5f, 0, 0, rng_next(&pl->rng), "");
        f->variant = (u8)style;
        // the gate: an opening where the gate point projects onto this edge
        vec2 ad = v2_scale(d, 1.0f / len), ag = v2_sub(gate_world, a);
        float t = ag.x * ad.x + ag.y * ad.y, off = fabsf(ag.x * ad.y - ag.y * ad.x);
        f->storeys = 0;
        if (off < 40.0f && t > gate_w && t < len - gate_w) {
            f->storeys = 1;                 // has a gate
            f->size.y = t;                  // along-edge position of the gate (m from a)
            f->tier[0] = (char)DG_MIN((int)gate_w, 120);
            vec2 gp = v2_add(a, v2_scale(ad, t));
            vec2 inward = v2_sub(c, gp);
            float il = v2_len(inward);
            MpLot *ck = add_lot(pl->p, LK_CHECKPOINT, pl->poi, v2_add(gp, v2_add(v2_scale(inward, 6.0f / il), v2_scale(ad, gate_w * 0.5f + 3.0f))), v2(3, 3), yaw_facing(v2_scale(inward, -1.0f / il)), 1, 0, rng_next(&pl->rng), "police");
            lot_keepout(pl->g, ck, 1);
        }
    }
}

static MpLot *keep_lot(Plan *pl, MpLot *l, float margin) { lot_keepout(pl->g, l, margin); return l; }

// Factory: concrete yard, fence + checkpoint, halls, warehouses, admin, workshop, tanks, stacks.
static void plan_factory(Plan *pl) {
    MpPoi *poi = &pl->p->pois[pl->poi];
    MapGen *g = pl->g;
    vec2 c = poi->center, h = poi->half;
    float yaw = poi->yaw;
    add_pad(g, c, v2(2 * h.x + 10, 2 * h.y + 10), yaw, 35, MGM_CONCRETE, 0, true);
    vec2 gate = jv2(pl->j, "gate", v2(c.x - h.x, c.y - h.y * 0.4f));
    plan_fence_rect(pl, c, v2(2 * h.x, 2 * h.y), yaw, gate, 12, 0);
#define AT(lx, lz) v2_add(c, rot2(v2((lx) * h.x, (lz) * h.y), yaw))
    keep_lot(pl, add_lot(pl->p, LK_HALL, pl->poi, AT(-0.35f, 0.42f), v2(70, 32), yaw, 1, MPL_ENTERABLE, 11, "industrial"), 2);
    keep_lot(pl, add_lot(pl->p, LK_HALL, pl->poi, AT(0.38f, 0.45f), v2(60, 30), yaw, 1, MPL_ENTERABLE, 12, "industrial"), 2);
    keep_lot(pl, add_lot(pl->p, LK_WAREHOUSE, pl->poi, AT(0.45f, -0.25f), v2(40, 24), yaw + DG_PI * 0.5f, 1, MPL_ENTERABLE, 13, "industrial"), 2);
    keep_lot(pl, add_lot(pl->p, LK_WAREHOUSE, pl->poi, AT(0.1f, -0.55f), v2(46, 22), yaw, 1, 0, 14, "industrial"), 2);
    keep_lot(pl, add_lot(pl->p, LK_ADMIN, pl->poi, AT(-0.62f, -0.5f), v2(26, 15), yaw + DG_PI, 3, MPL_ENTERABLE | MPL_ROOF, 15, "office"), 2);
    keep_lot(pl, add_lot(pl->p, LK_WORKSHOP, pl->poi, AT(-0.2f, -0.2f), v2(18, 12), yaw, 1, MPL_ENTERABLE, 16, "industrial"), 2);
    keep_lot(pl, add_lot(pl->p, LK_TANKS, pl->poi, AT(-0.72f, 0.1f), v2(34, 26), yaw, 1, 0, 17, ""), 2);
    keep_lot(pl, add_lot(pl->p, LK_STACK, pl->poi, AT(0.05f, 0.78f), v2(6, 6), yaw, 48, 0, 18, ""), 1);
    keep_lot(pl, add_lot(pl->p, LK_STACK, pl->poi, AT(0.15f, 0.8f), v2(5, 5), yaw, 40, 0, 19, ""), 1);
    keep_lot(pl, add_lot(pl->p, LK_PIPES, pl->poi, AT(0.02f, 0.45f), v2(14, 56), yaw, 1, 0, 20, ""), 1);
    keep_lot(pl, add_lot(pl->p, LK_CONTAINERS, pl->poi, AT(-0.1f, 0.05f), v2(40, 30), yaw, 2, 0, 21, "industrial"), 1);
    add_lot(pl->p, LK_YARD_CLUTTER, pl->poi, AT(0.0f, -0.05f), v2(2 * h.x * 0.9f, 2 * h.y * 0.9f), yaw, 1, 0, 22, "industrial");
    keep_lot(pl, add_lot(pl->p, LK_HUNT_STAND, pl->poi, AT(0.95f, 0.93f), v2(2.2f, 2.2f), yaw, 1, 0, 23, "police"), 1);   // watchtower
#undef AT
    poi->enterable = 5;
}

// Farms: parcels of crops between dirt tracks, farmsteads (farmhouse, barn, silos, sheds, machines).
static void plan_farms(Plan *pl) {
    MpPoi *poi = &pl->p->pois[pl->poi];
    MapGen *g = pl->g;
    vec2 c = poi->center, h = poi->half;
    const cJSON *fs = cJSON_GetObjectItemCaseSensitive(pl->j, "farmsteads");
    vec2 steads[6];
    int ns = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, fs) {
        if (ns >= 6 || !cJSON_IsArray(e)) continue;
        steads[ns++] = v2((float)cJSON_GetArrayItem(e, 0)->valuedouble, (float)cJSON_GetArrayItem(e, 1)->valuedouble);
    }
    float px = jnum(pl->j, "parcel_x", 95), pz = jnum(pl->j, "parcel_z", 70);
    int nx = (int)(2 * h.x / px), nz = (int)(2 * h.y / pz);
    float x0 = c.x - (float)nx * px * 0.5f, z0 = c.y - (float)nz * pz * 0.5f;
    // field tracks between the parcel columns, interrupted at the farmsteads (yards / buildings)
    for (int i = 1; i < nx; i++) {
        float x = x0 + (float)i * px;
        int run0 = -1;
        for (int j = 0; j <= nz; j++) {
            bool blocked = j == nz;
            for (int k = 0; k < ns && !blocked; k++) {
                float zc = z0 + ((float)j + 0.5f) * pz;
                if (fabsf(x - steads[k].x) < 50.0f && fabsf(zc - steads[k].y) < pz * 0.5f + 32.0f) blocked = true;
            }
            if (!blocked && run0 < 0) run0 = j;
            if (blocked && run0 >= 0) {
                road2(g, MGR_DIRT, 4.0f, v2(x, z0 + (float)run0 * pz + 4), v2(x, z0 + (float)j * pz - 4), "field track");
                run0 = -1;
            }
        }
    }
    int mats[3] = { MGM_FIELD, MGM_CROP, MGM_PLOWED };
    for (int i = 0; i < nx; i++)
        for (int j = 0; j < nz; j++) {
            vec2 pc = v2(x0 + ((float)i + 0.5f) * px, z0 + ((float)j + 0.5f) * pz);
            bool stead = false;
            for (int k = 0; k < ns; k++) if (fabsf(pc.x - steads[k].x) < px * 0.6f && fabsf(pc.y - steads[k].y) < pz * 0.6f) stead = true;
            if (stead) continue;
            add_paint(g, pc, v2(px - 7, pz - 5), 0, mats[rng_next(&pl->rng) % 3]);
        }
    for (int k = 0; k < ns; k++) {
        vec2 s = steads[k];
        float yaw = rng_range(&pl->rng, -0.3f, 0.3f);
        add_pad(g, s, v2(70, 56), yaw, 20, MGM_DIRT, 0, true);
        add_paint(g, v2_add(s, rot2(v2(-24, 18), yaw)), v2(16, 10), yaw, MGM_PLOWED);   // vegetable garden
#define AT(lx, lz) v2_add(s, rot2(v2(lx, lz), yaw))
        keep_lot(pl, add_lot(pl->p, LK_FARMHOUSE, pl->poi, AT(-20, -12), v2(12, 10), yaw + DG_PI, 2, MPL_ENTERABLE, rng_next(&pl->rng), "farm"), 1.5f);
        keep_lot(pl, add_lot(pl->p, LK_BARN, pl->poi, AT(10, 8), v2(24, 16), yaw, 1, MPL_ENTERABLE, rng_next(&pl->rng), "farm"), 1.5f);
        keep_lot(pl, add_lot(pl->p, LK_SILOS, pl->poi, AT(28, -10), v2(16, 6), yaw, 1, 0, rng_next(&pl->rng), ""), 1);
        keep_lot(pl, add_lot(pl->p, LK_SHED, pl->poi, AT(-2, -16), v2(7, 5), yaw, 1, MPL_ENTERABLE, rng_next(&pl->rng), "garage"), 1);
        keep_lot(pl, add_lot(pl->p, LK_WATER_TANK, pl->poi, AT(-30, 4), v2(5, 5), yaw, 1, 0, rng_next(&pl->rng), ""), 1);
        add_lot(pl->p, LK_TRACTOR, pl->poi, AT(2, -4), v2(4, 2.5f), yaw + 0.6f, 1, 0, rng_next(&pl->rng), "");
        add_lot(pl->p, LK_HAY, pl->poi, AT(22, 18), v2(10, 6), yaw, 1, 0, rng_next(&pl->rng), "");
        MpLot *f = add_lot(pl->p, LK_FENCE, pl->poi, AT(0, -28), v2(66, 1.1f), yaw, 0, 0, rng_next(&pl->rng), "");
        f->variant = 1;
        f = add_lot(pl->p, LK_FENCE, pl->poi, AT(-34, 0), v2(56, 1.1f), yaw + DG_PI * 0.5f, 0, 0, rng_next(&pl->rng), "");
        f->variant = 1;
#undef AT
    }
    poi->enterable = ns * 3;
}

// Airfield: runway + taxiway + apron (roads / pads), hangars, terminal, tower, fuel, a wreck, fence.
static void plan_airfield(Plan *pl) {
    MpPoi *poi = &pl->p->pois[pl->poi];
    MapGen *g = pl->g;
    vec2 a = jv2(pl->j, "runway_a", v2(poi->center.x - 300, poi->center.y)), b = jv2(pl->j, "runway_b", v2(poi->center.x + 300, poi->center.y));
    vec2 d = v2_sub(b, a);
    float len = v2_len(d);
    vec2 dir = v2_scale(d, 1.0f / len), nrm = v2(-dir.y, dir.x);   // nrm: to the left of a->b
    float side = jnum(pl->j, "apron_side", 1.0f);                  // +1: apron on the left side
    vec2 mid = v2_scale(v2_add(a, b), 0.5f);
    float ryaw = atan2f(dir.x, dir.y);
    // one flat pad under it all so runway, taxiway and apron share a level
    add_pad(g, v2_add(mid, v2_scale(nrm, side * 45)), v2(130, len + 40), ryaw, 60, -1, 0, true);
    road2(g, MGR_RUNWAY, 40, a, b, "runway");
    vec2 ta = v2_add(a, v2_add(v2_scale(nrm, side * 70), v2_scale(dir, 60))), tb = v2_add(b, v2_add(v2_scale(nrm, side * 70), v2_scale(dir, -60)));
    int tw = road2(g, MGR_RUNWAY, 18, ta, tb, "taxiway");
    g->roads[tw].shoulder = 8;
    for (int k = 0; k < 3; k++) {
        float t = 80.0f + (len - 160.0f) * (float)k / 2.0f;
        int cn = road2(g, MGR_RUNWAY, 16, v2_add(a, v2_scale(dir, t)), v2_add(v2_add(a, v2_scale(dir, t)), v2_scale(nrm, side * 70)), "taxi link");
        g->roads[cn].shoulder = 6;
    }
    vec2 apron = v2_add(mid, v2_add(v2_scale(nrm, side * 118), v2_scale(dir, -len * 0.12f)));
    add_paint(g, apron, v2(70, 190), ryaw, MGM_CONCRETE);
    float fy = yaw_facing(v2_scale(nrm, -side));   // buildings face the runway
#define AT(t, o) v2_add(mid, v2_add(v2_scale(dir, t), v2_scale(nrm, side * (o))))
    keep_lot(pl, add_lot(pl->p, LK_HANGAR, pl->poi, AT(-len * 0.12f - 60, 170), v2(42, 34), fy, 1, MPL_ENTERABLE, 31, "rare"), 2);
    keep_lot(pl, add_lot(pl->p, LK_HANGAR, pl->poi, AT(-len * 0.12f - 10, 170), v2(42, 34), fy, 1, MPL_ENTERABLE | MPL_DAMAGED, 32, "rare"), 2);
    keep_lot(pl, add_lot(pl->p, LK_HANGAR, pl->poi, AT(-len * 0.12f + 40, 170), v2(36, 30), fy, 1, MPL_ENTERABLE, 33, "industrial"), 2);
    keep_lot(pl, add_lot(pl->p, LK_TERMINAL, pl->poi, AT(len * 0.12f, 168), v2(48, 20), fy, 2, MPL_ENTERABLE | MPL_ROOF, 34, "rare"), 2);
    keep_lot(pl, add_lot(pl->p, LK_CONTROL_TOWER, pl->poi, AT(len * 0.12f + 40, 160), v2(8, 8), fy, 6, MPL_ENTERABLE, 35, "rare"), 2);
    keep_lot(pl, add_lot(pl->p, LK_FUEL_DEPOT, pl->poi, AT(len * 0.12f + 80, 175), v2(30, 18), fy, 1, 0, 36, ""), 2);
    keep_lot(pl, add_lot(pl->p, LK_WORKSHOP, pl->poi, AT(-len * 0.12f - 110, 168), v2(18, 12), fy, 1, MPL_ENTERABLE | MPL_LOCKED, 37, "rare"), 2);
    add_lot(pl->p, LK_PLANE_WRECK, pl->poi, AT(len * 0.25f, -32), v2(30, 26), ryaw + 0.5f, 1, 0, 38, "rare");
    add_lot(pl->p, LK_YARD_CLUTTER, pl->poi, AT(-len * 0.1f, 118), v2(60, 170), ryaw, 1, 0, 39, "rare");
    vec2 fc = AT(0, 60);
    plan_fence_rect(pl, fc, v2(320, len + 80), ryaw, jv2(pl->j, "gate", AT(0, 230)), 12, 0);
    // watchtowers inside two fence corners
    keep_lot(pl, add_lot(pl->p, LK_HUNT_STAND, pl->poi, AT(-len * 0.5f - 30, 210), v2(2.2f, 2.2f), ryaw, 1, 0, 301, "police"), 1);
    keep_lot(pl, add_lot(pl->p, LK_HUNT_STAND, pl->poi, AT(len * 0.5f + 30, -90), v2(2.2f, 2.2f), ryaw + DG_PI, 1, 0, 302, "police"), 1);
#undef AT
    poi->enterable = 7;
}

// Port: concrete quay into the sea, piers, warehouses, container yard, cranes, office, ship, fence.
static void plan_port(Plan *pl) {
    MpPoi *poi = &pl->p->pois[pl->poi];
    MapGen *g = pl->g;
    vec2 c = poi->center, h = poi->half;
    float yaw = poi->yaw;
    float level = jnum(pl->j, "level", 3.0f);
    add_pad(g, c, v2(2 * h.x, 2 * h.y), yaw, 30, MGM_CONCRETE, level, false);
#define AT(lx, lz) v2_add(c, rot2(v2((lx) * h.x, (lz) * h.y), yaw))
    // quay wall on the sea side (local -Z) and piers out into the sea
    keep_lot(pl, add_lot(pl->p, LK_QUAY, pl->poi, AT(0, -1.0f), v2(2 * h.x, 3), yaw, 1, 0, 41, ""), 1);
    for (int k = 0; k < 3; k++) {
        float lx = -0.6f + 0.6f * (float)k;
        float plen = 90.0f + 25.0f * (float)k;
        vec2 base = AT(lx, -1.0f);
        keep_lot(pl, add_lot(pl->p, LK_PIER, pl->poi, v2_add(base, rot2(v2(0, -plen * 0.5f), yaw)), v2(16, plen), yaw, 1, 0, 42 + (u32)k, ""), 1);
        if (k == 1) keep_lot(pl, add_lot(pl->p, LK_SHIP, pl->poi, v2_add(base, rot2(v2(22, -plen * 0.55f), yaw)), v2(14, 84), yaw, 1, MPL_ENTERABLE | MPL_LOCKED, 45, "rare"), 1);
        if (k != 1) keep_lot(pl, add_lot(pl->p, LK_JIB_CRANE, pl->poi, v2_add(base, rot2(v2(0, -plen * 0.7f), yaw)), v2(8, 8), yaw + (float)k, 1, 0, 46 + (u32)k, ""), 1);
        if (k == 2) add_lot(pl->p, LK_BOAT, pl->poi, v2_add(base, rot2(v2(-15, -plen * 0.4f), yaw)), v2(4, 10), yaw + 0.1f, 1, 0, 49, "");
    }
    keep_lot(pl, add_lot(pl->p, LK_GANTRY, pl->poi, AT(-0.25f, -0.82f), v2(30, 14), yaw, 1, 0, 50, ""), 1);
    keep_lot(pl, add_lot(pl->p, LK_GANTRY, pl->poi, AT(0.3f, -0.82f), v2(30, 14), yaw, 1, 0, 51, ""), 1);
    keep_lot(pl, add_lot(pl->p, LK_CONTAINERS, pl->poi, AT(-0.35f, -0.25f), v2(110, 70), yaw, 3, 0, 52, "rare"), 1);
    keep_lot(pl, add_lot(pl->p, LK_CONTAINERS, pl->poi, AT(0.35f, -0.3f), v2(80, 60), yaw, 2, 0, 53, "rare"), 1);
    keep_lot(pl, add_lot(pl->p, LK_WAREHOUSE, pl->poi, AT(-0.65f, 0.55f), v2(44, 26), yaw + DG_PI, 1, MPL_ENTERABLE, 54, "rare"), 2);
    keep_lot(pl, add_lot(pl->p, LK_WAREHOUSE, pl->poi, AT(-0.15f, 0.55f), v2(44, 26), yaw + DG_PI, 1, MPL_ENTERABLE | MPL_DAMAGED, 55, "industrial"), 2);
    keep_lot(pl, add_lot(pl->p, LK_WAREHOUSE, pl->poi, AT(0.3f, 0.6f), v2(36, 22), yaw + DG_PI, 1, 0, 56, "industrial"), 2);
    keep_lot(pl, add_lot(pl->p, LK_HARBOR_OFFICE, pl->poi, AT(0.72f, 0.45f), v2(20, 14), yaw + DG_PI, 3, MPL_ENTERABLE | MPL_LOCKED | MPL_ROOF, 57, "rare"), 2);
    keep_lot(pl, add_lot(pl->p, LK_FUEL_DEPOT, pl->poi, AT(0.8f, -0.2f), v2(26, 20), yaw, 1, 0, 58, ""), 2);
    add_lot(pl->p, LK_YARD_CLUTTER, pl->poi, AT(0, 0.1f), v2(2 * h.x * 0.9f, 2 * h.y * 0.6f), yaw, 1, 0, 59, "rare");
    // fence along the land side with the gate where the roads arrive
    vec2 gate = jv2(pl->j, "gate", AT(0.5f, 1.0f));
    vec2 fa = AT(-1.0f, 1.0f), fb = AT(1.0f, 1.0f);
    vec2 fd = v2_sub(fb, fa);
    float fl = v2_len(fd);
    MpLot *f = add_lot(pl->p, LK_FENCE, pl->poi, v2_scale(v2_add(fa, fb), 0.5f), v2(fl, 2.4f), yaw, 1, 0, 60, "");
    vec2 ag = v2_sub(gate, fa);
    f->size.y = (ag.x * fd.x + ag.y * fd.y) / fl;
    f->tier[0] = 14;
    add_lot(pl->p, LK_CHECKPOINT, pl->poi, v2_add(gate, rot2(v2(9, -6), yaw)), v2(3, 3), yaw, 1, 0, 61, "police");
    keep_lot(pl, add_lot(pl->p, LK_HUNT_STAND, pl->poi, AT(-0.95f, 0.9f), v2(2.2f, 2.2f), yaw, 1, 0, 62, "police"), 1);   // watchtower
#undef AT
    poi->enterable = 5;
}

// Quarry: the pit is a terrain feature; a spiral ramp down, a processing facility at the rim, machines.
static void plan_quarry(Plan *pl) {
    MpPoi *poi = &pl->p->pois[pl->poi];
    MapGen *g = pl->g;
    vec2 pit = jv2(pl->j, "pit", poi->center);
    float pr = jnum(pl->j, "pit_radius", 110);
    vec2 fac = jv2(pl->j, "facility", v2_add(pit, v2(pr + 60, pr * 0.5f)));
    // ramp: from the rim at angle a0, spiralling inwards and down 1.6 turns
    vec2 ramp[24];
    float a0 = jnum(pl->j, "ramp_angle", 40) * DG_DEG2RAD;
    for (int k = 0; k < 24; k++) {
        float t = (float)k / 23.0f;
        float ang = a0 + t * 5.2f, rr = pr * lerpf(1.08f, 0.2f, t);
        ramp[k] = v2_add(pit, v2(sinf(ang) * rr * 1.05f, cosf(ang) * rr * 0.95f));
    }
    int rr = mapgen_add_road(g, MGR_DIRT, 7.0f, ramp, 24, true, false, "quarry ramp");
    if (rr >= 0) g->roads[rr].shoulder = 3.0f;
    add_pad(g, fac, v2(80, 64), 0.3f, 20, MGM_GRAVEL, 0, true);
#define AT(lx, lz) v2_add(fac, rot2(v2(lx, lz), 0.3f))
    keep_lot(pl, add_lot(pl->p, LK_PLANT, pl->poi, AT(-12, 8), v2(20, 14), 0.3f, 1, MPL_ENTERABLE, 71, "industrial"), 2);
    keep_lot(pl, add_lot(pl->p, LK_SHED, pl->poi, AT(18, 12), v2(8, 6), 0.3f + DG_PI, 1, MPL_ENTERABLE, 72, "industrial"), 1);
    keep_lot(pl, add_lot(pl->p, LK_WORKSHOP, pl->poi, AT(20, -14), v2(14, 10), 0.3f + DG_PI, 1, MPL_ENTERABLE, 73, "industrial"), 1);
    keep_lot(pl, add_lot(pl->p, LK_GRAVEL, pl->poi, AT(-22, -18), v2(18, 18), 0.3f, 1, 0, 74, ""), 1);
    plan_fence_rect(pl, fac, v2(84, 68), 0.3f, jv2(pl->j, "gate", AT(0, 40)), 10, 0);
#undef AT
    add_lot(pl->p, LK_EXCAVATOR, pl->poi, v2_add(pit, v2(-pr * 0.1f, -pr * 0.05f)), v2(6, 10), 0.8f, 1, 0, 75, "");
    add_lot(pl->p, LK_DUMPTRUCK, pl->poi, v2_add(pit, v2(pr * 0.12f, pr * 0.1f)), v2(5, 10), 2.0f, 1, 0, 76, "");
    add_lot(pl->p, LK_DUMPTRUCK, pl->poi, v2_add(pit, v2(pr * 0.55f, -pr * 0.55f)), v2(5, 10), -0.7f, 1, MPL_DAMAGED, 77, "");
    add_lot(pl->p, LK_DRILL, pl->poi, v2_add(pit, v2(-pr * 0.45f, pr * 0.45f)), v2(4, 6), 0.2f, 1, 0, 78, "");
    add_lot(pl->p, LK_GRAVEL, pl->poi, v2_add(pit, v2(-pr * 0.05f, pr * 0.12f)), v2(14, 14), 0, 1, 0, 79, "");
    add_lot(pl->p, LK_BUNKER, pl->poi, v2_add(pit, v2(0, -pr * 0.17f)), v2(6, 5), DG_PI, 1, MPL_ENTERABLE, 80, "rare");
    poi->enterable = 5;
}

// Lake: dock, boats, fishing cabin, shed, campsite on the south shore (position given or the shore
// point towards the dock direction).
static void plan_lake(Plan *pl) {
    MpPoi *poi = &pl->p->pois[pl->poi];
    MapGen *g = pl->g;
    vec2 dock = jv2(pl->j, "dock", v2(poi->center.x, poi->center.y - poi->half.y));
    float dyaw = jnum(pl->j, "dock_yaw", 0) * DG_DEG2RAD;   // dock points along local +Z (into the water)
    keep_lot(pl, add_lot(pl->p, LK_DOCK, pl->poi, dock, v2(3, 28), dyaw, 1, 0, 81, ""), 0.5f);
    add_lot(pl->p, LK_BOAT, pl->poi, v2_add(dock, rot2(v2(4, 18), dyaw)), v2(2.2f, 5), dyaw + 0.15f, 1, 0, 82, "fishing");
    add_lot(pl->p, LK_BOAT, pl->poi, v2_add(dock, rot2(v2(-14, -4), dyaw)), v2(2.2f, 5), dyaw + 2.0f, 1, MPL_DAMAGED, 83, "");
    vec2 cab = v2_add(dock, rot2(v2(-12, -22), dyaw));
    // the cabin pad sits above the lake's water (the auto level would average the shore slope)
    float level = 12.0f;
    for (int i = 0; i < g->feature_count; i++)
        if (g->features[i].kind == MGF_LAKE && v2_len(v2_sub(g->features[i].pos, poi->center)) < 200.0f) level = g->features[i].level;
    add_pad(g, cab, v2(30, 24), dyaw, 14, -1, level + 1.8f, false);
    keep_lot(pl, add_lot(pl->p, LK_CABIN, pl->poi, cab, v2(8, 6), dyaw + DG_PI, 1, MPL_ENTERABLE, 84, "fishing"), 1);
    keep_lot(pl, add_lot(pl->p, LK_SHED, pl->poi, v2_add(cab, rot2(v2(9, 2), dyaw)), v2(4, 3), dyaw + DG_PI, 1, MPL_ENTERABLE, 85, "fishing"), 1);
    vec2 camp = jv2(pl->j, "campsite", v2_add(dock, rot2(v2(40, -18), dyaw)));
    add_lot(pl->p, LK_CAMPFIRE, pl->poi, camp, v2(3, 3), 0, 1, 0, 86, "");
    for (int k = 0; k < 3; k++) keep_lot(pl, add_lot(pl->p, LK_TENT, pl->poi, v2_add(camp, rot2(v2(7, 0), (float)k * 2.1f)), v2(2.6f, 3.2f), (float)k * 2.1f, 1, 0, 87 + (u32)k, "camping"), 0.5f);
    poi->enterable = 2;
}

// Mountain camp: a pad on the slope with cabins, tents, campfire, sheds; the radio tower on the ridge.
static void plan_camp(Plan *pl) {
    MpPoi *poi = &pl->p->pois[pl->poi];
    MapGen *g = pl->g;
    vec2 c = poi->center;
    float yaw = poi->yaw;
    add_pad(g, c, v2(110, 80), yaw, jnum(pl->j, "pad_blend", 26), MGM_DIRT, 0, true);
#define AT(lx, lz) v2_add(c, rot2(v2(lx, lz), yaw))
    add_lot(pl->p, LK_CAMPFIRE, pl->poi, AT(0, 0), v2(3, 3), 0, 1, 0, 91, "");
    for (int k = 0; k < 6; k++) {
        float a = (float)k / 6.0f * 6.283f + 0.3f;
        vec2 p = AT(cosf(a) * 32.0f, sinf(a) * 22.0f);
        keep_lot(pl, add_lot(pl->p, LK_CABIN, pl->poi, p, v2(7, 6), atan2f(c.x - p.x, c.y - p.y), 1, k < 4 ? MPL_ENTERABLE : 0, 92 + (u32)k, "camping"), 1);
    }
    for (int k = 0; k < 4; k++) keep_lot(pl, add_lot(pl->p, LK_TENT, pl->poi, AT(-14 + 9 * (float)k, 12 + (k & 1) * 3), v2(2.6f, 3.2f), yaw + 0.3f * (float)k, 1, 0, 100 + (u32)k, "camping"), 0.5f);
    keep_lot(pl, add_lot(pl->p, LK_SHED, pl->poi, AT(16, -12), v2(5, 4), yaw, 1, MPL_ENTERABLE, 105, "camping"), 0.5f);
    keep_lot(pl, add_lot(pl->p, LK_SHED, pl->poi, AT(-18, -10), v2(5, 4), yaw + DG_PI, 1, MPL_ENTERABLE, 106, "camping"), 0.5f);
#undef AT
    vec2 tower = jv2(pl->j, "tower", v2_add(c, v2(-60, 80)));
    add_pad(g, tower, v2(26, 22), 0, 14, MGM_GRAVEL, 0, true);
    keep_lot(pl, add_lot(pl->p, LK_RADIO_TOWER, pl->poi, tower, v2(7, 7), 0, 34, 0, 107, ""), 1);
    // the radio hut next to the tower, on the side away from the access track
    vec2 hut = v2_add(tower, v2(8, -6));
    for (int k = 0; k < 8 && road_clear(g, hut, v2(7, 6), DG_PI) < 3.0f; k++) hut = v2_add(tower, rot2(v2(9, 0), (float)k * 0.785f));
    keep_lot(pl, add_lot(pl->p, LK_CABIN, pl->poi, hut, v2(5, 4), DG_PI, 1, MPL_ENTERABLE, 108, "rare"), 1);
    poi->enterable = 7;
}

// Forest: cabins in clearings, hunting stands, improvised camps, logs, rocks (trees: a forest feature).
static void plan_forest(Plan *pl) {
    MpPoi *poi = &pl->p->pois[pl->poi];
    MapGen *g = pl->g;
    vec2 c = poi->center, h = poi->half;
    const cJSON *cabins = cJSON_GetObjectItemCaseSensitive(pl->j, "cabins");
    const cJSON *e;
    int n = 0;
    cJSON_ArrayForEach(e, cabins) {
        if (!cJSON_IsArray(e)) continue;
        vec2 p = v2((float)cJSON_GetArrayItem(e, 0)->valuedouble, (float)cJSON_GetArrayItem(e, 1)->valuedouble);
        add_pad(g, p, v2(22, 18), 0, 10, MGM_DIRT, 0, true);
        keep_lot(pl, add_lot(pl->p, LK_CABIN, pl->poi, p, v2(7, 6), rng_range(&pl->rng, 0, 6.28f), 1, MPL_ENTERABLE, 110 + (u32)n, "camping"), 6);
        n++;
    }
    for (int k = 0; k < 5; k++) {
        vec2 p = v2_add(c, v2(rng_range(&pl->rng, -h.x, h.x) * 0.8f, rng_range(&pl->rng, -h.y, h.y) * 0.8f));
        keep_lot(pl, add_lot(pl->p, LK_HUNT_STAND, pl->poi, p, v2(2.2f, 2.2f), rng_range(&pl->rng, 0, 6.28f), 1, 0, 120 + (u32)k, "camping"), 1.5f);
    }
    for (int k = 0; k < 2; k++) {
        vec2 p = v2_add(c, v2(rng_range(&pl->rng, -h.x, h.x) * 0.6f, rng_range(&pl->rng, -h.y, h.y) * 0.6f));
        keep_lot(pl, add_lot(pl->p, LK_CAMPFIRE, pl->poi, p, v2(3, 3), 0, 1, 0, 130 + (u32)k, ""), 8);
        for (int t = 0; t < 2; t++) add_lot(pl->p, LK_TENT, pl->poi, v2_add(p, rot2(v2(6, 0), (float)t * 2.5f + 0.4f)), v2(2.6f, 3.2f), (float)t * 2.5f, 1, 0, 140 + (u32)(k * 2 + t), "camping");
    }
    add_lot(pl->p, LK_LOGS, pl->poi, c, v2(2 * h.x, 2 * h.y), 0, 60, 0, 150, "");
    add_lot(pl->p, LK_ROCKS, pl->poi, c, v2(2 * h.x, 2 * h.y), 0, 40, 0, 151, "camping");
    poi->enterable = n;
}

bool mappoi_plan(MapPois *p, MapGen *g, const cJSON *pois, char *err, size_t err_size) {
    memset(p, 0, sizeof(*p));
    p->seed = g->seed * 2654435761u + 7u;
    const cJSON *j;
    cJSON_ArrayForEach(j, pois) {
        if (p->poi_count >= MP_MAX_POIS) { snprintf(err, err_size, "more than %d POIs", MP_MAX_POIS); return false; }
        MpPoi *poi = &p->pois[p->poi_count];
        memset(poi, 0, sizeof(*poi));
        dg_strcpy(poi->id, sizeof(poi->id), jstr(j, "id", "poi"));
        dg_strcpy(poi->name, sizeof(poi->name), jstr(j, "name", poi->id));
        dg_strcpy(poi->kind, sizeof(poi->kind), jstr(j, "kind", ""));
        dg_strcpy(poi->loot, sizeof(poi->loot), jstr(j, "loot", "household"));
        dg_strcpy(poi->ambience, sizeof(poi->ambience), jstr(j, "ambience", ""));
        poi->center = jv2(j, "pos", v2(0, 0));
        poi->half = v2_scale(jv2(j, "size", v2(200, 200)), 0.5f);
        poi->yaw = jnum(j, "yaw", 0) * DG_DEG2RAD;
        const cJSON *z = cJSON_GetObjectItemCaseSensitive(j, "zombies");
        poi->zombie_density = jnum(z, "density", 1.0f);
        poi->zombie_max = (int)jnum(z, "max", 16);
        poi->indoor = jnum(z, "indoor", 0.3f);
        Plan pl = { p, g, p->poi_count, j, rng_make(p->seed ^ dg_hash_str(poi->id)) };
        p->poi_count++;
        int before = p->enterable_count;
        if (!strcmp(poi->kind, "city")) plan_city(&pl);
        else if (!strcmp(poi->kind, "suburbs")) plan_suburbs(&pl);
        else if (!strcmp(poi->kind, "factory")) plan_factory(&pl);
        else if (!strcmp(poi->kind, "farms")) plan_farms(&pl);
        else if (!strcmp(poi->kind, "airfield")) plan_airfield(&pl);
        else if (!strcmp(poi->kind, "port")) plan_port(&pl);
        else if (!strcmp(poi->kind, "quarry")) plan_quarry(&pl);
        else if (!strcmp(poi->kind, "lake")) plan_lake(&pl);
        else if (!strcmp(poi->kind, "camp")) plan_camp(&pl);
        else if (!strcmp(poi->kind, "forest")) plan_forest(&pl);
        else if (strcmp(poi->kind, "region")) { snprintf(err, err_size, "POI '%s': unknown kind '%s'", poi->id, poi->kind); return false; }
        poi->enterable = p->enterable_count - before;
    }
    return true;
}

void mappoi_free(MapPois *p) {
    free(p->lots);
    free(p->prims);
    free(p->ents);
    memset(p, 0, sizeof(*p));
}

// ------------------------------------------------------------------ build: emitters

typedef struct Kit { MapPois *p; const MapGen *g; const MgRaster *r; u8 poi; Rng rng; } Kit;
typedef struct Frame { vec3 o; quat q; float yaw; } Frame;

static Frame frame(vec3 o, float yaw) { Frame f = { o, quat_yaw(yaw), yaw }; return f; }
static vec3 fp(const Frame *f, vec3 l) { return v3_add(f->o, quat_rotate(f->q, l)); }

static MpPrim *emit(Kit *k, int shape, vec3 pos, vec3 half, quat rot, u32 color, u16 flags, const char *surf) {
    MapPois *p = k->p;
    if (half.x <= 0.001f || half.y <= 0.001f || half.z <= 0.0005f) return NULL;
    if (p->prim_count == p->prim_cap) {
        p->prim_cap = p->prim_cap ? p->prim_cap * 2 : 8192;
        p->prims = (MpPrim *)realloc(p->prims, sizeof(MpPrim) * (size_t)p->prim_cap);
    }
    MpPrim *m = &p->prims[p->prim_count++];
    m->pos = pos;
    m->half = half;
    m->rot = rot;
    m->color = color;
    m->flags = flags;
    m->shape = (u8)shape;
    m->poi = k->poi;
    m->surf = surf ? surf : "concrete";
    return m;
}
static void kbox(Kit *k, const Frame *f, vec3 c, vec3 h, u32 col, u16 fl, const char *s) { emit(k, MPS_BOX, fp(f, c), h, f->q, col, fl, s); }
static void kbox_r(Kit *k, const Frame *f, vec3 c, vec3 h, quat lr, u32 col, u16 fl, const char *s) { emit(k, MPS_BOX, fp(f, c), h, quat_mul(f->q, lr), col, fl, s); }
// Box spanning y0..y1 (local), centred at (x, z).
static void kblock(Kit *k, const Frame *f, float x, float z, float hx, float hz, float y0, float y1, u32 col, u16 fl, const char *s) {
    if (y1 - y0 < 0.005f) return;
    kbox(k, f, v3(x, (y0 + y1) * 0.5f, z), v3(hx, (y1 - y0) * 0.5f, hz), col, fl, s);
}
static void kcyl(Kit *k, const Frame *f, vec3 c, float r, float hh, float taper, quat lr, u32 col, u16 fl, const char *s) {
    emit(k, MPS_CYL, fp(f, c), v3(r, hh, taper), quat_mul(f->q, lr), col, fl, s);
}
static void kwedge(Kit *k, const Frame *f, vec3 c, vec3 h, u32 col, u16 fl, const char *s) { emit(k, MPS_WEDGE, fp(f, c), h, f->q, col, fl, s); }

// Rotation taking +Y onto dir.
static quat quat_from_up(vec3 dir) {
    vec3 up = v3(0, 1, 0), d = v3_norm(dir);
    float c = v3_dot(up, d);
    if (c > 0.9999f) return quat_identity();
    if (c < -0.9999f) return quat_axis_angle(v3(1, 0, 0), DG_PI);
    return quat_axis_angle(v3_norm(v3_cross(up, d)), acosf(c));
}
// Beam (box) between two local points.
static void kbeam(Kit *k, const Frame *f, vec3 a, vec3 b, float hw, u32 col, u16 fl, const char *s) {
    vec3 d = v3_sub(b, a);
    float l = v3_len(d);
    if (l < 1e-3f) return;
    kbox_r(k, f, v3_scale(v3_add(a, b), 0.5f), v3(hw, l * 0.5f, hw), quat_from_up(d), col, fl, s);
}
static void kent(Kit *k, int kind, vec3 pos, float yaw, const char *tier_or_item) {
    MapPois *p = k->p;
    if (p->ent_count == p->ent_cap) {
        p->ent_cap = p->ent_cap ? p->ent_cap * 2 : 1024;
        p->ents = (MpEnt *)realloc(p->ents, sizeof(MpEnt) * (size_t)p->ent_cap);
    }
    MpEnt *e = &p->ents[p->ent_count++];
    memset(e, 0, sizeof(*e));
    e->kind = (u8)kind;
    e->poi = k->poi;
    e->pos = pos;
    e->yaw = yaw;
    e->count = 1;
    if (tier_or_item) {
        if (kind == MPE_CONTAINER) dg_strcpy(e->tier, sizeof(e->tier), tier_or_item);
        else dg_strcpy(e->item, sizeof(e->item), tier_or_item);
    }
}
static MpEnt *last_ent(Kit *k) { return &k->p->ents[k->p->ent_count - 1]; }
static void klight(Kit *k, vec3 pos, vec3 color, float radius, float intensity, bool scheduled, bool shadow) {
    kent(k, MPE_LIGHT, pos, 0, NULL);
    MpEnt *e = last_ent(k);
    e->shadow = shadow;
    e->color = color;
    e->radius = radius;
    e->intensity = intensity;
    e->scheduled = scheduled;
}

static float ground(const Kit *k, vec2 p) { return mapgen_raster_height(k->r, p.x, p.y); }
// Ground range under a rotated footprint (corners, edge midpoints, centre).
static void ground_range(const Kit *k, vec2 c, vec2 size, float yaw, float *lo, float *hi) {
    *lo = 1e9f;
    *hi = -1e9f;
    for (int a = -2; a <= 2; a++)
        for (int b = -2; b <= 2; b++) {
            vec2 q = v2_add(c, rot2(v2(size.x * 0.25f * (float)a, size.y * 0.25f * (float)b), yaw));
            float h = ground(k, q);
            *lo = fminf(*lo, h);
            *hi = fmaxf(*hi, h);
        }
}

// ------------------------------------------------------------------ walls

typedef struct Opening { float u, w, y0, y1; } Opening;   // centre along the wall, width, bottom / top (local y)

static int cmp_open(const void *a, const void *b) { float d = ((const Opening *)a)->u - ((const Opening *)b)->u; return d < 0 ? -1 : d > 0; }

// Axis-aligned wall in the frame from (ax, az) to (bx, bz) (either x or z constant), y0..y0+h, thickness t,
// with openings. Returns nothing; pieces are collidable.
static void kwall(Kit *k, const Frame *f, float ax, float az, float bx, float bz, float y0, float h, float t, Opening *op, int n, u32 col, const char *s, u16 fl) {
    bool along_x = fabsf(bz - az) < 1e-4f;
    float len = along_x ? fabsf(bx - ax) : fabsf(bz - az);
    float sgn = along_x ? (bx > ax ? 1.0f : -1.0f) : (bz > az ? 1.0f : -1.0f);
    qsort(op, (size_t)n, sizeof(Opening), cmp_open);
#define PIECE(u0, u1, py0, py1)                                                                                                   \
    do {                                                                                                                          \
        float uc = ((u0) + (u1)) * 0.5f, hu = ((u1) - (u0)) * 0.5f;                                                               \
        if (hu > 0.01f && (py1) - (py0) > 0.01f) {                                                                                \
            vec3 cc = along_x ? v3(ax + sgn * uc, ((py0) + (py1)) * 0.5f, az) : v3(ax, ((py0) + (py1)) * 0.5f, az + sgn * uc);   \
            vec3 hh = along_x ? v3(hu, ((py1) - (py0)) * 0.5f, t * 0.5f) : v3(t * 0.5f, ((py1) - (py0)) * 0.5f, hu);               \
            kbox(k, f, cc, hh, col, fl, s);                                                                                       \
        }                                                                                                                         \
    } while (0)
    float u = 0;
    for (int i = 0; i < n; i++) {
        float o0 = clampf(op[i].u - op[i].w * 0.5f, 0, len), o1 = clampf(op[i].u + op[i].w * 0.5f, 0, len);
        if (o0 < u) o0 = u;
        if (o1 <= o0) continue;
        PIECE(u, o0, y0, y0 + h);
        PIECE(o0, o1, y0, y0 + op[i].y0);
        PIECE(o0, o1, y0 + op[i].y1, y0 + h);
        u = o1;
    }
    PIECE(u, len, y0, y0 + h);
#undef PIECE
}

// ------------------------------------------------------------------ building kit

typedef struct BldSpec {
    float w, d, sh;              // footprint (local x, z), storey height
    int storeys;
    u32 wall, trim, roof, window;
    const char *surf;
    bool enterable, roof_access, pitched, shopfront, damaged, no_floors, glass;
    float big_w, big_h;          // wide front opening (halls, hangars, barns)
    float roof_h;                // pitched roof height
    const char *tier;
    int loot;                    // containers per storey (enterable)
    const char *lock_key;        // locked back room on the ground floor (door locked with this key)
    const char *lock_tier;
    float window_w, window_h, window_sill, window_spacing;
} BldSpec;

static BldSpec spec_default(float w, float d, int storeys) {
    BldSpec b;
    memset(&b, 0, sizeof(b));
    b.w = w;
    b.d = d;
    b.storeys = storeys;
    b.sh = 3.2f;
    b.wall = C(0.55f, 0.53f, 0.5f);
    b.trim = C(0.35f, 0.34f, 0.33f);
    b.roof = C(0.25f, 0.25f, 0.26f);
    b.window = C(0.06f, 0.08f, 0.1f);
    b.surf = "plaster";
    b.tier = "household";
    b.loot = 1;
    b.window_w = 1.3f;
    b.window_h = 1.4f;
    b.window_sill = 0.9f;
    b.window_spacing = 3.4f;
    b.roof_h = 2.4f;
    return b;
}

// Stair flight (visual steps + an invisible ramp collider) from floor y0 up by rise, starting at local
// z = z0 and running towards +dz (dz = +1 / -1), centred on x, width wd.
static void kstairs(Kit *k, const Frame *f, float x, float z0, float dz, float y0, float rise, float wd, u32 col) {
    int n = (int)lrintf(rise / 0.2f);
    float rs = rise / (float)n, run = 0.28f;
    for (int i = 0; i < n; i++) {
        float zc = z0 + dz * (run * (float)i + run * 0.5f);
        kblock(k, f, x, zc, wd * 0.5f, run * 0.5f, y0 + rs * (float)i, y0 + rs * (float)(i + 1), col, MPF_DETAIL, "wood");
    }
    float L = run * (float)n;
    float ang = atanf(rise / L), hyp = sqrtf(L * L + rise * rise);
    // ramp plane through the step nosings: from (z0, y0) to (z0 + dz L, y0 + rise), 0.12 thick below it
    vec3 mid = v3(x, y0 + rise * 0.5f, z0 + dz * L * 0.5f);
    quat q = quat_axis_angle(v3(1, 0, 0), -dz * ang);
    vec3 nrm = quat_rotate(q, v3(0, 1, 0));
    kbox_r(k, f, v3_sub(mid, v3_scale(nrm, 0.06f)), v3(wd * 0.5f, 0.06f, hyp * 0.5f), q, col, MPF_INVISIBLE | MPF_COLLIDE, "wood");
}

static void floor_slab(Kit *k, const Frame *f, float iw, float idp, float ytop, float hx0, float hx1, float hz0, float hz1, bool hole, u32 col, const char *s) {
    float t = 0.25f, y0 = ytop - t;
    float x0 = -iw * 0.5f, x1 = iw * 0.5f, z0 = -idp * 0.5f, z1 = idp * 0.5f;
    if (!hole) { kblock(k, f, 0, 0, iw * 0.5f, idp * 0.5f, y0, ytop, col, MPF_COLLIDE, s); return; }
    // four pieces around the hole [hx0, hx1] x [hz0, hz1]
    kblock(k, f, (x0 + hx0) * 0.5f, 0, (hx0 - x0) * 0.5f, idp * 0.5f, y0, ytop, col, MPF_COLLIDE, s);
    kblock(k, f, (hx1 + x1) * 0.5f, 0, (x1 - hx1) * 0.5f, idp * 0.5f, y0, ytop, col, MPF_COLLIDE, s);
    kblock(k, f, (hx0 + hx1) * 0.5f, (z0 + hz0) * 0.5f, (hx1 - hx0) * 0.5f, (hz0 - z0) * 0.5f, y0, ytop, col, MPF_COLLIDE, s);
    kblock(k, f, (hx0 + hx1) * 0.5f, (hz1 + z1) * 0.5f, (hx1 - hx0) * 0.5f, (z1 - hz1) * 0.5f, y0, ytop, col, MPF_COLLIDE, s);
}

// Window openings / panes along a facade of length len (centres).
static int window_slots(float len, float spacing, float *u, int max) {
    int n = (int)((len - 1.5f) / spacing);
    n = DG_CLAMP(n, 0, max);
    for (int i = 0; i < n; i++) u[i] = len * ((float)i + 0.5f) / (float)n;
    return n;
}

// A building on the lot centre `pos` (ground sampled), front facing yaw. Returns the floor height.
static float kit_building(Kit *k, vec2 pos, float yaw, const BldSpec *b) {
    float lo, hi;
    ground_range(k, pos, v2(b->w, b->d), yaw, &lo, &hi);
    float y0 = hi + 0.3f;
    Frame f = frame(v3(pos.x, y0, pos.y), yaw);
    float hw = b->w * 0.5f, hd = b->d * 0.5f, t = 0.3f;
    float top = b->sh * (float)b->storeys;
    // plinth down to the lowest ground (slopes)
    kblock(k, &f, 0, 0, hw + 0.25f, hd + 0.25f, lo - y0 - 0.8f, 0.0f, b->trim, MPF_COLLIDE, "concrete");
    if (b->enterable) kblock(k, &f, 0, 0, hw - 0.3f, hd - 0.3f, -0.03f, 0.005f, C(0.34f, 0.28f, 0.22f), MPF_NO_SHADOW, "wood");   // floor finish
    if (!b->enterable) {
        kblock(k, &f, 0, 0, hw, hd, 0, top, b->wall, MPF_COLLIDE, b->surf);
        // window panes on every facade, door panel on the front
        float us[24];
        for (int side = 0; side < 4; side++) {
            float len = side < 2 ? b->w : b->d;
            int n = window_slots(len, b->window_spacing, us, 24);
            for (int s = 0; s < b->storeys; s++) {
                for (int i = 0; i < n; i++) {
                    bool ground_shop = b->shopfront && s == 0;
                    if (side == 0 && s == 0 && i == n / 2 && !b->shopfront) continue;   // the door
                    float u = us[i] - len * 0.5f, wy = (float)s * b->sh + (ground_shop ? 0.4f : b->window_sill);
                    float ww = ground_shop ? fminf(b->window_spacing * 0.8f, 3.0f) : b->window_w, wh = ground_shop ? 2.3f : b->window_h;
                    u32 wc = b->window;
                    float r = rng_float(&k->rng);
                    if (b->damaged && r < 0.3f) wc = C(0.02f, 0.02f, 0.02f);
                    else if (r < 0.06f) wc = C(0.33f, 0.24f, 0.14f);   // boarded up
                    u16 fl = MPF_GLOSS | MPF_NO_SHADOW;
                    vec3 c, h;
                    switch (side) {
                    case 0: c = v3(u, wy + wh * 0.5f, hd + 0.03f); h = v3(ww * 0.5f, wh * 0.5f, 0.04f); break;
                    case 1: c = v3(-u, wy + wh * 0.5f, -hd - 0.03f); h = v3(ww * 0.5f, wh * 0.5f, 0.04f); break;
                    case 2: c = v3(hw + 0.03f, wy + wh * 0.5f, u); h = v3(0.04f, wh * 0.5f, ww * 0.5f); break;
                    default: c = v3(-hw - 0.03f, wy + wh * 0.5f, -u); h = v3(0.04f, wh * 0.5f, ww * 0.5f); break;
                    }
                    kbox(k, &f, c, h, wc, fl, "glass");
                }
            }
        }
        if (!b->shopfront) kbox(k, &f, v3(0, 1.15f, hd + 0.04f), v3(0.8f, 1.15f, 0.05f), C(0.2f, 0.15f, 0.1f), MPF_NO_SHADOW, "wood");
    } else {
        float iw = b->w - 2 * t, idp = b->d - 2 * t;
        int flights = b->no_floors ? 0 : b->storeys - 1 + (b->roof_access ? 1 : 0);
        float L = 0.28f * (float)lrintf(b->sh / 0.2f);
        float sx0 = hw - t - 2.7f, sz0 = -L * 0.5f;   // stair lanes x [sx0, sx0 + 2.6], z [sz0, sz0 + L]
        bool stairs_fit = idp >= L + 2.6f && iw >= 7.0f;
        if (!stairs_fit) flights = 0;
        int storeys = b->no_floors ? 1 : (stairs_fit ? b->storeys : 1);
        float sh = b->no_floors ? top : b->sh;
        if (!stairs_fit && !b->no_floors) top = sh;
        float us[24];
        for (int s = 0; s < storeys; s++) {
            float fy = (float)s * sh;
            if (s > 0) floor_slab(k, &f, iw, idp, fy, sx0 - 0.05f, sx0 + 2.65f, sz0 - 0.1f, sz0 + L + 0.1f, true, C(0.34f, 0.28f, 0.22f), "wood");
            for (int side = 0; side < 4; side++) {
                float len = side < 2 ? b->w : b->d;
                Opening op[26];
                int n = 0;
                int nw = window_slots(len, b->window_spacing, us, 24);
                bool big = side == 0 && s == 0 && b->big_w > 0;
                for (int i = 0; i < nw && !big; i++) {
                    if (side == 0 && s == 0 && i == nw / 2) continue;
                    bool shop = b->shopfront && s == 0 && side == 0;
                    op[n++] = (Opening){ us[i], shop ? fminf(b->window_spacing * 0.8f, 3.0f) : b->window_w, shop ? 0.5f : b->window_sill, shop ? 2.6f : b->window_sill + b->window_h };
                }
                if (side == 0 && s == 0) op[n++] = big ? (Opening){ len * 0.5f, b->big_w, 0, b->big_h } : (Opening){ len * 0.5f, 1.6f, 0, 2.3f };
                if (side == 1 && s == 0 && b->big_w > 0 && b->no_floors) op[n++] = (Opening){ len * 0.75f, 1.4f, 0, 2.3f };   // back door
                switch (side) {
                case 0: kwall(k, &f, -hw, hd - t * 0.5f, hw, hd - t * 0.5f, fy, sh, t, op, n, b->wall, b->surf, MPF_COLLIDE); break;
                case 1: kwall(k, &f, hw, -hd + t * 0.5f, -hw, -hd + t * 0.5f, fy, sh, t, op, n, b->wall, b->surf, MPF_COLLIDE); break;
                case 2: kwall(k, &f, hw - t * 0.5f, hd - t, hw - t * 0.5f, -hd + t, fy, sh, t, op, n, b->wall, b->surf, MPF_COLLIDE); break;
                default: kwall(k, &f, -hw + t * 0.5f, -hd + t, -hw + t * 0.5f, hd - t, fy, sh, t, op, n, b->wall, b->surf, MPF_COLLIDE); break;
                }
            }
            // interior partition along z with a doorway, left of the stairs
            if (iw >= 9.0f && !b->no_floors) {
                float px = -hw + iw * 0.42f;
                Opening dop = { idp * 0.3f, 1.1f, 0, 2.2f };
                kwall(k, &f, px, -hd + t, px, hd - t, fy, sh - 0.25f, 0.15f, &dop, 1, C(0.62f, 0.6f, 0.56f), "plaster", MPF_COLLIDE);
            }
            // loot, zombie spots
            for (int l = 0; l < b->loot; l++) {
                float lx = l & 1 ? -hw + 1.0f : -hw + iw * 0.42f - 1.0f, lz = l & 2 ? hd - 1.2f : -hd + 1.2f;
                kent(k, MPE_CONTAINER, fp(&f, v3(lx, fy, lz)), yaw + (l & 2 ? DG_PI : 0), b->tier);
            }
            kent(k, MPE_ZOMBIE_SPOT, fp(&f, v3(-hw * 0.4f, fy + 0.05f, 0)), yaw, NULL);
            if (iw > 14.0f) kent(k, MPE_ZOMBIE_SPOT, fp(&f, v3(hw * 0.2f, fy + 0.05f, hd * 0.5f)), yaw, NULL);
        }
        for (int s = 0; s < flights; s++) {
            bool up = (s & 1) == 0;
            float x = sx0 + (up ? 0.65f : 1.95f);
            kstairs(k, &f, x, up ? sz0 : sz0 + L, up ? 1.0f : -1.0f, (float)s * sh, sh, 1.2f, b->trim);
        }
        // locked back room (ground floor, back-left corner)
        if (b->lock_key) {
            float rx = -hw + t + 3.6f, rz = -hd + t + 3.4f;
            Opening dop = { 1.8f, 1.1f, 0, 2.2f };
            kwall(k, &f, -hw + t, rz, rx, rz, 0, sh - 0.25f, 0.2f, &dop, 1, C(0.5f, 0.5f, 0.52f), "concrete", MPF_COLLIDE);
            kwall(k, &f, rx, -hd + t, rx, rz + 0.1f, 0, sh - 0.25f, 0.2f, NULL, 0, C(0.5f, 0.5f, 0.52f), "concrete", MPF_COLLIDE);
            kent(k, MPE_DOOR, fp(&f, v3(-hw + t + 1.8f, 0, rz)), yaw, b->lock_key);
            MpEnt *d = last_ent(k);
            d->locked = true;
            d->w = 1.06f;
            d->h = 2.17f;
            kent(k, MPE_CONTAINER, fp(&f, v3(-hw + t + 1.0f, 0, -hd + t + 1.0f)), yaw, b->lock_tier ? b->lock_tier : "rare");
            kent(k, MPE_CONTAINER, fp(&f, v3(-hw + t + 2.6f, 0, -hd + t + 1.0f)), yaw, b->lock_tier ? b->lock_tier : "rare");
        }
        // roof slab (hatch over the stairs when they reach the roof)
        floor_slab(k, &f, b->w, b->d, top + 0.25f, sx0 - 0.05f, sx0 + 2.65f, sz0 - 0.1f, sz0 + L + 0.1f, b->roof_access && flights > 0, b->roof, "concrete");
        if (b->roof_access && flights > 0) {   // stair house over the hatch, open towards -x
            float y = top + 0.25f, hx0 = sx0 - 0.3f, hx1 = sx0 + 2.9f;
            kwall(k, &f, hx0, sz0 - 1.4f, hx1, sz0 - 1.4f, y, 2.5f, 0.2f, NULL, 0, b->wall, b->surf, MPF_COLLIDE);
            kwall(k, &f, hx1, sz0 - 1.4f, hx1, sz0 + L + 1.4f, y, 2.5f, 0.2f, NULL, 0, b->wall, b->surf, MPF_COLLIDE);
            kwall(k, &f, hx1, sz0 + L + 1.4f, hx0, sz0 + L + 1.4f, y, 2.5f, 0.2f, NULL, 0, b->wall, b->surf, MPF_COLLIDE);
            kblock(k, &f, (hx0 + hx1) * 0.5f, sz0 + L * 0.5f, (hx1 - hx0) * 0.5f + 0.1f, L * 0.5f + 1.5f, y + 2.5f, y + 2.7f, b->roof, MPF_COLLIDE, "concrete");
        }
    }
    float ry = top + (b->enterable ? 0.25f : 0.0f);
    if (b->pitched) {
        kwedge(k, &f, v3(0, ry + b->roof_h * 0.5f, 0), v3(hw + 0.45f, b->roof_h * 0.5f, hd + 0.45f), b->roof, MPF_COLLIDE, "wood");
    } else {
        // parapet + roof clutter
        float pt = 0.25f, ph = 0.6f;
        kblock(k, &f, 0, hd - pt * 0.5f, hw, pt * 0.5f, ry, ry + ph, b->trim, MPF_COLLIDE, "concrete");
        kblock(k, &f, 0, -hd + pt * 0.5f, hw, pt * 0.5f, ry, ry + ph, b->trim, MPF_COLLIDE, "concrete");
        kblock(k, &f, hw - pt * 0.5f, 0, pt * 0.5f, hd - pt, ry, ry + ph, b->trim, MPF_COLLIDE, "concrete");
        kblock(k, &f, -hw + pt * 0.5f, 0, pt * 0.5f, hd - pt, ry, ry + ph, b->trim, MPF_COLLIDE, "concrete");
        if (!b->enterable) kblock(k, &f, 0, 0, hw - pt, hd - pt, ry, ry + 0.02f, b->roof, MPF_NO_SHADOW, "concrete");
        int units = (int)(b->w * b->d / 120.0f);
        for (int u = 0; u < DG_MIN(units, 4); u++) {
            float ux = rng_range(&k->rng, -hw + 2, hw - 2), uz = rng_range(&k->rng, -hd + 2, hd - 2);
            if (b->roof_access && ux > hw - t - 4.7f && uz > -hd * 0.5f - 3 && uz < hd * 0.5f + 3) continue;
            kblock(k, &f, ux, uz, 0.9f, 0.7f, ry, ry + 1.1f, C(0.5f, 0.5f, 0.52f), MPF_DETAIL | MPF_COLLIDE | MPF_METAL, "metal_thin");
        }
        if (b->storeys >= 4 && rng_float(&k->rng) < 0.4f) kcyl(k, &f, v3(-hw * 0.5f, ry + 3.0f, 0), 1.4f, 1.6f, 1.0f, quat_identity(), C(0.35f, 0.3f, 0.26f), MPF_COLLIDE, "wood");
    }
    // cornice / storey bands
    if (!b->pitched && b->storeys > 1) kblock(k, &f, 0, 0, hw + 0.12f, hd + 0.12f, top - 0.25f, top, b->trim, MPF_DETAIL, b->surf);
    if (b->shopfront) {   // awning
        quat tilt = quat_axis_angle(v3(1, 0, 0), 0.35f);
        kbox_r(k, &f, v3(0, 3.0f, hd + 1.0f), v3(hw * 0.9f, 0.05f, 1.1f), tilt, Cv(jitter_col(&k->rng, v3(0.45f, 0.12f, 0.1f), 0.4f)), MPF_DETAIL, "cloth");
    }
    if (b->damaged) {   // rubble at the base
        for (int i = 0; i < 4; i++) {
            float dx = rng_range(&k->rng, -hw, hw), dz = hd + rng_range(&k->rng, 0.5f, 3.0f);
            kbox_r(k, &f, v3(dx, 0.25f - (y0 - hi) , dz), v3(rng_range(&k->rng, 0.3f, 1.0f), 0.3f, rng_range(&k->rng, 0.3f, 0.8f)), quat_yaw(rng_range(&k->rng, 0, 3)), b->trim, MPF_DETAIL | MPF_COLLIDE, "concrete");
        }
    }
    return y0;
}

// ------------------------------------------------------------------ props

static void kit_car(Kit *k, vec3 base, float yaw, int style) {
    // style: 0 abandoned (random paint), 1 burned, 2 police, 3 taxi, 4 van
    Frame f = frame(base, yaw);
    vec3 paint;
    static const vec3 paints[6] = { { 0.5f, 0.08f, 0.06f }, { 0.12f, 0.2f, 0.42f }, { 0.6f, 0.6f, 0.6f }, { 0.1f, 0.1f, 0.1f }, { 0.25f, 0.32f, 0.2f }, { 0.55f, 0.45f, 0.2f } };
    paint = paints[rng_next(&k->rng) % 6];
    if (style == 1) paint = v3(0.07f, 0.06f, 0.055f);
    if (style == 2) paint = v3(0.85f, 0.85f, 0.88f);
    if (style == 3) paint = v3(0.8f, 0.6f, 0.08f);
    float len = style == 4 ? 5.2f : 4.4f, wid = 1.85f, bh = style == 4 ? 1.5f : 0.75f;
    u32 pc = Cv(jitter_col(&k->rng, paint, 0.15f));
    u16 gl = style == 1 ? 0 : MPF_GLOSS;
    kblock(k, &f, 0, 0, wid * 0.5f, len * 0.5f, 0.35f, 0.35f + bh, pc, MPF_COLLIDE | gl | MPF_METAL, "metal_thin");
    if (style != 4) kblock(k, &f, 0, -0.2f, wid * 0.45f, 1.15f, 0.35f + bh, 0.35f + bh + 0.6f, style == 1 ? pc : C(0.08f, 0.1f, 0.12f), MPF_DETAIL | MPF_GLOSS | MPF_COLLIDE, "glass");
    if (style == 2) kblock(k, &f, 0, -0.2f, 0.5f, 0.12f, 1.75f, 1.85f, C(0.2f, 0.3f, 0.9f), MPF_DETAIL | MPF_EMISSIVE, "metal_thin");
    for (int i = 0; i < 4; i++) {
        float wx = (i & 1 ? 1.0f : -1.0f) * (wid * 0.5f - 0.05f), wz = (i & 2 ? 1.0f : -1.0f) * (len * 0.5f - 0.85f);
        if (style == 1 && i == 1) continue;   // a wheel gone
        kcyl(k, &f, v3(wx, 0.35f, wz), 0.35f, 0.12f, 1.0f, quat_axis_angle(v3(0, 0, 1), DG_PI * 0.5f), C(0.04f, 0.04f, 0.04f), MPF_DETAIL, "metal_thin");
    }
}

static void kit_tree_log(Kit *k, vec2 p, float yaw, float len, float r) {
    float h = ground(k, p);
    Frame f = frame(v3(p.x, h, p.y), yaw);
    kcyl(k, &f, v3(0, r * 0.8f, 0), r, len * 0.5f, 0.85f, quat_axis_angle(v3(1, 0, 0), DG_PI * 0.5f), C(0.24f, 0.18f, 0.12f), MPF_COLLIDE, "wood");
}

// Fence lot: variant 0 chain-link, 1 wooden, 2 concrete wall. size.x = length; storeys = has a gate,
// size.y = gate position from the start (m), tier[0] = gate width.
static void kit_fence(Kit *k, const MpLot *l) {
    vec2 dir = rot2(v2(1, 0), l->yaw);
    vec2 a = v2_sub(l->pos, v2_scale(dir, l->size.x * 0.5f));
    float len = l->size.x;
    float gate0 = 1e9f, gate1 = -1e9f;
    if (l->storeys) { float gw = (float)(u8)l->tier[0]; gate0 = l->size.y - gw * 0.5f; gate1 = l->size.y + gw * 0.5f; }
    float post = l->variant == 1 ? 2.5f : 3.0f;
    int n = (int)ceilf(len / post);
    for (int i = 0; i < n; i++) {
        float u0 = (float)i * len / (float)n, u1 = (float)(i + 1) * len / (float)n;
        float um = (u0 + u1) * 0.5f;
        if (um > gate0 && um < gate1) continue;
        if (l->variant != 2 && rng_float(&k->rng) < 0.04f) continue;   // damaged gap
        vec2 p0 = v2_add(a, v2_scale(dir, u0)), p1 = v2_add(a, v2_scale(dir, u1)), pm = v2_scale(v2_add(p0, p1), 0.5f);
        float h0 = ground(k, p0), h1 = ground(k, p1), hm = fminf(h0, h1);
        Frame f = frame(v3(pm.x, hm, pm.y), l->yaw);
        float seg = (u1 - u0) * 0.5f;
        float pitch = atan2f(h1 - h0, u1 - u0);
        quat q = quat_axis_angle(v3(0, 0, 1), pitch);
        float lift = fabsf(h1 - h0) * 0.5f;
        if (l->variant == 0) {
            kblock(k, &f, -seg, 0, 0.05f, 0.05f, -0.3f, 2.5f + lift, C(0.4f, 0.4f, 0.42f), MPF_DETAIL | MPF_METAL, "metal_thin");
            kbox_r(k, &f, v3(0, 1.25f + lift, 0), v3(seg, 1.15f, 0.02f), q, C(0.42f, 0.44f, 0.45f), MPF_COLLIDE | MPF_GLASS | MPF_NO_SHADOW, "metal_thin");
            kbox_r(k, &f, v3(0, 2.42f + lift, 0), v3(seg, 0.03f, 0.03f), q, C(0.4f, 0.4f, 0.42f), MPF_DETAIL | MPF_METAL, "metal_thin");
        } else if (l->variant == 1) {
            kblock(k, &f, -seg, 0, 0.06f, 0.06f, -0.3f, 1.15f + lift, C(0.3f, 0.22f, 0.14f), MPF_DETAIL, "wood");
            kbox_r(k, &f, v3(0, 0.95f + lift, 0), v3(seg, 0.06f, 0.025f), q, C(0.34f, 0.25f, 0.16f), MPF_COLLIDE, "wood");
            kbox_r(k, &f, v3(0, 0.5f + lift, 0), v3(seg, 0.06f, 0.025f), q, C(0.34f, 0.25f, 0.16f), MPF_DETAIL, "wood");
        } else {
            kbox_r(k, &f, v3(0, 1.0f + lift, 0), v3(seg, 1.3f + lift, 0.15f), q, C(0.5f, 0.49f, 0.46f), MPF_COLLIDE, "concrete");
        }
    }
}

static void kit_barrier(Kit *k, vec3 base, float yaw) {   // concrete jersey barrier
    Frame f = frame(base, yaw);
    kblock(k, &f, 0, 0, 1.0f, 0.3f, 0, 0.35f, C(0.62f, 0.6f, 0.56f), MPF_COLLIDE, "concrete");
    kblock(k, &f, 0, 0, 1.0f, 0.13f, 0.35f, 0.85f, C(0.62f, 0.6f, 0.56f), MPF_COLLIDE, "concrete");
}
static void kit_sandbags(Kit *k, vec3 base, float yaw, float len) {
    Frame f = frame(base, yaw);
    for (int row = 0; row < 3; row++)
        for (float u = -len * 0.5f + 0.3f; u < len * 0.5f; u += 0.62f)
            kblock(k, &f, u + (row & 1) * 0.3f, 0, 0.3f, 0.22f, (float)row * 0.24f, (float)row * 0.24f + 0.24f, C(0.42f, 0.38f, 0.27f), row == 0 ? MPF_COLLIDE | MPF_DETAIL : MPF_DETAIL, "cloth");
}

static void kit_checkpoint(Kit *k, vec2 p, float yaw) {
    float h = ground(k, p);
    Frame f = frame(v3(p.x, h, p.y), yaw);
    // booth with a window band
    kblock(k, &f, 0, 0, 1.4f, 1.4f, 0, 2.6f, C(0.6f, 0.6f, 0.58f), MPF_COLLIDE, "metal_thin");
    kblock(k, &f, 0, 0, 1.45f, 1.45f, 1.2f, 2.0f, C(0.07f, 0.09f, 0.1f), MPF_GLOSS | MPF_NO_SHADOW, "glass");
    kblock(k, &f, 0, 0, 1.6f, 1.6f, 2.6f, 2.8f, C(0.3f, 0.3f, 0.3f), MPF_COLLIDE, "metal_thin");
    kit_sandbags(k, fp(&f, v3(0, 0, 2.6f)), yaw, 3.0f);
    kit_barrier(k, fp(&f, v3(-4.0f, 0, 1.0f)), yaw + 0.2f);
    klight(k, fp(&f, v3(0, 3.4f, 0.5f)), v3(1.0f, 0.85f, 0.6f), 14, 30, true, false);
    kent(k, MPE_CONTAINER, fp(&f, v3(0, 0, -2.2f)), yaw, "police");
}

static void kit_campfire(Kit *k, vec2 p) {
    float h = ground(k, p);
    Frame f = frame(v3(p.x, h, p.y), 0);
    for (int i = 0; i < 8; i++) {
        float a = (float)i / 8.0f * 6.283f;
        kblock(k, &f, cosf(a) * 0.8f, sinf(a) * 0.8f, 0.18f, 0.15f, -0.05f, 0.2f, C(0.35f, 0.34f, 0.32f), MPF_DETAIL, "concrete");
    }
    kcyl(k, &f, v3(0, 0.15f, 0), 0.08f, 0.5f, 1.0f, quat_axis_angle(v3(1, 0, 0), 1.3f), C(0.1f, 0.07f, 0.05f), MPF_DETAIL, "wood");
    kcyl(k, &f, v3(0, 0.15f, 0), 0.08f, 0.5f, 1.0f, quat_mul(quat_yaw(1.6f), quat_axis_angle(v3(1, 0, 0), 1.3f)), C(0.1f, 0.07f, 0.05f), MPF_DETAIL, "wood");
    kblock(k, &f, 0, 0, 0.2f, 0.2f, 0.05f, 0.25f, C(1.0f, 0.45f, 0.1f), MPF_EMISSIVE | MPF_NO_SHADOW | MPF_DETAIL, "wood");
    klight(k, fp(&f, v3(0, 0.8f, 0)), v3(1.0f, 0.55f, 0.25f), 10, 40, false, true);
    for (int i = 0; i < 3; i++) {   // log benches
        float a = (float)i * 2.1f;
        Frame b = frame(fp(&f, v3(cosf(a) * 2.6f, 0, sinf(a) * 2.6f)), -a);
        kcyl(k, &b, v3(0, 0.25f, 0), 0.25f, 0.9f, 1.0f, quat_axis_angle(v3(1, 0, 0), DG_PI * 0.5f), C(0.3f, 0.22f, 0.14f), MPF_COLLIDE, "wood");
    }
}

static void kit_tent(Kit *k, const MpLot *l) {
    float h = ground(k, l->pos);
    Frame f = frame(v3(l->pos.x, h, l->pos.y), l->yaw);
    static const vec3 cols[4] = { { 0.2f, 0.32f, 0.2f }, { 0.55f, 0.35f, 0.12f }, { 0.2f, 0.25f, 0.4f }, { 0.5f, 0.15f, 0.1f } };
    kwedge(k, &f, v3(0, 0.75f, 0), v3(l->size.x * 0.5f, 0.75f, l->size.y * 0.5f), Cv(cols[l->seed % 4]), MPF_COLLIDE, "cloth");
    if (l->tier[0] && (l->seed & 1)) kent(k, MPE_CONTAINER, fp(&f, v3(0, 0, l->size.y * 0.5f + 0.6f)), l->yaw, l->tier);
}

static void kit_cabin(Kit *k, const MpLot *l) {
    BldSpec b = spec_default(l->size.x, l->size.y, 1);
    b.sh = 2.8f;
    b.wall = Cv(jitter_col(&k->rng, v3(0.36f, 0.25f, 0.16f), 0.2f));
    b.trim = C(0.25f, 0.2f, 0.15f);
    b.roof = Cv(jitter_col(&k->rng, v3(0.3f, 0.12f, 0.08f), 0.3f));
    b.surf = "wood";
    b.pitched = true;
    b.roof_h = 1.8f;
    b.enterable = (l->flags & MPL_ENTERABLE) != 0;
    b.tier = l->tier;
    b.loot = 1;
    b.window_spacing = 2.8f;
    b.window_w = 0.9f;
    b.window_h = 1.0f;
    float y = kit_building(k, l->pos, l->yaw, &b);
    Frame f = frame(v3(l->pos.x, y, l->pos.y), l->yaw);
    // porch
    kblock(k, &f, 0, l->size.y * 0.5f + 1.0f, l->size.x * 0.4f, 1.0f, -0.2f, 0.05f, b.trim, MPF_COLLIDE, "wood");
    kcyl(k, &f, v3(l->size.x * 0.35f, 1.3f, l->size.y * 0.5f + 1.8f), 0.08f, 1.3f, 1.0f, quat_identity(), b.trim, MPF_DETAIL, "wood");
    kcyl(k, &f, v3(-l->size.x * 0.35f, 1.3f, l->size.y * 0.5f + 1.8f), 0.08f, 1.3f, 1.0f, quat_identity(), b.trim, MPF_DETAIL, "wood");
}

static void kit_house(Kit *k, const MpLot *l) {
    static const vec3 walls[6] = { { 0.72f, 0.7f, 0.62f }, { 0.55f, 0.6f, 0.62f }, { 0.68f, 0.58f, 0.45f }, { 0.5f, 0.45f, 0.4f }, { 0.75f, 0.74f, 0.72f }, { 0.45f, 0.5f, 0.42f } };
    static const vec3 roofs[4] = { { 0.28f, 0.26f, 0.25f }, { 0.4f, 0.22f, 0.16f }, { 0.22f, 0.24f, 0.28f }, { 0.35f, 0.32f, 0.28f } };
    BldSpec b = spec_default(l->size.x, l->size.y, l->storeys);
    b.sh = 2.9f;
    b.wall = Cv(jitter_col(&k->rng, walls[l->variant % 6], 0.08f));
    b.trim = C(0.62f, 0.6f, 0.58f);
    b.roof = Cv(roofs[(l->variant / 6) % 4]);
    b.surf = "wood";
    b.pitched = true;
    b.roof_h = 2.2f + 0.2f * (float)(l->variant & 3);
    b.enterable = (l->flags & MPL_ENTERABLE) != 0;
    b.damaged = (l->flags & MPL_DAMAGED) != 0;
    b.tier = l->tier;
    b.loot = 2;
    b.window_spacing = 3.0f;
    b.window_w = 1.1f;
    b.window_h = 1.3f;
    if (b.storeys == 2 && l->size.y < 9.6f) b.storeys = 1;   // stairs need the depth
    float y = kit_building(k, l->pos, l->yaw, &b);
    Frame f = frame(v3(l->pos.x, y, l->pos.y), l->yaw);
    float hw = l->size.x * 0.5f, hd = l->size.y * 0.5f, top = b.sh * (float)b.storeys;
    // garage on the right (always open: fuel cans, tools)
    if (l->variant & 1) {
        Frame g = frame(fp(&f, v3(hw + 2.0f, 0, 0.5f)), l->yaw);
        Opening op = { 2.0f, 3.0f, 0, 2.3f };
        kwall(k, &g, -1.9f, hd - 0.6f, 1.9f, hd - 0.6f, 0, 2.6f, 0.2f, &op, 1, b.wall, "wood", MPF_COLLIDE);
        kwall(k, &g, 1.9f, hd - 0.6f, 1.9f, -hd + 1.6f, 0, 2.6f, 0.2f, NULL, 0, b.wall, "wood", MPF_COLLIDE);
        kwall(k, &g, 1.9f, -hd + 1.5f, -1.9f, -hd + 1.5f, 0, 2.6f, 0.2f, NULL, 0, b.wall, "wood", MPF_COLLIDE);
        kblock(k, &g, 0, -0.55f, 2.1f, hd - 0.4f, 2.6f, 2.8f, b.roof, MPF_COLLIDE, "wood");
        kblock(k, &g, 0, -0.55f, 2.0f, hd - 0.5f, -0.6f, 0.02f, C(0.45f, 0.44f, 0.42f), MPF_COLLIDE, "concrete");
        kent(k, MPE_CONTAINER, fp(&g, v3(1.2f, 0, -hd + 2.4f)), l->yaw, "garage");
        if ((l->seed & 7) == 0) kit_car(k, fp(&g, v3(0, 0, 0)), l->yaw, 0);
    }
    // chimney + porch
    kblock(k, &f, -hw * 0.5f, -hd * 0.3f, 0.45f, 0.45f, top, top + b.roof_h + 0.8f, C(0.42f, 0.25f, 0.2f), MPF_COLLIDE, "brick");
    kblock(k, &f, 0, hd + 1.0f, 1.6f, 1.0f, -0.3f, 0.02f, b.trim, MPF_COLLIDE, "wood");
    // mailbox at the street
    kblock(k, &f, hw * 0.3f, hd + 9.0f, 0.05f, 0.05f, -0.3f, 1.0f, C(0.3f, 0.3f, 0.3f), MPF_DETAIL, "metal_thin");
    kblock(k, &f, hw * 0.3f, hd + 9.0f, 0.15f, 0.25f, 1.0f, 1.25f, C(0.2f, 0.25f, 0.45f), MPF_DETAIL, "metal_thin");
    // backyard fence (low, wooden), open at the side
    if ((l->seed >> 4) % 3 != 0) {
        float by = -hd - 9.0f;
        for (int s = 0; s < 3; s++) {
            vec2 a, c2;
            if (s == 0) { a = v2(-hw - 1, -hd - 0.5f); c2 = v2(-hw - 1, by); }
            else if (s == 1) { a = v2(-hw - 1, by); c2 = v2(hw + 1, by); }
            else { a = v2(hw + 1, by); c2 = v2(hw + 1, -hd - 3.0f); }
            vec2 wa = v2_add(l->pos, rot2(a, l->yaw)), wb = v2_add(l->pos, rot2(c2, l->yaw)), d = v2_sub(wb, wa);
            MpLot fl;
            memset(&fl, 0, sizeof(fl));
            fl.pos = v2_scale(v2_add(wa, wb), 0.5f);
            fl.size = v2(v2_len(d), 1.1f);
            fl.yaw = atan2f(d.x, d.y) - DG_PI * 0.5f;
            fl.variant = 1;
            kit_fence(k, &fl);
        }
    }
    if (b.damaged && (l->seed & 3) == 0) {   // collapsed roof corner: a fallen tree / rubble
        kcyl(k, &f, v3(hw * 0.4f, top + 1.0f, 0), 0.3f, 7.0f, 0.4f, quat_axis_angle(v3(0, 0, 1), 1.1f), C(0.25f, 0.2f, 0.15f), MPF_COLLIDE, "wood");
    }
    if (b.enterable && (l->seed & 1)) kent(k, MPE_ZOMBIE_SPOT, fp(&f, v3(0, 0.05f, 0)), l->yaw, NULL);
}

static void kit_shed(Kit *k, const MpLot *l) {
    BldSpec b = spec_default(l->size.x, l->size.y, 1);
    b.sh = 2.4f;
    b.wall = Cv(jitter_col(&k->rng, v3(0.42f, 0.36f, 0.28f), 0.2f));
    b.roof = C(0.3f, 0.3f, 0.3f);
    b.surf = "wood";
    b.pitched = true;
    b.roof_h = 0.9f;
    b.enterable = (l->flags & MPL_ENTERABLE) != 0;
    b.tier = l->tier[0] ? l->tier : "garage";
    b.loot = 1;
    b.window_spacing = 9;
    kit_building(k, l->pos, l->yaw, &b);
}

// Big single-volume buildings: halls, warehouses, hangars, barns.
static void kit_shell(Kit *k, const MpLot *l, float height, vec3 wall, vec3 roof, float big_w, float big_h, bool pitched, float roof_h, const char *surf) {
    BldSpec b = spec_default(l->size.x, l->size.y, 1);
    b.sh = height;
    b.no_floors = true;
    b.wall = Cv(jitter_col(&k->rng, wall, 0.1f));
    b.roof = Cv(roof);
    b.trim = C(0.4f, 0.4f, 0.4f);
    b.surf = surf;
    b.pitched = pitched;
    b.roof_h = roof_h;
    b.enterable = (l->flags & MPL_ENTERABLE) != 0;
    b.big_w = big_w;
    b.big_h = big_h;
    b.tier = l->tier[0] ? l->tier : "industrial";
    b.loot = 3;
    b.window_spacing = 6.0f;
    b.window_sill = height - 2.2f;
    b.window_h = 1.2f;
    b.window_w = 3.0f;
    b.damaged = (l->flags & MPL_DAMAGED) != 0;
    float y = kit_building(k, l->pos, l->yaw, &b);
    if (!b.enterable) return;
    Frame f = frame(v3(l->pos.x, y, l->pos.y), l->yaw);
    float hw = l->size.x * 0.5f, hd = l->size.y * 0.5f;
    // racks / machinery rows and a mezzanine with stairs along the back wall
    int rows = (int)(l->size.x / 9.0f);
    for (int r = 0; r < rows; r++) {
        float x = -hw + 5.0f + (float)r * (l->size.x - 10.0f) / (float)DG_MAX(rows - 1, 1);
        if (fabsf(x) < big_w * 0.5f && r % 2 == 0) continue;
        kblock(k, &f, x, -hd * 0.15f, 0.8f, hd * 0.45f, 0, 2.6f, C(0.32f, 0.35f, 0.4f), MPF_COLLIDE | MPF_METAL, "metal_thick");
    }
    if (height >= 7.0f && l->size.y > 14) {
        float my = 3.4f;
        kblock(k, &f, 0, -hd + 2.3f, hw - 0.5f, 2.0f, my - 0.2f, my, C(0.35f, 0.33f, 0.3f), MPF_COLLIDE, "metal_thick");
        kblock(k, &f, 0, -hd + 4.25f, hw - 0.5f, 0.04f, my, my + 1.0f, C(0.6f, 0.5f, 0.1f), MPF_DETAIL | MPF_COLLIDE, "metal_thin");
        // stairs along the left wall up to the mezzanine
        Frame s = frame(fp(&f, v3(-hw + 1.2f, 0, -hd + 4.3f + 0.28f * 17)), l->yaw + DG_PI);
        kstairs(k, &s, 0, 0, 1.0f, 0, my, 1.2f, C(0.35f, 0.33f, 0.3f));
        kent(k, MPE_CONTAINER, fp(&f, v3(hw * 0.5f, my, -hd + 2.0f)), l->yaw, b.tier);
        kent(k, MPE_ZOMBIE_SPOT, fp(&f, v3(-hw * 0.3f, my + 0.05f, -hd + 2.0f)), l->yaw, NULL);
    }
    kent(k, MPE_ZOMBIE_SPOT, fp(&f, v3(hw * 0.4f, 0.05f, hd * 0.3f)), l->yaw, NULL);
}

static void kit_tanks(Kit *k, const MpLot *l) {
    float h = ground(k, l->pos);
    Frame f = frame(v3(l->pos.x, h, l->pos.y), l->yaw);
    for (int i = 0; i < 4; i++) {
        float x = (i & 1 ? 1.0f : -1.0f) * l->size.x * 0.25f, z = (i & 2 ? 1.0f : -1.0f) * l->size.y * 0.25f;
        float r = rng_range(&k->rng, 4.0f, 5.5f), hh = rng_range(&k->rng, 5.0f, 8.0f);
        kcyl(k, &f, v3(x, hh, z), r, hh, 1.0f, quat_identity(), Cv(jitter_col(&k->rng, v3(0.6f, 0.6f, 0.58f), 0.1f)), MPF_COLLIDE | MPF_METAL, "metal_thick");
        kcyl(k, &f, v3(x, 2 * hh + 0.4f, z), r, 0.4f, 0.6f, quat_identity(), C(0.45f, 0.45f, 0.45f), MPF_COLLIDE | MPF_METAL, "metal_thick");
        // spiral-ish ladder cage
        kblock(k, &f, x + r + 0.3f, z, 0.25f, 0.4f, 0, 2 * hh, C(0.4f, 0.35f, 0.15f), MPF_DETAIL, "metal_thin");
    }
    kblock(k, &f, 0, 0, l->size.x * 0.5f, 0.3f, 0, 0.6f, C(0.5f, 0.5f, 0.48f), MPF_COLLIDE, "concrete");   // bund wall
    // pipe to the halls
    kcyl(k, &f, v3(0, 4.5f, l->size.y * 0.5f + 8.0f), 0.4f, 9.0f, 1.0f, quat_axis_angle(v3(1, 0, 0), DG_PI * 0.5f), C(0.55f, 0.32f, 0.15f), MPF_METAL, "metal_thick");
}

static void kit_stack(Kit *k, const MpLot *l) {
    float h = ground(k, l->pos), H = (float)l->storeys;
    Frame f = frame(v3(l->pos.x, h, l->pos.y), 0);
    float r = l->size.x * 0.5f;
    kcyl(k, &f, v3(0, H * 0.5f, 0), r, H * 0.5f, 0.7f, quat_identity(), C(0.52f, 0.36f, 0.28f), MPF_COLLIDE, "brick");
    for (int b = 0; b < 3; b++) {   // red / white bands near the top
        float y = H * (0.72f + 0.09f * (float)b);
        float rr = r * lerpf(1.0f, 0.7f, y / H) + 0.05f;
        kcyl(k, &f, v3(0, y, 0), rr, 1.2f, 1.0f, quat_identity(), b & 1 ? C(0.8f, 0.78f, 0.74f) : C(0.6f, 0.12f, 0.1f), MPF_DETAIL, "brick");
    }
}

static void kit_pipes(Kit *k, const MpLot *l) {
    float h = ground(k, l->pos);
    Frame f = frame(v3(l->pos.x, h, l->pos.y), l->yaw);
    float hl = l->size.y * 0.5f;
    for (float z = -hl; z <= hl; z += 8.0f) {   // rack frames
        kblock(k, &f, -2.5f, z, 0.15f, 0.15f, 0, 6.0f, C(0.4f, 0.32f, 0.2f), MPF_COLLIDE | MPF_METAL, "metal_thick");
        kblock(k, &f, 2.5f, z, 0.15f, 0.15f, 0, 6.0f, C(0.4f, 0.32f, 0.2f), MPF_COLLIDE | MPF_METAL, "metal_thick");
        kblock(k, &f, 0, z, 2.7f, 0.15f, 5.7f, 6.0f, C(0.4f, 0.32f, 0.2f), MPF_METAL, "metal_thick");
    }
    for (int p = 0; p < 4; p++)
        kcyl(k, &f, v3(-1.8f + 1.2f * (float)p, 6.4f, 0), 0.35f + 0.1f * (float)(p & 1), hl, 1.0f, quat_axis_angle(v3(1, 0, 0), DG_PI * 0.5f),
             p & 1 ? C(0.5f, 0.3f, 0.15f) : C(0.55f, 0.55f, 0.55f), MPF_METAL, "metal_thick");
}

// Shipping containers: rows with corridors; some open (enterable) with loot.
static void kit_container(Kit *k, const Frame *f, vec3 c, float len, bool open, const char *tier) {
    static const vec3 cols[6] = { { 0.55f, 0.15f, 0.1f }, { 0.12f, 0.3f, 0.5f }, { 0.2f, 0.4f, 0.25f }, { 0.65f, 0.45f, 0.1f }, { 0.4f, 0.4f, 0.42f }, { 0.5f, 0.25f, 0.1f } };
    u32 col = Cv(jitter_col(&k->rng, cols[rng_next(&k->rng) % 6], 0.15f));
    float hw = 1.22f, hh = 1.3f, hl = len * 0.5f;
    if (!open) { kbox(k, f, v3(c.x, c.y + hh, c.z), v3(hw, hh, hl), col, MPF_COLLIDE | MPF_METAL, "metal_thin"); return; }
    kbox(k, f, v3(c.x - hw + 0.05f, c.y + hh, c.z), v3(0.05f, hh, hl), col, MPF_COLLIDE | MPF_METAL, "metal_thin");
    kbox(k, f, v3(c.x + hw - 0.05f, c.y + hh, c.z), v3(0.05f, hh, hl), col, MPF_COLLIDE | MPF_METAL, "metal_thin");
    kbox(k, f, v3(c.x, c.y + 2 * hh - 0.05f, c.z), v3(hw, 0.05f, hl), col, MPF_COLLIDE | MPF_METAL, "metal_thin");
    kbox(k, f, v3(c.x, c.y + 0.05f, c.z), v3(hw, 0.05f, hl), col, MPF_COLLIDE | MPF_METAL, "metal_thin");
    kbox(k, f, v3(c.x, c.y + hh, c.z - hl + 0.05f), v3(hw, hh, 0.05f), col, MPF_COLLIDE | MPF_METAL, "metal_thin");
    kent(k, MPE_CONTAINER, fp(f, v3(c.x, c.y + 0.1f, c.z - hl + 1.2f)), f->yaw, tier);
    kent(k, MPE_ZOMBIE_SPOT, fp(f, v3(c.x, c.y + 0.15f, c.z)), f->yaw, NULL);
}

static void kit_containers(Kit *k, const MpLot *l) {
    float h = ground(k, l->pos);
    Frame f = frame(v3(l->pos.x, h, l->pos.y), l->yaw);
    float lane = 3.2f, cw = 2.44f;
    int cols = (int)(l->size.x / (cw * 2 + lane));
    int rows = (int)(l->size.y / 13.5f);
    for (int i = 0; i < cols; i++)
        for (int j = 0; j < rows; j++) {
            for (int pair = 0; pair < 2; pair++) {
                float x = -l->size.x * 0.5f + (float)i * (cw * 2 + lane) + cw * 0.5f + (float)pair * cw;
                float z = -l->size.y * 0.5f + (float)j * 13.5f + 6.5f;
                if (rng_float(&k->rng) < 0.12f) continue;
                int stack = 1 + (int)(rng_float(&k->rng) * (float)l->storeys);
                for (int s = 0; s < stack; s++) {
                    bool open = s == 0 && rng_float(&k->rng) < 0.12f;
                    kit_container(k, &f, v3(x, (float)s * 2.6f, z), 12.2f, open, l->tier[0] ? l->tier : "industrial");
                }
            }
        }
}

static void kit_gantry(Kit *k, const MpLot *l) {
    float h = ground(k, l->pos);
    Frame f = frame(v3(l->pos.x, h, l->pos.y), l->yaw);
    float hw = l->size.x * 0.5f, hd = l->size.y * 0.5f, H = 26.0f;
    u32 c = C(0.65f, 0.25f, 0.08f);
    for (int i = 0; i < 4; i++)
        kblock(k, &f, (i & 1 ? 1.0f : -1.0f) * hw, (i & 2 ? 1.0f : -1.0f) * hd * 0.5f, 0.6f, 0.6f, 0, H, c, MPF_COLLIDE | MPF_METAL, "metal_thick");
    kblock(k, &f, 0, -hd * 0.5f, hw + 0.6f, 0.7f, H, H + 1.6f, c, MPF_COLLIDE | MPF_METAL, "metal_thick");
    kblock(k, &f, 0, hd * 0.5f, hw + 0.6f, 0.7f, H, H + 1.6f, c, MPF_COLLIDE | MPF_METAL, "metal_thick");
    kblock(k, &f, 0, -hd - 20.0f, 1.0f, hd + 21.0f, H + 1.6f, H + 3.0f, c, MPF_METAL, "metal_thick");   // boom over the water
    kblock(k, &f, 0, -hd * 0.2f, 1.6f, 1.4f, H - 3.0f, H, C(0.75f, 0.72f, 0.65f), MPF_METAL, "metal_thin");   // cab
}

static void kit_jib(Kit *k, const MpLot *l) {
    float h = ground(k, l->pos);
    Frame f = frame(v3(l->pos.x, fmaxf(h, 3.0f), l->pos.y), l->yaw);
    u32 c = C(0.75f, 0.6f, 0.1f);
    kcyl(k, &f, v3(0, 7.0f, 0), 1.2f, 7.0f, 0.8f, quat_identity(), c, MPF_COLLIDE | MPF_METAL, "metal_thick");
    kblock(k, &f, 0, 0, 2.0f, 2.5f, 14.0f, 17.0f, c, MPF_COLLIDE | MPF_METAL, "metal_thick");
    kbeam(k, &f, v3(0, 16.0f, 1.5f), v3(0, 30.0f, 24.0f), 0.5f, c, MPF_METAL, "metal_thick");
}

static void kit_ship(Kit *k, const MpLot *l) {
    // moored hull on the water (deck at +7), superstructure aft, a gangway to the pier
    Frame f = frame(v3(l->pos.x, 0, l->pos.y), l->yaw);
    float hw = l->size.x * 0.5f, hl = l->size.y * 0.5f;
    u32 hull = C(0.2f, 0.22f, 0.25f), red = C(0.45f, 0.12f, 0.08f), deck = C(0.35f, 0.3f, 0.25f);
    kblock(k, &f, 0, 0, hw, hl, -6.0f, -0.5f, red, MPF_COLLIDE | MPF_METAL, "metal_thick");
    kblock(k, &f, 0, 0, hw, hl, -0.5f, 6.6f, hull, MPF_COLLIDE | MPF_METAL, "metal_thick");
    kblock(k, &f, 0, 0, hw - 0.4f, hl - 0.4f, 6.6f, 7.0f, deck, MPF_COLLIDE | MPF_METAL, "metal_thick");
    kwedge(k, &f, v3(0, 3.2f, hl + 4.0f), v3(hw, 3.8f, 4.0f), hull, MPF_COLLIDE | MPF_METAL, "metal_thick");   // bow (approx.)
    // bulwark rails
    kblock(k, &f, hw - 0.1f, 0, 0.1f, hl, 7.0f, 8.0f, hull, MPF_COLLIDE | MPF_METAL, "metal_thin");
    kblock(k, &f, -hw + 0.1f, 0, 0.1f, hl, 7.0f, 8.0f, hull, MPF_COLLIDE | MPF_METAL, "metal_thin");
    // superstructure (enterable bridge with a locked control room)
    MpLot sl = *l;
    vec2 sp = v2_add(l->pos, rot2(v2(0, -hl + 8.0f), l->yaw));
    sl.pos = sp;
    sl.size = v2(hw * 2 - 2.0f, 12.0f);
    BldSpec b = spec_default(sl.size.x, sl.size.y, 3);
    b.sh = 3.0f;
    b.wall = C(0.85f, 0.85f, 0.82f);
    b.trim = C(0.5f, 0.5f, 0.5f);
    b.surf = "metal_thin";
    b.enterable = true;
    b.tier = "rare";
    b.lock_key = "key_blue";
    b.lock_tier = "rare";
    b.window_spacing = 2.6f;
    // the kit samples the terrain; the deck is the floor here: a flat raster at deck height
    Kit kd = *k;
    MgRaster deckr;
    float deck_h[4] = { 7.0f, 7.0f, 7.0f, 7.0f };
    deckr.n = 2;
    deckr.origin = -1e6f;
    deckr.spacing = 2e6f;
    deckr.h = deck_h;
    deckr.mat = NULL;
    kd.r = &deckr;
    kit_building(&kd, sp, l->yaw, &b);
    k->rng = kd.rng;
    // containers on deck
    for (int i = 0; i < 4; i++) kit_container(k, &f, v3(0, 7.0f, 4.0f + 13.0f * (float)i - hl * 0.3f), 12.2f, i == 2, "rare");
    // gangway from the pier (local -x side) up to the deck
    Frame gw = frame(fp(&f, v3(-hw - 4.5f, 3.0f, -hl * 0.3f)), l->yaw + DG_PI * 0.5f);
    kstairs(k, &gw, 0, -4.0f, 1.0f, 0, 4.0f, 1.2f, C(0.5f, 0.5f, 0.5f));
    kblock(k, &gw, 0, 2.5f, 0.6f, 2.4f, 3.8f, 4.0f, C(0.5f, 0.5f, 0.5f), MPF_COLLIDE | MPF_METAL, "metal_thin");
    kblock(k, &f, 0, -hl + 8.0f, 0.4f, 0.4f, 16.0f, 22.0f, C(0.6f, 0.15f, 0.1f), MPF_METAL, "metal_thick");   // funnel
}

static void kit_pier(Kit *k, const MpLot *l) {
    Frame f = frame(v3(l->pos.x, 0, l->pos.y), l->yaw);
    float hw = l->size.x * 0.5f, hl = l->size.y * 0.5f;
    kblock(k, &f, 0, 0, hw, hl, -14.0f, 3.0f, C(0.45f, 0.44f, 0.42f), MPF_COLLIDE, "concrete");
    for (float z = -hl + 3; z < hl; z += 9.0f) {   // bollards and fenders
        kcyl(k, &f, v3(hw - 0.6f, 3.35f, z), 0.25f, 0.35f, 1.0f, quat_identity(), C(0.2f, 0.2f, 0.2f), MPF_DETAIL | MPF_COLLIDE | MPF_METAL, "metal_thick");
        kcyl(k, &f, v3(-hw + 0.6f, 3.35f, z), 0.25f, 0.35f, 1.0f, quat_identity(), C(0.2f, 0.2f, 0.2f), MPF_DETAIL | MPF_COLLIDE | MPF_METAL, "metal_thick");
        kblock(k, &f, hw + 0.2f, z, 0.2f, 0.8f, 0.5f, 2.6f, C(0.08f, 0.08f, 0.08f), MPF_DETAIL, "cloth");
    }
    klight(k, fp(&f, v3(0, 9.0f, hl - 4.0f)), v3(1.0f, 0.8f, 0.55f), 22, 60, true, false);
    kblock(k, &f, 0, hl - 4.0f, 0.15f, 0.15f, 3.0f, 9.0f, C(0.3f, 0.3f, 0.3f), MPF_COLLIDE | MPF_METAL, "metal_thin");
}

static void kit_quay(Kit *k, const MpLot *l) {
    Frame f = frame(v3(l->pos.x, 0, l->pos.y), l->yaw);
    kblock(k, &f, 0, -0.5f, l->size.x * 0.5f, 1.5f, -14.0f, 3.05f, C(0.42f, 0.41f, 0.39f), MPF_COLLIDE, "concrete");
    kblock(k, &f, 0, -1.6f, l->size.x * 0.5f, 0.3f, 3.05f, 3.3f, C(0.65f, 0.55f, 0.1f), MPF_DETAIL, "concrete");
}

static void kit_dock(Kit *k, const MpLot *l) {
    float h = ground(k, l->pos);
    float level = mapgen_water_level(k->g, l->pos.x, l->pos.y);
    float deck = (level > -1e8f ? level : h) + 0.9f;
    Frame f = frame(v3(l->pos.x, 0, l->pos.y), l->yaw);
    float hw = l->size.x * 0.5f, hl = l->size.y * 0.5f;
    // planks start on the shore (local z = -hl) and run out over the water
    kblock(k, &f, 0, 0, hw, hl, deck - 0.2f, deck, C(0.4f, 0.3f, 0.2f), MPF_COLLIDE, "wood");
    for (float z = -hl + 1.0f; z <= hl; z += 3.0f) {
        vec2 pw = v2_add(l->pos, rot2(v2(hw, z), l->yaw));
        float gb = ground(k, pw);
        kblock(k, &f, hw - 0.1f, z, 0.1f, 0.1f, gb - 0.5f, deck + 0.9f, C(0.3f, 0.22f, 0.14f), MPF_COLLIDE, "wood");
        kblock(k, &f, -hw + 0.1f, z, 0.1f, 0.1f, gb - 0.5f, deck + 0.9f, C(0.3f, 0.22f, 0.14f), MPF_COLLIDE, "wood");
    }
    kent(k, MPE_CONTAINER, fp(&f, v3(0.6f, deck, hl - 1.0f)), l->yaw, "fishing");
}

static void kit_boat(Kit *k, const MpLot *l) {
    float h = ground(k, l->pos);
    float level = mapgen_water_level(k->g, l->pos.x, l->pos.y);
    bool afloat = level > h + 0.3f;
    float y = afloat ? level - 0.3f : h;
    Frame f = frame(v3(l->pos.x, y, l->pos.y), l->yaw);
    float hw = l->size.x * 0.5f, hl = l->size.y * 0.5f;
    u32 hull = (l->seed & 1) ? C(0.7f, 0.7f, 0.68f) : C(0.25f, 0.35f, 0.45f);
    kblock(k, &f, 0, -0.3f, hw, hl - 0.6f, 0, 0.9f, hull, MPF_COLLIDE, "wood");
    kwedge(k, &f, v3(0, 0.45f, hl - 0.6f), v3(hw, 0.45f, 0.6f), hull, MPF_COLLIDE, "wood");
    if (hl > 3.5f) kblock(k, &f, 0, -hl * 0.2f, hw * 0.7f, hl * 0.25f, 0.9f, 2.4f, C(0.85f, 0.85f, 0.82f), MPF_COLLIDE, "metal_thin");
    if (l->flags & MPL_DAMAGED) kbox_r(k, &f, v3(0, 0.5f, 0), v3(hw, 0.05f, hl * 0.8f), quat_axis_angle(v3(0, 0, 1), 0.3f), hull, MPF_DETAIL, "wood");
    if (l->tier[0]) kent(k, MPE_CONTAINER, fp(&f, v3(0, 0.95f, 0)), l->yaw, l->tier);
}

static void kit_radio_tower(Kit *k, const MpLot *l) {
    float h = ground(k, l->pos), H = (float)l->storeys;
    Frame f = frame(v3(l->pos.x, h, l->pos.y), l->yaw);
    u32 c = C(0.55f, 0.55f, 0.57f), red = C(0.7f, 0.12f, 0.1f);
    float b = 3.0f, t = 0.5f;
    vec3 base[4] = { v3(-b, 0, -b), v3(b, 0, -b), v3(b, 0, b), v3(-b, 0, b) };
    vec3 tip[4] = { v3(-t, H, -t), v3(t, H, -t), v3(t, H, t), v3(-t, H, t) };
    for (int i = 0; i < 4; i++) kbeam(k, &f, base[i], tip[i], 0.12f, c, MPF_COLLIDE | MPF_METAL, "metal_thick");
    for (float y = 4.0f; y < H; y += 4.0f) {
        float s = lerpf(b, t, y / H);
        for (int i = 0; i < 4; i++) {
            vec3 p0 = v3(base[i].x / b * s, y, base[i].z / b * s), p1 = v3(base[(i + 1) % 4].x / b * s, y, base[(i + 1) % 4].z / b * s);
            float s2 = lerpf(b, t, (y - 4.0f) / H);
            vec3 q1 = v3(base[(i + 1) % 4].x / b * s2, y - 4.0f, base[(i + 1) % 4].z / b * s2);
            kbeam(k, &f, p0, p1, 0.06f, ((int)(y / 4.0f) & 1) ? red : c, MPF_DETAIL | MPF_METAL, "metal_thin");
            kbeam(k, &f, p0, q1, 0.04f, c, MPF_DETAIL | MPF_METAL, "metal_thin");
        }
    }
    kcyl(k, &f, v3(0, H + 4.0f, 0), 0.08f, 4.0f, 0.5f, quat_identity(), c, MPF_METAL, "metal_thin");
    kblock(k, &f, 0, 0, 0.25f, 0.25f, H + 8.0f, H + 8.5f, C(1.0f, 0.1f, 0.05f), MPF_EMISSIVE | MPF_NO_SHADOW, "metal_thin");
    kblock(k, &f, 0, 0, b + 0.5f, b + 0.5f, -0.5f, 0.3f, C(0.5f, 0.5f, 0.48f), MPF_COLLIDE, "concrete");
    // dish antennas
    kcyl(k, &f, v3(t + 0.6f, H * 0.7f, 0), 0.9f, 0.15f, 1.0f, quat_axis_angle(v3(0, 0, 1), DG_PI * 0.5f), C(0.85f, 0.85f, 0.85f), MPF_DETAIL | MPF_METAL, "metal_thin");
    klight(k, fp(&f, v3(0, 2.5f, b + 2.0f)), v3(1.0f, 0.75f, 0.5f), 12, 25, true, false);
    kent(k, MPE_PICKUP, fp(&f, v3(1.5f, 0.35f, 0)), 0, "battery");
}

static void kit_hunt_stand(Kit *k, const MpLot *l) {
    float h = ground(k, l->pos);
    Frame f = frame(v3(l->pos.x, h, l->pos.y), l->yaw);
    float H = 3.6f, s = 1.1f;
    u32 c = C(0.33f, 0.24f, 0.15f);
    for (int i = 0; i < 4; i++) kblock(k, &f, (i & 1 ? s : -s), (i & 2 ? s : -s), 0.08f, 0.08f, -0.3f, H + 1.1f, c, MPF_COLLIDE, "wood");
    kblock(k, &f, 0, 0, s + 0.1f, s + 0.1f, H - 0.12f, H, c, MPF_COLLIDE, "wood");
    kblock(k, &f, 0, -s, s, 0.04f, H, H + 0.9f, c, MPF_COLLIDE, "wood");
    kblock(k, &f, -s, 0, 0.04f, s, H, H + 0.9f, c, MPF_COLLIDE, "wood");
    kblock(k, &f, s, 0, 0.04f, s, H, H + 0.9f, c, MPF_COLLIDE, "wood");
    kblock(k, &f, 0, 0, s + 0.2f, s + 0.2f, H + 1.8f, H + 1.9f, C(0.25f, 0.27f, 0.2f), MPF_COLLIDE, "wood");
    // steep steps up the open side
    Frame st = frame(fp(&f, v3(0, 0, s + 0.1f + 0.28f * 18)), l->yaw + DG_PI);
    kstairs(k, &st, 0, 0, 1.0f, 0, H, 0.8f, c);
    if (l->tier[0]) kent(k, MPE_CONTAINER, fp(&f, v3(0, H, 0)), l->yaw, l->tier);
}

static void kit_silos(Kit *k, const MpLot *l) {
    float h = ground(k, l->pos);
    Frame f = frame(v3(l->pos.x, h, l->pos.y), l->yaw);
    int n = 2 + (int)(l->seed % 2);
    for (int i = 0; i < n; i++) {
        float x = -l->size.x * 0.5f + 3.0f + (float)i * 5.5f;
        float H = 9.0f + 3.0f * (float)((l->seed >> i) & 1);
        kcyl(k, &f, v3(x, H * 0.5f, 0), 2.4f, H * 0.5f, 1.0f, quat_identity(), C(0.65f, 0.65f, 0.63f), MPF_COLLIDE | MPF_METAL, "metal_thin");
        kcyl(k, &f, v3(x, H + 0.8f, 0), 2.5f, 0.8f, 0.12f, quat_identity(), C(0.55f, 0.55f, 0.55f), MPF_COLLIDE | MPF_METAL, "metal_thin");
    }
}

static void kit_water_tank(Kit *k, const MpLot *l) {
    float h = ground(k, l->pos);
    Frame f = frame(v3(l->pos.x, h, l->pos.y), l->yaw);
    for (int i = 0; i < 4; i++) kbeam(k, &f, v3((i & 1 ? 1.8f : -1.8f), 0, (i & 2 ? 1.8f : -1.8f)), v3((i & 1 ? 1.2f : -1.2f), 7.0f, (i & 2 ? 1.2f : -1.2f)), 0.1f, C(0.35f, 0.3f, 0.25f), MPF_COLLIDE | MPF_METAL, "metal_thick");
    kcyl(k, &f, v3(0, 8.6f, 0), 2.0f, 1.6f, 1.0f, quat_identity(), C(0.5f, 0.52f, 0.5f), MPF_COLLIDE | MPF_METAL, "metal_thin");
    kcyl(k, &f, v3(0, 10.5f, 0), 2.1f, 0.3f, 0.2f, quat_identity(), C(0.4f, 0.4f, 0.4f), MPF_METAL, "metal_thin");
}

static void kit_tractor(Kit *k, vec3 base, float yaw, bool truck) {
    Frame f = frame(base, yaw);
    u32 body = truck ? C(0.25f, 0.3f, 0.35f) : C(0.55f, 0.15f, 0.08f);
    kblock(k, &f, 0, 0.4f, 0.7f, 1.3f, 0.6f, 1.6f, body, MPF_COLLIDE | MPF_METAL, "metal_thin");
    kblock(k, &f, 0, -0.9f, 0.75f, 0.6f, 1.0f, 2.8f, C(0.15f, 0.15f, 0.15f), MPF_DETAIL | MPF_COLLIDE | MPF_METAL, "metal_thin");
    kcyl(k, &f, v3(0.95f, 0.75f, -0.9f), 0.75f, 0.25f, 1.0f, quat_axis_angle(v3(0, 0, 1), DG_PI * 0.5f), C(0.05f, 0.05f, 0.05f), MPF_COLLIDE, "metal_thin");
    kcyl(k, &f, v3(-0.95f, 0.75f, -0.9f), 0.75f, 0.25f, 1.0f, quat_axis_angle(v3(0, 0, 1), DG_PI * 0.5f), C(0.05f, 0.05f, 0.05f), MPF_COLLIDE, "metal_thin");
    kcyl(k, &f, v3(0.8f, 0.42f, 1.3f), 0.42f, 0.15f, 1.0f, quat_axis_angle(v3(0, 0, 1), DG_PI * 0.5f), C(0.05f, 0.05f, 0.05f), MPF_DETAIL, "metal_thin");
    kcyl(k, &f, v3(-0.8f, 0.42f, 1.3f), 0.42f, 0.15f, 1.0f, quat_axis_angle(v3(0, 0, 1), DG_PI * 0.5f), C(0.05f, 0.05f, 0.05f), MPF_DETAIL, "metal_thin");
}

static void kit_dumptruck(Kit *k, const MpLot *l) {
    float h = ground(k, l->pos);
    Frame f = frame(v3(l->pos.x, h, l->pos.y), l->yaw);
    u32 y = (l->flags & MPL_DAMAGED) ? C(0.35f, 0.25f, 0.12f) : C(0.75f, 0.55f, 0.08f);
    for (int i = 0; i < 4; i++)
        kcyl(k, &f, v3((i & 1 ? 1.9f : -1.9f), 1.25f, (i & 2 ? 3.0f : -2.8f)), 1.25f, 0.45f, 1.0f, quat_axis_angle(v3(0, 0, 1), DG_PI * 0.5f), C(0.05f, 0.05f, 0.05f), MPF_COLLIDE, "metal_thick");
    kblock(k, &f, 0, 0, 1.6f, 4.4f, 1.0f, 2.2f, y, MPF_COLLIDE | MPF_METAL, "metal_thick");
    kbox_r(k, &f, v3(0, 3.2f, -0.8f), v3(2.3f, 1.0f, 3.6f), quat_axis_angle(v3(1, 0, 0), (l->flags & MPL_DAMAGED) ? -0.35f : -0.06f), y, MPF_COLLIDE | MPF_METAL, "metal_thick");
    kblock(k, &f, -0.8f, 3.6f, 0.9f, 0.9f, 2.2f, 4.2f, y, MPF_COLLIDE | MPF_METAL, "metal_thick");
}

static void kit_excavator(Kit *k, const MpLot *l) {
    float h = ground(k, l->pos);
    Frame f = frame(v3(l->pos.x, h, l->pos.y), l->yaw);
    u32 y = C(0.78f, 0.58f, 0.1f), dk = C(0.12f, 0.12f, 0.12f);
    kblock(k, &f, 1.4f, 0, 0.45f, 2.4f, 0, 1.0f, dk, MPF_COLLIDE, "metal_thick");
    kblock(k, &f, -1.4f, 0, 0.45f, 2.4f, 0, 1.0f, dk, MPF_COLLIDE, "metal_thick");
    kblock(k, &f, 0, -0.5f, 1.6f, 2.0f, 1.0f, 2.4f, y, MPF_COLLIDE | MPF_METAL, "metal_thick");
    kblock(k, &f, 0.8f, 0.8f, 0.6f, 0.7f, 2.4f, 3.6f, C(0.1f, 0.12f, 0.14f), MPF_GLOSS | MPF_COLLIDE, "glass");
    kbeam(k, &f, v3(-0.3f, 2.2f, 1.5f), v3(-0.3f, 5.5f, 4.5f), 0.35f, y, MPF_COLLIDE | MPF_METAL, "metal_thick");
    kbeam(k, &f, v3(-0.3f, 5.5f, 4.5f), v3(-0.3f, 1.2f, 7.0f), 0.28f, y, MPF_COLLIDE | MPF_METAL, "metal_thick");
    kblock(k, &f, -0.3f, 7.2f, 0.7f, 0.5f, 0.2f, 1.2f, dk, MPF_COLLIDE | MPF_METAL, "metal_thick");
}

static void kit_gravel(Kit *k, const MpLot *l) {
    float h = ground(k, l->pos);
    Frame f = frame(v3(l->pos.x, h, l->pos.y), 0);
    for (int i = 0; i < 3; i++) {
        float r = l->size.x * 0.5f * (0.5f + 0.25f * (float)i);
        vec3 c = v3(rng_range(&k->rng, -r, r) * 0.5f, 0, rng_range(&k->rng, -r, r) * 0.5f);
        float hh = r * 0.33f;
        kcyl(k, &f, v3(c.x, hh - 0.3f, c.z), r, hh, 0.1f, quat_identity(), Cv(jitter_col(&k->rng, v3(0.45f, 0.42f, 0.38f), 0.12f)), MPF_COLLIDE, "gravel");
    }
}

static void kit_drill(Kit *k, const MpLot *l) {
    float h = ground(k, l->pos);
    Frame f = frame(v3(l->pos.x, h, l->pos.y), l->yaw);
    kblock(k, &f, 0, 0, 1.2f, 2.5f, 0, 1.2f, C(0.6f, 0.2f, 0.1f), MPF_COLLIDE | MPF_METAL, "metal_thick");
    kbeam(k, &f, v3(0, 1.2f, 2.0f), v3(0, 9.0f, 2.6f), 0.25f, C(0.6f, 0.2f, 0.1f), MPF_COLLIDE | MPF_METAL, "metal_thick");
}

static void kit_plane_wreck(Kit *k, const MpLot *l) {
    float h = ground(k, l->pos);
    Frame f = frame(v3(l->pos.x, h, l->pos.y), l->yaw);
    u32 w = C(0.75f, 0.75f, 0.74f), burnt = C(0.12f, 0.11f, 0.1f);
    quat lie = quat_axis_angle(v3(1, 0, 0), DG_PI * 0.5f);
    kcyl(k, &f, v3(0, 1.6f, 2.0f), 1.8f, 7.0f, 0.95f, quat_mul(quat_axis_angle(v3(0, 0, 1), 0.15f), lie), w, MPF_COLLIDE | MPF_METAL, "metal_thin");
    kcyl(k, &f, v3(1.0f, 1.3f, -9.0f), 1.6f, 4.0f, 0.5f, quat_mul(quat_yaw(0.35f), lie), burnt, MPF_COLLIDE | MPF_METAL, "metal_thin");
    kbox_r(k, &f, v3(-6.5f, 0.6f, 2.5f), v3(5.5f, 0.15f, 1.6f), quat_axis_angle(v3(0, 0, 1), 0.12f), w, MPF_COLLIDE | MPF_METAL, "metal_thin");
    kbox_r(k, &f, v3(7.0f, 0.4f, 4.0f), v3(4.5f, 0.15f, 1.5f), quat_mul(quat_yaw(0.4f), quat_axis_angle(v3(0, 0, 1), -0.2f)), burnt, MPF_COLLIDE | MPF_METAL, "metal_thin");
    kbox_r(k, &f, v3(1.6f, 3.0f, -12.0f), v3(0.15f, 2.0f, 1.6f), quat_yaw(0.35f), w, MPF_COLLIDE | MPF_METAL, "metal_thin");
    for (int i = 0; i < 8; i++) {
        vec3 p = v3(rng_range(&k->rng, -12, 12), 0.15f, rng_range(&k->rng, -14, 14));
        kbox_r(k, &f, p, v3(rng_range(&k->rng, 0.3f, 1.2f), 0.06f, rng_range(&k->rng, 0.3f, 1.0f)), quat_yaw(rng_range(&k->rng, 0, 3)), i & 1 ? w : burnt, MPF_DETAIL, "metal_thin");
    }
    kent(k, MPE_CONTAINER, fp(&f, v3(0, 0.2f, 0)), l->yaw, "rare");
    kent(k, MPE_ZOMBIE_SPOT, fp(&f, v3(4, 0.1f, 6)), l->yaw, NULL);
}

static void kit_fuel_depot(Kit *k, const MpLot *l) {
    float h = ground(k, l->pos);
    Frame f = frame(v3(l->pos.x, h, l->pos.y), l->yaw);
    quat lie = quat_axis_angle(v3(1, 0, 0), DG_PI * 0.5f);
    for (int i = 0; i < 3; i++) {
        float x = -l->size.x * 0.5f + 4.5f + (float)i * (l->size.x - 9.0f) / 2.0f;
        kblock(k, &f, x, -4, 1.4f, 0.4f, 0, 1.0f, C(0.45f, 0.45f, 0.43f), MPF_COLLIDE, "concrete");
        kblock(k, &f, x, 4, 1.4f, 0.4f, 0, 1.0f, C(0.45f, 0.45f, 0.43f), MPF_COLLIDE, "concrete");
        kcyl(k, &f, v3(x, 2.6f, 0), 2.0f, 6.0f, 1.0f, lie, C(0.75f, 0.75f, 0.72f), MPF_COLLIDE | MPF_METAL, "metal_thick");
    }
    kent(k, MPE_PICKUP, fp(&f, v3(0, 0.3f, l->size.y * 0.5f - 1.0f)), l->yaw, "fuel_can");
}

static void kit_yard_clutter(Kit *k, const MpLot *l) {
    // pallets, crates, forklifts, trucks, scrap piles scattered on free ground of the lot
    Frame f0 = frame(v3(l->pos.x, 0, l->pos.y), l->yaw);
    int n = (int)(l->size.x * l->size.y / 600.0f);
    for (int i = 0; i < n; i++) {
        vec3 lp = v3(rng_range(&k->rng, -0.5f, 0.5f) * l->size.x, 0, rng_range(&k->rng, -0.5f, 0.5f) * l->size.y);
        vec3 wp = fp(&f0, lp);
        if (mapgen_in_keepout(k->g, wp.x, wp.z, 1.5f)) continue;
        float h = ground(k, v2(wp.x, wp.z));
        float yaw = rng_range(&k->rng, 0, 6.28f);
        Frame f = frame(v3(wp.x, h, wp.z), yaw);
        float r = rng_float(&k->rng);
        if (r < 0.35f) {   // pallet stack
            int st = 1 + (int)(rng_float(&k->rng) * 3);
            for (int s = 0; s < st; s++) kblock(k, &f, 0, 0, 0.6f, 0.5f, (float)s * 0.15f, (float)s * 0.15f + 0.14f, C(0.45f, 0.35f, 0.22f), MPF_DETAIL | MPF_COLLIDE, "wood");
        } else if (r < 0.55f) {   // crates
            kblock(k, &f, 0, 0, 0.6f, 0.6f, 0, 1.2f, C(0.4f, 0.32f, 0.2f), MPF_COLLIDE | MPF_DETAIL, "wood");
            kblock(k, &f, 1.25f, 0.2f, 0.5f, 0.5f, 0, 1.0f, C(0.36f, 0.3f, 0.2f), MPF_COLLIDE | MPF_DETAIL, "wood");
        } else if (r < 0.7f) {   // forklift
            kblock(k, &f, 0, 0, 0.6f, 1.1f, 0.2f, 1.4f, C(0.75f, 0.55f, 0.08f), MPF_COLLIDE | MPF_METAL, "metal_thick");
            kblock(k, &f, 0, 1.2f, 0.5f, 0.05f, 0, 2.6f, C(0.15f, 0.15f, 0.15f), MPF_DETAIL | MPF_METAL, "metal_thick");
        } else if (r < 0.82f) {   // parked truck
            kit_tractor(k, v3(wp.x, h, wp.z), yaw, true);
            kblock(k, &f, 0, -4.5f, 1.25f, 3.2f, 1.0f, 3.8f, Cv(jitter_col(&k->rng, v3(0.6f, 0.6f, 0.6f), 0.2f)), MPF_COLLIDE | MPF_METAL, "metal_thin");
        } else if (r < 0.92f) {   // scrap pile
            for (int s = 0; s < 6; s++)
                kbox_r(k, &f, v3(rng_range(&k->rng, -1.5f, 1.5f), 0.3f, rng_range(&k->rng, -1.5f, 1.5f)), v3(rng_range(&k->rng, 0.2f, 1.0f), 0.08f, rng_range(&k->rng, 0.1f, 0.5f)),
                       quat_euler(rng_range(&k->rng, 0, 6), rng_range(&k->rng, -0.5f, 0.5f), rng_range(&k->rng, -0.5f, 0.5f)), C(0.35f, 0.22f, 0.12f), MPF_DETAIL, "metal_thin");
            kent(k, MPE_PICKUP, v3(wp.x, h + 0.2f, wp.z), 0, "scrap");
        } else {
            kent(k, MPE_CONTAINER, v3(wp.x, h, wp.z), yaw, l->tier[0] ? l->tier : "industrial");
        }
    }
}

static void kit_playground(Kit *k, const MpLot *l) {
    float h = ground(k, l->pos);
    Frame f = frame(v3(l->pos.x, h, l->pos.y), l->yaw);
    u32 m = C(0.6f, 0.15f, 0.1f), b = C(0.15f, 0.3f, 0.6f);
    // swings
    kbeam(k, &f, v3(-3, 0, -1), v3(-3, 2.4f, 0), 0.05f, m, MPF_DETAIL | MPF_METAL, "metal_thin");
    kbeam(k, &f, v3(-3, 0, 1), v3(-3, 2.4f, 0), 0.05f, m, MPF_DETAIL | MPF_METAL, "metal_thin");
    kbeam(k, &f, v3(1, 0, -1), v3(1, 2.4f, 0), 0.05f, m, MPF_DETAIL | MPF_METAL, "metal_thin");
    kbeam(k, &f, v3(1, 0, 1), v3(1, 2.4f, 0), 0.05f, m, MPF_DETAIL | MPF_METAL, "metal_thin");
    kblock(k, &f, -1, 0, 2.0f, 0.05f, 2.35f, 2.45f, m, MPF_DETAIL | MPF_METAL, "metal_thin");
    kblock(k, &f, -2, 0, 0.25f, 0.12f, 0.5f, 0.55f, b, MPF_DETAIL, "wood");
    kblock(k, &f, 0, 0, 0.25f, 0.12f, 0.5f, 0.55f, b, MPF_DETAIL, "wood");
    // slide + benches
    kblock(k, &f, 6, 0, 0.6f, 0.6f, 0, 1.8f, b, MPF_COLLIDE, "metal_thin");
    kbox_r(k, &f, v3(6, 0.95f, 2.3f), v3(0.4f, 0.04f, 1.8f), quat_axis_angle(v3(1, 0, 0), 0.5f), C(0.8f, 0.7f, 0.1f), MPF_DETAIL, "metal_thin");
    for (int i = 0; i < 3; i++) kblock(k, &f, -10.0f + 10.0f * (float)i, l->size.y * 0.5f - 1.0f, 0.9f, 0.25f, 0, 0.45f, C(0.35f, 0.25f, 0.15f), MPF_COLLIDE | MPF_DETAIL, "wood");
}

static void kit_hay(Kit *k, const MpLot *l) {
    float h = ground(k, l->pos);
    Frame f = frame(v3(l->pos.x, h, l->pos.y), l->yaw);
    quat side = quat_axis_angle(v3(0, 0, 1), DG_PI * 0.5f);
    for (int i = 0; i < 6; i++) {
        float x = -3.0f + (float)(i % 3) * 1.6f, y = 0.7f + (float)(i / 3) * 1.35f;
        if (i >= 3 && i == 5) continue;
        kcyl(k, &f, v3(x + (float)(i / 3) * 0.8f, y, 0), 0.7f, 0.6f, 1.0f, side, C(0.62f, 0.52f, 0.28f), MPF_COLLIDE, "cloth");
    }
}

static void kit_rocks(Kit *k, const MpLot *l) {
    // boulders scattered over the area (rotated boxes; dense areas get many)
    for (int i = 0; i < l->storeys; i++) {
        vec2 p = v2_add(l->pos, v2(rng_range(&k->rng, -0.5f, 0.5f) * l->size.x, rng_range(&k->rng, -0.5f, 0.5f) * l->size.y));
        if (mapgen_in_keepout(k->g, p.x, p.y, 2) || mapgen_water_level(k->g, p.x, p.y) > ground(k, p) - 0.5f) continue;
        float rd;
        if (mapgen_nearest_road(k->g, p.x, p.y, 4, &rd) >= 0) continue;
        float h = ground(k, p), s = rng_range(&k->rng, 0.5f, 1.8f);
        Frame f = frame(v3(p.x, h, p.y), rng_range(&k->rng, 0, 6.28f));
        kbox_r(k, &f, v3(0, s * 0.3f, 0), v3(s, s * 0.7f, s * 0.8f), quat_euler(0, rng_range(&k->rng, -0.4f, 0.4f), rng_range(&k->rng, -0.4f, 0.4f)),
               Cv(jitter_col(&k->rng, v3(0.38f, 0.37f, 0.35f), 0.15f)), MPF_COLLIDE, "concrete");
        if (l->tier[0] && i % 9 == 0) kent(k, MPE_CONTAINER, v3(p.x + s + 0.8f, ground(k, v2(p.x + s + 0.8f, p.y)), p.y), 0, l->tier);   // supply cache
    }
}

static void kit_logs(Kit *k, const MpLot *l) {
    for (int i = 0; i < l->storeys; i++) {
        vec2 p = v2_add(l->pos, v2(rng_range(&k->rng, -0.5f, 0.5f) * l->size.x, rng_range(&k->rng, -0.5f, 0.5f) * l->size.y));
        if (mapgen_in_keepout(k->g, p.x, p.y, 3) || mapgen_water_level(k->g, p.x, p.y) > ground(k, p) - 0.5f) continue;
        float rd;
        if (mapgen_nearest_road(k->g, p.x, p.y, 5, &rd) >= 0) continue;
        kit_tree_log(k, p, rng_range(&k->rng, 0, 6.28f), rng_range(&k->rng, 4, 10), rng_range(&k->rng, 0.2f, 0.45f));
    }
}

static void kit_control_tower(Kit *k, const MpLot *l) {
    float h = ground(k, l->pos), H = (float)l->storeys * 3.0f;
    Frame f = frame(v3(l->pos.x, h, l->pos.y), l->yaw);
    kblock(k, &f, 0, 0, 2.6f, 2.6f, -0.5f, H, C(0.7f, 0.7f, 0.68f), MPF_COLLIDE, "concrete");
    kblock(k, &f, 0, 0, 3.8f, 3.8f, H, H + 0.3f, C(0.4f, 0.4f, 0.4f), MPF_COLLIDE, "concrete");
    kblock(k, &f, 0, 0, 3.6f, 3.6f, H + 0.3f, H + 2.6f, C(0.08f, 0.12f, 0.15f), MPF_GLOSS | MPF_COLLIDE, "glass");
    kblock(k, &f, 0, 0, 4.0f, 4.0f, H + 2.6f, H + 2.9f, C(0.4f, 0.4f, 0.4f), MPF_COLLIDE, "concrete");
    kblock(k, &f, 0, 0, 0.2f, 0.2f, H + 2.9f, H + 6.0f, C(0.5f, 0.5f, 0.5f), MPF_METAL, "metal_thin");
    kblock(k, &f, 0, 2.62f, 0.6f, 0.05f, 0, 2.2f, C(0.2f, 0.15f, 0.1f), MPF_NO_SHADOW, "wood");
}

static void kit_bunker(Kit *k, const MpLot *l) {
    BldSpec b = spec_default(l->size.x, l->size.y, 1);
    b.sh = 2.6f;
    b.wall = C(0.42f, 0.42f, 0.4f);
    b.surf = "concrete";
    b.enterable = true;
    b.tier = l->tier;
    b.loot = 2;
    b.window_spacing = 40;
    float y = kit_building(k, l->pos, l->yaw, &b);
    Frame f = frame(v3(l->pos.x, y, l->pos.y), l->yaw);
    kblock(k, &f, 0, 0, l->size.x * 0.5f + 1.0f, l->size.y * 0.5f + 1.0f, 2.85f, 3.6f, C(0.38f, 0.37f, 0.33f), MPF_COLLIDE, "gravel");   // earth cover
}

static void kit_fuel_station(Kit *k, const MpLot *l) {
    float h = ground(k, l->pos);
    Frame f = frame(v3(l->pos.x, h + 0.05f, l->pos.y), l->yaw);
    float hw = l->size.x * 0.5f, hd = l->size.y * 0.5f;
    // canopy on four pillars, pumps under it, the shop at the back
    for (int i = 0; i < 4; i++) kblock(k, &f, (i & 1 ? 8.0f : -8.0f), hd * 0.3f + (i & 2 ? 5.0f : -5.0f), 0.3f, 0.3f, 0, 5.0f, C(0.8f, 0.8f, 0.78f), MPF_COLLIDE, "metal_thin");
    kblock(k, &f, 0, hd * 0.3f, 10.0f, 7.0f, 5.0f, 5.8f, C(0.75f, 0.2f, 0.12f), MPF_COLLIDE, "metal_thin");
    kblock(k, &f, 0, hd * 0.3f, 9.0f, 6.0f, 4.95f, 5.0f, C(1.0f, 0.95f, 0.85f), MPF_EMISSIVE | MPF_NO_SHADOW, "metal_thin");
    for (int i = 0; i < 2; i++) {
        kblock(k, &f, (i ? 4.0f : -4.0f), hd * 0.3f, 0.4f, 0.7f, 0, 1.6f, C(0.7f, 0.7f, 0.68f), MPF_COLLIDE | MPF_DETAIL, "metal_thin");
        kent(k, MPE_PUMP, fp(&f, v3((i ? 4.0f : -4.0f), 0, hd * 0.3f + 1.2f)), l->yaw, NULL);
    }
    MpLot shop = *l;
    shop.pos = v2_add(l->pos, rot2(v2(0, -hd * 0.55f), l->yaw));
    shop.size = v2(hw * 1.2f, 11);
    BldSpec b = spec_default(shop.size.x, shop.size.y, 1);
    b.wall = C(0.78f, 0.76f, 0.7f);
    b.enterable = true;
    b.shopfront = true;
    b.tier = "food";
    b.loot = 3;
    b.sh = 3.6f;
    kit_building(k, shop.pos, l->yaw, &b);
    klight(k, fp(&f, v3(0, 4.6f, hd * 0.3f)), v3(1.0f, 0.95f, 0.85f), 18, 50, true, true);
    kit_car(k, fp(&f, v3(-4.0f, 0, hd * 0.3f - 3.0f)), l->yaw + 0.1f, 1);
    kent(k, MPE_VEHICLE, fp(&f, v3(4.0f, 0.4f, hd * 0.3f - 3.0f)), l->yaw, "car");
}

static void kit_parking(Kit *k, const MpLot *l) {
    float h = ground(k, l->pos);
    Frame f = frame(v3(l->pos.x, h, l->pos.y), l->yaw);
    int rows = (int)(l->size.y / 7.0f), cols = (int)(l->size.x / 3.0f);
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) {
            float x = -l->size.x * 0.5f + 1.5f + (float)c * 3.0f, z = -l->size.y * 0.5f + 3.5f + (float)r * 7.0f;
            kblock(k, &f, x - 1.5f, z, 0.06f, 2.5f, 0.0f, 0.03f, C(0.8f, 0.8f, 0.78f), MPF_NO_SHADOW | MPF_DETAIL, "asphalt");
            if (rng_float(&k->rng) < 0.45f) kit_car(k, fp(&f, v3(x, 0, z)), l->yaw + (r & 1 ? DG_PI : 0) + rng_range(&k->rng, -0.1f, 0.1f), rng_float(&k->rng) < 0.15f ? 1 : 0);
        }
}

static void kit_lot(Kit *k, const MpLot *l) {
    k->rng = rng_make(l->seed * 2654435761ull + 99u);
    k->poi = l->poi;
    bool enter = (l->flags & MPL_ENTERABLE) != 0, damaged = (l->flags & MPL_DAMAGED) != 0;
    switch (l->kind) {
    case LK_APARTMENT: case LK_OFFICE: case LK_SHOP: {
        static const vec3 walls[8] = { { 0.55f, 0.5f, 0.45f }, { 0.48f, 0.36f, 0.3f }, { 0.62f, 0.6f, 0.55f }, { 0.4f, 0.42f, 0.45f },
                                       { 0.6f, 0.55f, 0.42f }, { 0.5f, 0.52f, 0.5f }, { 0.42f, 0.3f, 0.25f }, { 0.65f, 0.64f, 0.62f } };
        BldSpec b = spec_default(l->size.x, l->size.y, l->storeys);
        b.wall = Cv(jitter_col(&k->rng, walls[l->variant & 7], 0.06f));
        b.surf = (l->variant & 1) ? "brick" : "plaster";
        b.enterable = enter;
        b.damaged = damaged;
        b.roof_access = (l->flags & MPL_ROOF) != 0;
        b.shopfront = l->kind == LK_SHOP;
        b.tier = l->tier;
        b.loot = l->kind == LK_SHOP ? 3 : 2;
        if (l->kind == LK_OFFICE) { b.window_spacing = 2.6f; b.window_w = 1.8f; b.window_h = 1.8f; b.window_sill = 0.7f; b.window = C(0.08f, 0.12f, 0.16f); b.sh = 3.4f; }
        kit_building(k, l->pos, l->yaw, &b);
        break;
    }
    case LK_CLINIC: {
        BldSpec b = spec_default(l->size.x, l->size.y, l->storeys);
        b.wall = C(0.78f, 0.78f, 0.75f);
        b.enterable = true;
        b.roof_access = true;
        b.tier = "medical";
        b.loot = 3;
        float y = kit_building(k, l->pos, l->yaw, &b);
        Frame f = frame(v3(l->pos.x, y, l->pos.y), l->yaw);
        float hd = l->size.y * 0.5f;
        kblock(k, &f, l->size.x * 0.3f, hd + 0.1f, 1.2f, 0.08f, b.sh * 2 + 0.6f, b.sh * 2 + 1.0f, C(0.9f, 0.1f, 0.1f), MPF_EMISSIVE | MPF_NO_SHADOW, "metal_thin");
        kblock(k, &f, l->size.x * 0.3f, hd + 0.1f, 0.2f, 0.08f, b.sh * 2 + 0.1f, b.sh * 2 + 1.5f, C(0.9f, 0.1f, 0.1f), MPF_EMISSIVE | MPF_NO_SHADOW, "metal_thin");
        kit_car(k, fp(&f, v3(-l->size.x * 0.25f, 0, hd + 5.5f)), l->yaw + DG_PI * 0.5f, 4);
        kent(k, MPE_PICKUP, fp(&f, v3(0, 0.05f, 0)), 0, "medkit");
        break;
    }
    case LK_POLICE: {
        BldSpec b = spec_default(l->size.x, l->size.y, l->storeys);
        b.wall = C(0.45f, 0.48f, 0.55f);
        b.enterable = true;
        b.tier = "police";
        b.loot = 2;
        b.lock_key = "key_red";
        b.lock_tier = "armory";
        float y = kit_building(k, l->pos, l->yaw, &b);
        Frame f = frame(v3(l->pos.x, y, l->pos.y), l->yaw);
        float hd = l->size.y * 0.5f;
        kit_car(k, fp(&f, v3(-6, -0.3f, hd + 7)), l->yaw + 0.3f, 2);
        kit_car(k, fp(&f, v3(3, -0.3f, hd + 8)), l->yaw - 0.2f, 2);
        kit_barrier(k, fp(&f, v3(9, -0.3f, hd + 4)), l->yaw);
        kit_barrier(k, fp(&f, v3(11.2f, -0.3f, hd + 4)), l->yaw);
        kit_sandbags(k, fp(&f, v3(0, -0.3f, hd + 2.5f)), l->yaw, 4.0f);
        kblock(k, &f, 0, hd + 0.1f, 2.5f, 0.06f, 2.6f, 3.1f, C(0.15f, 0.25f, 0.6f), MPF_EMISSIVE | MPF_NO_SHADOW, "metal_thin");
        break;
    }
    case LK_SUPERMARKET: {
        BldSpec b = spec_default(l->size.x, l->size.y, 1);
        b.sh = 6.0f;
        b.no_floors = true;
        b.wall = C(0.7f, 0.68f, 0.62f);
        b.enterable = true;
        b.shopfront = true;
        b.tier = "food";
        b.loot = 4;
        b.window_spacing = 4.0f;
        float y = kit_building(k, l->pos, l->yaw, &b);
        Frame f = frame(v3(l->pos.x, y, l->pos.y), l->yaw);
        float hw = l->size.x * 0.5f, hd = l->size.y * 0.5f;
        for (float x = -hw + 4.0f; x < hw - 3.0f; x += 4.5f) {   // shelf rows
            kblock(k, &f, x, -hd * 0.15f, 0.5f, hd * 0.55f, 0, 1.9f, C(0.55f, 0.55f, 0.6f), MPF_COLLIDE | MPF_METAL, "metal_thin");
            if (rng_float(&k->rng) < 0.5f) kent(k, MPE_CONTAINER, fp(&f, v3(x + 1.2f, 0, -hd * 0.15f)), l->yaw + DG_PI * 0.5f, "food");
        }
        kblock(k, &f, 0, hd + 0.1f, hw * 0.5f, 0.08f, 6.2f, 7.4f, C(0.15f, 0.5f, 0.2f), MPF_EMISSIVE | MPF_NO_SHADOW, "metal_thin");
        for (int i = 0; i < 6; i++) kit_car(k, fp(&f, v3(-hw + 4.0f + (float)i * 5.0f, -0.3f, hd + 10.0f + (float)(i & 1) * 3.0f)), l->yaw + (i & 1 ? DG_PI : 0), rng_float(&k->rng) < 0.2f ? 1 : 0);
        kent(k, MPE_ZOMBIE_SPOT, fp(&f, v3(0, 0.05f, 0)), l->yaw, NULL);
        kent(k, MPE_ZOMBIE_SPOT, fp(&f, v3(hw * 0.5f, 0.05f, -hd * 0.4f)), l->yaw, NULL);
        break;
    }
    case LK_FUEL: kit_fuel_station(k, l); break;
    case LK_PARKING: kit_parking(k, l); break;
    case LK_PARK: case LK_PLAYGROUND: kit_playground(k, l); break;
    case LK_HOUSE: kit_house(k, l); break;
    case LK_FARMHOUSE: {
        MpLot hl = *l;
        hl.variant = (u8)(l->seed & 15) | 1;
        kit_house(k, &hl);
        break;
    }
    case LK_SHED: kit_shed(k, l); break;
    case LK_HALL: kit_shell(k, l, 12.0f, v3(0.55f, 0.52f, 0.46f), v3(0.35f, 0.25f, 0.2f), 9.0f, 7.0f, false, 0, "metal_thin"); break;
    case LK_WAREHOUSE: kit_shell(k, l, 8.0f, v3(0.42f, 0.45f, 0.48f), v3(0.3f, 0.3f, 0.32f), 6.0f, 5.0f, true, 2.0f, "metal_thin"); break;
    case LK_WORKSHOP: {
        MpLot wl = *l;
        kit_shell(k, &wl, 5.0f, v3(0.5f, 0.48f, 0.42f), v3(0.3f, 0.3f, 0.3f), 4.0f, 4.0f, true, 1.2f, "brick");
        if (l->flags & MPL_LOCKED) {   // airfield maintenance: a locked store room
            float lo, hi;
            ground_range(k, l->pos, l->size, l->yaw, &lo, &hi);
            Frame f = frame(v3(l->pos.x, hi + 0.3f, l->pos.y), l->yaw);
            float hw = l->size.x * 0.5f, hd = l->size.y * 0.5f;
            Opening dop = { 1.8f, 1.1f, 0, 2.2f };
            kwall(k, &f, -hw + 0.3f, -hd + 4.0f, -hw + 4.3f, -hd + 4.0f, 0, 4.7f, 0.2f, &dop, 1, C(0.45f, 0.45f, 0.45f), "concrete", MPF_COLLIDE);
            kwall(k, &f, -hw + 4.3f, -hd + 0.3f, -hw + 4.3f, -hd + 4.1f, 0, 4.7f, 0.2f, NULL, 0, C(0.45f, 0.45f, 0.45f), "concrete", MPF_COLLIDE);
            kent(k, MPE_DOOR, fp(&f, v3(-hw + 0.3f + 1.8f, 0, -hd + 4.0f)), l->yaw, "key_blue");
            MpEnt *d = last_ent(k);
            d->locked = true;
            d->w = 1.06f;
            d->h = 2.17f;
            kent(k, MPE_CONTAINER, fp(&f, v3(-hw + 1.5f, 0, -hd + 1.5f)), l->yaw, "military");
        }
        break;
    }
    case LK_ADMIN: {
        BldSpec b = spec_default(l->size.x, l->size.y, l->storeys);
        b.wall = C(0.6f, 0.55f, 0.48f);
        b.surf = "brick";
        b.enterable = enter;
        b.roof_access = true;
        b.tier = l->tier;
        b.loot = 2;
        kit_building(k, l->pos, l->yaw, &b);
        break;
    }
    case LK_HARBOR_OFFICE: {
        BldSpec b = spec_default(l->size.x, l->size.y, l->storeys);
        b.wall = C(0.7f, 0.68f, 0.6f);
        b.enterable = true;
        b.roof_access = true;
        b.tier = "rare";
        b.loot = 2;
        b.lock_key = "key_blue";
        b.lock_tier = "military";
        kit_building(k, l->pos, l->yaw, &b);
        break;
    }
    case LK_TERMINAL: {
        BldSpec b = spec_default(l->size.x, l->size.y, l->storeys);
        b.wall = C(0.72f, 0.72f, 0.7f);
        b.enterable = true;
        b.roof_access = true;
        b.shopfront = true;
        b.tier = "rare";
        b.loot = 3;
        b.sh = 3.6f;
        kit_building(k, l->pos, l->yaw, &b);
        break;
    }
    case LK_PLANT: {
        kit_shell(k, l, 7.0f, v3(0.5f, 0.47f, 0.42f), v3(0.3f, 0.3f, 0.3f), 5.0f, 5.0f, true, 1.5f, "metal_thin");
        float h = ground(k, l->pos);
        Frame f = frame(v3(l->pos.x, h, l->pos.y), l->yaw);
        kbeam(k, &f, v3(l->size.x * 0.5f + 2.0f, 1.0f, 0), v3(l->size.x * 0.5f + 22.0f, 12.0f, 0), 0.7f, C(0.4f, 0.38f, 0.3f), MPF_COLLIDE | MPF_METAL, "metal_thick");   // conveyor
        kblock(k, &f, l->size.x * 0.5f + 24.0f, 0, 2.5f, 2.5f, 0, 14.0f, C(0.5f, 0.4f, 0.25f), MPF_COLLIDE | MPF_METAL, "metal_thick");   // hopper tower
        break;
    }
    case LK_BARN: {
        kit_shell(k, l, 6.5f, v3(0.5f, 0.12f, 0.08f), v3(0.35f, 0.33f, 0.3f), 5.0f, 5.0f, true, 4.5f, "wood");
        float lo, hi;
        ground_range(k, l->pos, l->size, l->yaw, &lo, &hi);
        Frame f = frame(v3(l->pos.x, hi + 0.3f, l->pos.y), l->yaw);
        MpLot hay = *l;
        hay.pos = v2_add(l->pos, rot2(v2(l->size.x * 0.25f, -l->size.y * 0.15f), l->yaw));
        kit_hay(k, &hay);
        if (l->seed & 1) kit_tractor(k, fp(&f, v3(-l->size.x * 0.25f, 0, 0)), l->yaw, false);
        else kent(k, MPE_VEHICLE, fp(&f, v3(-l->size.x * 0.22f, 0.4f, 0)), l->yaw, "car");
        break;
    }
    case LK_HANGAR: {
        kit_shell(k, l, 10.0f, v3(0.42f, 0.44f, 0.42f), v3(0.32f, 0.34f, 0.32f), l->size.x - 8.0f, 8.5f, true, 3.0f, "metal_thin");
        float lo, hi;
        ground_range(k, l->pos, l->size, l->yaw, &lo, &hi);
        Frame f = frame(v3(l->pos.x, hi + 0.3f, l->pos.y), l->yaw);
        if (l->seed == 31) {   // a small plane inside
            quat lie = quat_axis_angle(v3(1, 0, 0), DG_PI * 0.5f);
            kcyl(k, &f, v3(0, 1.4f, 0), 0.9f, 4.5f, 0.6f, lie, C(0.8f, 0.8f, 0.78f), MPF_COLLIDE | MPF_METAL, "metal_thin");
            kblock(k, &f, 0, 1.0f, 5.5f, 0.8f, 1.9f, 2.05f, C(0.8f, 0.8f, 0.78f), MPF_COLLIDE | MPF_METAL, "metal_thin");
        } else if (l->seed == 33) {
            kent(k, MPE_VEHICLE, fp(&f, v3(0, 0.4f, 0)), l->yaw, "car");
        }
        break;
    }
    case LK_CONTROL_TOWER: kit_control_tower(k, l); break;
    case LK_FUEL_DEPOT: kit_fuel_depot(k, l); break;
    case LK_PLANE_WRECK: kit_plane_wreck(k, l); break;
    case LK_TANKS: kit_tanks(k, l); break;
    case LK_STACK: kit_stack(k, l); break;
    case LK_PIPES: kit_pipes(k, l); break;
    case LK_YARD_CLUTTER: kit_yard_clutter(k, l); break;
    case LK_SILOS: kit_silos(k, l); break;
    case LK_WATER_TANK: kit_water_tank(k, l); break;
    case LK_TRACTOR: kit_tractor(k, v3(l->pos.x, ground(k, l->pos), l->pos.y), l->yaw, false); break;
    case LK_HAY: kit_hay(k, l); break;
    case LK_CONTAINERS: kit_containers(k, l); break;
    case LK_GANTRY: kit_gantry(k, l); break;
    case LK_JIB_CRANE: kit_jib(k, l); break;
    case LK_SHIP: kit_ship(k, l); break;
    case LK_PIER: kit_pier(k, l); break;
    case LK_QUAY: kit_quay(k, l); break;
    case LK_CABIN: kit_cabin(k, l); break;
    case LK_TENT: kit_tent(k, l); break;
    case LK_CAMPFIRE: kit_campfire(k, l->pos); break;
    case LK_RADIO_TOWER: kit_radio_tower(k, l); break;
    case LK_HUNT_STAND: kit_hunt_stand(k, l); break;
    case LK_LOGS: kit_logs(k, l); break;
    case LK_ROCKS: kit_rocks(k, l); break;
    case LK_DOCK: kit_dock(k, l); break;
    case LK_BOAT: kit_boat(k, l); break;
    case LK_EXCAVATOR: kit_excavator(k, l); break;
    case LK_DUMPTRUCK: kit_dumptruck(k, l); break;
    case LK_GRAVEL: kit_gravel(k, l); break;
    case LK_DRILL: kit_drill(k, l); break;
    case LK_BUNKER: kit_bunker(k, l); break;
    case LK_FENCE: kit_fence(k, l); break;
    case LK_CHECKPOINT: kit_checkpoint(k, l->pos, l->yaw); break;
    case LK_CAR: kit_car(k, v3(l->pos.x, ground(k, l->pos), l->pos.y), l->yaw, l->variant); break;
    default: break;
    }
}

// ------------------------------------------------------------------ roads: markings, bridges, streets, encounters

static float road_h(const MapGen *g, int s0, int s1, float t) { return lerpf(g->rs[s0].h, g->rs[s1].h, t); }

static void build_markings(Kit *k) {
    const MapGen *g = k->g;
    k->poi = 0;
    for (int r = 0; r < g->road_count; r++) {
        const MgRoad *rd = &g->roads[r];
        if (rd->cls == MGR_DIRT || rd->cls == MGR_TRAIL) continue;
        bool runway = rd->cls == MGR_RUNWAY && rd->width > 30.0f;
        if (rd->cls == MGR_RUNWAY && !runway) continue;
        u32 col = rd->cls == MGR_MAIN ? C(0.75f, 0.62f, 0.2f) : C(0.75f, 0.75f, 0.72f);
        float period = runway ? 30.0f : 9.0f, dash = runway ? 12.0f : 3.0f, wid = runway ? 0.45f : 0.12f;
        int step = (int)(period / MG_ROAD_STEP);
        for (int i = 0; i + 2 < rd->count; i += step) {
            const MgRoadSample *a = &g->rs[rd->first + i];
            int j = DG_MIN(i + (int)(dash / MG_ROAD_STEP), rd->count - 1);
            const MgRoadSample *b = &g->rs[rd->first + j];
            if (a->bridge || b->bridge) continue;
            if (mappoi_find(k->p, a->p.x, a->p.y) >= 0 && rd->cls == MGR_STREET && (i / step) % 5 == 4) continue;   // faded
            vec2 m = v2_scale(v2_add(a->p, b->p), 0.5f);
            float len = v2_len(v2_sub(b->p, a->p));
            float yaw = yaw_facing(v2_sub(b->p, a->p));
            float pitch = atanf((b->h - a->h) / fmaxf(len, 0.1f));
            float hm = mapgen_raster_height(k->r, m.x, m.y);
            Frame f = frame(v3(m.x, hm + 0.075f, m.y), yaw);
            kbox_r(k, &f, v3(0, 0, 0), v3(wid, 0.015f, len * 0.5f), quat_axis_angle(v3(1, 0, 0), -pitch), col, MPF_NO_SHADOW | MPF_DETAIL, "asphalt");
        }
        if (runway) {   // threshold bars at both ends
            for (int e = 0; e < 2; e++) {
                const MgRoadSample *s = &g->rs[rd->first + (e ? rd->count - 8 : 7)];
                float yaw = yaw_facing(s->dir);
                Frame f = frame(v3(s->p.x, mapgen_raster_height(k->r, s->p.x, s->p.y) + 0.07f, s->p.y), yaw);
                for (int bar = -6; bar <= 6; bar++) if (bar) kbox(k, &f, v3((float)bar * 2.8f, 0, 0), v3(0.7f, 0.015f, 15.0f), C(0.8f, 0.8f, 0.78f), MPF_NO_SHADOW, "asphalt");
            }
        }
    }
}

static void build_bridges(Kit *k) {
    const MapGen *g = k->g;
    k->poi = 0;
    for (int b = 0; b < g->bridge_count; b++) {
        const MgBridge *br = &g->bridges[b];
        vec2 d = v2_sub(br->b, br->a);
        float len = v2_len(d) + 4.0f;
        vec2 m = v2_scale(v2_add(br->a, br->b), 0.5f);
        Frame f = frame(v3(m.x, br->h, m.y), yaw_facing(d));
        float hw = br->width * 0.5f + 0.6f;
        u32 c = C(0.5f, 0.49f, 0.46f);
        kblock(k, &f, 0, 0, hw, len * 0.5f, -0.9f, 0.02f, c, MPF_COLLIDE, "concrete");
        kblock(k, &f, hw - 0.2f, 0, 0.2f, len * 0.5f, 0.02f, 1.0f, c, MPF_COLLIDE, "concrete");
        kblock(k, &f, -hw + 0.2f, 0, 0.2f, len * 0.5f, 0.02f, 1.0f, c, MPF_COLLIDE, "concrete");
        // piers down to the river bed
        for (int p = -1; p <= 1; p += 2) {
            vec3 wp = fp(&f, v3(0, 0, (float)p * len * 0.18f));
            float bed = mapgen_raster_height(k->r, wp.x, wp.z);
            kblock(k, &f, 0, (float)p * len * 0.18f, hw - 1.0f, 0.8f, bed - br->h - 1.0f, -0.9f, C(0.42f, 0.41f, 0.39f), MPF_COLLIDE, "concrete");
        }
        klight(k, fp(&f, v3(hw - 0.3f, 5.0f, 0)), v3(1.0f, 0.82f, 0.55f), 16, 35, true, false);
        kblock(k, &f, hw - 0.3f, 0, 0.08f, 0.08f, 1.0f, 5.0f, C(0.3f, 0.3f, 0.3f), MPF_DETAIL | MPF_METAL, "metal_thin");
    }
}

// City street furniture: lamps, signals at crossings, wrecks and barricades on the streets.
static void build_city_streets(Kit *k, const MpLot *l) {
    const MapGen *g = k->g;
    k->rng = rng_make(l->seed);
    k->poi = l->poi;
    int lights = 0;
    for (int r = 0; r < g->road_count; r++) {
        const MgRoad *rd = &g->roads[r];
        if (rd->cls != MGR_STREET) continue;
        const MgRoadSample *mid = &g->rs[rd->first + rd->count / 2];
        if (mappoi_find(k->p, mid->p.x, mid->p.y) != l->poi - 1) continue;
        bool city = !strcmp(k->p->pois[l->poi - 1].kind, "city");
        for (int i = 6; i + 6 < rd->count; i++) {
            const MgRoadSample *s = &g->rs[rd->first + i];
            vec2 nrm = v2(s->dir.y, -s->dir.x);
            float yaw = yaw_facing(s->dir);
            // lamps every 34 m, alternating sides
            if (i % 17 == 0) {
                float side = (i / 17) & 1 ? 1.0f : -1.0f;
                vec2 p = v2_add(s->p, v2_scale(nrm, side * (rd->width * 0.5f + 1.2f)));
                float h = ground(k, p);
                Frame f = frame(v3(p.x, h, p.y), yaw_facing(v2_scale(nrm, -side)));
                kcyl(k, &f, v3(0, 3.8f, 0), 0.09f, 3.8f, 0.7f, quat_identity(), C(0.3f, 0.3f, 0.32f), MPF_DETAIL | MPF_METAL | MPF_COLLIDE, "metal_thin");
                kblock(k, &f, 0, 0.8f, 0.06f, 0.8f, 7.45f, 7.55f, C(0.3f, 0.3f, 0.32f), MPF_DETAIL | MPF_METAL, "metal_thin");
                bool broken = rng_float(&k->rng) < 0.4f;
                kblock(k, &f, 0, 1.5f, 0.2f, 0.35f, 7.3f, 7.45f, broken ? C(0.2f, 0.2f, 0.2f) : C(1.0f, 0.9f, 0.7f), (broken ? 0 : MPF_EMISSIVE) | MPF_DETAIL | MPF_NO_SHADOW, "glass");
                if (!broken && city && lights < 14 && rng_float(&k->rng) < 0.18f) { klight(k, fp(&f, v3(0, 7.0f, 1.5f)), v3(1.0f, 0.85f, 0.6f), 18, 40, true, false); lights++; }
            }
            // wrecks
            if (rng_float(&k->rng) < (city ? 0.018f : 0.01f)) {
                float lane = (rng_float(&k->rng) < 0.5f ? -1.0f : 1.0f) * rd->width * 0.25f;
                vec2 p = v2_add(s->p, v2_scale(nrm, lane));
                int style = rng_float(&k->rng) < 0.3f ? 1 : (city && rng_float(&k->rng) < 0.25f ? 3 : 0);
                kit_car(k, v3(p.x, s->h, p.y), yaw + rng_range(&k->rng, -0.5f, 0.5f) + (rng_float(&k->rng) < 0.5f ? DG_PI : 0), style);
            }
            // debris
            if (city && rng_float(&k->rng) < 0.05f) {
                vec2 p = v2_add(s->p, v2_scale(nrm, rng_range(&k->rng, -rd->width * 0.5f, rd->width * 0.5f)));
                Frame f = frame(v3(p.x, s->h, p.y), rng_range(&k->rng, 0, 6));
                kbox_r(k, &f, v3(0, 0.1f, 0), v3(rng_range(&k->rng, 0.2f, 0.7f), 0.1f, rng_range(&k->rng, 0.2f, 0.5f)), quat_axis_angle(v3(1, 0, 0), rng_range(&k->rng, -0.3f, 0.3f)),
                       C(0.3f, 0.29f, 0.27f), MPF_DETAIL, "concrete");
            }
        }
        // barricade at one end of some city streets (one lane left open)
        if (city && (r % 5) == 2) {
            const MgRoadSample *s = &g->rs[rd->first + 10];
            vec2 nrm = v2(s->dir.y, -s->dir.x);
            float yaw = yaw_facing(nrm);
            for (int b = 0; b < 2; b++) {
                vec2 p = v2_add(s->p, v2_scale(nrm, -rd->width * 0.5f + 1.2f + 2.1f * (float)b));
                kit_barrier(k, v3(p.x, s->h, p.y), yaw);
            }
            vec2 p = v2_add(s->p, v2_add(v2_scale(nrm, -rd->width * 0.5f + 2.0f), v2_scale(s->dir, 1.5f)));
            kit_sandbags(k, v3(p.x, s->h, p.y), yaw, 3.0f);
        }
    }
    // traffic signals at grid crossings: where two city streets cross (sample within 1 m of another road)
    if (!strcmp(k->p->pois[l->poi - 1].kind, "city")) {
        for (int r = 0; r < g->road_count; r++) {
            const MgRoad *rd = &g->roads[r];
            if (rd->cls != MGR_STREET) continue;
            for (int i = 0; i < rd->count; i += 2) {
                const MgRoadSample *s = &g->rs[rd->first + i];
                if (mappoi_find(k->p, s->p.x, s->p.y) != l->poi - 1 || fabsf(s->dir.y) < 0.9f) continue;   // north-south streets only
                float d;
                int other = mapgen_nearest_road(g, s->p.x, s->p.y, 0.5f, &d);
                (void)d;
                if (other < 0 || g->rs[other].road == r || fabsf(g->rs[other].dir.x) < 0.9f) continue;
                if (i > 0 && i + 1 < rd->count && (i % 4)) continue;
                vec2 corner = v2_add(s->p, v2(rd->width * 0.5f + 1.0f, rd->width * 0.5f + 1.0f));
                float h = ground(k, corner);
                Frame f = frame(v3(corner.x, h, corner.y), DG_PI * 0.25f);
                kcyl(k, &f, v3(0, 2.0f, 0), 0.08f, 2.0f, 1.0f, quat_identity(), C(0.25f, 0.25f, 0.25f), MPF_DETAIL | MPF_COLLIDE | MPF_METAL, "metal_thin");
                kblock(k, &f, 0, 0, 0.18f, 0.18f, 4.0f, 4.9f, C(0.12f, 0.12f, 0.1f), MPF_DETAIL | MPF_METAL, "metal_thin");
                kblock(k, &f, 0, 0.2f, 0.08f, 0.02f, 4.6f, 4.75f, C(0.6f, 0.08f, 0.05f), MPF_DETAIL | MPF_NO_SHADOW, "glass");
                // crosswalk stripes on the four approaches
                for (int a = 0; a < 2; a++) {
                    vec2 dir = a ? v2(1, 0) : v2(0, 1);
                    for (int sgn = -1; sgn <= 1; sgn += 2) {
                        vec2 c = v2_add(s->p, v2_scale(dir, (float)sgn * (rd->width * 0.5f + 2.5f)));
                        Frame cf = frame(v3(c.x, ground(k, c) + 0.075f, c.y), yaw_facing(dir));
                        for (int st = -3; st <= 3; st++) kbox(k, &cf, v3((float)st * 1.1f, 0, 0), v3(0.3f, 0.015f, 1.4f), C(0.75f, 0.75f, 0.72f), MPF_NO_SHADOW | MPF_DETAIL, "asphalt");
                    }
                }
                i += 12;
            }
        }
    }
}

// Roadside encounters along the road network outside the POIs: wrecks, burned cars, an overturned
// truck, barricades (one lane open), checkpoints on main roads, supply stashes.
static void build_encounters(Kit *k) {
    const MapGen *g = k->g;
    k->rng = rng_make(k->p->seed ^ 0xE1u);
    k->poi = 0;
    for (int r = 0; r < g->road_count; r++) {
        const MgRoad *rd = &g->roads[r];
        if (rd->cls == MGR_STREET || rd->cls == MGR_RUNWAY || rd->cls == MGR_TRAIL) continue;
        float next = rng_range(&k->rng, 60, 180);
        for (int i = 0; i < rd->count; i++) {
            if ((float)i * MG_ROAD_STEP < next) continue;
            next += rng_range(&k->rng, 140, 260);
            const MgRoadSample *s = &g->rs[rd->first + i];
            if (s->bridge || mappoi_find(k->p, s->p.x, s->p.y) >= 0 || mapgen_water_level(g, s->p.x, s->p.y) > s->h - 1.0f) continue;
            vec2 nrm = v2(s->dir.y, -s->dir.x);
            float yaw = yaw_facing(s->dir), side = rng_float(&k->rng) < 0.5f ? -1.0f : 1.0f;
            float r0 = rng_float(&k->rng);
            vec2 shoulder = v2_add(s->p, v2_scale(nrm, side * (rd->width * 0.5f + 0.6f)));
            float hs = ground(k, shoulder);
            if (r0 < 0.35f) {
                kit_car(k, v3(shoulder.x, hs, shoulder.y), yaw + rng_range(&k->rng, -0.4f, 0.4f), 0);
                if (rng_float(&k->rng) < 0.4f) {
                    vec2 p2 = v2_add(shoulder, v2_scale(s->dir, 7.0f));
                    kit_car(k, v3(p2.x, ground(k, p2), p2.y), yaw + rng_range(&k->rng, -0.6f, 0.6f), rng_float(&k->rng) < 0.5f ? 1 : 0);
                }
            } else if (r0 < 0.5f) {
                kit_car(k, v3(shoulder.x, hs, shoulder.y), yaw + rng_range(&k->rng, 0.5f, 1.2f), 1);
            } else if (r0 < 0.62f) {   // overturned truck on its side, cargo spilled
                Frame f = frame(v3(shoulder.x, hs, shoulder.y), yaw + rng_range(&k->rng, -0.3f, 0.3f));
                kbox_r(k, &f, v3(side * 1.2f, 1.25f, 0), v3(1.3f, 1.25f, 4.5f), quat_axis_angle(v3(0, 0, 1), DG_PI * 0.5f), C(0.6f, 0.6f, 0.62f), MPF_COLLIDE | MPF_METAL, "metal_thin");
                kbox_r(k, &f, v3(side * 1.0f, 1.0f, 5.6f), v3(1.0f, 1.1f, 1.1f), quat_axis_angle(v3(0, 0, 1), DG_PI * 0.5f), C(0.2f, 0.3f, 0.5f), MPF_COLLIDE | MPF_METAL, "metal_thin");
                for (int c = 0; c < 4; c++) kblock(k, &f, side * (3.0f + rng_range(&k->rng, 0, 2)), rng_range(&k->rng, -4, 2), 0.4f, 0.4f, 0, 0.8f, C(0.4f, 0.32f, 0.2f), MPF_COLLIDE | MPF_DETAIL, "wood");
                kent(k, MPE_CONTAINER, fp(&f, v3(side * 3.5f, 0, 4.0f)), yaw, "industrial");
            } else if (r0 < 0.74f) {   // barricade across one lane
                for (int b = 0; b < 3; b++) {
                    vec2 p = v2_add(s->p, v2_scale(nrm, side * (rd->width * 0.5f - 1.0f - 1.9f * (float)b * 0.5f)));
                    p = v2_add(p, v2_scale(s->dir, (float)b * 1.2f));
                    kit_barrier(k, v3(p.x, ground(k, p), p.y), yaw + DG_PI * 0.5f + 0.25f * (float)b);
                }
                kit_sandbags(k, v3(shoulder.x, hs, shoulder.y), yaw, 3.0f);
            } else if (r0 < 0.82f && rd->cls == MGR_MAIN) {   // small checkpoint
                vec2 p = v2_add(s->p, v2_scale(nrm, side * (rd->width * 0.5f + 4.0f)));
                kit_checkpoint(k, p, yaw_facing(v2_scale(nrm, -side)));
            } else {   // supply stash behind a wreck
                kit_car(k, v3(shoulder.x, hs, shoulder.y), yaw + 0.2f, rng_float(&k->rng) < 0.5f ? 1 : 0);
                vec2 p = v2_add(shoulder, v2_scale(nrm, side * 3.0f));
                kent(k, MPE_CONTAINER, v3(p.x, ground(k, p), p.y), yaw, rd->cls == MGR_DIRT ? "camping" : "household");
            }
        }
    }
}

void mappoi_build(MapPois *p, const MapGen *g, const MgRaster *r) {
    p->prim_count = 0;
    p->ent_count = 0;
    Kit k = { p, g, r, 0, rng_make(p->seed) };
    for (int i = 0; i < p->lot_count; i++) {
        const MpLot *l = &p->lots[i];
        // the 2D outline for the preview / tests
        k.poi = l->poi;
        emit(&k, MPS_BOX, v3(l->pos.x, 0, l->pos.y), v3(l->size.x * 0.5f, 0.01f, l->size.y * 0.5f), quat_yaw(l->yaw), 0, MPF_FOOTPRINT, NULL);
        if (l->kind == LK_STREETLIGHTS) build_city_streets(&k, l);
        else kit_lot(&k, l);
    }
    // suburbs streets get lamps / wrecks too
    for (int i = 0; i < p->poi_count; i++)
        if (!strcmp(p->pois[i].kind, "suburbs")) {
            MpLot sl;
            memset(&sl, 0, sizeof(sl));
            sl.poi = (u8)(i + 1);
            sl.seed = p->seed ^ (u32)(i * 31 + 5);
            build_city_streets(&k, &sl);
        }
    build_markings(&k);
    build_bridges(&k);
    build_encounters(&k);
    // player spawns: the suburbs' streets (or the first POI)
    for (int i = 0; i < p->poi_count; i++) {
        if (strcmp(p->pois[i].kind, "suburbs")) continue;
        int n = 0;
        for (int s = 0; s < g->road_count && n < 8; s++) {
            const MgRoad *rd = &g->roads[s];
            if (rd->cls != MGR_STREET) continue;
            const MgRoadSample *m = &g->rs[rd->first + rd->count / 2];
            if (mappoi_find(p, m->p.x, m->p.y) != i) continue;
            k.poi = (u8)(i + 1);
            kent(&k, MPE_SPAWN, v3(m->p.x, m->h + 0.2f, m->p.y), atan2f(-m->dir.x, -m->dir.y), NULL);
            n++;
        }
    }
}
