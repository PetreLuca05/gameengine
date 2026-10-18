// Unit tests for the environment model and chain (src/audio/env, docs/AUDIO_PROCEDURAL.md "Tests"):
// ISO 9613-1 air absorption, the distance law, directivity, transmission loss against the material
// table, diffraction, the room estimate in a box and in the dev room's reverb corridor (rebuilt from
// data/levels/devroom.json, within 20 % of its real size), image-source taps, the cliff echo delay at
// the audio range pad, propagation delay = d / c, occlusion through a wall vs around it, the chain's
// band gains, doppler, a gain step without zipper noise, and the FDN's RT60.
#include "audio/env/env_model.h"
#include "audio/env/env_world.h"
#include "audio/env/env_chain.h"
#include "audio/synth/syn_material.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_checks;

#define CHECK(cond, ...)                                        \
    do {                                                        \
        g_checks++;                                             \
        if (!(cond)) {                                          \
            g_fail++;                                           \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);         \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
        }                                                       \
    } while (0)

#define SR 48000.0f

// ------------------------------------------------------------------ box world (mock physics)

typedef struct Box { vec3 mn, mx; int material; } Box;
static Box g_boxes[512];
static int g_nboxes;
static bool g_ground = true;

static void add_box(vec3 mn, vec3 mx, int mat) {
    if (g_nboxes < 512) g_boxes[g_nboxes++] = (Box){ mn, mx, mat };
}

// Ray against solid boxes and the ground plane y = 0. Like Jolt's convex casts, a ray starting inside
// a solid hits it at distance 0.
static bool mock_ray(vec3 o, vec3 dl, EnvRayHit *h, void *user) {
    (void)user;
    float len = v3_len(dl);
    if (len < 1e-6f) return false;
    vec3 d = v3_scale(dl, 1.0f / len);
    float best = len + 1;
    int mat = 0;
    for (int i = 0; i < g_nboxes; i++) {
        const Box *b = &g_boxes[i];
        float t0 = -1e30f, t1 = 1e30f;
        const float oo[3] = { o.x, o.y, o.z }, dd[3] = { d.x, d.y, d.z };
        const float mn[3] = { b->mn.x, b->mn.y, b->mn.z }, mx[3] = { b->mx.x, b->mx.y, b->mx.z };
        bool miss = false;
        for (int a = 0; a < 3 && !miss; a++) {
            if (fabsf(dd[a]) < 1e-9f) {
                if (oo[a] < mn[a] || oo[a] > mx[a]) miss = true;
                continue;
            }
            float ta = (mn[a] - oo[a]) / dd[a], tb = (mx[a] - oo[a]) / dd[a];
            if (ta > tb) { float t = ta; ta = tb; tb = t; }
            if (ta > t0) t0 = ta;
            if (tb < t1) t1 = tb;
            if (t0 > t1) miss = true;
        }
        if (miss || t1 < 0) continue;
        float t = t0 > 0 ? t0 : 0.0f;
        if (t < best) { best = t; mat = b->material; }
    }
    if (g_ground && d.y < -1e-6f && o.y >= 0) {
        float t = -o.y / d.y;
        if (t < best) { best = t; mat = SYN_MAT_CONCRETE; }
    }
    if (best > len) return false;
    h->hit = true;
    h->dist = best;
    h->point = v3_madd(o, d, best);
    h->material = mat;
    return true;
}

static void world_reset(void) { g_nboxes = 0; g_ground = true; }

// A closed room (inside mn..mx) with walls of thickness t.
static void add_room(vec3 mn, vec3 mx, float t, int mat) {
    add_box(v3(mn.x - t, mn.y - t, mn.z - t), v3(mx.x + t, mn.y, mx.z + t), mat);   // floor
    add_box(v3(mn.x - t, mx.y, mn.z - t), v3(mx.x + t, mx.y + t, mx.z + t), mat);   // ceiling
    add_box(v3(mn.x - t, mn.y, mn.z - t), v3(mn.x, mx.y, mx.z + t), mat);
    add_box(v3(mx.x, mn.y, mn.z - t), v3(mx.x + t, mx.y, mx.z + t), mat);
    add_box(v3(mn.x, mn.y, mn.z - t), v3(mx.x, mx.y, mn.z), mat);
    add_box(v3(mn.x, mn.y, mx.z), v3(mx.x, mx.y, mx.z + t), mat);
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) n = 0;
    b[n] = 0;
    fclose(f);
    return b;
}

