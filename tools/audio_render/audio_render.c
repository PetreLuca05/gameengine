// Offline renderer (docs/AUDIO_PROCEDURAL.md "Listening"): a recipe through the real mixer,
// environment chain and environment logic, in a scene built from boxes, to a stereo WAV. What the
// game would play, without the game: for listening, A/B, tuning and regression renders.
//
//   audio_render <recipe> [key=value ...] [options] -o out.wav
//     --scene field|valley|forest|urban|corridor|room|wall|indoor_out|anechoic   (default field)
//     --dist m (20)  --az deg (0 = ahead, 90 = right)  --hs m (1.5)  --dur s (3)  --seed n (1)
//     --sample name        the recipe's "@" layers
//     --no-samples         synthesized layers only (no sample layers at all)
//     --repeat n --every s the same event n times (footsteps, bursts), seeds n, n + 1, ...
//     --ramp key=a:b       a loop's input swept from a to b over the duration (rpm, wind)
//     --gain db            output gain (default 0; the mixer's soft clip still applies)
//     --chain off          distance law and pan only (A / B against the environment)
//     --normalize db       scale the file so its peak sits at db dBFS (listening packs)
#include "audio/mixer.h"
#include "audio/synth/syn_material.h"
#include "audio/env/env_world.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#include <io.h>
#else
#include <dirent.h>
#endif

#define SR 48000
#define MAX_SAMPLES 512

// ------------------------------------------------------------------ samples (16-bit PCM WAV)

static char g_sample_name[MAX_SAMPLES][48];
static float *g_sample_data[MAX_SAMPLES];
static int g_sample_frames[MAX_SAMPLES];
static int g_nsamples;
static bool g_no_samples;
static SynSampleBank g_bank = { (const float *const *)g_sample_data, g_sample_frames, 0 };

static float *to_mono(const short *data, int in_frames, int ch, int rate, int *frames);
int stb_vorbis_decode_filename(const char *filename, int *channels, int *sample_rate, short **output);   // third_party/stb/stb_vorbis.c

static float *read_wav(const char *path, int *frames) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = (unsigned char *)malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    fclose(f);
    int ch = 1, rate = 0, bits = 0;
    const unsigned char *data = NULL;
    unsigned dlen = 0;
    for (long p = 12; p + 8 <= n;) {
        unsigned len = (unsigned)b[p + 4] | (unsigned)b[p + 5] << 8 | (unsigned)b[p + 6] << 16 | (unsigned)b[p + 7] << 24;
        if (!memcmp(b + p, "fmt ", 4)) {
            ch = b[p + 10] | b[p + 11] << 8;
            rate = (int)(b[p + 12] | b[p + 13] << 8 | b[p + 14] << 16 | (unsigned)b[p + 15] << 24);
            bits = b[p + 22] | b[p + 23] << 8;
        } else if (!memcmp(b + p, "data", 4)) {
            data = b + p + 8;
            dlen = len;
            if (p + 8 + (long)dlen > n) dlen = (unsigned)(n - p - 8);
        }
        p += 8 + len + (len & 1);
    }
    if (!data || bits != 16 || rate <= 0 || ch < 1) { free(b); return NULL; }
    float *out = to_mono((const short *)data, (int)(dlen / (unsigned)(2 * ch)), ch, rate, frames);
    free(b);
    return out;
}

// Interleaved 16-bit (little endian) at any rate -> mono float at SR (linear interpolation).
static float *to_mono(const short *data, int in_frames, int ch, int rate, int *frames) {
    int out_frames = (int)((double)in_frames * SR / rate);
    float *out = (float *)malloc(sizeof(float) * (size_t)(out_frames > 0 ? out_frames : 1));
    for (int i = 0; i < out_frames; i++) {
        double src = (double)i * rate / SR;
        int i0 = (int)src;
        double fr = src - i0;
        float v[2] = { 0, 0 };
        for (int k = 0; k < 2; k++) {
            int j = i0 + k < in_frames ? i0 + k : in_frames - 1;
            float acc = 0;
            for (int c = 0; c < ch; c++) acc += (float)data[j * ch + c] / 32768.0f;
            v[k] = acc / (float)ch;
        }
        out[i] = (float)(v[0] + fr * (v[1] - v[0]));
    }
    *frames = out_frames;
    return out;
}

