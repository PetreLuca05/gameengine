"""Sky-driven shadow tests in the game (docs/SHADOWS.md), on the dev room's shadow reference corner:

  python tools/tests/shadow_ref.py [build_dir] [--skip-validation] [--only penumbra|contrast|cutoff|moon|tiers|taa]

  penumbra  linear EXR captures (screenshot_hdr) looking down on the 20 m fin's east / west shadow
            edges: the 10-90 % edge width at 1 / 5 / 20 m from the caster vs the uniform-disc
            source of half width d x tan(r) (r from shadow_info), clear and hazy; within 15 % where
            the half width is >= 3 shadow texels (Modern, 4096 tiles: 6 mm), under 2 texels +
            the analytic width below that. The image scale comes from the fin's 2 m shadow width.
  contrast  lit / shaded ground luminance next to the fin = shadow_info's prediction (1 + C sin e,
            fade) within 20 %; clear >> hazy > light cloud > overcast ~ 1 (< 1.05); the shade is
            bluer under a clear sky than under overcast (b / r chromaticity)
  cutoff    2 min clear -> overcast (weather_set overcast 120), an EXR every 2 s: the lit / shade
            ratio falls smoothly (no step: second difference < 0.08), ends < 1.05; shadow_info shows
            the passes stopping only under the cutoff contrast
  moon      night_moon: the moon owns the cascades, fade ~ 1, radius ~ its disc, lit / shade > 1.5;
            at 04:00 (sun ~ -3 deg, bright dawn sky) its cast shadow has faded out
  tiers     every Engine Fidelity level and era renders (plausible frame), shadow_info's filter
            matches the tier, 0 validation messages
  taa       hazy, the fog thickening (a slowly softening edge): 24 consecutive frames, the edge
            pixels' temporal std < 3 % of the lit / shade step
Exit code 0 when everything passes. Results: <build>/shadow_tests/report.txt
"""
import math
import os
import re
import shutil
import struct
import subprocess
import sys

import numpy as np

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
args = [a for i, a in enumerate(sys.argv[1:], 1) if not a.startswith('--') and sys.argv[i - 1] != '--only']
BUILD = os.path.abspath(args[0] if args else os.path.join(ROOT, 'build'))
BIN = os.path.join(BUILD, 'bin')
EXE = os.path.join(BIN, 'deadground.exe' if os.name == 'nt' else 'deadground')
OUT = os.path.join(BUILD, 'shadow_tests').replace('\\', '/')
ONLY = sys.argv[sys.argv.index('--only') + 1] if '--only' in sys.argv else None
VALIDATION = '--skip-validation' not in sys.argv
VVL = os.environ.get('VK_ADD_LAYER_PATH', os.path.join(ROOT, '.toolchain', 'vvl', 'install', 'bin'))
RES = (1600, 900)

# The corner (data/levels/devroom.json): the fin's thin face at z = -2.5, its edges at x = -59 / -57.
FIN_Z, FIN_W, FIN_E = -2.5, -59.0, -57.0
EYE_Y = 7.0            # setpos feet height (noclip): the eye ~ 8.6 m over the ground, cascade 0
EYE_H = EYE_Y + 1.6    # eye height over the ground (approx.: only the 2 deg tilt offset uses it)
PITCH = -88.0          # the view pitch limit: 2 deg off straight down
TILT_DZ = EYE_H * math.tan(math.radians(90.0 + PITCH))   # the view centre lands this far north (-z) of the eye
TEXEL = 1.0 / 113.0    # cascade 0 texel at the Modern tier (Very High = 4096, four cascades to 300 m; tests/test_shadow_csm.c prints the layout)

report = []
failures = 0


def log(msg):
    print(msg)
    report.append(msg)


def check(ok, msg):
    global failures
    log(('ok    ' if ok else 'FAIL  ') + msg)
    if not ok:
        failures += 1


def run_game(name, lines, validation=False, timeout=3600):
    os.makedirs(OUT, exist_ok=True)
    script = os.path.join(OUT, name + '.cfg')
    with open(script, 'w') as f:
        f.write('r_maxfps 60\nr_width %d\nr_height %d\nvid_restart\nwait 20\nmap devroom\nwait 120\n' % RES)
        f.write('r_fidelity 4\nr_era 0\nr_shadow_resolution 3\nwait 30\n')
        f.write('\n'.join(lines) + '\nwait 5\nquit\n')
    log_path = os.path.join(OUT, name + '.log')
    cmd = [EXE, '--test', '--nosound', '--log', log_path, '--script', script]
    if validation:
        cmd.insert(1, '--validation')
    if os.name != 'nt' and not os.environ.get('DISPLAY') and shutil.which('xvfb-run'):
        cmd = ['xvfb-run', '-a', '-s', '-screen 0 %dx%dx24' % RES] + cmd
    env = dict(os.environ)
    if os.path.isdir(VVL):
        env['VK_ADD_LAYER_PATH'] = VVL
    subprocess.run(cmd, cwd=BIN, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=timeout)
    try:
        with open(log_path, encoding='utf-8', errors='replace') as f:
            return f.read()
    except OSError:
        return ''


