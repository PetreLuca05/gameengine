// Inventory character preview: look + framing (see char_preview_rig.h). Pure.
#include "char_preview_rig.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

void char_preview_rig_default(CharPreviewRig *r) {
    memset(r, 0, sizeof(*r));
    r->head_top = 0.21f;
    r->bottom_t = 0.7f;
    r->crown_m = 0.05f;
    r->square = 1.0f;
    r->fov_deg = 28.0f;
    r->key_dir = v3(-0.45f, 0.6f, 0.65f);
    r->key_color = v3(1.0f, 0.96f, 0.9f);
    r->key = 2.4f;
    r->fill_dir = v3(0.7f, 0.05f, 0.5f);
    r->fill_color = v3(0.8f, 0.86f, 1.0f);
    r->fill = 0.35f;
    r->rim_pos[0] = v3(-0.9f, 0.7f, -1.4f);
    r->rim_pos[1] = v3(1.0f, 0.5f, -1.4f);
    r->rim_color = v3(0.95f, 0.97f, 1.0f);
    r->rim = 14.0f;
    r->rim_radius = 6.0f;
    r->sky = v3(0.55f, 0.6f, 0.68f);
    r->ground = v3(0.25f, 0.22f, 0.2f);
    r->ambient = 0.3f;
    r->exposure = 0.85f;
    r->saturation = 0.85f;
    r->supersample = 2;
    r->held_low = true;
}

static float num(const cJSON *o, const char *k, float def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}

static vec3 vec(const cJSON *o, const char *k, vec3 def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsArray(v) || cJSON_GetArraySize(v) < 3) return def;
    return v3((float)cJSON_GetArrayItem(v, 0)->valuedouble, (float)cJSON_GetArrayItem(v, 1)->valuedouble, (float)cJSON_GetArrayItem(v, 2)->valuedouble);
}

// { "dir" | "pos": [..], "color": [..], "intensity": k }
static void light(const cJSON *o, const char *key, vec3 *dir, vec3 *color, float *k) {
    const cJSON *l = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsObject(l)) return;
    if (dir) *dir = vec(l, "dir", *dir);
    *color = vec(l, "color", *color);
    *k = num(l, "intensity", *k);
}

bool char_preview_rig_parse(CharPreviewRig *r, const char *json, char *err, size_t err_size) {
    cJSON *root = cJSON_Parse(json);
    if (!cJSON_IsObject(root)) {
        snprintf(err, err_size, "char_preview: JSON parse error near '%.32s'", cJSON_GetErrorPtr() ? cJSON_GetErrorPtr() : "?");
        cJSON_Delete(root);
        return false;
    }
    const cJSON *f = cJSON_GetObjectItemCaseSensitive(root, "framing");
    r->head_top = clampf(num(f, "head_top", r->head_top), 0.0f, 0.9f);
    r->bottom_t = clampf(num(f, "bottom_t", r->bottom_t), -1.0f, 2.0f);
    r->fov_deg = clampf(num(f, "fov_deg", r->fov_deg), 5.0f, 90.0f);
    r->turn_deg = num(f, "turn_deg", r->turn_deg);
    r->crown_m = clampf(num(f, "crown_m", r->crown_m), -0.2f, 0.3f);
    r->square = clampf(num(f, "square", r->square), 0.0f, 1.0f);
    const cJSON *l = cJSON_GetObjectItemCaseSensitive(root, "lights");
    light(l, "key", &r->key_dir, &r->key_color, &r->key);
    light(l, "fill", &r->fill_dir, &r->fill_color, &r->fill);
    const cJSON *rim = cJSON_GetObjectItemCaseSensitive(l, "rim");
    if (cJSON_IsObject(rim)) {
        r->rim_color = vec(rim, "color", r->rim_color);
        r->rim = num(rim, "intensity", r->rim);
        r->rim_radius = num(rim, "radius", r->rim_radius);
        const cJSON *p = cJSON_GetObjectItemCaseSensitive(rim, "pos");
        for (int k = 0; k < 2 && cJSON_IsArray(p) && k < cJSON_GetArraySize(p); k++) {
            const cJSON *e = cJSON_GetArrayItem(p, k);
            if (cJSON_IsArray(e) && cJSON_GetArraySize(e) >= 3)
                r->rim_pos[k] = v3((float)cJSON_GetArrayItem(e, 0)->valuedouble, (float)cJSON_GetArrayItem(e, 1)->valuedouble, (float)cJSON_GetArrayItem(e, 2)->valuedouble);
        }
    }
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(l, "ambient");
    if (cJSON_IsObject(a)) {
        r->sky = vec(a, "sky", r->sky);
        r->ground = vec(a, "ground", r->ground);
        r->ambient = num(a, "intensity", r->ambient);
    }
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(root, "output");
    r->exposure = fmaxf(num(o, "exposure", r->exposure), 0.0f);
    const cJSON *tm = cJSON_GetObjectItemCaseSensitive(o, "tonemap");
    if (cJSON_IsString(tm)) r->tonemap = !strcmp(tm->valuestring, "aces") ? 1 : 0;
    r->saturation = clampf(num(o, "saturation", r->saturation), 0.0f, 2.0f);
    r->supersample = (int)clampf(num(o, "supersample", (float)r->supersample), 1.0f, 4.0f);
    const cJSON *h = cJSON_GetObjectItemCaseSensitive(root, "held");
    const cJSON *hp = cJSON_GetObjectItemCaseSensitive(h, "pose");
    if (cJSON_IsString(hp)) r->held_low = strcmp(hp->valuestring, "aim") != 0;
    cJSON_Delete(root);
    return true;
}

CharPreviewShot char_preview_frame(const CharPreviewRig *r, float head_top_y, float bottom_y) {
    // The image spans [bottom_y, top_y] at the character's plane; the head top sits head_top below its top.
    float f = clampf(r->head_top, 0.0f, 0.9f);
    float top_y = (head_top_y - f * bottom_y) / (1.0f - f);
    CharPreviewShot s;
    s.fov_rad = clampf(r->fov_deg, 5.0f, 90.0f) * DG_DEG2RAD;
    s.cam_y = 0.5f * (top_y + bottom_y);
    s.dist = fmaxf(top_y - bottom_y, 0.05f) / (2.0f * tanf(0.5f * s.fov_rad));
    return s;
}

float char_preview_bottom_y(const CharPreviewRig *r, float hip_y, float knee_y) { return hip_y + (knee_y - hip_y) * r->bottom_t; }

float char_preview_image_y(const CharPreviewShot *s, float y) { return 0.5f - (y - s->cam_y) / (2.0f * s->dist * tanf(0.5f * s->fov_rad)); }
