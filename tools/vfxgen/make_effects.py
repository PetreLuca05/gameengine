#!/usr/bin/env python3
"""Writes the milestone-1 effect set to assets/vfx/effects/*.json from parameter tables:
muzzle flashes (6 weapon types x 4 muzzle devices), bullet impacts (every surface material +
flesh headshot), shell casings and the GPU stress test.

The JSON files are the source of truth afterwards (edit them by hand or in the in-game editor,
F7 / vfx_editor). Re-running this script only rewrites files it generated itself unless --force:
files whose "_generated" marker is missing (hand edited and saved by the editor) are kept.

  python tools/vfxgen/make_effects.py [--force]
"""
import json
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
OUT = os.path.join(ROOT, "assets", "vfx", "effects")
FORCE = "--force" in sys.argv
MARK = "make_effects.py"


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


# ------------------------------------------------------------------ muzzle flashes

# type: core size, side flame length, side count, smoke, embers, light intensity, light radius
TYPES = {
    "pistol":  dict(core=0.10, side=0.12, sides=2, smoke=2, embers=3, light=7.0, radius=4.0, smoke_size=0.12),
    "smg":     dict(core=0.11, side=0.13, sides=2, smoke=2, embers=3, light=7.0, radius=4.0, smoke_size=0.12),
    "rifle":   dict(core=0.16, side=0.22, sides=3, smoke=3, embers=5, light=10.0, radius=5.0, smoke_size=0.16),
    "lmg":     dict(core=0.19, side=0.26, sides=3, smoke=4, embers=6, light=11.0, radius=5.5, smoke_size=0.18),
    "shotgun": dict(core=0.24, side=0.28, sides=4, smoke=5, embers=10, light=13.0, radius=6.0, smoke_size=0.22),
    "sniper":  dict(core=0.26, side=0.34, sides=4, smoke=5, embers=8, light=14.0, radius=6.5, smoke_size=0.24),
}

FADE_FAST = [[0, 1], [0.5, 0.85], [1, 0]]


def core_emitter(size, tex="flash_core", emissive=6.0, aspect=1.0, orient="cross"):
    return {
        "name": "core", "tier": "gpu",
        "spawn": {"mode": "burst", "count": 1, "life": [0.032, 0.05], "size": [size * 0.85, size * 1.15], "rotation": [0, 360],
                  "speed": [0.01, 0.01], "local": True, "frame_random": True, "variant_no_repeat": True,
                  "color": [1.0, 0.9, 0.75, 1.0], "color2": [1.0, 0.8, 0.6, 1.0]},
        "update": {"size_curve": [[0, 0.75], [0.3, 1.0], [1, 1.1]], "alpha_curve": FADE_FAST},
        "render": {"mode": "add", "orient": orient, "texture": tex, "emissive": emissive, "exposure_aware": True,
                   "near_fade": 0.0, "soft": 0.02, "aspect": aspect},
    }


def side_emitter(length, count, dir_mode="socket", angle=4.0, tex="flash_side", emissive=5.0):
    return {
        "name": "side", "tier": "gpu",
        "spawn": {"mode": "burst", "count": count, "life": [0.03, 0.045], "size": [length * 0.8, length * 1.2],
                  "speed": [0.02, 0.02], "dir": dir_mode, "angle": angle, "local": True, "frame_random": True,
                  "color": [1.0, 0.85, 0.6, 1.0], "color2": [1.0, 0.7, 0.45, 1.0]},
        "update": {"size_curve": [[0, 0.6], [0.4, 1.0], [1, 1.05]], "alpha_curve": FADE_FAST},
        "render": {"mode": "add", "orient": "axis", "texture": tex, "emissive": emissive, "exposure_aware": True,
                   "near_fade": 0.0, "soft": 0.02, "aspect": 0.45},
    }


