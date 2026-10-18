#!/usr/bin/env python3
"""Listening pack (docs/AUDIO_PROCEDURAL.md "Listening"): renders the procedural sounds in the scenes
that change them, as MP3s with a README of what each file shows and what to listen for.

Run: python3 tools/audio_render/render_pack.py [out_dir] [--wav]   (needs build*/bin/audio_render, ffmpeg)"""
import os, subprocess, sys, shutil

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
EXE = next((os.path.join(ROOT, b, "bin", "audio_render" + (".exe" if os.name == "nt" else "")) for b in ("build-linux", "build", "build-alt")
            if os.path.exists(os.path.join(ROOT, b, "bin", "audio_render" + (".exe" if os.name == "nt" else "")))), None)

RIFLE = ["energy=2000", "caliber=7.62", "barrel=0.4"]
# name, recipe, args, what to listen for. Every file is peak-normalized (-3 dBFS) unless it says
# "level kept": then levels across that series compare (distance).
PACK = [
    ("01_rifle_shooter_field", "gunshot", RIFLE + ["local=1", "--scene", "field", "--dist", "0.8", "--dur", "2"], "The shooter's ear in an open field: blast + mechanism, the ground's bounce, almost no tail (nothing around to return it)."),
    ("02_rifle_15m_field", "gunshot", RIFLE + ["--scene", "field", "--dist", "15", "--dur", "2.5"], "15 m away, open field: dry, a slight comb from the ground reflection."),
    ("03_rifle_15m_forest", "gunshot", RIFLE + ["--scene", "forest", "--dist", "15", "--dur", "3"], "15 m in a forest: the trunks scatter a dense, dark tail (~1 s)."),
    ("04_rifle_15m_street", "gunshot", RIFLE + ["--scene", "urban", "--dist", "15", "--dur", "3"], "15 m down a street: slaps off the facades, a brighter tail."),
    ("05_rifle_60m_valley", "gunshot", RIFLE + ["--scene", "valley", "--dist", "60", "--dur", "4"], "60 m in a valley: the report, then discrete echoes off both rock walls 0.7-1.1 s later rolling into a tail."),
    ("06_rifle_corridor_8m", "gunshot", RIFLE + ["--scene", "corridor", "--dist", "8", "--dur", "3"], "8 m down a concrete corridor: louder reverb than direct sound (as Sabine says), the end wall's flutter echo ~0.1 s."),
    ("07_rifle_tiled_room_3m", "gunshot", RIFLE + ["--scene", "room", "--dist", "3", "--dur", "3"], "3 m in a small tiled room: early reflections pile up, ~1.6 s ring."),
    ("08_rifle_behind_concrete_wall", "gunshot", RIFLE + ["--scene", "wall", "--dist", "30", "--dur", "2.5"], "30 m with a 20 cm concrete wall between: transmission takes the highs (thud)."),
    ("09_rifle_heard_from_indoors", "gunshot", RIFLE + ["--scene", "indoor_out", "--dist", "40", "--dur", "3"], "Inside a plaster room, the shot 40 m outside: through the walls and the open door."),
    ("10_distance_15m", "gunshot", RIFLE + ["--scene", "field", "--dist", "15", "--dur", "2.5", "--normalize", "1"], "Distance series, level kept: 15 m."),
    ("11_distance_100m", "gunshot", RIFLE + ["--scene", "field", "--dist", "100", "--dur", "2.5", "--normalize", "1", "--gain", "6"], "Distance series, level kept (+6 dB): 100 m: later, quieter, the air has taken the top."),
    ("12_distance_400m", "gunshot", RIFLE + ["--scene", "field", "--dist", "400", "--dur", "3", "--normalize", "1", "--gain", "18"], "Distance series (+18 dB): 400 m: a dull pop arriving 1.2 s late."),
    ("13_distance_1000m_valley", "gunshot", RIFLE + ["--scene", "valley", "--dist", "1000", "--dur", "6", "--normalize", "1", "--gain", "24"], "Distance series (+24 dB): 1 km down a valley: a low boom and its echoes."),
    ("14_pistol_15m_street", "gunshot", ["energy=500", "caliber=9", "barrel=0.1", "--scene", "urban", "--dist", "15", "--dur", "2.5"], "Pistol: less energy, shorter barrel (sharper, more crack)."),
    ("15_sniper_300m_valley", "gunshot", ["energy=6500", "caliber=8.6", "barrel=0.66", "--scene", "valley", "--dist", "300", "--dur", "5"], "Magnum rifle at 300 m in the valley: a deep report and a long roll."),
    ("16_shotgun_20m_field", "gunshot", ["energy=3200", "caliber=18.5", "barrel=0.5", "pellets=9", "--scene", "field", "--dist", "20", "--dur", "2.5"], "Shotgun: a big, low blast."),
    ("17_suppressed_15m_forest", "gunshot", RIFLE + ["device=3", "--scene", "forest", "--dist", "15", "--dur", "2.5"], "Suppressed (normalized: really ~28 dB quieter): a dark thump and gas hiss instead of a crack."),
    ("18_brake_15m_field", "gunshot", RIFLE + ["device=2", "--scene", "field", "--dist", "15", "--dur", "2.5"], "Muzzle brake: harsher, brighter blast."),
    ("19_auto_burst_forest", "gunshot", RIFLE + ["--scene", "forest", "--dist", "20", "--dur", "3", "--repeat", "6", "--every", "0.09"], "A 6-round burst at ~660 rpm: every shot differs (seeded), tails overlap."),
    ("20_AB_chain_off", "gunshot", RIFLE + ["--scene", "valley", "--dist", "60", "--dur", "4", "--chain", "off"], "A/B with 05: the same shot without the environment chain (distance law and pan only)."),
    ("21_AB_with_placeholder_samples", "gunshot", RIFLE + ["--scene", "valley", "--dist", "60", "--dur", "4", "--sample", "shot_rifle", "+samples"], "A/B with 05: plus the current sample layer (tools/gen_assets.py placeholders, not recordings)."),
    ("22_crack_near_miss_2m", "bullet_crack", ["miss=2", "caliber=7.62", "speed=800", "--scene", "field", "--dist", "2", "--dur", "1.5"], "A supersonic bullet passing 2 m away: the N-wave crack."),
    ("23_crack_far_miss_20m", "bullet_crack", ["miss=20", "caliber=7.62", "speed=800", "--scene", "forest", "--dist", "20", "--dur", "1.5"], "Passing 20 m away in a forest: longer, softer crack and its scatter."),
    ("24_whizz_subsonic", "bullet_whizz", ["speed=300", "miss=1.5", "--scene", "field", "--dist", "1", "--dur", "1"], "A subsonic round passing close."),
]
for m, name, e, smp in [(1, "concrete", 1500, "impact_hard"), (4, "metal_thin", 1500, "impact_metal"), (5, "metal_thick", 1500, "impact_metal"), (6, "wood", 1500, "impact_wood"),
                        (11, "glass", 1500, "impact_glass"), (7, "dirt", 1500, None), (12, "water", 1500, "impact_water"), (13, "flesh", 1200, "impact_flesh")]:
    # The VFX effect's sample name, as vfx_game_impact passes it (dirt has no recording: synthesized).
    sa = ["--sample", smp] if smp else []
    PACK.append(("3%d_impact_%s" % (len([p for p in PACK if "impact" in p[0]]), name), "impact", ["material=%d" % m, "energy=%d" % e] + sa + ["--scene", "field", "--dist", "5", "--dur", "1.5", "--repeat", "3", "--every", "0.45"],
                 "Bullet impacts on %s at 5 m: the material's ring / thud, debris falling back; three hits, each different." % name))
