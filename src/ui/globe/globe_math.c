// Maps tab globe math (globe_math.h, docs/MENU_GLOBE.md).
#include "globe_math.h"
#include <math.h>

#define D2R (3.14159265358979323846 / 180.0)

void globe_basis(const GlobeView *v, GlobeBasis *b) {
    b->lon0 = v->lon;
    b->sin_lat0 = sin(v->lat * D2R);
    b->cos_lat0 = cos(v->lat * D2R);
    b->radius = v->radius;
    b->cx = v->cx;
    b->cy = v->cy;
}

// Rotate so (lon0, lat0) faces the viewer: x east, y north, z towards the viewer.
void globe_project(const GlobeBasis *b, double lon, double lat, double out[3]) {
    double lam = (lon - b->lon0) * D2R, ph = lat * D2R;
    double cl = cos(ph);
    double x = cl * sin(lam), y = sin(ph), z = cl * cos(lam);
    double y2 = y * b->cos_lat0 - z * b->sin_lat0;
    double z2 = y * b->sin_lat0 + z * b->cos_lat0;
    out[0] = b->cx + b->radius * x;
    out[1] = b->cy - b->radius * y2;
    out[2] = z2;
}

bool globe_unproject(const GlobeBasis *b, double x, double y, double *lon, double *lat) {
    double nx = (x - b->cx) / b->radius, ny = (b->cy - y) / b->radius;
    double dd = nx * nx + ny * ny;
    if (dd > 1.0) return false;
    double nz = sqrt(1.0 - dd);
    double yy = ny * b->cos_lat0 + nz * b->sin_lat0;   // undo the latitude tilt
    double zz = -ny * b->sin_lat0 + nz * b->cos_lat0;
    *lat = asin(yy < -1.0 ? -1.0 : (yy > 1.0 ? 1.0 : yy)) / D2R;
    double l = atan2(nx, zz) / D2R + b->lon0;
    *lon = fmod(fmod(l + 180.0, 360.0) + 360.0, 360.0) - 180.0;
    return true;
}

// ------------------------------------------------------------------ flights

float globe_ease_in_out(float t) { return t < 0.5f ? 4.0f * t * t * t : 1.0f - powf(-2.0f * t + 2.0f, 3.0f) * 0.5f; }
float globe_ease_out(float t) { return 1.0f - powf(1.0f - t, 3.0f); }

double globe_lerp_lon(double a, double b, double t) {
    double d = fmod(fmod(b - a + 540.0, 360.0) + 360.0, 360.0) - 180.0;
    return a + d * t;
}

void globe_flight_start(GlobeFlight *f, const GlobeView *cur, const GlobeLeg *legs, int count, double now) {
    f->count = count < GLOBE_MAX_LEGS ? count : GLOBE_MAX_LEGS;
    for (int i = 0; i < f->count; i++) f->legs[i] = legs[i];
    f->leg = 0;
    f->from = *cur;
    f->t0 = now;
    f->active = f->count > 0;
}

bool globe_flight_last_leg(const GlobeFlight *f) { return f->leg >= f->count - 1; }

float globe_flight_update(GlobeFlight *f, GlobeView *cur, double now) {
    if (!f->active || f->count <= 0) return 1.0f;
    const GlobeLeg *g = &f->legs[f->leg];
    float dur = g->dur > 0.01f ? g->dur : 0.01f;
    float t = (float)((now - f->t0) / dur);
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    float e = globe_ease_in_out(t);
    cur->lon = globe_lerp_lon(f->from.lon, g->lon, e);
    cur->lat = f->from.lat + (g->lat - f->from.lat) * e;
    double r = exp(log(f->from.radius) + (log(g->radius) - log(f->from.radius)) * e);
    cur->radius = r * (1.0 - 0.45 * g->dip * sin(3.14159265358979323846 * t));   // pull back, then dive
    if (now - f->t0 > dur + g->hold) {
        if (globe_flight_last_leg(f)) f->active = false;
        else {
            f->leg++;
            f->from = *cur;
            f->t0 = now;
        }
    }
    return t;
}

// ------------------------------------------------------------------ tiles

double globe_tile_deg(int level) { return 360.0 / (double)(1 << (level + 1)); }
double globe_level_ppd(int level) { return (double)GLOBE_TILE_INNER / globe_tile_deg(level); }

