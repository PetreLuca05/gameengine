// Thermal scope sensor (docs/NIGHT_OPTICS.md "Thermal sensor"): an uncooled microbolometer core.
// The thermal pass renders band radiance (thermal.h) at the core's resolution; this module is the
// camera's processing between that signal and the eyepiece's microdisplay:
//
//   AGC        automatic gain / level: the displayed range follows the scene's radiance range
//              (low / high percentiles, fast attack, slow release), so a muzzle flash or a fire
//              stretches the range and compresses everything else, then it recovers. Or a fixed
//              range (thermal_debug range a b)
//   NUC        non-uniformity correction: the shutter closes every interval (30 s after power-on,
//              growing to 180 s) and on big ambient changes; the image freezes for NUC_FREEZE s
//              with a click, and the fixed-pattern drift resets
//   palettes   256-entry colour maps (assets/thermal/palettes.json): White Hot, Black Hot, Red Hot,
//              Ironbow, Rainbow
//   zoom       digital: a centred crop of the core, nearest-neighbour upscaled (true pixelation)
//
// The per-pixel stages (smear, NETD noise, fixed pattern, lens blur, edge enhancement, OSD) run in
// shaders/post/thermal_sensor.comp with the constants below. Pure (tests/test_night_optics.c).
#ifndef DG_THERMAL_SENSOR_H
#define DG_THERMAL_SENSOR_H

#include "core/base.h"

#define THERMAL_PALETTE_N 256
#define THERMAL_PALETTES 8
#define NUC_FREEZE 0.3f              // s the image holds during a shutter calibration
#define NUC_FIRST 30.0f              // s after power-on
#define NUC_MAX 180.0f               // s: the interval grows to this as the core stabilises
#define THERMAL_SMEAR_TAU 0.012f     // s: bolometer time constant (thermal smear)
#define THERMAL_NETD 0.045f          // C: temporal noise
#define THERMAL_ZOOM_MAX 4

typedef enum ThermalPaletteId { TPAL_WHITE_HOT, TPAL_BLACK_HOT, TPAL_RED_HOT, TPAL_IRONBOW, TPAL_RAINBOW } ThermalPaletteId;

typedef struct ThermalPalette {
    char name[24];
    u8 rgb[THERMAL_PALETTE_N][3];
} ThermalPalette;

typedef struct ThermalPalettes {
    ThermalPalette pal[THERMAL_PALETTES];
    int count;
} ThermalPalettes;

// The five built-in palettes; thermal_palettes_load replaces / adds from JSON
// ({"palettes": [{"name": .., "stops": [[pos, r, g, b], ...]}]}, pos 0..1 cold -> hot, rgb 0..255).
void thermal_palettes_default(ThermalPalettes *p);
bool thermal_palettes_load(ThermalPalettes *p, const char *json_text);
int thermal_palette_find(const ThermalPalettes *p, const char *name);

// Core formats by quality: 0 = 384 x 288, 1 = 640 x 512.
typedef struct ThermalCore { int w, h; float pitch_um; } ThermalCore;
ThermalCore thermal_core(int quality);
// The square region the round scope shows (the core's height), and the crop at a digital zoom.
int thermal_core_square(const ThermalCore *c);
int thermal_zoom_crop(const ThermalCore *c, int zoom);   // core pixels across at zoom 1..4

// AGC.
typedef struct ThermalAgc {
    float lo, hi;                    // radiance range shown (palette 0 .. 255)
    bool valid;
    bool fixed;                      // fixed range (no tracking)
} ThermalAgc;
#define AGC_TAU_UP 0.05f             // s: the range grows fast (a hot source appears)
#define AGC_TAU_DOWN 1.2f            // s: and shrinks slowly
#define AGC_MIN_SPAN_C 2.0f          // C: the range never gets narrower than this around its middle (noise floor)
void thermal_agc_step(ThermalAgc *a, float frame_lo, float frame_hi, float dt);
// Palette index 0..1 of a radiance under the AGC.
float thermal_agc_map(const ThermalAgc *a, float l);

// NUC scheduler.
typedef struct ThermalNuc {
    float on_time;                   // s since power-on
    float since;                     // s since the last calibration
    float interval;                  // s, current
    float freeze;                    // s left of the current freeze (> 0: image held)
    float ambient_ref;               // C at the last calibration
    int count;
} ThermalNuc;
void thermal_nuc_reset(ThermalNuc *n, float ambient);
// Advances; true on the frame a calibration starts (click sound, freeze, FPN reset).
bool thermal_nuc_step(ThermalNuc *n, float dt, float ambient);
// Fixed-pattern drift 0..1 since the last calibration (the shader's FPN amplitude).
float thermal_nuc_drift(const ThermalNuc *n);

// Battery: runtime in hours of real time; returns the new charge 0..1.
float thermal_battery_drain(float charge, float runtime_h, float dt);

#endif
