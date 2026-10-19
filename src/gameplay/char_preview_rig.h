// Inventory character preview (docs/CHARACTERS.md "Character preview"): its look and its framing.
// Pure C (cJSON): tests/test_char_preview.c.
//
// The look is assets/ui/char_preview.json over the built-in defaults: the framing (where the head top
// and the image bottom fall, the lens), the studio light rig in camera space (key / fill directions,
// rim lights around the chest, hemispheric ambient: independent of the world's time of day, weather
// and shadows), the exposure / curve / saturation and the supersampling.
//
// Framing: a level camera at chest height. For points at the character's plane (depth `dist`) a level
// perspective camera maps height linearly onto the image, so the head top (the skeleton's head-top
// bone) lands at `head_top` of the image height from the top and the image bottom cuts the legs at
// `bottom_t` between the hip joints (0) and the knees (1) whatever the skin or the character scale.
#ifndef DG_CHAR_PREVIEW_RIG_H
#define DG_CHAR_PREVIEW_RIG_H

#include "core/base.h"
#include "core/dgmath.h"

typedef struct CharPreviewRig {
    // framing
    float head_top;           // image height above the head top (fraction, 0.21 = the reference shot)
    float crown_m;            // the head top: the head-top bone + this (m x the character scale: skull / hair above the bone)
    float bottom_t;           // image bottom: 0 hip joints .. 1 knees (0.5 = mid-thigh; 0.7 frames the reference shot's idle)
    float fov_deg;            // vertical field of view (a longish lens: mild perspective)
    float turn_deg;           // the character's default turn about the vertical (+ = its left side to the camera)
    float square;             // 0..1: how much of the idle stance's own turn is undone (shoulders square to the camera)
    // light rig, camera space (x right, y up, z towards the camera); colours linear, x intensity
    vec3 key_dir, key_color;
    float key;
    vec3 fill_dir, fill_color;
    float fill;
    vec3 rim_pos[2];          // point lights: m from the chest point (camera space)
    vec3 rim_color;
    float rim, rim_radius;
    vec3 sky, ground;         // hemispheric ambient (up / down; also the reflections)
    float ambient;
    // output
    float exposure;           // linear multiplier before the curve
    int tonemap;              // 0 Khronos PBR Neutral, 1 ACES (fitted)
    float saturation;         // 1 = as lit
    int supersample;          // samples per side and output pixel (1 .. 4)
    bool held_low;            // a held item in the low ready (sprint hold), not aimed at the camera
} CharPreviewRig;

void char_preview_rig_default(CharPreviewRig *r);
// Parses a char_preview.json over *r (missing keys keep their value). false + err on a parse error.
bool char_preview_rig_parse(CharPreviewRig *r, const char *json, char *err, size_t err_size);

// A level camera framing the character: its height (m above the feet), its distance to the character's
// plane (m) and its vertical field of view (rad).
typedef struct CharPreviewShot { float cam_y, dist, fov_rad; } CharPreviewShot;
// head_top_y / bottom_y: the head-top bone and the image bottom (m above the feet).
CharPreviewShot char_preview_frame(const CharPreviewRig *r, float head_top_y, float bottom_y);
// The image bottom's height from the hip and knee heights (m above the feet).
float char_preview_bottom_y(const CharPreviewRig *r, float hip_y, float knee_y);
// Where a height at the character's plane lands in the image (0 top .. 1 bottom).
float char_preview_image_y(const CharPreviewShot *s, float y);

#endif
