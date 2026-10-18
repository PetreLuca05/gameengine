#include "env_world.h"
#include "audio/synth/syn_material.h"
#include <math.h>
#include <string.h>

#define ROOM_CACHE 64
#define SCAN_CACHE 32
#define ROOM_CELL 2.0f
#define SCAN_CELL 8.0f
#define ROOM_TTL 1.5f            // s before a cached probe is cast again
#define SCAN_TTL 4.0f

typedef struct RoomCache { bool used; int cx, cy, cz; float age; EnvRoom room; } RoomCache;
typedef struct ScanCache { bool used; int cx, cz; float age; vec3 origin; EnvEchoRay rays[ENV_ECHO_RAYS]; float urban, trunks; } ScanCache;

static struct {
    EnvWorldQuery q;
    bool have_q;
    EnvListener lis;
    RoomCache rooms[ROOM_CACHE];
    ScanCache scans[SCAN_CACHE];
    const EnvRoom *lis_room;
    EnvRoom remote;
    float remote_score;
    bool remote_valid, remote_seen;
    float forest, urban, env_age;
    vec3 probe_origin, probe_end[ENV_PROBE_RAYS];
    bool probe_hit[ENV_PROBE_RAYS];
    vec3 scan_origin, scan_end[ENV_ECHO_RAYS];
    bool scan_hit[ENV_ECHO_RAYS];
    int budget;
    EnvWorldStats stats;
} W;

void env_world_init(void) {
    memset(&W, 0, sizeof(W));
    W.budget = 6;
    W.lis.fwd = v3(0, 0, -1);
    W.lis.right = v3(1, 0, 0);
    W.lis.up = v3(0, 1, 0);
    W.lis.health = 1.0f;
    W.lis.weather.temp_c = 15.0f;
    W.lis.weather.humidity = 60.0f;
    W.lis.weather.exposure = 1.0f;
    W.lis.sound_speed = ENV_SOUND_SPEED;
}

void env_world_set_query(const EnvWorldQuery *q) {
    if (q) { W.q = *q; W.have_q = q->raycast != NULL; }
    else W.have_q = false;
}
const EnvWorldQuery *env_world_query(void) { return W.have_q ? &W.q : NULL; }
void env_world_set_listener(const EnvListener *l) { W.lis = *l; }
const EnvListener *env_world_listener(void) { return &W.lis; }
void env_world_set_budget(int n) { W.budget = n < 1 ? 1 : n; }
EnvWorldStats env_world_stats(void) { return W.stats; }

static bool cast(vec3 from, vec3 dir_len, EnvRayHit *h) {
    memset(h, 0, sizeof(*h));
    if (!W.have_q) return false;
    W.stats.rays++;
    return W.q.raycast(from, dir_len, h, W.q.user) && h->hit;
}

// ------------------------------------------------------------------ rooms

static const EnvRoom *probe_room(vec3 p, bool fresh, bool debug) {
    int cx = (int)floorf(p.x / ROOM_CELL), cy = (int)floorf(p.y / ROOM_CELL), cz = (int)floorf(p.z / ROOM_CELL);
    RoomCache *slot = NULL, *oldest = &W.rooms[0];
    for (int i = 0; i < ROOM_CACHE; i++) {
        RoomCache *c = &W.rooms[i];
        if (c->used && c->cx == cx && c->cy == cy && c->cz == cz) { slot = c; break; }
        if (!c->used || (oldest->used && c->age > oldest->age)) oldest = c;
    }
    if (slot && !fresh && slot->age < ROOM_TTL) return &slot->room;
    if (!slot) slot = oldest;
    EnvProbeHit hits[ENV_PROBE_RAYS];
    // Probe from the cell centre at ear / muzzle height within it (stable per cell), not the exact point.
    vec3 o = v3((cx + 0.5f) * ROOM_CELL, p.y, (cz + 0.5f) * ROOM_CELL);
    if (fresh) o = p;
    for (int i = 0; i < ENV_PROBE_RAYS; i++) {
        EnvRayHit h;
        hits[i].hit = cast(o, v3_scale(env_probe_dir[i], ENV_PROBE_RANGE), &h);
        hits[i].dist = hits[i].hit ? h.dist : ENV_PROBE_RANGE;
        hits[i].material = h.material;
        if (debug) {
            W.probe_end[i] = v3_madd(o, env_probe_dir[i], hits[i].dist);
            W.probe_hit[i] = hits[i].hit;
        }
    }
    if (debug) W.probe_origin = o;
    W.stats.probes++;
    slot->used = true;
    slot->cx = cx, slot->cy = cy, slot->cz = cz;
    slot->age = 0.0f;
    env_room_estimate(o, hits, ENV_PROBE_RANGE, &slot->room);
    return &slot->room;
}

