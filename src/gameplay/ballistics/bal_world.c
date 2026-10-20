// Ballistics in the game world: bullet pool, launch, per-tick swept hits with lag compensation,
// penetration (no ricochets), events, replays (docs/BALLISTICS.md "In the game").
#include "gameplay/ballistics/bal_world.h"
#include "gameplay/ballistics/bal_tracer.h"
#include "gameplay/ballistics/bal_drag.h"
#include "gameplay/weapons.h"
#include "gameplay/attachments.h"
#include "gameplay/characters.h"
#include "gameplay/recoil.h"
#include "gameplay/effects.h"
#include "gameplay/events.h"
#include "gameplay/health.h"
#include "core/cvar.h"
#include "core/engine.h"
#include "core/log.h"
#include "core/platform.h"
#include "render/debug_draw.h"
#include "net/net.h"
#include "world/weather.h"
#include "world/wind.h"
#include "game/game.h"
#include "audio/audio.h"
#include "gameplay/ballistics/bal_acoustics.h"
#include "gameplay/ballistics/bal_fx.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define GRID_CELL 8.0f
#define GRID_BUCKETS 1024
#define GRID_MAX_ITEMS 8192
#define CHAR_SLACK 4.5f          // broadphase radius around a character (body + rewind motion)
#define MAX_PENDING 64
#define ZERO_CACHE 32
#define MAX_ENDED 64

static struct {
    BalData data;
    bool loaded;
    BalLive pool[BAL_POOL];
    int high;                    // pool high-water mark (live bullets are below it)
    BalStats stats;
    // Tracers that ended, until a frame has drawn their last streak (frame: bal_world_frame count
    // when they ended).
    BalLive ended[MAX_ENDED];
    u32 ended_frame[MAX_ENDED];
    int ended_count;
    u32 frame;
    // EV_SHOT records waiting for the render clock to reach their tick (client).
    struct { BalShotRecord rec; vec3 pos, dir; int slot; } pending[MAX_PENDING];
    int pending_count;
    // Character broadphase (rebuilt each tick there are bullets): hashed 8 m cells.
    int bucket_head[GRID_BUCKETS];
    struct { int next; int ch; } items[GRID_MAX_ITEMS];
    int item_count;
    int legacy_hitboxes;         // C_HITBOX entities that are not characters (rare: old dummies)
    WindField wind;
    // Zero angle cache: (cartridge, speed, zero distance, drag model) -> rad.
    struct { int cart, model; float speed, zero, ang; } zero[ZERO_CACHE];
    int zero_count, zero_next;
    i64 mtime_sig;
    double next_poll;
    vec3 listener;
    int listener_slot;            // whose bullets make no crack here (the local shooter)
    bool have_listener;
    float sound_speed;
    Cvar *enable, *drag_model, *wind_cv, *wind_dir_cv, *spin, *tracers_only, *debug, *watch, *ignition, *shutter, *crack_radius;
} B;

static void play_report(const BalShotRecord *rec, const WeaponDef *wd, const BalDevice *dev, vec3 muzzle, vec3 aim, float extra_delay);

// ------------------------------------------------------------------ data

static bool load_text(const char *rel, bool (*parse)(BalData *, const char *)) {
    char buf[512];
    char *txt = platform_read_file(platform_data_path(rel, buf, sizeof(buf)), NULL);
    if (!txt) { log_warn("ballistics: missing %s", rel); return false; }
    bool ok = parse(&B.data, txt);
    if (!ok) log_warn("ballistics: bad JSON in %s", rel);
    free(txt);
    return ok;
}

static i64 files_signature(void) {
    char buf[512];
    i64 sig = platform_file_mtime(platform_data_path("assets/ballistics/cartridges.json", buf, sizeof(buf)));
    sig = sig * 31 + platform_file_mtime(platform_data_path("assets/ballistics/materials.json", buf, sizeof(buf)));
    int count = 0;
    char **files = SDL_GlobDirectory(platform_data_path("assets/attachments", buf, sizeof(buf)), "*.json", 0, &count);
    for (int i = 0; files && i < count; i++) {
        char rel[256], b2[512];
        snprintf(rel, sizeof(rel), "assets/attachments/%s", files[i]);
        sig = sig * 31 + platform_file_mtime(platform_data_path(rel, b2, sizeof(b2)));
    }
    SDL_free(files);
    return sig;
}

bool bal_world_load(void) {
    bal_data_init(&B.data);
    bool ok = load_text("assets/ballistics/cartridges.json", bal_data_parse_cartridges);
    ok = load_text("assets/ballistics/materials.json", bal_data_parse_materials) && ok;
    char buf[512];
    int count = 0;
    char **files = SDL_GlobDirectory(platform_data_path("assets/attachments", buf, sizeof(buf)), "*.json", 0, &count);
    for (int i = 0; files && i < count; i++) {
        char rel[256];
        snprintf(rel, sizeof(rel), "assets/attachments/%s", files[i]);
        load_text(rel, bal_data_parse_attachment);
    }
    SDL_free(files);
    B.zero_count = 0;
    B.loaded = ok;
    B.mtime_sig = files_signature();
    log_info("ballistics: %d cartridges, %d muzzle devices (hash %08x)", B.data.cart_count, B.data.dev_count, B.data.hash);
    return ok;
}

const BalData *bal_data(void) {
    if (!B.loaded) bal_world_load();
    return &B.data;
}

bool bal_world_enabled(void) { return B.enable ? B.enable->i != 0 : true; }

int bal_weapon_cartridge(const WeaponDef *d) {
    if (!d || !d->cartridge[0] || d->kind != WPN_HITSCAN) return -1;
    return bal_cartridge_find(bal_data(), d->cartridge);
}

int bal_weapon_device(const WeaponDef *d) { return d ? bal_device_find(bal_data(), d->muzzle_device_id) : 0; }

void bal_world_clear(void) {
    for (int i = 0; i < BAL_POOL; i++) B.pool[i].b.flags = BF_DEAD;
    B.high = 0;
    B.pending_count = 0;
    B.ended_count = 0;
}

const BalLive *bal_world_bullets(int *count) {
    *count = B.high;
    return B.pool;
}

const BalLive *bal_world_ended_tracers(int *count) {
    *count = B.ended_count;
    return B.ended;
}

