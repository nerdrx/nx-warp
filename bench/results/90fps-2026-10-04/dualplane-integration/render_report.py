#!/usr/bin/env python3
"""Render compact quality/payload, phase-stability, live-window, and crop-preview plots."""
import csv
import json
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent
POLICY = Path("/run/media/nerdrx/Lex/claude/nx-scratch/astc-dualplane-20261004/gpu/validation/quality-policy-20261004")
CROPS = POLICY / "q6-pan-crops"
rows = list(csv.DictReader((ROOT / "metrics.csv").open(newline="")))
fig, ax = plt.subplots(1, 3, figsize=(15, 4.6), constrained_layout=True)
colors = {"dark": "#8c3b2b", "forest": "#28658c"}
for scene in colors:
    subset = [r for r in rows if r["scene"] == scene]
    q = [int(r["quality"]) for r in subset]
    ax[0].plot(q, [float(r["psnr_gain_db"]) for r in subset], "o-", label=scene, color=colors[scene])
    ax[1].plot(q, [float(r["zstd3_change_percent"]) for r in subset], "o-", label=scene, color=colors[scene])
ax[0].axhline(0, color="#777", lw=.8)
ax[0].set(xlabel="Encoder quality", ylabel="PSNR gain vs baseline (dB)", xticks=range(2, 7), title="Quality")
ax[1].axhline(0, color="#777", lw=.8)
ax[1].set(xlabel="Encoder quality", ylabel="Zstd-3 payload change (%)", xticks=range(2, 7), title="Compressed payload")
for axis in ax[:2]:
    axis.grid(True, alpha=.25); axis.legend(frameon=False)
# q4/q6 show the eight-phase aligned step RMS, baseline beside candidate.
t = [r for r in rows if r["pan8_step_rms_change_percent"]]
labels = [f"{r['scene']} q{r['quality']}" for r in t]
x = np.arange(len(t)); width = .36
base = [float(r["pan8_baseline_step_rms"]) for r in t]
cand = [float(r["pan8_candidate_step_rms"]) for r in t]
ax[2].bar(x - width/2, base, width, label="baseline", color="#9aa1a8")
ax[2].bar(x + width/2, cand, width, label="candidate", color="#5c83b4")
for i, r in enumerate(t):
    ax[2].text(i + width/2, cand[i] + .035, f"+{float(r['pan8_step_rms_change_percent']):.2f}%", ha="center", va="bottom", fontsize=8)
ax[2].set_xticks(x, labels, rotation=20, ha="right")
ax[2].set(ylabel="Aligned phase-step RMS (RGB levels)", title="8-phase pan stability")
ax[2].legend(frameon=False); ax[2].grid(axis="y", alpha=.25)
fig.suptitle("Dual-plane mode 0x442 · selected quality policy", fontsize=13, fontweight="bold")
fig.savefig(ROOT / "quality-payload-temporal.png", dpi=180)
fig.savefig(ROOT / "quality-payload-temporal.svg")
plt.close(fig)

# Live check chart; all complete two-second windows are retained and special intervals labeled.
live = list(csv.DictReader((ROOT / "live-windows.csv").open(newline="")))
fig, ax = plt.subplots(2, 2, figsize=(12, 7), constrained_layout=True)
x = np.arange(1, len(live)+1)
status = [r["status"] for r in live]
colors_by_status = ["#c45a39" if s != "steady" else "#2f6d8e" for s in status]
for axis, field, label, title in [
    (ax[0,0], "iterations_per_s", "iterations/s", "Render cadence"),
    (ax[0,1], "fresh_source_frames", "fresh frames / 2 s", "Fresh source frames"),
    (ax[1,0], "app_gpu_pass_ms", "ms / iteration", "App-owned GPU pass"),
]:
    vals = [float(r[field]) for r in live]
    axis.plot(x, vals, color="#506b7d", lw=1)
    axis.scatter(x, vals, c=colors_by_status, zorder=3)
    axis.set(title=title, xlabel="2 s window index", ylabel=label, xticks=x)
    axis.grid(True, alpha=.25)
axis = ax[1,1]
axis.plot(x, [float(r["decode_ms"]) for r in live], "o-", label="decode")
axis.plot(x, [float(r["decode_to_selection_ms"]) for r in live], "o-", label="decode → selection")
axis.set(title="Decode and selection hold", xlabel="2 s window index", ylabel="ms", xticks=x)
axis.grid(True, alpha=.25); axis.legend(frameon=False)
for idx, label in ((1, "wake/connect"), (5, "focus transition"), (10, "26.8 ms hold")):
    for a in ax.flat:
        a.axvline(idx, color="#bb5b2c", ls="--", lw=.8, alpha=.55)
    ax[0,0].annotate(label, (idx, float(live[idx-1]["iterations_per_s"])), xytext=(3, 8), textcoords="offset points", fontsize=8, rotation=15)
fig.suptitle("Native run B · stationary WayVR · descriptive, not an A/B codec benchmark", fontsize=13, fontweight="bold")
fig.savefig(ROOT / "native-live-windows.png", dpi=170)
fig.savefig(ROOT / "native-live-windows.svg")
plt.close(fig)

# Crop-only q6 phase sweep: baseline left, selected-policy candidate right, 8 aligned roll phases.
for scene in ("dark", "forest"):
    frames = []
    for phase in range(8):
        left = Image.frombytes("RGBA", (512, 512), (CROPS / f"{scene}-baseline-q6-p{phase}.rgba").read_bytes()).convert("RGB")
        right = Image.frombytes("RGBA", (512, 512), (CROPS / f"{scene}-candidate-q6-p{phase}.rgba").read_bytes()).convert("RGB")
        frame = Image.new("RGB", (1024, 544), (25, 28, 32))
        draw = ImageDraw.Draw(frame)
        draw.text((8, 5), f"Baseline · phase {phase}", fill="white")
        draw.text((520, 5), f"Selected policy · phase {phase}", fill="white")
        frame.paste(left, (0, 32)); frame.paste(right, (512, 32))
        frames.append(frame.quantize(colors=256, method=Image.Quantize.MEDIANCUT))
    frames[0].save(ROOT / "previews" / f"{scene}-q6-phase-sweep.gif", save_all=True, append_images=frames[1:], duration=350, loop=0, optimize=True)
