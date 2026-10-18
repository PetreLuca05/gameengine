#include "env_model.h"
#include "audio/synth/syn_material.h"
#include <math.h>
#include <string.h>

float env_sound_speed(float temp_c) { return 331.3f * sqrtf(1.0f + temp_c / 273.15f); }

float env_air_alpha(float hz, float temp_c, float rh) {
    double T = 273.15 + temp_c, T0 = 293.15, T01 = 273.16;
    double C = -6.8346 * pow(T01 / T, 1.261) + 4.6151;
    double h = (rh < 1 ? 1 : rh) * pow(10.0, C);   // molar water vapour concentration, %
    double frO = 24.0 + 4.04e4 * h * (0.02 + h) / (0.391 + h);
    double frN = pow(T / T0, -0.5) * (9.0 + 280.0 * h * exp(-4.170 * (pow(T / T0, -1.0 / 3.0) - 1.0)));
    double f2 = (double)hz * hz;
    double a = 8.686 * f2 * (1.84e-11 * sqrt(T / T0) + pow(T / T0, -2.5) * (0.01275 * exp(-2239.1 / T) / (frO + f2 / frO) + 0.1068 * exp(-3352.0 / T) / (frN + f2 / frN)));
    return (float)a;
}

void env_air(float dist, float temp_c, float rh, float band_db[ENV_BANDS], float *lp_hz) {
    static const float k_hz[ENV_BANDS] = { ENV_BAND_HZ_LOW, ENV_BAND_HZ_MID, ENV_BAND_HZ_HIGH };
    for (int b = 0; b < ENV_BANDS; b++) band_db[b] = -env_air_alpha(k_hz[b], temp_c, rh) * dist;
    // The bands carry the loss at their centres; the top cut models the steeper fall above the high
    // band: where the loss exceeds the 4 kHz loss by 6 dB (bisected in log frequency, 2.5..20 kHz).
    float target = -band_db[2] + 6.0f;
    float lo = 2500.0f, hi = 20000.0f;
    if (env_air_alpha(hi, temp_c, rh) * dist < target) { *lp_hz = 20000.0f; return; }
    if (env_air_alpha(lo, temp_c, rh) * dist > target) { *lp_hz = 2500.0f; return; }
    for (int i = 0; i < 20; i++) {
        float mid = sqrtf(lo * hi);
        if (env_air_alpha(mid, temp_c, rh) * dist > target) hi = mid;
        else lo = mid;
    }
    *lp_hz = sqrtf(lo * hi);
}

float env_distance_gain(float d, float min_dist, float max_dist, float rolloff) {
    if (min_dist <= 0) min_dist = 1.0f;
    if (d < min_dist) d = min_dist;
    if (max_dist > min_dist && d > max_dist) d = max_dist;
    return min_dist / (min_dist + rolloff * (d - min_dist));
}

void env_directivity(float cos_axis, float front_db, float rear_db, float rear_lp, float *gain_db, float *lp_hz) {
    float c = cos_axis < -1 ? -1 : (cos_axis > 1 ? 1 : cos_axis);
    *gain_db = c >= 0 ? front_db * c : rear_db * -c;   // front_db on axis, rear_db straight behind
    float t = c < 0 ? -c : 0.0f;
    float lp = rear_lp > 0 ? rear_lp : 20000.0f;
    *lp_hz = expf(logf(20000.0f) * (1.0f - t) + logf(lp) * t);
}

float env_ground_db(float dist, float softness) {
    if (dist <= 20.0f) return 0.0f;
    float db = -2.5f * log2f(dist / 20.0f) * softness;
    return db < -8.0f ? -8.0f : db;
}

float env_noise_floor(const EnvWeather *w) {
    // Rain hiss and wind roar at the ear (exposure: a roof shields both).
    float e = 0.3f + 0.7f * w->exposure;
    float rain = 0.05f * w->rain * e;
    float wind = w->wind > 4.0f ? 0.004f * (w->wind - 4.0f) * e : 0.0f;
    return sqrtf(rain * rain + wind * wind);
}