// A tracer stopped at stop_pos, arriving with vel after dist m of path (called before the step's
// trail update: the impact becomes the newest trail point).
static void push_ended(const BalLive *l, vec3 stop_pos, vec3 vel, float dist) {
    if (B.ended_count >= MAX_ENDED) {
        memmove(B.ended, B.ended + 1, sizeof(B.ended[0]) * (MAX_ENDED - 1));
        memmove(B.ended_frame, B.ended_frame + 1, sizeof(B.ended_frame[0]) * (MAX_ENDED - 1));
        B.ended_count--;
    }
    BalLive *e = &B.ended[B.ended_count];
    *e = *l;
    e->b.pos = stop_pos;
    e->b.vel = vel;
    e->b.dist = dist;
    memmove(e->trail + 1, e->trail, sizeof(vec3) * (BAL_TRAIL - 1));
    e->trail[0] = stop_pos;
    if (e->trail_count < BAL_TRAIL) e->trail_count++;
    B.ended_frame[B.ended_count++] = B.frame;
}

// Drops the ended tracers a frame has drawn since they ended.
static void prune_ended(void) {
    int keep = 0;
    for (int i = 0; i < B.ended_count; i++) {
        if (B.ended_frame[i] != B.frame) continue;
        B.ended[keep] = B.ended[i];
        B.ended_frame[keep++] = B.ended_frame[i];
    }
    B.ended_count = keep;
}

const BalStats *bal_world_stats(void) { return &B.stats; }

// Energy on the impact event: log2 scale, 8 steps per octave from 1 J (code 1) to ~40 kJ.
u8 bal_energy_code(float joules) {
    if (joules < 1.0f) return 0;
    int c = 1 + (int)lrintf(log2f(joules) * 16.5f);
    return (u8)DG_CLAMP(c, 1, 255);
}
float bal_energy_decode(u8 code) { return code ? exp2f((float)(code - 1) / 16.5f) : 0.0f; }

float bal_impact_cavity(u32 id) {
    u32 o = bal_impact_outcome(id);
    return cbrtf(bal_impact_energy(id) / 1000.0f) * ((o & BAL_IMP_FRAGMENT) ? 2.5f : 1.0f) * ((o & BAL_IMP_YAWED) ? 1.5f : 1.0f);
}

// ------------------------------------------------------------------ air

static int drag_model(void) {
    int m = B.drag_model ? B.drag_model->i : -1;
    if (m < 0) m = BAL_DRAG_TABLE;   // auto: the table everywhere (the exp model is opt-in, bal_drag_model 0)
    return m;
}

static void wind_update(const World *w) {
    WeatherParams wp;
    weather_eval(&w->weather, w->tick, SIM_DT, &wp);
    WindBase base;
    base.dir_deg = wp.v[WP_WIND_DIR];
    base.speed = wp.v[WP_WIND];
    base.gust = wp.v[WP_GUST];
    // bal_wind (host, replicated): a steady wind for the range, no gusts.
    if (B.wind_cv && B.wind_cv->f >= 0.0f) {
        base.speed = B.wind_cv->f;
        base.dir_deg = B.wind_dir_cv ? B.wind_dir_cv->f : 90.0f;
        base.gust = 0.0f;
    }
    if (!B.wind.seed) wind_init(&B.wind, 0xBA11u);
    wind_set(&B.wind, &base, (double)w->tick * SIM_DT);
}

BalEnv bal_world_env(const World *w, vec3 pos) {
    WeatherParams wp;
    weather_eval(&w->weather, w->tick, SIM_DT, &wp);
    BalEnv e = bal_env_make(wp.v[WP_TEMPERATURE], 0.0f, drag_model());
    e.spin_drift = B.spin ? B.spin->i != 0 : true;
    float p[3] = { pos.x, pos.y, pos.z }, o[3];
    wind_at(&B.wind, p, o);
    e.wind = v3(o[0], o[1], o[2]);
    return e;
}

static float zero_angle(int cart, float speed, float zero_m) {
    int model = drag_model();
    for (int i = 0; i < B.zero_count; i++)
        if (B.zero[i].cart == cart && B.zero[i].model == model && B.zero[i].speed == speed && B.zero[i].zero == zero_m) return B.zero[i].ang;
    float a = bal_zero_angle(&B.data.cart[cart], speed, zero_m, 0.0f, model);
    int k = B.zero_count < ZERO_CACHE ? B.zero_count++ : (B.zero_next++ % ZERO_CACHE);
    B.zero[k].cart = cart;
    B.zero[k].model = model;
    B.zero[k].speed = speed;
    B.zero[k].zero = zero_m;
    B.zero[k].ang = a;
    return a;
}

// The zero of a shot: the distance under the crosshair (docs/BALLISTICS.md "Launch"), so the bullets land
// where it says at any range. The bullets leave the eye on the line of sight: a fixed zero_distance put
// every shot nearer than it above the crosshair (an 8x zeroed at 300 m hit high at 50 m). The static
// world only, so the host and the predicting client agree; 1 m steps (5 m past 100 m: the solve is
// cached); nothing within 1000 m: the weapon's zero_distance.
static float aimed_zero(World *w, vec3 origin, vec3 aim, float fallback) {
    const float range = 1000.0f;
    RayHit rh;
    if (!w || !w->phys || v3_len2(aim) < 1e-8f) return fallback;
    if (!phys_raycast(w->phys, origin, v3_scale(v3_norm(aim), range), PL_MASK_WORLD, PHYS_INVALID_BODY, &rh)) return fallback;
    float dist = clampf(rh.fraction * range, 5.0f, range);
    return dist < 100.0f ? roundf(dist) : 5.0f * roundf(dist / 5.0f);
}

// Rotates a pellet direction up by the zero elevation (+ the device's POI shift).
static vec3 launch_dir(vec3 d, float up, float right) {
    vec3 r = v3_cross(d, v3(0, 1, 0));
    if (v3_len2(r) < 1e-8f) r = v3(1, 0, 0);
    r = v3_norm(r);
    vec3 u = v3_cross(r, d);
    vec3 e = v3_add(v3_scale(d, cosf(up)), v3_scale(u, sinf(up)));
    return v3_norm(v3_add(v3_scale(e, cosf(right)), v3_scale(r, sinf(right))));
}

// ------------------------------------------------------------------ pool

static BalLive *alloc_bullet(void) {
    for (int i = 0; i < BAL_POOL; i++) {
        if (i < B.high && !(B.pool[i].b.flags & BF_DEAD)) continue;
        if (i >= B.high) B.high = i + 1;
        return &B.pool[i];
    }
    // Full: recycle the oldest replay / prediction (visual) before an authoritative bullet.
    BalLive *best = &B.pool[0];
    for (int i = 0; i < BAL_POOL; i++) {
        BalLive *l = &B.pool[i];
        if ((l->role != BAL_ROLE_HOST) > (best->role != BAL_ROLE_HOST) || (l->role == best->role && l->b.time > best->b.time)) best = l;
    }
    return best;
}

