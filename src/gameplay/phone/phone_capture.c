// Phone photo look, sidecars, media names, gallery (see phone_capture.h).
#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE   // POSIX / BSD socket and time declarations under -std=c11
#endif
#include "phone_capture.h"
#include "core/platform.h"
#include "cJSON.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static const char *FILTERS[PHONE_FILTER_COUNT] = { "none", "vivid", "warm", "cool", "mono", "noir" };
const char *phone_filter_name(int f) { return f >= 0 && f < PHONE_FILTER_COUNT ? FILTERS[f] : "none"; }
int phone_filter_from_name(const char *s) {
    for (int i = 0; i < PHONE_FILTER_COUNT; i++)
        if (s && strcmp(s, FILTERS[i]) == 0) return i;
    return PHONE_FILTER_NONE;
}

static u8 clamp8(float v) { return (u8)(v < 0 ? 0 : (v > 255 ? 255 : v + 0.5f)); }

void phone_photo_look(u8 *px, int w, int h, float sharpen, float vignette, int filter) {
    if (w < 3 || h < 3) return;
    // Unsharp mask on luma-preserving RGB: out = c + k * (c - blur3x3(c)).
    if (sharpen > 0.001f) {
        size_t row = (size_t)w * 4;
        u8 *src = (u8 *)malloc(row * (size_t)h);
        if (src) {
            memcpy(src, px, row * (size_t)h);
            float k = sharpen * 1.5f;
            for (int y = 1; y < h - 1; y++)
                for (int x = 1; x < w - 1; x++)
                    for (int c = 0; c < 3; c++) {
                        const u8 *p = src + (size_t)y * row + (size_t)x * 4 + c;
                        int blur = (p[-(int)row - 4] + 2 * p[-(int)row] + p[-(int)row + 4] + 2 * p[-4] + 4 * p[0] + 2 * p[4] + p[(int)row - 4] + 2 * p[(int)row] + p[(int)row + 4]) / 16;
                        px[(size_t)y * row + (size_t)x * 4 + c] = clamp8((float)p[0] + k * (float)(p[0] - blur));
                    }
            free(src);
        }
    }
    float cx = (float)w * 0.5f, cy = (float)h * 0.5f, inv = 1.0f / sqrtf(cx * cx + cy * cy);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            u8 *p = px + ((size_t)y * (size_t)w + (size_t)x) * 4;
            float r = p[0], g = p[1], b = p[2];
            float l = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            switch (filter) {
            case PHONE_FILTER_VIVID: r = l + (r - l) * 1.35f; g = l + (g - l) * 1.35f; b = l + (b - l) * 1.35f; break;
            case PHONE_FILTER_WARM: r *= 1.08f; g *= 1.02f; b *= 0.88f; break;
            case PHONE_FILTER_COOL: r *= 0.9f; g *= 1.0f; b *= 1.1f; break;
            case PHONE_FILTER_MONO: r = g = b = l; break;
            case PHONE_FILTER_NOIR: {
                float t = (l / 255.0f - 0.5f) * 1.5f + 0.5f;
                r = g = b = saturate(t) * 255.0f;
            } break;
            default: break;
            }
            if (vignette > 0.001f) {
                float dx = ((float)x - cx) * inv, dy = ((float)y - cy) * inv;
                float v = 1.0f - vignette * 0.6f * smoothstepf(0.35f, 1.0f, sqrtf(dx * dx + dy * dy));
                r *= v; g *= v; b *= v;
            }
            p[0] = clamp8(r);
            p[1] = clamp8(g);
            p[2] = clamp8(b);
        }
}

