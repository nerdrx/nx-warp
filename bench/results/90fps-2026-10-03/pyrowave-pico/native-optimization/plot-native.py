#!/usr/bin/env python3
"""Plot retained feature-enabled native samples, without mixing old runs."""
from pathlib import Path
import re
import csv
import statistics
import matplotlib.pyplot as plt

root = Path(__file__).resolve().parent
rows = [dict((k, float(v)) for k, v in re.findall(r"(\w+)=(\d+(?:\.\d+)?)", line))
        for line in (root / "pyrowave-native-feature-enabled.log").read_text().splitlines()
        if line.startswith("sample=")]
assert len(rows) == 30
fig, ax = plt.subplots(figsize=(9, 4.8), layout="constrained")
for field, label, color in [("cpu_total_ms", "Synchronous decode call", "#148a91"),
                            ("gpu_ms", "GPU timestamp span", "#6245d9")]:
    ax.plot([r["sample"] for r in rows], [r[field] for r in rows], "o-",
            markersize=3, color=color, label=label)
ax.axhline(1000 / 90, linestyle="--", color="#d84e4e", label="90 Hz: 11.11 ms")
ax.set(title="Pico 4 · native PyroWave · 4352 × 2176 stereo, 4:2:0",
       xlabel="Retained sample (12 warmups excluded)", ylabel="Decode time (ms)",
       ylim=(0, 27))
ax.grid(axis="y", alpha=.2)
ax.legend(loc="lower right")
fig.savefig(root / "native-feature-enabled.png", dpi=170)

lines = (root / "nxvc-native-q22-444-timestamps-run.log").read_text().splitlines()
header = lines.index("sample,parse_ms,submit_ms,passA_ms,passW_ms,passB_ms,gpu_ms,total_ms")
data = list(csv.DictReader([lines[header]] + [line for line in lines[header + 1:]
                                            if re.match(r"\d+,", line)]))
assert len(data) == 30
names = ["Entropy decoding", "Reconstruction", "Host parsing", "Host submission"]
fields = ["passA_ms", "passB_ms", "parse_ms", "submit_ms"]
times = [statistics.median(float(row[field]) for row in data) for field in fields]
fig, ax = plt.subplots(figsize=(8, 4.4), layout="constrained")
ax.barh(names, times, color=["#6245d9", "#148a91", "#999999", "#999999"])
for i, value in enumerate(times):
    ax.text(value + 1.2, i, f"{value:.2f} ms", va="center")
ax.invert_yaxis()
ax.set(xlabel="Median stage time (ms)", xlim=(0, 150),
       title="NXVC reference · native 4:4:4 · Pico 4\n30 samples; runtime GPU timestamp support confirmed")
ax.grid(axis="x", alpha=.15)
fig.savefig(root / "nxvc-reference-bottleneck.png", dpi=170)
