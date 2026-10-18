// The city outside in a story level (docs/STORY.md "Vista"); see game_vista.h.
//
// Everything here is a function of the vista clock (the host's story seconds; clients advance it by
// frame dt between resyncs) and the started sequences' ages: src/story/vista_seq.c evaluates them
// (power per district, helicopters, smoke / fire / flash spots, traffic, birds, the day state, crowds),
// this file draws: window lights through render_set_city, helicopter bodies as draw items, the damage
// layer (the city block's "damage" boxes: wrecks, scorched facades, rubble; one mesh per day, drawn up
// to the current day state), everything else as sprites of the story_vista effect
// (assets/vfx/effects/story_vista.json: smoke, glow, dot).
#include "game_vista.h"
#include "game_internal.h"
#include "game_story.h"
#include "story/vista_seq.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/log.h"
#include "core/platform.h"
#include "render/render.h"
#include "render/mesh_gen.h"
#include "vfx/vfx.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define G (g_engine.game)
#define VISTA_ROADS 64
#define VISTA_ROAD_PTS 48
#define VISTA_CARS 140
#define VISTA_DAYS 10

typedef struct VistaRoad {
    vec3 p[VISTA_ROAD_PTS];
    float cum[VISTA_ROAD_PTS];   // length along the road at each point
    int n;
} VistaRoad;

static struct {
    VistaSeqLib lib;
    bool lib_loaded;
    bool city;                   // the level has a "city" block
    float base, intensity;
    vec2 center;
    VistaRoad roads[VISTA_ROADS];
    int road_n;
    float road_total;
    struct { char name[VISTA_NAME]; float t0; } act[24];
    int n;
    float clock;
    VistaFrame f;
    MeshId damage[VISTA_DAYS + 1];   // per day: everything that appeared overnight (0 = none)
    int damage_boxes;
    Cvar *enable;
} VS;

static float hashf(int k, int salt) {
    u32 h = (u32)k * 2654435761u ^ (u32)salt * 40503u;
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    return (float)(h & 0xFFFFFF) / 16777216.0f;
}

static bool point_fn(void *user, const char *name, float out[3]) {
    (void)user;
    vec3 p;
    if (!story_point_pos(name, &p)) return false;
    out[0] = p.x, out[1] = p.y, out[2] = p.z;
    return true;
}

static char *read_text(const char *rel) {
    char buf[512];
    return platform_read_file(platform_data_path(rel, buf, sizeof(buf)), NULL);
}

static void load_lib(void) {
    char *text = read_text("assets/story/vista/sequences.json");
    char err[128] = "";
    VS.lib_loaded = text && vista_seq_parse(&VS.lib, text, err, sizeof(err));
    if (!VS.lib_loaded) log_warn("vista: assets/story/vista/sequences.json: %s", text ? err : "missing");
    free(text);
}

static float jnum(const cJSON *o, const char *k, float def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}

static void free_damage(void) {
    bool any = false;
    for (int d = 0; d <= VISTA_DAYS; d++) any |= VS.damage[d] != 0;
    if (any) render_wait_idle();
    for (int d = 0; d <= VISTA_DAYS; d++) {
        if (VS.damage[d]) render_mesh_destroy(VS.damage[d]);
        VS.damage[d] = 0;
    }
    VS.damage_boxes = 0;
}

