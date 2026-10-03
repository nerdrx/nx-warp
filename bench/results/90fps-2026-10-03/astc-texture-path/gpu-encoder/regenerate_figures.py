#!/usr/bin/env python3
"""Regenerate the ASTC latency and quality/bitrate figures from saved evidence."""
import argparse
import json
import re
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np

VIOLET = "#7654d6"
INK = "#282536"
MUTED = "#6f6a7b"
PALE = "#e8e3f4"

def read_metrics(path):
    s = Path(path).read_text()
    def pair(label):
        m = re.search(rf"{label} median/p95 ms:\s*([\d.]+)\s*/\s*([\d.]+)", s)
        if not m:
            raise ValueError(f"could not parse {label} from {path}")
        return tuple(map(float, m.groups()))
    return {"cpu": pair("CPU full"), "gpu": pair("GPU transfer\\+encode\\+readback")}

def quality_points(path):
    d = json.loads(Path(path).read_text())
    def case(stem):
        return next(x for x in d["cases"] if Path(x["astc_path"]).name == stem + ".astc")
    names = ["cached-pca6-resident", "ls6-resident", "flat-ls6-resident",
             "flat-ls5-resident", "flat-ls4-resident"]
    pts = []
    for name in names:
        x = case(name)
        pts.append((name, x["lz4_payload_bytes"] * 8 * 90 / 1e6, x["rgb_psnr_db"]))
    b = d["baseline_reference"]
    baseline = (b["format"], b["lz4_raw_astc_blocks_bytes"] * 8 * 90 / 1e6,
                b["rgb_psnr_db"])
    return pts, baseline

def style(ax):
    ax.set_facecolor("white")
    ax.grid(axis="y", color=PALE, linewidth=0.9)
    ax.spines[["top", "right"]].set_visible(False)
    ax.spines[["left", "bottom"]].set_color("#d8d3e2")
    ax.tick_params(colors=MUTED, labelsize=9)
    ax.title.set_color(INK)
    ax.xaxis.label.set_color(INK)
    ax.yaxis.label.set_color(INK)

def fig_latency(log_dir, out):
    labels = ["Final flat LS6\nresident", "Flat LS6\nupload", "Forest flat LS6\nresident"]
    stems = ["final-flat-ls6-resident", "flat-ls6-upload", "forest-flat-ls6-resident"]
    data = [read_metrics(Path(log_dir) / (s + ".log")) for s in stems]
    fig, ax = plt.subplots(figsize=(9.2, 5.2), constrained_layout=True)
    x = np.arange(len(labels))
    for dx, key, color, label in [(-0.18, "cpu", "#49425d", "CPU full call"),
                                  (0.18, "gpu", VIOLET, "GPU transfer + encode + readback")]:
        med = np.array([d[key][0] for d in data])
        p95 = np.array([d[key][1] for d in data])
        xx = x + dx
        ax.vlines(xx, med, p95, color=color, linewidth=2.0, alpha=.8)
        ax.scatter(xx, med, s=48, color=color, marker="o", label=f"{label}: p50")
        ax.scatter(xx, p95, s=52, facecolors="white", edgecolors=color, linewidths=1.8,
                   marker="D", label=f"{label}: p95")
        for xi, yi in zip(xx, p95):
            ax.annotate(f"{yi:.2f}", (xi, yi), xytext=(0, 7), textcoords="offset points",
                        ha="center", fontsize=8, color=color)
        for xi, yi in zip(xx, med):
            ax.annotate(f"{yi:.2f}", (xi, yi), xytext=(0, -13), textcoords="offset points",
                        ha="center", va="top", fontsize=8, color=color)
    ax.axhline(11.11, color="#c14363", linestyle=(0, (4, 3)), linewidth=1.5,
               label="11.11 ms frame budget @ 90 Hz")
    ax.set_xticks(x, labels)
    ax.set_ylabel("Time per full 4352 × 2176 image (ms)")
    ax.set_title("ASTC block encoder latency on RX 7900 XTX", loc="left", weight="bold", pad=13)
    ax.set_ylim(0, max(11.11 * 1.12, max(d["cpu"][1] for d in data) * 1.25))
    ax.legend(frameon=False, ncol=2, fontsize=8, loc="upper left")
    ax.text(0, -0.22, "Median dot; p95 open diamond. GPU measure includes upload/transfer, encode and readback.",
            transform=ax.transAxes, fontsize=8, color=MUTED)
    style(ax)
    fig.savefig(out / "encoder-latency.png", dpi=220, bbox_inches="tight")
    fig.savefig(out / "encoder-latency.svg", bbox_inches="tight")
    plt.close(fig)

def fig_quality(quality_path, out):
    pts, baseline = quality_points(quality_path)
    fig, ax = plt.subplots(figsize=(9.2, 5.4), constrained_layout=True)
    colors = [VIOLET, "#9180ca", "#44315f", "#9983bd", "#b9a9d5"]
    marks = ["o", "s", "D", "^", "v"]
    offsets = [(10, -17), (10, 12), (8, 8), (8, -17), (8, 8)]
    for (name, mbps, psnr), color, marker, offset in zip(pts, colors, marks, offsets):
        ax.scatter(mbps, psnr, color=color, marker=marker, s=74, zorder=3)
        ax.annotate(name, (mbps, psnr), xytext=offset, textcoords="offset points",
                    fontsize=8, color=INK, arrowprops={"arrowstyle": "-", "color": color, "lw": .6})
    label, mbps, psnr = baseline
    ax.scatter(mbps, psnr, facecolors="white", edgecolors="#c14363", marker="X",
               linewidths=2, s=100, zorder=4)
    ax.annotate("XUASTC 8×8 q25 · 33.2534 dB\n634,704 B · offline encode; latency not matched",
                (mbps, psnr), xytext=(-155, 17), textcoords="offset points",
                fontsize=8, color="#a03352", ha="left",
                arrowprops={"arrowstyle": "-", "color": "#c14363", "lw": .9})
    ax.set_xlabel("Compressed ASTC-block payload at 90 fps (Mbps)")
    ax.set_ylabel("Software-decoded RGB PSNR (dB)")
    ax.set_title("Payload rate versus decoded image quality", loc="left", weight="bold", pad=13)
    ax.set_xlim(0, max(mbps for _, mbps, _ in pts + [baseline]) * 1.18)
    ax.set_ylim(min(p for _, _, p in pts + [baseline]) - 0.5,
                max(p for _, _, p in pts + [baseline]) + 0.5)
    ax.text(0, -0.2, "Payload Mbps = LZ4 block bytes × 8 × 90 / 1,000,000. XUASTC is a separate offline encode reference.",
            transform=ax.transAxes, fontsize=8, color=MUTED)
    style(ax)
    fig.savefig(out / "quality-bitrate.png", dpi=220, bbox_inches="tight")
    fig.savefig(out / "quality-bitrate.svg", bbox_inches="tight")
    plt.close(fig)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gpu-runs", type=Path, required=True, help="directory containing the three .log files")
    ap.add_argument("--quality-json", type=Path, required=True, help="least-squares-flat-comparison/astc-quality.json")
    ap.add_argument("--output-dir", type=Path, default=Path(__file__).resolve().parent)
    a = ap.parse_args()
    a.output_dir.mkdir(parents=True, exist_ok=True)
    fig_latency(a.gpu_runs, a.output_dir)
    fig_quality(a.quality_json, a.output_dir)
    print(f"Wrote figures to {a.output_dir}")

if __name__ == "__main__":
    main()
