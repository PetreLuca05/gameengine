#include "effects.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "render/render.h"
#include "render/mesh_gen.h"
#include "render/debug_draw.h"
#include "audio/audio.h"
#include "ui/ui.h"
#include "vfx/vfx.h"
#include "ui/vfx_editor.h"
#include "vfx/vfx_library.h"
#include "player/hands.h"
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#define MAX_DECALS 192
#define MAX_SPARKS 512
#define MAX_TRACERS 128
#define MAX_FLASHES 16
#define MAX_DMG_NUMBERS 64

typedef struct Decal { vec3 pos, normal; float size, age; u32 color; } Decal;
typedef struct Spark { vec3 pos, vel; float life, max_life; u32 color; } Spark;
typedef struct Tracer { vec3 a, b; float age; } Tracer;
typedef struct Flash { vec3 pos; float age; } Flash;
typedef struct DmgNumber { vec3 pos; float amount, age; bool head; char label[24]; } DmgNumber;

typedef struct FxSystem {
    const BasicMeshes *meshes;
    Decal decals[MAX_DECALS];
    int decal_head, decal_count;
    Spark sparks[MAX_SPARKS];
    int spark_count;
    Tracer tracers[MAX_TRACERS];
    int tracer_head;
    Flash flashes[MAX_FLASHES];
    int flash_head;
    DmgNumber numbers[MAX_DMG_NUMBERS];
    int number_head;
    Rng rng;
    Cvar *dev_damage_numbers, *fx_tracers;
} FxSystem;

#define FX (g_engine.fx)

void fx_init(const BasicMeshes *meshes) {
    FX = (FxSystem *)calloc(1, sizeof(FxSystem));
    FX->meshes = meshes;
    FX->rng = rng_make(1234);
    FX->dev_damage_numbers = cvar_reg_bool("dev_damage_numbers", true, CVAR_ARCHIVE, "show floating damage numbers");
    FX->fx_tracers = cvar_reg_bool("fx_tracers", true, CVAR_ARCHIVE, "draw bullet tracers");
    vfx_game_init(meshes);
    vfx_editor_init();
}

void fx_shutdown(void) {
    vfx_game_shutdown();
    free(FX);
    FX = NULL;
}

void fx_tracer(vec3 a, vec3 b) {
    if (!FX) return;
    Tracer *t = &FX->tracers[FX->tracer_head];
    FX->tracer_head = (FX->tracer_head + 1) % MAX_TRACERS;
    t->a = a;
    t->b = b;
    t->age = 0;
}

static void spawn_sparks(vec3 p, vec3 n, int count, u32 color, float speed) {
    for (int i = 0; i < count && FX->spark_count < MAX_SPARKS; i++) {
        Spark *s = &FX->sparks[FX->spark_count++];
        vec3 r = v3(rng_range(&FX->rng, -1, 1), rng_range(&FX->rng, -1, 1), rng_range(&FX->rng, -1, 1));
        s->pos = p;
        s->vel = v3_scale(v3_norm(v3_add(n, v3_scale(r, 0.8f))), speed * rng_range(&FX->rng, 0.4f, 1.0f));
        s->max_life = s->life = rng_range(&FX->rng, 0.15f, 0.45f);
        s->color = color;
    }
}

void fx_impact(vec3 pos, vec3 normal, int kind) {
    fx_impact_ex(pos, normal, v3_neg(normal), kind ? SURF_FLESH : SURF_DEFAULT, VFX_CAL_RIFLE, kind ? FX_IMPACT_FLESH : FX_IMPACT_DECAL);
}

static void legacy_impact(vec3 pos, vec3 normal, int kind) {
    if (kind == 0) {
        Decal *d = &FX->decals[FX->decal_head];
        FX->decal_head = (FX->decal_head + 1) % MAX_DECALS;
        if (FX->decal_count < MAX_DECALS) FX->decal_count++;
        d->pos = pos;
        d->normal = v3_norm(normal);
        d->size = rng_range(&FX->rng, 0.05f, 0.08f);
        d->age = 0;
        d->color = rgba8(0.05f, 0.05f, 0.05f, 0.92f);
        spawn_sparks(pos, normal, 8, rgba8(1.0f, 0.8f, 0.4f, 1), 4.0f);
    } else {
        spawn_sparks(pos, normal, 12, rgba8(0.7f, 0.05f, 0.05f, 1), 2.5f);
    }
}

