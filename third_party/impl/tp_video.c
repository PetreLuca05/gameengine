// Phone video capture (src/gameplay/phone/phone_video.c): H.264 encoder (CC0). The MP4 muxer is
// in tp_mp4.c (the two single-file libraries can't share a translation unit).
#define MINIH264_IMPLEMENTATION
#define H264E_MAX_THREADS 0
#define H264E_SVC_API 0
#include "minih264/minih264e.h"