static int mat_from_name(const char *n) {
    int m = syn_material_find(n);
    return m < 0 ? SYN_MAT_CONCRETE : m;
}

// The dev room's box brushes (yaw 0 / 90 only) as the mock world; out: the boxes tagged `tag`.
static bool load_devroom(const char *tag, vec3 *tag_mn, vec3 *tag_mx) {
    char *txt = read_file(DG_SOURCE_DIR "/data/levels/devroom.json");
    cJSON *root = txt ? cJSON_Parse(txt) : NULL;
    free(txt);
    if (!root) return false;
    world_reset();
    *tag_mn = v3(1e9f, 1e9f, 1e9f);
    *tag_mx = v3(-1e9f, -1e9f, -1e9f);
    bool found = false;
    const cJSON *b;
    cJSON_ArrayForEach(b, cJSON_GetObjectItemCaseSensitive(root, "brushes")) {
        const cJSON *ty = cJSON_GetObjectItemCaseSensitive(b, "type"), *pos = cJSON_GetObjectItemCaseSensitive(b, "pos"), *sz = cJSON_GetObjectItemCaseSensitive(b, "size");
        if (!cJSON_IsString(ty) || strcmp(ty->valuestring, "box") || cJSON_GetArraySize(pos) != 3 || cJSON_GetArraySize(sz) != 3) continue;
        const cJSON *col = cJSON_GetObjectItemCaseSensitive(b, "collide");
        if (cJSON_IsFalse(col)) continue;
        const cJSON *yaw = cJSON_GetObjectItemCaseSensitive(b, "yaw");
        float y = cJSON_IsNumber(yaw) ? (float)yaw->valuedouble : 0.0f;
        float sx = (float)cJSON_GetArrayItem(sz, 0)->valuedouble, sy = (float)cJSON_GetArrayItem(sz, 1)->valuedouble, sz2 = (float)cJSON_GetArrayItem(sz, 2)->valuedouble;
        if (fabsf(fmodf(fabsf(y), 180.0f) - 90.0f) < 1.0f) { float t = sx; sx = sz2; sz2 = t; }
        else if (fmodf(fabsf(y), 90.0f) > 1.0f) continue;   // rotated boxes: not in the mock
        vec3 p = v3((float)cJSON_GetArrayItem(pos, 0)->valuedouble, (float)cJSON_GetArrayItem(pos, 1)->valuedouble, (float)cJSON_GetArrayItem(pos, 2)->valuedouble);
        vec3 mn = v3(p.x - sx * 0.5f, p.y, p.z - sz2 * 0.5f), mx = v3(p.x + sx * 0.5f, p.y + sy, p.z + sz2 * 0.5f);
        const cJSON *m = cJSON_GetObjectItemCaseSensitive(b, "material");
        add_box(mn, mx, mat_from_name(cJSON_IsString(m) ? m->valuestring : "concrete"));
        if (tag && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(b, tag))) {
            *tag_mn = v3_min(*tag_mn, mn);
            *tag_mx = v3_max(*tag_mx, mx);
            found = true;
        }
    }
    cJSON_Delete(root);
    return found;
}

static void probe(vec3 o, EnvRoom *r) {
    EnvProbeHit h[ENV_PROBE_RAYS];
    for (int i = 0; i < ENV_PROBE_RAYS; i++) {
        EnvRayHit rh;
        memset(&rh, 0, sizeof(rh));
        h[i].hit = mock_ray(o, v3_scale(env_probe_dir[i], ENV_PROBE_RANGE), &rh, NULL);
        h[i].dist = h[i].hit ? rh.dist : ENV_PROBE_RANGE;
        h[i].material = rh.material;
    }
    env_room_estimate(o, h, ENV_PROBE_RANGE, r);
}