static void spawn_shot(World *w, const BalShotRecord *rec, int slot, vec3 pos, vec3 aim, int role, u32 view_tick256, vec3 muzzle) {
    const BalCartridge *c = &B.data.cart[rec->cart];
    Rng rng = rng_make(rec->seed);
    int pellets = DG_MAX(1, rec->pellets);
    for (int p = 0; p < pellets; p++) {
        vec3 sdir = recoil_cone_dir(aim, rec->spread, &rng);
        vec3 d = launch_dir(sdir, rec->zero_angle + rec->poi[1], rec->poi[0]);
        BalLive *l = alloc_bullet();
        memset(l, 0, sizeof(*l));
        bal_launch(&l->b, c, rec->cart, pos, d, rec->speed, rec->seed, p, p == 0 && (rec->flags & BAL_SHOT_TRACER));
        l->role = (u8)role;
        l->slot = (u8)slot;
        l->shot_id = rec->shot_id;
        l->weapon = rec->weapon;
        memcpy(l->att, rec->att, sizeof(l->att));
        l->fire_tick = rec->fire_tick;
        l->view_tick256 = view_tick256;
        l->launch = pos;
        l->prev_pos = pos;
        l->trail[0] = pos;
        l->trail_count = 1;
        l->visual_offset = v3_sub(muzzle, pos);
        (void)w;
    }
}

void bal_fire(World *w, const BalFireIn *in) {
    const WeaponDef *d = in->def;
    int ci = bal_weapon_cartridge(d);
    if (ci < 0) return;
    bool host = w->authority;
    if (!host && !(in->local_view && in->first)) return;   // clients only predict their own shots
    const BalCartridge *c = &B.data.cart[ci];
    int di = bal_weapon_device(d);
    const BalDevice *dev = &B.data.dev[di];
    BalShotRecord rec;
    memset(&rec, 0, sizeof(rec));
    rec.seed = in->seed;
    rec.fire_tick = w->tick;
    rec.shot_id = (u16)(((u32)in->slot << 12) | (in->shot_counter & 0xFFFu));
    rec.weapon = (u16)in->weapon_index;
    if (in->att) memcpy(rec.att, in->att, sizeof(rec.att));
    rec.cart = (u8)ci;
    rec.pellets = (u8)DG_CLAMP(d->pellets > 1 ? d->pellets : c->pellets, 1, 32);
    bool forced = B.tracers_only && B.tracers_only->i;
    if (rec.pellets == 1 && bal_tracer_round(&c->tracer, in->mag_seed, in->shot_in_mag, in->mag_size, false, forced))
        rec.flags |= BAL_SHOT_TRACER;
    rec.device = (u8)di;
    rec.muzzle_type = (u8)d->vfx_muzzle_type;
    rec.vfx_device = (u8)d->vfx_muzzle_device;
    rec.caliber = (u8)d->vfx_caliber;
    rec.spread = in->spread;
    rec.speed = bal_cartridge_mv(c, d->barrel_length) * dev->velocity_mul;
    rec.zero_angle = zero_angle(ci, rec.speed, aimed_zero(w, in->origin, in->aim, d->zero_distance));
    const float moa = 3.14159265f / (180.0f * 60.0f);
    rec.poi[0] = dev->poi_moa[0] * moa;
    rec.poi[1] = dev->poi_moa[1] * moa;
    spawn_shot(w, &rec, in->slot, in->origin, in->aim, host ? BAL_ROLE_HOST : BAL_ROLE_PREDICT, host ? in->view_tick256 : 0, in->muzzle);
    if (host) {
        GameEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = EV_SHOT;
        ev.target = EVENT_ALL;
        ev.except = (u8)in->slot;
        ev.ent = w->players[in->slot].ent;
        ev.ent2 = (u32)in->slot + 1u;
        ev.pos = in->origin;   // the shooter's camera: every replay starts where the host's bullets did
        ev.dir = in->aim;
        memcpy(ev.data, &rec, sizeof(rec));
        ev_push(w, &ev);
    }
}

void bal_on_shot_event(World *w, const GameEvent *ev, vec3 muzzle) {
    BalShotRecord rec;
    memcpy(&rec, ev->data, sizeof(rec));
    if (rec.cart >= B.data.cart_count) return;
    // The report, timed from the moment the shot happens on this machine's clock.
    float early = 0.0f;
    if (!w->authority) early = (float)(((double)rec.fire_tick - net_client_render_tick()) * SIM_DT);
    if (w->items && rec.weapon < w->items->weapon_count)
        play_report(&rec, &w->items->weapons[rec.weapon], &B.data.dev[DG_MIN(rec.device, B.data.dev_count - 1)], muzzle, ev->dir, early);
    if (w->authority) return;   // the host has the real bullets
    if (B.pending_count >= MAX_PENDING) memmove(B.pending, B.pending + 1, sizeof(B.pending[0]) * (MAX_PENDING - 1)), B.pending_count--;
    B.pending[B.pending_count].rec = rec;
    B.pending[B.pending_count].pos = ev->pos;
    B.pending[B.pending_count].dir = ev->dir;
    B.pending[B.pending_count].slot = (int)ev->ent2 - 1;
    B.pending_count++;
}

// ------------------------------------------------------------------ character broadphase

static u32 cell_hash(int x, int y, int z) { return (dg_hash_u32((u32)x * 73856093u ^ (u32)y * 19349663u ^ (u32)z * 83492791u)) & (GRID_BUCKETS - 1); }
static int cell_of(float v) { return (int)floorf(v / GRID_CELL); }

static void grid_build(World *w) {
    for (int i = 0; i < GRID_BUCKETS; i++) B.bucket_head[i] = -1;
    B.item_count = 0;
    if (!w->chars) return;
    for (int k = 0; k < MAX_CHARACTERS; k++) {
        CharacterInst *c = w->chars->c[k];
        if (!c || c->dead) continue;
        vec3 p = v3_add(c->root.t, v3(0, 0.95f, 0));
        int x0 = cell_of(p.x - CHAR_SLACK), x1 = cell_of(p.x + CHAR_SLACK);
        int y0 = cell_of(p.y - CHAR_SLACK), y1 = cell_of(p.y + CHAR_SLACK);
        int z0 = cell_of(p.z - CHAR_SLACK), z1 = cell_of(p.z + CHAR_SLACK);
        for (int x = x0; x <= x1; x++)
            for (int y = y0; y <= y1; y++)
                for (int z = z0; z <= z1; z++) {
                    if (B.item_count >= GRID_MAX_ITEMS) return;
                    u32 h = cell_hash(x, y, z);
                    B.items[B.item_count].ch = k;
                    B.items[B.item_count].next = B.bucket_head[h];
                    B.bucket_head[h] = B.item_count++;
                }
    }
    B.legacy_hitboxes = 0;
    ENT_FOREACH(w, i) {
        if ((w->comps[i] & C_HITBOX) && !char_get_index(w, i)) B.legacy_hitboxes++;
    }
}

