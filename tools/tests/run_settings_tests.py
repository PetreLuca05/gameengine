"""Settings audit test suite (docs/SETTINGS_AUDIT.md), run after a build:

  python tools/tests/run_settings_tests.py [build_dir] [--res WxH]

  1. unit    test_settings (locks, config filter, consistency set, join check, locked set per tier)
             and lint_settings.py (one class flag per registration, audit / consistency files, the
             Settings tabs, the docs table).
  2. tiers   tools/tests/settings_tiers.cfg: every Engine Fidelity level in the devroom foliage yard,
             in fog, vgeo (era modern) and classic (era 2015). Per renderer, every level must show
             the same foliage instances (within 5 %), impostors (within 10 %), the same fog density and
             the same locked values (shadow distance, lamp / cloud shadows, lens droplets, auto
             exposure, foliage density, impostor switch, LOD scales, decal budget).
  3. locks   tools/tests/settings_locks.cfg with --release-locks: an EDITABLE write accepted, TIER /
             LOCKED writes rejected, a host rule accepted offline, no rejection from the level and
             era changes.

Headless on Linux: runs under xvfb-run when there is no DISPLAY (Mesa lavapipe works, ~1 fps).
Exit code 0 when everything passes. Results: <build>/settings_tests/report.txt
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
OUT = os.path.join(BUILD, 'settings_tests')
RES = opts[opts.index('--res') + 1] if '--res' in opts else '1600x900'   # the X screen (xvfb); the window keeps r_width / r_height
EXE = os.path.join(BIN, 'deadground.exe' if os.name == 'nt' else 'deadground')
LOCKED_KEYS = ['fog', 'shadow_distance', 'light_shadows', 'cloud_shadows', 'lens_drops', 'auto_exposure', 'foliage_density', 'impostor_px', 'lod_bias',
               'lod_static', 'lod_char', 'lod_foliage', 'vfx_decals']
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


def run_game(name, cfg, extra=()):
    os.makedirs(OUT, exist_ok=True)
    logp = os.path.join(OUT, name + '.log')
    w, h = RES.split('x')
    cmd = [EXE, '--test', '--nosound', '--log', logp] + list(extra) + ['--script', os.path.join(ROOT, 'tools', 'tests', cfg)]
    if os.name != 'nt' and not os.environ.get('DISPLAY') and shutil.which('xvfb-run'):
        cmd = ['xvfb-run', '-a', '-s', '-screen 0 %sx%sx24' % (w, h)] + cmd
    r = subprocess.run(cmd, cwd=BIN, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=7200)
    text = open(logp, encoding='utf-8', errors='replace').read() if os.path.exists(logp) else ''
    return r.returncode, text


def parse_vis(text):
    rows = []
    for line in text.splitlines():
        if 'settings_visibility+' in line and rows:   # the continuation lines (LOG_LINE_MAX)
            rows[-1].update(re.findall(r'(\w+)=([^\s]+)', line.split('settings_visibility+', 1)[1]))
            continue
        if 'settings_visibility:' not in line:
            continue
        kv = dict(re.findall(r'(\w+)=([^\s]+)', line.split('settings_visibility:', 1)[1]))
        rows.append(kv)
    return rows


def near(a, b, rel, absolute):
    return abs(a - b) <= max(absolute, rel * max(abs(a), abs(b)))


def main():
    log('== unit')
    for exe in ('test_settings', 'test_fidelity'):
        p = os.path.join(BIN, exe + ('.exe' if os.name == 'nt' else ''))
        if os.path.exists(p):
            r = subprocess.run([p], cwd=BIN, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            tail = r.stdout.decode(errors='replace').strip().splitlines()[-1:] or ['']
            check(r.returncode == 0, '%s: %s' % (exe, tail[0]))
        else:
            check(False, '%s not built' % exe)
    r = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'tests', 'lint_settings.py')], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    check(r.returncode == 0, 'lint_settings: %s' % r.stdout.decode(errors='replace').strip().splitlines()[-1])

    log('== tiers')
    code, text = run_game('tiers', 'settings_tiers.cfg')
    rows = parse_vis(text)
    check(code == 0 and len(rows) == 10, 'settings_tiers.cfg ran (%d visibility rows, exit %d)' % (len(rows), code))
    groups = {}
    for kv in rows:
        groups.setdefault(kv.get('vgeo', '?'), []).append(kv)
    for vg, g in sorted(groups.items()):
        name = 'vgeo' if vg == '1' else 'classic'
        ref = next((kv for kv in g if kv.get('level') == 'modern'), g[-1])
        for kv in g:
            lv = kv.get('level')
            for k in LOCKED_KEYS:
                a, b = float(kv.get(k, 'nan')), float(ref.get(k, 'nan'))
                check(near(a, b, 1e-3, 1e-6), '%s %s: %s %g (Modern %g)' % (name, lv, k, a, b))
            check(float(kv.get('shadows', 0)) >= 1 and float(kv.get('water', 0)) >= 1 and float(kv.get('precip', 0)) >= 1,
                  '%s %s: floors (shadows %s, water %s, precip %s)' % (name, lv, kv.get('shadows'), kv.get('water'), kv.get('precip')))
            if kv.get('counts') == '1' and ref.get('counts') == '1':
                v, rv = float(kv['visible']), float(ref['visible'])
                check(near(v, rv, 0.05, 50), '%s %s: %d visible foliage / instances (Modern %d)' % (name, lv, v, rv))
                i, ri = float(kv['impostors']), float(ref['impostors'])
                check(near(i, ri, 0.10, 50), '%s %s: %d impostors (Modern %d)' % (name, lv, i, ri))
    check('consistency set' in text and 'release locks' in text, 'settings_audit ran')

    log('== locks')
    code, text = run_game('locks', 'settings_locks.cfg', ['--release-locks'])
    res = dict(re.findall(r'settings_lock_test: (\w+) \(\S+\) \S+ -> \S+: (\w+)', text))
    want = {'r_vignette': 'accepted', 'r_ao': 'rejected', 'r_lens_drops': 'rejected', 'r_foliage_density': 'accepted'}
    for k, v in want.items():
        check(res.get(k) == v, '%s: %s (want %s)' % (k, res.get(k), v))
    stray = [l for l in text.splitlines() if 'rejected' in l and 'write "' in l and not any(n in l for n in ('r_ao is', 'r_lens_drops is'))]
    check(not stray, 'no rejection from the level / era changes%s' % ((': ' + stray[0]) if stray else ''))

    os.makedirs(OUT, exist_ok=True)
    open(os.path.join(OUT, 'report.txt'), 'w').write('\n'.join(report) + '\n')
    log('run_settings_tests: %d failure(s)' % failures)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