void fx_impact_ex(vec3 pos, vec3 normal, vec3 incoming, int material, int caliber, u32 flags) {
    if (!FX) return;
    if (vfx_game_active()) { vfx_game_impact(pos, normal, incoming, material, caliber, flags); return; }
    legacy_impact(pos, normal, (flags & FX_IMPACT_FLESH) ? 1 : 0);
}

void fx_muzzle(vec3 pos, vec3 dir) { fx_muzzle_ex(pos, dir, VFX_MUZZLE_RIFLE, VFX_DEV_BARE, false); }

void fx_muzzle_ex(vec3 pos, vec3 dir, int type, int device, bool viewmodel) {
    if (!FX) return;
    if (vfx_game_active()) { vfx_game_muzzle(pos, dir, type, device, viewmodel); return; }
    Flash *f = &FX->flashes[FX->flash_head];
    FX->flash_head = (FX->flash_head + 1) % MAX_FLASHES;
    f->pos = pos;
    f->age = 0;
    spawn_sparks(pos, dir, 5, rgba8(1.0f, 0.85f, 0.4f, 1), 6.0f);
}

void fx_shell(vec3 pos, vec3 vel) { fx_shell_ex(pos, vel, VFX_CAL_RIFLE); }

void fx_shell_ex(vec3 pos, vec3 vel, int caliber) {
    if (!FX) return;
    if (vfx_game_active()) { vfx_game_shell(pos, vel, caliber); return; }
    if (FX->spark_count >= MAX_SPARKS) return;
    Spark *s = &FX->sparks[FX->spark_count++];
    s->pos = pos;
    s->vel = v3_add(vel, v3(rng_range(&FX->rng, -0.3f, 0.3f), rng_range(&FX->rng, 0, 0.5f), rng_range(&FX->rng, -0.3f, 0.3f)));
    s->max_life = s->life = 0.7f;
    s->color = rgba8(0.85f, 0.65f, 0.25f, 1);
}

void fx_damage_number(vec3 pos, float amount, bool headshot) { fx_damage_number_label(pos, amount, headshot, NULL); }

void fx_damage_number_label(vec3 pos, float amount, bool headshot, const char *label) {
    if (!FX || !FX->dev_damage_numbers->i) return;
    DmgNumber *n = &FX->numbers[FX->number_head];
    FX->number_head = (FX->number_head + 1) % MAX_DMG_NUMBERS;
    n->pos = v3_add(pos, v3(rng_range(&FX->rng, -0.15f, 0.15f), 0.1f, rng_range(&FX->rng, -0.15f, 0.15f)));
    n->amount = amount;
    n->age = 0;
    n->head = headshot;
    dg_strcpy(n->label, sizeof(n->label), label ? label : "");
}

void fx_sound(const char *name, vec3 pos, float volume) { audio_play(name, pos, volume, SND_SFX); }

void fx_update(float dt) {
    if (!FX) return;
    vfx_game_update(dt);
    for (int i = 0; i < FX->spark_count;) {
        Spark *s = &FX->sparks[i];
        s->life -= dt;
        if (s->life <= 0) { FX->sparks[i] = FX->sparks[--FX->spark_count]; continue; }
        s->vel.y -= 9.81f * dt;
        s->pos = v3_madd(s->pos, s->vel, dt);
        i++;
    }
    for (int i = 0; i < MAX_TRACERS; i++) FX->tracers[i].age += dt;
    for (int i = 0; i < MAX_FLASHES; i++) FX->flashes[i].age += dt;
    for (int i = 0; i < MAX_DMG_NUMBERS; i++) FX->numbers[i].age += dt;
    for (int i = 0; i < MAX_DECALS; i++) FX->decals[i].age += dt;
}