const EnvRoom *env_world_room(vec3 p, bool fresh) { return probe_room(p, fresh, false); }

int env_world_debug_probe(vec3 *origin, vec3 *ends, bool *hits, int max) {
    *origin = W.probe_origin;
    int n = max < ENV_PROBE_RAYS ? max : ENV_PROBE_RAYS;
    for (int i = 0; i < n; i++) { ends[i] = W.probe_end[i]; hits[i] = W.probe_hit[i]; }
    return n;
}

// ------------------------------------------------------------------ echo scans

static ScanCache *scan_at(vec3 p) {
    int cx = (int)floorf(p.x / SCAN_CELL), cz = (int)floorf(p.z / SCAN_CELL);
    ScanCache *slot = NULL, *oldest = &W.scans[0];
    for (int i = 0; i < SCAN_CACHE; i++) {
        ScanCache *c = &W.scans[i];
        if (c->used && c->cx == cx && c->cz == cz) { slot = c; break; }
        if (!c->used || (oldest->used && c->age > oldest->age)) oldest = c;
    }
    if (slot && slot->age < SCAN_TTL) return slot;
    if (!slot) slot = oldest;
    // Near-horizontal rays from 1.5 m above the source, 3 degrees up (over the ground it stands on,
    // into valley walls, cliffs, facades, tree lines).
    vec3 o = v3(p.x, p.y + 1.5f, p.z);
    int hard = 0, near = 0, wood = 0;
    for (int i = 0; i < ENV_ECHO_RAYS; i++) {
        float a = 6.2831853f * (float)i / (float)ENV_ECHO_RAYS;
        vec3 d = v3_norm(v3(cosf(a), 0.0524f, sinf(a)));
        EnvRayHit h;
        EnvEchoRay *r = &slot->rays[i];
        r->dir = d;
        r->hit = cast(o, v3_scale(d, ENV_ECHO_RANGE), &h);
        r->dist = r->hit ? h.dist : ENV_ECHO_RANGE;
        r->material = h.material;
        if (r->hit && r->dist < 80.0f) {
            near++;
            hard += syn_material(r->material)->reflect > 0.75f;
            wood += r->dist < 40.0f && (r->material == SYN_MAT_WOOD || r->material == SYN_MAT_FOLIAGE);
        }
        W.scan_end[i] = v3_madd(o, d, r->dist);
        W.scan_hit[i] = r->hit;
    }
    W.scan_origin = o;
    W.stats.echo_scans++;
    slot->used = true;
    slot->cx = cx, slot->cz = cz;
    slot->age = 0.0f;
    slot->origin = o;
    // Urban: many hard reflectors close around (facades); a forest line counts as soft.
    slot->urban = syn_clampf(((float)hard / (float)ENV_ECHO_RAYS) * 1.6f * ((float)near / (float)ENV_ECHO_RAYS + 0.3f), 0.0f, 1.0f);
    // Trunks: many wooden hits close around scatter like a stand of trees (a forest without foliage data).
    slot->trunks = syn_clampf((float)wood / (float)ENV_ECHO_RAYS * 1.5f, 0.0f, 1.0f);
    return slot;
}

int env_world_debug_scan(vec3 *origin, vec3 *ends, bool *hits, int max) {
    *origin = W.scan_origin;
    int n = max < ENV_ECHO_RAYS ? max : ENV_ECHO_RAYS;
    for (int i = 0; i < n; i++) { ends[i] = W.scan_end[i]; hits[i] = W.scan_hit[i]; }
    return n;
}

static float humidity(void) { return syn_clampf(W.lis.weather.humidity, 5.0f, 100.0f); }
static float speed(void) { return W.lis.sound_speed > 100.0f ? W.lis.sound_speed : ENV_SOUND_SPEED; }

int env_world_echoes(vec3 src, EnvEcho *out, int max) {
    if (!W.have_q || max <= 0) return 0;
    ScanCache *s = scan_at(src);
    return env_outdoor_echoes(s->origin, W.lis.pos, s->rays, ENV_ECHO_RAYS, speed(), W.lis.weather.snow, W.lis.weather.temp_c, humidity(), out, max);
}

// ------------------------------------------------------------------ frame

