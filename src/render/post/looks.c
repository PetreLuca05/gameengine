// The five Looks (docs/LOOKS.md): fixed, compiled-in constants. There is no file format and no
// custom look; to change one, tune it live in a dev build (look_tune), print it (look_dump) and
// paste the initializer back here.
//
// Hue knots (LOOK_HUE_KNOTS, opponent hue): 0 red, 45 orange, 90 yellow-green, 135 green, 180 cyan,
// 225 sky blue, 270 blue-violet, 315 magenta. sat_sat knots: input saturation 0, .25, .5, .75, 1.
// Modulator sets: { temp, tint, exposure, sat, contrast, bloom, grain, vignette }.
#include "look.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(LOOK_COUNT == 5, "exactly five looks (docs/LOOKS.md)");

#define DAMAGE_DEFAULT { 1.3f, 0.25f, 0.2f }   // the damage edge tint before Looks existed

static const LookParams k_looks[LOOK_COUNT] = {
    // Realism: the reference. Neutral and photographic; every other look is a departure from it.
    {
        .id = "realism", .name = "Realism",
        .desc = "Neutral and photographic: true sky white balance, natural colour, no lens effects.",
        .exposure_ev = 0.0f, .wb_temp = 0.0f, .wb_tint = 0.0f,
        .contrast = 1.0f, .pivot_ev = 0.0f,
        .lift = { 0, 0, 0 }, .gamma = { 0, 0, 0 }, .gain = { 0, 0, 0 },
        .shadow_end = -2.5f, .highlight_start = 2.0f, .range_soft = 2.0f,
        .rolloff = 0.1f, .rolloff_start = 4.0f,
        .saturation = 1.0f, .vibrance = 0.05f, .skin_protect = 0.8f,
        .hue_hue = { 0, 0, 0, 0, 0, 0, 0, 0 },
        .hue_sat = { 1, 1, 1, 1, 1, 1, 1, 1 },
        .sat_sat = { 1, 1, 1, 1, 1 },
        .bal_shadows = { 0, 0, 0 }, .bal_mids = { 0, 0, 0 }, .bal_highs = { 0, 0, 0 },
        .tonemapper = LOOK_TM_AGX,
        .lut = LOOK_LUT_NONE, .lut_strength = 0.0f,
        .black_point = 0.0f, .white_point = 1.0f, .black_tint = { 1, 1, 1 },
        .bloom = 0.03f, .bloom_threshold = 0.0f, .bloom_tint = { 1, 1, 1 },
        .vignette = 0.0f, .grain = 0.0f, .chromatic = 0.0f, .sharpen = 0.3f, .halation = 0.0f, .letterbox = 0,
        .damage_color = DAMAGE_DEFAULT, .lowhealth_color = { 0, 0, 0 },
    },
    // PUBG-style (inspired by PlayerUnknown's Battlegrounds; our own implementation): muted, milky
    // blacks, low-medium contrast, cool green-grey midtones under slightly warm highlights.
    {
        .id = "pubg", .name = "PUBG-style",
        .desc = "Muted and slightly washed: milky shadows, cool green-grey mids, soft highlights. Made for open terrain.",
        .exposure_ev = 0.6f, .wb_temp = -0.03f, .wb_tint = -0.03f,
        .contrast = 0.78f, .pivot_ev = 0.0f,
        .lift = { 0.4f, 0.4f, 0.4f }, .gamma = { 0.15f, 0.15f, 0.15f }, .gain = { 0, 0, 0 },
        .shadow_end = -2.0f, .highlight_start = 1.5f, .range_soft = 2.0f,
        .rolloff = 0.35f, .rolloff_start = 1.5f,
        .saturation = 0.84f, .vibrance = -0.05f, .skin_protect = 0.8f,
        .hue_hue = { 0, 0, -4.0f, -6.0f, 0, 0, 0, 0 },
        .hue_sat = { 1.0f, 1.0f, 0.9f, 0.88f, 0.92f, 0.9f, 0.95f, 0.95f },
        .sat_sat = { 1.0f, 1.0f, 0.97f, 0.92f, 0.85f },
        .bal_shadows = { -0.01f, 0.0f, 0.012f }, .bal_mids = { -0.025f, 0.015f, 0.0f }, .bal_highs = { 0.025f, 0.008f, -0.03f },
        .tonemapper = LOOK_TM_ACES,
        .lut = LOOK_LUT_NONE, .lut_strength = 0.0f,
        .black_point = 0.008f, .white_point = 0.98f, .black_tint = { 0.95f, 1.0f, 1.03f },
        .bloom = 0.035f, .bloom_threshold = 1.0f, .bloom_tint = { 1.0f, 0.98f, 0.94f },
        .vignette = 0.12f, .grain = 0.015f, .chromatic = 0.0f, .sharpen = 0.1f, .halation = 0.0f, .letterbox = 0,
        .damage_color = { 1.25f, 0.3f, 0.22f }, .lowhealth_color = { 0.05f, 0.0f, 0.0f },
        .golden = { 0.04f, 0.0f, 0.0f, 0.03f, 0.0f, 0.0f, 0.0f, 0.0f },
        .overcast = { -0.03f, 0.0f, 0.0f, -0.15f, -0.1f, 0.0f, 0.0f, 0.0f },
        .storm = { 0.0f, 0.0f, 0.0f, -0.1f, -0.05f, 0.0f, 0.0f, 0.0f },
        .night = { -0.03f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
    },
    // Cinematic: filmic contrast, teal shadows / orange highlights, halation, letterbox.
    {
        .id = "cinematic", .name = "Cinematic",
        .desc = "Filmic: rich contrast, teal shadows and warm highlights, soft halation, letterboxed.",
        .exposure_ev = 0.35f, .wb_temp = 0.0f, .wb_tint = 0.0f,
        .contrast = 1.1f, .pivot_ev = 0.0f,
        .lift = { 0, 0, 0 }, .gamma = { 0, 0, 0 }, .gain = { 0, 0, 0 },
        .shadow_end = -2.0f, .highlight_start = 1.5f, .range_soft = 2.0f,
        .rolloff = 0.25f, .rolloff_start = 2.0f,
        .saturation = 1.0f, .vibrance = 0.08f, .skin_protect = 0.9f,
        .hue_hue = { 0, 0, 4.0f, 8.0f, 0, 0, 0, 0 },
        .hue_sat = { 1.0f, 1.05f, 0.92f, 0.85f, 1.05f, 1.0f, 0.95f, 0.9f },
        .sat_sat = { 1.0f, 1.0f, 1.0f, 0.96f, 0.9f },
        .bal_shadows = { -0.02f, 0.005f, 0.015f }, .bal_mids = { 0, 0, 0 }, .bal_highs = { 0.01f, 0.0f, -0.01f },
        .split_shadow_hue = 190.0f, .split_shadow_amt = 0.14f, .split_high_hue = 30.0f, .split_high_amt = 0.11f, .split_balance = 0.0f,
        .tonemapper = LOOK_TM_FILMIC, .tm_toe = 0.28f, .tm_shoulder = 0.22f,
        .lut = LOOK_LUT_PRINT, .lut_strength = 0.35f,
        .black_point = 0.005f, .white_point = 1.0f, .black_tint = { 0.85f, 1.0f, 1.05f },
        .bloom = 0.07f, .bloom_threshold = 0.8f, .bloom_tint = { 1.0f, 0.92f, 0.82f },
        .vignette = 0.3f, .grain = 0.035f, .chromatic = 0.05f, .sharpen = 0.15f, .halation = 0.35f, .letterbox = 1,
        .damage_color = { 1.2f, 0.2f, 0.18f }, .lowhealth_color = { 0.08f, 0.01f, 0.0f },
        .golden = { 0.12f, 0.0f, 0.1f, 0.08f, 0.05f, 0.6f, 0.0f, 0.0f },
        .overcast = { 0.0f, 0.0f, 0.0f, 0.0f, -0.05f, 0.0f, 0.0f, 0.0f },
        .storm = { 0, 0, 0, 0, 0, 0, 0, 0 },
        .night = { -0.08f, 0.0f, 0.0f, -0.1f, 0.0f, 0.0f, 0.0f, 0.0f },
    },
    // Vibrant: punchy colour and contrast, clean blacks, sharp.
    {
        .id = "vibrant", .name = "Vibrant",
        .desc = "Punchy: saturated greens and sky blues, bright mids, clean blacks, crisp detail.",
        .exposure_ev = 0.7f, .wb_temp = 0.0f, .wb_tint = 0.0f,
        .contrast = 1.0f, .pivot_ev = 0.0f,
        .lift = { 0.3f, 0.3f, 0.3f }, .gamma = { 0.35f, 0.35f, 0.35f }, .gain = { 0.0f, 0.0f, 0.0f },
        .shadow_end = -2.5f, .highlight_start = 1.5f, .range_soft = 2.0f,
        .rolloff = 0.15f, .rolloff_start = 2.5f,
        .saturation = 1.12f, .vibrance = 0.2f, .skin_protect = 0.7f,
        .hue_hue = { 0, 0, 0, 0, 0, 0, 0, 0 },
        .hue_sat = { 1.0f, 1.0f, 1.1f, 1.15f, 1.1f, 1.2f, 1.05f, 1.0f },
        .sat_sat = { 1.0f, 1.05f, 1.05f, 1.0f, 0.95f },
        .bal_shadows = { 0, 0, 0 }, .bal_mids = { 0, 0, 0 }, .bal_highs = { 0, 0, 0 },
        .tonemapper = LOOK_TM_ACES,
        .lut = LOOK_LUT_NONE, .lut_strength = 0.0f,
        .black_point = 0.0f, .white_point = 1.0f, .black_tint = { 1, 1, 1 },
        .bloom = 0.05f, .bloom_threshold = 1.0f, .bloom_tint = { 1, 1, 1 },
        .vignette = 0.05f, .grain = 0.005f, .chromatic = 0.0f, .sharpen = 0.55f, .halation = 0.0f, .letterbox = 0,
        .damage_color = { 1.35f, 0.25f, 0.2f }, .lowhealth_color = { 0, 0, 0 },
        .golden = { 0.06f, 0.0f, 0.0f, 0.05f, 0.0f, 0.0f, 0.0f, 0.0f },
        .overcast = { 0.0f, 0.0f, 0.0f, 0.08f, 0.03f, 0.0f, 0.0f, 0.0f },
        .storm = { 0, 0, 0, 0, 0, 0, 0, 0 },
        .night = { 0, 0, 0, 0, 0, 0, 0, 0 },
    },
    // Gritty: desaturated and harsh, warm reds / oranges kept, bleached highlights, heavy grain.
    {
        .id = "gritty", .name = "Gritty",
        .desc = "Harsh and drained: only fire and blood keep their colour, dark shadows, heavy grain. Storms make it worse.",
        .exposure_ev = 0.1f, .wb_temp = -0.08f, .wb_tint = 0.0f,
        .contrast = 1.2f, .pivot_ev = -0.3f,
        .lift = { -0.15f, -0.12f, -0.1f }, .gamma = { 0, 0, 0 }, .gain = { 0, 0, 0 },
        .shadow_end = -1.5f, .highlight_start = 1.5f, .range_soft = 2.0f,
        .rolloff = 0.2f, .rolloff_start = 1.5f,
        .saturation = 0.45f, .vibrance = -0.1f, .skin_protect = 0.0f,
        .hue_hue = { 0, 0, 0, 0, 0, 0, 0, 0 },
        .hue_sat = { 2.1f, 1.8f, 0.8f, 0.7f, 0.6f, 0.55f, 0.6f, 1.4f },
        .sat_sat = { 1, 1, 1, 1, 1 },
        .bal_shadows = { -0.015f, 0.0f, 0.02f }, .bal_mids = { -0.01f, 0.0f, 0.01f }, .bal_highs = { 0, 0, 0 },
        .tonemapper = LOOK_TM_ACES,
        .lut = LOOK_LUT_BLEACH, .lut_strength = 0.35f,
        .black_point = 0.0f, .white_point = 0.97f, .black_tint = { 1, 1, 1 },
        .bloom = 0.035f, .bloom_threshold = 1.2f, .bloom_tint = { 1.0f, 0.95f, 0.9f },
        .vignette = 0.45f, .grain = 0.12f, .chromatic = 0.25f, .sharpen = 0.45f, .halation = 0.0f, .letterbox = 0,
        .damage_color = { 1.4f, 0.15f, 0.12f }, .lowhealth_color = { 0.12f, 0.0f, 0.0f },
        .golden = { 0.03f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
        .overcast = { 0.0f, 0.0f, 0.0f, -0.1f, 0.05f, 0.0f, 0.0f, 0.0f },
        .storm = { -0.06f, 0.0f, -0.15f, -0.25f, 0.12f, 0.0f, 0.6f, 0.1f },
        .night = { -0.04f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
    },
};

const LookParams *look_preset(int id) { return id >= 0 && id < LOOK_COUNT ? &k_looks[id] : NULL; }

#if DG_LOOK_TUNE
// Dev builds only: live overrides of the constants (look_tune), never saved anywhere.
static LookParams s_tuned[LOOK_COUNT];
static bool s_tuned_on[LOOK_COUNT];

LookParams *look_tune_edit(int id) {
    if (id < 0 || id >= LOOK_COUNT) return NULL;
    if (!s_tuned_on[id]) {
        s_tuned[id] = k_looks[id];
        s_tuned_on[id] = true;
    }
    return &s_tuned[id];
}

void look_tune_reset(int id) {
    for (int i = 0; i < LOOK_COUNT; i++)
        if (id < 0 || id == i) s_tuned_on[i] = false;
}

bool look_tune_dirty(int id) { return id >= 0 && id < LOOK_COUNT && s_tuned_on[id] && memcmp(&s_tuned[id], &k_looks[id], sizeof(LookParams)) != 0; }

const LookParams *look_get(int id) {
    if (id < 0 || id >= LOOK_COUNT) return NULL;
    return s_tuned_on[id] ? &s_tuned[id] : &k_looks[id];
}
#else
const LookParams *look_get(int id) { return look_preset(id); }
#endif

static bool same_name(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    return *a == *b;
}

int look_find(const char *name) {
    if (!name || !name[0]) return -1;
    for (int i = 0; i < LOOK_COUNT; i++)
        if (same_name(name, k_looks[i].id) || same_name(name, k_looks[i].name)) return i;
    if (name[0] >= '0' && name[0] <= '9' && !name[1]) {
        int i = atoi(name);
        return i < LOOK_COUNT ? i : -1;
    }
    return -1;
}

const char *look_id_name(int id) { return id >= 0 && id < LOOK_COUNT ? k_looks[id].id : "?"; }
const char *look_display_name(int id) { return id >= 0 && id < LOOK_COUNT ? k_looks[id].name : "?"; }
const char *look_desc(int id) { return id >= 0 && id < LOOK_COUNT ? k_looks[id].desc : ""; }