static float *read_ogg(const char *path, int *frames) {
    int ch = 0, rate = 0;
    short *pcm = NULL;
    int n = stb_vorbis_decode_filename(path, &ch, &rate, &pcm);
    if (n <= 0 || !pcm) return NULL;
    float *out = to_mono(pcm, n, ch, rate, frames);
    free(pcm);
    return out;
}

static int resolve(const char *name, void *user) {
    (void)user;
    if (g_no_samples) return -1;
    for (int i = 0; i < g_nsamples; i++) if (!strcmp(g_sample_name[i], name)) return i;
    if (g_nsamples >= MAX_SAMPLES) return -1;
    char path[512];
    snprintf(path, sizeof(path), "%s/assets/sounds/%s.wav", DG_SOURCE_DIR, name);
    int frames = 0;
    float *d = read_wav(path, &frames);
    if (!d) {
        snprintf(path, sizeof(path), "%s/assets/sounds/%s.ogg", DG_SOURCE_DIR, name);
        d = read_ogg(path, &frames);
    }
    if (!d) return -1;
    int i = g_nsamples++;
    snprintf(g_sample_name[i], sizeof(g_sample_name[i]), "%s", name);
    g_sample_data[i] = d;
    g_sample_frames[i] = frames;
    g_bank.count = g_nsamples;
    return i;
}

static char *read_text(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = (char *)malloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) n = 0;
    b[n] = 0;
    fclose(f);
    return b;
}

static void load_dir(SynLibrary *lib, const char *sub, bool patches) {
    char dir[512], path[700], err[256];
    snprintf(dir, sizeof(dir), "%s/assets/audio/%s", DG_SOURCE_DIR, sub);
#if defined(_WIN32)
    char pat[600];
    snprintf(pat, sizeof(pat), "%s/*.json", dir);
    struct _finddata_t fd;
    intptr_t h = _findfirst(pat, &fd);
    if (h == -1) return;
    do {
        snprintf(path, sizeof(path), "%s/%s", dir, fd.name);
#else
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l < 6 || strcmp(e->d_name + l - 5, ".json")) continue;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
#endif
        char *t = read_text(path);
        if (t) {
            int r = patches ? syn_lib_add_patch(lib, t, err, sizeof(err)) : syn_lib_add_recipe(lib, t, err, sizeof(err));
            if (r < 0) fprintf(stderr, "audio_render: %s: %s\n", path, err);
            free(t);
        }
#if defined(_WIN32)
    } while (_findnext(h, &fd) == 0);
    _findclose(h);
#else
    }
    closedir(d);
#endif
}

// ------------------------------------------------------------------ scenes (boxes + a ground plane)

typedef struct Box { vec3 mn, mx; int material; } Box;
static Box g_box[1024];
static int g_nbox;
static int g_ground_mat = SYN_MAT_GRASS;
static bool g_ground = true;

static void box(vec3 mn, vec3 mx, int m) { if (g_nbox < 1024) g_box[g_nbox++] = (Box){ mn, mx, m }; }