void env_world_begin_frame(float dt) {
    for (int i = 0; i < ROOM_CACHE; i++) if (W.rooms[i].used) W.rooms[i].age += dt;
    for (int i = 0; i < SCAN_CACHE; i++) if (W.scans[i].used) W.scans[i].age += dt;
    // The listener's room: re-probed from the exact ear position a few times a second.
    W.env_age += dt;
    if (W.have_q) {
        static float since;
        since += dt;
        if (!W.lis_room || since > 0.25f) {
            W.lis_room = probe_room(W.lis.pos, true, true);
            since = 0.0f;
        }
        if (W.env_age > 0.5f) {
            W.env_age = 0.0f;
            W.forest = W.q.foliage ? syn_clampf(W.q.foliage(W.lis.pos, 25.0f, W.q.user), 0.0f, 1.0f) : 0.0f;
            if (W.lis_room && !W.lis_room->indoor) {
                ScanCache *sc = scan_at(W.lis.pos);
                W.urban = sc->urban;
                W.forest = fmaxf(W.forest, sc->trunks);
            }
        }
    }
    W.remote_seen = false;
    W.remote_score = 0.0f;
}

// ------------------------------------------------------------------ occlusion

static void occlusion(EnvVoiceState *v) {
    vec3 L = W.lis.pos, S = v->src.pos;
    vec3 d = v3_sub(S, L);
    float dist = v3_len(d);
    for (int b = 0; b < ENV_BANDS; b++) v->occl_db[b] = 0.0f;
    v->info.walls = 0;
    v->info.diffracted = false;
    v->occl_frames = 0;
    if (dist < 0.3f || !W.have_q) return;
    W.stats.occl_updates++;
    vec3 dir = v3_scale(d, 1.0f / dist);
    EnvWall walls[ENV_MAX_WALLS];
    int n = 0;
    vec3 from = L, entry0 = L;
    // Walk through up to three walls; each wall's thickness from a back-cast out of it.
    for (; n < ENV_MAX_WALLS;) {
        float remaining = v3_dist(from, S);
        if (remaining < 0.05f) break;
        EnvRayHit h;
        if (!cast(from, v3_scale(dir, remaining), &h)) break;
        if (n == 0) entry0 = h.point;
        float after = remaining - h.dist;
        float probe = after < 1.2f ? after : 1.2f;
        float thick = probe;
        vec3 exit = v3_madd(h.point, dir, probe);
        EnvRayHit b;
        if (probe > 0.02f && cast(exit, v3_scale(dir, -probe), &b)) {
            thick = probe - b.dist;
            exit = b.point;
        }
        if (thick < 0.02f) thick = 0.02f;
        walls[n].material = h.material;
        walls[n].thickness = thick;
        v->info.wall_material[n] = h.material;
        v->info.wall_thick[n] = thick;
        n++;
        from = v3_madd(exit, dir, 0.02f);
    }
    v->info.walls = n;
    if (!n) return;
    float trans[ENV_BANDS];
    env_transmission(walls, n, trans);
    // Diffraction: around the first wall (left / right / over), if both legs are clear.
    vec3 right = v3_cross(dir, v3(0, 1, 0));
    if (v3_len2(right) < 1e-4f) right = v3(1, 0, 0);
    right = v3_norm(right);
    const vec3 offs[3] = { v3_scale(right, 1.6f), v3_scale(right, -1.6f), v3(0, 1.8f, 0) };
    float best = 1e9f;
    for (int k = 0; k < 3; k++) {
        vec3 P = v3_add(entry0, offs[k]);
        EnvRayHit h;
        if (cast(L, v3_sub(P, L), &h)) continue;
        if (cast(P, v3_sub(S, P), &h)) continue;
        float detour = v3_dist(L, P) + v3_dist(P, S) - dist;
        if (detour < best) { best = detour; v->occl_path = P; }
    }
    if (best < 1e8f) {
        float dif[ENV_BANDS];
        env_diffraction(best, dif);
        // Both paths carry energy: power sum of transmission and diffraction.
        for (int b = 0; b < ENV_BANDS; b++) v->occl_db[b] = 10.0f * log10f(powf(10.0f, trans[b] / 10.0f) + powf(10.0f, dif[b] / 10.0f));
        v->info.diffracted = true;
    } else {
        for (int b = 0; b < ENV_BANDS; b++) v->occl_db[b] = trans[b];
    }
}

void env_world_voice_start(EnvVoiceState *v, const EnvSource *s) {
    memset(v, 0, sizeof(*v));
    v->src = *s;
    v->occl_frames = -1;
    if (s->spatial && !s->echo_child) occlusion(v);
}

