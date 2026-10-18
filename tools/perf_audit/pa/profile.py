"""Profile access (the JSON written by src/core/profiler.c, format dg-perf-profile v1)."""
import json
import math
import os


def valid(v):
    return [x for x in v if x is not None and x >= 0]


def mean(v):
    v = valid(v)
    return sum(v) / len(v) if v else 0.0


def median(v):
    v = sorted(valid(v))
    if not v:
        return 0.0
    n = len(v)
    return v[n // 2] if n % 2 else 0.5 * (v[n // 2 - 1] + v[n // 2])


def pct(v, p):
    v = sorted(valid(v))
    if not v:
        return 0.0
    k = min(len(v) - 1, max(0, int(math.ceil(p / 100.0 * len(v))) - 1))
    return v[k]


def mad(v):
    """Median absolute deviation, scaled to a standard deviation estimate."""
    m = median(v)
    return 1.4826 * median([abs(x - m) for x in valid(v)])


class Profile:
    def __init__(self, d, path=""):
        if d.get("format") != "dg-perf-profile":
            raise ValueError("%s: not a dg-perf-profile" % path)
        self.d = d
        self.path = path
        m = d.get("meta", {})
        self.scene = m.get("scene", "?")
        self.fidelity = m.get("fidelity", "?")
        self.label = m.get("label", "")
        self.frames = int(m.get("frames", len(d.get("frames", {}).get("cpu_ms", []))))
        self.key = "%s/%s" % (self.scene, self.fidelity)
        self._zones = {}
        for z in d.get("zones", []):
            self._zones[(z["name"], z.get("thread", "main"))] = z

    @staticmethod
    def load(path):
        with open(path, encoding="utf-8") as f:
            return Profile(json.load(f), path)

    # ---------------------------------------------------------------- CPU
    def cpu_ms(self):
        """Frame time without the frame limiter's sleep."""
        frame = self.d["frames"]["cpu_ms"]
        cap = self.zone("wait:fps_cap")
        if cap:
            return [max(0.0, f - c) for f, c in zip(frame, cap["incl_ms"])]
        return list(frame)

    def zone(self, name, thread="main"):
        return self._zones.get((name, thread))

    def zones(self, cat=None, thread=None):
        for (n, t), z in self._zones.items():
            if (cat is None or z.get("cat") == cat) and (thread is None or t == thread):
                yield z

    def zone_mean(self, name, thread="main", key="incl_ms"):
        z = self.zone(name, thread)
        return mean(z[key]) if z else 0.0

    def counter(self, name):
        return self.d.get("counters", {}).get(name, [])

    def counter_mean(self, name):
        return mean(self.counter(name))

    def series(self, name):
        return self.d.get("series", {}).get(name, [])

    def threads(self):
        return self.d.get("threads", [])

    def jobs(self):
        return self.d.get("jobs", [])

    def alloc_sites(self):
        return self.d.get("alloc_sites", [])

    # ---------------------------------------------------------------- GPU
    def gpu(self):
        return self.d.get("gpu") or {}

    def gpu_scopes(self):
        return self.gpu().get("scopes", [])

    def gpu_scope(self, name):
        for s in self.gpu_scopes():
            if s["name"] == name:
                return s
        return None

    def gpu_frame_ms(self):
        s = self.series("gpu_frame_ms")
        return [x for x in s if x is not None and x > 0]

    def scope_runs(self, s):
        """Frames the scope ran (ms read back)."""
        return [i for i, x in enumerate(s.get("ms", [])) if x is not None and x >= 0]

    def fg(self):
        return self.d.get("fg") or {}

    def net(self):
        return self.d.get("net") or {}

    def state(self):
        return self.d.get("state")


def load_dir(path, include_verify=False):
    """Every profile under path (recursive), skipping manifests, non-profiles and (unless asked) the A/B
    passes of perf_verify (fixes switched off: not the engine as shipped)."""
    out = []
    for dp, _, files in os.walk(path):
        for f in sorted(files):
            if not f.endswith(".json") or f in ("manifest.json",) or f.startswith("findings") or f.startswith("AUDIT"):
                continue
            p = os.path.join(dp, f)
            try:
                with open(p, encoding="utf-8") as fh:
                    d = json.load(fh)
            except (OSError, ValueError):
                continue
            if isinstance(d, dict) and d.get("format") == "dg-perf-profile":
                if not include_verify and str(d.get("meta", {}).get("label", "")).startswith("verify"):
                    continue
                out.append(Profile(d, p))
    return out
