// Ground reference viewer (docs/GROUND_REFERENCE.md "Comparing"): the reference shots in the game beside
// or over our frame, camera presets that frame the ground like each shot, and a colour sampler.
//   ground_ref <r1..r5|off> [side|overlay|flip] [alpha]   reference image (docs/reference/ground/view/<r>.jpg):
//                                                       side = right half, overlay = full frame at alpha,
//                                                       flip = alternates every second
//   ground_ref_cam <r1..r5>                               map / pose / FOV / time / weather of the shot
//                                                       (docs/reference/ground/views.json; host)
//   ground_sample <x0> <y0> <x1> <y1> [label]             mean / spread HSV of a screen box (fractions):
//                                                       captures the next frame, logs the statistics
// tools/ground_match.py turns screenshots of the presets into docs/ground_match/report.md.
#include "game_internal.h"
#include "render/render.h"
#include "ui/ui.h"
#include "ui/gui/gui_draw.h"
#include "core/log.h"
#include "core/engine.h"
#include "core/platform.h"
#include "cJSON.h"
#include "stb_image.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    TextureId tex;
    int w, h, mode;          // mode 0 off, 1 side, 2 overlay, 3 flip
    float alpha;
    char name[8];
    // ground_sample: pending capture
    int sample_frames;
    float box[4];
    char label[48], path[512];
} GR;

static void cmd_ref(int argc, const char **argv) {
    if (argc < 2 || !strcmp(argv[1], "off")) { GR.mode = 0; return; }
    char rel[96], path[512];
    snprintf(rel, sizeof(rel), "docs/reference/ground/view/%s.jpg", argv[1]);
    int w, h, comp;
    u8 *px = stbi_load(platform_data_path(rel, path, sizeof(path)), &w, &h, &comp, 4);
    if (!px) { log_info("ground_ref: %s missing (py tools/ground_ref.py writes it)", rel); return; }
    if (GR.tex) render_texture_destroy(GR.tex);
    GR.tex = render_texture_create(w, h, px, true, false);
    stbi_image_free(px);
    GR.w = w, GR.h = h;
    dg_strcpy(GR.name, sizeof(GR.name), argv[1]);
    GR.mode = 1;
    if (argc > 2) GR.mode = !strcmp(argv[2], "overlay") ? 2 : (!strcmp(argv[2], "flip") ? 3 : 1);
    GR.alpha = argc > 3 ? (float)atof(argv[3]) : 0.5f;
}

static void cmd_ref_cam(int argc, const char **argv) {
    if (argc < 2) { log_info("usage: ground_ref_cam <r1..r5>"); return; }
    char path[512];
    size_t sz;
    char *text = platform_read_file(platform_data_path("docs/reference/ground/views.json", path, sizeof(path)), &sz);
    cJSON *root = text ? cJSON_Parse(text) : NULL;
    free(text);
    const cJSON *v = root ? cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root, "views"), argv[1]) : NULL;
    if (!v) { log_info("ground_ref_cam: no view %s", argv[1]); cJSON_Delete(root); return; }
    const cJSON *map = cJSON_GetObjectItemCaseSensitive(v, "map"), *pos = cJSON_GetObjectItemCaseSensitive(v, "pos");
    char cmd[512];
    // The reference's horizontal FOV at 16:9 is the FOV model's unit; sv_fov takes it outside the
    // players' 80 .. 150 band (docs/CAMERA_HOLDPOINT.md "Field of view").
    float fov_h = (float)cJSON_GetObjectItemCaseSensitive(v, "fov")->valuedouble;
    snprintf(cmd, sizeof(cmd), "%s%s%sset sv_fov %.1f\ntime_set %s instant\ntime_pause\nweather_set %s 0\nweather_lock 1\nsetpos %g %g %g %g %g\n",
             (cJSON_IsString(map)) ? "map " : "", (cJSON_IsString(map)) ? map->valuestring : "", (cJSON_IsString(map)) ? "\n" : "", fov_h,
             cJSON_GetObjectItemCaseSensitive(v, "time")->valuestring, cJSON_GetObjectItemCaseSensitive(v, "weather")->valuestring,
             cJSON_GetArrayItem(pos, 0)->valuedouble, cJSON_GetArrayItem(pos, 1)->valuedouble, cJSON_GetArrayItem(pos, 2)->valuedouble,
             cJSON_GetObjectItemCaseSensitive(v, "yaw")->valuedouble, cJSON_GetObjectItemCaseSensitive(v, "pitch")->valuedouble);
    cbuf_add(cmd);
    log_info("ground_ref_cam %s: %s", argv[1], cmd);
    cJSON_Delete(root);
}

