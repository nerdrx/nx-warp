#!/usr/bin/env python3
"""Regenerate the 90 Hz benchmark gallery from the archived raw CSV inputs."""
from __future__ import annotations

import argparse, csv, io, tarfile
from pathlib import Path
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

DEADLINE = 1000 / 90
COLORS = ["#245b8a", "#d05a3a", "#3b8c68", "#8a5aa8", "#b17a22", "#4e858b", "#7c506a"]
SINGLE = ["full-a", "multi-a", "single-a", "single-b", "multi-b", "full-b"]

def read_archive(path: Path) -> dict[str, list[dict[str, float]]]:
    out = {}
    with tarfile.open(path, "r:gz") as ar:
        for m in ar.getmembers():
            if not m.name.endswith(".csv"):
                continue
            name = Path(m.name).stem
            out[name] = [{k: (float(v) if k != "frame" else int(v)) for k, v in r.items()}
                         for r in csv.DictReader(io.TextIOWrapper(ar.extractfile(m), encoding="utf-8"))]
    return out

def setup(title):
    fig, ax = plt.subplots(figsize=(10.5, 5.7), constrained_layout=True)
    fig.suptitle(title + "\nPico 4 · native 4352 × 2176 stereo · synthetic motion at 90 Hz · offscreen", fontsize=12, fontweight="bold")
    return fig, ax

def save(fig, out, name):
    fig.savefig(out / name, dpi=180, facecolor="white")
    plt.close(fig)