static void set_listener(vec3 pos) {
    EnvListener l;
    memset(&l, 0, sizeof(l));
    l.pos = pos;
    l.fwd = v3(0, 0, -1);
    l.right = v3(1, 0, 0);
    l.up = v3(0, 1, 0);
    l.health = 1;
    l.weather.temp_c = 20;
    l.weather.humidity = 50;
    l.weather.exposure = 1;
    l.sound_speed = 343.0f;
    env_world_set_listener(&l);
}

// ------------------------------------------------------------------ model

static void test_air(void) {
    // ISO 9613-1 table, 20 C, 50 % RH, dB / km.
    const float hz[7] = { 125, 250, 500, 1000, 2000, 4000, 8000 };
    const float ref[7] = { 0.44f, 1.31f, 2.73f, 4.66f, 9.86f, 29.7f, 105.0f };
    float worst = 0;
    for (int i = 0; i < 7; i++) worst = fmaxf(worst, fabsf(env_air_alpha(hz[i], 20, 50) * 1000.0f / ref[i] - 1.0f));
    CHECK(worst < 0.08f, "air absorption matches ISO 9613-1 (worst %.1f %%)", worst * 100.0f);
    CHECK(env_air_alpha(4000, 20, 20) > env_air_alpha(4000, 20, 80), "dry air absorbs 4 kHz more than humid air");
    float band[ENV_BANDS], lp_near, lp_far;
    env_air(10, 20, 50, band, &lp_near);
    env_air(800, 20, 50, band, &lp_far);
    CHECK(lp_near >= 19999.0f && lp_far < 8000.0f && lp_far > 3000.0f, "top cut falls with distance (10 m %.0f Hz, 800 m %.0f Hz)", lp_near, lp_far);
    CHECK(band[2] < band[1] && band[1] < band[0] && band[0] <= 0, "high band loses most over 800 m (%.1f %.1f %.1f dB)", band[0], band[1], band[2]);
    CHECK(fabsf(env_sound_speed(20.0f) - 343.2f) < 0.3f, "speed of sound at 20 C %.1f", env_sound_speed(20.0f));
    // Distance law (miniaudio's inverse model), directivity, ground.
    CHECK(fabsf(env_distance_gain(6, 6, 2500, 0.6f) - 1.0f) < 1e-6f && fabsf(env_distance_gain(66, 6, 2500, 0.6f) - 6.0f / 42.0f) < 1e-5f, "inverse distance law");
    float g0, lp0, g90, lp90, g180, lp180;
    env_directivity(1, 3, -6, 4000, &g0, &lp0);
    env_directivity(0, 3, -6, 4000, &g90, &lp90);
    env_directivity(-1, 3, -6, 4000, &g180, &lp180);
    CHECK(g0 == 3 && g90 == 0 && g180 == -6, "directivity gain front / side / rear (%.1f %.1f %.1f)", g0, g90, g180);
    CHECK(lp0 >= 19999.0f && fabsf(lp180 - 4000.0f) < 1.0f && lp90 > 19000.0f, "off-axis darker behind (%.0f Hz)", lp180);
    CHECK(env_ground_db(10, 1) == 0 && env_ground_db(200, 1) < -6.0f, "ground effect grows with distance");
    EnvWeather w = { 15, 60, 1, 12, 0, 1 };
    float floor = env_noise_floor(&w);
    CHECK(env_masking_gain(1.0f, floor) > 0.99f && env_masking_gain(floor * 0.3f, floor) < 0.35f, "rain masks quiet sounds, not loud ones");
}