float env_world_voice_eval(EnvVoiceState *v, bool rays, EnvParams *p) {
    const EnvSource *s = &v->src;
    env_params_neutral(p);
    if (!s->spatial) return 0.0f;
    if (rays && !s->echo_child) occlusion(v);
    else if (v->occl_frames >= 0) v->occl_frames++;
    vec3 L = W.lis.pos, S = s->pos;
    float d = v3_dist(S, L), c = speed();
    float delay = d / c;
    EnvVoiceInfo *in = &v->info;
    in->dist = d;
    in->delay = delay;
    env_pan(L, W.lis.right, W.lis.fwd, S, &p->pan, &p->front);
    p->spread = d < 1.0f ? 1.0f - d : 0.0f;
    if (s->echo_child) {
        // An echo: its path's gain and top cut were set at emit (volume / rear_lp); it arrives from the reflector.
        p->gain = 1.0f;
        p->lp_hz = s->rear_lp > 0 ? s->rear_lp : 20000.0f;
        p->spread = 0.25f;
        p->send[2] = 0.2f * s->reverb;
        in->gain = s->volume;
        in->lp_hz = p->lp_hz;
        return 0.0f;
    }
    float gd = env_distance_gain(d, s->min_dist, s->max_dist, s->rolloff), g = gd;
    float band[ENV_BANDS], lp;
    env_air(d, W.lis.weather.temp_c, humidity(), band, &lp);
    if (v3_len2(s->fwd) > 0.01f && d > 0.3f && (s->front_db != 0 || s->rear_db != 0 || s->rear_lp < 19000.0f)) {
        float cosax = v3_dot(v3_norm(s->fwd), v3_scale(v3_sub(L, S), 1.0f / d));
        float ddb, dlp;
        env_directivity(cosax, s->front_db, s->rear_db, s->rear_lp, &ddb, &dlp);
        g *= syn_db(ddb);
        if (dlp < lp) lp = dlp;
    }
    const EnvRoom *lr = W.lis_room;
    bool lis_in = lr && lr->indoor;
    bool same = lis_in && env_room_contains(lr, S, 0.5f);
    if (!lis_in) band[1] += env_ground_db(d, 0.6f);   // soft ground outdoors
    for (int b = 0; b < ENV_BANDS; b++) {
        band[b] += v->occl_db[b];
        in->occl_db[b] = v->occl_db[b];
    }
    // Masking: the rain / wind floor swallows quiet sounds (not the ambience itself).
    float mask = 1.0f;
    if (!s->ambience) {
        float level = s->volume * g * syn_db(band[1]);
        mask = env_masking_gain(level, env_noise_floor(&W.lis.weather));
    }
    in->mask = mask;
    g *= mask;
    p->gain = g;
    p->lp_hz = lp;
    for (int b = 0; b < ENV_BANDS; b++) p->band_db[b] = band[b];
    if (s->doppler) p->delay_s = delay;
    // Reverb and early reflections. The diffuse field barely falls with distance, so the send undoes
    // the distance law (capped); occlusion still applies (the chain sends after its gain).
    float dif = s->reverb * 0.3f * fminf(1.0f / fmaxf(gd, 1e-3f), 6.0f);
    in->same_room = same;
    in->zone = -1;
    if (same) {
        p->send[0] = dif;
        in->zone = 0;
        int nt = s->max_taps > 0 ? s->max_taps : 4;
        p->ntaps = env_room_taps(lr, S, L, W.lis.right, c, p->tap, nt < ENV_MAX_TAPS ? nt : ENV_MAX_TAPS);
        in->src_indoor = true;
    } else {
        const EnvRoom *sr = W.have_q ? probe_room(S, false, false) : NULL;
        in->src_indoor = sr && sr->indoor;
        if (sr && sr->indoor) {
            p->send[1] = dif;
            in->zone = 1;
            float score = s->volume * g;
            if (score > W.remote_score) { W.remote_score = score; W.remote = *sr; W.remote_seen = true; }
        } else {
            p->send[2] = dif * 0.8f;
            in->zone = 2;
        }
    }
    in->gain = g;
    in->lp_hz = lp;
    for (int b = 0; b < ENV_BANDS; b++) in->band_db[b] = band[b];
    return delay;
}

void env_world_reverbs(EnvReverbParams out[ENV_ZONES]) {
    const EnvRoom *lr = W.lis_room;
    if (lr && lr->indoor) env_room_reverb(lr, &out[0]);
    else memset(&out[0], 0, sizeof(out[0])), out[0].rt60_low = out[0].rt60_high = 0.5f, out[0].size = 1.0f;
    if (W.remote_seen) { env_room_reverb(&W.remote, &out[1]); W.remote_valid = true; }
    else if (!W.remote_valid) memset(&out[1], 0, sizeof(out[1])), out[1].rt60_low = out[1].rt60_high = 0.5f, out[1].size = 1.0f;
    else { env_room_reverb(&W.remote, &out[1]); out[1].wet *= 0.5f; }
    env_outdoor_reverb(W.forest, W.urban, W.lis.weather.snow, &out[2]);
}

void env_world_field(float *forest, float *urban) {
    *forest = W.forest;
    *urban = W.urban;
}
