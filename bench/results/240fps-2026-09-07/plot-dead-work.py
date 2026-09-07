#!/usr/bin/env python3
"""Plot the recorded clear-elision and pipeline-demand paired results."""
from pathlib import Path
import json
import re

import matplotlib.pyplot as plt


ROOT = Path(__file__).resolve().parents[3]
RESULTS = ROOT / "bench/results/240fps-2026-09-07"
OUT = ROOT / "docs/figures/240fps/dead-work-results.png"


def wall_p50(kind, pair):
    data = json.loads((RESULTS / "skip-clears" / f"242-{kind}{pair}.json").read_text())
    return data["metrics"]["total_ms"]["p50"]


def frame1_submit(kind, pair):
    text = (RESULTS / "pipeline-demand" / f"243-{kind}{pair}.log").read_text()
    match = re.search(r"^frame 1:.*?submit ([0-9.]+) ", text, re.MULTILINE)
    if not match:
        raise ValueError(f"frame 1 submit missing in {kind}{pair}")
    return float(match.group(1))


def main():
    pairs = [1, 2, 3]
    clear = [wall_p50("clear", p) for p in pairs]
    skip = [wall_p50("skip", p) for p in pairs]
    force = [frame1_submit("force", p) for p in pairs]
    demand = [frame1_submit("demand", p) for p in pairs]

    fig, (ax0, ax1) = plt.subplots(1, 2, figsize=(9.2, 3.8), constrained_layout=True)
    x = pairs
    ax0.plot(x, clear, "o-", label="Forced clear")
    ax0.plot(x, skip, "o-", label="Clear omitted")
    ax0.set_title("Skipped-tile clear\n(decoder-only short run)")
    ax0.set_ylabel("Decoder total p50 (ms)")
    ax0.set_xlabel("Paired run")
    ax0.set_xticks(x)
    ax0.grid(axis="y", alpha=0.25)
    ax0.legend(frameon=False, fontsize=8)

    ax1.plot(x, force, "o-", label="Force pipelines")
    ax1.plot(x, demand, "o-", label="Demand creation")
    ax1.set_title("Empty ATLAS pipeline demand\n(first empty frame startup)")
    ax1.set_ylabel("Frame 1 submit (ms)")
    ax1.set_xlabel("Paired run")
    ax1.set_xticks(x)
    ax1.grid(axis="y", alpha=0.25)
    ax1.legend(frameon=False, fontsize=8)

    fig.suptitle("Recorded dead-work measurements (Pico, short paired runs)", fontsize=11)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(OUT, dpi=180)
    print(OUT)


if __name__ == "__main__":
    main()
