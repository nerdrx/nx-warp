#!/usr/bin/env python3
"""Summarise warm decoder logs and plot comparable device timings."""
from pathlib import Path
import argparse, json, re
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

LINE = re.compile(r"frame (\d+): .*?parse ([\d.]+) .*?passA ([\d.]+) .*?passW ([\d.]+) .*?passB ([\d.]+) .*?gpu ([\d.]+) .*?total ([\d.]+) ms")

def read(paths, warm_after):
    out = {k: [] for k in ("parse", "passA", "passB", "gpu", "total")}
    for path in paths:
        for line in path.read_text(errors="replace").splitlines():
            m = LINE.search(line)
            if m and int(m.group(1)) >= warm_after:
                vals = m.groups()[1:]
                for k, v in zip(("parse", "passA", "passW", "passB", "gpu", "total"), vals):
                    if k != "passW": out[k].append(float(v))
    return out

def stats(values):
    a = np.asarray(values, dtype=float)
    return {"n": int(a.size), "mean_ms": float(np.mean(a)),
            "p50_ms": float(np.percentile(a, 50)),
            "p95_ms": float(np.percentile(a, 95))}

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--logs", type=Path,
                   default=Path(__file__).resolve().parent / "logs")
    p.add_argument("--out", type=Path, default=Path(__file__).resolve().parent)
    p.add_argument("--warm-after", type=int, default=10)
    a = p.parse_args(); a.out.mkdir(parents=True, exist_ok=True)
    groups = {
        "native": sorted(a.logs.glob("packed-*-native.log")),
        "packed compact": sorted(a.logs.glob("packed-*-compact.log")),
        "flat native (rejected)": sorted(a.logs.glob("flat-*-native.log")),
        "flat compact (rejected)": sorted(a.logs.glob("flat-*-compact.log")),
    }
    summary = {name: {k: stats(v) for k, v in read(paths, a.warm_after).items()}
               for name, paths in groups.items()}
    (a.out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    metrics = ["passA", "passB", "gpu", "total"]
    fig, ax = plt.subplots(figsize=(10, 5.4))
    x = np.arange(len(metrics)); width = .19
    colors = ["#3f8f83", "#1f6f67", "#d28b32", "#b36b1f"]
    for i, (name, values) in enumerate(summary.items()):
        med = [values[m]["p50_ms"] for m in metrics]
        p95 = [values[m]["p95_ms"] for m in metrics]
        ax.bar(x + (i - 1.5) * width, med, width, yerr=np.stack((np.zeros(len(med)), np.array(p95) - med)),
               capsize=3, color=colors[i], label=name)
    ax.set_xticks(x, ["Pass A", "Pass B", "GPU", "total"])
    ax.set_ylabel("warm time (ms)")
    ax.set_title("Compact-centre decoder archive — p50 with p95 whiskers")
    ax.grid(axis="y", alpha=.25); ax.legend(fontsize=8, ncols=2)
    fig.tight_layout(); fig.savefig(a.out / "timing-comparison.png", dpi=160)

if __name__ == "__main__": main()
