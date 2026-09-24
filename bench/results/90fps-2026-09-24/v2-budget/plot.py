#!/usr/bin/env python3
"""Reproduce v2 quality-budget result figures from the canonical matrix."""

import csv
import json
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np


ROOT = Path(__file__).resolve().parent


def read_trace(name):
    with (ROOT / name).open(newline="") as f:
        return list(csv.DictReader(f))


def main():
    legacy = read_trace("legacy-v2.csv")
    mapped = read_trace("mapped-v2.csv")
    with (ROOT / "summary.json").open() as f:
        summary = json.load(f)

    fig, (ax0, ax1) = plt.subplots(1, 2, figsize=(12, 4.8), constrained_layout=True)

    for rows, label, color in ((legacy, "legacy v2", "#667085"), (mapped, "mapped v2", "#146c94")):
        t = np.array([float(r["seconds"]) for r in rows])
        budget = np.array([float(r["budget_mbps"]) for r in rows])
        capacity = np.array([float(r["equivalent_capacity_mbps"]) for r in rows])
        ax0.plot(t, budget, color=color, lw=1.7, label=f"{label} quality budget")
        ax0.plot(t, capacity, color=color, lw=1.0, ls="--", alpha=0.75,
                 label=f"{label} equivalent capacity")
    ax0.set_title("Canonical 550 Mbit/s, 40 ms feedback case")
    ax0.set_xlabel("Model time (s)")
    ax0.set_ylabel("Quality budget / equivalent capacity (Mbit/s)")
    ax0.set_xlim(0, 100)
    ax0.set_ylim(bottom=0)
    ax0.grid(alpha=0.25)
    ax0.legend(fontsize=8, ncol=2)

    mapped_rows = [r for r in summary if r["policy"] == "mapped-v2"]
    delays = sorted({r["feedback_delay_ms"] for r in mapped_rows})
    capacities = sorted({r["weak_capacity_mbps"] for r in mapped_rows})
    x = np.arange(len(capacities))
    width = 0.8 / len(delays)
    for i, delay in enumerate(delays):
        recovery = [next(r["recovery_seconds"] for r in mapped_rows
                         if r["feedback_delay_ms"] == delay and
                         r["weak_capacity_mbps"] == cap) for cap in capacities]
        ax1.bar(x + (i - (len(delays) - 1) / 2) * width, recovery, width,
                label=f"feedback +{delay} ms")
    ax1.set_title("Mapped-v2 recovery across nine FIFO cases")
    ax1.set_xlabel("Weak equivalent capacity (Mbit/s)")
    ax1.set_ylabel("Recovery time (s)")
    ax1.set_xticks(x, [str(c) for c in capacities])
    ax1.set_ylim(bottom=0)
    ax1.grid(axis="y", alpha=0.25)
    ax1.legend(fontsize=8)

    fig.suptitle("NX v2 quality-budget mapping: model evidence", fontsize=13)
    fig.savefig(ROOT / "v2-budget.png", dpi=180)
    fig.savefig(ROOT / "v2-budget.svg", metadata={"Date": None})
    svg = ROOT / "v2-budget.svg"
    svg.write_text("\n".join(line.rstrip() for line in svg.read_text().splitlines()) + "\n")


if __name__ == "__main__":
    main()