// "damage": [{"day": d, "p": [x, y, z] (base centre), "s": [w, h, d], "yaw": deg, "c": [r, g, b]}]
static void build_damage(const cJSON *list) {
    MeshBuilder mb[VISTA_DAYS + 1];
    for (int d = 0; d <= VISTA_DAYS; d++) mb_init(&mb[d]);
    const cJSON *it;
    cJSON_ArrayForEach(it, list) {
        int day = DG_CLAMP((int)jnum(it, "day", 1), 0, VISTA_DAYS);
        const cJSON *p = cJSON_GetObjectItemCaseSensitive(it, "p"), *s = cJSON_GetObjectItemCaseSensitive(it, "s"), *c = cJSON_GetObjectItemCaseSensitive(it, "c");
        if (cJSON_GetArraySize(p) < 3 || cJSON_GetArraySize(s) < 3) continue;
        vec3 half = v3((float)cJSON_GetArrayItem(s, 0)->valuedouble * 0.5f, (float)cJSON_GetArrayItem(s, 1)->valuedouble * 0.5f, (float)cJSON_GetArrayItem(s, 2)->valuedouble * 0.5f);
        vec3 pos = v3((float)cJSON_GetArrayItem(p, 0)->valuedouble, (float)cJSON_GetArrayItem(p, 1)->valuedouble + half.y, (float)cJSON_GetArrayItem(p, 2)->valuedouble);
        vec3 col = cJSON_GetArraySize(c) >= 3 ? v3((float)cJSON_GetArrayItem(c, 0)->valuedouble, (float)cJSON_GetArrayItem(c, 1)->valuedouble, (float)cJSON_GetArrayItem(c, 2)->valuedouble) : v3(0.2f, 0.2f, 0.2f);
        mb_box_transformed(&mb[day], mat4_trs(pos, quat_yaw(jnum(it, "yaw", 0) * DG_DEG2RAD), v3s(1)), half, rgba8(col.x, col.y, col.z, 1));
        VS.damage_boxes++;
    }
    for (int d = 0; d <= VISTA_DAYS; d++) {
        if (mb[d].vcount) VS.damage[d] = mb_upload(&mb[d]);
        mb_free(&mb[d]);
    }
}

void vista_level_loaded(const char *map) {
    free_damage();
    VS.city = false;
    VS.n = 0;
    VS.clock = 0;
    VS.road_n = 0;
    VS.road_total = 0;
    if (!VS.lib_loaded) load_lib();
    char path[160];
    snprintf(path, sizeof(path), "data/levels/%s.json", map);
    char *text = read_text(path);
    cJSON *root = text ? cJSON_Parse(text) : NULL;
    free(text);
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(root, "city");
    if (c) {
        VS.city = true;
        VS.base = jnum(c, "base", 0.45f);
        VS.intensity = jnum(c, "intensity", 2.5f);
        const cJSON *ce = cJSON_GetObjectItemCaseSensitive(c, "center");
        VS.center = cJSON_GetArraySize(ce) >= 2 ? v2((float)cJSON_GetArrayItem(ce, 0)->valuedouble, (float)cJSON_GetArrayItem(ce, 1)->valuedouble) : v2(0, 0);
        const cJSON *r;
        cJSON_ArrayForEach(r, cJSON_GetObjectItemCaseSensitive(c, "roads")) {
            if (VS.road_n >= VISTA_ROADS) break;
            VistaRoad *rd = &VS.roads[VS.road_n];
            rd->n = 0;
            const cJSON *p;
            cJSON_ArrayForEach(p, cJSON_GetObjectItemCaseSensitive(r, "points")) {
                if (rd->n >= VISTA_ROAD_PTS || cJSON_GetArraySize(p) < 3) break;
                vec3 q = v3((float)cJSON_GetArrayItem(p, 0)->valuedouble, (float)cJSON_GetArrayItem(p, 1)->valuedouble, (float)cJSON_GetArrayItem(p, 2)->valuedouble);
                rd->cum[rd->n] = rd->n ? rd->cum[rd->n - 1] + v3_len(v3_sub(q, rd->p[rd->n - 1])) : 0.0f;
                rd->p[rd->n++] = q;
            }
            if (rd->n >= 2 && rd->cum[rd->n - 1] > 10.0f) {
                VS.road_total += rd->cum[rd->n - 1];
                VS.road_n++;
            }
        }
        build_damage(cJSON_GetObjectItemCaseSensitive(c, "damage"));
    }
    cJSON_Delete(root);
}

void vista_sequence(const char *name, float age, float now) {
    VS.clock = now;
    if (!name[0]) { VS.n = 0; return; }
    int k = 0;
    while (k < VS.n && strcmp(VS.act[k].name, name)) k++;
    if (k < VS.n) {
        memmove(&VS.act[k], &VS.act[k + 1], sizeof(VS.act[0]) * (size_t)(VS.n - k - 1));
        VS.n--;
    }
    if (VS.n >= ARRAY_COUNT(VS.act)) return;
    dg_strcpy(VS.act[VS.n].name, VISTA_NAME, name);
    VS.act[VS.n++].t0 = VS.clock - age;
    if (!vista_seq_find(&VS.lib, name)) log_warn("vista: unknown sequence '%s' (assets/story/vista/sequences.json)", name);
}

