#!/usr/bin/env python3
"""vfxgen - builds the VFX texture set (assets/vfx/tex) from recipes (assets/vfx/gen/*.json).

Sources per recipe:
  higgsfield image / video models (paid, cached)   default when the recipe has "model"
  procedural placeholders                          --placeholder, or recipes without "model"

Post-processing (process.py): background keying -> alpha, crop / fit to cells, flipbook packing,
motion vectors (optical flow), 6-way lightmaps, normal maps (decals / distortion), edge bleed,
premultiplied PNG (+ optional KTX2), and <name>.meta.json the game reads.

  python tools/vfxgen/vfxgen.py --placeholder            all textures, procedural, no API
  python tools/vfxgen/vfxgen.py --dry-run                estimate the API cost of every recipe
  python tools/vfxgen/vfxgen.py smoke_puff flash_core    generate (cached) via Higgsfield
  python tools/vfxgen/vfxgen.py --regen flash_core       ignore the cache, pay again
  options: --max-usd N (cost cap, default 20), --ktx2, --list, --only-placeholder-missing

The API key comes from the project .env (OPEN_HIGGSFIELD_READ_WRITE_TOKEN) and is used by this tool
only; it is never printed. Every API call is logged to tools/vfxgen/logs/api_log.jsonl.
"""
import argparse
import glob
import io
import json
import os
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import placeholder  # noqa: E402
import process as P  # noqa: E402

ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
GEN_DIR = os.path.join(ROOT, "assets", "vfx", "gen")
TEX_DIR = os.path.join(ROOT, "assets", "vfx", "tex")
CACHE_DIR = os.path.join(HERE, "cache")
TOOL_VERSION = 3
COMMIT_LIMIT_MB = 50.0


def load_recipes():
    out = {}
    for path in sorted(glob.glob(os.path.join(GEN_DIR, "*.json"))):
        with open(path, encoding="utf-8") as f:
            r = json.load(f)
        r.setdefault("name", os.path.splitext(os.path.basename(path))[0])
        r["_path"] = path
        out[r["name"]] = r
    return out


GEN_KEYS = ("model", "prompt", "negative_prompt", "duration", "aspect_ratio", "resolution", "cfg_scale", "images_per_request")


def recipe_hash(r):
    """Whole recipe (meta / provenance)."""
    from hf_client import stable_key
    keep = {k: v for k, v in r.items() if not k.startswith("_")}
    return stable_key(keep, TOOL_VERSION)


def generation_hash(r):
    """Only what the API sees: post-processing tweaks never trigger (paid) regeneration."""
    from hf_client import stable_key
    return stable_key({k: r.get(k) for k in GEN_KEYS})


def cache_dir(r):
    """tools/vfxgen/cache/<name>_<gen hash>; an older dir of the same recipe without a gen_key file
    (written by earlier tool versions) is adopted."""
    g = generation_hash(r)
    want = os.path.join(CACHE_DIR, f"{r['name']}_{g}")
    if os.path.isdir(want):
        return want, g
    for d in sorted(glob.glob(os.path.join(CACHE_DIR, r["name"] + "_*"))):
        if not os.path.exists(os.path.join(d, "gen_key")):
            sub = os.path.join(d, "submit_0.json")
            if os.path.exists(sub):
                with open(sub, encoding="utf-8") as f:
                    p = json.load(f).get("params", {})
                if p.get("prompt", "").startswith(r.get("prompt", "")[:80]):
                    os.replace(d, want)
                    break
    os.makedirs(want, exist_ok=True)
    with open(os.path.join(want, "gen_key"), "w") as f:
        f.write(g)
    return want, g


# ------------------------------------------------------------------ sources

def frames_placeholder(r):
    ph = dict(r.get("placeholder", {}))
    kind = ph.pop("type")
    gen = placeholder.GENERATORS[kind]
    cell = r.get("cell", 128)
    seed = ph.pop("seed", 1)
    n_frames = r.get("frames", 1)
    variants = r.get("variants", 1)
    if kind in ("puff", "mist", "splash"):
        res = gen(cell, n_frames, seed, **ph)
    else:
        res = gen(cell, max(variants, 1), seed, **ph)
    heights = None
    if isinstance(res, tuple):
        res, heights = res
    return res, heights