static void cmd_sample(int argc, const char **argv) {
    if (argc < 5) { log_info("usage: ground_sample <x0> <y0> <x1> <y1> [label] (screen fractions)"); return; }
    for (int i = 0; i < 4; i++) GR.box[i] = DG_CLAMP((float)atof(argv[1 + i]), 0.0f, 1.0f);
    dg_strcpy(GR.label, sizeof(GR.label), argc > 5 ? argv[5] : "box");
    snprintf(GR.path, sizeof(GR.path), "%sground_sample.png", platform_pref_dir());
    remove(GR.path);
    render_screenshot(GR.path);
    GR.sample_frames = 4;
}

static void rgb_hsv(float r, float g, float b, float *h, float *s, float *v) {
    float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), d = mx - mn;
    *v = mx;
    *s = mx > 0 ? d / mx : 0;
    if (d <= 0) { *h = 0; return; }
    if (mx == r) *h = fmodf((g - b) / d, 6.0f);
    else if (mx == g) *h = (b - r) / d + 2.0f;
    else *h = (r - g) / d + 4.0f;
    *h *= 60.0f;
    if (*h < 0) *h += 360.0f;
}

static void sample_finish(void) {
    int w, h, comp;
    u8 *px = stbi_load(GR.path, &w, &h, &comp, 3);
    if (!px) { if (--GR.sample_frames > -60) return; log_info("ground_sample: no capture"); GR.sample_frames = 0; return; }
    GR.sample_frames = 0;
    int x0 = (int)(GR.box[0] * w), y0 = (int)(GR.box[1] * h), x1 = (int)(GR.box[2] * w), y1 = (int)(GR.box[3] * h);
    double sx = 0, cx = 0, ss = 0, sv = 0, ss2 = 0, sv2 = 0, mr = 0, mg = 0, mb = 0;
    int n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++, n++) {
            const u8 *p = &px[((size_t)y * w + x) * 3];
            float hh, s, v;
            rgb_hsv(p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, &hh, &s, &v);
            sx += sinf(hh * DG_DEG2RAD), cx += cosf(hh * DG_DEG2RAD);
            ss += s, sv += v, ss2 += s * s, sv2 += v * v;
            mr += p[0], mg += p[1], mb += p[2];
        }
    stbi_image_free(px);
    if (!n) { log_info("ground_sample: empty box"); return; }
    float hue = atan2f((float)sx, (float)cx) * DG_RAD2DEG;
    if (hue < 0) hue += 360.0f;
    double ms = ss / n, mv = sv / n;
    log_info("ground_sample %s: %d px, mean #%02x%02x%02x, HSV mean %.1f %.3f %.3f, std s %.3f v %.3f", GR.label, n, (int)(mr / n), (int)(mg / n), (int)(mb / n), hue, ms, mv,
             sqrt(fmax(ss2 / n - ms * ms, 0)), sqrt(fmax(sv2 / n - mv * mv, 0)));
}

void ground_ref_register(void) {
    cmd_register("ground_ref", cmd_ref, "ground_ref <r1..r5|off> [side|overlay|flip] [alpha]: a reference shot beside / over the frame (docs/GROUND_REFERENCE.md)");
    cmd_register("ground_ref_cam", cmd_ref_cam, "ground_ref_cam <r1..r5>: map, pose, FOV, time and weather that frame the ground like the reference shot (host)");
    cmd_register("ground_sample", cmd_sample, "ground_sample <x0> <y0> <x1> <y1> [label]: mean / spread HSV of a screen box (fractions) of the next frame");
}

void ground_ref_draw(void) {
    if (GR.sample_frames > 0 && --GR.sample_frames == 0) GR.sample_frames = -1;
    if (GR.sample_frames < 0) sample_finish();
    if (!GR.mode || !GR.tex) return;
    float W = ui_width(), H = ui_height();
    bool show = GR.mode != 3 || fmod(g_engine.real_time, 2.0) < 1.0;
    if (!show) return;
    if (GR.mode == 1) {
        float w = W * 0.5f, h = w * (float)GR.h / (float)GR.w;
        gd_image((GuiRect){ W - w, (H - h) * 0.5f, w, h }, GR.tex, v4(0, 0, 1, 1), UI_RGBA(255, 255, 255, 255));
        ui_rect(W * 0.5f - 1.0f, 0, 2.0f, H, UI_RGBA(255, 255, 255, 160));
    } else {
        u32 a = (u32)lrintf(DG_CLAMP(GR.mode == 3 ? 1.0f : GR.alpha, 0.0f, 1.0f) * 255.0f);
        gd_image((GuiRect){ 0, 0, W, H }, GR.tex, v4(0, 0, 1, 1), UI_RGBA(255, 255, 255, a));
    }
    ui_text_shadow(12, 40, FONT_SMALL, UI_RGBA(255, 255, 220, 230), UI_ALIGN_LEFT, GR.name);
}
