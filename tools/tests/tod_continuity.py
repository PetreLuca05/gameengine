"""Day / night continuity tests in the game (docs/DAYNIGHT.md "Continuity"):

  python tools/tests/tod_continuity.py [build_dir] [--only cont|long|edge|net|perf] [--port 27920] [--fps 30,60,144] [--keep]

  cont   `tod_test continuity` (game_tod_probe.c) at r_maxfps 30 / 60 / 144 in the dev room's shadow
         corner: a full 24 s day at time_scale 60, the default day (24 min) by day and by moonlight,
         and the speed / time commands (time_scale 1 -> 60 -> 1, pause, resume, a time_set blend).
         A run 100 days later. Every per-frame step of the clock, the sky sun, the shading / shadow key,
         the moon light, the analytic shadow tip of a pole and the cloud drift within +-5 % of the
         clock's rate x the frame's real time
         (no zero-delta frame, no spike), the azimuth monotonic, the elevation never reversing in a
         half day, a rate change stepping between the old and the new rate, a blend inside its
         ease, the far cascades' light lag under half a texel of a 4 m caster's shadow motion.
  long   the same over one whole 10-minute day (time_scale 2.4) at 60 fps.
  edge   the fin's shadow edge in linear EXR captures, one per frame (dev_fixed_fps 60), in cascade 0
         at the default day and at time_scale 60, and in cascades 2 / 3 at time_scale 60 (camera 45 /
         100 m up): the edge's frame-to-frame motion vs what the exact sun predicts (no jump: |measured
         - predicted| <= the predicted step + half a pixel or 1.25 texels of the cascades there (a
         rasterized edge advances by whole texels); no frozen frame (under a quarter of the predicted
         step while it should move 0.02 px or more: the captures are noise-free to ~0.01 px).
  net    a host and a client (two processes, localhost): the client's continuity (its own clock,
         slewed to the host's) at time_scale 60 and the default day while the host changes the
         speed; the clock correction never exceeds the slew rate, no snap.
  perf   (not in the default run) frame time / GPU frame / sky passes with the clock running,
         uncapped: the dev room's shadow corner and the vegetation map's pine forest, at the default
         day and at time_scale 60 (each a process of its own; compare two builds' reports).
Exit code 0 when everything passes. Results: <build>/tod_tests/report.txt
"""
import math
import os
import re
import subprocess
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import shadow_ref as SR  # noqa: E402  (read_exr, lum)

ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
args = [a for i, a in enumerate(sys.argv[1:], 1) if not a.startswith('--') and sys.argv[i - 1] not in ('--only', '--port', '--fps')]
BUILD = os.path.abspath(args[0] if args else os.path.join(ROOT, 'build'))
BIN = os.path.join(BUILD, 'bin')
EXE = os.path.join(BIN, 'deadground.exe' if os.name == 'nt' else 'deadground')
OUT = os.path.join(BUILD, 'tod_tests').replace('\\', '/')
ONLY = sys.argv[sys.argv.index('--only') + 1] if '--only' in sys.argv else None
PORT = int(sys.argv[sys.argv.index('--port') + 1]) if '--port' in sys.argv else 27920
FPS = [int(x) for x in sys.argv[sys.argv.index('--fps') + 1].split(',')] if '--fps' in sys.argv else [30, 60, 144]
KEEP = '--keep' in sys.argv   # keep the per-frame EXR captures (~8 MB each) after the analysis
RES = (1280, 720)
EDGE_RES = (960, 540)   # the EXR captures: 4 MB a frame

report = []
failures = 0


def log(msg):
    print(msg, flush=True)
    report.append(msg)


def check(ok, msg):
    global failures
    log(('ok    ' if ok else 'FAIL  ') + msg)
    if not ok:
        failures += 1


HEAD = ['r_vsync 0', 'r_width %d' % RES[0], 'r_height %d' % RES[1], 'vid_restart', 'wait 20', 'map devroom', 'wait 120', 'r_fidelity 4', 'r_era 0',
        'r_shadow_resolution 3', 'wait 30', 'shadow_preset clear_noon', 'wait 120']
CORNER = ['setpos -57 7 -4.2 0 -88', 'noclip', 'wait 60']