void vista_frame(float dt, const float *host_now) {
    if (!VS.city) return;
    VS.clock = host_now ? *host_now : VS.clock + dt;
    VistaActive a[ARRAY_COUNT(VS.act)];
    for (int i = 0; i < VS.n; i++) {
        dg_strcpy(a[i].name, VISTA_NAME, VS.act[i].name);
        a[i].age = VS.clock - VS.act[i].t0;
    }
    vista_eval(&VS.lib, a, VS.n, point_fn, NULL, &VS.f);
    render_set_city(VS.base, VS.intensity, VS.center, VS.f.power);
}

// ------------------------------------------------------------------ drawing

static void box(vec3 pos, quat rot, vec3 size, vec3 color, float rough, float emissive) {
    DrawItem d;
    memset(&d, 0, sizeof(d));
    d.mesh = G->meshes.cube;
    d.model = mat4_trs(pos, rot, size);
    d.mat = material_color(v4(color.x, color.y, color.z, 1));
    d.mat.roughness = rough;
    d.mat.emissive = emissive;
    render_draw(&d);
}

static void draw_heli(const VistaHeli *h, int em_glow, int em_smoke) {
    vec3 pos = v3(h->pos[0], h->pos[1] + 1.6f, h->pos[2]);   // the path is where the skids are (a landed one stands on its pad)
    quat rot = quat_mul(quat_yaw(h->yaw), quat_axis_angle(v3(0, 0, 1), h->roll));
    vec3 body = v3(0.13f, 0.14f, 0.15f);
    box(pos, rot, v3(2.4f, 2.3f, 6.5f), body, 0.5f, 0);
    box(v3_add(pos, quat_rotate(rot, v3(0, 0.3f, 6.0f))), rot, v3(0.6f, 0.6f, 6.0f), body, 0.5f, 0);
    box(v3_add(pos, quat_rotate(rot, v3(0, 1.0f, 8.6f))), rot, v3(0.2f, 2.2f, 1.2f), body, 0.5f, 0);
    box(v3_add(pos, quat_rotate(rot, v3(0, -0.4f, -2.8f))), rot, v3(2.0f, 1.2f, 1.4f), v3(0.25f, 0.32f, 0.38f), 0.1f, 0);   // canopy
    for (int s = -1; s <= 1; s += 2) box(v3_add(pos, quat_rotate(rot, v3((float)s * 1.1f, -1.5f, 0))), rot, v3(0.12f, 0.12f, 4.5f), body, 0.5f, 0);   // skids
    for (int b = 0; b < 2; b++) {   // main rotor: two blades, spinning
        quat br = quat_mul(rot, quat_yaw(h->rotor + (float)b * DG_PI * 0.5f));
        box(v3_add(pos, quat_rotate(rot, v3(0, 1.5f, 0))), br, v3(11.0f, 0.08f, 0.45f), v3(0.08f, 0.08f, 0.08f), 0.6f, 0);
    }
    float t = VS.clock;
    if (fmodf(t, 1.3f) < 0.18f) vfx_game_push_sprite(em_glow, v3_add(pos, quat_rotate(rot, v3(0, 2.1f, 8.8f))), v3(0, 0, 0), 2.0f, v4(1.0f, 0.1f, 0.05f, 1));
    if (fmodf(t + 0.6f, 1.9f) < 0.08f) vfx_game_push_sprite(em_glow, v3_add(pos, quat_rotate(rot, v3(0, -1.2f, 0))), v3(0, 0, 0), 3.0f, v4(1.2f, 1.2f, 1.2f, 1));
    if (h->crashing) {   // a smoke trail behind the spinning body
        for (int k = 0; k < 8; k++) {
            float u = (float)k / 8.0f;
            vec3 p = v3_add(pos, v3((hashf(k, 3) - 0.5f) * 6.0f, 2.0f + u * 14.0f, (hashf(k, 4) - 0.5f) * 6.0f));
            vfx_game_push_sprite(em_smoke, p, v3(0, 0, 0), 4.0f + 10.0f * u, v4(0.12f, 0.12f, 0.12f, 0.6f * (1.0f - u)));
        }
    }
}

