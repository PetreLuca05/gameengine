// Unit tests for the inventory character preview's look and framing (gameplay/char_preview_rig.c,
// docs/CHARACTERS.md "Character preview"): the head top and the image bottom land where the rig says
// for any character height / scale, the camera looks from chest height, the JSON overrides the defaults
// key by key, and the shipped assets/ui/char_preview.json parses.
// Build: part of the normal CMake build (target test_char_preview); run: ctest or build/bin/test_char_preview.
#include "gameplay/char_preview_rig.h"
#include "core/platform.h"
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

static void test_framing(void) {
    CharPreviewRig r;
    char_preview_rig_default(&r);
    // Heights of a 1.8 m skeleton (head top, hips, knees) and of the same at 0.85 / 1.2 scale.
    const float scales[3] = { 1.0f, 0.85f, 1.2f };
    for (int i = 0; i < 3; i++) {
        float k = scales[i], head = 1.797f * k, hip = 0.93f * k, knee = 0.5f * k;
        float bottom = char_preview_bottom_y(&r, hip, knee);
        CHECK(fabsf(bottom - (hip + (knee - hip) * r.bottom_t)) < 1e-4f && bottom < hip && bottom > knee, "on the thigh: %.4f", bottom);
        CharPreviewShot s = char_preview_frame(&r, head, bottom);
        CHECK(fabsf(char_preview_image_y(&s, head) - r.head_top) < 1e-4f, "scale %.2f: head top at %.4f", k, char_preview_image_y(&s, head));
        CHECK(fabsf(char_preview_image_y(&s, bottom) - 1.0f) < 1e-4f, "scale %.2f: bottom at %.4f", k, char_preview_image_y(&s, bottom));
        // A level camera between the chest and the shoulders (the image centre), a longish lens.
        CHECK(s.cam_y > hip && s.cam_y < head, "camera height %.3f", s.cam_y);
        CHECK(fabsf(char_preview_image_y(&s, s.cam_y) - 0.5f) < 1e-5f, "the camera height is the image centre");
        CHECK(s.fov_rad > 20.0f * DG_DEG2RAD && s.fov_rad < 40.0f * DG_DEG2RAD, "fov %.1f deg", s.fov_rad * DG_RAD2DEG);
        CHECK(s.dist > 1.5f * k && s.dist < 6.0f * k, "distance %.2f m", s.dist);
    }
    // The reference shot (1080 px): head top at y 230, the belt (~ the hip joints) near y 850.
    float bottom = char_preview_bottom_y(&r, 0.93f, 0.5f);
    CharPreviewShot s = char_preview_frame(&r, 1.797f, bottom);
    float belt = char_preview_image_y(&s, 0.93f) * 1080.0f;
    CHECK(belt > 790.0f && belt < 900.0f, "hips at y %.0f of 1080", belt);
}

static void test_parse(void) {
    CharPreviewRig r;
    char_preview_rig_default(&r);
    char err[160];
    const char *js = "{ \"framing\": { \"head_top\": 0.25, \"fov_deg\": 32 }, \"lights\": { \"key\": { \"intensity\": 3.5 },"
                     " \"rim\": { \"pos\": [[1, 2, 3], [4, 5, 6]] } }, \"output\": { \"tonemap\": \"aces\", \"supersample\": 9 }, \"held\": { \"pose\": \"aim\" } }";
    CHECK(char_preview_rig_parse(&r, js, err, sizeof(err)), "parse: %s", err);
    CHECK(fabsf(r.head_top - 0.25f) < 1e-6f && fabsf(r.fov_deg - 32.0f) < 1e-6f, "framing keys");
    CHECK(fabsf(r.bottom_t - 0.7f) < 1e-6f, "missing keys keep the default");
    CHECK(fabsf(r.key - 3.5f) < 1e-6f && fabsf(r.key_dir.y - 0.6f) < 1e-6f, "a light's keys one by one");
    CHECK(r.rim_pos[1].z == 6.0f && r.rim_pos[0].x == 1.0f, "rim positions");
    CHECK(r.tonemap == 1 && r.supersample == 4 && !r.held_low, "output: aces, supersample clamped to 4, aimed hold");
    CHECK(!char_preview_rig_parse(&r, "{ broken", err, sizeof(err)) && err[0], "a parse error is reported");
    // The shipped look.
    char *text = platform_read_file("assets/ui/char_preview.json", NULL);
    CHECK(text != NULL, "assets/ui/char_preview.json present (run from build/bin)");
    if (text) {
        char_preview_rig_default(&r);
        CHECK(char_preview_rig_parse(&r, text, err, sizeof(err)), "shipped look: %s", err);
        CHECK(r.head_top > 0.1f && r.head_top < 0.35f && r.bottom_t > 0.2f && r.bottom_t < 0.8f, "shipped framing: head to mid-thigh");
        CHECK(r.fov_deg >= 25.0f && r.fov_deg <= 35.0f, "shipped lens %.1f deg", r.fov_deg);
        free(text);
    }
}

int main(void) {
    test_framing();
    test_parse();
    printf("char_preview: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
