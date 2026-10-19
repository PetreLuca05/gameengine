"""Looks test suite (docs/LOOKS.md), run after a build:

  python tools/tests/run_looks_tests.py [build_dir] [--res WxH] [--skip-validation] [--no-budget] [--quick]

  1. unit     test_looks, test_looks_release (look_tune compiled out), tools/tests/lint_looks.py.
  2. matrix   every look x 4 times of day (06:30, 12:00, 19:30, 00:30) x 3 weathers (clear, overcast,
              storm) x every Engine Fidelity level (Off .. Modern), with the Vulkan validation layers:
              every combination reaches its frame, 0 validation messages, and the look switches leave
              every r_* cvar but r_look exactly as they were (no fidelity / era cvar touched).
  3. frames   one screenshot per look (noon, clear, Modern, HUD hidden): PUBG-style less saturated
              with lifted blacks vs Realism, Gritty less saturated, Vibrant more, Cinematic letterboxed,
              every frame plausible (not black, not uniform).
  4. bench    post_bench per look at 1920x1080: the tonemap pass (the whole grade + finish) <= 0.15 ms
              (--no-budget: report only, for software / slow GPUs).

--quick: matrix at Off / Low / Modern, noon / night, clear / storm only.
Headless on Linux: runs under xvfb-run when there is no DISPLAY (Mesa lavapipe works).
Exit code 0 when everything passes. Results: <build>/looks_tests/report.txt
"""
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
args = [a for a in sys.argv[1:] if not a.startswith('--')]
opts = sys.argv[1:]
BUILD = os.path.abspath(args[0] if args else os.path.join(ROOT, 'build'))
BIN = os.path.join(BUILD, 'bin')
OUT = os.path.join(BUILD, 'looks_tests')
RES = opts[opts.index('--res') + 1] if '--res' in opts else '960x540'
VALIDATION = '--skip-validation' not in opts
BUDGET = '--no-budget' not in opts
QUICK = '--quick' in opts
VVL = os.environ.get('VK_ADD_LAYER_PATH', os.path.join(ROOT, '.toolchain', 'vvl', 'install', 'bin'))
EXE = os.path.join(BIN, 'deadground.exe' if os.name == 'nt' else 'deadground')
LOOKS = ['realism', 'pubg', 'cinematic', 'vibrant', 'gritty']
LEVELS = [0, 1, 4] if QUICK else [0, 1, 2, 3, 4]
TIMES = ['12:00', '00:30'] if QUICK else ['06:30', '12:00', '19:30', '00:30']
WEATHERS = ['clear', 'storm'] if QUICK else ['clear', 'overcast', 'storm']
GRADE_BUDGET_MS = 0.15
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


def exe(name):
    return os.path.join(BIN if os.path.exists(os.path.join(BIN, name + ('.exe' if os.name == 'nt' else ''))) else BUILD, name + ('.exe' if os.name == 'nt' else ''))


def run_game(name, lines, res=RES, validation=False, timeout=5400):
    os.makedirs(OUT, exist_ok=True)
    w, h = res.split('x')
    script = os.path.join(OUT, name + '.cfg')
    with open(script, 'w') as f:
        f.write('r_maxfps 0\nr_width %s\nr_height %s\nvid_restart\nwait 30\ntime_pause 1\nr_look_modulators 1\n' % (w, h))
        f.write('\n'.join(lines) + '\nquit\n')
    log_path = os.path.join(OUT, name + '.log')
    cmd = [EXE, '--test', '--nosound', '--log', log_path, '--script', script]
    if validation:
        cmd.insert(1, '--validation')
    if os.name != 'nt' and not os.environ.get('DISPLAY') and shutil.which('xvfb-run'):
        cmd = ['xvfb-run', '-a', '-s', '-screen 0 %sx%sx24' % (w, h)] + cmd
    env = dict(os.environ)
    if os.path.isdir(VVL):
        env['VK_ADD_LAYER_PATH'] = VVL
    subprocess.run(cmd, cwd=BIN, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=timeout)
    with open(log_path, encoding='utf-8', errors='replace') as f:
        return f.read()


def unit():
    for name in ('test_looks', 'test_looks_release'):
        p = subprocess.run([exe(name)], cwd=BIN, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        out = p.stdout.decode('utf-8', 'replace').strip().splitlines()
        check(p.returncode == 0, 'unit: ' + (out[-1] if out else name + ': no output'))
    p = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'tests', 'lint_looks.py')], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = p.stdout.decode('utf-8', 'replace').strip().splitlines()
    check(p.returncode == 0, 'unit: ' + (out[-1] if out else 'lint_looks: no output'))


def cvar_snapshot(text, tag):
    """The `cvarlist r_` dump between `echo <tag>` and `echo <tag> end`: {name: value}."""
    m = re.search(re.escape(tag) + r' \n(.*?)' + re.escape(tag) + ' end', text, re.S)
    if not m:
        return None
    return dict(re.findall(r'^.*?  (r_\w+)\s+= (\S*)', m.group(1), re.M))


