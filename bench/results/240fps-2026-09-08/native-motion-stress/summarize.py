#!/usr/bin/env python3
"""Summarize the native motion stress logs (stdlib only)."""

import json
import math
import re
from statistics import median
from pathlib import Path


ROOT = Path(__file__).resolve().parent
FRAME_RE = re.compile(
    r"^frame\s+(\d+):.*?\bgpu\s+([0-9.]+)\s+total\s+([0-9.]+)\s+ms"
)
INFO_RE = re.compile(r"^frame\s+(\d+)\s+@\S+.*?\bflags\s+0x([0-9a-fA-F]+)")
PHASES = {
    "static": range(2, 24),
    "motion": range(25, 96),
    "recovery": range(96, 120),
}


def percentile(values, fraction):
    if not values:
        return None
    return sorted(values)[max(0, math.ceil(fraction * len(values)) - 1)]


def stats(rows):
    gpu = [r["gpu_ms"] for r in rows]
    total = [r["total_ms"] for r in rows]
    return {
        "count": len(rows),
        "gpu_ms": {"median": median(gpu) if gpu else None, "p95": percentile(gpu, 0.95)},
        "total_ms": {"median": median(total) if total else None, "p95": percentile(total, 0.95)},
    }


def read_flags(path):
    flags = {}
    for line in path.read_text().splitlines():
        match = INFO_RE.search(line)
        if match:
            flags[int(match.group(1))] = int(match.group(2), 16)
    return flags


def main():
    runs = {}
    all_rows = []
    for path in sorted(ROOT.glob("pico-*.log")):
        fixture = re.match(r"pico-(d[0-9]+(?:-q[0-9]+)?)", path.stem).group(1)
        flags = read_flags(ROOT / f"dense-{fixture}.info")
        rows = []
        for line in path.read_text(errors="replace").splitlines():
            match = FRAME_RE.match(line)
            if not match:
                continue
            frame = int(match.group(1))
            if frame not in flags:
                raise ValueError(f"Missing frame flags: {path}:{frame}")
            row = {
                "frame": frame,
                "gpu_ms": float(match.group(2)),
                "total_ms": float(match.group(3)),
                "flags": flags.get(frame),
                "mode": "PICTURE" if flags.get(frame, 0) & 0x20 else "ATLAS",
            }
            rows.append(row)
            all_rows.append(row)
        runs[path.stem] = {
            "source": path.name,
            "frames": len(rows),
            "phases": {
                name: stats([r for r in rows if r["frame"] in frame_set])
                for name, frame_set in PHASES.items()
            },
            "modes": {
                mode: stats([r for r in rows if r["mode"] == mode])
                for mode in ("ATLAS", "PICTURE")
            },
        }
    summary = {
        "source": "pico-*.log plus dense-d*.info",
        "p95": "nearest-rank",
        "phase_frames": {name: [frame_set.start, frame_set.stop - 1] for name, frame_set in PHASES.items()},
        "runs": runs,
        "phases": {
            name: stats([r for r in all_rows if r["frame"] in frame_set])
            for name, frame_set in PHASES.items()
        },
        "modes": {
            mode: stats([r for r in all_rows if r["mode"] == mode])
            for mode in ("ATLAS", "PICTURE")
        },
    }
    (ROOT / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")


if __name__ == "__main__":
    main()
