// Maps tab globe math (docs/MENU_GLOBE.md), pure: the orthographic projection (and back), the
// camera's flights (legs: pull back, then dive), the tile pyramid's levels and the visible box.
// No engine state; tests/test_globe.c checks it.
#ifndef DG_GLOBE_MATH_H
#define DG_GLOBE_MATH_H

#include <stdbool.h>

#define GLOBE_TILE_INNER 510        // texels a tile covers (512 stored: 1 texel border each side)
#define GLOBE_TILE_SIZE 512

// The view: the point under the centre (degrees), the globe's radius and centre (pixels).
typedef struct GlobeView {
    double lon, lat, radius;
    double cx, cy;
} GlobeView;

// A view's rotation, precomputed per frame.
typedef struct GlobeBasis {
    double lon0, sin_lat0, cos_lat0;
    double radius, cx, cy;
} GlobeBasis;

void globe_basis(const GlobeView *v, GlobeBasis *b);
// Screen x, y (y down) and depth z (> 0: the visible hemisphere; -1..1 times the radius).
void globe_project(const GlobeBasis *b, double lon, double lat, double out[3]);
// The point under a screen pixel; false off the disc.
bool globe_unproject(const GlobeBasis *b, double x, double y, double *lon, double *lat);

// A flight is up to GLOBE_MAX_LEGS legs, each from where the camera is to (lon, lat, radius) in
// dur seconds (ease in-out, the radius log-interpolated and pulled back by dip x 0.45 at the
// middle), then held hold seconds before the next.
#define GLOBE_MAX_LEGS 4
typedef struct GlobeLeg {
    double lon, lat, radius;
    float dur, dip, hold;
} GlobeLeg;

typedef struct GlobeFlight {
    GlobeLeg legs[GLOBE_MAX_LEGS];
    int count, leg;
    GlobeView from;            // the camera at the current leg's start
    double t0;                 // the current leg's start (seconds)
    bool active;
} GlobeFlight;

void globe_flight_start(GlobeFlight *f, const GlobeView *cur, const GlobeLeg *legs, int count, double now);
// Moves cur (lon / lat / radius) along the flight; returns the current leg's progress 0..1.
// f->active goes false after the last leg ends.
float globe_flight_update(GlobeFlight *f, GlobeView *cur, double now);
bool globe_flight_last_leg(const GlobeFlight *f);
float globe_ease_in_out(float t);   // cubic
float globe_ease_out(float t);
double globe_lerp_lon(double a, double b, double t);   // the short way round

// Tile pyramid: level L has 2^(L+1) x 2^L tiles of 360 / 2^(L+1) degrees.
double globe_tile_deg(int level);
double globe_level_ppd(int level);                          // texels per degree
int globe_level_for(double px_per_deg, int max_level);      // the coarsest level with >= 1 texel per pixel
void globe_tile_of(int level, double lon, double lat, int *x, int *y);
void globe_tile_bounds(int level, int x, int y, double *west, double *south, double *east, double *north);

// The part of the globe on screen as a lon / lat box (conservative): screen samples unprojected,
// plus the limb inside the screen and the poles. Longitudes as [west, east] with east < west + 360
// (east may pass 180: test with globe_lon_in).
typedef struct GlobeBox { double west, south, east, north; bool all_lon; } GlobeBox;
GlobeBox globe_visible_box(const GlobeBasis *b, double sw, double sh);
bool globe_box_hits(const GlobeBox *v, double west, double south, double east, double north);
bool globe_lon_in(double lon, double west, double east);

double globe_px_per_deg(const GlobeView *v);   // at the centre

#endif
