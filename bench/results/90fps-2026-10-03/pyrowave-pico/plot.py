#!/usr/bin/env python3
"""Plot the bounded Pico PyroWave decoder measurements in this directory."""

from pathlib import Path

import matplotlib.pyplot as plt


ROOT = Path(__file__).parent
x = range(3)
labels = ["2688×1344\nstill", "2688×1344\nmotion", "4352×2176\nstill"]
gpu_p50 = [8.75182, 8.58677, 20.3481]
gpu_p95 = [8.76682, 9.23385, 20.6556]
cpu_p50 = [12.6583, 19.9579, 25.6947]
cpu_p95 = [23.4509, 23.7741, 35.6925]

plt.rcParams.update({"font.size": 11, "axes.spines.top": False, "axes.spines.right": False})
fig, ax = plt.subplots(figsize=(9, 5.5), layout="constrained")
ax.plot(x, gpu_p50, "o-", color="#6245d9", lw=2.5, label="GPU p50")
ax.plot(x, gpu_p95, "o--", color="#6245d9", alpha=0.65, label="GPU p95")
ax.plot(x, cpu_p50, "s-", color="#148a91", lw=2, label="CPU call p50")
ax.plot(x, cpu_p95, "s--", color="#148a91", alpha=0.65, label="CPU call p95")
ax.axhline(1000 / 90, color="#d84e4e", lw=1.7, ls=":", label="90 Hz: 11.11 ms")
ax.set_xticks(list(x), labels)
ax.set_ylabel("Decode time (ms)")
ax.set_xlabel("Whole stereo frame; 4:2:0")
ax.set_title("Pico 4 PyroWave decode after input flush · isolated runs")
ax.grid(axis="y", alpha=0.2)
ax.legend(ncol=3, loc="upper left")
ax.set_ylim(0, 40)
fig.savefig(ROOT / "decode-latency.png", dpi=180)

fig, ax = plt.subplots(figsize=(7.5, 4.8), layout="constrained")
names = ["2688×1344", "4352×2176"]
dequant = [3.74594, 7.45969]
idwt = [4.99927, 12.8841]
ax.bar(names, dequant, color="#148a91", label="Dequantization")
ax.bar(names, idwt, bottom=dequant, color="#6245d9", label="Inverse wavelet + output")
ax.axhline(1000 / 90, color="#d84e4e", lw=1.7, ls=":", label="90 Hz: 11.11 ms")
ax.set_ylim(0, 25)
ax.set_ylabel("Median GPU time (ms)")
ax.set_title("Pico 4 decode stages · 2 warmups + 12 measured frames")
ax.grid(axis="y", alpha=0.2)
ax.legend()
fig.savefig(ROOT / "stage-breakdown.png", dpi=180)
