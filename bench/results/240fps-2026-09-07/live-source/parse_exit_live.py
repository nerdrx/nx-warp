#!/usr/bin/env python3
"""Reproduce the exit-live JSON and figure from the retained adb logs."""
import hashlib, json, re
from pathlib import Path
import matplotlib.pyplot as plt

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[3]
PAT = re.compile(r"(?P<ts>\d\d-\d\d \d\d:\d\d:\d\d\.\d+).*render: (?P<it>\d+) iterations in (?P<sec>[\d.]+) s \((?P<rate>[\d.]+)/s\), (?P<sub>\d+) submitted.*?, (?P<new>\d+) new-source")
rows = []
for cap in ("exit-first", "exit-last"):
    filtered = HERE / f"{cap}-live-render-lines.txt"
    lines = filtered.read_text(errors="replace").splitlines(True)
    for line in lines:
        m = PAT.search(line)
        if m:
            d = m.groupdict(); sec = float(d["sec"]); it = int(d["it"])
            rows.append({"capture": cap, "timestamp": d["ts"], "iterations": it,
                         "window_s_printed": sec, "render_iterations_per_s": float(d["rate"]),
                         "submitted": int(d["sub"]), "new_source": int(d["new"]),
                         "startup": sec > 2.1 or it < 10})
data = {"source_logs": [f"{x}-live-render-lines.txt" for x in ("exit-first", "exit-last")],
        "apk_sha256": "ba7729409334305c634ee4eaa9d602993aabea05004b07572fc6d5d04d19e5f6",
        "atlas_mode": 0, "ratio_note": "new-source/s uses printed rounded window_s values",
        "rows": rows, "filtered_render_lines_sha256": {}}
for cap in ("exit-first", "exit-last"):
    data["filtered_render_lines_sha256"][f"{cap}-live"] = hashlib.sha256((HERE / f"{cap}-live-render-lines.txt").read_bytes()).hexdigest()
(HERE / "exit-live-series.json").write_text(json.dumps(data, indent=2) + "\n")
fig, ax = plt.subplots(2, 1, figsize=(8, 5))
for cap in ("exit-first", "exit-last"):
    rr = [r for r in rows if r["capture"] == cap and not r["startup"]]
    x = range(1, len(rr) + 1)
    ax[0].plot(x, [r["render_iterations_per_s"] for r in rr], ".-", label=cap)
    ax[1].plot(x, [r["new_source"] / r["window_s_printed"] for r in rr], ".-", label=cap)
ax[0].set_ylabel("render iterations/s"); ax[1].set_ylabel("new-source/s")
ax[1].set_xlabel("recorded 2 s window index")
for a in ax: a.grid(alpha=.3); a.legend()
fig.suptitle("Exit experiment live cadence (ordinary atlas mode 0)")
fig.tight_layout(); fig.savefig(REPO / "docs/figures/240fps/exit-live-series.png", dpi=160)