// Position + direction at distance s along a road.
static void road_at(const VistaRoad *r, float s, vec3 *pos, vec3 *dir) {
    int i = 1;
    while (i < r->n - 1 && r->cum[i] < s) i++;
    float seg = r->cum[i] - r->cum[i - 1];
    float u = seg > 1e-3f ? DG_CLAMP((s - r->cum[i - 1]) / seg, 0.0f, 1.0f) : 0.0f;
    *pos = v3_lerp(r->p[i - 1], r->p[i], u);
    *dir = v3_norm(v3_sub(r->p[i], r->p[i - 1]));
}

static void draw_traffic(vec3 cam, float night, int em_glow, int em_dot) {
    if (!VS.road_n || VS.road_total <= 0) return;
    const VistaFrame *f = &VS.f;
    int cars = (int)(VISTA_CARS * DG_CLAMP(f->traffic_density, 0.0f, 1.0f));
    static const float k_paint[6][3] = { { 0.7f, 0.7f, 0.72f }, { 0.08f, 0.08f, 0.09f }, { 0.45f, 0.08f, 0.07f }, { 0.15f, 0.22f, 0.38f }, { 0.85f, 0.85f, 0.82f }, { 0.35f, 0.36f, 0.38f } };
    for (int i = 0; i < cars; i++) {
        float pick = hashf(i, 11) * VS.road_total;
        int r = 0;
        while (r < VS.road_n - 1 && pick > VS.roads[r].cum[VS.roads[r].n - 1]) {
            pick -= VS.roads[r].cum[VS.roads[r].n - 1];
            r++;
        }
        const VistaRoad *rd = &VS.roads[r];
        float len = rd->cum[rd->n - 1];
        float dirsign = hashf(i, 12) < 0.5f ? 1.0f : -1.0f, speed = 9.0f + 6.0f * hashf(i, 13);
        float t = VS.clock;
        if (f->stall_age >= 0 && hashf(i, 15) < f->traffic_stall) t = fminf(t, VS.clock - f->stall_age + hashf(i, 16) * 20.0f);
        float s = fmodf(hashf(i, 14) * len + dirsign * speed * t, len);
        if (s < 0) s += len;
        vec3 p, d;
        road_at(rd, s, &p, &d);
        d = v3_scale(d, dirsign);
        p = v3_add(p, v3(-d.z * 3.5f, 0, d.x * 3.5f));   // the right-hand lane
        float dist = v3_len(v3_sub(p, cam));
        if (dist > 1000.0f) continue;
        bool lit = night > 0.25f && hashf(i, 17) >= f->traffic_dark;
        if (dist < 450.0f) {
            const float *c = k_paint[(int)(hashf(i, 18) * 6) % 6];
            box(v3_add(p, v3(0, 0.75f, 0)), quat_yaw(atan2f(-d.x, -d.z)), v3(1.8f, 1.4f, 4.4f), v3(c[0], c[1], c[2]), 0.35f, 0);
        } else {
            vfx_game_push_sprite(em_dot, v3_add(p, v3(0, 0.8f, 0)), v3(0, 0, 0), 2.6f, v4(0.1f, 0.1f, 0.11f, 0.8f * (1.0f - night)));
        }
        if (lit) {
            vfx_game_push_sprite(em_glow, v3_add(p, v3(d.x * 2.3f, 0.7f, d.z * 2.3f)), v3(0, 0, 0), 2.2f, v4(1.0f, 0.9f, 0.7f, 1));
            vfx_game_push_sprite(em_glow, v3_add(p, v3(-d.x * 2.3f, 0.8f, -d.z * 2.3f)), v3(0, 0, 0), 1.3f, v4(0.9f, 0.08f, 0.05f, 1));
        }
    }
}

// The damage layer up to today (static boxes, vertex coloured like the level's brushes).
static void draw_damage(void) {
    for (int d = 0; d <= VISTA_DAYS && d <= VS.f.day; d++) {
        if (!VS.damage[d]) continue;
        DrawItem it;
        memset(&it, 0, sizeof(it));
        it.mesh = VS.damage[d];
        it.model = mat4_identity();
        it.mat = material_color(v4(1, 1, 1, 1));
        it.mat.roughness = 0.9f;
        render_draw(&it);
    }
}