char *phone_meta_to_json(const PhoneShotMeta *m) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "kind", m->kind);
    cJSON_AddStringToObject(o, "file", m->file);
    cJSON_AddNumberToObject(o, "width", m->width);
    cJSON_AddNumberToObject(o, "height", m->height);
    if (strcmp(m->kind, "video") == 0) {
        cJSON_AddNumberToObject(o, "seconds", m->seconds);
        cJSON_AddNumberToObject(o, "frames", m->frames);
    }
    cJSON_AddNumberToObject(o, "zoom", m->zoom);
    cJSON_AddStringToObject(o, "filter", m->filter);
    cJSON_AddStringToObject(o, "map", m->map);
    float pos[3] = { m->position.x, m->position.y, m->position.z };
    cJSON_AddItemToObject(o, "position", cJSON_CreateFloatArray(pos, 3));
    cJSON_AddNumberToObject(o, "yaw", m->yaw_deg);
    cJSON_AddNumberToObject(o, "pitch", m->pitch_deg);
    cJSON *g = cJSON_AddObjectToObject(o, "game_time");
    cJSON_AddNumberToObject(g, "day", m->day);
    cJSON_AddNumberToObject(g, "hours", m->tod_hours);
    cJSON_AddStringToObject(g, "text", m->game_time);
    cJSON_AddStringToObject(o, "weather", m->weather);
    cJSON_AddStringToObject(o, "created", m->created);
    char *s = cJSON_Print(o);
    cJSON_Delete(o);
    return s;
}

static void jstr(const cJSON *o, const char *k, char *out, size_t n) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    dg_strcpy(out, n, cJSON_IsString(v) ? v->valuestring : "");
}
static double jnum(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : 0.0;
}

bool phone_meta_from_json(const char *json, PhoneShotMeta *m) {
    memset(m, 0, sizeof(*m));
    cJSON *o = cJSON_Parse(json);
    if (!o) return false;
    jstr(o, "kind", m->kind, sizeof(m->kind));
    jstr(o, "file", m->file, sizeof(m->file));
    m->width = (int)jnum(o, "width");
    m->height = (int)jnum(o, "height");
    m->seconds = jnum(o, "seconds");
    m->frames = (int)jnum(o, "frames");
    m->zoom = (float)jnum(o, "zoom");
    jstr(o, "filter", m->filter, sizeof(m->filter));
    jstr(o, "map", m->map, sizeof(m->map));
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(o, "position");
    if (cJSON_GetArraySize(p) == 3)
        m->position = v3((float)cJSON_GetArrayItem(p, 0)->valuedouble, (float)cJSON_GetArrayItem(p, 1)->valuedouble, (float)cJSON_GetArrayItem(p, 2)->valuedouble);
    m->yaw_deg = (float)jnum(o, "yaw");
    m->pitch_deg = (float)jnum(o, "pitch");
    const cJSON *g = cJSON_GetObjectItemCaseSensitive(o, "game_time");
    if (g) {
        m->day = (int)jnum(g, "day");
        m->tod_hours = (float)jnum(g, "hours");
        jstr(g, "text", m->game_time, sizeof(m->game_time));
    }
    jstr(o, "weather", m->weather, sizeof(m->weather));
    jstr(o, "created", m->created, sizeof(m->created));
    cJSON_Delete(o);
    return m->kind[0] != 0;
}

void phone_media_stem(const char *dir, const char *ext, i64 unix_time, char *out, size_t out_size) {
    time_t t = (time_t)unix_time;
    struct tm tm;
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char base[48];
    strftime(base, sizeof(base), "%Y-%m-%d_%H-%M-%S", &tm);
    for (int n = 1; n < 1000; n++) {
        char stem[64], path[512];
        if (n == 1) snprintf(stem, sizeof(stem), "%s", base);
        else snprintf(stem, sizeof(stem), "%s_%d", base, n);
        snprintf(path, sizeof(path), "%s%s%s", dir, stem, ext);
        if (!platform_file_exists(path)) {
            dg_strcpy(out, out_size, stem);
            return;
        }
    }
    dg_strcpy(out, out_size, base);
}

// ------------------------------------------------------------------ gallery

typedef struct ScanCtx { PhoneGallery *g; const char *dir; bool video; } ScanCtx;

