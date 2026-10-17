// Buoyancy probes, forces and the per-tick system (buoyancy.h).
#include "buoyancy.h"
#include "core/jobs.h"
#include "core/platform.h"
#include <math.h>
#include <string.h>

#define NO_WATER_H -1e9f

BuoyDesc buoy_desc_default(void) {
    BuoyDesc d;
    memset(&d, 0, sizeof(d));
    d.drag = 0.8f;
    d.angular_drag = 1.2f;
    d.quad_drag = 0.6f;
    d.lift = 0.35f;
    return d;
}

// ------------------------------------------------------------------ probes

void buoy_box_grid(vec3 half, int max, int *nx, int *ny, int *nz) {
    int x = (int)clampf(roundf(half.x * 2.0f / 0.5f), 1.0f, 4.0f);
    int y = (int)clampf(roundf(half.y * 2.0f / 0.5f), 1.0f, 4.0f);
    int z = (int)clampf(roundf(half.z * 2.0f / 0.5f), 1.0f, 4.0f);
    // Two horizontal cells per axis at least: the surface fit needs a lever arm to right the body.
    if (x < 2) x = 2;
    if (z < 2) z = 2;
    while (x * y * z > max) {
        if (y > 1 && y >= x && y >= z) y--;
        else if (x >= z && x > 1) x--;
        else if (z > 1) z--;
        else break;
    }
    *nx = x; *ny = y; *nz = z;
}

int buoy_probes_box(vec3 center, vec3 half, int nx, int ny, int nz, BuoyProbe *out, int max) {
    int n = 0;
    vec3 cell = v3(half.x / (float)nx, half.y / (float)ny, half.z / (float)nz);
    float vol = 8.0f * cell.x * cell.y * cell.z;
    for (int iy = 0; iy < ny; iy++)
        for (int iz = 0; iz < nz; iz++)
            for (int ix = 0; ix < nx; ix++) {
                if (n >= max) return n;
                BuoyProbe *p = &out[n++];
                p->local = v3(center.x - half.x + cell.x * (2.0f * (float)ix + 1.0f),
                              center.y - half.y + cell.y * (2.0f * (float)iy + 1.0f),
                              center.z - half.z + cell.z * (2.0f * (float)iz + 1.0f));
                p->half = cell;
                p->volume = vol;
                p->area = 0.0f;
            }
    return n;
}

// Cells of a box grid weighted by the fraction of each cell inside a round shape (sphere / capsule /
// cylinder), estimated on a 4^3 subsample grid: probes outside the shape are dropped.
typedef bool (*InsideFn)(const ShapeDesc *s, vec3 p);
static bool in_sphere(const ShapeDesc *s, vec3 p) { return v3_len2(p) <= s->radius * s->radius; }
static bool in_capsule(const ShapeDesc *s, vec3 p) {
    float y = clampf(p.y, -s->half_height, s->half_height);
    vec3 d = v3(p.x, p.y - y, p.z);
    return v3_len2(d) <= s->radius * s->radius;
}
static bool in_cylinder(const ShapeDesc *s, vec3 p) { return fabsf(p.y) <= s->half_height && p.x * p.x + p.z * p.z <= s->radius * s->radius; }

static int round_probes(const ShapeDesc *s, vec3 half, float true_volume, InsideFn inside, BuoyProbe *out, int max) {
    int nx, ny, nz;
    buoy_box_grid(half, max, &nx, &ny, &nz);
    if (ny < 2 && half.y > 0.2f && nx * 2 * nz <= max) ny = 2;
    BuoyProbe tmp[BUOY_MAX_PROBES];
    int n = buoy_probes_box(v3(0, 0, 0), half, nx, ny, nz, tmp, BUOY_MAX_PROBES < max ? BUOY_MAX_PROBES : max);
    int k = 0;
    float total = 0.0f;
    for (int i = 0; i < n; i++) {
        int hit = 0;
        for (int a = 0; a < 4; a++)
            for (int b = 0; b < 4; b++)
                for (int c = 0; c < 4; c++) {
                    vec3 q = v3(tmp[i].local.x + tmp[i].half.x * (((float)a + 0.5f) * 0.5f - 1.0f),
                                tmp[i].local.y + tmp[i].half.y * (((float)b + 0.5f) * 0.5f - 1.0f),
                                tmp[i].local.z + tmp[i].half.z * (((float)c + 0.5f) * 0.5f - 1.0f));
                    hit += inside(s, q) ? 1 : 0;
                }
        if (!hit) continue;
        out[k] = tmp[i];
        out[k].volume = tmp[i].volume * (float)hit / 64.0f;
        total += out[k].volume;
        k++;
    }
    // Renormalise to the analytic volume (the subsample estimate is within a few %).
    if (total > 0.0f)
        for (int i = 0; i < k; i++) out[i].volume *= true_volume / total;
    return k;
}