int globe_level_for(double px_per_deg, int max_level) {
    int l = 0;
    while (l < max_level && globe_level_ppd(l) < px_per_deg) l++;
    return l;
}

void globe_tile_of(int level, double lon, double lat, int *x, int *y) {
    double d = globe_tile_deg(level);
    int nx = 1 << (level + 1), ny = 1 << level;
    int tx = (int)floor((lon + 180.0) / d), ty = (int)floor((90.0 - lat) / d);
    *x = ((tx % nx) + nx) % nx;
    *y = ty < 0 ? 0 : (ty >= ny ? ny - 1 : ty);
}

void globe_tile_bounds(int level, int x, int y, double *west, double *south, double *east, double *north) {
    double d = globe_tile_deg(level);
    *west = -180.0 + x * d;
    *east = *west + d;
    *north = 90.0 - y * d;
    *south = *north - d;
}

double globe_px_per_deg(const GlobeView *v) { return v->radius * D2R; }

bool globe_lon_in(double lon, double west, double east) {
    double d = fmod(fmod(lon - west, 360.0) + 360.0, 360.0);
    return d <= east - west;
}

static void box_add(GlobeBox *b, double lon, double lat, bool *any, double *ref) {
    if (!*any) {
        *any = true;
        *ref = lon;
        b->west = b->east = lon;
        b->south = b->north = lat;
        return;
    }
    // Longitudes kept continuous around the first sample (the box never spans more than 360).
    double l = *ref + (fmod(fmod(lon - *ref + 540.0, 360.0) + 360.0, 360.0) - 180.0);
    if (l < b->west) b->west = l;
    if (l > b->east) b->east = l;
    if (lat < b->south) b->south = lat;
    if (lat > b->north) b->north = lat;
}

GlobeBox globe_visible_box(const GlobeBasis *b, double sw, double sh) {
    GlobeBox box = { -180.0, -90.0, 180.0, 90.0, true };
    bool any = false;
    double ref = 0, lon, lat;
    const int N = 24;
    // The screen's border and a grid inside it.
    for (int j = 0; j <= N; j++)
        for (int i = 0; i <= N; i++) {
            double x = sw * i / N, y = sh * j / N;
            if (globe_unproject(b, x, y, &lon, &lat)) box_add(&box, lon, lat, &any, &ref);
        }
    // The limb where it is on screen.
    for (int k = 0; k < 256; k++) {
        double a = 6.283185307179586 * k / 256.0;
        double x = b->cx + cos(a) * b->radius * 0.99999, y = b->cy + sin(a) * b->radius * 0.99999;
        if (x < 0 || y < 0 || x > sw || y > sh) continue;
        if (globe_unproject(b, x, y, &lon, &lat)) box_add(&box, lon, lat, &any, &ref);
    }
    if (!any) {
        box.all_lon = false;
        box.west = box.east = 0;
        box.south = box.north = 0;
        return box;
    }
    // Grid spacing: widen by a sample step's worth of degrees.
    double step = fmax(sw, sh) / N / (b->radius * D2R);
    box.west -= step;
    box.east += step;
    box.south = fmax(-90.0, box.south - step);
    box.north = fmin(90.0, box.north + step);
    box.all_lon = box.east - box.west >= 360.0;
    // A visible pole: every longitude meets there.
    double p[3];
    for (int s = -1; s <= 1; s += 2) {
        globe_project(b, 0.0, 90.0 * s, p);
        if (p[2] > 0 && p[0] >= 0 && p[1] >= 0 && p[0] <= sw && p[1] <= sh) {
            box.all_lon = true;
            if (s > 0) box.north = 90.0;
            else box.south = -90.0;
        }
    }
    if (box.all_lon) { box.west = -180.0; box.east = 180.0; }
    return box;
}

bool globe_box_hits(const GlobeBox *v, double west, double south, double east, double north) {
    if (north < v->south || south > v->north) return false;
    if (v->all_lon) return true;
    // Interval overlap on the circle: shift the tile's west into [v.west, v.west + 360).
    double w = v->west + fmod(fmod(west - v->west, 360.0) + 360.0, 360.0);
    double e = w + (east - west);
    if (w <= v->east) return true;
    return e - 360.0 >= v->west;
}