static bool ends_with(const char *s, const char *suf) {
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && SDL_strcasecmp(s + a - b, suf) == 0;
}

static SDL_EnumerationResult scan_cb(void *user, const char *dirname, const char *fname) {
    (void)dirname;
    ScanCtx *c = (ScanCtx *)user;
    if (c->g->count >= PHONE_GALLERY_MAX) return SDL_ENUM_SUCCESS;
    bool ok = c->video ? ends_with(fname, ".mp4") : (ends_with(fname, ".jpg") && !ends_with(fname, ".thumb.jpg"));
    if (!ok) return SDL_ENUM_CONTINUE;
    PhoneGalleryItem *it = &c->g->items[c->g->count++];
    memset(it, 0, sizeof(*it));
    snprintf(it->path, sizeof(it->path), "%s%s", c->dir, fname);
    dg_strcpy(it->name, sizeof(it->name), fname);
    it->video = c->video;
    it->mtime = platform_file_mtime(it->path);
    char side[300];
    size_t stem = strlen(it->path) - 4;
    snprintf(side, sizeof(side), "%.*s.json", (int)stem, it->path);
    char *js = platform_read_file(side, NULL);
    if (js) {
        phone_meta_from_json(js, &it->meta);
        free(js);
    }
    if (c->video) {
        snprintf(side, sizeof(side), "%.*s.thumb.jpg", (int)stem, it->path);
        dg_strcpy(it->thumb, sizeof(it->thumb), side);
    } else {
        dg_strcpy(it->thumb, sizeof(it->thumb), it->path);
    }
    return SDL_ENUM_CONTINUE;
}

static int cmp_items(const void *a, const void *b) {
    const PhoneGalleryItem *x = (const PhoneGalleryItem *)a, *y = (const PhoneGalleryItem *)b;
    if (x->mtime != y->mtime) return x->mtime < y->mtime ? 1 : -1;
    return strcmp(y->name, x->name);   // same second: by name, newest (higher suffix) first
}

int phone_gallery_scan(PhoneGallery *g, const char *base) {
    memset(g, 0, sizeof(*g));
    char dir[512];
    snprintf(dir, sizeof(dir), "%sscreenshots/phone/", base);
    ScanCtx c = { g, dir, false };
    SDL_EnumerateDirectory(dir, scan_cb, &c);
    char vdir[512];
    snprintf(vdir, sizeof(vdir), "%svideos/phone/", base);
    ScanCtx v = { g, vdir, true };
    SDL_EnumerateDirectory(vdir, scan_cb, &v);
    qsort(g->items, (size_t)g->count, sizeof(PhoneGalleryItem), cmp_items);
    return g->count;
}

PhoneGalleryItem *phone_gallery_add(PhoneGallery *g, const char *path, bool video, const PhoneShotMeta *meta) {
    if (g->count >= PHONE_GALLERY_MAX) g->count = PHONE_GALLERY_MAX - 1;
    memmove(&g->items[1], &g->items[0], sizeof(PhoneGalleryItem) * (size_t)g->count);
    g->count++;
    PhoneGalleryItem *it = &g->items[0];
    memset(it, 0, sizeof(*it));
    dg_strcpy(it->path, sizeof(it->path), path);
    const char *slash = strrchr(path, '/');
    const char *bs = strrchr(path, '\\');
    if (bs && (!slash || bs > slash)) slash = bs;
    dg_strcpy(it->name, sizeof(it->name), slash ? slash + 1 : path);
    it->video = video;
    it->mtime = (i64)time(NULL);
    if (meta) it->meta = *meta;
    size_t stem = strlen(path) > 4 ? strlen(path) - 4 : 0;
    if (video) snprintf(it->thumb, sizeof(it->thumb), "%.*s.thumb.jpg", (int)stem, path);
    else dg_strcpy(it->thumb, sizeof(it->thumb), path);
    return it;
}
