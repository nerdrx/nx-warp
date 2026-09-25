"""Plot the recorded short Pico runs; no simulated performance values."""
import json
from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

root = Path(__file__).resolve().parent
names = ["checker-off-f", "checker-on-g", "checker-off-h"]
rows = [json.loads((root / (name + ".json")).read_text())["summary"] for name in names]
plt.rcParams.update({"font.size": 10, "axes.spines.top": False, "axes.spines.right": False})
fig, axes = plt.subplots(1, 3, figsize=(12, 4.4))
for ax, group, key, label in zip(axes, ["server", "client", "client"],
        ["payload_mbps", "fresh_fps", "gpu_ms"],
        ["Complete codec payload (Mbit/s)", "New-source selections / second", "App GPU pass (ms)"]):
    values = [row[group][key]["mean"] for row in rows]
    bars = ax.bar(["Off F", "On G", "Off H"], values, color=["#6585ad", "#8b4ad8", "#6585ad"], width=.65)
    ax.bar_label(bars, labels=[f"{v:.2f}" for v in values], padding=5)
    ax.set_title(label, fontsize=11, pad=14)
    ax.set_ylim(0, max(values) * 1.23)
    ax.grid(axis="y", alpha=.17)
    ax.set_axisbelow(True)
fig.suptitle("Checkerboard: fewer transmitted bytes, more headset work", fontsize=16, y=.99)
fig.text(.5, .91, "Pico · 90 Hz · 500 Mbit/s requested budget · 35-second changing-picture runs", ha="center")
fig.text(.5, .025, "Device repositioned; clocks not fixed. Short observations, not isolated GPU cost or optical latency proof.", ha="center", fontsize=9)
fig.tight_layout(rect=(0, .075, 1, .87))
fig.savefig(root / "pico-comparison.png", dpi=160)