def _api_params(r):
    m = r["model"]
    if "video" in m:
        p = {"prompt": r["prompt"], "duration": r.get("duration", 5)}
        if r.get("negative_prompt"):
            p["negative_prompt"] = r["negative_prompt"]
        if "kling" in m:
            p["cfg_scale"] = r.get("cfg_scale", 0.5)
        if r.get("aspect_ratio"):
            p["aspect_ratio"] = r["aspect_ratio"]
        return [p]
    # Image model: the service returns one image per request in practice, so variants are separate
    # requests (images_per_request > 1 asks for more per call where a model supports it).
    total = max(1, r.get("variants", 1)) if r.get("frames", 1) <= 1 else 1
    total = min(total, r.get("api_variants", total))
    per = max(1, min(4, r.get("images_per_request", 1)))
    reqs = []
    left = total
    while left > 0:
        n = min(per, left)
        reqs.append({"prompt": r["prompt"] + (f" (variation {len(reqs) + 1})" if total > 1 else ""), "num_images": n,
                     "resolution": r.get("resolution", "1080p"), "aspect_ratio": r.get("aspect_ratio", "1:1")})
        left -= n
    return reqs


def _decode_video(path):
    import av
    frames = []
    with av.open(path) as c:
        for fr in c.decode(video=0):
            frames.append(fr.to_ndarray(format="rgb24").astype(np.float32) / 255.0)
    return frames


def _corners(rgb, n=12):
    return np.concatenate([rgb[:n, :n].reshape(-1, 3), rgb[:n, -n:].reshape(-1, 3), rgb[-n:, :n].reshape(-1, 3), rgb[-n:, -n:].reshape(-1, 3)])


def _key(r, rgb, bg_ref=None):
    """Background removal. The background colour is measured from the image corners (generated
    'black' / 'green' screens are never pure), unless a reference (whole video) is given."""
    bg = r.get("background", "black")
    a = r.get("alpha", {})
    corners = _corners(rgb) if bg_ref is None else bg_ref
    if bg == "black":
        col = a.get("albedo")
        bl = float(np.percentile(P.luma(corners), 95))
        black = max(a.get("black", 0.04), bl + a.get("black_margin", 0.035))
        white = max(a.get("white", 0.6), black + 0.15)
        return P.alpha_from_luma(rgb, black, white, a.get("gamma", 1.0), color=col)
    keys = {"white": (1.0, 1.0, 1.0), "green": (0.0, 1.0, 0.0), "magenta": (1.0, 0.0, 1.0)}
    key = np.array(keys.get(bg, keys["green"]), np.float32)
    measured = np.median(corners, axis=0)
    spread = float(np.percentile(np.linalg.norm(corners - measured, axis=1), 90))
    if spread < 0.12:
        key = measured   # uniform screen: key exactly its colour
    tol = a.get("tol", 0.1) + spread * 0.5
    return P.chroma_key(rgb, tuple(key), tol, a.get("soft", 0.12))


