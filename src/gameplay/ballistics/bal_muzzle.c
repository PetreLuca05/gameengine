// Muzzle flash composer (docs/BALLISTICS.md "Muzzle flash").
#include "gameplay/ballistics/bal_muzzle.h"
#include <math.h>

#define HEAT_TAU 8.0f
#define P_ATM 0.101325f   // MPa

vec3 bal_blackbody_rgb(float kelvin) {
    // Planck spectral radiance at three representative wavelengths, normalized to the max channel.
    const double c2 = 1.4388e-2;   // m·K
    const double wl[3] = { 610e-9, 550e-9, 465e-9 };
    double v[3], mx = 0.0;
    for (int i = 0; i < 3; i++) {
        double l5 = wl[i] * wl[i] * wl[i] * wl[i] * wl[i];
        v[i] = 1.0 / (l5 * (exp(c2 / (wl[i] * (double)kelvin)) - 1.0));
        if (v[i] > mx) mx = v[i];
    }
    return v3((float)(v[0] / mx), (float)(v[1] / mx), (float)(v[2] / mx));
}

float bal_flash_exposure_gain(float ev, float ev_night, float daylight) {
    // Undo the camera's adaptation relative to night (the tonemapper scales by 2^-EV), then apply
    // the eye's: an adapted daylight eye sees the same flash at ~0.15x.
    return exp2f(ev - ev_night) * lerpf(1.0f, BAL_FLASH_DAY_RATIO, saturate(daylight));
}

float bal_flash_perceived(float ev, float ev_night, float daylight) {
    return bal_flash_exposure_gain(ev, ev_night, daylight) * exp2f(-(ev - ev_night));
}

static BalFlashParams comp(float size, float dist, float life, float intensity, vec3 rgb) {
    BalFlashParams p = { size, dist, life, intensity, 1.0f, rgb };
    return p;
}

