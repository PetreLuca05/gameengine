#include "voice_capture.h"
#include "voice_codec.h"
#include "mix_atomic.h"
#include "core/log.h"
#include "miniaudio.h"
#include <string.h>

#define CAP_LEN 32768   // 2 s at 16 kHz

static struct {
    ma_device dev;
    bool open, failed;
    i16 ring[CAP_LEN];
    mix_atomic w, r;    // w: the device thread, r: the game thread
} C;

static void on_capture(ma_device *dev, void *out, const void *in, ma_uint32 frames) {
    (void)dev;
    (void)out;
    if (!in) return;
    const i16 *s = (const i16 *)in;
    long w = mix_load(&C.w);
    for (ma_uint32 i = 0; i < frames; i++) C.ring[(w + (long)i) % CAP_LEN] = s[i];
    mix_store(&C.w, (w + (long)frames) % CAP_LEN);
}

bool voice_capture_open(void) {
    if (C.open) return true;
    if (C.failed) return false;
    ma_device_config cfg = ma_device_config_init(ma_device_type_capture);
    cfg.capture.format = ma_format_s16;
    cfg.capture.channels = 1;
    cfg.sampleRate = VOICE_RATE;
    cfg.dataCallback = on_capture;
    if (ma_device_init(NULL, &cfg, &C.dev) != MA_SUCCESS) {
        log_warn("voice: no microphone (capture device failed to open)");
        C.failed = true;
        return false;
    }
    mix_store(&C.w, 0);
    mix_store(&C.r, 0);
    if (ma_device_start(&C.dev) != MA_SUCCESS) {
        log_warn("voice: the microphone failed to start");
        ma_device_uninit(&C.dev);
        C.failed = true;
        return false;
    }
    C.open = true;
    log_info("voice: microphone '%s' open (%u Hz mono)", C.dev.capture.name, VOICE_RATE);
    return true;
}

void voice_capture_close(void) {
    if (!C.open) return;
    ma_device_uninit(&C.dev);
    C.open = false;
}

bool voice_capture_is_open(void) { return C.open; }

int voice_capture_read(i16 *out, int max) {
    if (!C.open) return 0;
    long w = mix_load(&C.w), r = mix_load(&C.r);
    long avail = (w - r + CAP_LEN) % CAP_LEN;
    if (avail > VOICE_RATE) { r = (w - VOICE_RATE + CAP_LEN) % CAP_LEN; avail = VOICE_RATE; }   // fell behind: keep the last second
    int n = (int)(avail < max ? avail : max);
    for (int i = 0; i < n; i++) out[i] = C.ring[(r + i) % CAP_LEN];
    mix_store(&C.r, (r + n) % CAP_LEN);
    return n;
}