float env_masking_gain(float level, float floor) {
    if (floor <= 0 || level <= 0) return 1.0f;
    // level / sqrt(level^2 + floor^2) tends to 1 far above the floor, ~ level / floor below it.
    float r = level / floor;
    return r / sqrtf(r * r + 1.0f);
}

void env_transmission(const EnvWall *walls, int n, float out_db[ENV_BANDS]) {
    for (int b = 0; b < ENV_BANDS; b++) out_db[b] = 0.0f;
    for (int i = 0; i < n; i++) {
        float tl[SYN_BANDS];
        syn_material_tl(syn_material(walls[i].material), walls[i].thickness, tl);
        for (int b = 0; b < ENV_BANDS; b++) out_db[b] -= tl[b];
    }
    for (int b = 0; b < ENV_BANDS; b++) if (out_db[b] < -80.0f) out_db[b] = -80.0f;
}

void env_diffraction(float detour, float out_db[ENV_BANDS]) {
    static const float k_hz[ENV_BANDS] = { ENV_BAND_HZ_LOW, ENV_BAND_HZ_MID, ENV_BAND_HZ_HIGH };
    for (int b = 0; b < ENV_BANDS; b++) {
        float N = 2.0f * (detour > 0 ? detour : 0.0f) * k_hz[b] / ENV_SOUND_SPEED;
        float db = 10.0f * log10f(3.0f + 20.0f * N) - 4.77f;   // 0 dB at N = 0 (grazing)
        out_db[b] = -(db > 25.0f ? 25.0f : db);
    }
}

// ------------------------------------------------------------------ rooms

#define D 0.70710678f
#define E 0.57735027f
const vec3 env_probe_dir[ENV_PROBE_RAYS] = {
    { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 },
    { D, 0, D }, { -D, 0, D }, { D, 0, -D }, { -D, 0, -D }, { E, E, E }, { -E, E, -E },
};
#undef D
#undef E

void env_room_estimate(vec3 o, const EnvProbeHit h[ENV_PROBE_RAYS], float max_dist, EnvRoom *r) {
    memset(r, 0, sizeof(*r));
    int hits = 0;
    float ab[ENV_BANDS] = { 0 };
    for (int i = 0; i < ENV_PROBE_RAYS; i++) {
        if (h[i].hit) {
            hits++;
            const SynMaterial *m = syn_material(h[i].material);
            for (int b = 0; b < ENV_BANDS; b++) ab[b] += m->absorb[b];
        } else {
            for (int b = 0; b < ENV_BANDS; b++) ab[b] += 1.0f;   // open: nothing comes back
        }
    }
    r->enclosed = (float)hits / (float)ENV_PROBE_RAYS;
    for (int b = 0; b < ENV_BANDS; b++) r->absorb[b] = ab[b] / (float)ENV_PROBE_RAYS;
    // Box from the axis rays; an open axis extends to the probe range.
    float d[6];
    for (int i = 0; i < 6; i++) {
        d[i] = h[i].hit ? h[i].dist : max_dist;
        r->wall_material[i] = h[i].hit ? h[i].material : -1;
    }
    r->mn = v3(o.x - d[1], o.y - d[3], o.z - d[5]);
    r->mx = v3(o.x + d[0], o.y + d[2], o.z + d[4]);
    // Diagonals: a horizontal diagonal that hits much nearer than the box corner means the room is
    // narrower than the axis rays suggest there (an L shape, a pillar): shrink toward it.
    for (int i = 6; i < 10; i++) {
        if (!h[i].hit) continue;
        vec3 dir = env_probe_dir[i];
        float ex = dir.x > 0 ? d[0] : d[1], ez = dir.z > 0 ? d[4] : d[5];
        float corner = fminf(ex, ez) / 0.70710678f;
        if (h[i].dist < 0.6f * corner) {
            float k = h[i].dist / corner;
            if (dir.x > 0) r->mx.x = o.x + d[0] * (0.5f + 0.5f * k); else r->mn.x = o.x - d[1] * (0.5f + 0.5f * k);
            if (dir.z > 0) r->mx.z = o.z + d[4] * (0.5f + 0.5f * k); else r->mn.z = o.z - d[5] * (0.5f + 0.5f * k);
        }
    }
    r->dims = v3_sub(r->mx, r->mn);
    r->volume = r->dims.x * r->dims.y * r->dims.z;
    r->surface = 2.0f * (r->dims.x * r->dims.y + r->dims.x * r->dims.z + r->dims.y * r->dims.z);
    // Indoor: a roof above, floor below, and most of the horizon closed.
    int horiz = 0;
    const int hz_idx[8] = { 0, 1, 4, 5, 6, 7, 8, 9 };
    for (int i = 0; i < 8; i++) horiz += h[hz_idx[i]].hit;
    r->indoor = h[2].hit && horiz >= 6;
    for (int b = 0; b < ENV_BANDS; b++) {
        float a = r->absorb[b];
        if (a > 0.99f) a = 0.99f;
        if (a < 0.01f) a = 0.01f;
        float air = 4.0f * 0.0001f * (float)(b + 1) * (float)(b + 1) * r->volume;   // 4 m V, m grows with frequency
        float rt = 0.161f * r->volume / (-r->surface * logf(1.0f - a) + air);
        r->rt60[b] = rt < 0.05f ? 0.05f : (rt > 8.0f ? 8.0f : rt);
    }
}