// Characters whose cells the segment's box touches (deduplicated).
static int grid_query(vec3 a, vec3 b, int *out, int max) {
    int n = 0;
    int x0 = cell_of(fminf(a.x, b.x)), x1 = cell_of(fmaxf(a.x, b.x));
    int y0 = cell_of(fminf(a.y, b.y)), y1 = cell_of(fmaxf(a.y, b.y));
    int z0 = cell_of(fminf(a.z, b.z)), z1 = cell_of(fmaxf(a.z, b.z));
    if ((x1 - x0 + 1) * (y1 - y0 + 1) * (z1 - z0 + 1) > 64) return -1;   // huge segment: caller scans everything
    for (int x = x0; x <= x1; x++)
        for (int y = y0; y <= y1; y++)
            for (int z = z0; z <= z1; z++)
                for (int it = B.bucket_head[cell_hash(x, y, z)]; it >= 0; it = B.items[it].next) {
                    int ch = B.items[it].ch;
                    bool dup = false;
                    for (int k = 0; k < n; k++) if (out[k] == ch) { dup = true; break; }
                    if (!dup && n < max) out[n++] = ch;
                }
    return n;
}

// ------------------------------------------------------------------ hits

typedef struct SegHit {
    HitResult h;
    float t;            // along the segment
    float chord;        // characters: path inside the capsule
} SegHit;

static bool trace_segment(World *w, BalLive *l, vec3 o, vec3 dir, float len, SegHit *out) {
    memset(out, 0, sizeof(*out));
    HitResult *r = &out->h;
    r->tire = -1;
    r->part = -1;
    B.stats.rays++;
    Player *sp = &w->players[l->slot];
    // The shooter's own body / vehicle only while the bullet is leaving the muzzle.
    BodyId ignore = l->b.dist < 3.0f ? weapon_shooter_ignore_body(w, l->slot) : PHYS_INVALID_BODY;
    RayHit rh;
    if (phys_raycast(w->phys, o, v3_scale(dir, len), PL_MASK_WORLD, ignore, &rh)) {
        r->hit = true;
        r->dist = rh.distance;
        r->point = rh.point;
        r->normal = rh.normal;
        r->ent = (EntityId)rh.user;
        r->body = rh.body;
        r->material = rh.material;
    }
    float best = r->hit ? r->dist : len;
    u32 view256 = 0;
    if (l->role == BAL_ROLE_HOST && l->view_tick256) view256 = l->view_tick256 + (w->tick - l->fire_tick) * 256u;
    const WeaponVehicleHooks *vh = weapons_vehicle_hooks();
    if (vh->ray_tires) {
        EntityId veh = ENT_NONE;
        float tt = 0;
        int wheel = vh->ray_tires(w, o, dir, best, view256 >> 8, &veh, &tt);
        if (wheel >= 0 && tt < best) {
            best = tt;
            r->hit = true;
            r->dist = tt;
            r->point = v3_madd(o, dir, tt);
            r->normal = v3_neg(dir);
            r->ent = veh;
            r->body = PHYS_INVALID_BODY;
            r->material = SURF_CLOTH;
            r->tire = wheel;
        }
    }
    if (B.legacy_hitboxes && weapon_trace_legacy_hitboxes(w, l->slot, o, dir, best, view256, r)) best = r->dist;
    // Characters through the grid.
    int cand[256];
    vec3 end = v3_madd(o, dir, best);
    int n = grid_query(o, end, cand, 256);
    if (n < 0) {
        n = 0;
        for (int k = 0; k < MAX_CHARACTERS && n < 256; k++)
            if (w->chars && w->chars->c[k]) cand[n++] = k;
    }
    for (int i = 0; i < n; i++) {
        CharacterInst *c = w->chars->c[cand[i]];
        if (!c || c->ent == sp->ent) continue;
        HitboxRayHit hh;
        float chord = 0;
        if (!chars_raycast_one(w, c, o, dir, best, view256, &hh, &chord)) continue;
        if (hh.t >= best) continue;
        best = hh.t;
        r->hit = true;
        r->dist = hh.t;
        r->point = hh.point;
        r->normal = v3_neg(dir);
        r->ent = c->ent;
        r->hitbox = true;
        r->character = true;
        r->part = hh.part;
        r->headshot = hh.part == BP_HEAD;
        r->body = PHYS_INVALID_BODY;
        r->material = SURF_FLESH;
        r->tire = -1;
        out->chord = chord;
    }
    out->t = r->hit ? r->dist : len;
    return r->hit;
}

// Thickness of the body that was hit along the path: the ray cast back from where the bullet
// could reach. < 0 = unknown (nothing found from the far side).
static float measure_thickness(World *w, const SegHit *s, vec3 dir, float reach) {
    if (s->h.body == PHYS_INVALID_BODY || reach <= 0.0f) return -1.0f;
    vec3 far = v3_madd(s->h.point, dir, reach);
    vec3 from = far;
    float travelled = 0.0f;
    for (int it = 0; it < 3; it++) {
        RayHit rh;
        float left = reach - travelled;
        if (left <= 1e-4f || !phys_raycast(w->phys, from, v3_scale(dir, -left), PL_MASK_WORLD, PHYS_INVALID_BODY, &rh)) return -1.0f;
        if (rh.body == s->h.body) {
            // A ray starting inside a convex solid hits at 0 (thickness = reach: not defeated); one
            // starting inside a mesh sees the entry face from behind, right at the entry point.
            if (rh.distance >= left - 0.0005f) return reach * 2.0f;
            return fmaxf(reach - travelled - rh.distance, 0.0f);
        }
        // Another body between: step past it and look again.
        travelled += rh.distance + 0.002f;
        from = v3_madd(far, dir, -travelled);
    }
    return -1.0f;
}

static u8 outcome_bits(const BalHitResult *r, bool exit) {
    u8 b = BAL_IMP_BALLISTIC;
    if (r->yawed) b |= BAL_IMP_YAWED;
    if (r->outcome == BAL_PENETRATE) b |= BAL_IMP_PENETRATE;
    if (r->fragment) b |= BAL_IMP_FRAGMENT;
    if (exit) b |= BAL_IMP_EXIT;
    return b;
}