static void test_walls(void) {
    EnvWall w = { SYN_MAT_CONCRETE, 0.2f };
    float db[ENV_BANDS];
    env_transmission(&w, 1, db);
    const SynMaterial *m = syn_material(SYN_MAT_CONCRETE);
    bool table = true;
    for (int b = 0; b < ENV_BANDS; b++) {
        float want = -fminf(m->tl_base[b] + m->tl_per_cm[b] * 20.0f, m->tl_max);
        table = table && fabsf(db[b] - want) < 1e-4f;
    }
    CHECK(table, "transmission loss matches the material table (%.1f / %.1f / %.1f dB)", db[0], db[1], db[2]);
    CHECK(db[2] < db[1] && db[1] < db[0], "walls take the highs most");
    EnvWall two[2] = { { SYN_MAT_PLASTER, 0.02f }, { SYN_MAT_PLASTER, 0.02f } };
    float d2[ENV_BANDS], d1[ENV_BANDS];
    env_transmission(two, 2, d2);
    env_transmission(two, 1, d1);
    CHECK(fabsf(d2[1] - 2 * d1[1]) < 1e-3f, "walls add up (%.1f vs 2 x %.1f)", d2[1], d1[1]);
    CHECK(d1[1] > db[1] + 10, "a drywall passes far more than concrete (%.1f vs %.1f)", d1[1], db[1]);
    EnvWall glass = { SYN_MAT_GLASS, 0.004f };
    float dg[ENV_BANDS];
    env_transmission(&glass, 1, dg);
    CHECK(dg[1] < -15 && dg[1] > -30, "a window pane %.1f dB mid", dg[1]);
    float df0[ENV_BANDS], df1[ENV_BANDS];
    env_diffraction(0, df0);
    env_diffraction(2, df1);
    CHECK(fabsf(df0[0]) < 0.01f && df1[2] < df1[1] && df1[1] < df1[0] && df1[2] > -25.01f, "diffraction: 0 dB grazing, highs lose most (%.1f %.1f %.1f)", df1[0], df1[1], df1[2]);
}

static void test_rooms(void) {
    world_reset();
    add_room(v3(0, 0, 0), v3(8, 3, 5), 0.3f, SYN_MAT_PLASTER);
    EnvRoom r;
    probe(v3(3, 1.6f, 2), &r);
    CHECK(r.indoor && r.enclosed > 0.99f, "closed room is indoor");
    CHECK(fabsf(r.dims.x - 8) < 0.05f && fabsf(r.dims.y - 3) < 0.05f && fabsf(r.dims.z - 5) < 0.05f, "box room dims %.2f %.2f %.2f", r.dims.x, r.dims.y, r.dims.z);
    CHECK(r.rt60[1] > 0.3f && r.rt60[1] < 4.0f, "RT60 %.2f s", r.rt60[1]);
    // Image sources: the floor reflection of a source at height 1.6 heard at 1.6, 4 m apart.
    EnvTap taps[ENV_MAX_TAPS];
    int n = env_room_taps(&r, v3(1, 1.6f, 2), v3(5, 1.6f, 2), v3(1, 0, 0), 343.0f, taps, ENV_MAX_TAPS);
    CHECK(n == 6, "six first-order taps (%d)", n);
    float floor_extra = (sqrtf(16.0f + 3.2f * 3.2f) - 4.0f) / 343.0f;
    bool found = false;
    for (int i = 0; i < n; i++) found |= fabsf(taps[i].delay_s - floor_extra) < 1e-5f;
    CHECK(found, "floor reflection at +%.2f ms", floor_extra * 1000.0f);
    // An absorbent room rings shorter than a hard one.
    world_reset();
    add_room(v3(0, 0, 0), v3(8, 3, 5), 0.3f, SYN_MAT_CLOTH);
    EnvRoom soft;
    probe(v3(3, 1.6f, 2), &soft);
    world_reset();
    add_room(v3(0, 0, 0), v3(8, 3, 5), 0.3f, SYN_MAT_TILE);
    EnvRoom hard;
    probe(v3(3, 1.6f, 2), &hard);
    CHECK(hard.rt60[1] > 3 * soft.rt60[1], "tiled room rings longer than a padded one (%.2f vs %.2f s)", hard.rt60[1], soft.rt60[1]);
    // Open field: outdoor, nothing encloses.
    world_reset();
    probe(v3(0, 1.6f, 0), &r);
    CHECK(!r.indoor && r.enclosed < 0.3f, "open field is outdoor (enclosed %.2f)", r.enclosed);
    // The dev room's reverb corridor, from the level file: within 20 % of its real inside.
    vec3 mn, mx;
    bool ok = load_devroom("_audio_corridor", &mn, &mx);
    CHECK(ok, "devroom.json has the audio range corridor");
    if (ok) {
        // Inside = the tagged boxes' extent less the 0.3 m walls (and the ceiling), floor at 0.
        vec3 in_mn = v3(mn.x + 0.3f, 0.0f, mn.z + 0.3f), in_mx = v3(mx.x - 0.3f, 3.0f, mx.z - 0.3f);
        vec3 real = v3_sub(in_mx, in_mn);
        probe(v3(0.5f * (in_mn.x + in_mx.x), 1.6f, 0.5f * (in_mn.z + in_mx.z)), &r);
        float ex = fabsf(r.dims.x / real.x - 1), ey = fabsf(r.dims.y / real.y - 1), ez = fabsf(r.dims.z / real.z - 1);
        CHECK(r.indoor, "corridor is indoor");
        CHECK(ex < 0.2f && ey < 0.2f && ez < 0.2f, "corridor estimate %.1f x %.1f x %.1f m vs %.1f x %.1f x %.1f m", r.dims.x, r.dims.y, r.dims.z, real.x, real.y, real.z);
    }
}