void bal_muzzle_compose(const BalMuzzleIn *in, BalMuzzleOut *out) {
    memset(out, 0, sizeof(*out));
    const BalCartridge *c = in->cart;
    const BalDevice *d = in->dev;
    float L = fmaxf(in->barrel, 0.03f);
    float charge = fmaxf(c->powder, 0.05f);
    float cq = cbrtf(charge);   // cube-root energy scaling (Hopkinson-Cranz) for flame / blast sizes

    // Short barrels vent propellant that has not burnt yet: the fuel of the secondary flash.
    float u = clampf(0.15f + 0.85f * (1.0f - L / fmaxf(c->burn_length, 0.05f)), 0.05f, 0.9f);
    out->unburnt = u;
    // Muzzle pressure by isentropic expansion over the extra bore volume.
    float pm = c->muzzle_pressure * powf(fmaxf(c->ref_barrel, 0.05f) / L, 1.2f);
    out->pressure = pm;
    float gain = in->exposure_gain > 0.0f ? in->exposure_gain : 1.0f;
    float fast = in->speed > 800.0f ? 1.0f : 0.0f;

    // Primary flash: the glowing gas at the crown, dim and orange, under one tick.
    out->c[BFC_PRIMARY] = comp(0.012f + 0.012f * cq, 0.0f, 0.012f, 0.6f * gain, bal_blackbody_rgb(1800.0f));
    // Intermediate flash at the Mach disk of the under-expanded jet (Ashkenas-Sherman
    // x = 0.67·d·sqrt(p/p_atm)); kept in the 2-6 cm band so the gap reads at game scale.
    float xm = clampf(0.67f * c->diameter * sqrtf(pm / P_ATM), 0.02f, 0.06f);
    vec3 hot = bal_blackbody_rgb(2500.0f);
    hot = v3_lerp(hot, v3(0.75f, 0.85f, 1.0f), 0.08f * fast);   // high-velocity rifle: blue tinge
    out->c[BFC_MACH] = comp(0.6f * xm, xm, 1.0f / 60.0f, 1.6f * gain, hot);
    // Secondary flash: unburnt gas re-igniting as it mixes with air.
    float rs = clampf(0.06f * cq * (1.0f + 1.8f * u), 0.04f, 0.35f);
    float ds = clampf(0.10f + 0.12f * cq * (1.0f + u), 0.10f, 0.50f);
    float ticks = 1.0f + floorf(2.0f * u + 0.5f);
    out->c[BFC_SECONDARY] = comp(rs, ds, ticks / 60.0f, 1.0f * gain, bal_blackbody_rgb(1700.0f));
    // Hider: what is left burns as a dim forward cone between the prongs.
    out->c[BFC_PRONGS] = comp(0.7f * rs, 0.5f * ds, 1.0f / 60.0f, 0.8f * gain, bal_blackbody_rgb(1900.0f));
    // Compensator / brake ports: the gas diverted sideways and up.
    out->c[BFC_VENTS] = comp(0.6f * rs, 0.0f, ticks / 60.0f, 0.9f * gain, bal_blackbody_rgb(1800.0f));
    // Suppressor: a soft vapour wisp from the end cap.
    out->c[BFC_VAPOR] = comp(0.04f + 0.03f * cq, 0.0f, 0.4f, 0.35f, v3(0.85f, 0.85f, 0.88f));
    // Smoke: a thin grey puff that drifts with the wind.
    out->c[BFC_SMOKE] = comp(0.06f + 0.06f * cq, 0.05f, 0.2f + 0.3f * u, 0.25f + 0.3f * u, v3(0.62f, 0.62f, 0.64f));
    // Unburnt powder grains: many from pistols / short barrels, cooling within ~10 cm.
    float short_b = clampf(0.3f / L, 0.3f, 2.0f);
    out->c[BFC_SPARKS] = comp(0.004f, 0.0f, 0.01f, 1.2f * gain, bal_blackbody_rgb(1600.0f));
    out->c[BFC_SPARKS].count = floorf(24.0f * u * short_b + 0.5f) / 12.0f;
    // Blast: the pressure wave refracting light for 2-3 ticks.
    out->c[BFC_BLAST] = comp(0.12f * cbrtf(pm / 10.0f), 0.05f, (2.0f + (pm > 50.0f ? 1.0f : 0.0f)) / 60.0f, 1.0f, v3(1, 1, 1));
    // Flash light.
    float li = 6.0f * cq * cq * (0.3f + u);
    // The light is physical (it lights the scene, which the camera exposes): no exposure gain.
    out->c[BFC_LIGHT] = comp(0.0f, 0.1f, (1.0f + (u > 0.4f ? 1.0f : 0.0f)) / 60.0f, li, bal_blackbody_rgb(2200.0f));
    out->light_radius = 3.0f + 2.0f * cq;

    // The device.
    for (int i = 0; i < BFC_COUNT; i++) out->c[i].intensity *= d->mul[i];
    if (d->kind == BAL_DEV_SUPPRESSOR && in->pop) {
        // Oxygen in a cold can burns with the first round's gas.
        out->c[BFC_SECONDARY].intensity = in->dev->pop_flash * gain;
        out->c[BFC_SECONDARY].dist = 0.03f;
        out->c[BFC_LIGHT].intensity = fmaxf(out->c[BFC_LIGHT].intensity, 0.6f * li);
    }
    // Dust kicked up by the blast when the muzzle is near the ground / a wall.
    out->dust = in->near_surface && d->mul[BFC_BLAST] > 0.05f;
    out->dust_scale = cq * d->mul[BFC_BLAST];
    // Lingering haze: sustained fire, or a hot suppressor can.
    out->haze = in->heat > 6.0f * charge ||
                (d->kind == BAL_DEV_SUPPRESSOR && in->can_heat >= fmaxf(d->can_heat_rounds, 1.0f));
}

static float decay(float v, float dt, float tau) { return tau > 0.0f ? v * expf(-dt / tau) : 0.0f; }

float bal_muzzle_heat_at(const BalMuzzleHeat *h, double now) {
    return h->fired ? decay(h->heat, (float)(now - h->last_shot), HEAT_TAU) : 0.0f;
}

float bal_muzzle_can_heat_at(const BalMuzzleHeat *h, const BalDevice *dev, double now) {
    return h->fired ? decay(h->can_heat, (float)(now - h->last_shot), dev->can_heat_tau) : 0.0f;
}

bool bal_muzzle_heat_shot(BalMuzzleHeat *h, const BalCartridge *c, const BalDevice *dev, double now) {
    bool pop = false;
    if (dev->kind == BAL_DEV_SUPPRESSOR && dev->pop_cold_s > 0.0f)
        pop = !h->fired || (now - h->last_shot) >= dev->pop_cold_s;
    h->heat = bal_muzzle_heat_at(h, now) + c->powder;
    h->can_heat = bal_muzzle_can_heat_at(h, dev, now) + 1.0f;
    h->last_shot = now;
    h->fired = true;
    return pop;
}
