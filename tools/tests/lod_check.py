"""lod_check.py <dir>: checks the screenshots of tools/tests/lod_fade_shots.cfg (docs/LOD.md "Tests").

  fade_*.png / cut_*.png  a forced character LOD switch (0 -> 3) with the 0.35 s cross-fade / with
                          r_lod_fade_time 0, shots 2 frames apart. Progress of the switch per shot: over the
                          pixels the switch changes (first vs last shot), the mean distance from the first shot
                          relative to the last's (0 = the old LOD, 1 = the new). A cut jumps; a fade ramps.
                          Pass: the fade's largest step below 35 % (0.35 s = ~10 steps; TAA smooths both).
  taa_band_*.png / taa_cut_*.png  a fixed camera over the foliage yard (wind off) with the distance-band
                          cross-fades / without (r_lod_fade_band 0): mean temporal standard deviation (TAA
                          stability of the dithered fades). Pass: within +2.0 / 255 of the cut's.
  <log> (optional)        the run's log: its "classic_fade_probe" lines (foliage yard + vegetation forest,
                          40 m). Pass: no tree / bush / foliage part within the radius drawn with alpha < 1
                          outside a LOD cross-fade (they were: a quarter of all instances, docs/LOD.md).
Needs numpy + Pillow; exits 1 on a failed check, 2 when the shots are missing.
"""
import glob, os, re, sys
import numpy as np
from PIL import Image


def load(pattern):
    return [np.asarray(Image.open(f).convert('RGB'), dtype=np.float32) for f in sorted(glob.glob(pattern))]


def progress(frames):
    first, last = frames[0], frames[-1]
    region = np.abs(last - first).max(axis=2) > 24
    if region.sum() < 50:
        return None, int(region.sum())
    full = np.abs(last - first)[region].mean()
    return [float(np.abs(f - first)[region].mean() / full) for f in frames], int(region.sum())


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else '.'
    fade, cut = load(os.path.join(d, 'fade_*.png')), load(os.path.join(d, 'cut_*.png'))
    tb, tc = load(os.path.join(d, 'taa_band_*.png')), load(os.path.join(d, 'taa_cut_*.png'))
    if len(fade) < 3 or len(cut) < 3 or len(tb) < 3 or len(tc) < 3:
        print('lod_check: screenshots missing in', d)
        return 2
    ok = True
    worst = {}
    for name, fr in (('fade', fade), ('cut', cut)):
        p, n = progress(fr)
        if p is None:
            print('%s: the switch changed only %d px' % (name, n))
            ok = False
            continue
        st = [b - a for a, b in zip(p, p[1:])]
        worst[name] = max(st)
        print('%-4s progress (%d px): %s' % (name, n, ' '.join('%.2f' % v for v in p)))
        print('     largest step %.0f %%' % (100 * max(st)))
    if worst.get('fade', 1) > 0.35:
        ok = False
        print('FAIL: the fade moves %.0f %% in one step' % (100 * worst['fade']))
    std_b = float(np.std(np.stack(tb), axis=0).mean())
    std_c = float(np.std(np.stack(tc), axis=0).mean())
    print('TAA temporal std (mean, /255): band fades %.2f, cuts %.2f' % (std_b, std_c))
    if std_b > std_c + 2.0:
        ok = False
        print('FAIL: the dithered fades flicker under TAA')
    if len(sys.argv) > 2:
        probes = 0
        pat = re.compile(r'classic_fade_probe: radius (\d+) m: (\d+) instances.*trees / bushes (\d+), other (\d+) \(min alpha ([\d.]+)')
        with open(sys.argv[2], errors='replace') as f:
            for line in f:
                m = pat.search(line)
                if not m:
                    continue
                probes += 1
                r, n, tr, ot, mn = int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4)), float(m.group(5))
                print('fade probe %d m: %d instances, dithered outside a cross-fade: trees / bushes %d, other %d (min alpha %.3f)' % (r, n, tr, ot, mn))
                if r <= 40 and (tr or ot):
                    ok = False
                    print('FAIL: instances within %d m of the camera drawn see-through (alpha < 1)' % r)
                if n == 0:
                    ok = False
                    print('FAIL: the probe found no instances')
        if probes < 4:
            ok = False
            print('FAIL: %d classic_fade_probe lines in %s (want 4)' % (probes, sys.argv[2]))
    print('lod_check:', 'ok' if ok else 'FAILED')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