// The dead walking the streets below: slow dots along the city roads, spread across the carriageway.
static void draw_crowd(vec3 cam, float night, int em_dot) {
    if (!VS.road_n || VS.road_total <= 0 || VS.f.crowd <= 0) return;
    for (int i = 0; i < VS.f.crowd; i++) {
        float pick = hashf(i, 31) * VS.road_total;
        int r = 0;
        while (r < VS.road_n - 1 && pick > VS.roads[r].cum[VS.roads[r].n - 1]) {
            pick -= VS.roads[r].cum[VS.roads[r].n - 1];
            r++;
        }
        const VistaRoad *rd = &VS.roads[r];
        float len = rd->cum[rd->n - 1];
        float s = fmodf(hashf(i, 32) * len + (hashf(i, 33) < 0.5f ? 1.0f : -1.0f) * (0.3f + 0.5f * hashf(i, 34)) * VS.clock, len);
        if (s < 0) s += len;
        vec3 p, d;
        road_at(rd, s, &p, &d);
        float side = (hashf(i, 35) - 0.5f) * 14.0f;
        p = v3_add(p, v3(-d.z * side, 0, d.x * side));
        if (v3_len(v3_sub(p, cam)) > 700.0f) continue;
        float sway = 0.9f + 0.1f * sinf(VS.clock * 3.0f + (float)i);
        vfx_game_push_sprite(em_dot, v3_add(p, v3(0, 0.9f, 0)), v3(0, 0, 0), 1.1f * sway, v4(0.05f, 0.045f, 0.04f, 0.9f - 0.5f * night));
    }
}

static void draw_birds(float night, int em_dot) {
    if (night > 0.6f) return;
    float sa = VS.f.birds_scatter_age;
    if (sa > 25.0f) return;   // scattered and gone
    static const char *const k_flocks[2] = { "vista_birds_park", "vista_birds_river" };
    for (int fl = 0; fl < 2; fl++) {
        vec3 c;
        if (!story_point_pos(k_flocks[fl], &c)) continue;
        for (int b = 0; b < 24; b++) {
            int id = fl * 100 + b;
            float r = 18.0f + 22.0f * hashf(id, 1), ph = hashf(id, 2) * 6.2831853f, h = hashf(id, 3) * 10.0f;
            float t = sa >= 0 ? VS.clock - sa : VS.clock;   // where it was when they scattered
            float ang = ph + t * 0.35f * (fl ? -1.0f : 1.0f);
            vec3 p = v3_add(c, v3(cosf(ang) * r, h + 3.0f * sinf(t * 0.7f + ph), sinf(ang) * r));
            float alpha = 1.0f;
            if (sa >= 0) {
                vec3 out = v3_norm(v3(p.x - c.x, 0.4f, p.z - c.z));
                p = v3_add(p, v3_scale(out, sa * (10.0f + 6.0f * hashf(id, 4))));
                alpha = 1.0f - sa / 25.0f;
            }
            float flap = 0.8f + 0.3f * sinf(VS.clock * 12.0f + ph * 5.0f);
            vfx_game_push_sprite(em_dot, p, v3(0, 0, 0), 0.9f * flap, v4(0.04f, 0.04f, 0.05f, alpha * (1.0f - night)));
        }
    }
}

