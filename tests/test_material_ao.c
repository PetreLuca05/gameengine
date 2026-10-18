// Per-material ambient occlusion (src/render/material_ao.h, docs/RENDERING_POST.md "Foliage-aware AO"):
//   byte      the AO byte: zero = full AO / multiply / opaque (zero-initialised materials keep the old look),
//             strength quantisation, thin / min / vertex bits, the emissive word keeps its rgb
//   defaults  ao_strength 0 = automatic (1 opaque, 0.3 foliage), MATERIAL_AO_OFF = none, combine auto
//   vgeo      vg_material_set_ao / vg_material_ao_check: the thin bit follows VGM_MASKED (bark is not thin),
//             an RGBA8 alpha of 255 on an opaque material is repaired to the default
//   composite the lit pass' formula (lit_body.glsl): lerp(1, ao, s), then min / multiply with the baked AO
#include "render/material_ao.h"
#include "render/vgeo/vgeo_gpu.h"
#include <math.h>
#include <stdio.h>
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

static bool near_(float a, float b, float eps) { return fabsf(a - b) <= eps; }

// lit_body.glsl: the AO the indirect light gets.
static float composite(float ss, float intensity, u32 b, float baked) {
    float s = 1.0f + (ss - 1.0f) * intensity;
    s = 1.0f + (s - 1.0f) * material_ao_byte_weight(b);
    return (b & MAT_AO_BYTE_MIN) ? fminf(s, baked) : s * baked;
}

static void test_byte(void) {
    CHECK(material_ao_byte(0.0f, MAT_AO_COMBINE_AUTO, false, false) == 0, "default opaque byte is 0");
    CHECK(material_ao_byte(1.0f, MAT_AO_COMBINE_MULTIPLY, false, false) == 0, "explicit 1 / multiply is 0");
    for (int i = 0; i <= 20; i++) {
        float s = (float)i / 20.0f;
        u32 b = material_ao_byte(s > 0 ? s : MATERIAL_AO_OFF, MAT_AO_COMBINE_AUTO, false, false);
        CHECK(near_(material_ao_byte_weight(b), s, 0.5f / 31.0f + 1e-5f), "strength %.2f -> %.3f", s, material_ao_byte_weight(b));
        CHECK((b & ~MAT_AO_BYTE_STRENGTH) == 0, "opaque: only strength bits");
    }
    u32 f = material_ao_byte(0.0f, MAT_AO_COMBINE_AUTO, true, true);
    CHECK((f & MAT_AO_BYTE_THIN) && (f & MAT_AO_BYTE_MIN) && (f & MAT_AO_BYTE_VERTEX), "foliage: thin + min + vertex");
    CHECK(near_(material_ao_byte_weight(f), MATERIAL_AO_FOLIAGE, 0.02f), "foliage default 0.3 (%.3f)", material_ao_byte_weight(f));
    CHECK(!(material_ao_byte(0.0f, MAT_AO_COMBINE_MULTIPLY, false, true) & MAT_AO_BYTE_MIN), "foliage multiply");
    CHECK(material_ao_byte(0.0f, MAT_AO_COMBINE_MIN, false, false) & MAT_AO_BYTE_MIN, "opaque min");
    CHECK(material_ao_word(0xFF336699u, 0x45u) == 0x45336699u, "word keeps rgb");
    CHECK(material_ao_word(0x00336699u, 0x1FFu) == 0xFF336699u, "byte masked to 8 bits");
}

static void test_defaults(void) {
    CHECK(material_ao_weight(0.0f, false) == 1.0f && near_(material_ao_weight(0.0f, true), 0.3f, 1e-6f), "auto weights");
    CHECK(material_ao_weight(MATERIAL_AO_OFF, true) == 0.0f && material_ao_weight(2.0f, false) == 1.0f, "off / clamp");
    CHECK(material_ao_combine_parse("min") == MAT_AO_COMBINE_MIN && material_ao_combine_parse("multiply") == MAT_AO_COMBINE_MULTIPLY, "parse");
    CHECK(material_ao_combine_parse("auto") == MAT_AO_COMBINE_AUTO && material_ao_combine_parse(NULL) == MAT_AO_COMBINE_AUTO, "parse auto");
}

static void test_vgeo(void) {
    VgMaterial leaf, bark;
    memset(&leaf, 0, sizeof(leaf));
    memset(&bark, 0, sizeof(bark));
    leaf.flags = VGM_FOLIAGE | VGM_MASKED | VGM_TWO_SIDED;
    bark.flags = VGM_FOLIAGE | VGM_DITHER_FADE;
    leaf.emissive_rgb = bark.emissive_rgb = 0xFFFFFFFFu;   // an RGBA8 colour with alpha 255 (assets.c, material_color)
    vg_material_ao_check(&leaf);
    vg_material_ao_check(&bark);
    u32 lb = leaf.emissive_rgb >> 24, bb = bark.emissive_rgb >> 24;
    CHECK((lb & MAT_AO_BYTE_THIN) && (lb & MAT_AO_BYTE_MIN) && !(lb & MAT_AO_BYTE_VERTEX), "leaf repaired to the foliage default (%02x)", lb);
    CHECK(bb == 0, "bark repaired to the opaque default (%02x)", bb);
    CHECK((leaf.emissive_rgb & 0xFFFFFFu) == 0xFFFFFFu, "rgb kept");
    vg_material_set_ao(&leaf, 0.6f, MAT_AO_COMBINE_MULTIPLY, true);
    u32 before = leaf.emissive_rgb;
    vg_material_ao_check(&leaf);
    CHECK(leaf.emissive_rgb == before, "an explicit foliage setting survives the check");
    lb = leaf.emissive_rgb >> 24;
    CHECK(near_(material_ao_byte_weight(lb), 0.6f, 0.02f) && !(lb & MAT_AO_BYTE_MIN) && (lb & MAT_AO_BYTE_VERTEX), "explicit 0.6 multiply vertex");
}

static void test_composite(void) {
    u32 opaque = 0, fol = material_ao_byte(0.0f, MAT_AO_COMBINE_AUTO, false, true);
    CHECK(near_(composite(0.4f, 1.0f, opaque, 1.0f), 0.4f, 1e-6f), "opaque: the raw AO");
    CHECK(near_(composite(0.4f, 1.0f, opaque, 0.5f), 0.2f, 1e-6f), "opaque: x baked");
    float w = material_ao_byte_weight(fol);
    CHECK(near_(composite(0.4f, 1.0f, fol, 1.0f), 1.0f - 0.6f * w, 1e-5f), "foliage: lerp(1, ao, 0.3)");
    CHECK(near_(composite(0.4f, 1.0f, fol, 0.5f), fminf(1.0f - 0.6f * w, 0.5f), 1e-5f), "foliage: min with baked");
    CHECK(near_(composite(0.0f, 0.5f, opaque, 1.0f), 0.5f, 1e-6f), "intensity 0.5 halves it");
    CHECK(composite(1.0f, 1.0f, fol, 1.0f) == 1.0f, "no occlusion stays 1");
}

int main(void) {
    test_byte();
    test_defaults();
    test_vgeo();
    test_composite();
    printf("material_ao: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
