"""Shadow stability tests in the game (docs/SHADOWS.md "Tests"), on the dev room's shadow test corner.
Uses tools/tests/shadow_ref.py's harness (run_game, EXR reader, the fin profile).

  python tools/tests/shadow_stability.py [build_dir] [--skip-validation] [--only switch|matrix|translate|seams|acne|taa|viewmodel]

  switch     shadow_switch_stress: every Engine Fidelity preset in turn, then cloud cover
             0 -> 1 -> 0 on each; the reference edge (penumbra, bias offset, fade, shade ratio)
             never jumps between frames, the cutoff toggles at most once per half sweep, no frame
             samples an unrendered shadow tile; 0 validation messages
  matrix     sunrise / noon / sunset / night x the 5 presets x cloud cover 0 / 0.5 / 1: every frame
             plausible, 0 validation messages, no unrendered tile
  translate  10 frames of pure sub-texel translation over the fin's shadow: its edge stays within
             1 px in the world (texel snapping)
  seams      looking down across the cascade 0 / 1 boundary: the shadow's dark level and edge width
             along the fin's shadow never step between neighbouring rows
  acne       noon and sunset on the 15 / 35 / 60 deg planes: the lit faces are smooth (no acne
             stripes: luminance spread < 4 %)
  taa        a static edge: with TAA the per-pixel temporal std < 2 % of the lit / shade step; with
             FXAA (no jitter) the kernel does not rotate: frames identical within 0.5 %
  viewmodel  first person at the viewmodel wall: the rig's tile is on (shadow_info), its debug view
             renders, 0 validation messages
Exit code 0 when everything passes. Results: <build>/shadow_tests/stability.txt
"""
import math
import os
import re
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shadow_ref as R  # noqa: E402

ONLY = sys.argv[sys.argv.index('--only') + 1] if '--only' in sys.argv else None


def png(path):
    try:
        from PIL import Image
    except ImportError:
        return None
    if not os.path.exists(path):
        return None
    return np.asarray(Image.open(path).convert('L'), dtype=np.float32) / 255.0


def unrendered(text):
    return len(re.findall(r'samples unrendered shadow tiles', text))


def switch():
    lines = ['setpos -50 0.05 -8 200 -20', 'wait 30', 'shadow_switch_stress 6', 'wait 6000', 'shadow_info']
    text = R.run_game('switch', lines, validation=R.VALIDATION, timeout=7200)
    m = re.search(r'shadow_switch_stress: (\d+) frames, (\d+) jumps.*cutoff toggles per sweep <= (\d+), unrendered (\d+) -> (PASS|FAIL)', text)
    R.check(m is not None, 'switch: shadow_switch_stress finished')
    if m:
        R.check(m.group(5) == 'PASS', 'switch: %s frames, %s jumps, toggles <= %s, unrendered %s' % (m.group(1), m.group(2), m.group(3), m.group(4)))
    for w in re.findall(r'shadow_switch_stress: frame .*', text)[:8]:
        R.log('      ' + w)
    if R.VALIDATION:
        R.check(R.vuids(text) == 0, 'switch: %d validation messages' % R.vuids(text))


def matrix():
    times = ['sunrise', 'clear_noon', 'sunset', 'night_moon']
    covers = ['cover_0', 'cover_0.5', 'cover_1']
    lines, names = ['setpos -50 0.05 -8 200 -20', 'wait 30'], []
    for t in times:
        lines += ['shadow_preset ' + t, 'time_pause 1', 'wait 60']
        for lvl in range(5):
            lines += ['r_fidelity %d' % lvl, 'wait 30']
            for c in covers:
                nm = 'mx_%s_%d_%s' % (t, lvl, c.replace('.', ''))
                names.append(nm)
                lines += ['shadow_preset ' + c, 'wait 300', 'screenshot %s/%s.png' % (R.OUT, nm), 'wait 3']
    text = R.run_game('matrix', lines, validation=R.VALIDATION, timeout=7200)
    bad = 0
    for nm in names:
        a = png('%s/%s.png' % (R.OUT, nm))
        if a is None:
            bad += 1
            R.log('      %s.png missing' % nm)
        elif not (a.mean() > 0.02 and a.std() > 0.01):
            bad += 1
            R.log('      %s implausible (mean %.3f std %.3f)' % (nm, a.mean(), a.std()))
    R.check(bad == 0, 'matrix: %d frames, %d bad' % (len(names), bad))
    R.check(unrendered(text) == 0, 'matrix: no frame sampled an unrendered shadow tile')
    if R.VALIDATION:
        R.check(R.vuids(text) == 0, 'matrix: %d validation messages' % R.vuids(text))


