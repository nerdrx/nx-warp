#!/usr/bin/env python3
"""Plot virtual controller-input traces; no real transport timing is measured."""
import csv
from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
root = Path(__file__).resolve().parent
fig, axes = plt.subplots(2, 2, figsize=(11, 6.5), sharex=True)
for col, capacity in enumerate(["500", "2g"]):
    for mode, colour, label in [("baseline", "#9860bf", "Before"), ("patched", "#208c81", "Matched intervals")]:
        with (root / f"{mode}-{capacity}.csv").open() as file:
            rows = list(csv.DictReader(file))
        assert len(rows) == 1200
        x = [int(row["frame"]) for row in rows]
        axes[0,col].plot(x, [int(row["next_target_bps"])/1e6 for row in rows], color=colour, label=label)
        axes[1,col].plot(x, [int(row["serial_frame_ns"])/1e6 for row in rows], color=colour)
    axes[0,col].set_title("Assumed capacity: " + ("500 Mbit/s" if capacity == "500" else "2 Gbit/s"))
    axes[0,col].axhline(1000, color="#606060", linestyle=":", label="1 Gbit/s ceiling")
    if col == 0:
        axes[0,col].axhline(500, color="#606060", linestyle="--", label="Assumed capacity")
    axes[0,col].set_ylim(0, 1100)
    axes[1,col].axhline(11.111111, color="#606060", linestyle="--", label="Desired 90 Hz period")
    axes[1,col].set_xlabel("Input frame ID (virtual model)")
    axes[1,col].set_ylim(0, 24)
    for ax in axes[:,col]:
        ax.spines[["top", "right"]].set_visible(False)
        ax.grid(alpha=0.2)
axes[0,0].set_ylabel("Controller target (Mbit/s)")
axes[1,0].set_ylabel("Modeled serial service interval (ms)")
axes[0,0].legend(fontsize=8)
axes[1,0].legend(fontsize=8)
fig.suptitle("Matching byte and timing scopes improves the model's budget response", fontsize=14)
fig.text(0.5, 0.025, "Production controller + shared pacing-slot helper; ideal serialized payload service\nQueue caps/drops, FEC, feedback delay and actual encoder output are omitted. No measured FPS or latency.", ha="center", fontsize=9)
fig.tight_layout(rect=(0, 0.08, 1, 0.95))
fig.savefig(root / "budget-response.png", dpi=160)
fig.savefig(root / "budget-response.svg")
svg=root/"budget-response.svg"
svg.write_text("\n".join(line.rstrip() for line in svg.read_text().splitlines()) + "\n")
