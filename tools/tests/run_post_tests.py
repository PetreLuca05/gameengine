"""Post-processing test suite (run after a build):

  python tools/tests/run_post_tests.py [build_dir] [--skip-perf] [--only toggle|perf|taa]

  1. toggle  every pass disabled on its own + every AA / upscaler / tonemapper / debug view /
             preset / feedback / LUT / photo mode / HDR capture, rendered with the Vulkan
             validation layers: 0 validation messages, every screenshot a plausible frame
             (not black / not uniform), the EXR capture readable.
  2. perf    GPU ms per preset (no validation): post total <= 4 ms on High, <= 2 ms on Medium.
  3. taa     ghosting: a scripted camera pan rendered with TAA vs. no AA (same poses); TAA may
             differ at edges only (aliasing), not by trails; after the pan stops the image must
             settle (frame 12 after stop vs. frame 200: no residual trails).
Exit code 0 when everything passes. Results: <build>/post_tests/report.txt
"""
import os
import re
import struct
import subprocess
import sys
import zlib

import numpy as np
from PIL import Image

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
args = [a for a in sys.argv[1:] if not a.startswith('--')]
BUILD = os.path.abspath(args[0] if args else os.path.join(ROOT, 'build'))
BIN = os.path.join(BUILD, 'bin')
OUT = os.path.join(BUILD, 'post_tests').replace('\\', '/')
ONLY = None
if '--only' in sys.argv:
    ONLY = sys.argv[sys.argv.index('--only') + 1]
SKIP_PERF = '--skip-perf' in sys.argv
VVL = os.environ.get('VK_ADD_LAYER_PATH', os.path.join(ROOT, '.toolchain', 'vvl', 'install', 'bin'))
report = []


def log(msg):
    print(msg)
    report.append(msg)


def run(script, validation, timeout=900):
    env = dict(os.environ)
    env['VK_ADD_LAYER_PATH'] = VVL
    cmd = [os.path.join(BIN, 'deadground.exe'), '--test', '--nosound', '--script', script]
    if validation:
        cmd.insert(1, '--validation')
    p = subprocess.run(cmd, cwd=BIN, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
    text = p.stdout.decode('utf-8', 'replace')
    return p.returncode, text


def gen_scripts():
    subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'tests', 'gen_post_tests.py'), OUT], check=True)


def load(path):
    return np.asarray(Image.open(path).convert('RGB'), dtype=np.float32) / 255.0


def plausible(img):
    # Ignore the HUD strip at the top; a real frame has some mean brightness and variation.
    body = img[40:-120]
    m, sd = float(body.mean()), float(body.std())
    return 0.02 < m < 0.995 and sd > 0.015, m, sd


def read_exr_header(path):
    with open(path, 'rb') as f:
        data = f.read()
    if struct.unpack('<I', data[:4])[0] != 20000630:
        return None
    m = re.search(rb'dataWindow\x00box2i\x00\x10\x00\x00\x00(.{16})', data, re.S)
    if not m:
        return None
    x0, y0, x1, y1 = struct.unpack('<4i', m.group(1))
    return x1 - x0 + 1, y1 - y0 + 1, len(data)


def test_toggle():
    os.makedirs(OUT + '/post_toggle', exist_ok=True)
    for f in os.listdir(OUT + '/post_toggle'):
        os.remove(os.path.join(OUT + '/post_toggle', f))
    rc, text = run(os.path.join(ROOT, 'tools', 'tests', 'post_toggle.cfg'), True)
    with open(OUT + '/toggle.log', 'w', encoding='utf-8') as f:
        f.write(text)
    vk = [l for l in text.splitlines() if '[vk]' in l or '[vk+]' in l]
    names = [l.split('post_toggle/')[1].split('.png')[0] for l in open(os.path.join(ROOT, 'tools', 'tests', 'post_toggle.cfg'))
             if l.startswith('screenshot ') and 'post_toggle/' in l]
    ok = rc == 0 and not vk
    bad = []
    for n in names:
        p = OUT + '/post_toggle/' + n + '.png'
        if not os.path.exists(p):
            bad.append(n + ' (missing)')
            continue
        good, m, sd = plausible(load(p))
        # Velocity, flare and motion blur tiles are legitimately empty with a still camera.
        if not good and n in ('debug_05', 'debug_15', 'debug_16'):
            good = True
        # Indirect-light views are dim outdoors (little bounce light): only need some variation.
        if not good and n in ('debug_04',) and sd > 0.015:
            good = True
        if not good:
            bad.append('%s (mean %.3f sd %.3f)' % (n, m, sd))
    exr = read_exr_header(OUT + '/post_toggle/hdr_capture.exr') if os.path.exists(OUT + '/post_toggle/hdr_capture.exr') else None
    log('toggle: exit %d, %d screenshots, %d validation messages, %d implausible, exr %s' % (rc, len(names), len(vk), len(bad), exr))
    for l in vk[:10]:
        log('   ' + l[:300])
    for b in bad:
        log('   implausible: ' + b)
    if not exr:
        log('   EXR capture missing / unreadable')
    return ok and not bad and exr is not None