static bool ray(vec3 o, vec3 dl, EnvRayHit *h, void *user) {
    (void)user;
    float len = v3_len(dl);
    if (len < 1e-6f) return false;
    vec3 d = v3_scale(dl, 1.0f / len);
    float best = len + 1;
    int mat = 0;
    for (int i = 0; i < g_nbox; i++) {
        const float oo[3] = { o.x, o.y, o.z }, dd[3] = { d.x, d.y, d.z };
        const float mn[3] = { g_box[i].mn.x, g_box[i].mn.y, g_box[i].mn.z }, mx[3] = { g_box[i].mx.x, g_box[i].mx.y, g_box[i].mx.z };
        float t0 = -1e30f, t1 = 1e30f;
        bool miss = false;
        for (int a = 0; a < 3 && !miss; a++) {
            if (fabsf(dd[a]) < 1e-9f) { if (oo[a] < mn[a] || oo[a] > mx[a]) miss = true; continue; }
            float ta = (mn[a] - oo[a]) / dd[a], tb = (mx[a] - oo[a]) / dd[a];
            if (ta > tb) { float t = ta; ta = tb; tb = t; }
            if (ta > t0) t0 = ta;
            if (tb < t1) t1 = tb;
            if (t0 > t1) miss = true;
        }
        if (miss || t1 < 0) continue;
        float t = t0 > 0 ? t0 : 0.0f;
        if (t < best) { best = t; mat = g_box[i].material; }
    }
    if (g_ground && d.y < -1e-6f && o.y >= 0) {
        float t = -o.y / d.y;
        if (t < best) { best = t; mat = g_ground_mat; }
    }
    if (best > len) return false;
    h->hit = true;
    h->dist = best;
    h->point = v3_madd(o, d, best);
    h->material = mat;
    return true;
}

static float g_foliage;
static float foliage(vec3 p, float r, void *u) { (void)p; (void)r; (void)u; return g_foliage; }

static void room(vec3 mn, vec3 mx, float t, int m, bool door) {
    box(v3(mn.x - t, mx.y, mn.z - t), v3(mx.x + t, mx.y + t, mx.z + t), m);   // ceiling
    box(v3(mn.x - t, mn.y, mn.z - t), v3(mn.x, mx.y, mx.z + t), m);
    box(v3(mx.x, mn.y, mn.z - t), v3(mx.x + t, mx.y, mx.z + t), m);
    box(v3(mn.x, mn.y, mx.z), v3(mx.x, mx.y, mx.z + t), m);
    if (door) {
        // A 1 m door near the corner of the -z wall.
        box(v3(mn.x, mn.y, mn.z - t), v3(mx.x - 1.6f, mx.y, mn.z), m);
        box(v3(mx.x - 0.6f, mn.y, mn.z - t), v3(mx.x, mx.y, mn.z), m);
        box(v3(mx.x - 1.6f, 2.1f, mn.z - t), v3(mx.x - 0.6f, mx.y, mn.z), m);
    } else box(v3(mn.x, mn.y, mn.z - t), v3(mx.x, mx.y, mn.z), m);
}

