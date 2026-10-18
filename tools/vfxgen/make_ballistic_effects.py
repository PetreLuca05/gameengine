#!/usr/bin/env python3
"""Writes the physically composed muzzle-flash components, tracer and ballistic impact effects to
assets/vfx/effects (docs/BALLISTICS.md). Unlike the old muzzle_<type>_<device> set, nothing here is
tuned per weapon: gameplay/ballistics/bal_muzzle.c derives every component's size, distance along
the bore, life, brightness and colour from the cartridge, barrel and device, and the game spawns
each component with those numbers (VfxSpawnParams scale / intensity / tint / count).

Conventions: scale 1 = the component's characteristic size in metres (radius, or length for the
axis flames); intensity 1 = the composer's value; the hot components are exposure aware (their
brightness follows the scene exposure, gameplay/ballistics/bal_muzzle.c bal_flash_exposure_gain).

  python tools/vfxgen/make_ballistic_effects.py [--force]
"""
import json
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
OUT = os.path.join(ROOT, "assets", "vfx", "effects")
FORCE = "--force" in sys.argv
MARK = "make_ballistic_effects.py"
FADE_FAST = [[0, 1], [0.5, 0.85], [1, 0]]
LOD = [[40, 0.8], [120, 0.5], [250, 0.3]]


def write(name, fx):
    fx = dict(fx)
    fx["name"] = name
    fx["_generated"] = MARK
    path = os.path.join(OUT, name + ".json")
    if os.path.exists(path) and not FORCE:
        with open(path, encoding="utf-8") as f:
            try:
                old = json.load(f)
            except ValueError:
                old = {}
        if old.get("_generated") != MARK:
            print("keep (hand edited):", name)
            return
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(fx, f, indent=2)
        f.write("\n")


def effect(emitters, duration=0.1, max_inst=16, light=None, priority=3, cull=600):
    fx = {"duration": duration, "priority": priority, "max_instances": max_inst, "cull_distance": cull, "lod": LOD, "emitters": emitters}
    if light:
        fx["light"] = light
    return fx


def hot(name, tex, orient, size, life, emissive, aspect=1.0, count=1, angle=0.0, color=(1, 1, 1), speed=0.02, variants=True):
    sp = {"mode": "burst", "count": count, "life": list(life), "size": list(size), "rotation": [0, 360], "speed": [speed, speed],
          "local": True, "color": list(color) + [1.0], "color2": list(color) + [1.0]}
    if angle:
        sp["angle"] = angle
    if variants:
        sp["frame_random"] = True
        sp["variant_no_repeat"] = True
    return {"name": name, "tier": "gpu", "spawn": sp,
            "update": {"size_curve": [[0, 0.7], [0.3, 1.0], [1, 1.08]], "alpha_curve": FADE_FAST},
            "render": {"mode": "add", "orient": orient, "texture": tex, "emissive": emissive, "exposure_aware": True,
                       "near_fade": 0.0, "soft": 0.02, "aspect": aspect}}


def smoke(name, count, size, life, speed, alpha, rise=0.04, wind=0.6, color=(0.62, 0.62, 0.64), dir_mode=None, angle=18, inherit=0.3):
    sp = {"mode": "burst", "count": count, "life": list(life), "size": list(size), "speed": list(speed), "angle": angle,
          "inherit": inherit, "rotation": [0, 360], "spin": [-40, 40], "color": list(color) + [alpha], "color2": [c * 0.9 for c in color] + [alpha * 0.7]}
    if dir_mode:
        sp["dir"] = dir_mode
    return {"name": name, "tier": "gpu", "spawn": sp,
            "update": {"gravity": -rise, "drag": 3.0, "wind": wind, "turbulence": {"strength": 0.5, "freq": 1.6, "speed": 0.4},
                       "size_curve": {"type": "bezier", "keys": [[0, 0.45], [0.25, 1.1], [1, 2.0]]},
                       "alpha_curve": {"type": "bezier", "keys": [[0, 0], [0.1, 1], [0.5, 0.5], [1, 0]]},
                       "frame_over_life": True, "motion_vectors": True},
            "render": {"mode": "alpha", "orient": "billboard", "lighting": "sixway", "texture": "smoke_puff", "soft": 0.25, "near_fade": 0.15}}