def matrix():
    lines = []
    for lvl in LEVELS:
        lines += ['r_fidelity %d' % lvl, 'wait 15', 'echo LM_CVARS_%d' % lvl, 'cvarlist r_', 'echo LM_CVARS_%d end' % lvl]
        for tod in TIMES:
            lines += ['time_set ' + tod + ' instant', 'wait 4']
            for wx in WEATHERS:
                lines += ['weather_set %s 0' % wx, 'wait 4']
                for look in LOOKS:
                    lines += ['look ' + look, 'wait 2', 'echo LM_FRAME %d %s %s %s' % (lvl, tod, wx, look)]
        lines += ['look realism', 'wait 2', 'echo LM_AFTER_%d' % lvl, 'cvarlist r_', 'echo LM_AFTER_%d end' % lvl]
    text = run_game('matrix', lines, validation=VALIDATION)
    want = len(LEVELS) * len(TIMES) * len(WEATHERS) * len(LOOKS)
    got = len(re.findall(r'LM_FRAME \d', text))
    check(got == want, 'matrix: %d / %d look x time x weather x fidelity combinations rendered' % (got, want))
    for lvl in LEVELS:
        a, b = cvar_snapshot(text, 'LM_CVARS_%d' % lvl), cvar_snapshot(text, 'LM_AFTER_%d' % lvl)
        if a is None or b is None:
            check(False, 'matrix: level %d cvar snapshots missing' % lvl)
            continue
        changed = sorted(k for k in set(a) | set(b) if k != 'r_look' and a.get(k) != b.get(k))
        check(not changed, 'matrix: level %d, 15 look switches left the r_* cvars alone%s' % (lvl, (' (changed: %s)' % ', '.join(changed)) if changed else ''))
    if VALIDATION:
        msgs = re.findall(r'Validation Error: \[ (VUID-[\w-]+)', text)
        kinds = sorted(set(msgs))
        check(not msgs, 'matrix: %d validation messages%s' % (len(msgs), (' (%s)' % ', '.join(kinds)) if kinds else ''))
    check('looks LUTs baked' in text or 'look LUTs baked' in text, 'matrix: the look LUTs were baked')


def stats(path):
    from PIL import Image
    import numpy as np
    a = np.asarray(Image.open(path).convert('RGB'), dtype=np.float32) / 255.0
    h, w, _ = a.shape
    centre = a[int(h * 0.2):int(h * 0.8), int(w * 0.4):int(w * 0.95)]   # away from the photo-mode panel / HUD
    mx, mn = centre.max(axis=2), centre.min(axis=2)
    sat = np.where(mx > 1e-3, (mx - mn) / np.maximum(mx, 1e-3), 0.0)
    luma = centre @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)
    top = a[:int(h * 0.05), int(w * 0.4):int(w * 0.95)].mean()
    return {'sat': float(sat.mean()), 'black': float(np.percentile(luma, 1)), 'mean': float(luma.mean()), 'std': float(luma.std()), 'top': float(top)}


def frames():
    try:
        import numpy  # noqa: F401
        from PIL import Image  # noqa: F401
    except ImportError:
        log('skip  frames: PIL / numpy missing')
        return
    lines = ['r_fidelity 4', 'wait 15', 'time_set 12:00 instant', 'weather_set clear 0', 'photomode 1', 'wait 10']
    shots = {}
    for look in LOOKS:
        shots[look] = os.path.join(OUT, 'look_%s.png' % look).replace('\\', '/')
        lines += ['look ' + look, 'wait 6', 'screenshot ' + shots[look], 'wait 3']
    run_game('frames', lines)
    st = {}
    for look, path in shots.items():
        if not os.path.exists(path):
            check(False, 'frames: %s screenshot missing' % look)
            continue
        st[look] = stats(path)
        s = st[look]
        log('      %-10s saturation %.3f  1%% black %.3f  mean %.3f  std %.3f  top rows %.3f' % (look, s['sat'], s['black'], s['mean'], s['std'], s['top']))
        check(s['mean'] > 0.05 and s['std'] > 0.03, 'frames: %s plausible' % look)
    if len(st) == len(LOOKS):
        r = st['realism']
        check(st['pubg']['sat'] < r['sat'] and st['pubg']['black'] > r['black'], 'frames: PUBG-style less saturated with lifted blacks')
        check(st['gritty']['sat'] < r['sat'], 'frames: Gritty less saturated')
        check(st['vibrant']['sat'] > r['sat'], 'frames: Vibrant more saturated')
        check(st['cinematic']['top'] < 0.02 and r['top'] > st['cinematic']['top'], 'frames: Cinematic letterboxed')


def bench():
    lines = ['r_fidelity 4', 'wait 15', 'time_set 12:00 instant', 'weather_set clear 0', 'wait 10']
    for look in LOOKS:
        lines += ['look ' + look, 'wait 10', 'post_bench 120 look_' + look, 'wait 140']
    text = run_game('bench', lines, res='1920x1080')
    for look in LOOKS:
        m = re.search(r'post_bench look_%s: \d+ frames.*?\n((?:.*\n)*?).*?post_bench look_%s: post' % (look, look), text)
        t = re.search(r'tonemap\s+([\d.]+) ms', m.group(1)) if m else None
        if not t:
            check(False, 'bench: %s tonemap pass not measured' % look)
            continue
        ms = float(t.group(1))
        if BUDGET:
            check(ms <= GRADE_BUDGET_MS, 'bench: %s grading pass %.3f ms (budget %.2f ms at 1080p)' % (look, ms, GRADE_BUDGET_MS))
        else:
            log('      %-10s grading pass %.3f ms at 1080p (budget %.2f ms, not checked)' % (look, ms, GRADE_BUDGET_MS))


def main():
    os.makedirs(OUT, exist_ok=True)
    log('Looks tests: %s, %s%s%s' % (EXE, RES, ', validation' if VALIDATION else '', ', quick' if QUICK else ''))
    unit()
    matrix()
    frames()
    bench()
    log('%s (%d failure(s))' % ('PASS' if not failures else 'FAIL', failures))
    with open(os.path.join(OUT, 'report.txt'), 'w') as f:
        f.write('\n'.join(report) + '\n')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