bool env_room_contains(const EnvRoom *r, vec3 p, float m) {
    return p.x >= r->mn.x - m && p.x <= r->mx.x + m && p.y >= r->mn.y - m && p.y <= r->mx.y + m && p.z >= r->mn.z - m && p.z <= r->mx.z + m;
}

int env_room_taps(const EnvRoom *r, vec3 s, vec3 l, vec3 right, float c, EnvTap *out, int max) {
    float direct = fmaxf(v3_dist(s, l), 0.3f);
    int n = 0;
    for (int w = 0; w < 6 && n < max; w++) {
        if (r->wall_material[w] < 0) continue;   // open side: nothing reflects
        vec3 img = s;
        int axis = w / 2;
        bool pos = (w & 1) == 0;
        float plane = axis == 0 ? (pos ? r->mx.x : r->mn.x) : axis == 1 ? (pos ? r->mx.y : r->mn.y) : (pos ? r->mx.z : r->mn.z);
        if (axis == 0) img.x = 2.0f * plane - s.x;
        else if (axis == 1) img.y = 2.0f * plane - s.y;
        else img.z = 2.0f * plane - s.z;
        float path = v3_dist(img, l);
        const SynMaterial *m = syn_material(r->wall_material[w]);
        float refl = sqrtf(fmaxf(1.0f - m->absorb[1], 0.0f));   // pressure reflection
        EnvTap *t = &out[n++];
        t->delay_s = (path - direct) / c;
        t->gain = refl * direct / path;
        // Absorptive walls darken the reflection: the high band absorbs more than the mid.
        float hi = sqrtf(fmaxf(1.0f - m->absorb[2], 0.01f)) / fmaxf(refl, 0.05f);
        t->lp_hz = 3000.0f + 15000.0f * (hi > 1 ? 1 : hi);
        float dummy;
        env_pan(l, right, v3(0, 0, 0), img, &t->pan, &dummy);
    }
    return n;
}

void env_room_reverb(const EnvRoom *r, EnvReverbParams *o) {
    o->rt60_low = 0.5f * (r->rt60[0] + r->rt60[1]);
    o->rt60_high = r->rt60[2];
    float mean = cbrtf(fmaxf(r->volume, 1.0f));
    o->size = syn_clampf(mean / 6.0f, 0.3f, 2.5f);
    o->predelay_ms = syn_clampf(mean * 1.5f, 2.0f, 60.0f);
    // A big hard room returns more diffuse energy than a small padded one.
    o->wet = syn_clampf(0.25f + 0.25f * logf(1.0f + o->rt60_low), 0.1f, 0.7f) * r->enclosed;
}

