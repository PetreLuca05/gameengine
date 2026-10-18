#include "env_chain.h"
#include <math.h>
#include <string.h>

#define CHAIN_TAU 0.02f          // s: parameter glide
#define XO_LOW 250.0f
#define XO_HIGH 2000.0f

void env_params_neutral(EnvParams *p) {
    memset(p, 0, sizeof(*p));
    p->gain = 1.0f;
    p->lp_hz = 20000.0f;
    p->front = 1.0f;
    p->spread = 1.0f;
}

static void snap(SynSmooth *s, float v, float sr) { syn_smooth_init(s, v, CHAIN_TAU, sr); }

void env_chain_init(EnvChain *c, float sr, float *dop, int dop_len, const EnvParams *p) {
    memset(c, 0, sizeof(*c));
    c->sr = sr;
    c->dop = dop;
    c->dop_len = dop_len;
    if (dop) memset(dop, 0, sizeof(float) * (size_t)dop_len);
    c->target = *p;
    snap(&c->gain, p->gain, sr);
    snap(&c->lp, p->lp_hz, sr);
    for (int b = 0; b < ENV_BANDS; b++) snap(&c->band[b], syn_db(p->band_db[b]), sr);
    snap(&c->pan, p->pan, sr);
    snap(&c->front, p->front, sr);
    snap(&c->spread, p->spread, sr);
    for (int z = 0; z < ENV_ZONES; z++) snap(&c->send[z], p->send[z], sr);
    snap(&c->delay, p->delay_s * sr, sr);
    c->delay.coef = 1.0f - expf(-(float)SYN_BLOCK / (0.05f * sr));   // doppler glide: slower than the gains
    c->ntaps = p->ntaps;
    c->tap_limit = ENV_MAX_TAPS;
    for (int t = 0; t < ENV_MAX_TAPS; t++) {
        const EnvTap *tp = &p->tap[t];
        snap(&c->tap[t].gain, t < p->ntaps ? tp->gain : 0.0f, sr);
        snap(&c->tap[t].delay, t < p->ntaps ? tp->delay_s * sr : 0.0f, sr);
        snap(&c->tap[t].pan, t < p->ntaps ? tp->pan : 0.0f, sr);
        c->tap[t].delay.coef = 1.0f - expf(-(float)SYN_BLOCK / (0.08f * sr));
        c->tap[t].lp_coef = syn_onepole_coef(t < p->ntaps ? tp->lp_hz : 20000.0f, sr);
    }
}

void env_chain_set(EnvChain *c, const EnvParams *p) {
    c->target = *p;
    c->gain.target = p->gain;
    c->lp.target = p->lp_hz;
    for (int b = 0; b < ENV_BANDS; b++) c->band[b].target = syn_db(p->band_db[b]);
    c->pan.target = p->pan;
    c->front.target = p->front;
    c->spread.target = p->spread;
    for (int z = 0; z < ENV_ZONES; z++) c->send[z].target = p->send[z];
    c->delay.target = p->delay_s * c->sr;
    int nt = p->ntaps < ENV_MAX_TAPS ? p->ntaps : ENV_MAX_TAPS;
    for (int t = 0; t < ENV_MAX_TAPS; t++) {
        EnvTapState *ts = &c->tap[t];
        if (t < nt) {
            if (t >= c->ntaps) {
                // A new tap fades in at its place (no delay glide from zero).
                ts->delay.cur = ts->delay.target = p->tap[t].delay_s * c->sr;
                ts->pan.cur = ts->pan.target = p->tap[t].pan;
            }
            ts->gain.target = p->tap[t].gain;
            ts->delay.target = p->tap[t].delay_s * c->sr;
            ts->pan.target = p->tap[t].pan;
            ts->lp_coef = syn_onepole_coef(p->tap[t].lp_hz, c->sr);
        } else {
            ts->gain.target = 0.0f;   // fades out, keeps its delay
        }
    }
    if (nt > c->ntaps) c->ntaps = nt;
}

