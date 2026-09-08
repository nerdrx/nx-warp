#!/usr/bin/env python3
"""Validate and summarize one or more frame timing CSVs."""

import argparse
import csv
import json
import math
from pathlib import Path


STAGE_COLUMNS = ("gpu_ms", "parse_ms", "upload_ms")
OPTIONAL_COLUMNS = (
    "tiles_rendered",
    "tiles_skipped",
    "tiles_concealed",
    "age",
    "tile_age",
    "peripheral_age_max",
    "rings_rendered",
)


def fail(path, message):
    raise SystemExit(f"{path}: {message}")


def number(path, row_number, row, column):
    value = row.get(column, "")
    if value is None or not value.strip():
        fail(path, f"row {row_number}: missing {column}")
    try:
        result = float(value)
    except ValueError:
        fail(path, f"row {row_number}: invalid {column}={value!r}")
    if not math.isfinite(result) or result < 0:
        fail(path, f"row {row_number}: {column} must be finite and non-negative")
    return result


def percentile(values, fraction):
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    position = fraction * (len(ordered) - 1)
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    weight = position - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def stats(values):
    return {
        "p50_ms": percentile(values, 0.50),
        "p95_ms": percentile(values, 0.95),
        "p99_ms": percentile(values, 0.99),
        "max_ms": max(values),
    }


def summarize(path, fps, warmup):
    try:
        with path.open(newline="", encoding="utf-8") as stream:
            reader = csv.DictReader(stream)
            if not reader.fieldnames:
                fail(path, "CSV has no header")
            if "frame" not in reader.fieldnames:
                fail(path, "missing frame column")
            if "total_ms" not in reader.fieldnames:
                fail(path, "missing total_ms column")
            timing_columns = [
                column for column in reader.fieldnames if column.endswith("_ms")
            ]
            rows = list(reader)
    except OSError as error:
        fail(path, str(error))

    if not rows:
        fail(path, "CSV has no data rows")

    frame_ids = []
    parsed = {column: [] for column in timing_columns}
    optional = {column: [] for column in OPTIONAL_COLUMNS if column in reader.fieldnames}
    for row_number, row in enumerate(rows, 2):
        raw_frame = row.get("frame", "")
        try:
            frame = int(raw_frame)
        except (TypeError, ValueError):
            fail(path, f"row {row_number}: invalid frame={raw_frame!r}")
        frame_ids.append(frame)
        for column in timing_columns:
            parsed[column].append(number(path, row_number, row, column))
        for column in optional:
            optional[column].append(number(path, row_number, row, column))

    if frame_ids[0] != 0 or any(b != a + 1 for a, b in zip(frame_ids, frame_ids[1:])):
        fail(path, "frame IDs must be contiguous in CSV order and start at zero")
    if warmup < 0 or warmup >= len(rows):
        fail(path, f"warmup must be in [0, {len(rows) - 1}]")

    measured = slice(warmup, None)
    total = parsed["total_ms"][measured]
    deadline_ms = 1000.0 / fps
    result = {
        "count": len(total),
        "warmup_excluded": warmup,
        "deadline_ms": deadline_ms,
        "deadline_misses": sum(value > deadline_ms for value in total),
        "total_ms": stats(total),
        "stage_p99_ms": {
            column: percentile(parsed[column][measured], 0.99)
            for column in STAGE_COLUMNS
            if column in parsed
        },
    }
    if optional:
        result["optional"] = {
            column: {
                "min": min(optional[column][measured]),
                "max": max(optional[column][measured]),
                "mean": sum(optional[column][measured]) / len(total),
            }
            for column in optional
        }
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("csv", nargs="+", type=Path)
    parser.add_argument("--fps", type=float, default=90.0)
    parser.add_argument("--warmup", type=int, default=0)
    args = parser.parse_args()
    if not math.isfinite(args.fps) or args.fps <= 0:
        parser.error("--fps must be finite and positive")
    if args.warmup < 0:
        parser.error("--warmup must be non-negative")
    output = {
        "fps": args.fps,
        "runs": {
            str(path): summarize(path, args.fps, args.warmup) for path in args.csv
        },
    }
    print(json.dumps(output, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
