"""Synthetic profiles for the rule tests: a minimal valid dg-perf-profile, then per-test edits."""
import copy

N = 20


def base(frames=N, scene="synthetic", fidelity="modern"):
    return {
        "format": "dg-perf-profile", "version": 1,
        "meta": {"scene": scene, "label": "test", "fidelity": fidelity, "frames": frames, "warmup": 1, "wall_ms": frames * 10.0, "config": "release"},
        "frames": {"cpu_ms": [10.0] * frames},
        "series": {"gpu_frame_ms": [8.0] * frames},
        "counters": {},
        "threads": [{"index": 0, "name": "main", "class": 0, "job_busy_ms": [0.0] * frames},
                    {"index": 1, "name": "dg_worker_0", "class": 1, "job_busy_ms": [5.0] * frames}],
        "zones": [], "jobs": [], "alloc_sites": [], "timeline": [],
        "gpu": {"device": "synthetic", "pipeline_stats": True, "render_w": 1920, "render_h": 1080, "output_w": 1920, "output_h": 1080, "scopes": []},
        "fg": {"render_w": 1920, "render_h": 1080, "dead_passes": [], "unused_res": [], "resources": [], "passes": [], "stale_mb": 0.0},
        "net": None, "state": None,
    }


def zone(d, name, ms, thread="main", cat="zone", calls=1, excl=None):
    n = d["meta"]["frames"]
    d["zones"].append({"name": name, "thread": thread, "cat": cat, "total_ms": ms * n, "max_ms": ms, "incl_ms": [ms] * n,
                       "excl_ms": [ms if excl is None else excl] * n, "calls": [calls] * n})
    return d


def scope(d, name, ms, runs=None, **counts):
    n = d["meta"]["frames"]
    runs = n if runs is None else runs
    s = {"name": name, "ms": [ms if i < runs else -1.0 for i in range(n)]}
    for k in ("vs_invocations", "clip_primitives", "fs_invocations", "cs_invocations"):
        s[k] = [float(counts.get(k.split("_")[0], 0)) if i < runs else -1.0 for i in range(n)]
    for k in ("draws", "draws_indirect", "dispatches", "pipeline_binds", "descriptor_binds", "push_constants", "barriers", "image_barriers", "renderings",
              "clears", "clear_px", "copies", "copy_bytes"):
        s[k] = [counts.get(k, 0) if i < runs else 0 for i in range(n)]
    d["gpu"]["scopes"].append(s)
    return d


def counter(d, name, v):
    d["counters"][name] = [v] * d["meta"]["frames"]
    return d


def clone(d):
    return copy.deepcopy(d)