void env_chain_process(EnvChain *c, const float *in, int n, float *out_l, float *out_r, float *const send[ENV_ZONES]) {
    const float sr = c->sr;
    float x[SYN_BLOCK];
    // 1. Doppler: a variable read from the propagation line (the delay itself is the arrival time).
    if (c->dop) {
        float d0, d1;
        syn_smooth_block(&c->delay, &d0, &d1);
        float lim = (float)(c->dop_len - 4);
        for (int i = 0; i < n; i++) {
            c->dop[c->dop_pos] = in[i];
            float d = syn_clampf(d0 + (d1 - d0) * (float)(i + 1) / (float)n, 0.0f, lim);
            float rp = (float)c->dop_pos - d;
            int i0 = (int)floorf(rp);
            float fr = rp - (float)i0;
            float a = c->dop[i0 & (c->dop_len - 1)], b = c->dop[(i0 + 1) & (c->dop_len - 1)];
            x[i] = a + fr * (b - a);
            c->dop_pos = (c->dop_pos + 1) & (c->dop_len - 1);
        }
    } else {
        memcpy(x, in, sizeof(float) * (size_t)n);
    }
    // 2..6 in one pass, so the independent one-pole chains (top cut, crossovers, the two ears)
    // overlap: top cut -> 3 bands -> gain -> ring -> pan + ITD + head shadow.
    float l0, l1, g0, g1, b0[ENV_BANDS], b1[ENV_BANDS];
    syn_smooth_block(&c->lp, &l0, &l1);
    syn_smooth_block(&c->gain, &g0, &g1);
    for (int b = 0; b < ENV_BANDS; b++) syn_smooth_block(&c->band[b], &b0[b], &b1[b]);
    float p0, p1, f0, f1, s0, s1;
    syn_smooth_block(&c->pan, &p0, &p1);
    syn_smooth_block(&c->front, &f0, &f1);
    syn_smooth_block(&c->spread, &s0, &s1);
    float k = syn_onepole_coef(l1, sr), klo = syn_onepole_coef(XO_LOW, sr), khi = syn_onepole_coef(XO_HIGH, sr);
    bool cut = l1 < 19000.0f || l0 < 19000.0f;
    float inv = 1.0f / (float)n;
    float lp1 = c->lp1, lp2 = c->lp2, x250 = c->x250, x2k = c->x2k;
    // Complementary split: low + mid + high == s; each band and the gain ramp across the block.
    float d0 = (b1[0] - b0[0]) * inv, d1 = (b1[1] - b0[1]) * inv, d2 = (b1[2] - b0[2]) * inv, dg = (g1 - g0) * inv;
    // Direct: the spherical-head model (Brown & Duda 1998). Each ear is a first-order shelf
    // H(s) = (alpha s + beta) / (s + beta), beta = 2c / a (head radius a = 8.75 cm): alpha ~ 2 on the
    // ear facing the source (+6 dB highs), ~0.1 in its shadow, the bright spot right behind the head.
    // The far ear hears it later by Woodworth's ITD, (a / c)(psi + sin psi). Plus a mild broadband
    // level difference, and the pinna's darkening from behind. Spread (2D, very close) blends to centre.
    float pan = p1 * (1.0f - s1), front = f1;
    float az = atan2f(pan, fabsf(front) > 1e-4f || fabsf(pan) > 1e-4f ? front : 1.0f);   // 0 ahead, +pi/2 right
    float rear = front < 0 ? -front : 0.0f;
    const float head_a = 0.0875f, beta = 2.0f * 343.0f / head_a, K = 2.0f * sr;
    float hb0[2], hb1[2], ha1[2];
    for (int e = 0; e < 2; e++) {
        float ear = e == 0 ? -SYN_PI * 0.5f : SYN_PI * 0.5f;   // left, right ear axis
        float th = fabsf(az - ear);
        if (th > SYN_PI) th = 2.0f * SYN_PI - th;
        float alpha = 1.05f + 0.95f * cosf(th * (180.0f / 150.0f));
        alpha = 1.0f + (alpha - 1.0f) * (1.0f - s1);
        alpha *= 1.0f - 0.3f * rear;
        hb0[e] = (alpha * K + beta) / (K + beta);
        hb1[e] = (beta - alpha * K) / (K + beta);
        ha1[e] = (beta - K) / (K + beta);
    }
    float psi = asinf(syn_clampf(pan, -1.0f, 1.0f));
    float itd = head_a / 343.0f * (fabsf(psi) + sinf(fabsf(psi))) * sr;
    float gl1 = 0.72f * (1.0f - 0.22f * pan), gr1 = 0.72f * (1.0f + 0.22f * pan);
    float pa = p0 * (1.0f - s0);
    float gl0 = 0.72f * (1.0f - 0.22f * pa), gr0 = 0.72f * (1.0f + 0.22f * pa);
    bool right_far = pan < 0;   // source on the left: the right ear is far
    int di = (int)itd;
    float fr = itd - (float)di;
    float shl = c->shadow_l, shr = c->shadow_r, xl1 = c->head_x[0], xr1 = c->head_x[1];
    float dgl = (gl1 - gl0) * inv, dgr = (gr1 - gr0) * inv;
    bool out = out_l && out_r;
    int base = c->ring_pos;
    for (int i = 0; i < n; i++) {
        float s = x[i], t = (float)(i + 1);
        if (cut) {
            lp1 += k * (s - lp1);
            lp2 += k * (lp1 - lp2);
            s = lp2;
        }
        x250 += klo * (s - x250);
        x2k += khi * (s - x2k);
        float y = x250 * (b0[0] + d0 * t) + (x2k - x250) * (b0[1] + d1 * t) + (s - x2k) * (b0[2] + d2 * t);
        y *= g0 + dg * t;
        x[i] = y;
        int pos = (base + i) & (ENV_RING - 1);
        c->ring[pos] = y;   // the taps and the far ear read it back
        if (!out) continue;
        // far = ring[pos - itd] (linear interpolation between the two neighbours)
        int q0 = (pos - di) & (ENV_RING - 1), q1 = (q0 - 1) & (ENV_RING - 1);
        float far = c->ring[q0] + fr * (c->ring[q1] - c->ring[q0]);
        float sl = right_far ? y : far, srr = right_far ? far : y;
        float yl = hb0[0] * sl + hb1[0] * xl1 - ha1[0] * shl;
        float yr = hb0[1] * srr + hb1[1] * xr1 - ha1[1] * shr;
        xl1 = sl, xr1 = srr, shl = yl, shr = yr;
        out_l[i] += (gl0 + dgl * t) * yl;
        out_r[i] += (gr0 + dgr * t) * yr;
    }
    c->lp1 = lp1, c->lp2 = lp2, c->x250 = x250, c->x2k = x2k;
    c->shadow_l = shl;
    c->shadow_r = shr;
    c->head_x[0] = xl1, c->head_x[1] = xr1;
    // 7. Early reflections: each tap is a gain / pan of the ring at its delay (one read offset per
    // block: the delay glides a fraction of a sample per block), summed per ear, then one low-pass per
    // ear at the taps' gain-weighted cutoff (wall absorption colours them alike). The tap loops have
    // no feedback, so they vectorize.
    float erl[SYN_BLOCK], err_[SYN_BLOCK];
    int nt = 0;
    float kw = 0.0f, wsum = 0.0f;
    for (int t = 0; t < c->ntaps && t < c->tap_limit; t++) {
        EnvTapState *ts = &c->tap[t];
        float ga, gb, da, db, pa, pb;
        syn_smooth_block(&ts->gain, &ga, &gb);
        syn_smooth_block(&ts->delay, &da, &db);
        syn_smooth_block(&ts->pan, &pa, &pb);
        (void)da;
        (void)pa;
        if ((ga < 1e-5f && gb < 1e-5f) || !out_l || !out_r) continue;
        if (!nt) {
            memset(erl, 0, sizeof(float) * (size_t)n);
            memset(err_, 0, sizeof(float) * (size_t)n);
        }
        nt++;
        float th = (pb + 1.0f) * (SYN_PI * 0.25f), tl = cosf(th), tr = sinf(th);
        float d = syn_clampf(db, 1.0f, (float)(ENV_RING - 2));
        int di = (int)d;
        float fr = d - (float)di, dg = (gb - ga) * inv;
        int start = (base - di) & (ENV_RING - 1);
        float tmp[SYN_BLOCK];
        if (start >= 1 && start + n <= ENV_RING) {
            const float *a = c->ring + start, *b = a - 1;
            for (int i = 0; i < n; i++) tmp[i] = a[i] + fr * (b[i] - a[i]);
        } else {
            for (int i = 0; i < n; i++) {
                int p0 = (start + i) & (ENV_RING - 1), p1 = (p0 - 1) & (ENV_RING - 1);
                tmp[i] = c->ring[p0] + fr * (c->ring[p1] - c->ring[p0]);
            }
        }
        for (int i = 0; i < n; i++) {
            float g = ga + dg * (float)(i + 1);
            erl[i] += g * tl * tmp[i];
            err_[i] += g * tr * tmp[i];
        }
        kw += ts->lp_coef * gb;
        wsum += gb;
    }
    if (nt) {
        float k = wsum > 0 ? kw / wsum : 1.0f, ll = c->er_lp[0], lr = c->er_lp[1];
        for (int i = 0; i < n; i++) {
            ll += k * (erl[i] - ll);
            lr += k * (err_[i] - lr);
            out_l[i] += ll;
            out_r[i] += lr;
        }
        c->er_lp[0] = ll, c->er_lp[1] = lr;
    }
    while (c->ntaps > 0 && c->tap[c->ntaps - 1].gain.cur < 1e-5f && c->ntaps > c->target.ntaps) c->ntaps--;
    // 8. Sends.
    for (int z = 0; z < ENV_ZONES; z++) {
        float a, b;
        syn_smooth_block(&c->send[z], &a, &b);
        if (send && send[z] && (a > 1e-5f || b > 1e-5f)) syn_ramp_mac(x, send[z], n, a, b);
    }
    c->ring_pos = (base + n) & (ENV_RING - 1);
}