def distort(name, size, life, strength, speed=(0.2, 0.5), count=1):
    return {"name": name, "tier": "gpu",
            "spawn": {"mode": "burst", "count": count, "life": list(life), "size": list(size), "speed": list(speed), "color": [1, 1, 1, 1]},
            "update": {"size_curve": [[0, 0.6], [1, 1.4]], "alpha_curve": [[0, 1], [1, 0]]},
            "render": {"mode": "distort", "texture": "heat_distort", "distortion": strength, "soft": 0.1, "near_fade": 0.1}}


def muzzle_components():
    # Primary flash: the glowing gas right at the crown, dim orange, under a tick.
    write("muzzle_primary", effect([hot("glow", "glow", "billboard", (1.8, 2.2), (0.008, 0.014), 5.0, color=(1.0, 0.6, 0.25), variants=False)]))
    # Intermediate flash: the bright disc behind the Mach disk, a visible gap in front of the muzzle.
    write("muzzle_mach", effect([hot("disc", "flash_mach", "cross", (1.8, 2.2), (0.012, 0.017), 10.0)]))
    # Secondary flash: the fireball 10-50 cm out (1, 2 or 3 ticks: the composer picks the variant).
    for ticks in (1, 2, 3):
        life = (ticks / 60.0 * 0.8, ticks / 60.0 * 1.05)
        write(f"muzzle_secondary_{ticks}", effect([
            hot("petal", "flash_petal", "axis", (2.6, 3.4), life, 6.0, aspect=0.55),
            hot("ball", "flash_core", "billboard", (1.3, 1.7), life, 4.0),
        ]))
    # Flash hider: dim forward cone / spikes between the prongs (count = prongs).
    write("muzzle_prongs", effect([hot("spikes", "flash_prong", "axis", (1.6, 2.0), (0.012, 0.018), 4.0, aspect=0.35, angle=14, speed=0.05)]))
    # Compensator / brake port jet (one per port; the effect's forward is the port direction).
    write("muzzle_vent", effect([
        hot("jet", "flash_side", "axis", (1.6, 2.0), (0.025, 0.04), 5.0, aspect=0.45, speed=0.03),
        smoke("puff", 1, (0.8, 1.2), (0.25, 0.45), (1.0, 2.5), 0.12, dir_mode="socket", angle=10),
    ]))
    # Suppressor: a soft vapour wisp from the end cap.
    write("muzzle_vapor", effect([smoke("wisp", [2, 3], (1.4, 2.4), (0.3, 0.5), (0.2, 0.7), 0.16, rise=0.12, color=(0.82, 0.82, 0.85))], duration=0.05))
    # Smoke: the thin grey puff (0.2-0.5 s) drifting with the wind.
    write("muzzle_smoke", effect([smoke("puff", [1, 2], (1.4, 2.2), (0.2, 0.5), (0.4, 1.4), 0.14)], duration=0.05))
    # Lingering haze after sustained fire / a hot can: slow wisps and a shimmer.
    write("muzzle_haze", effect([smoke("wisp", 1, (2.0, 3.0), (1.5, 3.0), (0.05, 0.25), 0.05, rise=0.15, wind=0.8, inherit=0.0)], duration=0.05, max_inst=8))
    write("muzzle_can_shimmer", effect([distort("shimmer", (0.1, 0.14), (0.4, 0.7), 0.008, speed=(0.1, 0.25))], duration=0.05, max_inst=4))
    # Unburnt powder: sparks that cool within ~10 cm.
    write("muzzle_sparks", effect([{
        "name": "grains", "tier": "gpu",
        "spawn": {"mode": "burst", "count": [10, 14], "life": [0.004, 0.011], "size": [0.004, 0.008], "speed": [8.0, 20.0], "angle": 12,
                  "inherit": 0.5, "color": [1.0, 0.75, 0.4, 1.0], "color2": [1.0, 0.55, 0.2, 1.0]},
        "update": {"drag": 2.0, "alpha_curve": [[0, 1], [0.6, 0.8], [1, 0]]},
        "render": {"mode": "add", "orient": "stretch", "texture": "spark", "emissive": 6.0, "exposure_aware": True, "stretch": 0.01, "stretch_max": 8.0, "near_fade": 0.02},
    }]))
    # Blast: the pressure wave's heat-haze ring for 2-3 ticks.
    write("muzzle_blast", effect([distort("wave", (1.6, 2.0), (0.03, 0.05), 0.02, speed=(0.0, 0.05))]))
    # Near a surface: dust / debris kicked up by the blast (forward = the surface normal).
    write("muzzle_dust", effect([{
        "name": "dust", "tier": "gpu",
        "spawn": {"mode": "burst", "count": [4, 6], "life": [0.8, 1.6], "size": [0.6, 1.0], "speed": [1.0, 3.0], "dir": "socket", "angle": 70,
                  "rotation": [0, 360], "spin": [-30, 30], "color": [0.62, 0.56, 0.47, 0.35], "color2": [0.55, 0.5, 0.42, 0.25]},
        "update": {"gravity": 0.3, "drag": 3.0, "wind": 0.5, "size_curve": {"type": "bezier", "keys": [[0, 0.4], [0.3, 1.0], [1, 2.2]]},
                   "alpha_curve": {"type": "bezier", "keys": [[0, 0], [0.08, 1], [0.6, 0.5], [1, 0]]}, "frame_over_life": True},
        "render": {"mode": "alpha", "orient": "billboard", "lighting": "sixway", "texture": "dust_puff", "soft": 0.3, "near_fade": 0.2},
    }], duration=0.05, max_inst=8))
    # Flash light (1-2 ticks): radius = scale, intensity = intensity. Shadowed only at the top VFX tier.
    for name, shadow in (("muzzle_light", False), ("muzzle_light_shadow", True)):
        write(name, effect([], duration=0.03, light={"color": [1.0, 0.62, 0.3], "intensity": 1.0, "radius": 1.0, "duration": 0.025,
                                                     "exposure_aware": False, "shadow": shadow}))


