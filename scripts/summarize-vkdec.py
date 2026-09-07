#!/usr/bin/env python3
"""Summarize nxvc-vkdec --stats output as JSON.

The decoder's stats lines are the real measurement shape:

    frame 0: 1234 B, 2048 tiles (321 tskip, 3 lane group(s), 6 dispatches)
      parse 0.123  submit 0.456  passA 1.234  passW 0.000  passB 2.345
      gpu 3.579  total 4.002 ms

This tool keeps the same naming, discards a configurable warmup prefix, and
reports p50/p95/p99 for the recorded samples.  It also keeps a small warning in
the output: `pass_b_ms` in nxvc-vkdec includes `pass_w_ms`, so do not sum them
when reading the totals back.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import sys
from pathlib import Path


FPS_DEADLINES = {90: 1000.0 / 90.0, 120: 1000.0 / 120.0, 144: 1000.0 / 144.0,
                 180: 1000.0 / 180.0, 240: 1000.0 / 240.0}

FRAME_RE = re.compile(
    r"^frame (?P<frame>\d+): (?P<frame_bytes>\d+) B, "
    r"(?P<tiles>\d+) tiles "
    r"\((?P<tiles_tskip>\d+) tskip, (?P<lane_groups>\d+) lane group\(s\), "
    r"(?P<dispatches>\d+) dispatches\)\s+"
    r"parse (?P<parse>[^\s]+)\s+submit (?P<submit>[^\s]+)\s+"
    r"passA (?P<pass_a>[^\s]+)\s+passW (?P<pass_w>[^\s]+)\s+"
    r"passB (?P<pass_b>[^\s]+)\s+gpu (?P<gpu>[^\s]+)\s+"
    r"total (?P<total>[^\s]+) ms$"
)

HASH_RE = re.compile(r"^[0-9a-f]{64}  .+$")

SUMMARY_RE = re.compile(
    r"^\d+ frame\(s\), \d+x\d+ \S+(?: \+\S+)? on .+$"
)


def percentile(samples: list[float], p: float) -> float:
    if not samples:
        raise ValueError("no samples")
    ordered = sorted(samples)
    idx = p * (len(ordered) - 1)
    lo = int(math.floor(idx))
    hi = min(lo + 1, len(ordered) - 1)
    frac = idx - lo
    return round(ordered[lo] * (1.0 - frac) + ordered[hi] * frac, 3)


def finite_nonnegative(name: str, value: str) -> float:
    try:
        parsed = float(value)
    except ValueError as exc:
        raise ValueError(f"{name}: not a number: {value!r}") from exc
    if not math.isfinite(parsed):
        raise ValueError(f"{name}: not finite: {value!r}")
    if parsed < 0.0:
        raise ValueError(f"{name}: negative: {value!r}")
    return parsed


def parse_report(
    text: str,
    warmup_frames: int = 0,
    *,
    strict: bool = True,
    raw_bytes: bytes | None = None,
) -> dict:
    if warmup_frames < 0:
        raise ValueError("warmup_frames must be >= 0")

    if raw_bytes is None:
        raw_bytes = text.encode("utf-8", "surrogateescape")
    sha256 = hashlib.sha256(raw_bytes).hexdigest()
    samples = []
    seen_frames = 0
    last_frame = None

    for raw in text.splitlines():
        line = raw.rstrip("\r")
        if not line:
            continue
        match = FRAME_RE.match(line)
        if match:
            frame_id = int(match.group("frame"))
            if last_frame is not None and frame_id <= last_frame:
                raise ValueError(f"frame id reset/duplicate: {frame_id} after {last_frame}")
            last_frame = frame_id

            sample = {
                "frame": frame_id,
                "frame_bytes": int(match.group("frame_bytes")),
                "tiles": int(match.group("tiles")),
                "tiles_tskip": int(match.group("tiles_tskip")),
                "lane_groups": int(match.group("lane_groups")),
                "dispatches": int(match.group("dispatches")),
                "parse_ms": finite_nonnegative("parse", match.group("parse")),
                "submit_ms": finite_nonnegative("submit", match.group("submit")),
                "pass_a_ms": finite_nonnegative("passA", match.group("pass_a")),
                "pass_w_ms": finite_nonnegative("passW", match.group("pass_w")),
                "pass_b_ms": finite_nonnegative("passB", match.group("pass_b")),
                "gpu_ms": finite_nonnegative("gpu", match.group("gpu")),
                "total_ms": finite_nonnegative("total", match.group("total")),
            }
            samples.append(sample)
            seen_frames += 1
            continue

        if SUMMARY_RE.match(line):
            continue

        if line.startswith(("[segms]", "[hashes]", "[temp]", "[timing]")):
            continue

        if line.startswith("benchmark_exit="):
            if line != "benchmark_exit=0":
                raise ValueError(line)
            continue

        if HASH_RE.match(line) or line.isdecimal():
            continue

        if line.startswith(("frame ", "repeat ", "readback:", "no usable Vulkan ICD:")):
            raise ValueError(line)

        if strict:
            raise ValueError(f"unexpected decoder output: {line}")

    kept = samples[warmup_frames:]
    if not kept:
        raise ValueError("no samples after warmup")

    def summary(values: list[float]) -> dict[str, float]:
        if not values:
            raise ValueError("no samples")
        return {
            "p50": percentile(values, 0.50),
            "p95": percentile(values, 0.95),
            "p99": percentile(values, 0.99),
        }

    gpu_rows = [s for s in kept if s["gpu_ms"] > 0.0]

    def timestamp_summary(values: list[float]) -> dict[str, float | None]:
        if not values:
            return {"p50": None, "p95": None, "p99": None,
                    "available_samples": 0, "unavailable_samples": len(kept) - len(gpu_rows)}
        return {
            **summary(values),
            "available_samples": len(values),
            "unavailable_samples": len(kept) - len(values),
        }

    kept_values = {
        "parse_ms": [s["parse_ms"] for s in kept],
        "submit_ms": [s["submit_ms"] for s in kept],
        "pass_a_ms": [s["pass_a_ms"] for s in kept],
        "pass_w_ms": [s["pass_w_ms"] for s in kept],
        "pass_b_ms": [s["pass_b_ms"] for s in kept],
        "gpu_ms": [s["gpu_ms"] for s in kept],
        "total_ms": [s["total_ms"] for s in kept],
        "tiles": [float(s["tiles"]) for s in kept],
        "dispatches": [float(s["dispatches"]) for s in kept],
    }

    metrics = {}
    for key in ("parse_ms", "submit_ms", "total_ms"):
        metrics[key] = {**summary(kept_values[key]), "available_samples": len(kept_values[key]), "unavailable_samples": 0}
    for key in ("pass_a_ms", "pass_w_ms", "pass_b_ms", "gpu_ms"):
        metrics[key] = timestamp_summary([s[key] for s in gpu_rows])

    counts = {
        "tiles": {**summary(kept_values["tiles"]), "available_samples": len(kept_values["tiles"]), "unavailable_samples": 0},
        "dispatches": {**summary(kept_values["dispatches"]), "available_samples": len(kept_values["dispatches"]), "unavailable_samples": 0},
    }

    deadline_miss = {}
    total_values = kept_values["total_ms"]
    for fps, deadline_ms in FPS_DEADLINES.items():
        missed = sum(1 for value in total_values if value > deadline_ms)
        deadline_miss[str(fps)] = missed / float(len(total_values))

    return {
        "input_sha256": sha256,
        "sample_count": len(kept),
        "samples_seen": seen_frames,
        "warmup_frames": warmup_frames,
        "pass_b_includes_pass_w": True,
        "metrics": metrics,
        "counts": counts,
        "deadline_miss_fraction_decoder_only": deadline_miss,
        "timestamps_available": any(
            metrics[name]["available_samples"] > 0
            for name in ("pass_a_ms", "pass_w_ms", "pass_b_ms", "gpu_ms")
        ),
        "warnings": [
            "passA includes upload+atlas compose; timestamp 0 is before upload/compose and timestamp 1 is after entropy.",
            "nxvc-vkdec passB includes passW; do not add them together.",
        ],
        "unmeasured": {
            "occupancy": None,
            "memory_traffic": None,
            "thermal": None,
            "quality": None,
            "end_to_end": None,
        },
    }


def read_input(path: str) -> str:
    if path == "-":
        data = sys.stdin.buffer.read()
    else:
        data = Path(path).read_bytes()
    return data.decode("utf-8", "surrogateescape")


def build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="Summarize nxvc-vkdec --stats output as JSON.",
    )
    p.add_argument("input", nargs="?", default="-",
                   help="stats log file, or - for stdin")
    p.add_argument("--warmup", type=int, default=0,
                   help="discard the first N decoded frames before summarizing")
    p.add_argument("--no-strict", action="store_true",
                   help="accept unexpected extra lines instead of failing")
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_arg_parser().parse_args(argv)
    try:
        raw = Path(args.input).read_bytes() if args.input != "-" else sys.stdin.buffer.read()
        report = parse_report(
            raw.decode("utf-8", "surrogateescape"),
            warmup_frames=args.warmup,
            strict=not args.no_strict,
            raw_bytes=raw,
        )
    except Exception as exc:
        print(f"summarize-vkdec: {exc}", file=sys.stderr)
        return 1
    json.dump(report, sys.stdout, indent=2, sort_keys=True)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
