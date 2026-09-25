#!/usr/bin/env python3
"""Regenerate figures from the published, bounded benchmark samples."""
from pathlib import Path
import csv
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parent
plt.rcParams.update({"font.size": 10, "figure.facecolor": "#100d1b", "axes.facecolor": "#191426",
                     "text.color": "#f1edf9", "axes.labelcolor": "#e0d7ef", "xtick.color": "#cfc3df",
                     "ytick.color": "#cfc3df", "axes.edgecolor": "#665974", "grid.color": "#45394f"})


def rows(name):
    with (ROOT / name).open() as f:
        return list(csv.DictReader(f))


def finish(fig, name, note):
    fig.text(.03, .025, note, fontsize=9, color="#bdb0d0")
    fig.tight_layout(rect=(0, .08, 1, .94))
    fig.savefig(ROOT / name, dpi=160)
    plt.close(fig)


def pico():
    data = rows("pico-summary.csv")
    labels = [r["fixture"].replace("forest", "Forest").replace("dark", "Detail").replace("_", " ") for r in data]
    savings = [-float(r["delta_pct"]) for r in data]
    fig, (a, b) = plt.subplots(1, 2, figsize=(13, 6.5), gridspec_kw={"width_ratios": [1, 1.2]})
    fig.suptitle("Lossless vertical prediction: bytes saved and Pico CPU cost", weight="bold", fontsize=15)
    y = np.arange(len(data))
    a.barh(y, savings, color=["#b48aff" if v >= 5 else "#6b6079" for v in savings])
    a.axvline(5, color="#ffca73", ls="--", lw=1, label="5% selection gate")
    a.set_yticks(y, labels)
    a.invert_yaxis()
    a.set_xlabel("Detail-envelope bytes saved (%)")
    a.grid(axis="x", alpha=.35)
    a.legend(loc="lower right", framealpha=.1)
    for i, v in enumerate(savings):
        a.text(v + .15, i, f"{v:.2f}%" if v < 5 else f"{v:.1f}%", va="center", fontsize=9)
    a.set_xlim(0, max(savings) + 2.5)
    for shift, prefix, label, color in [(-.17, "baseline", "Existing predictor", "#8180a0"),
                                        (.17, "candidate", "Vertical predictor", "#b48aff")]:
        med = np.array([float(r[f"{prefix}_p50_us"]) / 1000 for r in data])
        tail = np.array([float(r[f"{prefix}_p95_us"]) / 1000 for r in data])
        b.barh(y + shift, med, .31, color=color, label=label)
        b.errorbar(med, y + shift, xerr=[np.zeros(len(data)), np.maximum(0, tail-med)],
                   fmt="none", ecolor="#e6def1", capsize=2, lw=1)
    b.set_yticks(y, [])
    b.invert_yaxis()
    b.set_xlabel("Full isolated decode helper (ms); bar p50, whisker p95")
    b.grid(axis="x", alpha=.35)
    b.legend(loc="lower left", bbox_to_anchor=(0, 1.01), ncol=2, framealpha=.1)
    finish(fig, "pico.png", "Byte-exact reconstruction. Independent frames and motion residuals; duplicated-eye photo fixtures.\nCPU helper only: excludes upload, GPU reconstruction, Wi-Fi and presentation. Raw trials below 5% are not selected.")


def production():
    data = [r for r in rows("encoder-samples.csv") if r["phase"].startswith("measure") and r.get("frame") == "measure"]
    # Each case/region setting has two runs per arm in ABBA order.
    keys = list(dict.fromkeys((r["case"], r["regions"]) for r in data))
    fig, (a, b) = plt.subplots(1, 2, figsize=(13, max(5.5, len(keys)*.56)))
    fig.suptitle("Production Vulkan encoder: same encoded pixels, optional vertical prediction", weight="bold", fontsize=14)
    y = np.arange(len(keys))
    for shift, variant, color in [(-.17, "baseline", "#8180a0"), (.17, "candidate", "#b48aff")]:
        groups = [[r for r in data if (r["case"], r["regions"]) == k and r["variant"] == variant] for k in keys]
        a.barh(y+shift, [np.mean([float(r["wire_bytes"]) for r in g])/1000 for g in groups], .31, color=color, label=variant)
        med = [np.median([float(r["encode_ms"]) for r in g]) for g in groups]
        b.barh(y+shift, med, .31, color=color, label=variant)
    a.set_yticks(y, [f"{case}\n{region}" for case, region in keys], fontsize=9)
    b.set_yticks(y, [])
    for ax in (a,b):
        ax.invert_yaxis()
        ax.grid(axis="x", alpha=.35)
        ax.legend(framealpha=.1)
    a.set_xlabel("Mean complete codec frame (kB, includes safety)")
    b.set_xlabel("Median PC encode (ms)")
    b.axvline(1000/90, color="#ffca73", ls="--", lw=1)
    finish(fig, "encoder.png", "Bounded offscreen production encodes, ABBA order. Dashed line: 11.11 ms frame budget at 90 Hz.\nNo sustained FPS, Wi-Fi improvement or physical photon-latency claim.")


if __name__ == "__main__":
    pico()
    if (ROOT / "encoder-samples.csv").exists():
        production()
