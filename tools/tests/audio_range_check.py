#!/usr/bin/env python3
"""Checks a log of tools/tests/audio_range.cfg (docs/AUDIO_PROCEDURAL.md "Audio range"):
  * the reverb corridor probe: INDOOR, 24 x 3 x 3 m within 20 %
  * the cliff pad: OUTDOOR with an echo at 2 x 145 m / c (+-3 %)
  * the sound test bay's tiled room: INDOOR, 6 x 3 x 5 m within 20 %
  * every audio_synth_play played, voices were listed, the footstep on the strip is not occluded
  * the recorded CC0 takes loaded, and footsteps / impacts / zombies play them
Run: python3 tools/tests/audio_range_check.py audio.log"""
import re, sys

def main(path):
    log = open(path, encoding="utf-8", errors="replace").read().splitlines()
    fails = []
    probes = [l for l in log if "audio_env_probe:" in l and "box" in l and "min" not in l]
    def dims(l):
        m = re.search(r"box ([\d.]+) x ([\d.]+) x ([\d.]+) m", l)
        return [float(x) for x in m.groups()] if m else None
    def near(d, want):
        return d and all(abs(a / b - 1) < 0.2 for a, b in zip(d, want))
    if len(probes) < 4: fails.append("expected 4 room probes, got %d" % len(probes))
    else:
        if "INDOOR" not in probes[0] or not near(dims(probes[0]), (24, 3, 3)): fails.append("corridor probe: " + probes[0])
        if "OUTDOOR" not in probes[1]: fails.append("pad probe not outdoor: " + probes[1])
        if "INDOOR" not in probes[2] or not near(dims(probes[2]), (6, 3, 5)): fails.append("tiled room probe: " + probes[2])
    echoes = [float(m.group(1)) for l in log for m in [re.search(r"echo \d+ at .*: \+([\d.]+) s", l)] if m]
    want = 2 * 145 / 343.0
    if not any(abs(e - want) < 0.03 * want for e in echoes): fails.append("no cliff echo near +%.3f s (got %s)" % (want, echoes))
    if any("FAILED" in l for l in log if "audio_synth_play" in l): fails.append("an audio_synth_play failed")
    if not any("audio: " in l and " voices, mixer" in l for l in log): fails.append("no audio_voices listing")
    steps = [l for l in log if "snd: footstep starts" in l]
    if steps and not re.search(r"occl -?0/-?0/-?0 dB", steps[-1]): fails.append("the strip footstep is occluded: " + steps[-1])
    if not any(re.search(r"audio: .* (\d+) recorded takes", l) and int(re.search(r"(\d+) recorded takes", l).group(1)) > 100 for l in log):
        fails.append("the recorded takes did not load")
    for rec, take in [("footstep", "footstep_hard_"), ("impact", "impact_metal_"), ("impact", "impact_glass_"), ("zombie_voice", "zombie_scream_")]:
        if not any("snd: %s starts" % rec in l and take in l for l in log): fails.append("no %s voice played a %s take" % (rec, take))
    for f in fails: print("FAIL", f)
    print("audio_range_check: %d failure(s)" % len(fails))
    return 1 if fails else 0

if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
