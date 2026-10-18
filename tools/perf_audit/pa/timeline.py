"""Per-profile HTML: CPU timeline (thread lanes, nested zones) of the median and the worst frame, the GPU
pass waterfall (mean ms per pass in execution order) and table views. Self-contained, no scripts from
the network; light and dark themes."""
import html
import json

from .profile import mean, median
from .rules_runtime import scope_stats
from .verify import cpu_work

CATS = ["zone", "wait", "job", "parfor"]

STYLE = """
:root{color-scheme:light;--surface:#fcfcfb;--panel:#f3f2ef;--text:#0b0b0b;--text2:#52514e;--muted:#8a8984;--grid:#e4e3df;
--s1:#2a78d6;--s2:#eb6834;--s3:#1baf7a;--s4:#eda100;}
@media (prefers-color-scheme:dark){:root:not([data-theme="light"]){color-scheme:dark;--surface:#1a1a19;--panel:#232322;--text:#f0efec;
--text2:#c3c2b7;--muted:#8f8e86;--grid:#33332f;--s1:#3987e5;--s2:#d95926;--s3:#199e70;--s4:#c98500;}}
:root[data-theme="dark"]{color-scheme:dark;--surface:#1a1a19;--panel:#232322;--text:#f0efec;--text2:#c3c2b7;--muted:#8f8e86;--grid:#33332f;
--s1:#3987e5;--s2:#d95926;--s3:#199e70;--s4:#c98500;}
body{margin:0;background:var(--surface);color:var(--text);font:14px/1.45 system-ui,-apple-system,Segoe UI,sans-serif}
main{max-width:1180px;margin:0 auto;padding:20px 16px 48px}
h1{font-size:20px;margin:0 0 4px}h2{font-size:16px;margin:28px 0 8px}p.sub{color:var(--text2);margin:0 0 12px}
.tiles{display:flex;gap:12px;flex-wrap:wrap}.tile{background:var(--panel);border-radius:8px;padding:10px 14px;min-width:120px}
.tile b{display:block;font-size:20px}.tile span{color:var(--text2);font-size:12px}
.legend{display:flex;gap:14px;flex-wrap:wrap;color:var(--text2);font-size:12px;margin:6px 0}
.legend i{display:inline-block;width:10px;height:10px;border-radius:2px;margin-right:5px;vertical-align:-1px}
.scroll{overflow-x:auto;border:1px solid var(--grid);border-radius:8px;background:var(--surface)}
svg text{font:11px system-ui,sans-serif;fill:var(--text2)}svg .lbl{fill:var(--text)}
svg rect.m{stroke:var(--surface);stroke-width:1}svg rect.m:hover{stroke:var(--text);stroke-width:1.5}
#tip{position:fixed;pointer-events:none;background:var(--panel);color:var(--text);border:1px solid var(--grid);border-radius:6px;
padding:6px 9px;font-size:12px;display:none;max-width:420px;z-index:9;box-shadow:0 2px 8px rgba(0,0,0,.18)}
table{border-collapse:collapse;width:100%;font-size:12px}th,td{padding:4px 8px;border-bottom:1px solid var(--grid);text-align:right}
th:first-child,td:first-child{text-align:left}th{color:var(--text2);font-weight:600}
details{margin:8px 0}summary{cursor:pointer;color:var(--text2)}
.tabs button{background:var(--panel);color:var(--text);border:1px solid var(--grid);border-radius:6px;padding:4px 10px;margin-right:6px;cursor:pointer}
.tabs button[aria-pressed="true"]{border-color:var(--s1)}
"""

SCRIPT = """
const tip=document.getElementById('tip');
document.querySelectorAll('[data-t]').forEach(e=>{
 e.addEventListener('mousemove',ev=>{tip.style.display='block';tip.textContent=e.getAttribute('data-t');
  const x=Math.min(ev.clientX+14,window.innerWidth-tip.offsetWidth-8);tip.style.left=x+'px';tip.style.top=(ev.clientY+14)+'px';});
 e.addEventListener('mouseleave',()=>tip.style.display='none');});
document.querySelectorAll('.tabs button').forEach(b=>b.addEventListener('click',()=>{
 document.querySelectorAll('.tabs button').forEach(x=>x.setAttribute('aria-pressed','false'));b.setAttribute('aria-pressed','true');
 document.querySelectorAll('.tl').forEach(t=>t.style.display=t.id===b.dataset.for?'block':'none');}));
"""