PACK.append(("38_impact_metal_grazing", "impact", ["material=4", "energy=1500", "cosi=0.15", "--scene", "field", "--dist", "5", "--dur", "1.5", "--repeat", "3", "--every", "0.45"], "Grazing hits on thin metal: scrape more than ring."))
for i, (name, args, what) in enumerate([
        ("gravel_walk", ["material=9", "speed=1.8"], "Walking on gravel: crunch grains, a little thud."),
        ("grass_walk", ["material=10", "speed=1.8"], "Walking on grass: soft swish."),
        ("wood_walk", ["material=6", "speed=1.8"], "Walking on a wooden floor: heel-toe knock."),
        ("concrete_run", ["material=1", "speed=6", "--every", "0.33"], "Running on concrete."),
        ("metal_walk", ["material=4", "speed=1.8"], "Walking on a metal floor: ringing."),
        ("snow_walk", ["material=17", "speed=1.6"], "Walking on snow: squeaky crunch."),
        ("water_wade", ["material=12", "water=1", "speed=1.5"], "Wading through water."),
        ("wood_crouch", ["material=6", "speed=1.0", "crouch=1", "--every", "0.7"], "Crouched on wood: soft, slow."),
        ("zombie_drag_concrete", ["material=1", "speed=1.0", "zombie=1", "weight=70", "--every", "0.75"], "A zombie dragging its feet on concrete."),
]):
    args = args + (["--every", "0.5"] if "--every" not in args else [])
    PACK.append(("4%d_footsteps_%s" % (i, name), "footstep", args + ["--scene", "field" if "zombie" not in name else "corridor", "--dist", "3", "--hs", "0.05", "--dur", "4", "--repeat", "8"], what))
