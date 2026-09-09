#!/usr/bin/env python3
"""Regenerate compact-direct summary.json and latency.png from archived logs."""
import json
import re
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np

ROOT = Path(__file__).resolve().parent
METRICS = ("passA", "passB", "gpu", "total")
METRIC_RE = {metric: re.compile(r"\b" + metric + r" ([0-9.]+)")
             for metric in METRICS}


def samples():
    values = {v: {m: [] for m in METRICS} for v in ("baseline", "direct")}
    for path in sorted(ROOT.glob("paired-*.log")):
        variant = "direct" if "-direct.log" in path.name else "baseline"
        for line in path.read_text().splitlines():
            match = re.match(r"frame (\d+):", line)
            if not match or int(match.group(1)) < 10:
                continue
            for metric, pattern in METRIC_RE.items():
                frame_metric = pattern.search(line)
                if frame_metric:
                    values[variant][metric].append(float(frame_metric.group(1)))
    return values


def main():
    values = samples()
    def stats(data):
        data = np.asarray(data)
        if not data.size:
            return None
        return {
            "n": int(data.size),
            "mean": float(np.mean(data)),
            "p50": float(np.percentile(data, 50)),
            "p95": float(np.percentile(data, 95)),
            "p99": float(np.percentile(data, 99)),
        }

    summary = {
        variant: {
            metric: result
            for metric, data in metrics.items()
            if (result := stats(data)) is not None
        }
        for variant, metrics in values.items()
    }
    (ROOT / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")

    # Show the central latency and the long tail together. Whiskers are p95;
    # p99 is plotted separately so it remains visible beside the p50 bars.
    fig, axes = plt.subplots(1, 4, figsize=(12, 4), sharey=False)
    for axis, metric in zip(axes, METRICS):
        data = [values[variant][metric] for variant in ("baseline", "direct")]
        axis.boxplot(data, tick_labels=("baseline", "direct"), showfliers=True,
                     whis=(5, 95), widths=0.55)
        p50 = [np.percentile(x, 50) for x in data]
        p95 = [np.percentile(x, 95) for x in data]
        p99 = [np.percentile(x, 99) for x in data]
        axis.plot((1, 2), p50, "o", label="p50", color="#1769aa")
        axis.plot((1, 2), p95, "_", ms=14, label="p95", color="#d1495b")
        axis.plot((1, 2), p99, "^", ms=5, label="p99", color="#6a4c93")
        axis.set_title(metric)
        axis.set_ylabel("milliseconds")
        axis.grid(axis="y", alpha=0.25)
    axes[0].legend(fontsize=8, loc="upper right")
    fig.suptitle("Compact center decoder latency (frames 10–59)")
    fig.tight_layout()
    fig.savefig(ROOT / "latency.png", dpi=160)
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
