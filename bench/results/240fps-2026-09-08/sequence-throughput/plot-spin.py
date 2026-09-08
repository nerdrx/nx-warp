#!/usr/bin/env python3
"""Plot the retained sequence spin-wait aggregate logs."""
import re
from pathlib import Path

import matplotlib.pyplot as plt

ROOT = Path(__file__).parent
arms = [("control", "sequence-spin-control.log"),
        ("spin 4000 us", "sequence-spin-spin.log"),
        ("restored", "sequence-spin-restored.log")]
rows = []
for label, name in arms:
    text = (ROOT / name).read_text()
    def number(key):
        return float(re.search(rf"\b{key} ([0-9.]+)", text).group(1))
    rows.append((label, number("render_steady_fps"), number("render_p99_ms"),
                 number("process_cpu_s")))

fig, axes = plt.subplots(1, 3, figsize=(9.2, 3.2))
labels = [r[0] for r in rows]
for ax, values, title, ylabel in [
    (axes[0], [r[1] for r in rows], "Repeated renders", "steady renders/s"),
    (axes[1], [r[2] for r in rows], "Render tail", "p99 ms"),
    (axes[2], [r[3] for r in rows], "Process cost", "CPU seconds"),
]:
    ax.bar(labels, values, color=["#4472c4", "#ed7d31", "#a5a5a5"])
    ax.set_title(title)
    ax.set_ylabel(ylabel)
    ax.grid(axis="y", alpha=.2)
axes[1].axhline(1000 / 240, color="crimson", ls="--", lw=1,
                label="4.1667-ms 240-Hz budget")
axes[1].legend(fontsize=7, frameon=False, loc="upper left", bbox_to_anchor=(0.0, -0.22))
fig.suptitle("Pico sequence spin-wait experiment (same binary)")
fig.tight_layout(rect=(0, 0.06, 1, 1))
fig.savefig(ROOT / "spin-comparison.png", dpi=160)