static void test_echoes(void) {
    // A cliff face 150 m away: the echo arrives 2 x 150 / c after the direct sound at the source.
    world_reset();
    add_box(v3(150, 0, -200), v3(160, 60, 200), SYN_MAT_CONCRETE);
    EnvEchoRay rays[ENV_ECHO_RAYS];
    vec3 o = v3(0, 1.5f, 0);   // the scan origin: 1.5 m above the source
    for (int i = 0; i < ENV_ECHO_RAYS; i++) {
        float a = 6.2831853f * (float)i / (float)ENV_ECHO_RAYS;
        rays[i].dir = v3_norm(v3(cosf(a), 0.0524f, sinf(a)));
        EnvRayHit h;
        memset(&h, 0, sizeof(h));
        rays[i].hit = mock_ray(o, v3_scale(rays[i].dir, ENV_ECHO_RANGE), &h, NULL);
        rays[i].dist = h.dist;
        rays[i].material = h.material;
    }
    EnvEcho e[6];
    int n = env_outdoor_echoes(o, v3(0, 1.6f, 1), rays, ENV_ECHO_RAYS, 343.0f, 0, 20, 50, e, 6);
    float want = (2.0f * 150.0f - 1.0f) / 343.0f;
    CHECK(n >= 1, "the cliff echoes (%d)", n);
    if (n) CHECK(fabsf(e[0].extra_s - want) < 0.03f * want, "cliff echo +%.3f s (2 d / c = %.3f s)", e[0].extra_s, want);
    if (n) CHECK(e[0].lp_hz < 12000.0f && e[0].gain < 0.1f && e[0].gain > 1e-3f, "the echo is quieter and darker (gain %.3f, lp %.0f Hz)", e[0].gain, e[0].lp_hz);
    // Snow on the ground and the face damps it.
    EnvEcho es[6];
    env_outdoor_echoes(o, v3(0, 1.6f, 1), rays, ENV_ECHO_RAYS, 343.0f, 1.0f, 20, 50, es, 6);
    if (n) CHECK(es[0].gain < 0.7f * e[0].gain, "snow damps the echo");
    // The open field: nothing comes back.
    world_reset();
    for (int i = 0; i < ENV_ECHO_RAYS; i++) {
        EnvRayHit h;
        memset(&h, 0, sizeof(h));
        rays[i].hit = mock_ray(o, v3_scale(rays[i].dir, ENV_ECHO_RANGE), &h, NULL);
    }
    CHECK(env_outdoor_echoes(o, v3(0, 1.6f, 1), rays, ENV_ECHO_RAYS, 343.0f, 0, 20, 50, e, 6) == 0, "open field: no discrete echoes");
    // The dev room's audio range pad: the cliff 145 m west, through the env_world scan.
    vec3 mn, mx;
    if (load_devroom("_audio_cliff", &mn, &mx)) {
        EnvWorldQuery q = { mock_ray, NULL, NULL };
        env_world_init();
        env_world_set_query(&q);
        vec3 pad = v3(-230, 0, -230);
        set_listener(v3(pad.x, 1.6f, pad.z));
        int ne = env_world_echoes(pad, e, 6);
        // The face nearest the pad.
        float d = fabsf(pad.x - mn.x) < fabsf(pad.x - mx.x) ? fabsf(pad.x - mn.x) : fabsf(pad.x - mx.x);
        float wantp = 2.0f * d / 343.0f;
        bool match = false;
        for (int i = 0; i < ne; i++) match |= fabsf(e[i].extra_s - wantp) < 0.03f * wantp;
        CHECK(match, "audio range pad: an echo at 2 x %.0f m / c = %.3f s (%d echoes, first +%.3f s)", d, wantp, ne, ne ? e[0].extra_s : 0.0f);
    } else CHECK(false, "devroom.json has the audio range cliff");
}