def cpu_svg(p, tl, width=1100):
    """Thread lanes; each zone a bar at its depth inside the lane."""
    names = [t["name"] for t in p.threads()]
    ev = tl["events"]
    if not ev:
        return "<p>No events.</p>"
    t0 = min(e[2] for e in ev)
    t1 = max(e[2] + e[3] for e in ev)
    span = max(1.0, t1 - t0)
    lanes = sorted({e[0] for e in ev})
    depth = {l: max(e[4] for e in ev if e[0] == l) + 1 for l in lanes}
    row, pad, left = 14, 10, 110
    y = {}
    yy = 24
    for l in lanes:
        y[l] = yy
        yy += depth[l] * row + pad
    h = yy + 4
    sx = (width - left - 44) / span
    out = ['<svg width="%d" height="%d" role="img" aria-label="CPU timeline">' % (width, h)]
    for k in range(0, 11):
        x = left + k * (width - left - 44) / 10.0
        out.append('<line x1="%.1f" y1="16" x2="%.1f" y2="%d" stroke="var(--grid)"/>' % (x, x, h))
        out.append('<text x="%.1f" y="12" text-anchor="middle">%.1f ms</text>' % (x, (t0 + span * k / 10.0) / 1000.0))
    for l in lanes:
        out.append('<text class="lbl" x="4" y="%d">%s</text>' % (y[l] + 11, html.escape(names[l] if l < len(names) else str(l))))
    for th, name, start, dur, d in ev:
        cat = name.split(":")[0] if name.split(":")[0] in ("wait", "job", "parfor") else "zone"
        col = "var(--s%d)" % (CATS.index(cat) + 1)
        x = left + (start - t0) * sx
        w = max(1.0, dur * sx)
        tipt = "%s | %s | start %.3f ms | %.3f ms | depth %d" % (name, names[th] if th < len(names) else th, start / 1000.0, dur / 1000.0, d)
        out.append('<rect class="m" x="%.1f" y="%d" width="%.1f" height="%d" rx="2" fill="%s" data-t="%s"/>' % (
            x, y[th] + d * row, w, row - 2, col, html.escape(tipt)))
        if w > 60:
            out.append('<text x="%.1f" y="%d" style="fill:#fff;pointer-events:none">%s</text>' % (x + 3, y[th] + d * row + 10, html.escape(name[:int(w / 6)])))
    out.append("</svg>")
    return "".join(out)


def gpu_svg(p, width=1100):
    rows = []
    for s in p.gpu_scopes():
        if s["name"] == "(unscoped)":
            continue
        st = scope_stats(p, s)
        if st["runs"]:
            rows.append((s["name"], st))
    if not rows:
        return "<p>No GPU scopes in this profile.</p>", rows
    total = sum(st["ms"] for _, st in rows)
    left, rh = 140, 16
    h = 24 + rh * len(rows)
    sx = (width - left - 90) / max(total, 1e-6)
    out = ['<svg width="%d" height="%d" role="img" aria-label="GPU pass waterfall">' % (width, h)]
    acc = 0.0
    for k in range(0, 11):
        x = left + k * (width - left - 90) / 10.0
        out.append('<line x1="%.1f" y1="16" x2="%.1f" y2="%d" stroke="var(--grid)"/>' % (x, x, h))
        out.append('<text x="%.1f" y="12" text-anchor="middle">%.2f%s</text>' % (x, total * k / 10.0, " ms" if k == 10 else ""))
    for i, (name, st) in enumerate(rows):
        yy = 20 + i * rh
        x = left + acc * sx
        w = max(1.5, st["ms"] * sx)
        tipt = "%s | %.3f ms (%.1f%%) | draws %.0f, dispatches %.0f | fs %.0f, cs %.0f | barriers %.0f, clears %.0f | ran %d / %d frames" % (
            name, st["ms"], 100 * st["ms"] / max(total, 1e-9), st["draws"], st["dispatches"], st["fs"], st["cs"], st["barriers"], st["clears"], st["runs"], st["frames"])
        out.append('<text class="lbl" x="4" y="%d">%s</text>' % (yy + 11, html.escape(name)))
        out.append('<rect class="m" x="%.1f" y="%d" width="%.1f" height="%d" rx="2" fill="var(--s1)" data-t="%s"/>' % (x, yy, w, rh - 3, html.escape(tipt)))
        out.append('<text x="%.1f" y="%d">%.3f</text>' % (x + w + 4, yy + 11, st["ms"]))
        acc += st["ms"]
    out.append("</svg>")
    return "".join(out), rows