PACK += [
    ("50_engine_rev_10m", "vehicle_engine", ["throttle=0.8", "--ramp", "rpm=800:6000", "--scene", "field", "--dist", "10", "--az", "40", "--dur", "6"], "A car engine revving 800 -> 6000 rpm, 10 m away: firing pulses through the exhaust, gearbox whine, valve tick."),
    ("51_engine_idle_corridor", "vehicle_engine", ["rpm=850", "throttle=0", "--scene", "corridor", "--dist", "6", "--dur", "4"], "Idling in a concrete tunnel."),
    ("52_motorbike_rev", "vehicle_engine", ["moto=1", "cyl=2", "throttle=0.9", "--ramp", "rpm=1200:9000", "--scene", "field", "--dist", "8", "--dur", "5"], "A motorbike twin revving."),
    ("53_tires_gravel", "vehicle_tires", ["material=9", "--ramp", "speed=2:25", "--scene", "field", "--dist", "5", "--dur", "5"], "Tires on gravel, speeding up."),
    ("54_tires_asphalt_skid", "vehicle_tires", ["material=16", "speed=20", "skid=0.8", "--scene", "field", "--dist", "6", "--dur", "3"], "Skidding on asphalt."),
    ("55_wind_rising", "wind", ["gust=0.6", "exposure=1", "--ramp", "wind=2:22", "--dur", "8"], "Wind rising from a breeze to a gale: random gusts, the high whistle comes in."),
    ("56_rain_open", "rain", ["rain=0.8", "exposure=1", "--dur", "6"], "Heavy rain in the open."),
    ("57_rain_on_metal_roof", "rain", ["rain=0.8", "exposure=0.1", "roof=1", "roof_metal=1", "--dur", "6"], "The same rain under a thin metal roof: drumming."),
    ("58_thunder_near", "thunder", ["strike=500", "--scene", "valley", "--dist", "500", "--hs", "300", "--dur", "8", "--normalize", "-3"], "A strike 500 m away: crackle, then the roll (arrives 1.5 s late)."),
    ("59_thunder_far", "thunder", ["strike=4000", "--scene", "field", "--dist", "4000", "--hs", "1200", "--dur", "16", "--normalize", "-3"], "4 km away: a low rumble 12 s after the flash."),
    ("60_leaves_wind", "leaves", ["density=0.9", "wind=9", "--dur", "6"], "Leaves rustling in a breeze."),
    ("61_water_lapping", "water_lap", ["waves=0.5", "--scene", "field", "--dist", "2", "--hs", "0.1", "--dur", "6"], "Water lapping at an edge."),
    ("62_fire", "fire", ["size=1.2", "--scene", "field", "--dist", "3", "--hs", "0.3", "--dur", "6"], "A fire: crackles, hiss, roar."),
    ("70_zombie_groan", "zombie_voice", ["call=0", "pitch=0.95", "tract=1.0", "rough=0.5", "--scene", "forest", "--dist", "6", "--dur", "3"], "A zombie groan (source-filter synthesis)."),
    ("71_zombie_growl", "zombie_voice", ["call=1", "pitch=1.05", "tract=0.92", "rough=0.8", "--scene", "forest", "--dist", "6", "--dur", "2.5"], "A growl."),
    ("72_zombie_scream", "zombie_voice", ["call=2", "pitch=1.1", "tract=1.1", "rough=0.6", "--scene", "urban", "--dist", "12", "--dur", "3"], "A scream down a street."),
    ("73_zombie_attack", "zombie_voice", ["call=3", "pitch=0.9", "tract=0.9", "rough=0.9", "--scene", "field", "--dist", "2", "--dur", "1.5"], "An attack bark, close."),
    ("74_zombie_crowd_80m", "zombie_crowd", ["count=40", "--scene", "field", "--dist", "80", "--dur", "6"], "A horde of ~40, 80 m away."),
    ("80_ui_click", "ui_click", ["--dur", "0.5"], "UI click."),
    ("81_ui_confirm", "ui_confirm", ["--dur", "0.6"], "UI confirm."),
    ("75_AB_impact_metal_synth_only", "impact", ["material=4", "energy=1500", "--sample", "impact_metal", "--scene", "field", "--dist", "5", "--dur", "1.5", "--repeat", "3", "--every", "0.45", "-samples"], "A/B with 31: the same metal hits, synthesized layers only (no recorded take)."),
    ("76_AB_footsteps_concrete_synth_only", "footstep", ["material=1", "speed=6", "--every", "0.33", "--scene", "field", "--dist", "3", "--hs", "0.05", "--dur", "4", "--repeat", "8", "-samples"], "A/B with 43: running on concrete, synthesized only."),
    ("77_AB_zombie_scream_synth_only", "zombie_voice", ["call=2", "pitch=1.1", "tract=1.1", "rough=0.6", "--scene", "urban", "--dist", "12", "--dur", "3", "-samples"], "A/B with 72: the scream, synthesized only."),
]