static void test_world_voices(void) {
    EnvWorldQuery q = { mock_ray, NULL, NULL };
    env_world_init();
    env_world_set_query(&q);
    world_reset();
    set_listener(v3(0, 1.6f, 0));
    env_world_begin_frame(0.016f);
    EnvSource s;
    memset(&s, 0, sizeof(s));
    s.pos = v3(0, 1.6f, -100);
    s.volume = 1;
    s.min_dist = 1, s.max_dist = 1000, s.rolloff = 1;
    s.rear_lp = 20000;
    s.reverb = 1;
    s.spatial = true;
    EnvVoiceState v;
    env_world_voice_start(&v, &s);
    EnvParams p;
    float delay = env_world_voice_eval(&v, true, &p);
    CHECK(fabsf(delay - 100.0f / 343.0f) < 1e-5f, "propagation delay = d / c (%.4f s)", delay);
    CHECK(v.info.walls == 0 && v.occl_db[0] == 0 && v.occl_db[1] == 0 && v.occl_db[2] == 0, "clear path: no occlusion");
    CHECK(fabsf(p.pan) < 0.01f && p.front > 0.99f, "ahead: centred");
    // A wide concrete wall (no way around) between them: transmission as the table says.
    add_box(v3(-200, 0, -50.1f), v3(200, 40, -49.9f), SYN_MAT_CONCRETE);
    env_world_voice_eval(&v, true, &p);
    EnvWall w = { SYN_MAT_CONCRETE, 0.2f };
    float tl[ENV_BANDS];
    env_transmission(&w, 1, tl);
    CHECK(v.info.walls == 1 && fabsf(v.info.wall_thick[0] - 0.2f) < 0.01f, "one wall, %.3f m thick", v.info.wall_thick[0]);
    bool match = true;
    for (int b = 0; b < ENV_BANDS; b++) match = match && fabsf(v.occl_db[b] - tl[b]) < 0.01f;
    CHECK(match && !v.info.diffracted, "occlusion bands match the material table (%.1f / %.1f / %.1f dB)", v.occl_db[0], v.occl_db[1], v.occl_db[2]);
    // A narrow wall: the sound bends around it, much less loss than through it.
    world_reset();
    add_box(v3(-0.5f, 0, -50.1f), v3(0.5f, 3, -49.9f), SYN_MAT_CONCRETE);
    env_world_voice_eval(&v, true, &p);
    CHECK(v.info.diffracted && v.occl_db[1] > tl[1] + 15.0f, "around a narrow wall: diffraction (%.1f dB vs %.1f dB through)", v.occl_db[1], tl[1]);
    // Same room: early reflections and the room send.
    world_reset();
    add_room(v3(-5, 0, -110), v3(5, 3, 5), 0.3f, SYN_MAT_CONCRETE);
    set_listener(v3(0, 1.6f, 0));
    env_world_begin_frame(1.0f);
    s.pos = v3(0, 1.6f, -10);
    env_world_voice_start(&v, &s);
    env_world_voice_eval(&v, true, &p);
    CHECK(v.info.same_room && p.ntaps >= 4 && p.send[0] > 0, "same room: %d taps, room send %.2f", p.ntaps, p.send[0]);
    EnvReverbParams rv[ENV_ZONES];
    env_world_reverbs(rv);
    CHECK(rv[0].wet > 0.05f && rv[0].rt60_low > 0.3f, "listener room reverb (RT60 %.2f s, wet %.2f)", rv[0].rt60_low, rv[0].wet);
}