def ecdf(values):
    x = np.sort(values); y = np.arange(1, len(x) + 1) / len(x)
    return x, y

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--single", type=Path, default=Path("bench/results/90fps-2026-09-08/single-pass/raw-inputs.tar.gz"))
    ap.add_argument("--centre", type=Path, default=Path("bench/results/90fps-2026-09-08/centre-first/raw-inputs.tar.gz"))
    ap.add_argument("--pressure", type=Path, default=Path("bench/results/90fps-2026-09-08/budget-pressure/raw-inputs.tar.gz"))
    ap.add_argument("--out", type=Path, default=Path("docs/visual-results"))
    args = ap.parse_args(); args.out.mkdir(parents=True, exist_ok=True)
    single, centre, pressure = read_archive(args.single), read_archive(args.centre), read_archive(args.pressure)
    plt.rcParams.update({"font.family": "DejaVu Sans", "axes.spines.top": False, "axes.spines.right": False,
                         "axes.grid": True, "grid.alpha": .22, "font.size": 10})

    # 1. ECDF: the deadline is a reference line, not an interval measurement.
    fig, ax = setup("Latency distribution · single-pass experiment")
    for i, n in enumerate(SINGLE):
        x, y = ecdf([r["total_ms"] for r in single[n]])
        ax.step(x, y, where="post", label=n, color=COLORS[i], lw=1.8)
    ax.axvline(DEADLINE, color="#b3261e", ls="--", lw=1.4, label=f"90 Hz deadline ({DEADLINE:.3f} ms)")
    ax.set(xlabel="Arrival to GPU completion (ms)", ylabel="Empirical cumulative fraction", xlim=(0, max(DEADLINE * 1.35, 15)))
    ax.legend(ncol=2, fontsize=8); save(fig, args.out, "01-latency-ecdf.png")

    # 2. Time series of the paired single-pass and full controls.
    fig, ax = setup("Frame latency over the 720-frame run")
    for i, n in enumerate(["single-a", "single-b", "full-a", "full-b"]):
        rows = single[n]; ax.plot([r["frame"] for r in rows], [r["total_ms"] for r in rows], lw=.7, alpha=.85, label=n, color=COLORS[i+1])
    ax.axhline(DEADLINE, color="#b3261e", ls="--", lw=1.2, label="90 Hz deadline")
    ax.set(xlabel="Frame index (n = 720)", ylabel="Arrival to GPU completion (ms)"); ax.legend(ncol=3, fontsize=8)
    save(fig, args.out, "02-single-vs-full-timeseries.png")

    # 3. Independent stage components; GPU and fence are deliberately not stacked.
    fig, axes = plt.subplots(1, 2, figsize=(11, 5.7), sharey=True)
    fig.suptitle("Stage components · medians and p99s\nPico 4 · native 4352 × 2176 stereo · synthetic motion at 90 Hz · offscreen", fontsize=12, fontweight="bold")
    names = ["full-a", "multi-a", "single-a", "single-b", "multi-b", "full-b"]
    comps = [("parse_ms", "parse"), ("upload_ms", "upload"), ("gpu_ms", "GPU execution")]
    x = np.arange(len(names)); width = .24
    for j, (field, label) in enumerate(comps):
        med = [np.median([r[field] for r in single[n]]) for n in names]
        p99 = [np.percentile([r[field] for r in single[n]], 99) for n in names]
        axes[0].bar(x + (j-1)*width, med, width, label=label, color=COLORS[j])
        axes[1].bar(x + (j-1)*width, p99, width, label=label, color=COLORS[j])
    for ax, title in zip(axes, ["Median", "p99"]):
        ax.set_title(title); ax.set_xticks(x, names, rotation=35, ha="right"); ax.set_ylabel("Stage time (ms)"); ax.legend(fontsize=8)
    fig.tight_layout(rect=(0, .12, 1, .89))
    fig.text(.5, .035, "Independent measured components; fence wait is omitted because it overlaps GPU execution and would double-count wall time.", ha="center", fontsize=8)
    save(fig, args.out, "03-stage-components.png")

    # 4. Exact integer deadline miss counts.
    fig, ax = setup("Deadline misses · exact counts")
    all_names = SINGLE
    misses = [sum(r["total_ms"] > DEADLINE for r in single[n]) for n in all_names]
    ax.set_yticks(range(max(misses + [1]) + 2))
    bars = ax.bar(np.arange(len(all_names)), misses, color=[COLORS[i % len(COLORS)] for i in range(len(all_names))])
    ax.bar_label(bars, fmt="%d", padding=3); ax.set_xticks(np.arange(len(all_names)), all_names, rotation=40, ha="right")
    ax.set(xlabel="Run label", ylabel=f"Frames over {DEADLINE:.3f} ms (out of 720)", ylim=(0, max(misses + [1]) + 1)); save(fig, args.out, "04-deadline-misses.png")

    # 5. Skip fraction and retained age traces for the single-pass arms.
    fig, axes = plt.subplots(2, 1, figsize=(10.5, 6.8), sharex=True)
    fig.suptitle("Single-pass traces · skipped tiles and retained peripheral age\nPico 4 · native 4352 × 2176 stereo · synthetic motion at 90 Hz · offscreen", fontsize=12, fontweight="bold")
    for i, n in enumerate(["single-a", "single-b", "multi-a", "multi-b"]):
        rows = single[n]; total = np.array([r["tiles_rendered"] + r["tiles_skipped"] for r in rows]);
        axes[0].plot([r["frame"] for r in rows], np.array([r["tiles_skipped"] for r in rows]) / total, lw=.8, label=n, color=COLORS[i])
        axes[1].plot([r["frame"] for r in rows], np.array([r["peripheral_age_max"] for r in rows]) / 90 * 1000, lw=.8, label=n, color=COLORS[i])
    axes[0].set_ylabel("Skipped tile fraction"); axes[0].set_ylim(-.02, 1.02); axes[0].legend(fontsize=8)
    axes[1].set(xlabel="Frame index (n = 720)", ylabel="Oldest retained tile (ms)"); axes[1].legend(fontsize=8)
    save(fig, args.out, "05-skips-and-age.png")

    # 6. Historical v1/v2 starvation traces, clearly marked as historical.
    fig, axes = plt.subplots(1, 2, figsize=(11, 5.5))
    fig.suptitle("Historical peripheral starvation comparison\nPico 4 · native 4352 × 2176 stereo · synthetic motion at 90 Hz · offscreen", fontsize=12, fontweight="bold")
    hist = [("foveated-b", "v1 lifetime-maximum estimate", centre["foveated-b"]), ("budget2", "v1 forced 2 ms budget", centre["budget2"]),
            ("foveated-v2-b", "v2 expiring estimate", centre["foveated-v2-b"])]
    for i, (_, label, rows) in enumerate(hist):
        axes[0].plot([r["frame"] for r in rows], [r["peripheral_age_max"] for r in rows], lw=1, label=label, color=COLORS[i])
        axes[1].plot([r["frame"] for r in rows], [r["tiles_skipped"] / (r["tiles_skipped"] + r["tiles_rendered"]) for r in rows], lw=1, label=label, color=COLORS[i])
    axes[0].set(xlabel="Frame index", ylabel="Oldest retained age (frames)"); axes[1].set(xlabel="Frame index", ylabel="Skipped tile fraction"); axes[0].legend(fontsize=8)
    fig.tight_layout(rect=(0, .11, 1, .89))
    fig.text(.5, .035, "Historical controls are included to show starvation behavior; they are not a current production claim.", ha="center", fontsize=8)
    save(fig, args.out, "06-historical-v1-v2-starvation.png")

    # 7. Artificial admission pressure: keep its budget axis separate from the 90 Hz deadline.
    fig, axes = plt.subplots(1, 2, figsize=(11, 5.5))
    fig.suptitle("Artificial admission pressure · 2 ms vs 3 ms budgets\nPico 4 · native 4352 × 2176 stereo · synthetic motion at 90 Hz · offscreen", fontsize=12, fontweight="bold")
    for i, n in enumerate(["budget2", "budget3"]):
        rows = pressure[n]; age = np.array([r["peripheral_age_max"] for r in rows]);
        axes[0].plot(age, [r["total_ms"] for r in rows], ".", ms=2.2, alpha=.5, label=n, color=COLORS[i])
        axes[1].plot([r["frame"] for r in rows], age, lw=.8, label=n, color=COLORS[i])
    axes[0].set(xlabel="Oldest retained age (frames)", ylabel="Measured completion time (ms)"); axes[1].set(xlabel="Frame index", ylabel="Oldest retained age (frames)"); axes[0].legend(fontsize=8)
    fig.tight_layout(rect=(0, .12, 1, .89))
    fig.text(.5, .035, "Artificial budget-pressure controls; 90 Hz deadline is not used as the budget threshold here. Both traces end at 719 frames of retained age.", ha="center", fontsize=8)
    save(fig, args.out, "07-budget-pressure-age.png")

if __name__ == "__main__": main()