def vuids(text):
    return len(re.findall(r'VUID-', text))


# ------------------------------------------------------------------ EXR (post_tonemap.c write_exr)

def read_exr(path):
    """Uncompressed HALF scanline EXR, channels A B G R -> float32 (h, w, 3) RGB."""
    with open(path, 'rb') as f:
        data = f.read()
    assert struct.unpack_from('<I', data, 0)[0] == 20000630, 'not an EXR'
    p = 8
    w = h = None
    while data[p] != 0:
        e = data.index(b'\0', p)
        name = data[p:e].decode()
        e2 = data.index(b'\0', e + 1)
        size = struct.unpack_from('<I', data, e2 + 1)[0]
        val = e2 + 5
        if name == 'dataWindow':
            x0, y0, x1, y1 = struct.unpack_from('<4i', data, val)
            w, h = x1 - x0 + 1, y1 - y0 + 1
        p = val + size
    p += 1 + h * 8
    img = np.zeros((h, w, 3), np.float32)
    for _ in range(h):
        y, n = struct.unpack_from('<2i', data, p)
        p += 8
        line = np.frombuffer(data, dtype='<f2', count=w * 4, offset=p).reshape(4, w).astype(np.float32)
        img[y] = np.stack([line[3], line[2], line[1]], axis=1)   # R G B
        p += n
    return img


def lum(img):
    return img[..., 0] * 0.2126 + img[..., 1] * 0.7152 + img[..., 2] * 0.0722


# ------------------------------------------------------------------ shadow_info

INFO = re.compile(r'shadows: key (sun|moon) ([-\d.]+) deg \| direct ([\d.]+)\s+sky ([\d.]+)\s+C ([\d.]+) \| lit/shade \(ground\) ([\d.]+)')
INFO2 = re.compile(r'source ([\d.]+) deg \| ([^|]+?)\s+fade ([\d.]+)\s+cascade0 ([\d.]+)\s+pcss ([\d.]+)')


def infos(text):
    a = INFO.findall(text)
    b = INFO2.findall(text)
    out = []
    for i in range(min(len(a), len(b))):
        out.append({'key': a[i][0], 'elev': float(a[i][1]), 'direct': float(a[i][2]), 'sky': float(a[i][3]), 'C': float(a[i][4]),
                    'ratio': float(a[i][5]), 'radius': float(b[i][0]), 'mode': b[i][1].strip(), 'fade': float(b[i][2]),
                    'near': float(b[i][3]), 'pcss': float(b[i][4])})
    return out


# ------------------------------------------------------------------ the fin's shadow in an EXR

def disc_10_90(R):
    u = np.linspace(-R, R, 20001)
    t = np.clip(u / R, -1, 1)
    prof = 1 - (np.arccos(t) - t * np.sqrt(1 - t * t)) / math.pi
    return u[np.argmax(prof >= 0.9)] - u[np.argmax(prof >= 0.1)]


def edge_cross(v, lo, hi, rising):
    """Sub-pixel position where the normalised profile v crosses lo / 0.5 / hi."""
    res = []
    for thr in (lo, 0.5, hi):
        idx = None
        rng = range(len(v) - 1)
        for i in rng:
            a, b = v[i], v[i + 1]
            if (rising and a < thr <= b) or (not rising and a > thr >= b):
                idx = i + (thr - a) / (b - a) if b != a else i
                if rising:
                    break
        res.append(idx)
    return res