def write(path, p):
    cw = cpu_work(p)
    g = p.gpu_frame_ms()
    tiles = [("CPU work p50", "%.2f ms" % median(cw)), ("GPU p50", "%.2f ms" % median(g) if g else "n/a"), ("Frame p50", "%.2f ms" % median(p.cpu_ms())),
             ("Frames", str(p.frames)), ("Allocs / frame", "%.1f" % p.counter_mean("alloc_count")), ("VRAM", "%.0f MB" % mean(p.series("vram_usage_mb")))]
    tls = p.d.get("timeline", [])
    tabs = "".join('<button data-for="tl%d" aria-pressed="%s">%s frame %d (%.2f ms)</button>' % (
        i, "true" if i == 0 else "false", "median" if i == 0 else "worst", t["frame"], t["ms"]) for i, t in enumerate(tls))
    lanes = "".join('<div class="tl scroll" id="tl%d" style="display:%s">%s</div>' % (i, "block" if i == 0 else "none", cpu_svg(p, t)) for i, t in enumerate(tls))
    gsvg, grows = gpu_svg(p)
    zones = sorted(p.zones(), key=lambda z: -mean(z["incl_ms"]))[:40]
    ztab = "".join("<tr><td>%s</td><td>%s</td><td>%s</td><td>%.3f</td><td>%.3f</td><td>%.1f</td></tr>" % (
        html.escape(z["name"]), z.get("thread"), z.get("cat"), mean(z["incl_ms"]), mean(z["excl_ms"]), mean(z["calls"])) for z in zones)
    gtab = "".join("<tr><td>%s</td><td>%.3f</td><td>%.0f</td><td>%.0f</td><td>%.0f</td><td>%.0f</td><td>%.0f</td><td>%.0f</td><td>%d/%d</td></tr>" % (
        html.escape(n), st["ms"], st["draws"], st["dispatches"], st["fs"], st["cs"], st["barriers"], st["clears"], st["runs"], st["frames"]) for n, st in grows)
    legend = "".join('<span><i style="background:var(--s%d)"></i>%s</span>' % (i + 1, c) for i, c in enumerate(CATS))
    doc = """<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Perf Timeline %s</title><style>%s</style></head><body><main>
<h1>%s / %s</h1><p class="sub">%s, %d frames, %s build. CPU timeline of the median and the worst frame; GPU passes in execution order (mean ms).</p>
<div class="tiles">%s</div>
<h2>CPU timeline</h2><div class="tabs">%s</div><div class="legend">%s</div>%s
<h2>GPU pass timeline</h2><div class="scroll">%s</div>
<details><summary>GPU table</summary><table><tr><th>Pass</th><th>ms</th><th>Draws</th><th>Dispatches</th><th>FS inv.</th><th>CS inv.</th><th>Barriers</th><th>Clears</th><th>Ran</th></tr>%s</table></details>
<details><summary>CPU zones table (top 40 by inclusive ms)</summary><table><tr><th>Zone</th><th>Thread</th><th>Kind</th><th>Incl ms</th><th>Excl ms</th><th>Calls</th></tr>%s</table></details>
</main><div id="tip" role="tooltip"></div><script>%s</script></body></html>""" % (
        html.escape(p.scene), STYLE, html.escape(p.scene), html.escape(p.fidelity), html.escape(p.label), p.frames, html.escape(p.d.get("meta", {}).get("config", "?")),
        "".join('<div class="tile"><span>%s</span><b>%s</b></div>' % (a, b) for a, b in tiles), tabs, legend, lanes, gsvg, gtab, ztab, SCRIPT)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(doc)
