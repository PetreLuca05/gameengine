// Phone video capture (src/gameplay/phone/phone_video.c): H.264 encoder (CC0). The MP4 muxer is
// in tp_mp4.c (the two single-file libraries can't share a translation unit).
#if defined(__APPLE__) && defined(__aarch64__)
// minih264's Apple arm64 path calls vtbl2q_u8, which no arm_neon.h declares: the AArch64 two-register
// table lookup it means is vqtbl2_u8 (uint8x16x2_t table, uint8x8_t indices).
#include <arm_neon.h>
#define vtbl2q_u8 vqtbl2_u8
#endif
#define MINIH264_IMPLEMENTATION
#define H264E_MAX_THREADS 0
#define H264E_SVC_API 0
#include "minih264/minih264e.h"