void vista_render(vec3 cam, float night) {
    if (!VS.city) return;
    draw_damage();   // the world's state: drawn even with story_vista_life 0
    if (!VS.enable->i || !vfx_game_active()) return;
    int em_smoke = vfx_game_emitter("story_vista", 0), em_glow = vfx_game_emitter("story_vista", 1), em_dot = vfx_game_emitter("story_vista", 2);
    if (em_smoke < 0 || em_glow < 0 || em_dot < 0) return;
    const VistaFrame *f = &VS.f;
    for (int i = 0; i < f->smoke_n; i++) {
        const VistaSmoke *s = &f->smoke[i];
        int n = vista_smoke_count(s), k0 = n - (int)(s->def->life * s->def->rate) - 2;
        for (int k = k0 < 0 ? 0 : k0; k <= n; k++) {
            float p[3], size, shade, alpha;
            if (!vista_smoke_particle(s, k, p, &size, &shade, &alpha)) continue;
            vfx_game_push_sprite(em_smoke, v3(p[0], p[1], p[2]), v3(0, 0, 0), size, v4(shade, shade, shade * 1.03f, alpha));
        }
    }
    for (int i = 0; i < f->fire_n; i++) {
        const VistaSpot *s = &f->fire[i];
        float fl = 0.75f + 0.25f * sinf(VS.clock * 7.0f + (float)i * 2.1f) * sinf(VS.clock * 3.1f + (float)i);
        // A far fire reads as a glow wider than its flames; additive light loses to the fog in front of it.
        float k = (0.5f + 1.5f * night) * fl * (1.0f + v3_len(v3_sub(v3(s->pos[0], s->pos[1], s->pos[2]), cam)) / 300.0f);
        vfx_game_push_sprite(em_glow, v3(s->pos[0], s->pos[1] + s->size * 0.9f, s->pos[2]), v3(0, 0, 0), s->size * 2.5f * fl, v4(1.0f * k, 0.42f * k, 0.1f * k, 1));
        vfx_game_push_sprite(em_glow, v3(s->pos[0], s->pos[1] + s->size * 0.5f, s->pos[2]), v3(0, 0, 0), s->size * fl, v4(1.2f * k, 0.8f * k, 0.35f * k, 1));
    }
    for (int i = 0; i < f->flash_n; i++) {
        const VistaSpot *s = &f->flash[i];
        float u = s->age / fmaxf(s->time, 0.01f), k = 3.0f * (1.0f - u) * (1.0f - u) * (1.0f + v3_len(v3_sub(v3(s->pos[0], s->pos[1], s->pos[2]), cam)) / 300.0f);
        vfx_game_push_sprite(em_glow, v3(s->pos[0], s->pos[1] + 8.0f, s->pos[2]), v3(0, 0, 0), s->size * (0.4f + 0.6f * sqrtf(u)), v4(1.0f * k, 0.65f * k, 0.3f * k, 1));
    }
    for (int i = 0; i < f->heli_n; i++) draw_heli(&f->heli[i], em_glow, em_smoke);
    draw_traffic(cam, night, em_glow, em_dot);
    draw_crowd(cam, night, em_dot);
    draw_birds(night, em_dot);
}

// ------------------------------------------------------------------ console

static void cmd_story_vista(int argc, const char **argv) {
    if (argc < 2) {
        log_info("story_vista: %s, clock %.1f s, %d sequence(s):", VS.city ? "city" : "no city block", VS.clock, VS.n);
        for (int i = 0; i < VS.n; i++) log_info("  %s  age %.1f s", VS.act[i].name, VS.clock - VS.act[i].t0);
        log_info("  power %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f, traffic %.2f stall %.2f, %d heli, %d smoke, %d fire, day %d (%d damage boxes), crowd %d",
                 VS.f.power[0], VS.f.power[1], VS.f.power[2], VS.f.power[3], VS.f.power[4], VS.f.power[5], VS.f.power[6], VS.f.power[7],
                 VS.f.traffic_density, VS.f.traffic_stall, VS.f.heli_n, VS.f.smoke_n, VS.f.fire_n, VS.f.day, VS.damage_boxes, VS.f.crowd);
        return;
    }
    if (!strcmp(argv[1], "clear")) { vista_sequence("", 0, VS.clock); return; }
    if (!strcmp(argv[1], "reload")) { load_lib(); log_info("story_vista: %d sequence(s)", VS.lib.n); return; }
    // Local preview (this machine only; the chapter's own sequences come from the host).
    vista_sequence(argv[1], argc > 2 ? (float)atof(argv[2]) : 0.0f, VS.clock);
}

void vista_register(void) {
    VS.enable = cvar_reg_bool("story_vista_life", true, CVAR_EDITABLE, "Story vista: helicopters, smoke, traffic and birds outside (window lights stay)");
    cmd_register("story_vista", cmd_story_vista, "story_vista [<sequence> [age s] | clear | reload]: list the vista sequences, or preview one on this machine");
}