static const WeaponDef *effective_def(World *w, const BalLive *l, WeaponDef *tmp) {
    if (!w->items || l->weapon >= w->items->weapon_count) return NULL;
    const WeaponDef *base = &w->items->weapons[l->weapon];
    attach_weapon_effective(w->items, base, l->att, tmp);
    return tmp;
}

// Resolves one hit. Returns true when the bullet continues (penetration).
static bool resolve_hit(World *w, BalLive *l, const SegHit *s, vec3 dir, float path_before) {
    const BalCartridge *c = &B.data.cart[l->b.cart];
    BalHitResult r;
    float e_launch = c->e_ref;
    HitResult h = s->h;
    h.dist = path_before + s->t;
    if (h.character) {
        r = bal_hit_flesh(&l->b, c, &B.data.mat[SURF_FLESH], h.point, dir, fmaxf(s->chord, 0.02f), l->flesh_depth);
        l->flesh_depth += s->chord;
    } else if (h.hitbox || h.tire >= 0) {
        memset(&r, 0, sizeof(r));   // legacy hitboxes / tires: the bullet stays in them
        r.outcome = BAL_STOP;
        r.energy_in = r.energy_deposit = bal_bullet_energy(&l->b);
    } else {
        int mat = DG_CLAMP(h.material, 0, SURF_COUNT - 1);
        float cap = bal_capacity(&B.data.mat[mat], c, bal_bullet_energy(&l->b));
        float thick = measure_thickness(w, s, dir, fminf(cap + 0.01f, 3.0f));
        r = bal_hit_surface(&l->b, c, &B.data.mat[mat], h.point, dir, h.normal, thick);
    }
    u32 extra = BAL_IMPACT_EXTRA(h.character ? r.energy_deposit : r.energy_in, outcome_bits(&r, false) | ((l->b.flags & BF_TRACER) ? BAL_IMP_TRACER : 0));
    if (l->role == BAL_ROLE_HOST) {
        WeaponDef tmp;
        const WeaponDef *d = effective_def(w, l, &tmp);
        if (d) {
            // Damage keeps the weapon's balance (damage x part x falloff); ballistics scales it by
            // the energy lost on the way (earlier penetrations) and by how much of it this target
            // took (a clean through-and-through deposits less).
            float before = l->b.events ? fminf(r.energy_in / fmaxf(e_launch, 1.0f), 1.0f) : 1.0f;
            float deposit = r.energy_in > 0.0f ? r.energy_deposit / r.energy_in : 1.0f;
            float scale = before * lerpf(0.75f, 1.0f, saturate(deposit));
            bool was_alive = h.character && ent_valid(w, h.ent) && !(w->flags[ENT_INDEX(h.ent)] & EF_DEAD);
            health_set_collateral(l->kills > 0);   // through someone it already killed: the kill feed's collateral
            weapon_apply_hit(w, l->slot, d, &h, dir, scale);
            health_set_collateral(false);
            bool killed = was_alive && (w->flags[ENT_INDEX(h.ent)] & EF_DEAD);
            if (killed && l->kills < 255) l->kills++;
            weapon_push_impact(w, l->slot, d, &h, dir, killed, extra);
            if (w->players[l->slot].local) weapon_local_impact(w, d, &h, dir, extra);   // listen-server shooter
            // A burning tracer stopped in dry grass: the fire hook (stub until foliage burns).
            if ((l->b.flags & BF_TRACER) && r.outcome == BAL_STOP && h.material == SURF_GRASS) {
                WeatherLocal wl = weather_at(w, h.point);
                if (wl.wetness < 0.2f && wl.rain <= 0.0f && wl.snow <= 0.0f) fire_try_ignite(w, h.point, r.energy_in);
            }
            if (r.outcome == BAL_PENETRATE && !h.character) {
                // Exit-side event: spall cone on the far face.
                HitResult ex = h;
                ex.point = v3_madd(r.out_pos, dir, -0.002f);
                ex.normal = dir;
                ex.ent = ENT_NONE;
                ex.body = PHYS_INVALID_BODY;
                u32 xe = BAL_IMPACT_EXTRA(r.energy_out, outcome_bits(&r, true));
                weapon_push_impact(w, l->slot, d, &ex, dir, false, xe);
                if (w->players[l->slot].local) weapon_local_impact(w, d, &ex, dir, xe);
            }
        }
    } else if (l->role == BAL_ROLE_PREDICT) {
        WeaponDef tmp;
        const WeaponDef *d = effective_def(w, l, &tmp);
        if (d) {
            weapon_local_impact(w, d, &h, dir, extra);
            if (h.character) chars_predict_hit(w, h.ent, h.part, dir);
            if (r.outcome == BAL_PENETRATE && !h.character) {
                HitResult ex = h;
                ex.point = v3_madd(r.out_pos, dir, -0.002f);
                ex.normal = dir;
                ex.ent = ENT_NONE;
                weapon_local_impact(w, d, &ex, dir, BAL_IMPACT_EXTRA(r.energy_out, outcome_bits(&r, true)));
            }
        }
    }
    if (B.debug && B.debug->i) {
        u32 col = r.outcome == BAL_PENETRATE ? DBG_CYAN : DBG_RED;
        dbg_cross(h.point, 0.08f, col, 8.0f);
        if (r.outcome == BAL_PENETRATE) dbg_cross(r.out_pos, 0.08f, DBG_MAGENTA, 8.0f);
        if (B.debug->i >= 2)
            log_info("ballistics: %s shot %04x %s %s at %.1f m (%.2f %.2f %.2f), %.0f J in, %.0f J out, incidence %.1f deg, cap %.3f m, thick %.3f m",
                     l->role == BAL_ROLE_HOST ? "host" : (l->role == BAL_ROLE_PREDICT ? "predict" : "replay"), l->shot_id,
                     h.character ? "flesh" : bal_material_name(h.material),
                     r.outcome == BAL_PENETRATE ? "penetrate" : "stop", h.dist, h.point.x, h.point.y, h.point.z,
                     r.energy_in, r.energy_out, r.incidence * DG_RAD2DEG, r.capacity, r.thickness);
    }
    return bal_apply_hit(&l->b, &r);
}

// ------------------------------------------------------------------ sound