def main(args):
    out = next((a for a in args if not a.startswith("--")), os.path.join(ROOT, "build-linux", "listening_pack"))
    keep_wav = "--wav" in args
    if not EXE:
        sys.exit("render_pack: build audio_render first")
    os.makedirs(out, exist_ok=True)
    lines = ["# Procedural audio listening pack", "", "Rendered offline by `tools/audio_render` through the game's mixer, environment chain and environment logic "
             "(box-built scenes; listener 1.7 m up facing the source). Each file is peak-normalized to -3 dBFS unless its note says the level is kept. "
             "Impacts, footsteps, zombies and rain mix the recorded CC0 takes (assets/sounds/CREDITS.md) under the synthesized layers; 75-77 are the same sounds synthesized only. "
             "Gunshots are synthesized only, except 21 (A/B with the placeholder samples). Headphones show the spatial cues (head shadow, ITD, reflections) best.", ""]
    for name, recipe, a, what in PACK:
        cmd = [EXE, recipe] + [x for x in a if x not in ("+samples", "-samples")]
        # The recorded CC0 takes play by default; gunshots stay synthesized (their samples are
        # tools/gen_assets.py placeholders) unless "+samples"; "-samples" makes a synth-only A/B.
        if "-samples" in a or (recipe == "gunshot" and "+samples" not in a):
            cmd.append("--no-samples")
        if "--normalize" not in a:
            cmd += ["--normalize", "-3"]
        wav = os.path.join(out, name + ".wav")
        r = subprocess.run(cmd + ["-o", wav], capture_output=True, text=True)
        if r.returncode:
            print("FAIL", name, r.stderr.strip())
            continue
        mp3 = os.path.join(out, name + ".mp3")
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", wav, "-codec:a", "libmp3lame", "-q:a", "0", mp3], check=True)
        if not keep_wav:
            os.remove(wav)
        lines.append("- **%s** - %s" % (name, what))
        print(name)
    with open(os.path.join(out, "README.md"), "w") as f:
        f.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main(sys.argv[1:])
