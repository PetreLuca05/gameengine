#include "voice_codec.h"

static const int k_steps[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411,
    1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};
static const int k_index[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

// One nibble through the decoder: the shared step, so encoder and decoder track the same state.
static void step(int *pred, int *index, int code) {
    int st = k_steps[*index];
    int diff = st >> 3;
    if (code & 4) diff += st;
    if (code & 2) diff += st >> 1;
    if (code & 1) diff += st >> 2;
    *pred += code & 8 ? -diff : diff;
    *pred = DG_CLAMP(*pred, -32768, 32767);
    *index = DG_CLAMP(*index + k_index[code], 0, 88);
}

void voice_encode(VoiceEncoder *e, const i16 *pcm, u8 *out) {
    out[0] = (u8)(e->pred & 0xFF);
    out[1] = (u8)((e->pred >> 8) & 0xFF);
    out[2] = (u8)e->index;
    u8 *d = out + 3;
    for (int i = 0; i < VOICE_FRAME; i++) {
        int diff = pcm[i] - e->pred, code = 0;
        if (diff < 0) { code = 8; diff = -diff; }
        int st = k_steps[e->index];
        if (diff >= st) { code |= 4; diff -= st; }
        if (diff >= st >> 1) { code |= 2; diff -= st >> 1; }
        if (diff >= st >> 2) code |= 1;
        step(&e->pred, &e->index, code);
        if (i & 1) d[i >> 1] |= (u8)(code << 4);
        else d[i >> 1] = (u8)code;
    }
}

void voice_decode(const u8 *in, i16 *pcm) {
    int pred = (i16)(in[0] | (in[1] << 8)), index = DG_CLAMP((int)in[2], 0, 88);
    const u8 *d = in + 3;
    for (int i = 0; i < VOICE_FRAME; i++) {
        int code = i & 1 ? d[i >> 1] >> 4 : d[i >> 1] & 15;
        step(&pred, &index, code);
        pcm[i] = (i16)pred;
    }
}

void voice_upsample3(VoiceUpsample *u, const i16 *in, int n, float *out) {
    float a = u->last;
    for (int i = 0; i < n; i++) {
        float b = (float)in[i] * (1.0f / 32768.0f);
        out[i * 3 + 0] = a + (b - a) * (1.0f / 3.0f);
        out[i * 3 + 1] = a + (b - a) * (2.0f / 3.0f);
        out[i * 3 + 2] = b;
        a = b;
    }
    u->last = a;
}