// The supersonic crack (Mach cone sweeping the listener) or the subsonic whizz of a bullet
// passing this machine's listener; never for the listener's own shots.
static void listen_step(BalLive *l, const BalEnv *env, vec3 from, float dt) {
    if (!B.have_listener || l->crack_done || l->slot == B.listener_slot) return;
    float speed = bal_bullet_speed(&l->b);
    float radius = B.crack_radius ? B.crack_radius->f : 120.0f;
    float t0 = l->b.time - dt, arrival, miss;
    // Cheap reject: the listener is not near this step.
    vec3 c = v3_lerp(from, l->b.pos, 0.5f);
    float half = 0.5f * v3_dist(from, l->b.pos) + radius;
    if (v3_len2(v3_sub(c, B.listener)) > half * half) return;
    float delay;
    const BalCartridge *cart = &B.data.cart[l->b.cart];
    AudioParams ap = { 0 };
    AudioEmit e;
    if (speed > env->sound_speed) {
        if (!bal_crack_segment(from, l->b.pos, t0, speed, env->sound_speed, B.listener, &arrival, &miss) || miss > radius) return;
        // The N-wave (assets/audio/recipes/bullet_crack.json): its length from the miss distance and the
        // bullet's size, heard exactly at the shock's arrival (the environment adds the rest).
        delay = arrival - l->b.time;
        audio_emit_init(&e, "bullet_crack", v3_lerp(from, l->b.pos, 0.5f));
        e.delay = fmaxf(delay, 0.0f);
        e.volume = bal_crack_gain(miss);
        e.seed = dg_hash_u32((u32)l->shot_id * 2654435761u ^ 0xC4AC4u);
        audio_param(&ap, "miss", miss);
        audio_param(&ap, "caliber", cart->diameter * 1000.0f);
        audio_param(&ap, "speed", speed);
        if (delay > -0.1f) audio_emit(&e, &ap);
        if (B.debug && B.debug->i) {
            // The supersonic radius around the path where it passed, and when the crack is heard.
            char txt[64];
            snprintf(txt, sizeof(txt), "crack +%.2f s (%.0f m)", arrival, miss);
            dbg_circle(v3_lerp(from, l->b.pos, 0.5f), v3_norm(v3_sub(l->b.pos, from)), miss, DBG_RED, 4.0f);
            dbg_text(v3_add(B.listener, v3(0, 0.4f, 0)), txt, DBG_RED, 0.05f, 4.0f);
        }
        if (B.debug && B.debug->i >= 2) log_info("ballistics: crack shot %04x miss %.1f m, heard in %.3f s (flight %.3f s)", l->shot_id, miss, delay, arrival);
    } else {
        if (!bal_whizz_segment(from, l->b.pos, t0, speed, env->sound_speed, B.listener, &arrival, &miss) || miss > 8.0f) return;
        delay = arrival - l->b.time;
        audio_emit_init(&e, "bullet_whizz", B.listener);
        e.delay = fmaxf(delay, 0.0f);
        e.volume = clampf(1.5f / fmaxf(miss, 0.5f), 0.1f, 1.0f);
        e.pitch = clampf(speed / 300.0f, 0.6f, 1.3f);
        e.seed = dg_hash_u32((u32)l->shot_id * 2654435761u ^ 0x3122u);
        audio_param(&ap, "speed", speed);
        audio_param(&ap, "miss", miss);
        if (delay > -0.1f) audio_emit(&e, &ap);
        if (B.debug && B.debug->i >= 2) log_info("ballistics: whizz shot %04x miss %.1f m, heard in %.3f s", l->shot_id, miss, delay);
    }
    l->crack_done = 1;
}

// The shot's physical values for the gunshot recipe (assets/audio/recipes/gunshot.json): muzzle
// energy of every pellet, calibre, barrel, muzzle device; the replicated shot seed varies it the same
// way on every machine.
static void shot_params(AudioParams *ap, const BalCartridge *c, const WeaponDef *wd, const BalDevice *dev, float speed, int pellets, bool local) {
    float energy = 0.5f * c->mass * speed * speed * (float)(pellets > 0 ? pellets : 1);
    audio_param(ap, "energy", energy);
    audio_param(ap, "caliber", c->diameter * 1000.0f);
    audio_param(ap, "barrel", wd->barrel_length > 0 ? wd->barrel_length : c->ref_barrel);
    audio_param(ap, "device", (float)dev->kind);
    audio_param(ap, "pellets", (float)pellets);
    audio_param(ap, "auto", wd->bolt_action ? 0.0f : 1.0f);
    audio_param(ap, "local", local ? 1.0f : 0.0f);
}

// A shot's report for listeners away from the gun: travel time (and the shooter's head start on a
// client), the device's level (suppressor -28 dB, brake +3 / +6 dB to the side); the recipe and the
// environment do the rest (blast, air absorption, the distant layer, echoes, rooms).
static void play_report(const BalShotRecord *rec, const WeaponDef *wd, const BalDevice *dev, vec3 muzzle, vec3 aim, float extra_delay) {
    vec3 L = B.have_listener ? B.listener : audio_listener_pos();
    float d = v3_dist(muzzle, L);
    float c = B.sound_speed > 100.0f ? B.sound_speed : 343.0f;
    float db = dev->report_db;
    if (dev->report_db_side != 0.0f && d > 0.5f) {
        float side = 1.0f - fabsf(v3_dot(v3_norm(v3_sub(L, muzzle)), aim));   // 1 beside the muzzle, 0 ahead / behind
        db += dev->report_db_side * side;
    }
    AudioParams ap = { 0 };
    shot_params(&ap, &B.data.cart[rec->cart], wd, dev, rec->speed, rec->pellets, false);
    AudioEmit e;
    audio_emit_init(&e, "gunshot", muzzle);
    e.fwd = aim;
    e.sample = wd->sound_fire;
    e.volume = bal_db_gain(db);
    e.seed = rec->seed ^ ((u32)rec->shot_id << 16);
    e.delay = fmaxf(bal_report_delay(d, c) + extra_delay, 0.0f);
    audio_emit(&e, &ap);
    if (B.debug && B.debug->i) {
        char txt[64];
        snprintf(txt, sizeof(txt), "report +%.2f s (%.0f m)", e.delay, d);
        dbg_text(v3_add(L, v3(0, 0.3f, 0)), txt, DBG_YELLOW, 0.05f, 4.0f);
    }
    if (B.debug && B.debug->i >= 2) log_info("ballistics: report %s at %.0f m, heard in %.3f s (%+.0f dB)", wd->sound_fire, d, e.delay, db);
}