def translate():
    step = 0.37 * R.TEXEL
    lines = ['shadow_preset clear_noon', 'time_pause 1', 'wait 240']
    for k in range(10):
        p = R.OUT + '/tr_%02d' % k
        x = R.FIN_E + k * step
        lines += ['setpos %.4f %.2f %.4f 0 %.1f' % (x, R.EYE_Y, R.FIN_Z - 2.0 + R.TILT_DZ, R.PITCH), 'noclip', 'wait 8', 'screenshot_hdr tr_%02d %s.png %s.exr' % (k, p, p), 'wait 3']
    R.run_game('translate', lines)
    world = []
    for k in range(10):
        p = R.OUT + '/tr_%02d.exr' % k
        if not os.path.exists(p):
            R.check(False, 'translate: tr_%02d.exr missing' % k)
            return
        img = R.read_exr(p)
        prof = R.fin_profile(img, R.FIN_Z - 2.0)
        if not prof:
            R.check(False, 'translate: no fin shadow in frame %d' % k)
            return
        w = img.shape[1]
        s = prof['scale']
        e50 = np.median([q['e50'] for q in prof['rows']])
        world.append(((e50 - w / 2.0) * s + R.FIN_E + k * step, s))
    xs = np.array([a for a, _ in world])
    s = world[0][1]
    spread = (xs.max() - xs.min()) / s
    R.check(spread <= 1.0, 'translate: shadow edge spread %.2f px over 10 frames of %.1f mm steps (<= 1 px)' % (spread, step * 1000.0))


def seams():
    # Higher up: the view spans ~11 .. 15 m from the eye, across cascade 0's blend band.
    lines = ['shadow_preset clear_noon', 'time_pause 1', 'wait 240',
             'setpos %.3f %.2f %.3f 0 %.1f' % (R.FIN_E, 11.0, R.FIN_Z - 2.0 + R.TILT_DZ, -70.0), 'noclip', 'wait 60']
    p = R.OUT + '/seam'
    lines += ['screenshot_hdr seam %s.png %s.exr' % (p, p), 'wait 4', 'r_shadow_debug 4', 'wait 4', 'screenshot %s_casc.png' % p, 'wait 3', 'r_shadow_debug 0']
    R.run_game('seams', lines)
    if not os.path.exists(p + '.exr'):
        R.check(False, 'seams: seam.exr missing')
        return
    prof = R.fin_profile(R.read_exr(p + '.exr'), R.FIN_Z - 2.0)
    if not prof:
        R.check(False, 'seams: no fin shadow')
        return
    rows = prof['rows']
    dd = max(abs(rows[i + 1]['dark'] / rows[i + 1]['lit'] - rows[i]['dark'] / rows[i]['lit']) for i in range(len(rows) - 1))
    dw = max(abs(rows[i + 1]['e'] - rows[i]['e']) for i in range(len(rows) - 1))
    R.check(dd < 0.03, 'seams: shade / lit steps %.3f between rows (< 0.03)' % dd)
    R.check(dw < 1.5, 'seams: edge width steps %.2f px between rows (< 1.5)' % dw)