def test_perf():
    rc, text = run(os.path.join(ROOT, 'tools', 'tests', 'post_perf.cfg'), False)
    with open(OUT + '/perf.log', 'w', encoding='utf-8') as f:
        f.write(text)
    res = {}
    for m in re.finditer(r'post_bench (\w+): post ([\d.]+) ms, all passes ([\d.]+) ms, memory ([\d.]+) MB', text):
        res[m.group(1)] = (float(m.group(2)), float(m.group(3)), float(m.group(4)))
    # Per-pass table of the last bench.
    blocks = re.findall(r'post_bench (\w+): \d+ frames.*?\n((?:.*?\d+\.\d+ ms.*\n)+)', text)
    for name, body in blocks:
        log('perf %s:' % name)
        for line in body.strip().splitlines():
            log('   ' + line.split('] ', 1)[-1].replace('info  ', '').strip())
    budgets = {'high': 4.0, 'medium': 2.0}
    ok = rc == 0 and len(res) == 4
    for k in ('low', 'medium', 'high', 'ultra'):
        if k in res:
            post, allp, mem = res[k]
            b = budgets.get(k)
            verdict = '' if b is None else (' <= %.1f ms OK' % b if post <= b else ' > %.1f ms BUDGET EXCEEDED' % b)
            if b is not None and post > b:
                ok = False
            log('perf %-6s post %.3f ms  all passes %.3f ms  frame graph memory %.1f MB%s' % (k, post, allp, mem, verdict))
        else:
            log('perf %-6s missing' % k)
    return ok


def ghost_fraction(a, b, thresh):
    d = np.abs(a - b).max(axis=2)
    body = d[40:-120]
    return float((body > thresh).mean())


def test_taa():
    vk_total = 0
    for mode in ('taa', 'noaa'):
        rc, text = run(os.path.join(ROOT, 'tools', 'tests', 'post_taa_%s.cfg' % mode), True)
        vk_total += sum(1 for l in text.splitlines() if '[vk]' in l)
        if rc != 0:
            log('taa: %s run failed (exit %d)' % (mode, rc))
            return False
    t_move, n_move = load(OUT + '/taa_taa_moving.png'), load(OUT + '/taa_noaa_moving.png')
    t_stop, t_still = load(OUT + '/taa_taa_stop12.png'), load(OUT + '/taa_taa_still.png')
    n_stop, n_still = load(OUT + '/taa_noaa_stop12.png'), load(OUT + '/taa_noaa_still.png')
    # Moving: TAA vs. no AA at the same pose. Large differences (> 0.3) are trails / smearing;
    # aliasing alone only differs along edges by smaller amounts.
    moving = ghost_fraction(t_move, n_move, 0.3)
    moving_mean = float(np.abs(t_move - n_move).mean())
    # Settling: the TAA image 12 frames after the stop vs. 200 frames later (history converged).
    settle = ghost_fraction(t_stop, t_still, 0.2)
    settle_ref = ghost_fraction(n_stop, n_still, 0.2)   # what changes anyway (animated things)
    ok = moving < 0.01 and settle < 0.005 + settle_ref and vk_total == 0
    log('taa: moving diff>0.3 %.3f%% (mean %.4f), settle diff>0.2 %.3f%% (no-AA baseline %.3f%%), validation %d -> %s' %
        (moving * 100, moving_mean, settle * 100, settle_ref * 100, vk_total, 'PASS' if ok else 'FAIL'))
    # Difference image for inspection.
    d = (np.clip(np.abs(t_move - n_move) * 3, 0, 1) * 255).astype(np.uint8)
    Image.fromarray(d).save(OUT + '/taa_diff_moving.png')
    return ok


def main():
    os.makedirs(OUT, exist_ok=True)
    gen_scripts()
    results = {}
    if ONLY in (None, 'toggle'):
        results['toggle'] = test_toggle()
    if ONLY in (None, 'perf') and not SKIP_PERF:
        results['perf'] = test_perf()
    if ONLY in (None, 'taa'):
        results['taa'] = test_taa()
    log('RESULT: ' + ', '.join('%s %s' % (k, 'PASS' if v else 'FAIL') for k, v in results.items()))
    with open(OUT + '/report.txt', 'w') as f:
        f.write('\n'.join(report) + '\n')
    return 0 if all(results.values()) else 1


if __name__ == '__main__':
    sys.exit(main())
