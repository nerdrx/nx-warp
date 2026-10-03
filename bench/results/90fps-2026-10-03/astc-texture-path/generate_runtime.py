#!/usr/bin/env python3
"""Plot retained Pico samples; CPU call includes GPU fence wait, never add bars."""
import csv
import json
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

ROOT = Path(__file__).resolve().parent
rows = list(csv.DictReader((ROOT / "evidence/pico-astc/logs/q25-alternating-samples.csv").open()))
paths = ["raw_rgba", "astc_raw_payload", "astc_lz4_alternating_frames"]
names = ["Raw RGBA upload + sample", "Raw ASTC upload + sample", "LZ4 + ASTC, two changing frames"]

def quantile(values, q):
    values = sorted(values)
    return values[int(q * (len(values) - 1))]

summary = {}
for path in paths:
    samples = [r for r in rows if r["path"] == path]
    assert len(samples) == 30, (path, len(samples))
    summary[path] = {"samples": len(samples)}
    for field in ("gpu_upload_us", "gpu_sample_us", "gpu_total_us", "cpu_call_us"):
        values = [float(r[field]) / 1000 for r in samples]
        summary[path][field.replace("_us", "_ms")] = {
            "p50": quantile(values, .5), "p95": quantile(values, .95)
        }
(ROOT / "pico-summary.json").write_text(json.dumps(summary, indent=2) + "\n")

plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 11,
                     "text.color": "#eee6ff", "axes.labelcolor": "#eee6ff",
                     "xtick.color": "#ccc0de", "ytick.color": "#eee6ff",
                     "axes.edgecolor": "#665675", "svg.fonttype": "none"})
fig, axes = plt.subplots(1, 2, figsize=(15, 5.2), facecolor="#0c0818")
fig.subplots_adjust(left=.23, right=.96, top=.76, bottom=.20, wspace=.18)
fig.text(.035, .93, "Pico 4 · native ASTC texture-path prototype", fontsize=21, weight="bold")
fig.text(.035, .86, "4352 × 2176 pixels · 12 warmups + 30 samples per path · p50 bars, p95 whiskers", fontsize=12)
colors = ["#81738f", "#36cde0", "#ab70ff"]
for ax, field, title in zip(axes, ("gpu_total_ms", "cpu_call_ms"),
                             ("GPU: upload + full-image sample/write", "CPU call: unpack through GPU fence")):
    ax.set_facecolor("#0c0818")
    p50 = np.array([summary[p][field]["p50"] for p in paths])
    p95 = np.array([summary[p][field]["p95"] for p in paths])
    ys = np.arange(3)
    ax.barh(ys, p50, color=colors, height=.55,
            xerr=np.vstack([np.zeros(3), p95 - p50]), capsize=4,
            error_kw={"ecolor": "#eee6ff", "elinewidth": 1.4})
    ax.set_yticks(ys, names if ax is axes[0] else [""] * 3)
    ax.invert_yaxis()
    ax.set_xlim(0, 16)
    ax.set_xticks([0, 4, 8, 12, 16])
    ax.set_xlabel("Elapsed time (ms)")
    ax.set_title(title, pad=13, fontsize=11)
    ax.grid(axis="x", color="#574063", alpha=.35)
    ax.set_axisbelow(True)
    ax.axvline(1000 / 90, color="#ff7c8e", linestyle="--", linewidth=1.3)
    for y, a, b in zip(ys, p50, p95):
        ax.text(b + .3, y, f"{a:.2f} / {b:.2f}", va="center", fontsize=10)
fig.text(.035, .09, "Dashed: entire 90 Hz interval (11.11 ms), not an allocated decoder budget. CPU includes GPU wait; do not add CPU + GPU.", fontsize=10)
fig.text(.035, .045, "Standalone compute output, not compositor presentation. No network, live encoder, or photon-latency measurement.", fontsize=10)
for ext in ("png", "svg"):
    fig.savefig(ROOT / f"pico-texture-path.{ext}", dpi=180, facecolor=fig.get_facecolor())
plt.close(fig)
print("Retained-sample summaries and Pico figures generated.")
