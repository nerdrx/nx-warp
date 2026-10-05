#!/usr/bin/env python3
"""Recompute the published summaries and figure from retained measurements."""
import csv
from pathlib import Path
import statistics
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = Path(__file__).resolve().parent
PC_MODES = ["parallel_eye_l3", "parallel_eye_worker2_job512k_nooverlap", "parallel_eye_worker2_auto"]
PICO_MODES = ["ordinary_l3", "bounded_jobs", "automatic_workers"]
LABELS = ["Ordinary L3", "Fixed jobs", "Automatic jobs"]
COLORS = ["#8897ad", "#b79aff", "#62c7bd"]

def rows(name):
    with (HERE / name).open() as f:
        return list(csv.DictReader(f))

def percentile(values, p):
    values = sorted(values)
    return values[int((len(values) - 1) * p)]

pc = rows("pc.csv")
pico = rows("pico.csv")
steady = {m: [r for r in pc if r["mode"] == m and r["kind"] == "steady"] for m in PC_MODES}
device = {m: [r for r in pico if r["mode"] == m] for m in PICO_MODES}
assert all(len(v) == 30 for v in steady.values())
assert all(len(v) == 40 for v in device.values())
for mode, expected in [("pc-summary.csv", steady), ("pico-summary.csv", device)]:
    for r in rows(mode):
        values = expected[r["mode"]]
        field = "wall_ms" if mode.startswith("pc") else "pair_us"
        prefix = "wall" if mode.startswith("pc") else "pair"
        unit = "ms" if mode.startswith("pc") else "us"
        for p, label in [(0.5, "p50"), (0.95, "p95")]:
            actual = percentile([float(v[field]) for v in values], p)
            assert abs(actual - float(r[f"{prefix}_{label}_{unit}"])) < 1e-5

baseline = {int(r["trial"]): float(r["wall_ms"]) for r in steady[PC_MODES[0]]}
deltas = [float(r["wall_ms"]) - baseline[int(r["trial"])] for r in steady[PC_MODES[1]]]
assert all(d < 0 for d in deltas)
print(f"30/30 matched fixed-job pairs improve; mean delta {statistics.mean(deltas):.6f} ms")

plt.rcParams.update({"font.size": 10, "axes.spines.top": False, "axes.spines.right": False, "svg.hashsalt": "nx-zstd-jobs-20261005"})
fig, axes = plt.subplots(1, 3, figsize=(14, 4.7), layout="constrained")
for i, mode in enumerate(PC_MODES):
    data = [float(r["wall_ms"]) for r in steady[mode]]
    axes[0].scatter([i + ((j % 7) - 3) * .035 for j in range(len(data))], data, color=COLORS[i], s=17, alpha=.55)
    axes[0].plot([i-.25, i+.25], [percentile(data, .5)]*2, color=COLORS[i], linewidth=3)
axes[0].set(xticks=range(3), xticklabels=LABELS, ylabel="Stereo packing wall time (ms)", title="PC: 30 matched steady trials")
axes[0].set_ylim(0, 3.3)
axes[1].scatter(range(30), deltas, color=COLORS[1], s=25)
axes[1].axhline(0, color="#666666", linewidth=1)
axes[1].axhline(statistics.mean(deltas), color=COLORS[1], linestyle="--")
axes[1].set(xlabel="Matched trial", ylabel="Fixed jobs − ordinary L3 (ms)", title="Every measured pair improved")
for i, mode in enumerate(PICO_MODES):
    data = [float(r["pair_us"]) / 1000 for r in device[mode]]
    axes[2].scatter([i + ((j % 7) - 3) * .035 for j in range(len(data))], data, color=COLORS[i], s=17, alpha=.5)
    axes[2].plot([i-.25, i+.25], [percentile(data, .5)]*2, color=COLORS[i], linewidth=3)
axes[2].set(xticks=range(3), xticklabels=LABELS, ylabel="Sequential two-eye CPU decode (ms)", title="Pico: 40 calls per mode, asleep")
axes[2].set_ylim(0, 2.3)
for ax in axes:
    ax.grid(axis="y", alpha=.18)
fig.suptitle("Fixed 512 KiB Zstd jobs: less PC wall time, unchanged ASTC blocks", fontsize=15)
fig.supxlabel("Component replay only · Ryzen 9 9950X3D / Pico A8110 · +1,112 bytes per two-image fixture · no live FPS or photon claim", fontsize=9)
fig.savefig(HERE / "comparison.png", dpi=160)
fig.savefig(HERE / "comparison.svg", metadata={"Date": None})

# Keep actual bundled-library runs separate; show the regression rather than pooling tails.
fig, axes = plt.subplots(1, 2, figsize=(11, 4.7), layout="constrained")
for index, name in enumerate(["pc-bundled.csv", "pc-bundled-repeat.csv"]):
    data = rows(name)
    for i, mode in enumerate(PC_MODES[:2]):
        values = [float(r["wall_ms"]) for r in data if r["kind"] == "steady" and r["mode"] == mode]
        assert len(values) == 30
        axes[index].scatter([i + ((j % 7) - 3) * .035 for j in range(30)], values, color=COLORS[i], s=22, alpha=.6)
        axes[index].plot([i-.25, i+.25], [percentile(values, .5)]*2, color=COLORS[i], linewidth=3)
        axes[index].plot([i-.25, i+.25], [percentile(values, .95)]*2, color=COLORS[i], linestyle="--", linewidth=2)
    axes[index].set(xticks=range(2), xticklabels=LABELS[:2], ylabel="Two-eye packing wall time (ms)", title=f"Bundled Zstd: run {index+1}", ylim=(0, 4))
    axes[index].grid(axis="y", alpha=.18)
fig.suptitle("Actual server library: median gain, variable slow tails", fontsize=15)
fig.supxlabel("30 matched pairs per run · solid = p50 · dashed = p95 · first run p95 regresses · component replay, not live latency", fontsize=9)
fig.savefig(HERE / "bundled.png", dpi=160)
fig.savefig(HERE / "bundled.svg", metadata={"Date": None})
for path in [HERE / "comparison.svg", HERE / "bundled.svg"]:
    path.write_text("\n".join(line.rstrip() for line in path.read_text().splitlines()) + "\n")