// Listener at (0, 1.7, 0) facing -Z; returns the source position for (dist, az, hs).
static bool scene(const char *name, float dist, float az, float hs, vec3 *src, vec3 *lis) {
    g_nbox = 0;
    g_ground = true;
    g_ground_mat = SYN_MAT_GRASS;
    g_foliage = 0;
    *lis = v3(0, 1.7f, 0);
    float a = az * 0.0174533f;
    *src = v3(sinf(a) * dist, hs, -cosf(a) * dist);
    if (!strcmp(name, "anechoic")) { g_ground = false; return true; }
    if (!strcmp(name, "field")) return true;
    if (!strcmp(name, "valley")) {
        // Rock walls 150 m either side, 60 m high, running along the line of sight.
        box(v3(-160, 0, -800), v3(-150, 60, 800), SYN_MAT_CONCRETE);
        box(v3(150, 0, -800), v3(160, 60, 800), SYN_MAT_CONCRETE);
        g_ground_mat = SYN_MAT_DIRT;
        return true;
    }
    if (!strcmp(name, "forest")) {
        // Trunks every ~4 m (jittered) out to 120 m; the listener and the source in a small clearing.
        unsigned s = 12345;
        for (int x = -30; x <= 30; x++)
            for (int z = -30; z <= 30; z++) {
                s = s * 1103515245u + 12345u;
                float jx = ((s >> 8) & 255) / 255.0f - 0.5f, jz = ((s >> 16) & 255) / 255.0f - 0.5f;
                vec3 c = v3((float)x * 4.0f + jx * 2.5f, 0, (float)z * 4.0f + jz * 2.5f);
                if (v3_len(c) < 3.0f || v3_dist(c, v3(src->x, 0, src->z)) < 2.0f) continue;
                if (g_nbox < 1000) box(v3(c.x - 0.2f, 0, c.z - 0.2f), v3(c.x + 0.2f, 14, c.z + 0.2f), SYN_MAT_WOOD);
            }
        g_foliage = 0.8f;
        g_ground_mat = SYN_MAT_LEAVES;
        return true;
    }
    if (!strcmp(name, "urban")) {
        // A street: 12 m facades 10 m apart along the line of sight, side streets every 40 m.
        for (int k = -6; k < 6; k++) {
            float z0 = (float)k * 40.0f + 4.0f, z1 = z0 + 32.0f;
            box(v3(-6, 0, z0), v3(-5, 12, z1), SYN_MAT_BRICK);
            box(v3(5, 0, z0), v3(6, 15, z1), SYN_MAT_CONCRETE);
        }
        g_ground_mat = SYN_MAT_ASPHALT;
        *src = v3(fminf(fmaxf(src->x, -4.0f), 4.0f), hs, src->z);
        return true;
    }
    if (!strcmp(name, "corridor")) {
        room(v3(-1.5f, 0, -24), v3(1.5f, 3, 2), 0.3f, SYN_MAT_CONCRETE, false);
        g_ground_mat = SYN_MAT_CONCRETE;
        *src = v3(0, hs, -fminf(dist, 22.0f));
        return true;
    }
    if (!strcmp(name, "room")) {
        room(v3(-3, 0, -4), v3(3, 3, 1), 0.2f, SYN_MAT_TILE, false);
        g_ground_mat = SYN_MAT_TILE;
        *src = v3(sinf(a) * fminf(dist, 2.5f), hs, -cosf(a) * fminf(dist, 3.5f));
        return true;
    }
    if (!strcmp(name, "wall")) {
        // A wide 20 cm concrete wall halfway: transmission only.
        float zm = src->z * 0.5f;
        box(v3(-200, 0, zm - 0.1f), v3(200, 30, zm + 0.1f), SYN_MAT_CONCRETE);
        return true;
    }
    if (!strcmp(name, "indoor_out")) {
        // The listener in a plaster room with an open door; the source outside.
        room(v3(-3, 0, -3), v3(3, 2.8f, 3), 0.15f, SYN_MAT_PLASTER, true);
        g_ground_mat = SYN_MAT_GRASS;
        *src = v3(sinf(a) * fmaxf(dist, 8.0f), hs, -cosf(a) * fmaxf(dist, 8.0f));
        return true;
    }
    return false;
}

// ------------------------------------------------------------------ voices

typedef struct RVoice { MixHandle h; EnvVoiceState env; bool echo; int recipe; u32 seed; float inputs[RCP_MAX_INPUTS]; int layer_map[RCP_MAX_LAYERS]; int nlayers; } RVoice;
static RVoice g_v[MIX_VOICES];
static int g_nv;
static bool g_chain = true;

