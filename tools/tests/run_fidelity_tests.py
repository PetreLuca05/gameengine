"""Engine Fidelity test suite (docs/ENGINE_FIDELITY.md), run after a build:

  python tools/tests/run_fidelity_tests.py [build_dir] [--frames N] [--res WxH] [--budgets] [--skip-validation]

  1. unit      test_fidelity (tables, bounds, apply / clamp, era matrix, frame graph check).
  2. levels    fidelity_test in the game with the Vulkan validation layers: every level applied
               fresh builds a valid frame graph (no dead pass, no declared-but-unused resource, no
               undefined read, no pooled image left unscheduled after the trim), Off schedules zero
               post-processing passes, every cvar equals the level's table; then 50 level switches
               and back to Modern: the GPU memory (all VMA allocations) returns to where it was.
               0 validation messages.
  3. images    Off and Modern screenshots of the same view: both plausible frames (not black, not
               uniform), Off rendered at its lower resolution without the post stack.
  4. bench     bench_fidelity all (headless-capable): every level runs the fixed flythrough and logs
               per-pass GPU ms; Off logs 0 post passes. --budgets also checks each level's GPU frame
               time against its budget (meaningful at 1920x1080 on the reference mid-range GPU).

Headless on Linux: runs under xvfb-run when there is no DISPLAY (Mesa llvmpipe works).
Exit code 0 when everything passes. Results: <build>/fidelity_tests/report.txt
"""
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
args = [a for a in sys.argv[1:] if not a.startswith('--')]
opts = sys.argv[1:]
BUILD = os.path.abspath(args[0] if args and not args[0].isdigit() else os.path.join(ROOT, 'build'))
BIN = os.path.join(BUILD, 'bin')
OUT = os.path.join(BUILD, 'fidelity_tests')
FRAMES = int(opts[opts.index('--frames') + 1]) if '--frames' in opts else 120
RES = opts[opts.index('--res') + 1] if '--res' in opts else '1920x1080'
BUDGETS = '--budgets' in opts
VALIDATION = '--skip-validation' not in opts
VVL = os.environ.get('VK_ADD_LAYER_PATH', os.path.join(ROOT, '.toolchain', 'vvl', 'install', 'bin'))
EXE = os.path.join(BIN, 'deadground.exe' if os.name == 'nt' else 'deadground')
LEVELS = ['off', 'low', 'medium', 'high', 'modern']
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
    w, h = RES.split('x')
    script = os.path.join(OUT, name + '.cfg')
    with open(script, 'w') as f:
        f.write('r_maxfps 0\nr_width %s\nr_height %s\nvid_restart\nwait 20\n' % (w, h))
        f.write('\n'.join(lines) + '\n')
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
    exe = os.path.join(BIN, 'test_fidelity.exe' if os.name == 'nt' else 'test_fidelity')
    p = subprocess.run([exe], cwd=BIN, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = p.stdout.decode('utf-8', 'replace').strip().splitlines()
    check(p.returncode == 0, 'unit: ' + (out[-1] if out else 'no output'))


def levels():
    text = run_game('levels', ['fidelity_test quit'], validation=VALIDATION)
    for lvl in LEVELS:
        m = re.search(r'fidelity_test %s: table (\w+), graph (\w+)' % lvl, text)
        check(bool(m) and m.group(1) == 'ok' and m.group(2) == 'ok', 'levels: %s table / graph %s' % (lvl, m.group(0) if m else 'missing'))
        g = re.search(r'fidelity_check %s: (\d+) passes \((\d+) post' % lvl, text)
        if g:
            log('      %s: %s passes, %s post' % (lvl, g.group(1), g.group(2)))
        if lvl == 'off':
            check(bool(g) and g.group(2) == '0', 'levels: off schedules zero post passes')
    m = re.search(r'fidelity_test: 50 level switches: GPU memory ([\d.]+) MB -> ([\d.]+) MB \(([-+\d.]+) MB\) -> (\w+)', text)
    check(bool(m) and m.group(4) == 'ok', 'levels: 50 switches, GPU memory %s' % ('%s -> %s MB (%s MB)' % (m.group(1), m.group(2), m.group(3)) if m else 'missing'))
    check('fidelity_test: PASS' in text, 'levels: fidelity_test PASS')
    if VALIDATION:
        n = len(re.findall(r'VUID-', text))
        check(n == 0, 'levels: %d validation messages' % n)


def plausible(path):
    try:
        from PIL import Image
        import numpy as np
    except ImportError:
        return os.path.exists(path), 'exists (PIL / numpy missing: not analysed)'
    if not os.path.exists(path):
        return False, 'missing'
    a = np.asarray(Image.open(path).convert('L'), dtype=np.float32) / 255.0
    return a.mean() > 0.05 and a.std() > 0.03, 'mean %.2f std %.2f' % (a.mean(), a.std())


def images():
    off = os.path.join(OUT, 'off.png').replace('\\', '/')
    modern = os.path.join(OUT, 'modern.png').replace('\\', '/')
    run_game('images', ['screenshot ' + modern, 'wait 3', 'r_fidelity 0', 'wait 12', 'screenshot ' + off, 'wait 3', 'quit'])
    for name, path in (('modern', modern), ('off', off)):
        ok, what = plausible(path)
        check(ok, 'images: %s frame plausible (%s)' % (name, what))


def bench():
    text = run_game('bench', ['bench_fidelity all %d quit' % FRAMES])
    ms = {}
    for lvl in LEVELS:
        m = re.search(r'bench_fidelity %s: (\d+) frames.*?GPU frame ([\d.]+) ms \(budget ([\d.]+) ms.*?\((?:post [\d.]+ ms, )?(\d+) post passes\)' % lvl, text)
        check(bool(m), 'bench: %s ran' % lvl)
        if not m:
            continue
        ms[lvl] = float(m.group(2))
        log('      %-7s GPU frame %8.3f ms, budget %5.1f ms, %s post passes' % (lvl, ms[lvl], float(m.group(3)), m.group(4)))
        if lvl == 'off':
            check(m.group(4) == '0', 'bench: off logs 0 post passes')
        if BUDGETS:
            check(ms[lvl] <= float(m.group(3)), 'bench: %s within its budget' % lvl)
    if 'off' in ms and 'modern' in ms:
        check(ms['off'] <= ms['modern'], 'bench: Off (%.2f ms) not slower than Modern (%.2f ms)' % (ms['off'], ms['modern']))


def main():
    os.makedirs(OUT, exist_ok=True)
    log('Engine Fidelity tests: %s, %s, %d bench frames%s' % (EXE, RES, FRAMES, ', validation' if VALIDATION else ''))
    unit()
    levels()
    images()
    bench()
    log('%s (%d failure(s))' % ('PASS' if not failures else 'FAIL', failures))
    with open(os.path.join(OUT, 'report.txt'), 'w') as f:
        f.write('\n'.join(report) + '\n')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
