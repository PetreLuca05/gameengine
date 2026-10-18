// Sun shadow caster cascade limits (docs/SHADOWS.md "Caster cascade limits", src/render/shadow_casters.c,
// docs/perf/FINDINGS.md PF-0013):
//   table      assets/render/shadow_casters.json equals scc_defaults; malformed tables are rejected
//   selection  per mode / cascade count / cascade: which classes cast, where they end, the wide-mode
//              shift (the reach in metres stays), mode 0 / unknown modes limit nothing
//   fade       the caster weight is 1 before the fade, falls monotonically and continuously to 0 at
//              the end, 0 beyond it and in cascades the class does not cast into
//   tiers      every Engine Fidelity level's default mode (assets/render/fidelity/*.json) with its
//              cascade layout (shs_quality_for_tier): ground cover ends within 8 .. 16 m and small
//              plants within 25 .. 31 m at every level (fair: about the same reach for everyone),
//              trees are never limited
//   merge      a mesh takes the least limited class of its materials
#include "render/shadow_casters.h"
#include "render/shadow_settings.h"
#include "cJSON.h"
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

static bool near_(float a, float b, float eps) { return fabsf(a - b) <= eps; }

static char *load_text(const char *rel) {
    static const char *pre[] = { "", "../", "../../" };
    for (int i = 0; i < 3; i++) {
        char p[256];
        snprintf(p, sizeof(p), "%s%s", pre[i], rel);
        FILE *f = fopen(p, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *s = malloc((size_t)n + 1);
        size_t got = fread(s, 1, (size_t)n, f);
        s[got] = 0;
        fclose(f);
        return s;
    }
    return NULL;
}

static void test_table(void) {
    SccTable d, j;
    scc_defaults(&d);
    char *s = load_text("assets/render/shadow_casters.json");
    CHECK(s != NULL, "load assets/render/shadow_casters.json");
    if (s) {
        char err[128] = "";
        CHECK(scc_parse(&j, s, err, sizeof(err)), "parse: %s", err);
        CHECK(near_(j.fade, d.fade, 1e-6f), "fade %g = default %g", j.fade, d.fade);
        for (int m = 0; m < SCC_MODES; m++)
            for (int c = 0; c < SCC_COUNT; c++)
                CHECK(j.lim[m][c].cascade == d.lim[m][c].cascade && near_(j.lim[m][c].max_m, d.lim[m][c].max_m, 1e-6f), "mode %d %s: JSON = scc_defaults", m, scc_class_name(c));
        free(s);
    }
    char err[128];
    CHECK(!scc_parse(&j, "{\"modes\":{\"1\":{\"grass\":{\"cascade\":0}}}}", err, sizeof(err)) && strstr(err, "grass"), "unknown class: %s", err);
    CHECK(!scc_parse(&j, "{\"modes\":{\"3\":{}}}", err, sizeof(err)), "unknown mode: %s", err);
    CHECK(!scc_parse(&j, "{\"modes\":{\"0\":{}}}", err, sizeof(err)), "mode 0 is fixed (no limit): %s", err);
    CHECK(!scc_parse(&j, "{\"modes\":{\"1\":{\"ground\":{\"cascades\":0}}}}", err, sizeof(err)) && strstr(err, "cascades"), "unknown key: %s", err);
    CHECK(!scc_parse(&j, "{\"modes\":{\"1\":{\"ground\":{\"cascade\":4}}}}", err, sizeof(err)), "cascade range: %s", err);
    CHECK(!scc_parse(&j, "{\"fade\":2}", err, sizeof(err)), "fade range: %s", err);
    CHECK(!scc_parse(&j, "[1]", err, sizeof(err)), "not an object: %s", err);
    CHECK(scc_parse(&j, "{\"modes\":{\"2\":{\"tree\":{\"max_m\":80}}}}", err, sizeof(err)) && near_(j.lim[2][SCC_TREE].max_m, 80.0f, 1e-6f) && j.lim[1][SCC_GROUND].cascade == 0,
          "a partial table overrides the defaults");
    CHECK(scc_class_from_name("small") == SCC_SMALL && scc_class_from_name("bush") < 0 && !strcmp(scc_class_name(SCC_GROUND), "ground"), "class names");
}

static void test_selection(void) {
    SccTable t;
    scc_defaults(&t);
    const float s4[4] = { 10.05f, 30.0f, 70.05f, 150.0f }, s2[2] = { 37.5f, 150.0f }, s1[1] = { 150.0f };
    const float border = 0.12f;
    SccView v;
    // mode 0 and unknown modes: nothing limited
    for (int mode = -1; mode <= 3; mode += 4)
        for (int k = 0; k < 4; k++) {
            scc_view(&t, mode == -1 ? 0 : mode, k, 4, s4, false, border, &v);
            for (int c = 0; c < SCC_COUNT; c++) CHECK(scc_alpha(&v, c, 149.0f) == 1.0f, "mode %d cascade %d %s: unlimited", mode, k, scc_class_name(c));
        }
    // mode 1, four cascades: ground cascade 0 to split 0, small 0 - 1 to split 1, trees / none everywhere
    for (int k = 0; k < 4; k++) {
        scc_view(&t, 1, k, 4, s4, false, border, &v);
        CHECK(((v.allow >> SCC_GROUND) & 1u) == (k == 0), "mode 1 cascade %d: ground %s", k, k == 0 ? "casts" : "culled");
        CHECK(((v.allow >> SCC_SMALL) & 1u) == (k <= 1), "mode 1 cascade %d: small", k);
        CHECK((v.allow >> SCC_TREE & 1u) && (v.allow >> SCC_NONE & 1u) && v.end_m[SCC_TREE] == 0.0f && v.end_m[SCC_NONE] == 0.0f, "mode 1 cascade %d: trees / unclassified unlimited", k);
        if (k == 0) CHECK(near_(v.end_m[SCC_GROUND], 10.05f, 1e-4f) && near_(v.end_m[SCC_SMALL], 30.0f, 1e-4f), "mode 1 ends at the splits");
        if (k == 1) CHECK(scc_alpha(&v, SCC_GROUND, 1.0f) == 0.0f && scc_alpha(&v, SCC_SMALL, 5.0f) == 1.0f, "mode 1 cascade 1: no ground, small near");
        CHECK(scc_mask_end(&v, 1u << SCC_TREE | 1u << SCC_GROUND) == 0.0f, "a cell with a tree is unlimited");
        CHECK(near_(scc_mask_end(&v, 1u << SCC_GROUND), k == 0 ? 10.05f : -1.0f, 1e-4f), "a ground-only cell: end %g at cascade %d", scc_mask_end(&v, 1u << SCC_GROUND), k);
    }
    // wide mode (cascade 0 fading into 1): the limited classes also cast into the next cascade, same reach
    scc_view(&t, 1, 1, 4, s4, true, border, &v);
    CHECK((v.allow >> SCC_GROUND & 1u) && near_(v.end_m[SCC_GROUND], 10.05f, 1e-4f), "wide: ground in cascade 1, still to split 0");
    scc_view(&t, 1, 2, 4, s4, true, border, &v);
    CHECK(!(v.allow >> SCC_GROUND & 1u) && (v.allow >> SCC_SMALL & 1u) && near_(v.end_m[SCC_SMALL], 30.0f, 1e-4f), "wide: small in cascade 2 to split 1, no ground");
    // mode 1 with one or two cascades: a limit at or past the last cascade is no limit
    scc_view(&t, 1, 0, 1, s1, false, border, &v);
    CHECK(v.end_m[SCC_GROUND] == 0.0f && v.end_m[SCC_SMALL] == 0.0f && v.allow == 15u, "mode 1, one cascade: unlimited");
    scc_view(&t, 1, 1, 2, s2, false, border, &v);
    CHECK(!(v.allow >> SCC_GROUND & 1u) && (v.allow >> SCC_SMALL & 1u) && v.end_m[SCC_SMALL] == 0.0f, "mode 1, two cascades: small unlimited");
    // mode 2: cascade 0 and the caps
    scc_view(&t, 2, 0, 2, s2, false, border, &v);
    CHECK(near_(v.end_m[SCC_GROUND], 15.0f, 1e-4f) && near_(v.end_m[SCC_SMALL], 30.0f, 1e-4f), "mode 2, two cascades: the caps");
    scc_view(&t, 2, 1, 2, s2, false, border, &v);
    CHECK(!(v.allow >> SCC_GROUND & 1u) && !(v.allow >> SCC_SMALL & 1u) && (v.allow >> SCC_TREE & 1u), "mode 2 cascade 1: trees only");
    scc_view(&t, 2, 0, 1, s1, false, border, &v);
    CHECK(near_(v.end_m[SCC_GROUND], 15.0f, 1e-4f) && near_(v.end_m[SCC_SMALL], 30.0f, 1e-4f) && v.end_m[SCC_TREE] == 0.0f, "mode 2, one cascade: the caps only");
    // a capped class that ends before a cascade's receivers start writes nothing there
    SccTable c = t;
    c.lim[2][SCC_TREE] = (SccLimit){ -1, 20.0f };
    scc_view(&c, 2, 1, 2, s2, false, border, &v);
    CHECK(!(v.allow >> SCC_TREE & 1u), "a 20 m cap: nothing in cascade 1 (receivers from 33 m)");
    scc_view(&c, 2, 1, 2, s2, true, border, &v);
    CHECK((v.allow >> SCC_TREE & 1u), "wide: cascade 1 serves the near field, the capped class casts");
}

static void test_fade(void) {
    SccTable t;
    scc_defaults(&t);
    const float s4[4] = { 10.05f, 30.0f, 70.05f, 150.0f };
    SccView v;
    scc_view(&t, 1, 0, 4, s4, false, 0.12f, &v);
    for (int c = SCC_GROUND; c <= SCC_SMALL; c++) {
        float e = v.end_m[c], f0 = e * (1.0f - t.fade), prev = 1.0f, worst = 0.0f;
        CHECK(scc_alpha(&v, c, 0.0f) == 1.0f && scc_alpha(&v, c, f0 - 0.01f) == 1.0f, "%s: opaque before the fade (%g m)", scc_class_name(c), f0);
        CHECK(scc_alpha(&v, c, e) == 0.0f && scc_alpha(&v, c, e + 5.0f) == 0.0f, "%s: gone from %g m", scc_class_name(c), e);
        for (float d = 0.0f; d <= e + 1.0f; d += 0.01f) {
            float a = scc_alpha(&v, c, d);
            CHECK(a <= prev + 1e-6f, "%s: monotonic at %g", scc_class_name(c), d);
            worst = fmaxf(worst, prev - a);
            prev = a;
        }
        CHECK(worst < 0.01f / (e * t.fade) + 1e-4f, "%s: continuous (largest step %g per cm)", scc_class_name(c), worst);
    }
    CHECK(scc_alpha(&v, SCC_TREE, 140.0f) == 1.0f, "trees: no fade");
}

static int feat_default(cJSON *j, const char *f) {
    const cJSON *e = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(j, "features"), f);
    const cJSON *d = cJSON_GetObjectItemCaseSensitive(e, "default");
    return cJSON_IsNumber(d) ? (int)d->valuedouble : -1;
}