int buoy_probes_from_shape(const ShapeDesc *s, BuoyProbe *out, int max) {
    if (max > BUOY_MAX_PROBES) max = BUOY_MAX_PROBES;
    switch (s->kind) {
    case SHAPE_BOX: {
        int nx, ny, nz;
        buoy_box_grid(s->half, max, &nx, &ny, &nz);
        return buoy_probes_box(v3(0, 0, 0), s->half, nx, ny, nz, out, max);
    }
    case SHAPE_SPHERE: {
        float r = s->radius;
        return round_probes(s, v3s(r), 4.18879020f * r * r * r, in_sphere, out, max);
    }
    case SHAPE_CAPSULE: {
        float r = s->radius, h = s->half_height;
        return round_probes(s, v3(r, h + r, r), DG_PI * r * r * 2.0f * h + 4.18879020f * r * r * r, in_capsule, out, max);
    }
    case SHAPE_CYLINDER: {
        float r = s->radius, h = s->half_height;
        return round_probes(s, v3(r, h, r), DG_PI * r * r * 2.0f * h, in_cylinder, out, max);
    }
    case SHAPE_MESH: {
        if (!s->verts || !s->vert_count) return 0;
        vec3 lo = s->verts[0], hi = s->verts[0];
        for (u32 i = 1; i < s->vert_count; i++) { lo = v3_min(lo, s->verts[i]); hi = v3_max(hi, s->verts[i]); }
        vec3 c = v3_scale(v3_add(lo, hi), 0.5f), half = v3_scale(v3_sub(hi, lo), 0.5f);
        int nx, ny, nz;
        buoy_box_grid(half, max, &nx, &ny, &nz);
        int n = buoy_probes_box(c, half, nx, ny, nz, out, max);
        for (int i = 0; i < n; i++) out[i].volume *= 0.6f;    // a mesh fills ~60 % of its box
        return n;
    }
    default: return 0;
    }
}

void buoy_setup(Buoyancy *b, BodyId body, const BuoyProbe *probes, int n, float mass, const BuoyDesc *d) {
    memset(b, 0, sizeof(*b));
    b->body = body;
    b->d = d ? *d : buoy_desc_default();
    if (n > BUOY_MAX_PROBES) n = BUOY_MAX_PROBES;
    b->n = n;
    float ext = 0.1f;
    for (int i = 0; i < n; i++) {
        b->probe[i] = probes[i];
        b->probe[i].area = cbrtf(probes[i].volume * probes[i].volume);
        b->shape_volume += probes[i].volume;
        ext = fmaxf(ext, fmaxf(fabsf(probes[i].local.x) + probes[i].half.x, fabsf(probes[i].local.z) + probes[i].half.z));
    }
    b->extent = ext;
    b->mass = mass;
    b->volume = b->d.density > 0.0f && mass > 0.0f ? mass / b->d.density : b->shape_volume;
    b->water_h = NO_WATER_H;
}

// ------------------------------------------------------------------ forces

void buoy_compute(const Buoyancy *b, const BuoyBodyState *st, const BuoyWater *water, float dt, BuoyResult *out) {
    memset(out, 0, sizeof(*out));
    out->water_h = NO_WATER_H;
    if (b->n <= 0 || b->mass <= 0.0f || b->shape_volume <= 0.0f || !water || !water->plane) return;
    const float g = water->gravity > 0.0f ? water->gravity : 9.81f;
    const vec3 com = st->com;

    BuoyWaterPlane wp;
    if (!water->plane(water->user, com.x, com.z, b->extent, &wp)) return;
    const float h0 = wp.height, gx = wp.gx, gz = wp.gz, rho = wp.density;
    const float *u = wp.vel;
    out->water_h = h0;

    const vec3 ex = quat_rotate(st->rot, v3(1, 0, 0)), ey = quat_rotate(st->rot, v3(0, 1, 0)), ez = quat_rotate(st->rot, v3(0, 0, 1));
    const float vscale = b->volume / b->shape_volume;
    const BuoyDesc *d = &b->d;
    vec3 J = v3(0, 0, 0), L = v3(0, 0, 0);
    float sub = 0.0f;
    for (int i = 0; i < b->n; i++) {
        const BuoyProbe *p = &b->probe[i];
        vec3 pw = v3_add(st->pos, quat_rotate(st->rot, p->local));
        float H = h0 + gx * (pw.x - com.x) + gz * (pw.z - com.z);
        const float *pu = u;
        float dens = rho;
        float he = fabsf(ex.y) * p->half.x + fabsf(ey.y) * p->half.y + fabsf(ez.y) * p->half.z;
        if (he < 1e-3f) he = 1e-3f;
        float f = (H - (pw.y - he)) / (2.0f * he);
        if (f <= 0.0f) continue;
        if (f > 1.0f) f = 1.0f;
        out->probes_wet++;
        float vsub = p->volume * f * vscale;
        sub += vsub;
        vec3 cp = v3(pw.x, pw.y - he + f * he, pw.z);       // centroid of the wet part of the cell
        vec3 r = v3_sub(cp, com);
        float fb = dens * vsub * g;
        vec3 F = v3(-gx * fb * d->lift, fb, -gz * fb * d->lift);
        vec3 Ji = v3_scale(F, dt);
        // Drag on the point velocity relative to the water: linear + quadratic, never reversing it.
        float mi = b->mass * (p->volume / b->shape_volume) * f;
        vec3 vlin = v3_sub(st->v, v3(pu[0], pu[1], pu[2]));
        vec3 vrot = v3_cross(st->w, r);
        float speed = v3_len(v3_add(vlin, vrot));
        float kq = mi > 0.0f ? 0.5f * dens * d->quad_drag * p->area * f * speed / mi : 0.0f;
        float kl = fminf(1.0f, (d->drag + kq) * dt), ka = fminf(1.0f, (d->angular_drag + kq) * dt);
        Ji = v3_sub(Ji, v3_scale(v3_add(v3_scale(vlin, kl), v3_scale(vrot, ka)), mi));
        J = v3_add(J, Ji);
        L = v3_add(L, v3_cross(r, Ji));
    }
    out->impulse = J;
    out->ang_impulse = L;
    out->submerged = b->volume > 0.0f ? clampf(sub / b->volume, 0.0f, 1.0f) : 0.0f;
    out->in_water = out->probes_wet > 0;
}

