"""Writes the flesh impact effects (assets/vfx/effects/flesh_*.json, wound / blood decals).

Spawned by src/gameplay/flesh_fx.c (rules in assets/vfx/flesh_rules.json). Colours are the
reference human blood: dark, desaturated red (the per-character "blood" block tints them).
Run: python tools/vfxgen/make_flesh_effects.py
"""
import json
import os

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
OUT = os.path.join(ROOT, "assets", "vfx", "effects")

BLOOD = [0.26, 0.022, 0.018]
BLOOD_DARK = [0.17, 0.016, 0.013]
MIST = [0.16, 0.026, 0.026]
MIST_DARK = [0.10, 0.018, 0.018]
LOD = [[25, 0.75], [60, 0.45], [110, 0.2]]


def write(name, fx):
    fx = dict(fx)
    fx["name"] = name
    fx["_generated"] = "make_flesh_effects.py"
    with open(os.path.join(OUT, name + ".json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(fx, f, indent=2)
        f.write("\n")


def effect(emitters, max_inst=48, prio=1, dur=0.1, cull=150):
    return {"duration": dur, "priority": prio, "max_instances": max_inst, "cull_distance": cull, "lod": LOD, "emitters": emitters}


def mist(count, size, speed, life=(0.3, 0.6), angle=30, alpha=0.6, drag=4.0, wind=0.0, gravity=0.05, scatter=0.9, name="mist", dir="incoming"):
    return {"name": name, "tier": "gpu",
            "spawn": {"mode": "burst", "count": list(count), "life": list(life), "size": list(size), "speed": list(speed), "dir": dir, "angle": angle,
                      "rotation": [0, 360], "spin": [-20, 20], "color": MIST + [alpha], "color2": MIST_DARK + [alpha * 0.8]},
            "update": {"drag": drag, "gravity": gravity, "wind": wind,
                       "size_curve": {"type": "bezier", "keys": [[0, 0.6], [0.2, 1.0], [1, 1.9]]},
                       "alpha_curve": {"type": "bezier", "keys": [[0, 0.3], [0.04, 1], [0.5, 0.55], [1, 0]]},
                       "frame_over_life": True, "motion_vectors": True},
            "render": {"mode": "alpha", "lighting": "sixway", "texture": "flesh_mist", "soft": 0.2, "near_fade": 0.35, "scatter": scatter}}


def drops(count, speed, angle=30, size=(0.008, 0.02), life=(0.6, 1.2), dir="incoming", splat=True, gravity=1.0):
    up = {"gravity": gravity, "drag": 0.25, "collide": "physics", "kill_on_hit": True}
    if splat:
        up["sub_emitter"] = {"on": "collision", "effect": "blood_splat"}
    return {"name": "drops", "tier": "cpu",
            "spawn": {"mode": "burst", "count": list(count), "life": list(life), "size": list(size), "speed": list(speed), "dir": dir, "angle": angle,
                      "frame_random": True, "color": BLOOD + [1.0], "color2": BLOOD_DARK + [1.0]},
            "update": up,
            "render": {"mode": "alpha", "lighting": "simple", "orient": "stretch", "texture": "blood_droplet", "stretch": 0.012, "stretch_max": 4, "near_fade": 0.45}}


def spray(count, speed, angle=35, dir="incoming", size=(0.006, 0.014), life=(0.12, 0.3)):
    return {"name": "spray", "tier": "gpu",
            "spawn": {"mode": "burst", "count": list(count), "life": list(life), "size": list(size), "speed": list(speed), "dir": dir, "angle": angle,
                      "frame_random": True, "color": BLOOD + [0.9]},
            "update": {"gravity": 1.0, "drag": 1.5, "collide": "depth", "bounce": 0.0, "friction": 1.0},
            "render": {"mode": "alpha", "lighting": "simple", "orient": "stretch", "texture": "blood_droplet", "stretch": 0.015, "stretch_max": 5, "near_fade": 0.45}}


def strands(count, speed, angle=18, dir="incoming"):
    return {"name": "strands", "tier": "cpu",
            "spawn": {"mode": "burst", "count": list(count), "life": [0.25, 0.5], "size": [0.012, 0.025], "speed": list(speed), "dir": dir, "angle": angle,
                      "frame_random": True, "color": BLOOD + [0.95]},
            "update": {"gravity": 0.8, "drag": 1.2, "alpha_curve": [[0, 1], [0.7, 0.8], [1, 0]]},
            "render": {"mode": "alpha", "lighting": "simple", "orient": "stretch", "texture": "blood_strand", "stretch": 0.03, "stretch_max": 9, "near_fade": 0.5}}


def fibers(count):
    return {"name": "fibers", "tier": "gpu",
            "spawn": {"mode": "burst", "count": [count // 2, count], "life": [0.5, 1.2], "size": [0.01, 0.022], "speed": [1.0, 2.8], "dir": "incoming", "angle": 45,
                      "rotation": [0, 360], "spin": [-360, 360], "frame_random": True, "color": [0.32, 0.3, 0.28, 1.0]},
            "update": {"gravity": 0.25, "drag": 2.5, "alpha_curve": [[0, 1], [0.8, 1], [1, 0]]},
            "render": {"mode": "alpha", "lighting": "simple", "texture": "cloth_fibers", "soft": 0.02, "near_fade": 0.1}}


def sparks(count, emissive=4.0):
    return {"name": "sparks", "tier": "gpu",
            "spawn": {"mode": "burst", "count": [count // 2, count], "life": [0.1, 0.35], "size": [0.006, 0.012], "speed": [4.0, 11.0], "dir": "reflect",
                      "dir_bias": 0.4, "angle": 40, "frame_random": True, "color": [1.0, 0.72, 0.38, 1.0], "color2": [1.0, 0.52, 0.2, 1.0]},
            "update": {"gravity": 1.0, "drag": 0.8, "collide": "depth", "bounce": 0.35, "friction": 0.3, "alpha_curve": [[0, 1], [0.6, 0.9], [1, 0]]},
            "render": {"mode": "add", "orient": "stretch", "texture": "helmet_spark", "emissive": emissive, "stretch": 0.012, "stretch_max": 10, "near_fade": 0.05}}


def flash():
    return {"name": "hitflash", "tier": "cpu",
            "spawn": {"mode": "burst", "count": 1, "life": [0.03, 0.05], "size": [0.06, 0.09], "offset": 0.01, "dir": "normal", "color": [1.0, 0.8, 0.55, 1.0]},
            "update": {"alpha_curve": [[0, 1], [0.5, 0.85], [1, 0]]},
            "render": {"mode": "add", "texture": "ember", "emissive": 3.0, "near_fade": 0.05}}


def dust(count=3, color=(0.5, 0.47, 0.42), alpha=0.45, size=(0.1, 0.2)):
    return {"name": "dust", "tier": "gpu",
            "spawn": {"mode": "burst", "count": [max(1, count - 1), count], "life": [0.6, 1.4], "size": list(size), "speed": [0.4, 1.5], "dir": "normal", "angle": 35,
                      "rotation": [0, 360], "spin": [-30, 30], "color": list(color) + [alpha]},
            "update": {"gravity": 0.03, "drag": 3.5, "wind": 0.4, "size_curve": {"type": "bezier", "keys": [[0, 0.4], [0.2, 1.3], [1, 2.6]]},
                       "alpha_curve": {"type": "bezier", "keys": [[0, 0], [0.06, 1], [0.45, 0.6], [1, 0]]}, "frame_over_life": True},
            "render": {"mode": "alpha", "lighting": "sixway", "texture": "dust_puff", "soft": 0.3, "near_fade": 0.25}}


def chunks(mesh, count, color, size=(0.008, 0.02), speed=(1.5, 4.0)):
    return {"name": "chunks", "tier": "cpu",
            "spawn": {"mode": "burst", "count": [max(1, count // 2), count], "life": [2.0, 3.5], "size": list(size), "speed": list(speed), "dir": "incoming",
                      "angle": 35, "spin": [-900, 900], "color": list(color) + [1.0]},
            "update": {"gravity": 1.0, "drag": 0.25, "collide": "physics", "bounce": 0.25, "friction": 0.6, "alpha_curve": [[0, 1], [0.85, 1], [1, 0]]},
            "render": {"mode": "mesh", "mesh": mesh}}


def decal_fx(tex, size, life, depth, rough, dry, darken, variants=4, normal=True, tint=(1, 1, 1, 1)):
    d = {"texture": tex, "size": list(size), "lifetime": life, "depth": depth, "variants": variants, "tint": list(tint),
         "roughness": rough, "dry_time": dry, "dry_darken": darken, "pool": "blood"}
    if normal:
        d["normal"] = tex + "_n"
    return {"duration": 0.05, "priority": 0, "max_instances": 64, "emitters": [], "decal": d}


def main():
    os.makedirs(OUT, exist_ok=True)
    # Entry: a puff back toward the shooter, droplets, a fine spray (mist is its own effect so
    # it scales with the character's "mist").
    write("flesh_entry", effect([drops((2, 5), (0.8, 2.2), 40), spray((4, 9), (1.0, 3.0), 45)]))
    write("flesh_entry_cloth", effect([drops((2, 4), (0.8, 2.0), 40), spray((3, 7), (1.0, 2.8), 45), fibers(8)]))
    write("flesh_entry_mist", effect([mist((2, 4), (0.08, 0.17), (0.3, 1.2), life=(0.25, 0.55), angle=35, alpha=0.6)], max_inst=64))
    # Exit: a cone of mist and droplets out of the far side, strands, a fast fine spray.
    write("flesh_exit", effect([mist((4, 7), (0.18, 0.4), (1.2, 3.5), life=(0.3, 0.7), angle=20, alpha=0.65, drag=3.5),
                                drops((8, 14), (2.5, 6.5), 16, size=(0.008, 0.022)), strands((3, 5), (4.0, 8.0)),
                                spray((10, 20), (3.0, 8.0), 15)], max_inst=40))
    write("flesh_exit_head", effect([mist((6, 10), (0.22, 0.48), (1.4, 4.0), life=(0.35, 0.85), angle=24, alpha=0.62, drag=3.2),
                                     drops((14, 22), (3.0, 7.5), 20, size=(0.008, 0.026)), strands((5, 8), (4.5, 9.0), 22),
                                     spray((18, 30), (3.5, 9.0), 18)], max_inst=24))
    # Lingering haze (1-2 s) where the blood hung in the air.
    write("flesh_mist", effect([mist((2, 3), (0.22, 0.42), (0.15, 0.5), life=(1.1, 2.0), angle=40, alpha=0.22, drag=2.2, wind=0.3, gravity=0.02,
                                     scatter=1.1, name="haze")], max_inst=32, prio=0))
    write("flesh_kill", effect([mist((5, 8), (0.2, 0.42), (0.8, 2.6), life=(0.4, 0.9), angle=28, alpha=0.7),
                                drops((10, 18), (2.0, 6.0), 26, size=(0.01, 0.026)), strands((3, 6), (3.0, 7.0), 26)], max_inst=24, prio=2))
    write("melee_flesh", effect([drops((8, 14), (2.0, 4.5), 22, size=(0.01, 0.024)), strands((2, 4), (2.5, 5.0), 20),
                                 mist((1, 2), (0.08, 0.15), (0.4, 1.2), angle=30, alpha=0.45), spray((6, 12), (2.0, 5.0), 25)]))
    write("flesh_bone", effect([chunks("shard_bone", 6, [0.78, 0.72, 0.6]),
                                {"name": "bits", "tier": "gpu",
                                 "spawn": {"mode": "burst", "count": [4, 8], "life": [0.5, 1.0], "size": [0.008, 0.016], "speed": [2.0, 5.0], "dir": "incoming", "angle": 30,
                                           "rotation": [0, 360], "spin": [-720, 720], "frame_random": True, "color": [1, 1, 1, 1]},
                                 "update": {"gravity": 1.0, "drag": 0.4, "collide": "depth", "bounce": 0.2, "friction": 0.6},
                                 "render": {"mode": "alpha", "lighting": "simple", "texture": "bone_frag", "soft": 0.02, "near_fade": 0.1}}], max_inst=8, prio=2))
    # Helmets: sparks and a puff of paint, no blood until it breaks.
    light = {"color": [1.0, 0.7, 0.4], "intensity": 3.0, "radius": 2.0, "duration": 0.05, "exposure_aware": True}
    fx = effect([flash(), sparks(12), dust(2, (0.35, 0.37, 0.32), 0.4, (0.06, 0.12))])
    fx["light"] = light
    fx["sound"] = "impact_metal"
    write("flesh_helmet", fx)
    fx = effect([flash(), sparks(24, 5.0), dust(3, (0.35, 0.37, 0.32), 0.5, (0.1, 0.18)), chunks("shard_helmet", 4, [0.25, 0.28, 0.22], size=(0.015, 0.035))])
    fx["light"] = light
    write("flesh_helmet_break", fx)
    # Gore off: a dust puff only.
    write("gore_dust", effect([dust(2, (0.45, 0.42, 0.38), 0.4, (0.06, 0.12))]))
    # Corpse wounds: one heavy drop at a time.
    write("flesh_bleed", effect([{"name": "drop", "tier": "cpu",
                                  "spawn": {"mode": "burst", "count": 1, "life": [1.5, 2.5], "size": [0.008, 0.014], "speed": [0.0, 0.25], "dir": "normal", "angle": 60,
                                            "frame_random": True, "color": BLOOD + [1.0]},
                                  "update": {"gravity": 1.0, "drag": 0.1, "collide": "physics", "kill_on_hit": True,
                                             "sub_emitter": {"on": "collision", "effect": "blood_drip_splat"}},
                                  "render": {"mode": "alpha", "lighting": "simple", "orient": "stretch", "texture": "blood_droplet", "stretch": 0.01, "stretch_max": 3}}],
                                max_inst=96, prio=0, cull=60))
    # Decals (pool "blood": separate budget, wet then drying).
    write("blood_splatter", decal_fx("blood_splatter", (0.16, 0.42), 240, 0.12, 0.15, 150, 0.55, variants=16))
    write("blood_drip_splat", decal_fx("blood_splatter", (0.025, 0.06), 180, 0.08, 0.15, 120, 0.55, variants=16))
    write("blood_drip", decal_fx("blood_drip", (0.04, 0.06), 240, 0.08, 0.12, 150, 0.55))
    write("blood_pool", decal_fx("blood_pool", (0.25, 0.25), 360, 0.3, 0.06, 300, 0.6))
    write("wound_entry", decal_fx("wound_entry", (0.035, 0.035), 600, 0.05, 0.3, 90, 0.7))
    write("wound_exit", decal_fx("wound_exit", (0.07, 0.07), 600, 0.06, 0.25, 90, 0.7))
    write("wound_slash", decal_fx("wound_slash", (0.09, 0.09), 600, 0.05, 0.25, 90, 0.7))
    write("blood_stain", decal_fx("blood_stain", (0.06, 0.06), 600, 0.12, 0.3, 120, 0.6))
    write("blood_stain_cloth", decal_fx("blood_stain_cloth", (0.06, 0.06), 600, 0.12, 0.45, 120, 0.6))
    print("flesh effects written to", OUT)


if __name__ == "__main__":
    main()