// ------------------------------------------------------------------ chain DSP

static float sine_gain(EnvChain *c, float hz) {
    static float in[48000], l[48000], r[48000];
    for (int i = 0; i < 48000; i++) in[i] = sinf(6.2831853f * hz * (float)i / SR);
    memset(l, 0, sizeof(l));
    memset(r, 0, sizeof(r));
    for (int off = 0; off < 48000; off += SYN_BLOCK) env_chain_process(c, in + off, SYN_BLOCK, l + off, r + off, NULL);
    float pk = 0;
    for (int i = 24000; i < 48000; i++) pk = fmaxf(pk, fabsf(l[i]));
    return 20.0f * log10f(pk / 0.70710678f);   // centre pan: 0.707 per ear
}

static void test_chain(void) {
    static EnvChain c;
    EnvParams p;
    env_params_neutral(&p);
    p.spread = 0;
    env_chain_init(&c, SR, NULL, 0, &p);
    float g = sine_gain(&c, 1000);
    CHECK(fabsf(g) < 0.1f, "neutral chain is transparent (%.2f dB)", g);
    p.band_db[0] = -6, p.band_db[1] = -12, p.band_db[2] = -24;
    env_chain_init(&c, SR, NULL, 0, &p);
    float lo = sine_gain(&c, 40), hi = sine_gain(&c, 14000), mid = sine_gain(&c, 700);
    // One-pole crossovers are gentle: the high band leaks some mid at 14 kHz.
    CHECK(fabsf(lo + 6) < 1.5f && fabsf(hi + 24) < 3.5f, "band gains at the extremes (%.1f dB low, %.1f dB high)", lo, hi);
    CHECK(mid < lo - 3 && mid > hi + 5, "mid band in between (%.1f dB)", mid);
    // Pan: hard right silences the left ear's direct path except the ITD-delayed, shadowed far ear.
    env_params_neutral(&p);
    p.spread = 0, p.pan = 1.0f;
    env_chain_init(&c, SR, NULL, 0, &p);
    static float in[9600], l[9600], r[9600];
    for (int i = 0; i < 9600; i++) in[i] = sinf(6.2831853f * 500.0f * (float)i / SR);
    memset(l, 0, sizeof(l));
    memset(r, 0, sizeof(r));
    for (int off = 0; off < 9600; off += SYN_BLOCK) env_chain_process(&c, in + off, SYN_BLOCK, l + off, r + off, NULL);
    float el = 0, er = 0;
    for (int i = 4800; i < 9600; i++) { el += l[i] * l[i]; er += r[i] * r[i]; }
    CHECK(er > 1000.0f * el, "hard right pans right (L/R %.2g)", el / er);
    // Doppler: the delay shrinking 0.1 s over 1 s raises the pitch ~10 %.
    static float dop[65536];
    env_params_neutral(&p);
    p.spread = 0;
    p.delay_s = 0.5f;
    env_chain_init(&c, SR, dop, 65536, &p);
    static float din[96000], dl[96000], dr[96000];
    for (int i = 0; i < 96000; i++) din[i] = sinf(6.2831853f * 1000.0f * (float)i / SR);
    memset(dl, 0, sizeof(dl));
    memset(dr, 0, sizeof(dr));
    for (int off = 0, b = 0; off < 96000; off += SYN_BLOCK, b++) {
        if (off >= 24000 && off < 72000) {
            p.delay_s = 0.5f - 0.1f * (float)(off - 24000) / 48000.0f;
            env_chain_set(&c, &p);
        }
        env_chain_process(&c, din + off, SYN_BLOCK, dl + off, dr + off, NULL);
    }
    int zc = 0;
    for (int i = 48000; i < 67200; i++) if ((dl[i - 1] < 0) != (dl[i] < 0)) zc++;
    float hz = (float)zc / 2.0f / 0.4f;
    CHECK(hz > 1060.0f && hz < 1140.0f, "approaching source pitches up (%.0f Hz from 1000)", hz);
    // A gain step through the chain is ramped: no click energy above 4 kHz.
    env_params_neutral(&p);
    p.spread = 0, p.gain = 0.1f;
    env_chain_init(&c, SR, NULL, 0, &p);
    static float zl[8192], zr[8192], zin[8192];
    for (int i = 0; i < 8192; i++) zin[i] = sinf(6.2831853f * 500.0f * (float)i / SR);
    memset(zl, 0, sizeof(zl));
    memset(zr, 0, sizeof(zr));
    for (int off = 0; off < 8192; off += SYN_BLOCK) {
        if (off == 4096) { p.gain = 1.0f; env_chain_set(&c, &p); }
        env_chain_process(&c, zin + off, SYN_BLOCK, zl + off, zr + off, NULL);
    }
    // High-passed energy (first difference twice) around the step vs a steady tone of the same level.
    float e_step = 0, e_tone = 0;
    for (int i = 4000; i < 6000; i++) { float d = zl[i] - 2 * zl[i - 1] + zl[i - 2]; e_step += d * d; }
    for (int i = 7000; i < 8000; i++) { float d = zl[i] - 2 * zl[i - 1] + zl[i - 2]; e_tone += d * d; }
    CHECK(e_step < 2.2f * e_tone, "gain step without a click (curvature energy %.3g vs steady %.3g per 1000)", e_step / 2, e_tone);
}