static void test_tiers(void) {
    static const char *files[5] = { "off.json", "low.json", "medium.json", "high.json", "modern.json" };
    SccTable t;
    scc_defaults(&t);
    for (int l = 0; l < 5; l++) {
        char path[96];
        snprintf(path, sizeof(path), "assets/render/fidelity/%s", files[l]);
        char *s = load_text(path);
        CHECK(s != NULL, "load %s", path);
        if (!s) continue;
        cJSON *j = cJSON_Parse(s);
        free(s);
        int mode = feat_default(j, "shadow_caster_limit");
        cJSON_Delete(j);
        CHECK(mode >= 1 && mode < SCC_MODES, "%s: shadow_caster_limit default %d (every level limits)", files[l], mode);
        QualityShadowSettings q;
        shs_quality_for_tier(&q, l);
        float splits[4];
        int n = shs_cascade_splits(&q, splits);
        float reach[SCC_COUNT] = { 0 };
        for (int k = 0; k < n; k++) {
            SccView v;
            scc_view(&t, mode, k, n, splits, false, q.border, &v);
            for (int c = 0; c < SCC_COUNT; c++)
                if (v.allow >> c & 1u) reach[c] = v.end_m[c] > 0.0f ? fmaxf(reach[c], v.end_m[c]) : 1e9f;
        }
        printf("  %-11s mode %d, %d cascades: ground to %.1f m, small to %.1f m, trees %s\n", files[l], mode, n, reach[SCC_GROUND], reach[SCC_SMALL], reach[SCC_TREE] >= 1e9f ? "unlimited" : "LIMITED");
        CHECK(reach[SCC_GROUND] >= 8.0f && reach[SCC_GROUND] <= 16.0f, "%s: ground cover ends at %g m (8 .. 16)", files[l], reach[SCC_GROUND]);
        CHECK(reach[SCC_SMALL] >= 25.0f && reach[SCC_SMALL] <= 31.0f, "%s: small plants end at %g m (25 .. 31)", files[l], reach[SCC_SMALL]);
        CHECK(reach[SCC_TREE] >= 1e9f && reach[SCC_NONE] >= 1e9f, "%s: trees / unclassified unlimited", files[l]);
    }
}

static void test_merge(void) {
    CHECK(scc_merge(SCC_GROUND, SCC_SMALL) == SCC_SMALL && scc_merge(SCC_SMALL, SCC_GROUND) == SCC_SMALL, "ground + small = small");
    CHECK(scc_merge(SCC_TREE, SCC_SMALL) == SCC_TREE && scc_merge(SCC_GROUND, SCC_NONE) == SCC_NONE && scc_merge(SCC_NONE, SCC_TREE) == SCC_NONE, "the least limited wins");
    CHECK(scc_merge(SCC_GROUND, SCC_GROUND) == SCC_GROUND, "same class");
}

int main(void) {
    test_table();
    test_selection();
    test_fade();
    test_tiers();
    test_merge();
    printf("test_shadow_casters: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
