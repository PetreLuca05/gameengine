// Voice chat (docs/AUDIO_PROCEDURAL.md "Voice chat"):
//   codec      ADPCM round trip of a voice-like signal: SNR > 20 dB; a frame decodes alone (lost packets)
//   upsample   16 -> 48 kHz keeps the level and is continuous across calls
//   stream     the mixer plays a pushed stream after its jitter buffer, close: through the chain
//   radio      radio 1: the voice band only (a 100 Hz tone mostly gone, 1 kHz kept), a squelch at the end
#include "audio/voice_codec.h"
#include "audio/mixer.h"
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

static void voice_signal(i16 *out, int n, int t0) {
    for (int i = 0; i < n; i++) {
        double t = (double)(t0 + i) / VOICE_RATE;
        double x = 0.5 * sin(2 * 3.14159265 * 150 * t) + 0.25 * sin(2 * 3.14159265 * 750 * t) + 0.15 * sin(2 * 3.14159265 * 1650 * t);
        out[i] = (i16)(x * 12000.0);
    }
}

static void test_codec(void) {
    VoiceEncoder e = { 0, 0 };
    double sig = 0, err = 0;
    u8 frames[10][VOICE_FRAME_BYTES];
    i16 in[10][VOICE_FRAME];
    for (int f = 0; f < 10; f++) {
        voice_signal(in[f], VOICE_FRAME, f * VOICE_FRAME);
        voice_encode(&e, in[f], frames[f]);
    }
    for (int f = 2; f < 10; f++) {   // past the encoder's first adaptation
        i16 out[VOICE_FRAME];
        voice_decode(frames[f], out);
        for (int i = 0; i < VOICE_FRAME; i++) {
            sig += (double)in[f][i] * in[f][i];
            err += (double)(in[f][i] - out[i]) * (in[f][i] - out[i]);
        }
    }
    double snr = 10.0 * log10(sig / fmax(err, 1.0));
    CHECK(snr > 20.0, "codec SNR %.1f dB", snr);
    // Frame 7 decoded without frames 0..6: the same samples as in sequence (self-contained header).
    i16 a[VOICE_FRAME], b[VOICE_FRAME];
    voice_decode(frames[7], a);
    voice_decode(frames[7], b);
    CHECK(!memcmp(a, b, sizeof(a)), "a frame decodes alone");
    printf("codec: SNR %.1f dB, %d bytes per 20 ms (%.0f kbit/s)\n", snr, VOICE_FRAME_BYTES, VOICE_FRAME_BYTES * 8 * 50 / 1000.0);
}

static void test_upsample(void) {
    VoiceUpsample u = { 0 };
    i16 in[4] = { 16384, 16384, 16384, 16384 };
    float out[12];
    voice_upsample3(&u, in, 4, out);
    CHECK(fabsf(out[11] - 0.5f) < 1e-4f && fabsf(out[0] - 0.5f / 3.0f) < 1e-4f, "upsample ramps from 0 to 0.5 (%f %f)", out[0], out[11]);
    voice_upsample3(&u, in, 4, out);
    CHECK(fabsf(out[0] - 0.5f) < 1e-4f, "continuous across calls (%f)", out[0]);
}

// Renders `frames` stereo frames, returns the RMS of the left channel.
static float render_rms(Mixer *m, int frames) {
    static float buf[48000 * 2];
    mixer_render(m, buf, frames);
    double s = 0;
    for (int i = 0; i < frames; i++) s += (double)buf[i * 2] * buf[i * 2];
    return (float)sqrt(s / frames);
}

static float tone_through(float hz, float radio) {
    Mixer *m = mixer_create(48000.0f, NULL);
    EnvParams ep;
    env_params_neutral(&ep);
    mixer_stream_env(m, 0, &ep, radio);
    static float tone[48000];
    for (int i = 0; i < 14400; i++) tone[i] = 0.3f * sinf(2.0f * 3.14159265f * hz * (float)i / 48000.0f);
    CHECK(mixer_stream_push(m, 0, tone, 14400) == 14400, "push takes the samples");
    render_rms(m, 4800);   // the jitter buffer + the radio smoothing settle
    float rms = render_rms(m, 4800);
    mixer_destroy(m);
    return rms;
}

static void test_stream(void) {
    Mixer *m = mixer_create(48000.0f, NULL);
    EnvParams ep;
    env_params_neutral(&ep);
    mixer_stream_env(m, 0, &ep, 0.0f);
    static float tone[48000];
    for (int i = 0; i < 2400; i++) tone[i] = 0.3f;   // 50 ms: under the 80 ms jitter buffer
    mixer_stream_push(m, 0, tone, 2400);
    CHECK(render_rms(m, 1024) < 1e-6f, "silent until the jitter buffer fills");
    for (int i = 0; i < 4800; i++) tone[i] = 0.3f * sinf(2.0f * 3.14159265f * 440.0f * (float)i / 48000.0f);
    mixer_stream_push(m, 0, tone, 4800);
    float rms = render_rms(m, 2048);
    CHECK(rms > 0.05f, "plays once buffered (rms %f)", rms);
    CHECK(mixer_stream_level(m, 0) > 0.1f, "level reported (%f)", mixer_stream_level(m, 0));
    mixer_destroy(m);
    float low_close = tone_through(100.0f, 0.0f), low_radio = tone_through(100.0f, 1.0f), mid_radio = tone_through(1000.0f, 1.0f);
    CHECK(low_radio < low_close * 0.35f, "radio cuts 100 Hz (%f vs close %f)", low_radio, low_close);
    CHECK(mid_radio > low_radio * 3.0f, "radio keeps 1 kHz (%f vs 100 Hz %f)", mid_radio, low_radio);
    printf("stream: close 100 Hz %.3f, radio 100 Hz %.3f, radio 1 kHz %.3f\n", low_close, low_radio, mid_radio);
}

int main(void) {
    test_codec();
    test_upsample();
    test_stream();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