// ------------------------------------------------------------------ system

void buoy_system_init(BuoySystem *s) { memset(s, 0, sizeof(*s)); }

Buoyancy *buoy_find(BuoySystem *s, BodyId body) {
    for (int i = 0; i < s->count; i++)
        if (s->item[i].body == body) return &s->item[i];
    return NULL;
}

int buoy_add_probes(BuoySystem *s, PhysicsWorld *pw, BodyId body, const BuoyProbe *probes, int n, const BuoyDesc *d) {
    float mass = phys_body_mass(pw, body);
    if (mass <= 0.0f || n <= 0) return -1;
    Buoyancy *b = buoy_find(s, body);
    if (!b) {
        if (s->count >= BUOY_MAX_BODIES) return -1;
        b = &s->item[s->count++];
    }
    buoy_setup(b, body, probes, n, mass, d);
    return (int)(b - s->item);
}

int buoy_add(BuoySystem *s, PhysicsWorld *pw, BodyId body, const BuoyDesc *d) {
    ShapeDesc sh;
    if (!phys_body_shape(pw, body, &sh)) return -1;
    BuoyProbe pr[BUOY_MAX_PROBES];
    int n = buoy_probes_from_shape(&sh, pr, BUOY_MAX_PROBES);
    return buoy_add_probes(s, pw, body, pr, n, d);
}

void buoy_remove(BuoySystem *s, BodyId body) {
    for (int i = 0; i < s->count; i++)
        if (s->item[i].body == body) {
            s->item[i] = s->item[--s->count];
            return;
        }
}

typedef struct StepCtx { BuoySystem *s; const BuoyWater *water; float dt; } StepCtx;

static void step_range(void *ctx_v, int begin, int end) {
    StepCtx *c = (StepCtx *)ctx_v;
    for (int i = begin; i < end; i++) buoy_compute(&c->s->item[i], &c->s->st[i], c->water, c->dt, &c->s->res[i]);
}

void buoy_step(BuoySystem *s, PhysicsWorld *pw, const BuoyWater *water, float dt) {
    double t0 = platform_time();
    const float g = water && water->gravity > 0.0f ? water->gravity : 9.81f;
    for (int i = 0; i < s->count; i++) {
        BuoyBodyState *st = &s->st[i];
        BodyId id = s->item[i].body;
        phys_get_transform(pw, id, &st->pos, &st->rot);
        st->com = phys_body_com(pw, id);
        st->v = phys_get_velocity(pw, id);
        st->w = phys_get_angular_velocity(pw, id);
    }
    double t1 = platform_time();
    StepCtx ctx = { s, water, dt };
    jobs_parallel_for(s->count, 8, step_range, &ctx);
    double t2 = platform_time();
    for (int i = 0; i < s->count; i++) {
        Buoyancy *b = &s->item[i];
        const BuoyResult *r = &s->res[i];
        b->prev_submerged = b->submerged;
        b->submerged = r->submerged;
        b->water_h = r->water_h;
        b->in_water = r->in_water;
        b->impulse = r->impulse;
        b->ang_impulse = r->ang_impulse;
        if (!r->in_water) continue;
        // A body at rest in still water stays asleep: skip impulses that change nothing.
        float wdt = b->mass * g * dt;
        if (!phys_is_active(pw, b->body) && fabsf(b->submerged - b->prev_submerged) < 1e-4f &&
            v3_len(v3_sub(r->impulse, v3(0, wdt, 0))) < 0.02f * wdt && v3_len(r->ang_impulse) < 0.02f * wdt)
            continue;
        phys_add_impulse_com(pw, b->body, r->impulse, r->ang_impulse);
    }
    double t3 = platform_time();
    s->read_ms = (t1 - t0) * 1000.0;
    s->compute_ms = (t2 - t1) * 1000.0;
    s->apply_ms = (t3 - t2) * 1000.0;
    s->last_ms = (t3 - t0) * 1000.0;
}