def fin_profile(img, cam_z):
    """Rows near the image centre: both edges of the fin's shadow -> (scale m/px, tilt, rows with
    their ground z). The camera is over the east edge: the shadow band is left of the centre."""
    Y = lum(img)
    h, w = Y.shape
    c0, c1 = max(w // 2 - 520, 0), min(w // 2 + 320, w)   # ~ x -62 .. -54: the grey slab only
    rows = []
    for r in range(h // 2 - 120, h // 2 + 121, 4):
        line = np.convolve(Y[r, c0:c1], np.ones(3) / 3, mode='same')
        # shadow: the darkest band; lit: the median of the window (mostly lit slab)
        lit = np.median(line)
        dark = np.percentile(line, 3)
        if lit <= dark * 1.02:
            continue
        v = (line - dark) / (lit - dark)
        c = int(np.argmin(np.convolve(v, np.ones(41), mode='same')))
        west = edge_cross(v[c::-1], 0.1, 0.9, True)    # from the shadow centre outwards
        east = edge_cross(v[c:], 0.1, 0.9, True)
        if None in west or None in east:
            continue
        rows.append({'row': r, 'w50': c0 + c - west[1], 'e50': c0 + c + east[1], 'w': (west[2] - west[0]), 'e': (east[2] - east[0]),
                     'lit': lit, 'dark': dark, 'rgb_dark': img[r, c0 + c - 3:c0 + c + 4].mean(axis=0)})
    if len(rows) < 8:
        return None
    sep = np.median([q['e50'] - q['w50'] for q in rows])
    s = 2.0 / sep
    rr = np.array([q['row'] for q in rows], np.float32)
    e50 = np.array([q['e50'] for q in rows], np.float32)
    slope = np.polyfit(rr, e50, 1)[0]   # px of x per px of z
    tilt = math.atan(slope)
    # ground z of each row: the view centre is TILT_DZ north of the eye, image rows go south (+z)
    for q in rows:
        q['z'] = cam_z - TILT_DZ + (q['row'] - h / 2.0) * s
    return {'scale': s, 'tilt': tilt, 'rows': rows}


def setpos_lines(x, z, name):
    """The view centre over ground (x, z): the eye sits TILT_DZ south of it."""
    p = OUT + '/' + name
    return ['setpos %.3f %.2f %.3f 0 %.1f' % (x, EYE_Y, z + TILT_DZ, PITCH), 'noclip', 'wait 90', 'shadow_info',
            'screenshot_hdr %s %s.png %s.exr' % (name, p, p), 'wait 6']


def penumbra_and_contrast():
    presets = ['clear_noon', 'hazy', 'light_cloud', 'overcast']
    dists = [1.0, 5.0, 20.0]
    lines = []
    for pr in presets:
        lines += ['shadow_preset ' + pr, 'time_pause 1', 'wait 240']
        for d in dists:
            # t: ground distance along the shadow (sun at ~66 deg: cos e ~ 0.40)
            t = d * 0.40
            lines += setpos_lines(FIN_E, FIN_Z - t, '%s_%g' % (pr, d))
    text = run_game('penumbra', lines)
    info = infos(text)
    if len(info) < len(presets) * len(dists):
        check(False, 'penumbra: %d of %d shadow_info readouts in the log' % (len(info), len(presets) * len(dists)))
        return
    ratios, chroma = {}, {}
    k = 0
    for pr in presets:
        for d in dists:
            inf = info[k]
            k += 1
            name = '%s_%g' % (pr, d)
            path = OUT + '/' + name + '.exr'
            if not os.path.exists(path):
                check(False, 'penumbra: %s.exr missing' % name)
                continue
            img = read_exr(path)
            fp = fin_profile(img, FIN_Z - d * 0.40 + TILT_DZ)
            if pr == 'overcast':
                Y = lum(img)
                ratios[pr] = float(np.percentile(Y, 90) / max(np.percentile(Y, 10), 1e-6))   # no cast shadow: flat
                h, w = Y.shape
                chroma[pr] = img[h // 2 - 20:h // 2 + 20, w // 2 - 20:w // 2 + 20].reshape(-1, 3).mean(axis=0)
                continue
            if fp is None:
                check(False, 'penumbra: %s: no fin shadow edges found' % name)
                continue
            e = math.radians(inf['elev'])
            # each row's own blocker distance: ground distance along the shadow / cos(elevation)
            mid = fp['rows'][len(fp['rows']) // 2 - 2:len(fp['rows']) // 2 + 3]
            dd = float(np.median([(FIN_Z - q['z']) / math.cos(fp['tilt']) / math.cos(e) for q in mid]))
            R = dd * math.tan(math.radians(inf['radius']))
            expect = disc_10_90(R)
            widths = [0.5 * (q['w'] + q['e']) * fp['scale'] * math.cos(fp['tilt']) for q in mid]
            got = float(np.median(widths))
            tex = R / TEXEL
            log('      %s: elev %.1f, radius %.2f deg, d %.2f m, half width %.4f m (%.1f texels): 10-90 expected %.4f, measured %.4f' % (name, inf['elev'], inf['radius'], dd, R, tex, expect, got))
            if pr in ('clear_noon', 'hazy'):
                if tex >= 3.0:
                    check(abs(got - expect) <= 0.15 * expect, 'penumbra %s: %.4f vs %.4f m (15 %%)' % (name, got, expect))
                else:
                    check(got <= expect + 2.0 * TEXEL * 1.8, 'penumbra %s: sub-texel penumbra %.4f <= %.4f m' % (name, got, expect + 2.0 * TEXEL * 1.8))
            if d == 5.0:
                mid = fp['rows'][len(fp['rows']) // 2]
                ratios[pr] = float(np.median([q['lit'] / max(q['dark'], 1e-6) for q in fp['rows']]))
                chroma[pr] = mid['rgb_dark']
                check(abs(ratios[pr] - inf['ratio']) <= 0.2 * inf['ratio'], 'contrast %s: lit / shade %.2f vs predicted %.2f (20 %%)' % (pr, ratios[pr], inf['ratio']))
    if len(ratios) == 4:
        r = ratios
        check(r['clear_noon'] > 1.5 * r['hazy'] and r['hazy'] > r['light_cloud'] and r['light_cloud'] > r['overcast'],
              'contrast order: clear %.2f >> hazy %.2f > light cloud %.2f > overcast %.2f' % (r['clear_noon'], r['hazy'], r['light_cloud'], r['overcast']))
        check(r['overcast'] < 1.05, 'contrast: overcast lit / shade %.3f < 1.05' % r['overcast'])
    if 'clear_noon' in chroma and 'overcast' in chroma:
        bc = chroma['clear_noon'][2] / max(chroma['clear_noon'][0], 1e-6)
        bo = chroma['overcast'][2] / max(chroma['overcast'][0], 1e-6)
        check(bc > bo + 0.05, 'chromaticity: shade b / r clear %.3f > overcast %.3f' % (bc, bo))


def cutoff():
    lines = ['shadow_preset clear_noon', 'time_pause 1', 'wait 240'] + setpos_lines(FIN_E, FIN_Z - 2.0, 'cut_start')
    lines += ['weather_lock 0', 'weather_set overcast 120', 'weather_lock 1']
    for i in range(61):
        p = OUT + '/cut_%02d' % i
        lines += ['shadow_info', 'screenshot_hdr cut_%02d %s.png %s.exr' % (i, p, p), 'wait 120']
    text = run_game('cutoff', lines)
    info = infos(text)
    rs = []
    for i in range(61):
        path = OUT + '/cut_%02d.exr' % i
        if not os.path.exists(path):
            break
        img = read_exr(path)
        Y = lum(img)
        h, w = Y.shape
        # the fixed view: shade = the fin's shadow band at the centre, lit = the line's median
        line = Y[h // 2 - 10:h // 2 + 10].mean(axis=0)
        rs.append(float(np.median(line) / max(np.percentile(line, 3), 1e-6)))
    if len(rs) < 30:
        check(False, 'cutoff: %d EXR frames' % len(rs))
        return
    d2 = max(abs(rs[i + 1] - 2 * rs[i] + rs[i - 1]) for i in range(1, len(rs) - 1))
    log('      cutoff: lit / shade %s' % ' '.join('%.2f' % r for r in rs[::5]))
    check(d2 < 0.08 * max(1.0, max(rs) - 1.0), 'cutoff: smooth (max second difference %.3f)' % d2)
    check(rs[-1] < 1.05, 'cutoff: overcast end lit / shade %.3f < 1.05' % rs[-1])
    off = [q for q in info[1:] if q['mode'].startswith('off')]
    if off:
        check(off[0]['C'] < 0.08 * 1.25, 'cutoff: the passes stopped at C %.3f (cutoff 0.08)' % off[0]['C'])
    else:
        check(False, 'cutoff: the cast shadow never turned off')


def moon():
    lines = ['shadow_preset night_moon', 'time_pause 1', 'wait 300'] + setpos_lines(FIN_E, FIN_Z - 2.0, 'moon_night')
    lines += ['time_set 04:00', 'wait 300', 'shadow_info']
    text = run_game('moon', lines)
    info = infos(text)
    if len(info) < 2:
        check(False, 'moon: %d shadow_info readouts' % len(info))
        return
    n, dawn = info[0], info[-1]
    check(n['key'] == 'moon' and n['fade'] > 0.9, 'moon: night key %s fade %.2f' % (n['key'], n['fade']))
    check(n['radius'] < 0.4, 'moon: source radius %.2f deg' % n['radius'])
    path = OUT + '/moon_night.exr'
    if os.path.exists(path):
        img = read_exr(path)
        fp = fin_profile(img, FIN_Z - 2.0 + TILT_DZ)
        rr = float(np.median([q['lit'] / max(q['dark'], 1e-6) for q in fp['rows']])) if fp else 0.0
        check(rr > 1.5, 'moon: lit / shade %.2f > 1.5 (moon shadows visible)' % rr)
    else:
        check(False, 'moon: moon_night.exr missing')
    check(dawn['fade'] < 0.1 or dawn['mode'].startswith('off'), 'moon: dawn (04:00, sun %.1f deg) key %s fade %.2f' % (dawn['elev'], dawn['key'], dawn['fade']))


def tiers():
    lines = ['shadow_preset clear_noon', 'time_pause 1', 'wait 120', 'setpos -50 0.05 -8 200 -20', 'wait 30']
    names = []
    for lvl in range(5):
        for era in range(4):
            nm = 'tier_%d_era_%d' % (lvl, era)
            names.append((lvl, era, nm))
            lines += ['r_fidelity %d' % lvl, 'r_era %d' % era, 'wait 40', 'shadow_info', 'screenshot %s/%s.png' % (OUT, nm), 'wait 4']
    text = run_game('tiers', lines, validation=VALIDATION)
    info = infos(text)
    try:
        from PIL import Image
    except ImportError:
        Image = None
    for i, (lvl, era, nm) in enumerate(names):
        p = '%s/%s.png' % (OUT, nm)
        if not os.path.exists(p):
            check(False, 'tiers: %s.png missing' % nm)
            continue
        if Image:
            a = np.asarray(Image.open(p).convert('L'), dtype=np.float32) / 255.0
            check(a.mean() > 0.05 and a.std() > 0.03, 'tiers: %s plausible (mean %.2f std %.2f)' % (nm, a.mean(), a.std()))
        if i < len(info) and era == 3:
            check(info[i]['near'] >= 0.999 and info[i]['pcss'] == 0.0 and info[i]['mode'].startswith('fixed'), 'tiers: %s fixed hardness (pcss %.2f, %s)' % (nm, info[i]['pcss'], info[i]['mode']))
    if VALIDATION:
        n = vuids(text)
        check(n == 0, 'tiers: %d validation messages' % n)


def taa():
    lines = ['shadow_preset hazy', 'time_pause 1', 'wait 240'] + setpos_lines(FIN_E, FIN_Z - 2.0, 'taa_start')
    lines += ['weather_params fog_density 0.035 time 60', 'wait 600']
    for i in range(24):
        p = OUT + '/taa_%02d' % i
        lines += ['screenshot_hdr taa_%02d %s.png %s.exr' % (i, p, p), 'wait 1']
    run_game('taa', lines)
    frames = []
    for i in range(24):
        path = OUT + '/taa_%02d.exr' % i
        if os.path.exists(path):
            frames.append(lum(read_exr(path)))
    if len(frames) < 16:
        check(False, 'taa: %d frames' % len(frames))
        return
    st = np.stack(frames)
    h, w = st.shape[1:]
    band = st[:, h // 2 - 40:h // 2 + 40, :]
    mean = band.mean(axis=0)
    step = np.percentile(mean, 90) - np.percentile(mean, 3)
    # the edge pixels: between 20 % and 80 % of the step
    lo, hi = np.percentile(mean, 3) + 0.2 * step, np.percentile(mean, 3) + 0.8 * step
    mask = (mean > lo) & (mean < hi)
    sd = float((band.std(axis=0)[mask]).mean() / max(step, 1e-6)) if mask.any() else 1.0
    check(mask.sum() > 50 and sd < 0.03, 'taa: edge temporal std %.4f of the step (%d px)' % (sd, int(mask.sum())))


def main():
    if not os.path.exists(EXE):
        print('no game executable at %s' % EXE)
        return 2
    groups = [('penumbra', penumbra_and_contrast), ('contrast', None), ('cutoff', cutoff), ('moon', moon), ('tiers', tiers), ('taa', taa)]
    for name, fn in groups:
        if fn is None:
            continue
        if ONLY and ONLY != name and not (ONLY == 'contrast' and name == 'penumbra'):
            continue
        fn()
    log('shadow_ref: %d failure(s)' % failures)
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, 'report.txt'), 'w') as f:
        f.write('\n'.join(report) + '\n')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