def frames_api(r, client, regen, budget):
    from hf_client import new_idempotency_key, usd_of
    cdir, key = cache_dir(r)
    if regen:
        for f in glob.glob(os.path.join(cdir, "raw_*")):
            os.remove(f)
    reqs = _api_params(r)
    todo = [qi for qi in range(len(reqs)) if not glob.glob(os.path.join(cdir, f"raw_{qi:02d}_*"))]
    if not todo:
        print(f"  cache hit ({len(reqs)} request(s))")
    else:
        for qi in todo:
            params = reqs[qi]
            est = client.estimate(r["model"], params, recipe=r["name"])
            usd = usd_of(est)
            if usd is None:
                raise RuntimeError(f"{r['name']}: no USD in estimate {est}")
            if budget["spent"] + usd > budget["cap"]:
                raise RuntimeError(f"{r['name']}: estimate ${usd:.2f} would exceed the cap (${budget['spent']:.2f} of ${budget['cap']:.2f} used)")
            idem = new_idempotency_key(f"{key}/{qi}/{int(regen and time.time())}")
            sub = client.submit(r["model"], params, idem, recipe=r["name"])
            with open(os.path.join(cdir, f"submit_{qi}.json"), "w", encoding="utf-8") as f:
                json.dump({"params": params, "response": sub, "estimate": est}, f, indent=1)
            st = client.wait(sub, recipe=r["name"])
            with open(os.path.join(cdir, f"status_{qi}.json"), "w", encoding="utf-8") as f:
                json.dump(st, f, indent=1)
            status = str(st.get("status")).lower()
            if status != "completed":
                raise RuntimeError(f"{r['name']}: request ended '{status}' (not billed for failed / nsfw)")
            budget["spent"] += usd
            urls = []
            if st.get("video") and st["video"].get("url"):
                urls.append((st["video"]["url"], ".mp4"))
            for im in st.get("images") or []:
                if im.get("url"):
                    ext = os.path.splitext(im["url"].split("?")[0])[1] or ".png"
                    urls.append((im["url"], ext))
            for ui, (u, ext) in enumerate(urls):
                dst = os.path.join(cdir, f"raw_{qi:02d}_{ui:02d}{ext}")
                client.download(u, dst, recipe=r["name"])
        print(f"  generated {len(todo)} request(s), spent so far ${budget['spent']:.2f}")
    raws = sorted(glob.glob(os.path.join(cdir, "raw_*")))
    cell = r.get("cell", 128)
    frames = []
    for raw in sorted(raws):
        if raw.endswith(".mp4") or raw.endswith(".webm") or raw.endswith(".mov"):
            src = _decode_video(raw)
            # One background reference for the whole clip (first frame corners: the puff starts small).
            ref = np.concatenate([_corners(src[0]), _corners(src[-1])])
            vid = [_key(r, f, ref) for f in src]
            t0, t1 = r.get("trim", [0.0, 1.0])
            n = len(vid)
            vid = vid[int(t0 * n):max(int(t1 * n), int(t0 * n) + 1)]
            bbox = P.union_bbox(vid)
            fitted = [P.fit_cell(f, cell, pad=r.get("pad", 0.04), bbox=bbox) for f in vid]
            frames.extend(P.resample_frames(fitted, r.get("frames", 16)))
        else:
            img = P.load_rgba(raw)[..., :3]
            keyed = _key(r, img)
            cut = r.get("crop", None)   # [x0, y0, x1, y1] fractions: drop props the model added (lighter, barrel)
            if cut:
                h, w = keyed.shape[:2]
                m = np.zeros((h, w, 1), np.float32)
                m[int(cut[1] * h):int(cut[3] * h), int(cut[0] * w):int(cut[2] * w)] = 1
                keyed = keyed * m
            if r.get("fill_holes"):
                keyed = P.fill_enclosed(keyed)
            frames.append(P.fit_cell(keyed, cell, pad=r.get("pad", 0.05)))
    if not frames:
        raise RuntimeError(f"{r['name']}: no frames decoded")
    # Keyed sources are premultiplied: convert to straight alpha for the shared pipeline.
    return [P.unpremultiply(f) for f in frames], None


# ------------------------------------------------------------------ pipeline

