#!/usr/bin/env python3
"""Summarize same-binary PLANAR pacing captures and make a latency plot."""
import csv
import json
import io
import tarfile
from pathlib import Path
from statistics import mean, median

ROOT = Path(__file__).parent
RAW = ROOT / "raw-captures.tar.gz"
WARMUP = 24
DEADLINE_MS = 1000.0 / 240.0
RUNS = {
    "spin-80-first": "pace-spin-7200.csv",
    "spin-f0": "pace-spin-f0-7200.csv",
    "spin-repeat": "pace-spin-repeat-7200.csv",
    "sleep-control": "pace-sleep-control-7200.csv",
}


def percentiles(values):
    values = sorted(values)
    def nearest(p):
        return values[max(0, min(len(values) - 1, int((p * len(values) + .999999) - 1)))]
    return {"mean_ms": mean(values), "median_ms": median(values),
            "p95_ms": nearest(.95), "p99_ms": nearest(.99),
            "max_ms": max(values)}


def main():
    result = {"fixture": "4352x2176 synthetic all-PLANAR changing-pixel pan (~157 Mb/s)",
              "deadline_ms": DEADLINE_MS, "warmup_excluded": WARMUP, "runs": {}}
    for label, filename in RUNS.items():
        with tarfile.open(RAW) as archive:
            member = next(m for m in archive.getmembers() if Path(m.name).name == filename)
            rows = list(csv.DictReader(io.StringIO(archive.extractfile(member).read().decode())))
        measured = rows[WARMUP:]
        total = [float(r["total_ms"]) for r in measured]
        interval = [float(r["interval_ms"]) for r in measured]
        misses = sum(v > DEADLINE_MS for v in total)
        result["runs"][label] = {
            "file": filename, "frames": len(rows), "measured_frames": len(measured),
            "deadline_misses": misses, "deadline_miss_pct": 100 * misses / len(measured),
            "total_ms": percentiles(total), "interval_ms": percentiles(interval),
        }
    (ROOT / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
    try:
        import matplotlib.pyplot as plt
        labels = list(RUNS)
        rates = [result["runs"][x]["deadline_miss_pct"] for x in labels]
        plt.style.use("seaborn-v0_8-whitegrid")
        fig, ax = plt.subplots(figsize=(7.2, 4.0), dpi=160)
        bars = ax.bar(labels, rates, color=["#5b21b6", "#7c3aed", "#8b5cf6", "#94a3b8"])
        ax.set_ylabel("Deadline misses (%)")
        ax.set_xlabel("Pacing configuration")
        ax.set_title("PLANAR pacing, 7200 frames; 24-frame warmup excluded")
        ax.axhline(1, color="#334155", linestyle="--", linewidth=0.8, label="1%")
        ax.legend(frameon=True)
        ax.bar_label(bars, fmt="%.2f%%", padding=3, fontsize=8)
        fig.tight_layout()
        fig.savefig(ROOT / "latency-summary.png")
    except ImportError:
        pass


if __name__ == "__main__":
    main()