void fx_render(vec3 cam) {
    if (!FX) return;
    vec3 mp, mf;
    bool mv = hands_muzzle(&mp, &mf);
    vfx_game_set_local_muzzle(v3_madd(mp, mf, 0.015f), mf, mv);
    vfx_game_render(cam);
    // Decals: thin dark quads slightly off the surface.
    for (int i = 0; i < FX->decal_count; i++) {
        Decal *d = &FX->decals[i];
        if (d->age > 30.0f) continue;
        vec3 n = d->normal;
        vec3 up = fabsf(n.y) > 0.9f ? v3(0, 0, 1) : v3(0, 1, 0);
        vec3 r = v3_norm(v3_cross(up, n));
        vec3 u = v3_cross(n, r);
        mat4 m = mat4_identity();
        m.m[0] = r.x * d->size; m.m[1] = r.y * d->size; m.m[2] = r.z * d->size;
        m.m[4] = u.x * d->size; m.m[5] = u.y * d->size; m.m[6] = u.z * d->size;
        m.m[8] = n.x; m.m[9] = n.y; m.m[10] = n.z;
        vec3 p = v3_madd(d->pos, n, 0.004f);
        m.m[12] = p.x; m.m[13] = p.y; m.m[14] = p.z;
        DrawItem di;
        memset(&di, 0, sizeof(di));
        di.mesh = FX->meshes->quad;
        di.model = m;
        di.mat = material_color(v4(0.03f, 0.03f, 0.03f, 0.9f * saturate(1.0f - (d->age - 25.0f) / 5.0f)));
        di.mat.flags = MAT_UNLIT | MAT_NO_SHADOW;
        render_draw(&di);
    }
    // Sparks as short streaks.
    for (int i = 0; i < FX->spark_count; i++) {
        Spark *s = &FX->sparks[i];
        vec3 tail = v3_madd(s->pos, s->vel, -0.02f);
        dbg_line(tail, s->pos, s->color, 0, true);
    }
    // Tracers: bright streak travelling along the shot line.
    if (FX->fx_tracers->i) {
        for (int i = 0; i < MAX_TRACERS; i++) {
            Tracer *t = &FX->tracers[i];
            if (t->age > 0.12f) continue;
            float len = v3_dist(t->a, t->b);
            if (len < 0.01f) continue;
            vec3 dir = v3_scale(v3_sub(t->b, t->a), 1.0f / len);
            float head = fminf(len, t->age * 400.0f + 2.0f);
            float tail = fmaxf(0.0f, head - 6.0f);
            dbg_line(v3_madd(t->a, dir, tail), v3_madd(t->a, dir, head), rgba8(1.0f, 0.9f, 0.6f, 1), 0, true);
        }
    }
    // Muzzle flashes light the scene briefly.
    for (int i = 0; i < MAX_FLASHES; i++) {
        Flash *f = &FX->flashes[i];
        if (f->age > 0.06f) continue;
        PointLight l = { f->pos, 6.0f, v3(1.0f, 0.75f, 0.4f), 40.0f * (1.0f - f->age / 0.06f) };
        render_add_light(&l);
    }
}

void fx_draw_hud(void) {
    if (!FX) return;
    for (int i = 0; i < MAX_DMG_NUMBERS; i++) {
        DmgNumber *n = &FX->numbers[i];
        if (n->age > 1.0f || n->amount <= 0) continue;
        vec3 p = v3_add(n->pos, v3(0, n->age * 0.6f, 0));
        vec2 s;
        if (!render_world_to_screen(p, &s)) continue;
        float a = 1.0f - n->age;
        char buf[48];
        if (n->label[0]) snprintf(buf, sizeof(buf), "%.0f  %s", n->amount, n->label);
        else snprintf(buf, sizeof(buf), "%.0f", n->amount);
        u32 c = n->head ? UI_RGBA(255, 70, 50, (int)(255 * a)) : UI_RGBA(255, 230, 120, (int)(255 * a));
        ui_text_shadow(s.x, s.y, n->head ? FONT_LARGE : FONT_UI, c, UI_ALIGN_CENTER, buf);
    }
}