def build(r, frames, heights, placeholder_src, ktx2):
    name = r["name"]
    cell = r.get("cell", 128)
    n = len(frames)
    if r.get("variants", 1) > 1 and n < r["variants"]:
        # Fewer generated images than variants: fill the rest with mirrored / rotated copies
        # (decals and debris read as distinct; flashes should pay for real variants instead).
        if r.get("augment", True):
            ops = [lambda f: f[:, ::-1], lambda f: f[::-1, :], lambda f: np.rot90(f, 1), lambda f: np.rot90(f, 3), lambda f: f[::-1, ::-1]]
            src = list(frames)
            k = 0
            while len(frames) < r["variants"]:
                frames.append(np.ascontiguousarray(ops[k % len(ops)](src[(k // len(ops)) % len(src)])))
                k += 1
            n = len(frames)
            heights = None if heights is None else heights
        else:
            r = dict(r, variants=n)
    cols, rows = r.get("grid", [1, 1])
    count = cols * rows
    if n < count:
        frames = frames + [np.zeros_like(frames[0])] * (count - n)
    frames = [f if f.shape[0] == cell else P.resize(f, cell, cell) for f in frames[:count]]
    if r.get("raw"):
        pre = frames   # data textures (distortion normals): stored as-is
    else:
        if r.get("edge_fade", True):
            frames = [P.unpremultiply(P.edge_fade(P.premultiply(f), r.get("edge_margin", 0.06))) for f in frames]
        pre = [P.bleed_edges(P.premultiply(f)) for f in frames]
    os.makedirs(TEX_DIR, exist_ok=True)
    atlas = P.pack_grid(pre, cols, rows)
    out_files = []
    path = os.path.join(TEX_DIR, name + ".png")
    P.write_png(path, atlas)
    out_files.append(path)
    if ktx2:
        P.write_ktx2(os.path.join(TEX_DIR, name + ".ktx2"), atlas)
    mv = bool(r.get("motion_vectors")) and n > 1
    if mv:
        flows = P.flow_flipbook(frames[:n], cell, loop=r.get("loop", False))
        flows += [P.encode_flow(np.zeros((cell, cell), np.float32), np.zeros((cell, cell), np.float32), cell)] * (count - len(flows))
        p = os.path.join(TEX_DIR, name + "_mv.png")
        P.write_png(p, P.pack_grid(flows, cols, rows))
        out_files.append(p)
    lighting = r.get("lighting", "none")
    if lighting == "sixway":
        lm0s, lm1s = [], []
        for f in frames:
            a, b = P.sixway_from_density(f[..., 3])
            lm0s.append(a)
            lm1s.append(b)
        for suffix, ims in (("_lm0", lm0s), ("_lm1", lm1s)):
            p = os.path.join(TEX_DIR, name + suffix + ".png")
            P.write_png(p, P.pack_grid(ims, cols, rows))
            out_files.append(p)
    if r.get("normal_map"):
        hs = heights if heights is not None else [P.blur(f[..., 3], 1.0) for f in frames]
        hs = hs + [np.zeros((cell, cell), np.float32)] * (count - len(hs))
        nm = [np.concatenate([P.normal_from_height(h, r.get("normal_strength", 6.0)), f[..., 3:4]], axis=2) for h, f in zip(hs, frames)]
        p = os.path.join(TEX_DIR, name + "_n.png")
        P.write_png(p, P.pack_grid(nm, cols, rows))
        out_files.append(p)
        write_meta(name + "_n", r, cols, rows, n, placeholder_src, normal=True)
    write_meta(name, r, cols, rows, n, placeholder_src, mv=mv, lighting=lighting)
    return out_files


def write_meta(name, r, cols, rows, n, placeholder_src, mv=False, lighting="none", normal=False):
    meta = {
        "file": name + ".png",
        "width": cols * r.get("cell", 128),
        "height": rows * r.get("cell", 128),
        "grid": [cols, rows],
        "frames": n if not r.get("variants", 1) > 1 else cols * rows,
        "variants": r.get("variants", 1),
        "fps": r.get("fps", 30),
        "loop": r.get("loop", False),
        "motion_vectors": mv,
        "lighting": "none" if normal else lighting,
        "premultiplied": not normal and not r.get("raw"),
        "placeholder": placeholder_src,
        "kind": "normal" if normal else r.get("kind", "sprite"),
        "source": "procedural" if placeholder_src else r.get("model", "procedural"),
        "recipe": os.path.relpath(r["_path"], ROOT).replace("\\", "/"),
        "recipe_hash": recipe_hash(r),
        "generated": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "tool_version": TOOL_VERSION,
    }
    if r.get("variants", 1) > 1:
        meta["frames"] = min(r["variants"], cols * rows)
    with open(os.path.join(TEX_DIR, name + ".meta.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(meta, f, indent=2)
        f.write("\n")


def tex_size_mb():
    total = 0
    for p in glob.glob(os.path.join(TEX_DIR, "*")):
        total += os.path.getsize(p)
    return total / 1048576.0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("names", nargs="*", help="recipes to build (default: all)")
    ap.add_argument("--placeholder", action="store_true", help="procedural textures only (no API)")
    ap.add_argument("--dry-run", action="store_true", help="estimate API cost, generate nothing")
    ap.add_argument("--regen", action="store_true", help="ignore the download cache")
    ap.add_argument("--max-usd", type=float, default=20.0, help="spending cap for this run")
    ap.add_argument("--ktx2", action="store_true", help="also write KTX2 (RGBA8 + mips)")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--base-url", default=None)
    args = ap.parse_args()

    recipes = load_recipes()
    names = args.names or list(recipes)
    unknown = [n for n in names if n not in recipes]
    if unknown:
        sys.exit(f"unknown recipe(s): {', '.join(unknown)}")
    if args.list:
        for n in names:
            r = recipes[n]
            print(f"{n:28s} {r.get('model', 'procedural'):48s} grid {r.get('grid', [1, 1])} frames {r.get('frames', 1)} variants {r.get('variants', 1)}")
        return

    client = None
    need_api = not args.placeholder and any("model" in recipes[n] for n in names)
    if need_api:
        from hf_client import Client
        client = Client(base_url=args.base_url)

    if args.dry_run:
        from hf_client import usd_of
        total = 0.0
        for n in names:
            r = recipes[n]
            if "model" not in r:
                print(f"{n:28s} procedural (free)")
                continue
            sub = 0.0
            for p in _api_params(r):
                est = client.estimate(r["model"], p, recipe=n)
                u = usd_of(est)
                sub += u or 0.0
                credits = est.get("credits") if isinstance(est, dict) else None
                print(f"{n:28s} {r['model']:48s} ${u if u is not None else float('nan'):.3f}  credits {credits}")
            total += sub
        print(f"TOTAL estimate: ${total:.2f} (cap {args.max_usd:.2f})  base {client.base if client else '-'}")
        return

    budget = {"spent": 0.0, "cap": args.max_usd}
    failures = []
    for n in names:
        r = recipes[n]
        use_api = client is not None and "model" in r
        print(f"[{n}] {'higgsfield ' + r['model'] if use_api else 'placeholder'}")
        try:
            if use_api:
                frames, heights = frames_api(r, client, args.regen, budget)
                if r.get("placeholder", {}).get("type") == "decal_hole" and r.get("normal_map"):
                    heights = None   # derive from alpha
            else:
                frames, heights = frames_placeholder(r)
            files = build(r, frames, heights, not use_api, args.ktx2)
            print("  -> " + ", ".join(os.path.basename(f) for f in files))
        except Exception as e:  # keep going: report at the end
            failures.append((n, str(e)))
            print(f"  FAILED: {e}")
            if use_api and "placeholder" in r and not glob.glob(os.path.join(TEX_DIR, n + ".png")):
                frames, heights = frames_placeholder(r)
                build(r, frames, heights, True, args.ktx2)
                print("  (placeholder written instead)")
    mb = tex_size_mb()
    print(f"assets/vfx/tex: {mb:.1f} MB" + ("  (over the commit limit - ask before committing)" if mb > COMMIT_LIMIT_MB else ""))
    if budget["spent"]:
        print(f"spent ${budget['spent']:.2f} of the ${budget['cap']:.2f} cap")
    if failures:
        print("failures:")
        for n, e in failures:
            print(f"  {n}: {e}")
        sys.exit(1)


if __name__ == "__main__":
    main()
