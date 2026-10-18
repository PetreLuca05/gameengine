"""Settings audit: visibility outcomes per Engine Fidelity level (docs/SETTINGS_AUDIT.md), run after a build:

  python tools/tests/run_settings_tests.py [build_dir] [--res WxH]

For every level the game starts headless-capable (--test), switches to the level, scatters the dev room
foliage yard (foliage_test_scatter), stands at a fixed camera in fog at noon and logs visibility_probe.
The levels must agree: the locked parameters exactly (foliage density, shadow distance, impostor / LOD
thresholds, LOD / grass ranges, droplets, decals, draw distance, the consistency hash), the measured
outcomes within tolerance (visible / faded foliage instances 5 %, fog extinction 2 %). A second pass runs
--release-locks and checks that a player write of a locked and a tier cvar is rejected.
Results: <build>/settings_tests/report.txt; exit code 0 when everything passes.
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
RES = opts[opts.index('--res') + 1] if '--res' in opts else '1280x720'
EXE = os.path.join(BIN, 'deadground.exe' if os.name == 'nt' else 'deadground')
LEVELS = ['off', 'low', 'medium', 'high', 'modern']
EXACT = ['foliage_density', 'shadow_distance', 'impostor_px', 'classic_impostor_px', 'lod_px', 'error_px', 'lod_static', 'lod_foliage', 'lod_impostor',
         'grass_end', 'lens_drops', 'decal_max', 'chunk_radius', 'consistency']
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


def run_game(name, lines, extra=()):
    os.makedirs(OUT, exist_ok=True)
    w, h = RES.split('x')
    script = os.path.join(OUT, name + '.cfg')
    with open(script, 'w') as f:
        f.write('set r_maxfps 60\nset r_width %s\nset r_height %s\nvid_restart\nwait 30\n' % (w, h))
        f.write('\n'.join(lines) + '\nwait 2\nquit\n')
    log_path = os.path.join(OUT, name + '.log')
    cmd = [EXE, '--test', '--nosound', '--log', log_path] + list(extra) + ['--script', script]
    if os.name != 'nt' and not os.environ.get('DISPLAY') and shutil.which('xvfb-run'):
        cmd = ['xvfb-run', '-a', '-s', '-screen 0 %sx%sx24' % (w, h)] + cmd
    subprocess.run(cmd, cwd=BIN, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=900)
    with open(log_path, encoding='utf-8', errors='replace') as f:
        return f.read()


def probe(text, tag):
    m = re.search(r'visibility_probe %s: (.*)' % re.escape(tag), text)
    m2 = re.search(r'visibility_probe %s\+ (.*)' % re.escape(tag), text)
    if not m or not m2:
        return None
    return dict(kv.split('=', 1) for kv in (m.group(1) + ' ' + m2.group(1)).split() if '=' in kv)


def levels():
    results = {}
    for i, lvl in enumerate(LEVELS):
        text = run_game('level_' + lvl, [
            'set r_fidelity %d' % i, 'wait 30',
            'time_set 12:00', 'weather_lock 1', 'weather_set fog 0.1',
            'foliage_test_scatter 300000', 'wait 60',
            'setpos 220 0.1 82 180 -4', 'wait 240',
            'visibility_probe ' + lvl,
        ])
        p = probe(text, lvl)
        check(p is not None, '%s: visibility_probe logged' % lvl)
        if p:
            check(p.get('level') == lvl, '%s: the level is %s' % (lvl, p.get('level')))
            results[lvl] = p
            log('      %s: %s' % (lvl, ' '.join('%s=%s' % kv for kv in p.items())))
    if 'modern' not in results:
        return
    ref = results['modern']
    for lvl, p in results.items():
        if lvl == 'modern':
            continue
        for k in EXACT:
            check(p.get(k) == ref.get(k), '%s vs modern: %s %s / %s' % (lvl, k, p.get(k), ref.get(k)))
        for k, tol in (('instances', 0.05), ('faded', 0.05), ('fog', 0.02)):
            a, b = float(p.get(k, 0)), float(ref.get(k, 0))
            ok = abs(a - b) <= tol * max(abs(b), 1e-6) or (k != 'fog' and abs(a - b) <= 50)
            check(ok, '%s vs modern: %s %g / %g (tolerance %d %%)' % (lvl, k, a, b, int(tol * 100)))
        check(p.get('counted') == '1' and float(p.get('instances', 0)) > 0, '%s: foliage instances counted (%s)' % (lvl, p.get('instances')))


def release_locks():
    text = run_game('release_locks', ['set r_lens_drops 0', 'set r_foliage_density 0.6', 'set r_ao 0', 'set cl_fov 110', 'visibility_probe locks', 'cvarlist cl_fov'],
                    extra=('--release-locks',))
    p = probe(text, 'locks')
    check(p is not None and p.get('lens_drops') == '1', 'release locks: r_lens_drops (locked) stays 1 (%s)' % (p.get('lens_drops') if p else 'no probe'))
    check('r_lens_drops is fixed for fair play' in text, 'release locks: r_lens_drops write rejected and logged')
    # A solo game is its own host: the server rules are this player's to set (and replicate when hosting).
    check(p is not None and p.get('foliage_density') == '0.6', 'release locks: the host sets the foliage density rule (%s)' % (p.get('foliage_density') if p else 'no probe'))
    check('r_ao is set by Engine Fidelity' in text, 'release locks: r_ao (tier) write rejected')
    check(re.search(r'cl_fov\s+= 110', text) is not None, 'release locks: cl_fov (editable) set')
    check('consistency: ' in text and 'bad entries' not in text, 'release locks: the consistency set applied without bad entries')


def main():
    if not os.path.exists(EXE):
        print('no game binary at %s (build first)' % EXE)
        return 1
    levels()
    release_locks()
    log('run_settings_tests: %d failure(s)' % failures)
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, 'report.txt'), 'w') as f:
        f.write('\n'.join(report) + '\n')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