void bal_shooter_report(const WeaponDef *d, vec3 eye) {
    const BalDevice *dev = &bal_data()->dev[bal_weapon_device(d)];
    int ci = bal_weapon_cartridge(d);
    AudioEmit e;
    audio_emit_init(&e, "gunshot", eye);
    e.sample = d->sound_fire;
    e.volume = bal_db_gain(dev->report_db);
    e.seed = dg_hash_u32((u32)(platform_time() * 1000.0));
    e.delay = 0.0f;
    AudioParams ap = { 0 };
    if (ci >= 0) {
        const BalCartridge *c = &bal_data()->cart[ci];
        shot_params(&ap, c, d, dev, bal_cartridge_mv(c, d->barrel_length) * dev->velocity_mul, d->pellets > 1 ? d->pellets : c->pellets, true);
    }
    audio_emit(&e, &ap);
}

// ------------------------------------------------------------------ tick

static void step_bullet(World *w, BalLive *l, float dt) {
    const BalCartridge *c = &B.data.cart[l->b.cart];
    BalEnv env = bal_world_env(w, l->b.pos);
    vec3 from = l->b.pos;
    vec3 from_step = from;
    float path_before = l->b.dist;
    l->prev_pos = from;
    bool alive = bal_step(&l->b, c, &env, dt);
    B.stats.steps++;
    vec3 to = l->b.pos;
    // Sweep the step; a penetration continues along the new direction for the rest of the step's
    // length.
    for (int guard = 0; guard < BAL_MAX_EVENTS + 1; guard++) {
        vec3 seg = v3_sub(to, from);
        float len = v3_len(seg);
        if (len < 1e-4f) break;
        vec3 dir = v3_scale(seg, 1.0f / len);
        SegHit s;
        if (!trace_segment(w, l, from, dir, len, &s)) break;
        vec3 vel_in = l->b.vel;
        if (!resolve_hit(w, l, &s, dir, path_before)) {
            alive = false;
            // A tracer's last streak ends at the impact (bal_apply_hit leaves the position at the
            // end of the step and zeroes the velocity). Spent tracers have burnt out long before.
            if (l->b.flags & BF_TRACER) push_ended(l, s.h.point, vel_in, path_before + s.t);
            break;
        }
        float rest = fmaxf(len - s.t, 0.0f);
        path_before += s.t;
        from = l->b.pos;   // exit point
        to = v3_madd(from, v3_norm(l->b.vel), rest);
        l->b.pos = to;
    }
    if (B.debug && B.debug->i) {
        float mach = bal_bullet_mach(&l->b, &env);
        u32 col = mach > 1.2f ? DBG_RED : (mach > 0.8f ? DBG_ORANGE : DBG_BLUE);
        if (l->role != BAL_ROLE_REPLAY || !w->authority) dbg_line(l->prev_pos, l->b.pos, col, 6.0f, true);
    }
    listen_step(l, &env, from_step, dt);
    // Trail for the tracer ribbon (newest first).
    memmove(l->trail + 1, l->trail, sizeof(vec3) * (BAL_TRAIL - 1));
    l->trail[0] = l->b.pos;
    if (l->trail_count < BAL_TRAIL) l->trail_count++;
    if (!alive) l->b.flags |= BF_DEAD;
}

static void spawn_pending(World *w) {
    if (!B.pending_count) return;
    double now = net_client_render_tick();
    int keep = 0;
    for (int i = 0; i < B.pending_count; i++) {
        double t = (double)B.pending[i].rec.fire_tick;
        if (now + 0.5 < t) { B.pending[keep++] = B.pending[i]; continue; }
        // Due (or late: fast-forward to the render clock, at most a second).
        int slot = DG_CLAMP(B.pending[i].slot, 0, MAX_PLAYERS - 1);
        // The tracer from the shooter's visible barrel (the bullets from its camera).
        vec3 muzzle = B.pending[i].pos;
        if (ent_valid(w, w->players[slot].ent)) {
            vec3 md;
            chars_item_muzzle(w, w->players[slot].ent, &muzzle, &md);
        }
        BalShotRecord rec = B.pending[i].rec;
        spawn_shot(w, &rec, slot, B.pending[i].pos, B.pending[i].dir, BAL_ROLE_REPLAY, 0, muzzle);
        int late = (int)floor(now - t);
        if (late > 0) {
            late = DG_MIN(late, SIM_HZ);
            for (int k = 0; k < B.high; k++) {
                BalLive *l = &B.pool[k];
                if ((l->b.flags & BF_DEAD) || l->role != BAL_ROLE_REPLAY || l->shot_id != rec.shot_id || l->b.time > 0.0f) continue;
                for (int s = 0; s < late && !(l->b.flags & BF_DEAD); s++) step_bullet(w, l, SIM_DT);
            }
        }
    }
    B.pending_count = keep;
}

void bal_world_tick(World *w, float dt) {
    if (!B.loaded) bal_world_load();
    double t0 = platform_time();
    B.stats.steps = B.stats.rays = 0;
    prune_ended();
    if (!w->authority) spawn_pending(w);
    int live = 0;
    for (int i = 0; i < B.high; i++)
        if (!(B.pool[i].b.flags & BF_DEAD)) live++;
    B.stats.live = live;
    if (!live) { B.high = 0; B.stats.tick_ms = 0; return; }
    wind_update(w);
    grid_build(w);
    int high = 0;
    for (int i = 0; i < B.high; i++) {
        BalLive *l = &B.pool[i];
        if (l->b.flags & BF_DEAD) continue;
        step_bullet(w, l, dt);
        if (!(l->b.flags & BF_DEAD)) high = i + 1;
    }
    B.high = high;
    B.stats.tick_ms = (platform_time() - t0) * 1000.0;
    if (B.stats.tick_ms > B.stats.worst_ms) B.stats.worst_ms = B.stats.tick_ms;
}

void bal_world_frame(World *w, vec3 listener, float dt) {
    (void)dt;
    B.frame++;   // the tracers that ended before this point are drawn by this frame
    B.listener = listener;
    B.have_listener = true;
    B.listener_slot = game_view_slot();
    if (w) {
        WeatherParams wp;
        weather_eval(&w->weather, w->tick, SIM_DT, &wp);
        B.sound_speed = bal_sound_speed(wp.v[WP_TEMPERATURE]);
        audio_set_sound_speed(B.sound_speed);
    }
    // Hot reload: poll the data files once a second.
    double now = platform_time();
    if (B.watch && B.watch->i && now >= B.next_poll) {
        B.next_poll = now + 1.0;
        i64 sig = files_signature();
        if (sig != B.mtime_sig) {
            log_info("ballistics: data changed, reloading");
            bal_world_load();
        }
    }
}

// ------------------------------------------------------------------ console

static void cmd_reload(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    bal_world_load();
}