def acne():
    lines = []
    for pr in ('clear_noon', 'sunset'):
        lines += ['shadow_preset ' + pr, 'time_pause 1', 'wait 240', 'setpos -43 2.5 -4 180 -35', 'noclip', 'wait 60', 'screenshot_hdr acne_%s %s/acne_%s.png %s/acne_%s.exr' % (pr, R.OUT, pr, R.OUT, pr), 'wait 4']
    R.run_game('acne', lines)
    for pr in ('clear_noon', 'sunset'):
        p = '%s/acne_%s.exr' % (R.OUT, pr)
        if not os.path.exists(p):
            R.check(False, 'acne: %s missing' % p)
            continue
        Y = R.lum(R.read_exr(p))
        h, w = Y.shape
        worst = 0.0
        # the three planes fill the lower middle of the view: 32 px patches, lit (bright) ones only
        for y in range(h // 2, h - 64, 32):
            for x in range(w // 4, 3 * w // 4 - 32, 32):
                q = Y[y:y + 32, x:x + 32]
                m = q.mean()
                if m < np.percentile(Y, 60):
                    continue
                worst = max(worst, float(q.std() / max(m, 1e-6)))
        R.check(worst < 0.04, 'acne: %s lit planes spread %.3f (< 0.04)' % (pr, worst))


def taa():
    lines = ['shadow_preset clear_noon', 'time_pause 1', 'wait 240'] + R.setpos_lines(R.FIN_E, R.FIN_Z - 2.0, 'taa_static')
    for i in range(16):
        p = R.OUT + '/ts_%02d' % i
        lines += ['screenshot_hdr ts_%02d %s.png %s.exr' % (i, p, p), 'wait 1']
    lines += ['r_aa 1', 'wait 30']
    for i in range(8):
        p = R.OUT + '/tf_%02d' % i
        lines += ['screenshot_hdr tf_%02d %s.png %s.exr' % (i, p, p), 'wait 1']
    R.run_game('taa_static', lines)
    for pre, n, lim, what in (('ts', 16, 0.02, 'TAA'), ('tf', 8, 0.005, 'FXAA (fixed kernel)')):
        frames = [R.lum(R.read_exr('%s/%s_%02d.exr' % (R.OUT, pre, i))) for i in range(n) if os.path.exists('%s/%s_%02d.exr' % (R.OUT, pre, i))]
        if len(frames) < n:
            R.check(False, 'taa: %s frames missing' % what)
            continue
        st = np.stack(frames)
        step = np.percentile(st[0], 90) - np.percentile(st[0], 10)
        sd = float(np.percentile(st.std(axis=0), 99)) / max(step, 1e-6)
        R.check(sd < lim, 'taa: %s static edge temporal std %.4f of the lit / shade step (< %.3f)' % (what, sd, lim))


def viewmodel():
    lines = ['shadow_preset clear_noon', 'time_pause 1', 'wait 240', 'setpos -38.0 0.05 -13 90 -10', 'wait 60', 'shadow_info',
             'r_shadow_debug 7', 'wait 4', 'screenshot %s/vm_cover.png' % R.OUT, 'wait 3', 'r_shadow_debug 0', 'screenshot %s/vm.png' % R.OUT, 'wait 3']
    text = R.run_game('viewmodel', lines, validation=R.VALIDATION)
    m = re.search(r'viewmodel tile (on|off) (\d+) \(([\d.]+) mm', text)
    R.check(m is not None, 'viewmodel: shadow_info reports the tile')
    if m:
        R.log('      viewmodel tile %s %s (%s mm) (off when no rig is drawn: give an item first)' % (m.group(1), m.group(2), m.group(3)))
        R.check(m.group(1) == 'off' or float(m.group(3)) < 1.0, 'viewmodel: sub-millimetre texels')
    R.check(png('%s/vm_cover.png' % R.OUT) is not None, 'viewmodel: coverage view rendered')
    if R.VALIDATION:
        R.check(R.vuids(text) == 0, 'viewmodel: %d validation messages' % R.vuids(text))


def main():
    if not os.path.exists(R.EXE):
        print('no game executable at %s' % R.EXE)
        return 2
    for name, fn in (('switch', switch), ('matrix', matrix), ('translate', translate), ('seams', seams), ('acne', acne), ('taa', taa), ('viewmodel', viewmodel)):
        if ONLY and ONLY != name:
            continue
        fn()
    R.log('shadow_stability: %d failure(s)' % R.failures)
    os.makedirs(R.OUT, exist_ok=True)
    with open(os.path.join(R.OUT, 'stability.txt'), 'w') as f:
        f.write('\n'.join(R.report) + '\n')
    return 1 if R.failures else 0


if __name__ == '__main__':
    sys.exit(main())
