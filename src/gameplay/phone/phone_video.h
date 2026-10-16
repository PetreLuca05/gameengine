// Phone video recording: RGBA frames -> H.264 (minih264, CC0) -> MP4 (minimp4, CC0), no audio
// track yet (game audio via Opus / AAC is the documented upgrade path, docs/PHONE.md). Frames
// carry their capture time, so dropped frames lengthen the previous one instead of speeding the
// clip up. Width / height are rounded down to multiples of 16 (encoder requirement).
//
// phone_video_* calls are not thread safe on one recorder: PhoneRecorder (below) runs them in
// order on the job system.
#ifndef DG_PHONE_VIDEO_H
#define DG_PHONE_VIDEO_H

#include "core/base.h"

typedef struct PhoneVideo PhoneVideo;

PhoneVideo *phone_video_open(const char *path, int w, int h, int kbps, char *err, size_t err_size);
// rgba: w x h of the size passed to open (stride w * 4, top row first). t_ms: capture time.
bool phone_video_frame(PhoneVideo *v, const u8 *rgba, u32 t_ms);
// Writes the index and closes the file. end_ms: when recording stopped (the last frame lasts until
// then; 0 = one average frame). *seconds receives the clip length. Frees v.
bool phone_video_close(PhoneVideo *v, u32 end_ms, double *seconds, int *frames);
void phone_video_size(int w, int h, int *ew, int *eh);   // the encoded size for a source size

// RGBA (full range sRGB) -> I420, BT.601 limited range. Planes are tightly packed.
void phone_rgba_to_i420(const u8 *rgba, int w, int h, int stride, u8 *y, u8 *u, u8 *v);

// ------------------------------------------------------------------ asynchronous recorder
// Frames are copied into a small FIFO and encoded on one worker at a time, in order. When the
// FIFO is full the frame is dropped (the clip stays in sync: see the timing note above).
typedef struct PhoneRecorder PhoneRecorder;
PhoneRecorder *phone_recorder_start(const char *path, int w, int h, int kbps, char *err, size_t err_size);
bool phone_recorder_push(PhoneRecorder *r, const u8 *rgba, int w, int h, u32 t_ms);   // false = dropped
// Stops at end_ms (same clock as the pushes): waits for queued frames, closes the file, frees r.
bool phone_recorder_stop(PhoneRecorder *r, u32 end_ms, double *seconds, int *frames, int *dropped);
double phone_recorder_seconds(const PhoneRecorder *r);   // recorded so far (pushed frames)

#endif