def tracers():
    # Tracer: drawn by gameplay/ballistics/bal_fx.c as per-frame segment sprites with these two
    # emitters' render settings (never spawned): the velocity-stretched head (length = speed x
    # shutter) and the fading tail along the bullet's last positions (curved by drop and wind).
    def seg(name, tex, emissive):
        return {"name": name, "tier": "cpu",
                "spawn": {"mode": "burst", "count": 0, "life": [0.1, 0.1], "size": [1.0, 1.0], "color": [1, 1, 1, 1], "color2": [1, 1, 1, 1]},
                "update": {},
                "render": {"mode": "add", "orient": "segment", "texture": tex, "emissive": emissive, "exposure_aware": True, "aspect": 1.0,
                           "soft": 0.0, "near_fade": 0.5}}
    write("tracer_head", effect([seg("head", "tracer_head", 14.0), seg("tail", "glow", 5.0)], duration=0.01, max_inst=1, cull=3000))
    # A faint self-lit smoke trail left by the burning compound.
    write("tracer_smoke", effect([{
        "name": "trail", "tier": "gpu",
        "spawn": {"mode": "distance", "per_meter": 0.35, "life": [0.4, 0.9], "size": [0.05, 0.09], "speed": [0.0, 0.2], "rotation": [0, 360],
                  "color": [0.8, 0.75, 0.72, 0.06], "color2": [0.7, 0.66, 0.64, 0.04]},
        "update": {"gravity": -0.05, "drag": 2.0, "wind": 0.7, "size_curve": [[0, 0.6], [1, 2.2]], "alpha_curve": [[0, 0], [0.15, 1], [1, 0]]},
        "render": {"mode": "alpha", "orient": "billboard", "lighting": "simple", "texture": "smoke_puff", "soft": 0.2, "near_fade": 0.3},
    }], duration=30.0, max_inst=96, priority=1, cull=600))
    # Bullet trace: the shock / heat ripple along a supersonic path, seen through magnified optics.
    write("bullet_trace", effect([distort("ripple", (0.06, 0.09), (0.12, 0.25), 0.012, speed=(0.0, 0.1))], duration=0.05, max_inst=64, priority=1))
    # Vapour trail: humid / cold air condensing in the low-pressure wake of a supersonic bullet.
    write("bullet_vapor", effect([smoke("wake", 1, (0.08, 0.14), (0.25, 0.5), (0.0, 0.2), 0.10, rise=0.0, wind=0.6, color=(0.9, 0.9, 0.92), inherit=0.0)],
                                  duration=0.05, max_inst=256, priority=0))