static double clock_s(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void source_of(const SynRecipe *r, vec3 pos, float volume, EnvSource *s) {
    memset(s, 0, sizeof(*s));
    s->pos = pos;
    s->fwd = v3_norm(v3_sub(v3(0, 1.7f, 0), pos));   // the source faces the listener (directivity on axis)
    s->volume = volume;
    s->min_dist = r->min_dist, s->max_dist = r->max_dist, s->rolloff = r->rolloff;
    s->front_db = r->front_db, s->rear_db = r->rear_db, s->rear_lp = r->rear_lp;
    s->reverb = r->reverb;
    s->spatial = r->spatial;
    s->doppler = r->doppler;
    s->ambience = r->bus == SYN_BUS_AMBIENCE;
    s->max_taps = 6;
}

static void neutralize(EnvParams *ep) {
    float g = ep->gain, pan = ep->pan, fr = ep->front;
    env_params_neutral(ep);
    ep->gain = g, ep->pan = pan, ep->front = fr, ep->spread = 0;
}

static int start(Mixer *m, SynLibrary *lib, int ri, const float *inputs, u32 seed, const char *sample, const EnvSource *src, float delay, bool echo) {
    const SynRecipe *r = lib->recipe[ri];
    SynRecipeOut o;
    syn_recipe_eval(lib, r, inputs, seed, sample, &o);
    if (g_nv >= MIX_VOICES) return -1;
    RVoice *v = &g_v[g_nv];
    memset(v, 0, sizeof(*v));
    MixStart ms;
    memset(&ms, 0, sizeof(ms));
    for (int i = 0; i < o.nlayers; i++) {
        if (!o.layer[i].on || !o.layer[i].patch || (echo && o.layer[i].optional)) continue;
        MixLayerStart *l = &ms.layer[ms.nlayers];
        l->patch = o.layer[i].patch;
        l->gain = o.layer[i].gain;
        l->optional = o.layer[i].optional;
        memcpy(l->params, o.layer[i].params, sizeof(l->params));
        v->layer_map[ms.nlayers++] = i;
    }
    if (!ms.nlayers) return -1;
    env_world_voice_start(&v->env, src);
    EnvParams ep;
    float d = env_world_voice_eval(&v->env, false, &ep);
    if (!g_chain && src->spatial) neutralize(&ep);
    ms.seed = seed;
    ms.bus = r->bus;
    ms.env = src->spatial;
    ms.doppler = src->doppler;
    ms.start_delay = delay >= 0 ? delay : (src->doppler ? 0.0f : d);
    ms.volume = src->volume;
    ms.score = 1.0f;
    ms.env_params = ep;
    v->h = mixer_start(m, &ms);
    if (!v->h) return -1;
    v->echo = echo;
    v->recipe = ri;
    v->seed = seed;
    v->nlayers = ms.nlayers;
    memcpy(v->inputs, inputs, sizeof(v->inputs));
    return g_nv++;
}

static void emit(Mixer *m, SynLibrary *lib, int ri, const float *inputs, u32 seed, const char *sample, vec3 pos) {
    const SynRecipe *r = lib->recipe[ri];
    EnvSource src;
    source_of(r, pos, 1.0f, &src);
    start(m, lib, ri, inputs, seed, sample, &src, -1.0f, false);
    if (!r->echo || !r->spatial || !g_chain) return;
    const EnvRoom *room = env_world_room(pos, false);
    if (room && room->indoor) return;
    EnvEcho e[6];
    int ne = env_world_echoes(pos, e, 4);
    float direct = v3_dist(pos, env_world_listener()->pos) / 343.0f;
    for (int k = 0; k < ne; k++) {
        EnvSource es = src;
        es.pos = e[k].point;
        es.echo_child = true;
        es.fwd = v3(0, 0, 0);
        es.volume = e[k].gain;
        es.rear_lp = e[k].lp_hz;
        if (es.volume < 1e-3f) continue;
        start(m, lib, ri, inputs, seed, sample, &es, direct + e[k].extra_s, true);
        fprintf(stderr, "  echo +%.3f s from (%.0f, %.0f, %.0f), gain %.3f\n", e[k].extra_s, e[k].point.x, e[k].point.y, e[k].point.z, e[k].gain);
    }
}

static void write_wav(const char *path, const float *st, int frames, float gain) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "audio_render: cannot write %s\n", path); return; }
    unsigned data = (unsigned)frames * 4, riff = data + 36, rate = SR, br = SR * 4, fmt = 16;
    unsigned short pcm = 1, ch = 2, align = 4, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmt, 4, 1, f);
    fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&br, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    for (int i = 0; i < frames * 2; i++) {
        float x = st[i] * gain;
        x = x < -1 ? -1 : (x > 1 ? 1 : x);
        short s = (short)lrintf(x * 32767.0f);
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: audio_render <recipe> [key=value ...] [--scene s] [--dist m] [--az deg] [--hs m] [--dur s] [--seed n] [--sample name] [--no-samples] [--repeat n --every s] [--ramp key=a:b] [--gain db] [--chain off] -o out.wav\n"); return 2; }
    const char *recipe = argv[1], *out = "out.wav", *scn = "field", *sample = NULL;
    float dist = 20, az = 0, hs = 1.5f, dur = 3, every = 0.5f, gain_db = 0, norm_db = 1.0f;
    int seed = 1, repeat = 1;
    const char *keys[32];
    float vals[32];
    int nk = 0;
    char ramp_key[32] = "";
    float ramp_a = 0, ramp_b = 0;
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i], *nx = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "-o") && nx) out = argv[++i];
        else if (!strcmp(a, "--scene") && nx) scn = argv[++i];
        else if (!strcmp(a, "--dist") && nx) dist = (float)atof(argv[++i]);
        else if (!strcmp(a, "--az") && nx) az = (float)atof(argv[++i]);
        else if (!strcmp(a, "--hs") && nx) hs = (float)atof(argv[++i]);
        else if (!strcmp(a, "--dur") && nx) dur = (float)atof(argv[++i]);
        else if (!strcmp(a, "--seed") && nx) seed = atoi(argv[++i]);
        else if (!strcmp(a, "--sample") && nx) sample = argv[++i];
        else if (!strcmp(a, "--no-samples")) g_no_samples = true;
        else if (!strcmp(a, "--repeat") && nx) repeat = atoi(argv[++i]);
        else if (!strcmp(a, "--every") && nx) every = (float)atof(argv[++i]);
        else if (!strcmp(a, "--gain") && nx) gain_db = (float)atof(argv[++i]);
        else if (!strcmp(a, "--normalize") && nx) norm_db = (float)atof(argv[++i]);
        else if (!strcmp(a, "--chain") && nx) g_chain = strcmp(argv[++i], "off") != 0;
        else if (!strcmp(a, "--ramp") && nx) {
            const char *s = argv[++i], *eq = strchr(s, '=');
            if (eq) { snprintf(ramp_key, sizeof(ramp_key), "%.*s", (int)(eq - s), s); sscanf(eq + 1, "%f:%f", &ramp_a, &ramp_b); }
        } else if (strchr(a, '=') && nk < 32) {
            const char *eq = strchr(a, '=');
            static char kb[32][32];
            snprintf(kb[nk], sizeof(kb[nk]), "%.*s", (int)(eq - a), a);
            keys[nk] = kb[nk];
            vals[nk++] = (float)atof(eq + 1);
        } else { fprintf(stderr, "audio_render: unknown argument '%s'\n", a); return 2; }
    }
    // Library: materials, patches, recipes from the source tree.
    static SynMaterialTable mt;
    syn_material_defaults(&mt);
    char path[600], err[200];
    snprintf(path, sizeof(path), "%s/assets/audio/materials.json", DG_SOURCE_DIR);
    char *txt = read_text(path);
    if (txt && !syn_material_parse(&mt, txt, err, sizeof(err))) fprintf(stderr, "%s\n", err);
    free(txt);
    syn_material_set_table(&mt);
    syn_set_sample_bank(&g_bank);
    static SynLibrary lib;
    syn_lib_init(&lib, (float)SR, resolve, NULL);
    load_dir(&lib, "patches", true);
    load_dir(&lib, "recipes", false);
    int ri = syn_lib_find_recipe(&lib, recipe);
    if (ri < 0) { fprintf(stderr, "audio_render: no recipe '%s'\n", recipe); return 1; }
    const SynRecipe *r = lib.recipe[ri];
    vec3 src, lis;
    if (!scene(scn, dist, az, hs, &src, &lis)) { fprintf(stderr, "audio_render: no scene '%s'\n", scn); return 2; }
    EnvWorldQuery q = { ray, foliage, NULL };
    env_world_init();
    env_world_set_query(&q);
    EnvListener el = *env_world_listener();
    el.pos = lis;
    el.fwd = v3(0, 0, -1);
    el.right = v3(1, 0, 0);
    el.up = v3(0, 1, 0);
    env_world_set_listener(&el);
    env_world_begin_frame(1.0f);
    Mixer *m = mixer_create((float)SR, clock_s);
    mixer_force_degrade(m, 0);
    float inputs[RCP_MAX_INPUTS];
    for (int i = 0; i < r->ninputs; i++) inputs[i] = r->input_def[i];
    for (int k = 0; k < nk; k++) {
        int idx = syn_recipe_input(r, keys[k]);
        if (idx >= 0) inputs[idx] = vals[k];
        else fprintf(stderr, "audio_render: %s has no input '%s'\n", r->name, keys[k]);
    }
    int di = syn_recipe_input(r, "distance");
    if (di >= 0) inputs[di] = v3_dist(src, lis);
    int rk = ramp_key[0] ? syn_recipe_input(r, ramp_key) : -1;
    if (rk >= 0) inputs[rk] = ramp_a;
    int frames = (int)(dur * SR);
    float *st = (float *)calloc((size_t)frames * 2, sizeof(float));
    int next_emit = 0, emitted = 0, chunk = 480;
    for (int off = 0; off < frames; off += chunk) {
        float t = (float)off / SR;
        while (emitted < repeat && off >= next_emit) {
            emit(m, &lib, ri, inputs, (u32)(seed + emitted), sample, src);
            emitted++;
            next_emit = (int)(emitted * every * SR);
        }
        // Every 10 ms: the environment of every voice (as the game's frame does), ramps.
        env_world_begin_frame((float)chunk / SR);
        for (int i = 0; i < g_nv; i++) {
            RVoice *v = &g_v[i];
            if (!mixer_alive(m, v->h)) continue;
            if (rk >= 0) {
                v->inputs[rk] = ramp_a + (ramp_b - ramp_a) * t / dur;
                SynRecipeOut o;
                syn_recipe_eval(&lib, lib.recipe[v->recipe], v->inputs, v->seed, sample, &o);
                for (int l = 0; l < v->nlayers; l++) {
                    const SynLayerOut *lo = &o.layer[v->layer_map[l]];
                    if (lo->patch) mixer_params(m, v->h, l, lo->params, lo->patch->nparams);
                }
            }
            if (v->echo || !v->env.src.spatial) continue;
            EnvParams ep;
            env_world_voice_eval(&v->env, (off / chunk) % 8 == 0, &ep);
            if (!g_chain) neutralize(&ep);
            mixer_env(m, v->h, &ep);
        }
        EnvReverbParams rev[ENV_ZONES];
        env_world_reverbs(rev);
        for (int z = 0; z < ENV_ZONES; z++) mixer_set_reverb(m, z, &rev[z]);
        mixer_update(m);
        int n = frames - off < chunk ? frames - off : chunk;
        mixer_render(m, st + (size_t)off * 2, n);
    }
    // A 30 ms fade at the end (loops are cut off).
    int fade = SR * 3 / 100;
    for (int i = 0; i < fade && i < frames; i++) {
        float g = (float)i / (float)fade;
        st[(frames - 1 - i) * 2] *= g;
        st[(frames - 1 - i) * 2 + 1] *= g;
    }
    float peak = 0, rms = 0;
    for (int i = 0; i < frames * 2; i++) { peak = fmaxf(peak, fabsf(st[i])); rms += st[i] * st[i]; }
    if (norm_db <= 0.0f && peak > 1e-6f) gain_db = norm_db - 20.0f * log10f(peak);
    const EnvRoom *rm = env_world_room(lis, true);
    float forest, urban;
    env_world_field(&forest, &urban);
    fprintf(stderr, "audio_render: %s in %s at %.0f m: peak %.3f (%.1f dBFS), rms %.4f; listener %s (RT60 %.2f s), forest %.2f, urban %.2f -> %s\n", r->name, scn, v3_dist(src, lis),
            peak, 20.0f * log10f(fmaxf(peak, 1e-9f)), sqrtf(rms / (frames * 2)), rm && rm->indoor ? "indoor" : "outdoor", rm ? rm->rt60[1] : 0.0f, forest, urban, out);
    write_wav(out, st, frames, powf(10.0f, gain_db / 20.0f));
    mixer_destroy(m);
    syn_lib_free(&lib);
    free(st);
    return 0;
}