static void test_reverb(void) {
    static EnvReverb r;
    EnvReverbParams p = { 1.0f, 1.0f, 1.0f, 5.0f, 1.0f };
    env_reverb_init(&r, SR, &p);
    static float in[96000], l[96000], rr[96000];
    memset(in, 0, sizeof(in));
    in[0] = 1.0f;
    memset(l, 0, sizeof(l));
    memset(rr, 0, sizeof(rr));
    for (int off = 0; off < 96000; off += SYN_BLOCK) env_reverb_process(&r, in + off, SYN_BLOCK, l + off, rr + off);
    // Energy in 100 ms windows: the slope from 0.1 s to 0.6 s (-60 dB per RT60 = -30 dB per 0.5 s).
    double e1 = 0, e2 = 0;
    for (int i = 4800; i < 9600; i++) e1 += (double)l[i] * l[i] + (double)rr[i] * rr[i];
    for (int i = 28800; i < 33600; i++) e2 += (double)l[i] * l[i] + (double)rr[i] * rr[i];
    float db = 10.0f * (float)log10(e2 / e1);
    CHECK(db < -22.0f && db > -38.0f, "FDN decays -30 dB over 0.5 s at RT60 1 s (%.1f dB)", db);
    bool finite = true;
    for (int i = 0; i < 96000; i++) finite = finite && isfinite(l[i]);
    CHECK(finite, "reverb finite");
}

int main(void) {
    test_air();
    test_walls();
    test_rooms();
    test_echoes();
    test_world_voices();
    test_chain();
    test_reverb();
    printf("test_audio_env: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
