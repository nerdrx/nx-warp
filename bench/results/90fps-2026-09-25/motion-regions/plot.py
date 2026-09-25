#!/usr/bin/env python3
"""Plot recorded regional compression data; no synthetic performance values."""
import csv
import json
from pathlib import Path
import matplotlib.pyplot as plt
import numpy as np
HERE = Path(__file__).resolve().parent
DATA = json.loads((HERE / "summary.json").read_text())
BG, FG, MUTED, GREY, PURPLE = "#100d18", "#f2eef8", "#b9b2c8", "#827994", "#b88aff"
plt.rcParams.update({"figure.facecolor": BG, "axes.facecolor": "#191526", "axes.edgecolor": MUTED,
 "text.color": FG, "axes.labelcolor": FG, "xtick.color": MUTED, "ytick.color": MUTED,
 "font.family": "DejaVu Sans", "font.size": 11})
keys = ["opposing_source_motion", "continuous_pan_source", "photo_scene_cut", "opposing_no_ack"]
labels = ["Opposing\ncentre motion", "Continuous\npan", "Scene cut +\ncontinued pan", "No ACK\nfallback"]
fig, axs = plt.subplots(1, 2, figsize=(14, 6))
fig.subplots_adjust(left=.07, right=.97, top=.77, bottom=.22, wspace=.28)
fig.text(.07, .94, "MORE COMPRESSION WHERE ONE SHIFT FAILS", fontsize=20, weight="bold")
fig.text(.07, .875, "Production Vulkan encoder · 2176 × 2176 per eye · source motion before the native-centre fade · ABBA, 48 frames per arm/case", fontsize=10, color=MUTED)
x = np.arange(4)
for arm, shift, color, label in [("global", -.18, GREY, "Existing global motion"), ("regional_fallback", .18, PURPLE, "Global + regional fallback")]:
    sizes = [DATA["encoder"][k][arm]["mean_frame_bytes"]/1000 for k in keys]
    med = [DATA["encoder"][k][arm]["p50_ms"] for k in keys]
    p95 = [DATA["encoder"][k][arm]["p95_ms"] for k in keys]
    axs[0].bar(x+shift, sizes, .34, color=color, label=label)
    axs[1].bar(x+shift, med, .34, color=color)
    axs[1].errorbar(x+shift, med, yerr=[np.zeros(4), np.array(p95)-med], fmt="none", ecolor=FG, capsize=3)
    for at, val in zip(x+shift, sizes): axs[0].text(at, val+2, f"{val:.1f}", ha="center", fontsize=9)
    for at, val, tail in zip(x+shift, med, p95): axs[1].text(at, tail+.16, f"{val:.2f}", ha="center", fontsize=9)
for ax in axs:
    ax.set_xticks(x, labels)
    ax.grid(axis="y", alpha=.13); ax.set_axisbelow(True)
    for side in ("top", "right"): ax.spines[side].set_visible(False)
axs[0].set_ylabel("Mean complete codec frame (decimal kB)")
axs[0].set_ylim(0, 150); axs[0].legend(frameon=False, fontsize=9, loc="upper left")
axs[1].set_ylabel("PC encode time (ms) · p50 bars / p95 whiskers")
axs[1].set_ylim(0, 11.2)
fig.text(.07, .065, "Opposing motion: 12.6% fewer frame bytes, +0.87 ms median PC time. Tested pan/cut/no-ACK bytes unchanged. Exact reconstruction throughout.", fontsize=10)
fig.text(.07, .025, "Controlled photographic centre / fixed synthetic periphery. Includes independent anchors and safety; excludes FEC and network overhead. No live-FPS claim.", fontsize=9, color=MUTED)
fig.savefig(HERE / "encoder.png", dpi=150); plt.close(fig)

fig, ax = plt.subplots(figsize=(11, 5.5))
fig.subplots_adjust(left=.12, right=.96, top=.76, bottom=.24)
fig.text(.12, .94, "PICO: FOUR SHIFTS, SAME RECONSTRUCTED BYTES", fontsize=18, weight="bold")
fig.text(.12, .875, "Isolated CPU header parse + restore · 561,808-byte frame · 128 native tiles · 24 warm + 24 measured samples per mode", fontsize=9, color=MUTED)
rr = list(csv.DictReader((HERE / "restore-pico.csv").open()))
for i, (mode,color) in enumerate([("global",GREY),("four_regions",PURPLE)]):
    values=[float(r["us"])/1000 for r in rr if r["mode"]==mode and r["phase"]=="measured"]
    stat=DATA["restore"]["pico"][mode]
    ax.scatter(np.full(len(values),i)+np.linspace(-.13,.13,len(values)),values,color=color,s=22,alpha=.7)
    ax.hlines(stat["p50_us"]/1000,i-.23,i+.23,color=FG,linewidth=2)
    ax.text(i,.145,f'p50 {stat["p50_us"]/1000:.3f} ms  /  p95 {stat["p95_us"]/1000:.3f} ms',ha="center",fontsize=11)
ax.set_xticks([0,1],["Global motion", "Four-region motion"])
ax.set_ylabel("CPU duration (ms)"); ax.set_xlim(-.6,1.6); ax.set_ylim(0,.16)
ax.grid(axis="y",alpha=.13); ax.set_axisbelow(True)
for side in ("top","right"):ax.spines[side].set_visible(False)
fig.text(.12,.095,"Median difference: +0.011 ms. All restores byte-exact. Interleaved ABBA/BAAB order; no clock pinning or thermal soak.",fontsize=10)
fig.text(.12,.045,"Residual copies prepared outside timer. Excludes decompression, GPU upload, rendering, networking and presentation.",fontsize=9,color=MUTED)
fig.savefig(HERE / "pico-restore.png",dpi=150);plt.close(fig)