def write_cfg(name, lines):
    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, name + '.cfg')
    with open(path, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    return path


def game_cmd(name, cfg, extra=()):
    log_path = os.path.join(OUT, name + '.log')
    cmd = [EXE, '--test', '--nosound', '--log', log_path.replace('\\', '/'), '+set', 'net_port', str(PORT)] + list(extra) + ['--script', cfg.replace('\\', '/')]
    return cmd, log_path


def read(path):
    try:
        with open(path, encoding='utf-8', errors='replace') as f:
            return f.read()
    except OSError:
        return ''


def run_game(name, lines, timeout=1200):
    cfg = write_cfg(name, lines)
    cmd, log_path = game_cmd(name, cfg)
    env = dict(os.environ, DG_NO_FATAL_DIALOG='1')
    try:
        subprocess.run(cmd, cwd=BIN, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=timeout)
    except subprocess.TimeoutExpired:
        log('FAIL  %s: timed out' % name)
    return read(log_path)


RES_LINE = re.compile(r'tod_test continuity (\S+): (.*)$')


def continuity_results(text):
    """tag -> list of the result lines' bodies."""
    out = {}
    for line in text.splitlines():
        m = RES_LINE.search(line)
        if m:
            out.setdefault(m.group(1), []).append(m.group(2).strip())
    return out


def report_runs(text, tags):
    res = continuity_results(text)
    for tag in tags:
        lines = res.get(tag, [])
        verdict = [x for x in lines if x in ('PASS', 'FAIL')]
        for x in lines:
            if x not in ('PASS', 'FAIL') and not x.endswith(' s'):
                log('      %s: %s' % (tag, x))
        check(verdict == ['PASS'], '%s: continuity %s' % (tag, verdict[0] if verdict else 'no result'))


def wait_frames(secs, fps):
    return 'wait %d' % int((secs + 2.0) * fps + 30)


def scenario_lines(fps):
    tag = lambda t: '%s_%d' % (t, fps)  # noqa: E731
    L = list(HEAD) + ['r_maxfps %d' % fps] + CORNER
    # A whole day at time_scale 60 (24 s): sunset, the moon on the cascades, sunrise.
    L += ['time_set 15:30 instant', 'time_scale 60', 'wait 30', 'tod_test continuity 25 %s' % tag('ts60'), wait_frames(25, fps)]
    # The default day (24 min per day), afternoon sun.
    L += ['time_scale 1', 'time_set 16:45 instant', 'wait 30', 'tod_test continuity 10 %s' % tag('day'), wait_frames(10, fps)]
    # The default day at night: the moon owns the cascades.
    L += ['time_set 23:15 instant', 'wait 60', 'tod_test continuity 8 %s' % tag('night'), wait_frames(8, fps)]
    # 100 days later (float precision of the clock and the sky), the afternoon.
    L += ['time_days 100', 'time_set 16:45 instant', 'wait 30', 'tod_test continuity 6 %s' % tag('day100'), wait_frames(6, fps)]
    # Commands: time_scale 1 -> 60 -> 1, pause, resume, a time_set blend, all inside one run.
    L += ['time_set 13:00 instant', 'wait 30', 'tod_test continuity 12 %s' % tag('cmds'), 'wait %d' % fps, 'time_scale 60', 'wait %d' % (fps * 2), 'time_scale 1',
          'wait %d' % fps, 'time_pause 1', 'wait %d' % fps, 'time_pause 0', 'wait %d' % fps, 'time_scale 10', 'wait %d' % fps, 'time_set 15:00', 'wait %d' % (fps * 3),
          'time_scale 1', wait_frames(1, fps)]
    return L, [tag('ts60'), tag('day'), tag('night'), tag('day100'), tag('cmds')]


def cont():
    for fps in FPS:
        L, tags = scenario_lines(fps)
        t0 = time.time()
        text = run_game('cont_%d' % fps, L + ['quit'])
        log('cont %d fps: %.0f s' % (fps, time.time() - t0))
        report_runs(text, tags)


def long_day():
    L = list(HEAD) + ['r_maxfps 60'] + CORNER + ['time_set 06:00 instant', 'time_scale 2.4', 'wait 30', 'tod_test continuity 605 day10_60', wait_frames(605, 60), 'quit']
    text = run_game('long_60', L, timeout=1800)
    report_runs(text, ['day10_60'])


# ------------------------------------------------------------------ the fin's shadow edge per frame

FIN_Z, FIN_E, FIN_W = -2.5, -57.0, -59.0


def load_trace(path):
    with open(path) as f:
        head = f.readline().strip().split(',')
        rows = [line.strip().split(',') for line in f if line.strip()]
    cols = {h: np.array([float(r[i]) for r in rows]) for i, h in enumerate(head)}
    return cols


def ratio_xz(e_deg, a_deg):
    """L.x / L.z of the direction towards the light (azimuth from north, clockwise; -z north)."""
    e, a = np.radians(e_deg), np.radians(a_deg)
    return (np.cos(e) * np.sin(a)) / (-np.cos(e) * np.cos(a))


def cross(v, thr):
    for i in range(len(v) - 1):
        a, b = v[i], v[i + 1]
        if a < thr <= b:
            return i + (thr - a) / (b - a)
    return None


def fin_edges(img):
    """Per image row in the upper half: the shadow band's west / east 50 % crossings (sub-pixel)."""
    Y = SR.lum(img)
    h, w = Y.shape
    out = {}
    for r in range(int(h * 0.12), int(h * 0.48), 2):
        line = np.convolve(Y[r, :], np.ones(3) / 3, mode='same')
        c0, c1 = int(w * 0.22), int(w * 0.62)
        seg = line[c0:c1]
        dark, lit = np.percentile(seg, 3), np.percentile(seg, 80)
        if lit < dark * 1.5:
            continue
        v = (seg - dark) / (lit - dark)
        c = int(np.argmin(np.convolve(v, np.ones(15), mode='same')))
        wv = cross(v[c::-1], 0.5)
        ev = cross(v[c:], 0.5)
        if wv is None or ev is None:
            continue
        out[r] = (c0 + c - wv, c0 + c + ev)
    return out


# Cascade texels at the Modern tier (4096 tiles, 4 cascades to 150 m: docs/SHADOWS.md), when the
# trace has no tex<k> columns (a build before them).
MODERN_TEXEL = [0.0089, 0.0265, 0.0618, 0.132]


def edge_run(name, eye_y, ts, frames, cascades=(0,)):
    d = OUT + '/' + name
    os.makedirs(d, exist_ok=True)
    for f in os.listdir(d):
        if f.endswith('.exr'):
            os.remove(os.path.join(d, f))
    # FXAA (no TAA history, a fixed filter kernel): each capture is that frame's shadow alone.
    head = [x.replace('r_width %d' % RES[0], 'r_width %d' % EDGE_RES[0]).replace('r_height %d' % RES[1], 'r_height %d' % EDGE_RES[1]) for x in HEAD]
    L = head + ['r_maxfps 60', 'r_aa %s' % os.environ.get('TOD_EDGE_AA', '1')] + os.environ.get('TOD_EDGE_EXTRA', '').split(';') + ['setpos -57 %.1f -4.2 0 -88' % eye_y, 'noclip', 'wait 60', 'dev_fixed_fps 60', 'time_scale %g' % ts, 'time_set 12:00 instant',
                      'wait 40', 'tod_test trace %d %s/trace.csv' % (frames + 4, d), 'tod_test shots %d %s' % (frames, d), 'wait %d' % (frames + 10), 'dev_fixed_fps 0',
                      'quit']
    # The captures are measured while the game runs and deleted (unless --keep): a few on disk at a time.
    cfg = write_cfg(name, L)
    cmd, _ = game_cmd(name, cfg)
    proc = subprocess.Popen(cmd, cwd=BIN, env=dict(os.environ, DG_NO_FATAL_DIALOG='1'), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    edges = {}

    def consume(final):
        names = sorted(f for f in os.listdir(d) if f.startswith('f') and f.endswith('.exr') and int(f[1:7]) not in edges)
        for f in names if final else names[:-1]:   # the newest may still be written
            path = os.path.join(d, f)
            try:
                edges[int(f[1:7])] = fin_edges(SR.read_exr(path))
            except Exception:
                continue
            if not KEEP:
                os.remove(path)

    t0 = time.time()
    while proc.poll() is None and time.time() - t0 < 900:
        consume(False)
        time.sleep(0.2)
    if proc.poll() is None:
        proc.kill()
        log('FAIL  %s: timed out' % name)
    consume(True)
    tr = load_trace(d + '/trace.csv')
    idx = {int(f): i for i, f in enumerate(tr['frame'])}
    # A capture whose band was not found (a read while written, a hitch frame) is left out: the step
    # across it is predicted over its two frames.
    shots = sorted(edges.keys())
    med = np.median([len(edges[f]) for f in shots]) if shots else 0
    dropped = [f for f in shots if len(edges[f]) < 0.5 * med]
    shots = [f for f in shots if f not in dropped]
    if dropped:
        log('      %s: %d captures left out (no band found)' % (name, len(dropped)))
    if len(shots) < frames // 2:
        check(False, '%s: %d captures' % (name, len(shots)))
        return
    E = [edges[f] for f in shots]
    rows = sorted(set.intersection(*[set(e.keys()) for e in E]))
    if len(rows) < 6:
        check(False, '%s: fin edge found on %d rows' % (name, len(rows)))
        return
    width = np.median([E[0][r][1] - E[0][r][0] for r in rows])
    scale = 2.0 / width                                  # m / px: the fin is 2 m wide
    far = rows[:max(len(rows) // 3, 3)]                  # the rows farthest north of the fin move most
    z_center = -4.2 - (eye_y + 1.6) * math.tan(math.radians(2.0))
    h = EDGE_RES[1]
    dist = np.array([FIN_Z - (z_center + (r - h / 2.0) * scale) for r in far])
    kr = ratio_xz(tr['sun_e'], tr['sun_a'])             # the exact sun: where the edge should be
    # Both edges of the band (the fin's two vertical edges); the one that crosses no other surface
    # (the smaller median error against the prediction) is the measurement.
    best = None
    for side in (0, 1):
        meas, pred = [], []
        for a, b in zip(shots[:-1], shots[1:]):
            if a not in idx or b not in idx:
                continue
            ia, ib = idx[a], idx[b]
            m = np.median([(E[shots.index(b)][r][side] - E[shots.index(a)][r][side]) for r in far])
            p = np.median(-dist * (kr[ib] - kr[ia]) / scale)
            meas.append(m), pred.append(p)
        meas, pred = np.array(meas), np.array(pred)
        # The prediction's scale (the view's tilt and distances are approximate from 45 / 100 m up):
        # one factor fitted over the run; the frame-to-frame test is on the residual.
        k = float(np.sum(meas * pred) / max(np.sum(pred * pred), 1e-12))
        pred = pred * k
        err = np.abs(meas - pred)
        if best is None or np.median(err) < best[0]:
            best = (np.median(err), meas, pred, err, k)
    _, meas, pred, err, kfit = best
    texel = max(float(np.median(tr['tex%d' % k])) if 'tex%d' % k in tr else MODERN_TEXEL[k] for k in cascades)
    texel_px = texel / scale
    moving = np.abs(pred) >= 0.02
    frozen = int(np.sum(moving & (np.abs(meas) < 0.25 * np.abs(pred))))
    jumps = int(np.sum(err > max(0.5, 1.25 * texel_px) + np.abs(pred)))
    log('      %s: %d frames, %.2f mm/px, cascades %s texel %.2f px, predicted step %.3f px (median; scale fit %.2f), measured %.3f .. %.3f px, |measured - predicted| '
        'max %.3f px (median %.3f), frozen %d, jumps %d' % (name, len(meas), scale * 1000, '/'.join(str(k) for k in cascades), texel_px, np.median(np.abs(pred)), kfit, meas.min(),
                                                           meas.max(), err.max(), np.median(err), frozen, jumps))
    ok = jumps == 0 and frozen <= max(1, int(0.1 * np.sum(moving)))
    check(ok, '%s: shadow edge continuity (max jump %.3f px over the prediction)' % (name, err.max()))
    return meas, pred


def edge():
    edge_run('edge_c0_day', 7.0, 1.0, 36, (0, 1))
    edge_run('edge_c0_ts60', 7.0, 60.0, 24, (0, 1))
    edge_run('edge_c2_ts60', 45.0, 60.0, 24, (2, 3))
    edge_run('edge_c3_ts60', 100.0, 60.0, 24, (3,))


# ------------------------------------------------------------------ host + client

def net():
    # The host serves the dev room it starts in (no map command: a reload would drop the client).
    host_lines = ['set cl_name Host', 'r_maxfps 60', 'r_fidelity 4', 'r_shadow_resolution 3', 'wait 60', 'shadow_preset clear_noon', 'wait 120',
                  'time_set 15:30 instant', 'time_scale 60', 'wait 1500',   # the client joins and settles
                  'time_scale 1', 'wait 600', 'time_scale 60', 'wait 300', 'time_scale 1', 'wait 300', 'time_set 18:00', 'wait 300', 'time_scale 60', 'wait 600',
                  'wait 900', 'quit']
    client_lines = ['wait 400', 'r_maxfps 144', 'r_vsync 0', 'tod_test trace 4400 %s/net_client_trace.csv' % OUT, 'tod_test continuity 30 cl_mixed', 'wait 4500', 'quit']
    hcfg = write_cfg('net_host', host_lines)
    ccfg = write_cfg('net_client', client_lines)
    env = dict(os.environ, DG_NO_FATAL_DIALOG='1')
    hcmd, hlog = game_cmd('net_host', hcfg)
    ccmd, clog = game_cmd('net_client', ccfg, ['+connect', '127.0.0.1'])
    for f in (hlog, clog):   # the readiness test reads the host's log: no stale one
        if os.path.exists(f):
            os.remove(f)
    host = subprocess.Popen(hcmd, cwd=BIN, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    t0 = time.time()
    while time.time() - t0 < 120 and 'net: hosting' not in read(hlog):   # the host is up
        time.sleep(1)
    time.sleep(3)
    try:
        subprocess.run(ccmd, cwd=BIN, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=600)
    except subprocess.TimeoutExpired:
        log('FAIL  net: the client timed out')
    try:
        host.wait(timeout=300)
    except subprocess.TimeoutExpired:
        host.kill()
        log('FAIL  net: the host timed out')
    text = read(clog)
    check(re.search(r'cl_mixed: \d+ frames, .* client$', text, re.M) is not None, 'net: the measurement ran on a connected client')
    report_runs(text, ['cl_mixed'])


def perf():
    scenes = [('dev', ['map devroom', 'wait 120'] + CORNER[:2] + ['wait 60']),
              ('veg', ['map vegetation', 'wait 300', 'weather_lock 1', 'weather_set little_cloudy 0', 'setpos -135 16.41 -125 30 -2', 'wait 120'])]
    for sid, setup in scenes:
        for ts, label in ((1, 'day'), (60, 'ts60')):
            L = ['r_vsync 0', 'r_maxfps 0', 'r_width %d' % RES[0], 'r_height %d' % RES[1], 'vid_restart', 'wait 20'] + setup + [
                'r_fidelity 4', 'r_era 0', 'r_shadow_resolution 3', 'wait 60', 'time_scale %d' % ts, 'wait 300', 'time_set 15:00 instant', 'wait 2',
                'tod_test continuity 15 quit perf_%s_%s' % (sid, label)]
            text = run_game('perf_%s_%s' % (sid, label), L)
            for x in continuity_results(text).get('perf_%s_%s' % (sid, label), []):
                if 'fps' in x or 'GPU' in x:
                    log('      perf_%s_%s: %s' % (sid, label, x))


def main():
    if not os.path.exists(EXE):
        print('no game binary at %s' % EXE)
        return 2
    groups = [('cont', cont), ('edge', edge), ('net', net), ('long', long_day), ('perf', perf)]
    for name, fn in groups:
        if ONLY and ONLY != name:
            continue
        if not ONLY and name in ('long', 'perf'):
            continue
        log('== %s' % name)
        fn()
    log('tod_continuity: %d failures' % failures)
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, 'report.txt'), 'w') as f:
        f.write('\n'.join(report) + '\n')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
