#!/usr/bin/env python3
"""Plot the retained fixed-QP40 active report windows."""
import re
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = Path(__file__).resolve().parent
NUM = r"(?:\d+(?:\.\d+)?|\.\d+)"

def parse(path):
    text = path.read_text(errors="replace")
    gpu = [float(x) for x in re.findall(rf"nxwarp\[\d+\]: \d+ frames in .*? gpu ({NUM}) ms", text)]
    rates = [float(n) / float(d) for d, n in re.findall(
        rf"render: \d+ iterations in ({NUM}) s.*?, (\d+) new-source", text, re.I)]
    return gpu, rates

atlas_gpu, atlas_rate = parse(HERE / "atlas-measure-filtered.log")
off_gpu, off_rate = parse(HERE / "off-measure-filtered.log")
if not all((atlas_gpu, atlas_rate, off_gpu, off_rate)):
    raise SystemExit("missing report-window values")

fig, axes = plt.subplots(1, 2, figsize=(10, 4.2))
colors = {"atlas:auto": "#1677b3", "atlas:off": "#c44e52"}
for ax, title, series, ylabel in (
    (axes[0], "Decoder GPU window", [("atlas:auto", atlas_gpu), ("atlas:off", off_gpu)], "ms"),
    (axes[1], "New-source window rate", [("atlas:auto", atlas_rate), ("atlas:off", off_rate)], "sources/s"),
):
    for label, values in series:
        x = range(1, len(values) + 1)
        ax.plot(x, values, "o", ms=4, alpha=.8, color=colors[label], label=label)
        med = sorted(values)[len(values) // 2] if len(values) % 2 else (sorted(values)[len(values)//2-1] + sorted(values)[len(values)//2]) / 2
        ax.axhline(med, color=colors[label], ls="--", lw=1)
        ax.text(1.01, med, f" median {med:g}", color=colors[label], va="center", fontsize=8)
    ax.set_title(title)
    ax.set_xlabel("Active 2 s report-window index")
    ax.set_ylabel(ylabel)
    ax.grid(alpha=.25)
    ax.legend(frameon=False, loc="best")

fig.suptitle("Fixed QP 40, pace 45: matched active-window observations", fontsize=12)
fig.tight_layout(rect=(0, .14, 1, 1))
fig.text(.5, .025, "Report gaps: atlas ≤5.522 s; off ≤6.842 s. No continuous-FPS or causal-performance claim.", ha="center", fontsize=8)
fig.savefig(HERE / "fixed-qp40-window-results.png", dpi=160)
fig.savefig(HERE / "fixed-qp40-window-results.svg")