void env_outdoor_reverb(float forest, float urban, float snow, EnvReverbParams *o) {
    // Forest: dense, short, dark scattering from trunks and leaves. Urban: brighter, longer.
    o->rt60_low = 0.6f + 0.8f * forest + 1.2f * urban;
    o->rt60_high = 0.3f + 0.3f * forest + 0.8f * urban;
    o->size = 1.4f + 0.8f * urban;
    o->predelay_ms = 15.0f + 25.0f * urban;
    o->wet = (0.25f * forest + 0.2f * urban) * (1.0f - 0.6f * snow);
}

// ------------------------------------------------------------------ outdoor echoes

int env_outdoor_echoes(vec3 s, vec3 l, const EnvEchoRay *rays, int nrays, float c, float snow, float temp_c, float rh, EnvEcho *out, int max) {
    float direct = fmaxf(v3_dist(s, l), 1.0f);
    EnvEcho cand[64];
    int nc = 0;
    // Clusters: consecutive rays (by azimuth, wrapping) hitting at similar distances form one reflector.
    int start = 0;
    for (int i = 0; i < nrays; i++) if (!rays[i].hit) { start = i; break; }
    for (int k = 0; k < nrays && nc < 64;) {
        int i = (start + k) % nrays;
        if (!rays[i].hit) { k++; continue; }
        int cnt = 0;
        vec3 sum = v3(0, 0, 0);
        float dsum = 0, refl = 0;
        int mat = rays[i].material;
        float d0 = rays[i].dist;
        while (k < nrays) {
            int j = (start + k) % nrays;
            if (!rays[j].hit || fabsf(rays[j].dist - d0) > 0.2f * d0 + 3.0f) break;
            vec3 p = v3_madd(s, rays[j].dir, rays[j].dist);
            sum = v3_add(sum, p);
            dsum += rays[j].dist;
            refl += syn_material(rays[j].material)->reflect;
            cnt++;
            k++;
        }
        if (!cnt) { k++; continue; }
        EnvEcho *e = &cand[nc++];
        e->point = v3_scale(sum, 1.0f / (float)cnt);
        e->rays = cnt;
        e->material = mat;
        float sr = v3_dist(s, e->point), rl = v3_dist(e->point, l);
        float path = sr + rl;
        e->extra_s = (path - direct) / c;
        // Spherical spreading relative to the direct path; a reflector seen by more rays is bigger
        // (a cliff vs a lamp post): sqrt(rays) up to a facade; snow on the ground absorbs.
        float size = sqrtf((float)cnt) / 2.0f;
        if (size > 1.0f) size = 1.0f;
        e->gain = (refl / (float)cnt) * size * direct / path * (1.0f - 0.5f * snow);
        float band[ENV_BANDS], lp;
        env_air(path, temp_c, rh, band, &lp);
        e->lp_hz = lp;
        e->gain *= syn_db(band[1]);
        (void)dsum;
    }
    // Strongest first, discrete only.
    int n = 0;
    while (n < max) {
        int best = -1;
        for (int i = 0; i < nc; i++)
            if (cand[i].extra_s >= 0.025f && cand[i].gain > 1e-3f && (best < 0 || cand[i].gain > cand[best].gain)) best = i;
        if (best < 0) break;
        out[n++] = cand[best];
        cand[best].gain = 0;
    }
    return n;
}

void env_pan(vec3 lis, vec3 right, vec3 fwd, vec3 p, float *pan, float *front) {
    vec3 d = v3_norm(v3_sub(p, lis));
    *pan = syn_clampf(v3_dot(d, right), -1.0f, 1.0f);
    float f = v3_dot(d, fwd);
    *front = v3_len2(fwd) > 0 ? f : 1.0f;
}
