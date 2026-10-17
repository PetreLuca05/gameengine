// Phone photos and the gallery (pure apart from file I/O; unit tested).
//
//   screenshots/phone/<YYYY-MM-DD_HH-MM-SS>[_n].jpg   + .json sidecar (PhoneShotMeta)
//   videos/phone/<YYYY-MM-DD_HH-MM-SS>[_n].mp4        + .json sidecar + .thumb.jpg
// Paths are relative to the user's pref dir (platform_pref_dir). The gallery scans both folders
// on start, newest first.
#ifndef DG_PHONE_CAPTURE_H
#define DG_PHONE_CAPTURE_H

#include "core/base.h"
#include "core/dgmath.h"

typedef enum PhoneFilter { PHONE_FILTER_NONE, PHONE_FILTER_VIVID, PHONE_FILTER_WARM, PHONE_FILTER_COOL, PHONE_FILTER_MONO, PHONE_FILTER_NOIR, PHONE_FILTER_COUNT } PhoneFilter;
const char *phone_filter_name(int f);
int phone_filter_from_name(const char *s);

// The phone "camera look" on a display-encoded RGBA image, in place: unsharp mask (sharpen
// 0..1), colour filter, vignette (0..1).
void phone_photo_look(u8 *rgba, int w, int h, float sharpen, float vignette, int filter);

typedef struct PhoneShotMeta {
    char kind[8];             // "photo" / "video"
    char file[96];            // file name (no directory)
    int width, height;
    double seconds;           // videos
    int frames;
    float zoom;
    char filter[16];
    char map[32];
    vec3 position;            // world (m)
    float yaw_deg, pitch_deg; // phone lens direction
    int day;                  // in-game day
    float tod_hours;          // in-game time of day (0..24)
    char game_time[32];       // "Day 3  14:05"
    char weather[32];
    char created[32];         // ISO 8601 local wall clock
} PhoneShotMeta;

// Sidecar JSON (caller frees) and back.
char *phone_meta_to_json(const PhoneShotMeta *m);
bool phone_meta_from_json(const char *json, PhoneShotMeta *m);

// "<YYYY-MM-DD_HH-MM-SS>" for a wall clock time (local), unique in dir with ext ("_2", "_3" ...).
void phone_media_stem(const char *dir, const char *ext, i64 unix_time, char *out, size_t out_size);

// Gallery: photos and videos, newest first.
#define PHONE_GALLERY_MAX 256
typedef struct PhoneGalleryItem {
    char path[260];           // media file
    char name[96];            // file name
    char thumb[260];          // thumbnail image (photos: the photo itself)
    bool video;
    i64 mtime;
    int server_id;            // id on the companion server (0 = not registered)
    u32 texture;              // in-game thumbnail texture (0 = not loaded)
    PhoneShotMeta meta;
} PhoneGalleryItem;

typedef struct PhoneGallery { PhoneGalleryItem items[PHONE_GALLERY_MAX]; int count; } PhoneGallery;

// Scans <base>screenshots/phone and <base>videos/phone (base ends with a separator).
int phone_gallery_scan(PhoneGallery *g, const char *base);
// Inserts a new item at the front (dropping the oldest when full). Returns it.
PhoneGalleryItem *phone_gallery_add(PhoneGallery *g, const char *path, bool video, const PhoneShotMeta *meta);
// Deletes item i: the media file, its .json sidecar and a video's .thumb.jpg, then the entry.
// Returns false when i is out of range (the files are removed best effort).
bool phone_gallery_delete(PhoneGallery *g, int i);
int phone_gallery_find_server_id(const PhoneGallery *g, int server_id);   // -1 none

#endif