def impacts():
    # Exit-side spall: a wider cone of fragments than the entry, along the bullet path.
    write("impact_exit_spall", effect([{
        "name": "spall", "tier": "gpu",
        "spawn": {"mode": "burst", "count": [8, 14], "life": [0.4, 0.9], "size": [0.01, 0.025], "speed": [3.0, 9.0], "dir": "incoming", "angle": 40,
                  "rotation": [0, 360], "spin": [-400, 400], "color": [0.6, 0.58, 0.55, 1.0], "color2": [0.45, 0.43, 0.4, 1.0]},
        "update": {"gravity": 1.0, "drag": 0.6, "collide": "depth", "bounce": 0.3, "friction": 0.5, "alpha_curve": [[0, 1], [0.8, 1], [1, 0]]},
        "render": {"mode": "alpha", "orient": "billboard", "texture": "debris_concrete", "lighting": "simple", "soft": 0.0},
    }, smoke("dust", [2, 3], (0.15, 0.3), (0.6, 1.4), (0.8, 2.5), 0.3, dir_mode="incoming", angle=35, color=(0.65, 0.62, 0.58))],
        duration=0.05, max_inst=32, priority=1, cull=300))
    # Steel plate: a lead splash disc around the hit (sprayed along the surface).
    write("impact_lead_splash", effect([{
        "name": "lead", "tier": "gpu",
        "spawn": {"mode": "burst", "count": [10, 18], "life": [0.15, 0.4], "size": [0.004, 0.009], "speed": [6.0, 16.0], "dir": "surface", "angle": 85,
                  "color": [0.75, 0.75, 0.78, 1.0], "color2": [0.6, 0.6, 0.62, 1.0]},
        "update": {"gravity": 1.0, "drag": 1.0, "alpha_curve": [[0, 1], [1, 0]]},
        "render": {"mode": "alpha", "orient": "stretch", "texture": "spark", "lighting": "simple", "stretch": 0.01, "stretch_max": 6.0},
    }], duration=0.05, max_inst=24, priority=1, cull=300))
    # Pyrophoric flash: a hard target hit at speed throws a tiny bright flash (visible at night).
    write("impact_pyro", effect([hot("flash", "flash_mach", "billboard", (0.08, 0.14), (0.015, 0.03), 8.0, color=(1.0, 0.9, 0.7))],
                                duration=0.04, max_inst=32, priority=1, cull=800,
                                light={"color": [1.0, 0.8, 0.5], "intensity": 2.5, "radius": 2.5, "duration": 0.02, "exposure_aware": False, "shadow": False}))


def main():
    muzzle_components()
    tracers()
    impacts()


if __name__ == "__main__":
    main()