def smoke_emitter(count, size, speed=(0.4, 1.4), life=(0.6, 1.3), alpha=0.13):
    return {
        "name": "smoke", "tier": "gpu",
        "spawn": {"mode": "burst", "count": [max(1, count // 2), max(1, count - 1)], "life": list(life), "size": [size * 0.5, size * 0.85],
                  "speed": list(speed), "angle": 18, "offset": 0.03, "inherit": 0.4, "rotation": [0, 360], "spin": [-40, 40],
                  "color": [0.78, 0.77, 0.75, alpha], "color2": [0.68, 0.67, 0.66, alpha * 0.7]},
        "update": {"gravity": -0.04, "drag": 3.2, "wind": 0.35, "turbulence": {"strength": 0.5, "freq": 1.6, "speed": 0.4},
                   "size_curve": {"type": "bezier", "keys": [[0, 0.45], [0.25, 1.2], [1, 2.2]]},
                   "alpha_curve": {"type": "bezier", "keys": [[0, 0], [0.08, 1], [0.5, 0.55], [1, 0]]},
                   "frame_over_life": True, "motion_vectors": True},
        "render": {"mode": "alpha", "orient": "billboard", "lighting": "sixway", "texture": "smoke_puff", "soft": 0.25, "near_fade": 0.2},
    }


def ember_emitter(count, speed=(3.0, 9.0), angle=22):
    return {
        "name": "embers", "tier": "gpu",
        "spawn": {"mode": "burst", "count": [max(0, count - 2), count + 2], "life": [0.08, 0.32], "size": [0.008, 0.016],
                  "speed": list(speed), "angle": angle, "inherit": 0.5,
                  "color": [1.0, 0.75, 0.4, 1.0], "color2": [1.0, 0.55, 0.2, 1.0]},
        "update": {"gravity": 0.5, "drag": 1.5, "collide": "depth", "bounce": 0.3, "friction": 0.5, "alpha_curve": [[0, 1], [0.7, 0.8], [1, 0]]},
        "render": {"mode": "add", "orient": "stretch", "texture": "spark", "emissive": 5.0, "stretch": 0.015, "stretch_max": 10.0, "near_fade": 0.05},
    }


def heat_emitter(size):
    return {
        "name": "heat", "tier": "gpu",
        "spawn": {"mode": "burst", "count": 1, "life": [0.12, 0.2], "size": [size * 1.5, size * 2.0], "speed": [0.3, 0.6], "offset": 0.05,
                  "color": [1, 1, 1, 1]},
        "update": {"size_curve": [[0, 0.6], [1, 1.4]], "alpha_curve": [[0, 1], [1, 0]]},
        "render": {"mode": "distort", "texture": "heat_distort", "distortion": 0.015, "soft": 0.1, "near_fade": 0.1},
    }


def muzzle(type_name, device):
    t = TYPES[type_name]
    ems = []
    light = {"color": [1.0, 0.68, 0.36], "intensity": t["light"], "radius": t["radius"], "duration": 0.034, "exposure_aware": True}
    if device == "bare":
        ems.append(core_emitter(t["core"]))
        ems.append(side_emitter(t["side"], t["sides"]))
        ems.append(smoke_emitter(t["smoke"], t["smoke_size"]))
        ems.append(ember_emitter(t["embers"]))
        ems.append(heat_emitter(t["core"]))
    elif device == "hider":
        # Flash hider: small core, the flame is broken into short prongs, less visible light.
        ems.append(core_emitter(t["core"] * 0.7, emissive=4.5))
        ems.append(side_emitter(t["side"] * 0.45, 4, dir_mode="socket", angle=28, emissive=3.5))
        ems.append(smoke_emitter(t["smoke"], t["smoke_size"]))
        ems.append(ember_emitter(max(1, t["embers"] // 2)))
        light["intensity"] *= 0.6
    elif device == "comp":
        # Compensator: gas vented sideways / up, short forward core.
        ems.append(core_emitter(t["core"] * 0.8, emissive=5.0))
        ems.append(side_emitter(t["side"] * 0.7, 2, dir_mode="side", angle=6, emissive=4.5))
        sm = smoke_emitter(t["smoke"] + 1, t["smoke_size"])
        sm["spawn"]["dir"] = "side"
        sm["spawn"]["angle"] = 25
        ems.append(sm)
        em = ember_emitter(t["embers"])
        em["spawn"]["dir"] = "side"
        ems.append(em)
        ems.append(heat_emitter(t["core"]))
    else:  # suppressor
        ems.append(core_emitter(t["core"] * 0.35, tex="flash_suppressed", emissive=1.6, orient="billboard"))
        sm = smoke_emitter(t["smoke"] + 2, t["smoke_size"] * 0.8, speed=(0.15, 0.6), life=(0.9, 1.8), alpha=0.22)
        ems.append(sm)
        light["intensity"] *= 0.12
        light["radius"] *= 0.6
    return {"duration": 0.1, "priority": 3, "max_instances": 12, "cull_distance": 400,
            "lod": [[30, 0.75], [80, 0.45], [160, 0.25]], "light": light, "emitters": ems}


# ------------------------------------------------------------------ impacts

def sparks(count, speed=(4.0, 12.0), life=(0.12, 0.45), angle=38, color=(1.0, 0.72, 0.35), emissive=5.0, light=None):
    e = {
        "name": "sparks", "tier": "gpu",
        "spawn": {"mode": "burst", "count": [max(1, count // 2), count], "life": list(life), "size": [0.006, 0.013], "speed": list(speed),
                  "dir": "reflect", "dir_bias": 0.35, "angle": angle, "color": list(color) + [1.0], "color2": [1.0, 0.5, 0.15, 1.0]},
        "update": {"gravity": 1.0, "drag": 0.8, "collide": "depth", "bounce": 0.35, "friction": 0.3,
                   "alpha_curve": [[0, 1], [0.6, 0.9], [1, 0]], "size_curve": [[0, 1], [1, 0.6]]},
        "render": {"mode": "add", "orient": "stretch", "texture": "spark", "emissive": emissive, "stretch": 0.012, "stretch_max": 12.0, "near_fade": 0.05},
    }
    return e


def hit_flash(size=0.08, color=(1.0, 0.8, 0.55), emissive=3.0):
    return {
        "name": "hitflash", "tier": "cpu",
        "spawn": {"mode": "burst", "count": 1, "life": [0.03, 0.05], "size": [size * 0.8, size * 1.2], "offset": 0.01,
                  "dir": "normal", "color": list(color) + [1.0]},
        "update": {"alpha_curve": FADE_FAST},
        "render": {"mode": "add", "texture": "ember", "emissive": emissive, "near_fade": 0.05},
    }


def dust(color, count=3, size=0.2, speed=(0.6, 2.2), life=(0.9, 2.2), alpha=0.5, angle=35, tex="dust_puff", grow=3.0):
    return {
        "name": "dust", "tier": "gpu",
        "spawn": {"mode": "burst", "count": [max(1, count - 1), count + 1], "life": list(life), "size": [size * 0.6, size * 1.1], "speed": list(speed),
                  "dir": "normal", "angle": angle, "offset": 0.02, "rotation": [0, 360], "spin": [-30, 30],
                  "color": list(color) + [alpha], "color2": [c * 0.85 for c in color] + [alpha * 0.8]},
        "update": {"gravity": 0.03, "drag": 3.5, "wind": 0.4, "turbulence": {"strength": 0.35, "freq": 1.2, "speed": 0.3},
                   "size_curve": {"type": "bezier", "keys": [[0, 0.4], [0.2, 1.3], [1, grow]]},
                   "alpha_curve": {"type": "bezier", "keys": [[0, 0], [0.06, 1], [0.45, 0.6], [1, 0]]},
                   "frame_over_life": True, "motion_vectors": True},
        "render": {"mode": "alpha", "lighting": "sixway", "texture": tex, "soft": 0.3, "near_fade": 0.25},
    }


def debris(tex, count, color=(1, 1, 1), speed=(2.0, 6.0), size=(0.012, 0.03), life=(0.6, 1.3), gravity=1.0, angle=45, bounce=0.3):
    return {
        "name": "debris", "tier": "gpu",
        "spawn": {"mode": "burst", "count": [max(1, count // 2), count], "life": list(life), "size": list(size), "speed": list(speed),
                  "dir": "reflect", "dir_bias": 0.55, "angle": angle, "rotation": [0, 360], "spin": [-720, 720], "frame_random": True,
                  "color": list(color) + [1.0]},
        "update": {"gravity": gravity, "drag": 0.4, "collide": "depth", "bounce": bounce, "friction": 0.5, "alpha_curve": [[0, 1], [0.8, 1], [1, 0]]},
        "render": {"mode": "alpha", "lighting": "simple", "texture": tex, "soft": 0.02, "near_fade": 0.1},
    }


def chunks_cpu(mesh, count, color, size=(0.015, 0.035), speed=(1.5, 4.5)):
    return {
        "name": "chunks", "tier": "cpu",
        "spawn": {"mode": "burst", "count": [0, count], "life": [2.5, 4.0], "size": list(size), "speed": list(speed),
                  "dir": "reflect", "dir_bias": 0.6, "angle": 40, "spin": [-900, 900], "color": list(color) + [1.0]},
        "update": {"gravity": 1.0, "drag": 0.2, "collide": "physics", "bounce": 0.3, "friction": 0.6, "alpha_curve": [[0, 1], [0.85, 1], [1, 0]]},
        "render": {"mode": "mesh", "mesh": mesh},
    }


def decal(tex, size=(0.07, 0.11), life=90, depth=0.05, tint=(1, 1, 1, 1)):
    # Readability: holes are drawn ~1.6x real scale (a 7.62 mm hole + chipping is tiny at 10 m).
    size = [round(v * 1.6, 3) for v in size]
    return {"texture": tex, "normal": tex + "_n", "size": size, "lifetime": life, "depth": depth, "variants": 4, "tint": list(tint)}


def impact(emitters, dec=None, light=None, sound=None, max_inst=48):
    fx = {"duration": 0.1, "priority": 1, "max_instances": max_inst, "cull_distance": 160,
          "lod": [[25, 0.7], [60, 0.4], [110, 0.2]], "emitters": emitters}
    if dec:
        fx["decal"] = dec
    if light:
        fx["light"] = light
    if sound:
        fx["sound"] = sound
    return fx


SPARK_LIGHT = {"color": [1.0, 0.7, 0.4], "intensity": 3.0, "radius": 2.0, "duration": 0.05, "exposure_aware": True}

IMPACTS = {
    "concrete": impact([hit_flash(), sparks(6, emissive=3.0), dust([0.62, 0.61, 0.58]), debris("debris_concrete", 10),
                        chunks_cpu("shard_concrete", 2, [0.58, 0.57, 0.55])], decal("decal_concrete")),
    "brick": impact([hit_flash(), sparks(4, emissive=2.5), dust([0.6, 0.38, 0.3]), debris("debris_brick", 10),
                     chunks_cpu("shard_brick", 2, [0.55, 0.3, 0.22])], decal("decal_brick")),
    "plaster": impact([hit_flash(0.06), dust([0.88, 0.87, 0.84], count=4, size=0.24, alpha=0.6), debris("debris_plaster", 12, speed=(1.5, 4.5))],
                      decal("decal_plaster", size=(0.08, 0.12))),
    "metal_thin": impact([hit_flash(0.1, emissive=4.0), sparks(18, speed=(5, 14), emissive=6.0),
                          dust([0.5, 0.5, 0.52], count=1, size=0.12, alpha=0.25)], decal("decal_metal", size=(0.05, 0.07)), SPARK_LIGHT),
    "metal_thick": impact([hit_flash(0.12, emissive=5.0), sparks(26, speed=(6, 16), life=(0.15, 0.6), emissive=7.0),
                           dust([0.45, 0.45, 0.47], count=1, size=0.1, alpha=0.2)], decal("decal_metal", size=(0.045, 0.06)), SPARK_LIGHT),
    "wood": impact([dust([0.55, 0.42, 0.28], count=2, size=0.16, alpha=0.4), debris("debris_wood", 12, size=(0.015, 0.04), speed=(2, 5)),
                    chunks_cpu("splinter_wood", 3, [0.6, 0.44, 0.27], size=(0.02, 0.05))], decal("decal_wood")),
    "dirt": impact([dust([0.42, 0.33, 0.24], count=4, size=0.3, speed=(1, 3.5), alpha=0.65, angle=25), debris("debris_dirt", 16, speed=(2.5, 7), angle=25)],
                   decal("decal_dirt", size=(0.08, 0.12))),
    "sand": impact([dust([0.78, 0.68, 0.5], count=5, size=0.32, speed=(1, 3.5), alpha=0.6, angle=22),
                    debris("debris_dirt", 18, color=(1.6, 1.5, 1.2), speed=(2.5, 7), angle=22, size=(0.006, 0.014))],
                   decal("decal_dirt", size=(0.09, 0.12), tint=(1.5, 1.4, 1.1, 0.8))),
    "gravel": impact([hit_flash(0.05), sparks(3, emissive=2.0), dust([0.55, 0.53, 0.5], count=3, size=0.22), debris("debris_concrete", 14, speed=(3, 8))],
                     decal("decal_dirt", size=(0.07, 0.1))),
    "grass": impact([dust([0.38, 0.33, 0.24], count=2, size=0.2, alpha=0.45), debris("grass_bits", 14, speed=(1.5, 4), gravity=0.6, size=(0.02, 0.05)),
                     debris("debris_dirt", 8, speed=(2, 5))], decal("decal_dirt", size=(0.06, 0.1))),
    "glass": impact([hit_flash(0.06, color=(0.9, 0.95, 1.0), emissive=2.0), debris("debris_glass", 18, speed=(2, 6), size=(0.01, 0.03), bounce=0.45),
                     chunks_cpu("shard_glass", 4, [0.8, 0.9, 0.95], size=(0.02, 0.05)), dust([0.85, 0.9, 0.95], count=1, size=0.12, alpha=0.25)],
                    decal("decal_glass", size=(0.08, 0.14), life=120)),
    "water": impact([
        {"name": "splash", "tier": "gpu",
         "spawn": {"mode": "burst", "count": 1, "life": [0.45, 0.6], "size": [0.35, 0.5], "dir": "up", "offset": 0.12, "color": [0.85, 0.9, 0.95, 0.9]},
         "update": {"frame_over_life": True, "motion_vectors": True, "alpha_curve": [[0, 1], [0.7, 0.8], [1, 0]]},
         "render": {"mode": "alpha", "lighting": "simple", "texture": "water_splash", "soft": 0.1, "near_fade": 0.2}},
        {"name": "droplets", "tier": "gpu",
         "spawn": {"mode": "burst", "count": [10, 18], "life": [0.4, 0.8], "size": [0.008, 0.018], "speed": [1.5, 4.0], "dir": "up", "angle": 30,
                   "color": [0.8, 0.88, 0.95, 0.8]},
         "update": {"gravity": 1.0, "drag": 0.3, "collide": "depth", "kill_on_hit": True},
         "render": {"mode": "alpha", "lighting": "simple", "orient": "stretch", "texture": "ember", "stretch": 0.01, "stretch_max": 4}},
        {"name": "ripple", "tier": "gpu",
         "spawn": {"mode": "burst", "count": 1, "life": [0.8, 1.1], "size": [0.15, 0.2], "dir": "normal", "speed": [0.001, 0.001], "offset": 0.005,
                   "color": [0.9, 0.95, 1.0, 0.7]},
         "update": {"size_curve": [[0, 0.4], [1, 3.0]], "alpha_curve": [[0, 1], [1, 0]]},
         "render": {"mode": "alpha", "orient": "normal", "texture": "water_ripple", "soft": 0.0, "near_fade": 0.1}}]),
    "flesh": impact([
        {"name": "mist", "tier": "gpu",
         "spawn": {"mode": "burst", "count": [2, 3], "life": [0.25, 0.45], "size": [0.12, 0.2], "speed": [0.4, 1.4], "dir": "incoming", "angle": 35,
                   "rotation": [0, 360], "color": [0.42, 0.03, 0.03, 0.75], "color2": [0.3, 0.02, 0.02, 0.6]},
         "update": {"drag": 4.0, "gravity": 0.1, "size_curve": [[0, 0.5], [1, 1.8]], "alpha_curve": [[0, 1], [0.4, 0.7], [1, 0]], "frame_over_life": True},
         "render": {"mode": "alpha", "lighting": "simple", "texture": "blood_mist", "soft": 0.15, "near_fade": 0.2}},
        {"name": "drops", "tier": "cpu",
         "spawn": {"mode": "burst", "count": [3, 6], "life": [0.6, 1.2], "size": [0.012, 0.025], "speed": [1.5, 4.0], "dir": "incoming", "angle": 30,
                   "color": [0.4, 0.02, 0.02, 1.0]},
         "update": {"gravity": 1.0, "drag": 0.2, "collide": "physics", "kill_on_hit": True, "sub_emitter": {"on": "collision", "effect": "blood_splat"}},
         "render": {"mode": "alpha", "lighting": "simple", "orient": "stretch", "texture": "blood_drops", "stretch": 0.01, "stretch_max": 3}},
        {"name": "spray", "tier": "gpu",
         "spawn": {"mode": "burst", "count": [4, 8], "life": [0.15, 0.35], "size": [0.01, 0.02], "speed": [2, 5], "dir": "normal", "angle": 40,
                   "color": [0.45, 0.03, 0.03, 0.9]},
         "update": {"gravity": 1.0, "drag": 1.0},
         "render": {"mode": "alpha", "lighting": "simple", "orient": "stretch", "texture": "blood_drops", "stretch": 0.01, "stretch_max": 3}}],
        sound="impact_flesh"),
    "cloth": impact([dust([0.5, 0.5, 0.5], count=2, size=0.12, alpha=0.35), debris("cloth_fibers", 8, speed=(1, 3), gravity=0.3, color=(0.7, 0.7, 0.75))]),
    "tile": impact([hit_flash(0.06), sparks(5, emissive=3.0), dust([0.85, 0.85, 0.86], count=2, size=0.16, alpha=0.4), debris("debris_tile", 12, speed=(2, 6), bounce=0.4),
                    chunks_cpu("shard_tile", 2, [0.82, 0.84, 0.86], size=(0.015, 0.03))], decal("decal_tile")),
    "asphalt": impact([hit_flash(0.06), sparks(5, emissive=3.0), dust([0.32, 0.31, 0.3], count=3, size=0.22), debris("debris_concrete", 10, color=(0.5, 0.5, 0.5))],
                      decal("decal_concrete", tint=(0.55, 0.55, 0.55, 1))),
}


def headshot():
    fx = json.loads(json.dumps(IMPACTS["flesh"]))
    for e in fx["emitters"]:
        if e["name"] == "mist":
            e["spawn"]["count"] = [4, 6]
            e["spawn"]["size"] = [0.2, 0.32]
        if e["name"] == "drops":
            e["spawn"]["count"] = [8, 12]
            e["spawn"]["speed"] = [2.0, 5.0]
    fx["sound"] = "impact_flesh"
    return fx


def shell(name, mesh, size, color):
    return {"duration": 0.05, "priority": 1, "max_instances": 64, "cull_distance": 60, "emitters": [
        {"name": "casing", "tier": "cpu",
         "spawn": {"mode": "burst", "count": 1, "life": [3.0, 4.0], "size": [size, size], "speed": [0.0, 0.3], "inherit": 1.0,
                   "spin": [-1400, 1400], "color": color + [1.0]},
         "update": {"gravity": 1.0, "drag": 0.15, "collide": "physics", "bounce": 0.35, "friction": 0.45, "alpha_curve": [[0, 1], [0.9, 1], [1, 0]]},
         "render": {"mode": "mesh", "mesh": mesh}},
        {"name": "wisp", "tier": "gpu",
         "spawn": {"mode": "burst", "count": 1, "life": [0.3, 0.5], "size": [0.03, 0.05], "inherit": 0.6, "color": [0.75, 0.75, 0.75, 0.18]},
         "update": {"drag": 3.0, "gravity": -0.05, "size_curve": [[0, 1], [1, 2.5]], "alpha_curve": [[0, 1], [1, 0]], "frame_over_life": True},
         "render": {"mode": "alpha", "lighting": "sixway", "texture": "smoke_puff", "soft": 0.05}}]}


def main():
    os.makedirs(OUT, exist_ok=True)
    for t in TYPES:
        for d in ("bare", "hider", "comp", "suppressor"):
            write(f"muzzle_{t}_{d}", muzzle(t, d))
    for m, fx in IMPACTS.items():
        write(f"impact_{m}", fx)
    write("impact_flesh_head", headshot())
    write("blood_splat", {"duration": 0.05, "priority": 0, "max_instances": 64, "emitters": [],
                          "decal": decal("decal_blood", size=(0.05, 0.12), life=120, depth=0.08)})
    write("shell_pistol", shell("shell_pistol", "casing_pistol", 0.02, [0.85, 0.65, 0.25]))
    write("shell_rifle", shell("shell_rifle", "casing_rifle", 0.045, [0.85, 0.65, 0.25]))
    write("shell_shotgun", shell("shell_shotgun", "shell_shotgun", 0.06, [0.7, 0.12, 0.1]))
    write("stress_test", {"duration": 1.0, "priority": 3, "max_instances": 2, "cull_distance": 1000, "lod": [[1e9, 1.0], [1e9, 1.0], [1e9, 1.0]], "emitters": [
        {"name": "swarm", "tier": "gpu",
         "spawn": {"mode": "rate", "rate": 600000, "duration": 1.0, "count": 1, "shape": "sphere", "radius": 4.0, "life": [4.0, 6.0],
                   "size": [0.02, 0.04], "speed": [0.2, 1.0], "dir": "random", "color": [0.4, 0.7, 1.0, 1.0], "color2": [1.0, 0.5, 0.2, 1.0]},
         "update": {"turbulence": {"strength": 3.0, "freq": 0.4, "speed": 0.4}, "drag": 0.5, "alpha_curve": [[0, 0], [0.1, 1], [0.8, 1], [1, 0]]},
         "render": {"mode": "add", "texture": "glow", "emissive": 0.6, "soft": 0.0, "near_fade": 0.1}}]})
    print("effects written to", OUT)


if __name__ == "__main__":
    main()