static void cmd_info(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    const BalData *d = bal_data();
    for (int i = 0; i < d->cart_count; i++) {
        const BalCartridge *c = &d->cart[i];
        log_info("  %-12s %5.1f g  mv %4.0f m/s (%3.0f mm)  %s %.3f  E %5.0f J  Sg %.2f  tracer 1/%d", c->id, c->mass * 1000.0f, c->mv_ref,
                 c->ref_barrel * 1000.0f, c->curve == BAL_CURVE_G7 ? "G7" : (c->curve == BAL_CURVE_GS ? "GS" : "G1"), c->bc, c->e_ref, c->spin_sg,
                 c->tracer.every);
    }
    for (int i = 0; i < d->dev_count; i++)
        log_info("  device %-14s %-10s secondary x%.2f vents x%.2f report %+.0f dB", d->dev[i].id, bal_device_kind_name(d->dev[i].kind),
                 d->dev[i].mul[BFC_SECONDARY], d->dev[i].mul[BFC_VENTS], d->dev[i].report_db);
    log_info("ballistics: %d live, %d steps / %d rays last tick, %.3f ms (worst %.3f ms)", B.stats.live, B.stats.steps, B.stats.rays,
             B.stats.tick_ms, B.stats.worst_ms);
}

// ballistics_bench <n> [cartridge]: n host bullets from above the player in a cone of
// directions; the per-tick cost is logged by bal_info / the next ticks.
static void cmd_bench(int argc, const char **argv) {
    World *w = game_world();
    if (!w || !w->authority) { log_info("ballistics_bench: host only"); return; }
    int n = argc > 1 ? atoi(argv[1]) : 500;
    int ci = bal_cartridge_find(bal_data(), argc > 2 ? argv[2] : "556_m855");
    if (ci < 0) ci = 0;
    Player *p = game_local_player();
    vec3 o = p ? v3_add(p->sim.move.pos, v3(0, 1.6f, 0)) : v3(0, 2, 0);
    float yaw = p ? p->sim.move.yaw : 0.0f;
    Rng rng = rng_make(1234);
    for (int i = 0; i < n; i++) {
        BalLive *l = alloc_bullet();
        memset(l, 0, sizeof(*l));
        float a = yaw + rng_range(&rng, -0.6f, 0.6f), pitch = rng_range(&rng, -0.02f, 0.05f);
        vec3 d = dir_from_angles(a, pitch);
        const BalCartridge *c = &B.data.cart[ci];
        bal_launch(&l->b, c, ci, o, d, c->mv_ref, (u32)i + 1u, 0, false);
        l->role = BAL_ROLE_REPLAY;   // no damage / events: pure cost
        l->slot = (u8)(p ? (int)(p - w->players) : 0);
        l->fire_tick = w->tick;
        l->launch = o;
        l->trail[0] = o;
        l->trail_count = 1;
    }
    B.stats.worst_ms = 0;
    log_info("ballistics_bench: %d bullets launched (%s)", n, B.data.cart[ci].id);
}

// bal_range [pen]: the ballistics range firing points (dev room, east, docs/BALLISTICS.md).
static void cmd_range(int argc, const char **argv) {
    bool pen = argc > 1 && !strcmp(argv[1], "pen");
    cmd_execute_line(pen ? "setpos 32 0.05 -56 -90 0" : "setpos 32 0.05 -40 -90 0");
}

void bal_world_register(void) {
    cmd_register("bal_range", cmd_range, "bal_range [pen]: go to the ballistics range firing point (dev room; pen = the penetration stacks)");
    B.enable = cvar_reg_bool("bal_enable", true, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "projectile ballistics: guns with a cartridge fire simulated bullets (drop, drag, wind, penetration); 0 = instant hitscan");
    B.drag_model = cvar_reg_int("bal_drag_model", -1, -1, 1, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "bullet drag: -1 auto (G1/G7 tables), 0 exponential approximation, 1 G1/G7 drag tables (host, replicated)");
    B.wind_cv = cvar_reg_float("bal_wind", -1.0f, -1.0f, 40.0f, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "steady wind for bullets in m/s (-1 = the weather's wind with gusts; host, replicated)");
    B.wind_dir_cv = cvar_reg_float("bal_wind_dir", 90.0f, 0.0f, 360.0f, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "bal_wind direction: degrees the wind blows towards (0 = -Z, 90 = +X)");
    B.spin = cvar_reg_bool("bal_spin_drift", true, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "spin drift of rifled bullets (Litz, tiny, long range only)");
    B.tracers_only = cvar_reg_bool("fire_tracers_only", false, CVAR_SERVER | CVAR_CHEAT | CVAR_LOCKED | CVAR_RULE, "every round is a tracer (ballistics range / debugging)");
    B.debug = cvar_reg_int("ballistics_debug", 0, 0, 2, CVAR_LOCKED, "draw bullet paths (red supersonic, orange transonic, blue subsonic), penetration points, crack timing; 2 = also log every hit");
    B.watch = cvar_reg_bool("bal_watch", true, CVAR_LOCKED, "reload assets/ballistics and the attachment muzzle devices when the files change");
    B.ignition = cvar_reg_bool("bal_tracer_ignition", true, CVAR_LOCKED, "tracers: dim igniter for the first tens of metres (0 = arcade: bright from the muzzle)");
    cvar_reg_float("bal_tracer_scale_dist", 6.0f, 0.5f, 1000.0f, CVAR_LOCKED, "tracers: width scale 1 at this camera distance in m, growing linearly with distance (bal_tracer_scale_min .. max)");
    cvar_reg_float("bal_tracer_scale_min", 1.0f, 0.1f, 100.0f, CVAR_LOCKED, "tracers: smallest width scale (near the camera; scale 1 = 3 cm wide)");
    cvar_reg_float("bal_tracer_scale_max", 30.0f, 0.1f, 200.0f, CVAR_LOCKED, "tracers: largest width scale (far from the camera; past it a tracer shrinks on screen with distance)");
    B.crack_radius = cvar_reg_float("bal_crack_radius", 120.0f, 0.0f, 1000.0f, CVAR_LOCKED, "supersonic bullets passing within this many metres of you make a crack");
    B.shutter = cvar_reg_float("shutter_preview", 0.0f, 0.0f, 0.25f, CVAR_LOCKED, "tracer streak shutter time in s (0 = the frame time): streak length = speed x shutter");
    cmd_register("bal_reload", cmd_reload, "bal_reload: reload the cartridges, materials and muzzle devices");
    cmd_register("bal_info", cmd_info, "bal_info: cartridges, muzzle devices and the bullet budget of the last tick");
    cmd_register("ballistics_bench", cmd_bench, "ballistics_bench [n] [cartridge]: launch n bullets (host) to measure the per-tick cost (bal_info)");
}