// ------------------------------------------------------------------ FDN reverb

// Mutually prime lengths (samples at 48 kHz, size 1): 31..58 ms.
static const int k_fdn_len[ENV_FDN] = { 1499, 1693, 1867, 2053, 2251, 2399, 2617, 2797 };

void env_reverb_init(EnvReverb *r, float sr, const EnvReverbParams *p) {
    memset(r, 0, sizeof(*r));
    r->sr = sr;
    r->target = *p;
    syn_smooth_init(&r->rt_lo, p->rt60_low, 0.3f, sr);
    syn_smooth_init(&r->rt_hi, p->rt60_high, 0.3f, sr);
    syn_smooth_init(&r->size, p->size, 0.5f, sr);
    syn_smooth_init(&r->wet, p->wet, 0.1f, sr);
}

void env_reverb_set(EnvReverb *r, const EnvReverbParams *p) {
    r->target = *p;
    r->rt_lo.target = p->rt60_low;
    r->rt_hi.target = p->rt60_high;
    r->size.target = p->size;
    r->wet.target = p->wet;
}

void env_reverb_process(EnvReverb *r, const float *in, int n, float *out_l, float *out_r) {
    float a, b;
    syn_smooth_block(&r->wet, &a, &b);
    float rlo, rhi, sz, dummy;
    syn_smooth_block(&r->rt_lo, &dummy, &rlo);
    syn_smooth_block(&r->rt_hi, &dummy, &rhi);
    syn_smooth_block(&r->size, &dummy, &sz);
    // Always runs (a few us): a muted zone keeps decaying instead of replaying stale energy later.
    float scale = syn_clampf(sz, 0.3f, 2.5f) * r->sr / 48000.0f;
    int len[ENV_FDN];
    float glo[ENV_FDN], ghi[ENV_FDN], circ = 0.0f;
    for (int l = 0; l < ENV_FDN; l++) {
        len[l] = DG_CLAMP((int)((float)k_fdn_len[l] * scale), 64, ENV_FDN_MAX - 1);
        glo[l] = powf(10.0f, -3.0f * (float)len[l] / (r->sr * fmaxf(rlo, 0.05f)));
        ghi[l] = powf(10.0f, -3.0f * (float)len[l] / (r->sr * fmaxf(rhi, 0.05f)));
        float g2 = 0.5f * (glo[l] * glo[l] + ghi[l] * ghi[l]);
        circ += g2 / (1.0f - g2 + 1e-6f);
    }
    // Calibrated output: a unit impulse decays like an ideal diffuse field, its energy integral
    // RT60 / 13.8 whatever the size (the network's own energy ~ 0.17 x mean g^2 / (1 - g^2), measured),
    // so a send sets the direct-to-reverberant ratio physically (env_world_voice_eval).
    float rt_mean = 0.5f * (fmaxf(rlo, 0.05f) + fmaxf(rhi, 0.05f));
    float norm = sqrtf((rt_mean / 13.8f) / (0.17f * circ / (float)ENV_FDN + 1e-6f));
    int pre = DG_CLAMP((int)(r->target.predelay_ms * 0.001f * r->sr), 0, 4095);
    float kd = syn_onepole_coef(1500.0f, r->sr);
    float inv = 1.0f / (float)n;
    for (int i = 0; i < n; i++) {
        r->pre[r->pre_pos] = in[i];
        float x = r->pre[(r->pre_pos - pre + 4096) & 4095];
        r->pre_pos = (r->pre_pos + 1) & 4095;
        // Two input allpasses (diffusion).
        static const int k_ap[2] = { 142, 379 };
        for (int k = 0; k < 2; k++) {
            float *buf = r->ap_buf[k];
            int p = r->ap_pos[k];
            float d = buf[p];
            float y = -0.6f * x + d;
            buf[p] = x + 0.6f * y;
            r->ap_pos[k] = (p + 1) % k_ap[k];
            x = y;
        }
        float o[ENV_FDN], sum = 0.0f;
        for (int l = 0; l < ENV_FDN; l++) {
            int rp = r->pos[l] - len[l];
            if (rp < 0) rp += ENV_FDN_MAX;
            float y = r->line[l][rp];
            // Two-band decay: highs at ghi, lows at glo (crossover 1.5 kHz).
            r->damp[l] += kd * (y - r->damp[l]);
            o[l] = ghi[l] * y + (glo[l] - ghi[l]) * r->damp[l];
            sum += o[l];
        }
        float h = sum * (2.0f / (float)ENV_FDN);   // Householder: o - 2/N * sum
        float wl = 0.0f, wr = 0.0f;
        for (int l = 0; l < ENV_FDN; l++) {
            r->line[l][r->pos[l]] = o[l] - h + x * 0.35f;
            if (++r->pos[l] >= ENV_FDN_MAX) r->pos[l] = 0;
            if (l & 1) wr += o[l]; else wl += o[l];
        }
        float g = (a + (b - a) * (float)(i + 1) * inv) * norm;
        out_l[i] += g * wl * 0.35f;
        out_r[i] += g * wr * 0.35f;
    }
}
